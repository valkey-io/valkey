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
import time
import argparse


def prepare_ib():
    cmd = "modprobe rdma_cm && modprobe udp_tunnel && modprobe ip6_udp_tunnel && modprobe ib_uverbs"
    p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE)
    if p.wait():
        outs, _ = p.communicate()
        print("Valkey Over RDMA probe modules of IB [FAILED]")
        print("---------------\n" + outs.decode() + "---------------\n")
        os._exit(1);

    print("Valkey Over RDMA probe modules of IB [OK]")


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
    cmd = "rdma link add " + softrdma + " type rxe netdev " + interface
    p = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE)
    if p.wait():
        outs, _ = p.communicate()
        print("Valkey Over RDMA install RXE [FAILED]")
        print("---------------\n" + outs.decode() + "---------------\n")
        os._exit(1);

    print("Valkey Over RDMA add RXE device <%s> [OK]" % softrdma)


# find any IPv4 available networking interface
def find_iface():
    interfaces = netifaces.interfaces()
    for interface in interfaces:
        if interface == "lo":
            continue

        addrs = netifaces.ifaddresses(interface)
        if netifaces.AF_INET not in addrs:
            continue

        return interface


def setup_rdma(driver, interface):
    if interface == None:
        interface = find_iface()

    prepare_ib()
    if driver == "rxe":
        prepare_rxe(interface)
    else:
        print("rxe is currently supported only")
        os._exit(1);


# iterate /sys/class/infiniband, find any all virtual RDMA device, and remove them
def cleanup_rdma():
    # Ex, /sys/class/infiniband/mlx5_0
    # Ex, /sys/class/infiniband/rxe_eth0
    # Ex, /sys/class/infiniband/siw_eth0
    ibclass = "/sys/class/infiniband/"
    try:
        for dev in os.listdir(ibclass):
            # Ex, /sys/class/infiniband/rxe_eth0/ports/1/gid_attrs/ndevs/0
            origpath = os.readlink(ibclass + dev)
            if "virtual" in origpath:
                subprocess.Popen("rdma link del " + dev, shell=True).wait()
                print("Remove virtual RDMA device : " + dev + " [OK]")
    except os.error:
        return None

    # try to remove RXE driver from kernel, ignore error
    subprocess.Popen("rmmod rdma_rxe 2> /dev/null", shell=True).wait()

    # try to remove SIW driver from kernel, ignore error
    subprocess.Popen("rmmod rdma_siw 2> /dev/null", shell=True).wait()

    return None


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description = "Script to setup/cleanup soft RDMA devices, note that root privilege is required",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-o", "--operation", type=str,
        help="[setup|cleanup] setup or cleanup soft RDMA environment")
    parser.add_argument("-d", "--driver", type=str, default="rxe",
        help="[rxe|siw] specify soft RDMA driver, rxe by default")
    parser.add_argument("-i", "--interface", type=str,
        help="[IFACE] network interface, auto-select any available interface by default")
    args = parser.parse_args()

    # test UID. none-root user must stop on none RDMA platform, show some hints and exit.
    if os.geteuid():
        print("You are not root privileged. Abort.")
        print("Or you may setup RXE manually in root privileged by commands:")
        print("\t~# modprobe rdma_rxe")
        print("\t~# rdma link add rxe0 type rxe netdev [IFACE]")
        os._exit(1);

    if args.operation == "cleanup":
        cleanup_rdma()
    elif args.operation == "setup":
        setup_rdma(args.driver, args.interface)

    os._exit(0);
