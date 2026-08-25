/* qos.c -- Quality of Service utilities
 *
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qos.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <arpa/inet.h>
#include "zmalloc.h"

/* Encapsulated compiled QoS subnets */
static qosSubnet *qos_subnets = NULL;
static int qos_subnets_count = 0;

/* parseQosSubnetSource parses a subnet token in CIDR notation (e.g. "192.168.1.0/24")
 * and populates the qosSubnet structure.
 * Returns 0 on success, -1 on parsing/validation error. */
int parseQosSubnetSource(const char *token, qosSubnet *subnet) {
    if (!token || !subnet) return -1;

    const char *slash = strchr(token, '/');
    char ip_part[100];
    long prefix;
    int family;

    if (slash) {
        int ip_len = slash - token;
        if (ip_len <= 0 || ip_len >= 100) return -1;

        memcpy(ip_part, token, ip_len);
        ip_part[ip_len] = '\0';

        char *endptr;
        prefix = strtol(slash + 1, &endptr, 10);
        if (endptr == slash + 1 || *endptr != '\0') return -1;

        family = strchr(ip_part, ':') ? AF_INET6 : AF_INET;
        if (family == AF_INET) {
            if (prefix < 0 || prefix > 32) return -1;
        } else {
            if (prefix < 0 || prefix > 128) return -1;
        }
    } else {
        int ip_len = strlen(token);
        if (ip_len <= 0 || ip_len >= 100) return -1;

        memcpy(ip_part, token, ip_len);
        ip_part[ip_len] = '\0';

        family = strchr(ip_part, ':') ? AF_INET6 : AF_INET;
        prefix = (family == AF_INET) ? 32 : 128;
    }

    if (family == AF_INET) {
        if (inet_pton(AF_INET, ip_part, &subnet->addr.ipv4) != 1) return -1;
    } else {
        if (inet_pton(AF_INET6, ip_part, &subnet->addr.ipv6) != 1) return -1;
        /* Normalize IPv4-mapped IPv6 subnet */
        if (IN6_IS_ADDR_V4MAPPED(&subnet->addr.ipv6)) {
            family = AF_INET;
            subnet->addr.ipv4.s_addr = *(uint32_t *)(&subnet->addr.ipv6.s6_addr[12]);
            prefix = (prefix > 96) ? (prefix - 96) : 0;
        }
    }

    subnet->family = family;
    subnet->prefix_len = prefix;
    return 0;
}

/* parseQosSubnetSourceList parses a string containing a list of subnets separated by spaces, tabs, or commas.
 * On success, it allocates an array of qosSubnet, populates it, and sets *subnets and *count.
 * Returns 0 on success, and -1 on any parsing or allocation error.
 * Caller is responsible for freeing *subnets using zfree() if it is non-NULL. */
int parseQosSubnetSourceList(const char *raw_sources, qosSubnet **subnets, int *count) {
    if (!subnets || !count) return -1;
    *subnets = NULL;
    *count = 0;

    if (!raw_sources || raw_sources[0] == '\0') {
        return 0;
    }

    /* First pass: count non-empty tokens */
    char *sources_to_count = zstrdup(raw_sources);
    if (!sources_to_count) return -1;
    char *token;
    char *saveptr;
    int sources_count = 0;

    token = strtok_r(sources_to_count, " \t,", &saveptr);
    while (token != NULL) {
        if (strlen(token) > 0) {
            sources_count++;
        }
        token = strtok_r(NULL, " \t,", &saveptr);
    }
    zfree(sources_to_count);

    if (sources_count == 0) {
        return 0;
    }

    qosSubnet *new_subnets = zmalloc(sizeof(qosSubnet) * sources_count);
    if (!new_subnets) return -1;

    char *sources_to_parse = zstrdup(raw_sources);
    if (!sources_to_parse) {
        zfree(new_subnets);
        return -1;
    }

    int source_index = 0;
    int success = 1;
    token = strtok_r(sources_to_parse, " \t,", &saveptr);
    while (token != NULL) {
        if (strlen(token) > 0) {
            if (parseQosSubnetSource(token, &new_subnets[source_index++]) < 0) {
                success = 0;
                break;
            }
        }
        token = strtok_r(NULL, " \t,", &saveptr);
    }
    zfree(sources_to_parse);

    if (!success) {
        zfree(new_subnets);
        return -1;
    }

    *subnets = new_subnets;
    *count = sources_count;
    return 0;
}

