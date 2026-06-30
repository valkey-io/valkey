#!/usr/bin/python3
"""
==========================================================================
run.py - script for test client for Valkey Over RDMA (Linux only)
--------------------------------------------------------------------------
Copyright (C) 2024  zhenwei pi <pizhenwei@bytedance.com>

This work is licensed under BSD 3-Clause, License 1 of the COPYING file in
the top-level directory.
==========================================================================
"""
import os
import subprocess
import netifaces
import time
import argparse
import sys
import signal

def build_program():
    valkeydir = os.path.dirname(os.path.abspath(__file__)) + "/../.."
    cmd = "make -C " + valkeydir + "/tests/rdma"
    p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    outs, _ = p.communicate()
    if p.returncode:
        print("---------------\n" + outs.decode() + "---------------\n")
        print("Valkey Over RDMA build test programs [FAILED]")
        return 1

    print("Valkey Over RDMA build test programs [OK]")
    return 0


def ipaddr_from_iface(iface):
    addrs = netifaces.ifaddresses(iface)
    if netifaces.AF_INET in addrs:
        return addrs[netifaces.AF_INET][0]["addr"]
    if netifaces.AF_INET6 in addrs:
        return addrs[netifaces.AF_INET6][0]["addr"]
    return None


def find_default_iface():
    for interface in netifaces.interfaces():
        if interface == "lo":
            continue
        addrs = netifaces.ifaddresses(interface)
        if netifaces.AF_INET in addrs:
            return interface
    return None


def is_rxe_device(ibclass, dev):
    # RXE driver sets node_desc to "rxe" (see kernel drivers/infiniband/sw/rxe/rxe_verbs.c).
    try:
        with open(os.path.join(ibclass, dev, "node_desc")) as fp:
            return fp.read().strip() == "rxe"
    except OSError:
        return False


def find_rdma_ip_from_sysfs(rxe_only=False):
    # Ex, /sys/class/infiniband/mlx5_0
    # Ex, /sys/class/infiniband/rxe_eth0
    # Ex, /sys/class/infiniband/siw_eth0
    ibclass = "/sys/class/infiniband/"
    try:
        devices = os.listdir(ibclass)
    except OSError:
        return None

    candidates = sorted(devices)
    if rxe_only:
        candidates = [dev for dev in candidates if is_rxe_device(ibclass, dev)]

    for dev in candidates:
        # Ex, /sys/class/infiniband/rxe_eth0/ports/1/gid_attrs/ndevs/0
        netdev = ibclass + dev + "/ports/1/gid_attrs/ndevs/0"
        try:
            with open(netdev) as fp:
                iface = fp.readline().strip()
            if not iface:
                continue
            ipaddr = ipaddr_from_iface(iface)
            if ipaddr is None:
                continue
            print("Valkey Over RDMA test prepare " + dev + " <" + ipaddr + "> [OK]")
            return ipaddr
        except (OSError, ValueError):
            continue

    return None


# iterate /sys/class/infiniband, find any usable RDMA device, and return IPv4/IPV6 address
def find_rdma_dev(install_rxe=False):
    # After rdma link add, gid_attrs/ndevs can lag behind the device node. Unstable
    # usually hid this because rdma-test.c compilation took long enough; on CI the
    # main BUILD_RDMA=yes step pre-builds libvalkey, so this script's make is often
    # instant and find runs too early unless we retry.
    retries = 10 if install_rxe else 1
    for attempt in range(retries):
        # With --install-rxe, ignore host RDMA NICs (e.g. GitHub Actions mana_0).
        # They share the same IP but rdma_resolve_addr() fails for local tests.
        ipaddr = find_rdma_ip_from_sysfs(rxe_only=install_rxe)
        if ipaddr is not None:
            return ipaddr
        if attempt + 1 < retries:
            time.sleep(0.2)

    if install_rxe:
        iface = find_default_iface()
        if iface is not None:
            ipaddr = ipaddr_from_iface(iface)
            if ipaddr is not None:
                print("Valkey Over RDMA test prepare rxe_" + iface + " <" + ipaddr + "> [OK]")
                return ipaddr

    return None


def run_test_client(name, cmd, timeout):
    start = time.time()
    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, text=True)
    except subprocess.TimeoutExpired as e:
        print("Valkey Over RDMA " + name + " timed out after " + str(timeout) + "s [FAILED]")
        if e.stdout:
            outs = e.stdout.decode() if isinstance(e.stdout, bytes) else e.stdout
            print("---------------\n" + outs + "---------------\n")
        return 1

    if result.returncode:
        print("Valkey Over RDMA " + name + " [FAILED]")
        print("---------------\n" + result.stdout + "---------------\n")
        return 1

    elapsed = time.time() - start
    print("Valkey Over RDMA " + name + " in " + str(round(elapsed, 2)) + "s [OK]")
    print(result.stdout)
    return 0


def build_server_cmd(svrpath, tmpdir, ipaddr, rdma_port, io_threads):
    cmd = [svrpath, "--port", "0", "--loglevel", "verbose", "--protected-mode", "yes",
           "--appendonly", "no", "--daemonize", "no", "--dir", tmpdir,
           "--rdma-port", str(rdma_port), "--rdma-bind", ipaddr]
    if io_threads:
        cmd.extend(["--io-threads", str(io_threads), "--io-threads-always-active", "yes"])
    return cmd


def build_test_client_cmd(clipath, ipaddr, rdma_port):
    """Only pass connection info; workload defaults live in each C test binary."""
    return [clipath, "-h", ipaddr, "-p", str(rdma_port)]


