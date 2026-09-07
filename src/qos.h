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
 *   - Connection admission control.
 * -----------------------------------------------------------------------------
 */

#ifndef QOS_H
#define QOS_H

#include <stdbool.h>
#include <arpa/inet.h>

#ifndef C_OK
#define C_OK 0
#define C_ERR -1
#endif

/* Represents an IP subnet (IPv4 or IPv6) and its prefix length. */
typedef struct qosSubnet {
    int family; /* AF_INET or AF_INET6 */
    union {
        struct in_addr ipv4;
        struct in6_addr ipv6;
    } addr;
    int prefix_len;
} qosSubnet;

/* Connection Priority Configuration */
typedef struct qosConfig {
    char *priority_subnets;           /* Raw priority-subnets string config */
    unsigned int maxclients_reserved; /* Client connection slots reserved for priority subnets */
} qosConfig;

extern qosConfig qos_config;

/* Connection Priority Telemetry / Metrics */
typedef struct qosMetrics {
    long long stat_rejected_priority_conn;         /* Prioritized clients rejected because of maxclients */
    long long stat_num_active_clients_prioritized; /* Number of active prioritized clients */
} qosMetrics;

extern qosMetrics qos_metrics;

/* QoS Subsystem Lifecycle */
int qosInit(void);
void qosFree(void);
void qosResetStats(void);

/* Low-level Subnet Primitives */
int parseQosSubnetSource(const char *token, qosSubnet *subnet);
int parseQosSubnetSourceList(const char *raw_sources, qosSubnet **subnets, int *count);
/* matchIpAgainstQosSubnetSources checks if the given IP matches any subnet.
 * Note: ip can be NULL for non-IP transports (e.g. UNIX domain sockets), in which
 * case false is returned as non-IP connections cannot match IP subnets. */
bool matchIpAgainstQosSubnetSources(const char *ip, const qosSubnet *subnets, int count);

/* Configuration Validation & Updates */
int validateQosSubnetSources(const char *sources, const char **err);
int updateQosSubnetSources(const char *sources);

/* Connection Priority & Active Subnet Sources */
bool isIpQosPrioritized(const char *ip);
bool hasQosSubnetSources(void);

#endif /* QOS_H */
