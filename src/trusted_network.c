#include "server.h"
#include <sys/types.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/socket.h>

int compareIP(const void *arg1, const void *arg2) {
    in_addr_t ip1 = *(in_addr_t *)arg1;
    in_addr_t ip2 = *(in_addr_t *)arg2;

    if (ip1 == ip2)
        return 0;
    else if (ip1 < ip2)
        return -1;
    else
        return 1;
}

void valkeySortIP(in_addr_t *IPlist, unsigned int IPcount) {
    qsort(IPlist, IPcount, sizeof(IPlist[0]), compareIP);
}

int checkTrustedIP(in_addr_t ip) {
    return bsearch(&ip, server.trustedIPList, server.trustedIPCount, sizeof(server.trustedIPList[0]), compareIP) != NULL
               ? 1
               : 0;
}

int isUnixNetwork(client *c) {
    return c->flags & CLIENT_UNIX_SOCKET;
}


in_addr_t getIPv4Netmask(in_addr_t ip) {
    struct ifaddrs *addrs = NULL;
    in_addr_t netmask = 0;

    if (getifaddrs(&addrs) == -1) return 0;

    for (struct ifaddrs *addr = addrs; addr != NULL; addr = addr->ifa_next) {
        if (addr->ifa_addr == NULL || addr->ifa_netmask == NULL) continue;

        if (addr->ifa_addr->sa_family != AF_INET || addr->ifa_netmask->sa_family != AF_INET) continue;

        struct sockaddr_in *in_addr = (struct sockaddr_in *)addr->ifa_addr;
        if (in_addr->sin_addr.s_addr == ip) {
            struct sockaddr_in *mask = (struct sockaddr_in *)addr->ifa_netmask;
            netmask = mask->sin_addr.s_addr;
            break;
        }
    }

    freeifaddrs(addrs);
    return netmask;
}
