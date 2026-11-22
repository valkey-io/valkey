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
import rdma_env

RDMA_PORT = 6379
IO_THREADS = 4
BENCH_TIMEOUT = 120
RXE_TEST_NETDEV = rdma_env.TEST_NETDEV
RXE_TEST_DEVICE = "rxe_" + RXE_TEST_NETDEV
RXE_TEST_IP = rdma_env.TEST_IP


def build_program():
    valkeydir = os.path.dirname(os.path.abspath(__file__)) + "/../.."
    cmd = "make -C " + valkeydir + "/tests/rdma"
    p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    outs, _ = p.communicate()
    if p.returncode:
        print("---------------\n" + outs.decode() + "---------------\n")
        print("Valkey Over RDMA build rdma-test [FAILED]")
        return 1

    print("Valkey Over RDMA build rdma-test program [OK]")
    return 0


def ipaddr_from_iface(iface):
    addrs = netifaces.ifaddresses(iface)
    if netifaces.AF_INET in addrs:
        return addrs[netifaces.AF_INET][0]["addr"]
    if netifaces.AF_INET6 in addrs:
        return addrs[netifaces.AF_INET6][0]["addr"]
    return None


def is_rdma_port_active(ibclass, dev):
    try:
        with open(os.path.join(ibclass, dev, "ports", "1", "state")) as fp:
            return fp.read().strip().startswith("4:")
    except OSError:
        return False


def find_rdma_ip_from_sysfs(expected_dev=None):
    # Ex, /sys/class/infiniband/mlx5_0
    # Ex, /sys/class/infiniband/rxe_eth0
    # Ex, /sys/class/infiniband/siw_eth0
    ibclass = "/sys/class/infiniband/"
    try:
        devices = os.listdir(ibclass)
    except OSError:
        return None

    candidates = [expected_dev] if expected_dev else sorted(devices)

    for dev in candidates:
        if dev not in devices:
            continue
        if not is_rdma_port_active(ibclass, dev):
            continue

        # A RoCE device can expose several GID entries. Use one that has a
        # non-zero GID and maps to a netdev with an IP address.
        ndevs = os.path.join(ibclass, dev, "ports", "1", "gid_attrs", "ndevs")
        try:
            gid_indexes = sorted(os.listdir(ndevs), key=int)
        except (OSError, ValueError):
            continue

        for gid_index in gid_indexes:
            try:
                with open(os.path.join(ndevs, gid_index)) as fp:
                    iface = fp.readline().strip()
                with open(os.path.join(ibclass, dev, "ports", "1", "gids", gid_index)) as fp:
                    gid = fp.readline().strip()
            except OSError:
                continue

            if not iface or not gid.replace(":", "").strip("0"):
                continue
            if expected_dev and iface != RXE_TEST_NETDEV:
                continue
            ipaddr = ipaddr_from_iface(iface)
            if ipaddr is None or (expected_dev and ipaddr != RXE_TEST_IP):
                continue
            print("Valkey Over RDMA test prepare " + dev + " <" + iface + " " + ipaddr + "> [OK]")
            return ipaddr

    return None


def find_rdma_dev(install_rxe=False):
    # After rdma link add, the port and GID table can lag behind the device
    # node. When RXE was installed for this test, require that exact device;
    # an IP address alone cannot distinguish RXE from a hardware RDMA provider.
    expected_dev = RXE_TEST_DEVICE if install_rxe else None
    retries = 20 if install_rxe else 1
    for attempt in range(retries):
        ipaddr = find_rdma_ip_from_sysfs(expected_dev)
        if ipaddr is not None:
            return ipaddr
        if attempt + 1 < retries:
            time.sleep(0.2)

    return None


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


def start_server(svrcmd, logpath):
    logfile = open(logpath, "w", buffering=1)
    svr = subprocess.Popen(svrcmd, shell=False, stdout=logfile, stderr=subprocess.STDOUT)
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


def run_cmd(name, cmd, timeout):
    start = time.time()
    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=timeout, text=True)
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


