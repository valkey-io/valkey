#!/usr/bin/python3
"""
==========================================================================
run.py - script to setup/cleanup soft RDMA devices.
         note that is script need root privilege.
--------------------------------------------------------------------------
Copyright (C) 2024  zhenwei pi <pizhenwei@bytedance.com>

This work is licensed under BSD 3-Clause, License 1 of the COPYING file in
the top-level directory.
==========================================================================
"""
import os
import subprocess
import netifaces
import argparse
import json


TEST_NETDEV = "valkeyrdma0"
TEST_IP = "192.0.2.1"
TEST_IP_CIDR = TEST_IP + "/24"


def prepare_ib():
    cmd = "modprobe rdma_cm && modprobe udp_tunnel && modprobe ip6_udp_tunnel && modprobe ib_uverbs"
    p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE)
    if p.wait():
        outs, _ = p.communicate()
        print("Valkey Over RDMA probe modules of IB [FAILED]")
        print("---------------\n" + outs.decode() + "---------------\n")
        os._exit(1);

    print("Valkey Over RDMA probe modules of IB [OK]")


def is_dummy_netdev(interface):
    p = subprocess.run(["ip", "-details", "-json", "link", "show", "dev", interface],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        link_info = json.loads(p.stdout)
    except json.JSONDecodeError:
        return False
    return (not p.returncode and link_info
            and link_info[0].get("linkinfo", {}).get("info_kind") == "dummy")


def prepare_test_netdev():
    p = subprocess.run(["modprobe", "dummy"], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    if p.returncode:
        print("Valkey Over RDMA load dummy netdev driver [FAILED]")
        print("---------------\n" + p.stdout + "---------------\n")
        os._exit(1)

    if os.path.exists("/sys/class/net/" + TEST_NETDEV):
        if not is_dummy_netdev(TEST_NETDEV):
            print("Valkey Over RDMA existing interface <%s> is not a dummy netdev [FAILED]" % TEST_NETDEV)
            os._exit(1)
    else:
        p = subprocess.run(["ip", "link", "add", TEST_NETDEV, "type", "dummy"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if p.returncode:
            print("Valkey Over RDMA create dummy netdev <%s> [FAILED]" % TEST_NETDEV)
            print("---------------\n" + p.stdout + "---------------\n")
            os._exit(1)

    for interface in netifaces.interfaces():
        if interface == TEST_NETDEV:
            continue
        addresses = netifaces.ifaddresses(interface).get(netifaces.AF_INET, [])
        if any(address.get("addr") == TEST_IP for address in addresses):
            print("Valkey Over RDMA test IP <%s> is already in use by <%s> [FAILED]"
                  % (TEST_IP, interface))
            os._exit(1)

    addresses = netifaces.ifaddresses(TEST_NETDEV).get(netifaces.AF_INET, [])
    if not any(address.get("addr") == TEST_IP for address in addresses):
        p = subprocess.run(["ip", "address", "add", TEST_IP_CIDR, "dev", TEST_NETDEV],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if p.returncode:
            print("Valkey Over RDMA configure dummy netdev <%s> [FAILED]" % TEST_NETDEV)
            print("---------------\n" + p.stdout + "---------------\n")
            os._exit(1)

    p = subprocess.run(["ip", "link", "set", TEST_NETDEV, "up"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode:
        print("Valkey Over RDMA enable dummy netdev <%s> [FAILED]" % TEST_NETDEV)
        print("---------------\n" + p.stdout + "---------------\n")
        os._exit(1)

    print("Valkey Over RDMA prepare dummy netdev <%s %s> [OK]" % (TEST_NETDEV, TEST_IP))
    return TEST_NETDEV


def prepare_rxe(interface):
    # is there any builtin rdma_rxe.ko?
    p = subprocess.Popen("modprobe rdma_rxe 2> /dev/null", shell=True, stdout=subprocess.PIPE)
    if p.wait():
        valkeydir = os.path.dirname(os.path.abspath(__file__)) + "/../.."
        rxedir = valkeydir + "/tests/rdma/rxe"
        rxekmod = rxedir + "/rdma_rxe.ko"
        print(rxedir)
        print(rxekmod)
        if not os.path.exists(rxekmod):
            print("Neither kernel builtin nor out-of-tree rdma_rxe.ko found. Abort")
            print("Please run the following commands to build out-of-tree RXE on Linux-6.5, then retry:")
            print("\t~# mkdir -p " + rxedir)
            print("\t~# git clone https://github.com/pizhenwei/rxe.git " + rxedir)
            print("\t~# cd " + rxedir)
            print("\t~# make")
            os._exit(1);

        cmd = "insmod " + rxekmod
        p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE)
        if p.wait():
            os._exit(1);

    print("Valkey Over RDMA install RXE [OK]")

    softrdma = "rxe_" + interface
    if os.path.exists("/sys/class/infiniband/" + softrdma):
        print("Valkey Over RDMA add RXE device <%s> [OK]" % softrdma)
        return

    cmd = "rdma link add " + softrdma + " type rxe netdev " + interface
    p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE)
    if p.wait():
        outs, _ = p.communicate()
        print("Valkey Over RDMA install RXE [FAILED]")
        print("---------------\n" + outs.decode() + "---------------\n")
        os._exit(1);

    print("Valkey Over RDMA add RXE device <%s> [OK]" % softrdma)


def setup_rdma(driver, interface):
    prepare_ib()
    if driver == "rxe":
        if interface is None:
            interface = prepare_test_netdev()
        prepare_rxe(interface)
    else:
        print("rxe is currently supported only")
        os._exit(1);


def cleanup_rdma(interface):
    if interface is None:
        interface = TEST_NETDEV

    softrdma = "rxe_" + interface
    if os.path.exists("/sys/class/infiniband/" + softrdma):
        p = subprocess.run(["rdma", "link", "del", softrdma],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if p.returncode:
            print("Valkey Over RDMA remove RXE device <%s> [FAILED]" % softrdma)
            print("---------------\n" + p.stdout + "---------------\n")
        else:
            print("Valkey Over RDMA remove RXE device <%s> [OK]" % softrdma)

    if (interface == TEST_NETDEV and os.path.exists("/sys/class/net/" + TEST_NETDEV)
            and is_dummy_netdev(TEST_NETDEV)):
        p = subprocess.run(["ip", "link", "del", TEST_NETDEV],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if p.returncode:
            print("Valkey Over RDMA remove dummy netdev <%s> [FAILED]" % TEST_NETDEV)
            print("---------------\n" + p.stdout + "---------------\n")
        else:
            print("Valkey Over RDMA remove dummy netdev <%s> [OK]" % TEST_NETDEV)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description = "Script to setup/cleanup soft RDMA devices, note that root privilege is required",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-o", "--operation", type=str,
        help="[setup|cleanup] setup or cleanup soft RDMA environment")
    parser.add_argument("-d", "--driver", type=str, default="rxe",
        help="[rxe|siw] specify soft RDMA driver, rxe by default")
    parser.add_argument("-i", "--interface", type=str,
        help="[IFACE] network interface, use a dedicated dummy interface by default")
    args = parser.parse_args()

    # test UID. none-root user must stop on none RDMA platform, show some hints and exit.
    if os.geteuid():
        print("You are not root privileged. Abort.")
        print("Or you may setup RXE manually in root privileged by commands:")
        print("\t~# modprobe rdma_rxe")
        print("\t~# rdma link add rxe0 type rxe netdev [IFACE]")
        os._exit(1);

    if args.operation == "cleanup":
        cleanup_rdma(args.interface)
    elif args.operation == "setup":
        setup_rdma(args.driver, args.interface)

    os._exit(0);
