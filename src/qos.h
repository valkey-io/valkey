/* qos.h -- Quality of Service utilities
 *
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef QOS_H
#define QOS_H

#include <arpa/inet.h>

/* Represents an IP subnet (IPv4 or IPv6) and its prefix length. */
typedef struct qosSubnet {
    int family; /* AF_INET or AF_INET6 */
    union {
        struct in_addr ipv4;
        struct in6_addr ipv6;
    } addr;
    int prefix_len;
} qosSubnet;

int parseSubnet(const char *token, qosSubnet *subnet);
int parseSubnetList(const char *raw_sources, qosSubnet **subnets, int *count);
int matchIpAgainstSubnets(const char *ip, qosSubnet *subnets, int count);

#endif