/* matchIpAgainstQosSubnetSources checks if the given IP address matches any of the
 * subnets in the list.
 * Returns true if matching any subnet, false otherwise. */
bool matchIpAgainstQosSubnetSources(const char *ip, const qosSubnet *subnets, int count) {
    if (!ip || !subnets || count <= 0) return false;

    int family;
    union {
        struct in_addr ipv4;
        struct in6_addr ipv6;
    } ip_addr;

    if (strchr(ip, ':')) {
        family = AF_INET6;
        if (inet_pton(AF_INET6, ip, &ip_addr.ipv6) != 1) return false;
        /* Normalize IPv4-mapped IPv6 address */
        if (IN6_IS_ADDR_V4MAPPED(&ip_addr.ipv6)) {
            family = AF_INET;
            ip_addr.ipv4.s_addr = *(uint32_t *)(&ip_addr.ipv6.s6_addr[12]);
        }
    } else {
        family = AF_INET;
        if (inet_pton(AF_INET, ip, &ip_addr.ipv4) != 1) return false;
    }

    for (int i = 0; i < count; i++) {
        const qosSubnet *subnet = &subnets[i];
        if (subnet->family != family) continue;

        if (family == AF_INET) {
            uint32_t subnet_val = ntohl(subnet->addr.ipv4.s_addr);
            uint32_t ip_val = ntohl(ip_addr.ipv4.s_addr);
            uint32_t mask = (subnet->prefix_len == 0) ? 0 : (0xFFFFFFFFU << (32 - subnet->prefix_len));
            if ((ip_val & mask) == (subnet_val & mask)) {
                return true;
            }
        } else {
            int bytes = subnet->prefix_len / 8;
            int bits = subnet->prefix_len % 8;
            int match = 1;

            for (int j = 0; j < bytes; j++) {
                if (ip_addr.ipv6.s6_addr[j] != subnet->addr.ipv6.s6_addr[j]) {
                    match = 0;
                    break;
                }
            }

            if (match && bits > 0) {
                uint8_t mask = (uint8_t)(0xFF << (8 - bits));
                if ((ip_addr.ipv6.s6_addr[bytes] & mask) != (subnet->addr.ipv6.s6_addr[bytes] & mask)) {
                    match = 0;
                }
            }

            if (match) return true;
        }
    }

    return false;
}

void qosInit(void) {
    qos_subnets = NULL;
    qos_subnets_count = 0;
}

void qosFree(void) {
    if (qos_subnets) {
        zfree(qos_subnets);
        qos_subnets = NULL;
    }
    qos_subnets_count = 0;
}

int validateQosSubnetSources(const char *sources, const char **err) {
    qosSubnet *subnets = NULL;
    int count = 0;
    if (parseQosSubnetSourceList(sources, &subnets, &count) < 0) {
        if (err) *err = "Invalid IP address or CIDR subnet in qos-subnet-sources";
        return C_ERR;
    }
    if (subnets) zfree(subnets);
    return C_OK;
}

int updateQosSubnetSources(const char *sources) {
    qosSubnet *new_subnets = NULL;
    int new_count = 0;
    if (parseQosSubnetSourceList(sources, &new_subnets, &new_count) < 0) {
        return C_ERR;
    }
    if (qos_subnets) zfree(qos_subnets);
    qos_subnets = new_subnets;
    qos_subnets_count = new_count;
    return C_OK;
}

bool isIpQosPrioritized(const char *ip) {
    if (qos_subnets_count > 0 && ip != NULL && matchIpAgainstQosSubnetSources(ip, qos_subnets, qos_subnets_count)) {
        return true;
    }
    return false;
}

bool hasQosSubnetSources(void) {
    return qos_subnets_count > 0;
}