def server_base_cmd(svrpath, tmpdir, ipaddr):
    return [svrpath, "--port", "0", "--loglevel", "verbose", "--protected-mode", "yes",
            "--appendonly", "no", "--daemonize", "no", "--dir", tmpdir,
            "--rdma-port", str(RDMA_PORT), "--rdma-bind", ipaddr]


def test_rdma(ipaddr):
    valkeydir = os.path.dirname(os.path.abspath(__file__)) + "/../.."
    tmpdir = valkeydir + "/tests/rdma/tmp"
    subprocess.Popen("mkdir -p " + tmpdir, shell=True).wait()

    svrpath = valkeydir + "/src/valkey-server"
    benchpath = valkeydir + "/src/valkey-benchmark"
    clipath = valkeydir + "/tests/rdma/rdma-test"
    svr = None
    try:
        # Phase 1: basic RDMA CM/verbs smoke (no IO threads), same as upstream.
        svr_log = tmpdir + "/server-rdma-test.log"
        svr = start_server(server_base_cmd(svrpath, tmpdir, ipaddr), svr_log)
        if svr is None:
            return 1

        clicmd = [clipath, "--thread", "4", "-h", ipaddr, "-p", str(RDMA_PORT)]
        retval = run_cmd("rdma-test", clicmd, 60)
        if retval:
            print_server_log(svr_log, "rdma-test")
            return retval
        stop_server(svr)
        svr = None

        # Phase 2: RDMA + IO threads stress via valkey-benchmark (repro path for #3611).
        svr_log = tmpdir + "/server-benchmark.log"
        svrcmd = server_base_cmd(svrpath, tmpdir, ipaddr) + [
            "--io-threads", str(IO_THREADS), "--io-threads-always-active", "yes"]
        svr = start_server(svrcmd, svr_log)
        if svr is None:
            return 1

        benchcmd = [benchpath, "-h", ipaddr, "-p", str(RDMA_PORT), "--rdma", "-q",
                    "-d", "256", "--threads", "16", "-c", "128", "-P", "384",
                    "-n", "200000", "-t", "set,get"]
        print("Valkey Over RDMA valkey-benchmark " + " ".join(benchcmd[1:]))
        retval = run_cmd("valkey-benchmark", benchcmd, BENCH_TIMEOUT)
        if retval:
            print_server_log(svr_log, "valkey-benchmark")
            return retval
        return 0
    finally:
        stop_server(svr)
        subprocess.Popen("rm -rf " + tmpdir, shell=True).wait()


def test_exit(retval, install_rxe):
    # Ignore further interrupts so cleanup (kill server / remove RXE) finishes.
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)

    if install_rxe and not os.geteuid():
        rdma_env_py = os.path.dirname(os.path.abspath(__file__)) + "/rdma_env.py"
        cmd = [rdma_env_py, "-o", "cleanup", "-i", RXE_TEST_NETDEV]
        subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(retval)


def install_signal_handlers():
    def handler(signum, frame):
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
    args = parser.parse_args()
    install_signal_handlers()

    retval = 1
    try:
        if args.install_rxe:
            if os.geteuid():
                print("--install-rxe/-r must be root privileged")
                test_exit(1, False)

            rdma_env_py = os.path.dirname(os.path.abspath(__file__)) + "/rdma_env.py"
            cmd = [rdma_env_py, "-o", "setup", "-d", "rxe"]
            if subprocess.call(cmd):
                print("Valkey Over RDMA setup RXE [FAILED]")
                test_exit(1, args.install_rxe)

        retval = build_program()
        if retval:
            test_exit(retval, args.install_rxe)

        ipaddr = find_rdma_dev(args.install_rxe)
        if ipaddr is None:
            print("Valkey Over RDMA test detect existing RDMA device [FAILED]")
            retval = 1
        else:
            retval = test_rdma(ipaddr)
            if not retval:
                print("Valkey Over RDMA test over " + ipaddr + " [OK]")
    except KeyboardInterrupt:
        print("\nValkey Over RDMA test interrupted [FAILED]")
        retval = 1
    finally:
        test_exit(retval, args.install_rxe)
