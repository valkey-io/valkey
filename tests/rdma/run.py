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
    if p.wait():
        outs, _ = p.communicate()
        print("---------------\n" + outs.decode() + "---------------\n")
        print("Valkey Over RDMA build test programs [FAILED]")
        return 1

    print("Valkey Over RDMA build test programs [OK]")
    return 0


# iterate /sys/class/infiniband, find any usable RDMA device, and return IPv4/IPV6 address
def find_rdma_dev():
    # Ex, /sys/class/infiniband/mlx5_0
    # Ex, /sys/class/infiniband/rxe_eth0
    # Ex, /sys/class/infiniband/siw_eth0
    ibclass = "/sys/class/infiniband/"
    try:
        for dev in os.listdir(ibclass):
            # Ex, /sys/class/infiniband/rxe_eth0/ports/1/gid_attrs/ndevs/0
            netdev = ibclass + dev + "/ports/1/gid_attrs/ndevs/0"
            with open(netdev) as fp:
                addrs = netifaces.ifaddresses(fp.readline().strip("\n"))
                if netifaces.AF_INET in addrs:
                    ipaddr = addrs[netifaces.AF_INET][0]["addr"]
                elif netifaces.AF_INET6 in addrs:
                    ipaddr = addrs[netifaces.AF_INET6][0]["addr"]
                else:
                    continue
                print("Valkey Over RDMA test prepare " + dev + " <" + ipaddr  + "> [OK]")
                return ipaddr
    except os.error:
        return None

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


def build_server_cmd(svrpath, tmpdir, ipaddr, args, io_threads):
    cmd = [svrpath, "--port", "0", "--loglevel", "verbose", "--protected-mode", "yes",
           "--appendonly", "no", "--daemonize", "no", "--dir", tmpdir,
           "--rdma-port", str(args.rdma_port), "--rdma-bind", ipaddr]
    if io_threads:
        cmd.extend(["--io-threads", str(args.io_threads), "--io-threads-always-active", "yes"])
    return cmd


def start_server(cmd):
    svr = subprocess.Popen(cmd, shell=False, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        if svr.wait(1):
            print("Valkey Over RDMA valkey-server runs less than 1s [FAILED]")
            return None
    except subprocess.TimeoutExpired:
        print("Valkey Over RDMA valkey-server start [OK]")
        return svr

    svr.kill()
    svr.wait()
    return None


def stop_server(svr):
    if svr is not None and svr.poll() is None:
        svr.kill()
    if svr is not None:
        svr.wait()


def test_rdma(ipaddr, args):
    valkeydir = os.path.dirname(os.path.abspath(__file__)) + "/../.."

    # step 1, prepare test directory
    tmpdir = valkeydir + "/tests/rdma/tmp"
    subprocess.Popen("mkdir -p " + tmpdir, shell=True).wait()

    svrpath = valkeydir + "/src/valkey-server"
    svr = None
    try:
        # step 2, rdma-test uses the original server config (RDMA only, no IO threads)
        svr = start_server(build_server_cmd(svrpath, tmpdir, ipaddr, args, io_threads=False))
        if svr is None:
            return 1

        clipath = valkeydir + "/tests/rdma/rdma-test"
        clicmd = [clipath, "--thread", "4", "-h", ipaddr, "-p", str(args.rdma_port)]
        retval = run_test_client("rdma-test", clicmd, 60)
        if retval:
            return retval

        # step 3, libvalkey-test restarts server with IO threads for the regression case
        stop_server(svr)
        svr = start_server(build_server_cmd(svrpath, tmpdir, ipaddr, args, io_threads=True))
        if svr is None:
            return 1

        clipath = valkeydir + "/tests/rdma/libvalkey-test"
        clicmd = [clipath, "--threads", str(args.threads), "--clients", str(args.clients),
                  "--pipeline", str(args.pipeline), "--requests", str(args.requests),
                  "--datasize", str(args.datasize), "-h", ipaddr, "-p", str(args.rdma_port)]
        return run_test_client("libvalkey-test", clicmd, args.timeout)
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
        help="RDMA port to listen on")
    parser.add_argument("--io-threads", type=int, default=4,
        help="server IO threads for libvalkey-test")
    parser.add_argument("-t", "--threads", type=int, default=16,
        help="libvalkey-test worker threads")
    parser.add_argument("-c", "--clients", type=int, default=128,
        help="libvalkey-test client connections")
    parser.add_argument("-P", "--pipeline", type=int, default=384,
        help="libvalkey-test pipeline depth")
    parser.add_argument("-n", "--requests", type=int, default=200000,
        help="libvalkey-test total SET requests and total GET requests")
    parser.add_argument("-d", "--datasize", type=int, default=256,
        help="libvalkey-test value size in bytes")
    parser.add_argument("--timeout", type=int, default=120,
        help="libvalkey-test timeout in seconds")
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
            p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE)
            if p.wait():
                print("Valkey Over RDMA setup RXE [FAILED]")
                test_exit(1, args.install_rxe)

        # build C client into binary
        retval = build_program()
        if retval:
            test_exit(1, args.install_rxe)

        ipaddr = find_rdma_dev()
        if ipaddr is None:
            # not fatal error, continue to create software version: RXE and SIW
            print("Valkey Over RDMA test detect existing RDMA device [FAILED]")
            retval = 1
        else:
            retval = test_rdma(ipaddr, args)
            if not retval:
                print("Valkey Over RDMA test over " + ipaddr + " [OK]")
    except KeyboardInterrupt:
        print("\nValkey Over RDMA test interrupted [FAILED]")
        retval = 1
    finally:
        test_exit(retval, args.install_rxe)
