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
