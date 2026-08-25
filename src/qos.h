/* qos.h -- Quality of Service and Connection Admission Control
 *
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * -----------------------------------------------------------------------------
 * Module Purpose:
 *   Provides connection admission control and prioritization (QoS).
 *   Allows critical administrative traffic from trusted CIDR subnets to
 *   connect when normal client capacity (maxclients - reserved) is full.
 *
 * Scope:
 *   - Binary IP subnet compilation, CIDR normalization, and fast binary matching.
 *   - QoS configuration lifecycle, validation, and atomicity.
 *   - Connection admission evaluation (canAcceptQosConnection).
 * -----------------------------------------------------------------------------
 */

#ifndef QOS_H
#define QOS_H

#include <stdbool.h>
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

/* Low-level Subnet Primitives */
int parseQosSubnetSource(const char *token, qosSubnet *subnet);
int parseQosSubnetSourceList(const char *raw_sources, qosSubnet **subnets, int *count);
bool matchIpAgainstQosSubnetSources(const char *ip, const qosSubnet *subnets, int count);

/* Subsystem Lifecycle */
void qosInit(void);
void qosFree(void);

/* Configuration Validation & Updates */
int validateQosSubnetSources(const char *sources, const char **err);
int updateQosSubnetSources(const char *sources);

/* Connection Priority & Admission Control */
bool isIpQosPrioritized(const char *ip);
bool canAcceptQosConnection(bool is_prioritized);

#endif /* QOS_H */