def print_server_log(logpath, label):
    try:
        with open(logpath, "r") as fp:
            content = fp.read()
    except OSError:
        return

    if not content:
        return

    print("Valkey Over RDMA valkey-server " + label + " log:")
    print("---------------\n" + content + "---------------\n")


def start_server(cmd, logpath):
    logfile = open(logpath, "w", buffering=1)
    svr = subprocess.Popen(cmd, shell=False, stdout=logfile, stderr=subprocess.STDOUT)
    try:
        svr.wait(1)
    except subprocess.TimeoutExpired:
        print("Valkey Over RDMA valkey-server start [OK]")
        return (svr, logfile, logpath)

    logfile.flush()
    logfile.close()
    print("Valkey Over RDMA valkey-server exited within 1s [FAILED]")
    print_server_log(logpath, "startup")
    svr.wait()
    return None


def stop_server(svr_state):
    if svr_state is None:
        return

    svr, logfile, _logpath = svr_state
    if svr.poll() is None:
        svr.kill()
    svr.wait()
    logfile.flush()
    logfile.close()


def test_rdma(ipaddr, rdma_port, io_threads, libvalkey_timeout):
    valkeydir = os.path.dirname(os.path.abspath(__file__)) + "/../.."

    # step 1, prepare test directory
    tmpdir = valkeydir + "/tests/rdma/tmp"
    subprocess.Popen("mkdir -p " + tmpdir, shell=True).wait()

    svrpath = valkeydir + "/src/valkey-server"
    svr = None
    try:
        # step 2, rdma-test uses the original server config (RDMA only, no IO threads)
        svr_log = tmpdir + "/server-rdma-test.log"
        svr = start_server(build_server_cmd(svrpath, tmpdir, ipaddr, rdma_port, io_threads=0),
                           svr_log)
        if svr is None:
            return 1

        clipath = valkeydir + "/tests/rdma/rdma-test"
        clicmd = build_test_client_cmd(clipath, ipaddr, rdma_port)
        retval = run_test_client("rdma-test", clicmd, 60)
        if retval:
            stop_server(svr)
            svr = None
            print_server_log(svr_log, "rdma-test")
            return retval
        stop_server(svr)
        svr = None

        # step 3, libvalkey-test restarts server with IO threads for the regression case
        svr_log = tmpdir + "/server-libvalkey-test.log"
        svr = start_server(build_server_cmd(svrpath, tmpdir, ipaddr, rdma_port, io_threads),
                           svr_log)
        if svr is None:
            return 1

        clipath = valkeydir + "/tests/rdma/libvalkey-test"
        clicmd = build_test_client_cmd(clipath, ipaddr, rdma_port)
        retval = run_test_client("libvalkey-test", clicmd, libvalkey_timeout)
        if retval:
            stop_server(svr)
            svr = None
            print_server_log(svr_log, "libvalkey-test")
            return retval
        stop_server(svr)
        svr = None
        return 0
    finally:
        stop_server(svr)
        subprocess.Popen("rm -rf " + tmpdir, shell=True).wait()


def test_exit(retval, install_rxe):
    # Once we're tearing down, ignore further interrupts so cleanup runs to
    # completion. Otherwise a Ctrl+C during rmmod leaves the RXE device behind.
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)

    if install_rxe and not os.geteuid():
        rdma_env_py = os.path.dirname(os.path.abspath(__file__)) + "/rdma_env.py"
        cmd = rdma_env_py + " -o cleanup"
        subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE).wait()

    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(retval)


def install_signal_handlers():
    """Raise KeyboardInterrupt on SIGINT/SIGTERM so that interrupting the test
    (e.g. Ctrl+C) still runs the normal cleanup path - killing valkey-server
    and removing the RXE device. Without this, an interrupted run leaks the
    RXE device and the server, and the leftover RXE state can make the next
    rdma-test fail with a spurious assertion."""
    def handler(signum, frame):
        # Re-arm as ignored so a second Ctrl+C can't interrupt the teardown
        # (kill server / remove RXE device) already in progress.
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        raise KeyboardInterrupt

    signal.signal(signal.SIGINT, handler)
    signal.signal(signal.SIGTERM, handler)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description = "Script to test Valkey Over RDMA",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-r", "--install-rxe", action='store_true',
        help="install RXE driver and setup RXE device")
    parser.add_argument("--rdma-port", type=int, default=6379,
        help="RDMA port for valkey-server and test clients")
    parser.add_argument("--io-threads", type=int, default=4,
        help="valkey-server IO threads during libvalkey-test")
    parser.add_argument("--timeout", type=int, default=120,
        help="libvalkey-test client timeout in seconds")
    args = parser.parse_args()
    install_signal_handlers()

    retval = 1
    try:
        if args.install_rxe:
            if os.geteuid():
                print("--install-rxe/-r must be root privileged")
                test_exit(1, False)

            rdma_env_py = os.path.dirname(os.path.abspath(__file__)) + "/rdma_env.py"
            cmd = rdma_env_py + " -o setup -d rxe"
            if subprocess.call(cmd, shell=True):
                print("Valkey Over RDMA setup RXE [FAILED]")
                test_exit(1, args.install_rxe)

        # build C client into binary
        retval = build_program()
        if retval:
            test_exit(retval, args.install_rxe)

        ipaddr = find_rdma_dev(args.install_rxe)
        if ipaddr is None:
            print("Valkey Over RDMA test detect existing RDMA device [FAILED]")
            retval = 1
        else:
            retval = test_rdma(ipaddr, args.rdma_port, args.io_threads, args.timeout)
            if not retval:
                print("Valkey Over RDMA test over " + ipaddr + " [OK]")
    except KeyboardInterrupt:
        print("\nValkey Over RDMA test interrupted [FAILED]")
        retval = 1
    finally:
        test_exit(retval, args.install_rxe)
