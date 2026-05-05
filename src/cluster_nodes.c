/*
 * Copyright (c) 2009-2012, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * cluster_nodes.c — Cluster node serialization and persistence.
 *
 * This file handles the nodes.conf file format: loading, saving, and locking.
 * It also contains the CLUSTER NODES text representation (used by both the
 * command and the config file), aux field handlers for node property
 * serialization, and slot info pair generation.
 */

#include "server.h"
#include "cluster.h"
#include "cluster_bus.h"
#include "cluster_state.h"
#include "cluster_slot_stats.h"

#include <arpa/inet.h>
#include <sys/file.h>
#include <sys/stat.h>

/* Forward declarations. */
void clusterGenNodesSlotsInfo(int filter);
void clusterFreeNodesSlotsInfo(clusterNode *n);

/* -----------------------------------------------------------------------------
 * Nodes to string representation functions.
 * -------------------------------------------------------------------------- */

/* Aux fields were introduced in Redis OSS 7.2 to support the persistence
 * of various important node properties, such as shard id, in nodes.conf.
 * Aux fields take an explicit format of name=value pairs and have no
 * intrinsic order among them. Aux fields are always grouped together
 * at the end of the second column of each row after the node's IP
 * address/port/cluster_port and the optional hostname. Aux fields
 * are separated by ','. */

typedef int(aux_value_setter)(clusterNode *n, void *value, size_t length);
typedef sds(aux_value_getter)(clusterNode *n, sds s);
typedef int(aux_value_present)(clusterNode *n);

typedef struct {
    char *field;
    aux_value_setter *setter;
    aux_value_getter *getter;
    aux_value_present *isPresent;
} auxFieldHandler;

typedef enum {
    af_shard_id,
    af_human_nodename,
    af_tcp_port,
    af_tls_port,
    af_announce_client_ipv4,
    af_announce_client_ipv6,
    af_announce_client_tcp_port,
    af_announce_client_tls_port,
    af_availability_zone,
    af_count,
} auxFieldIndex;

static int auxShardIdSetter(clusterNode *n, void *value, size_t length) {
    if (verifyClusterNodeId(value, length) == C_ERR) {
        return C_ERR;
    }
    memcpy(n->shard_id, value, CLUSTER_NAMELEN);
    /* if n already has replicas, make sure they all agree
     * on the shard id. If not, update them. */
    for (int i = 0; i < n->num_replicas; i++) {
        if (memcmp(n->replicas[i]->shard_id, n->shard_id, CLUSTER_NAMELEN) != 0) {
            serverLog(LL_NOTICE,
                      "Node %.40s has a different shard id (%.40s) than its primary's shard id %.40s (%.40s). "
                      "Updating replica's shard id to match primary's shard id.",
                      n->replicas[i]->name, n->replicas[i]->shard_id, n->name, n->shard_id);
            clusterRemoveNodeFromShard(n->replicas[i]);
            memcpy(n->replicas[i]->shard_id, n->shard_id, CLUSTER_NAMELEN);
            clusterAddNodeToShard(n->shard_id, n->replicas[i]);
        }
    }
    clusterAddNodeToShard(value, n);
    return C_OK;
}

static sds auxShardIdGetter(clusterNode *n, sds s) {
    return sdscatlen(s, n->shard_id, CLUSTER_NAMELEN);
}

static int auxShardIdPresent(clusterNode *n) {
    return strlen(n->shard_id);
}

static int auxHumanNodenameSetter(clusterNode *n, void *value, size_t length) {
    if (sdslen(n->human_nodename) == length && !strncmp(value, n->human_nodename, length)) {
        return C_OK;
    }
    n->human_nodename = sdscpylen(n->human_nodename, value, length);
    return C_OK;
}

static sds auxHumanNodenameGetter(clusterNode *n, sds s) {
    return sdscat(s, n->human_nodename);
}

static int auxHumanNodenamePresent(clusterNode *n) {
    return sdslen(n->human_nodename);
}

static int auxAvailabilityZoneSetter(clusterNode *n, void *value, size_t length) {
    if (sdslen(n->availability_zone) == length && !strncmp(value, n->availability_zone, length)) {
        return C_OK;
    }
    n->availability_zone = sdscpylen(n->availability_zone, value, length);
    return C_OK;
}

static sds auxAvailabilityZoneGetter(clusterNode *n, sds s) {
    return sdscat(s, n->availability_zone);
}

static int auxAvailabilityZonePresent(clusterNode *n) {
    return sdslen(n->availability_zone);
}

static int auxAnnounceClientIpV4Setter(clusterNode *n, void *value, size_t length) {
    if (sdslen(n->announce_client_ipv4) == length && !strncmp(value, n->announce_client_ipv4, length)) {
        return C_OK;
    }
    if (length != 0) {
        struct sockaddr_in sa;
        if (inet_pton(AF_INET, (const char *)value, &(sa.sin_addr)) == 0) {
            return C_ERR;
        }
    }
    n->announce_client_ipv4 = sdscpylen(n->announce_client_ipv4, value, length);
    return C_OK;
}

static sds auxAnnounceClientIpV4Getter(clusterNode *n, sds s) {
    return sdscat(s, n->announce_client_ipv4);
}

static int auxAnnounceClientIpV4Present(clusterNode *n) {
    return sdslen(n->announce_client_ipv4) != 0;
}

static int auxAnnounceClientIpV6Setter(clusterNode *n, void *value, size_t length) {
    if (sdslen(n->announce_client_ipv6) == length && !strncmp(value, n->announce_client_ipv6, length)) {
        return C_OK;
    }
    if (length != 0) {
        struct sockaddr_in6 sa;
        if (inet_pton(AF_INET6, (const char *)value, &(sa.sin6_addr)) == 0) {
            return C_ERR;
        }
    }
    n->announce_client_ipv6 = sdscpylen(n->announce_client_ipv6, value, length);
    return C_OK;
}

static sds auxAnnounceClientIpV6Getter(clusterNode *n, sds s) {
    return sdscat(s, n->announce_client_ipv6);
}

static int auxAnnounceClientIpV6Present(clusterNode *n) {
    return sdslen(n->announce_client_ipv6) != 0;
}

static int auxTcpPortSetter(clusterNode *n, void *value, size_t length) {
    if (length > 5 || length < 1) return C_ERR;
    char buf[length + 1];
    memcpy(buf, (char *)value, length);
    buf[length] = '\0';
    n->tcp_port = atoi(buf);
    return (n->tcp_port < 0 || n->tcp_port >= 65536) ? C_ERR : C_OK;
}

static sds auxTcpPortGetter(clusterNode *n, sds s) {
    return sdscatfmt(s, "%i", n->tcp_port);
}

static int auxTcpPortPresent(clusterNode *n) {
    return n->tcp_port >= 0 && n->tcp_port < 65536;
}

static int auxTlsPortSetter(clusterNode *n, void *value, size_t length) {
    if (length > 5 || length < 1) return C_ERR;
    char buf[length + 1];
    memcpy(buf, (char *)value, length);
    buf[length] = '\0';
    n->tls_port = atoi(buf);
    return (n->tls_port < 0 || n->tls_port >= 65536) ? C_ERR : C_OK;
}

static sds auxTlsPortGetter(clusterNode *n, sds s) {
    return sdscatfmt(s, "%i", n->tls_port);
}

static int auxTlsPortPresent(clusterNode *n) {
    return n->tls_port >= 0 && n->tls_port < 65536;
}

static int auxAnnounceClientTcpPortSetter(clusterNode *n, void *value, size_t length) {
    if (length > 5 || length < 1) return C_ERR;
    char buf[length + 1];
    memcpy(buf, (char *)value, length);
    buf[length] = '\0';
    n->announce_client_tcp_port = atoi(buf);
    return (n->announce_client_tcp_port < 0 || n->announce_client_tcp_port >= 65536) ? C_ERR : C_OK;
}

static sds auxAnnounceClientTcpPortGetter(clusterNode *n, sds s) {
    return sdscatfmt(s, "%i", n->announce_client_tcp_port);
}

static int auxAnnounceClientTcpPortPresent(clusterNode *n) {
    return n->announce_client_tcp_port > 0 && n->announce_client_tcp_port < 65536;
}

static int auxAnnounceClientTlsPortSetter(clusterNode *n, void *value, size_t length) {
    if (length > 5 || length < 1) return C_ERR;
    char buf[length + 1];
    memcpy(buf, (char *)value, length);
    buf[length] = '\0';
    n->announce_client_tls_port = atoi(buf);
    return (n->announce_client_tls_port < 0 || n->announce_client_tls_port >= 65536) ? C_ERR : C_OK;
}

static sds auxAnnounceClientTlsPortGetter(clusterNode *n, sds s) {
    return sdscatfmt(s, "%i", n->announce_client_tls_port);
}

static int auxAnnounceClientTlsPortPresent(clusterNode *n) {
    return n->announce_client_tls_port > 0 && n->announce_client_tls_port < 65536;
}

/* Note that
 * 1. the order of the elements below must match that of their
 *    indices as defined in auxFieldIndex
 * 2. aux name can contain characters that pass the isValidAuxChar check only */
auxFieldHandler auxFieldHandlers[] = {
    {"shard-id", auxShardIdSetter, auxShardIdGetter, auxShardIdPresent},
    {"nodename", auxHumanNodenameSetter, auxHumanNodenameGetter, auxHumanNodenamePresent},
    {"tcp-port", auxTcpPortSetter, auxTcpPortGetter, auxTcpPortPresent},
    {"tls-port", auxTlsPortSetter, auxTlsPortGetter, auxTlsPortPresent},
    {"client-ipv4", auxAnnounceClientIpV4Setter, auxAnnounceClientIpV4Getter, auxAnnounceClientIpV4Present},
    {"client-ipv6", auxAnnounceClientIpV6Setter, auxAnnounceClientIpV6Getter, auxAnnounceClientIpV6Present},
    {"client-tcp-port", auxAnnounceClientTcpPortSetter, auxAnnounceClientTcpPortGetter, auxAnnounceClientTcpPortPresent},
    {"client-tls-port", auxAnnounceClientTlsPortSetter, auxAnnounceClientTlsPortGetter, auxAnnounceClientTlsPortPresent},
    {"availability-zone", auxAvailabilityZoneSetter, auxAvailabilityZoneGetter, auxAvailabilityZonePresent},
};

struct clusterNodeFlags {
    uint16_t flag;
    char *name;
};

static struct clusterNodeFlags clusterNodeFlagsTable[] = {
    {CLUSTER_NODE_MYSELF, "myself,"},
    {CLUSTER_NODE_PRIMARY, "master,"},
    {CLUSTER_NODE_REPLICA, "slave,"},
    {CLUSTER_NODE_PFAIL, "fail?,"},
    {CLUSTER_NODE_FAIL, "fail,"},
    {CLUSTER_NODE_HANDSHAKE, "handshake,"},
    {CLUSTER_NODE_NOADDR, "noaddr,"},
    {CLUSTER_NODE_NOFAILOVER, "nofailover,"}};

/* Concatenate the comma separated list of node flags to the given SDS
 * string 'ci'. */
sds representClusterNodeFlags(sds ci, uint16_t flags) {
    size_t orig_len = sdslen(ci);
    int i, size = sizeof(clusterNodeFlagsTable) / sizeof(struct clusterNodeFlags);
    for (i = 0; i < size; i++) {
        struct clusterNodeFlags *nodeflag = clusterNodeFlagsTable + i;
        if (flags & nodeflag->flag) ci = sdscat(ci, nodeflag->name);
    }
    /* If no flag was added, add the "noflags" special flag. */
    if (sdslen(ci) == orig_len) ci = sdscat(ci, "noflags,");
    sdsIncrLen(ci, -1); /* Remove trailing comma. */
    return ci;
}

/* Concatenate the slot ownership information to the given SDS string 'ci'.
 * If the slot ownership is in a contiguous block, it's represented as start-end pair,
 * else each slot is added separately. */
static sds representSlotInfo(sds ci, uint16_t *slot_info_pairs, int slot_info_pairs_count) {
    for (int i = 0; i < slot_info_pairs_count; i += 2) {
        unsigned int start = slot_info_pairs[i];
        unsigned int end = slot_info_pairs[i + 1];
        if (start == end) {
            ci = sdscatfmt(ci, " %u", start);
        } else {
            ci = sdscatfmt(ci, " %u-%u", start, end);
        }
    }
    return ci;
}

/* Append the address+aux string for a node to an sds: ip:port@cport[,hostname][,aux=val]*
 * This is the same format used in the second column of nodes.conf. */
sds clusterNodeAppendAddressString(sds s, clusterNode *node, int tls_primary) {
    int port = tls_primary ? node->tls_port : node->tcp_port;
    s = sdscatfmt(s, "%s:%i@%i", node->ip, port, node->cport);
    if (sdslen(node->hostname) != 0) {
        s = sdscatfmt(s, ",%s", node->hostname);
    } else {
        s = sdscatlen(s, ",", 1);
    }
    for (int i = af_count - 1; i >= 0; i--) {
        if ((tls_primary && i == af_tls_port) || (!tls_primary && i == af_tcp_port)) continue;
        if (auxFieldHandlers[i].isPresent(node)) {
            s = sdscatfmt(s, ",%s=", auxFieldHandlers[i].field);
            s = auxFieldHandlers[i].getter(node, s);
        }
    }
    return s;
}

/* Parse an address+aux string onto a node. The string format is:
 * ip:port@cport[,hostname][,aux=val]*
 * Returns C_OK on success, C_ERR on parse error. The input string is modified. */
int clusterNodeParseAddressString(clusterNode *n, char *str) {
    int aux_argc;
    sds *aux_argv = sdssplitlen(str, strlen(str), ",", 1, &aux_argc);
    if (aux_argv == NULL) return C_ERR;

    /* Hostname */
    if (aux_argc > 1 && sdslen(aux_argv[1]) > 0) {
        n->hostname = sdscpy(n->hostname, aux_argv[1]);
    } else if (sdslen(n->hostname) != 0) {
        sdsclear(n->hostname);
    }

    /* Aux fields */
    int aux_tcp_port = 0, aux_tls_port = 0;
    for (int i = 2; i < aux_argc; i++) {
        int field_argc;
        sds *field_argv = sdssplitlen(aux_argv[i], sdslen(aux_argv[i]), "=", 1, &field_argc);
        if (field_argv == NULL || field_argc != 2) {
            if (field_argv) sdsfreesplitres(field_argv, field_argc);
            goto err;
        }
        if (!isValidAuxString(field_argv[0], sdslen(field_argv[0])) ||
            !isValidAuxString(field_argv[1], sdslen(field_argv[1]))) {
            sdsfreesplitres(field_argv, field_argc);
            goto err;
        }
        int found = 0;
        for (unsigned j = 0; j < af_count; j++) {
            if (sdslen(field_argv[0]) != strlen(auxFieldHandlers[j].field) ||
                memcmp(field_argv[0], auxFieldHandlers[j].field, sdslen(field_argv[0])) != 0)
                continue;
            found = 1;
            aux_tcp_port |= j == af_tcp_port;
            aux_tls_port |= j == af_tls_port;
            if (auxFieldHandlers[j].setter(n, field_argv[1], sdslen(field_argv[1])) != C_OK) {
                sdsfreesplitres(field_argv, field_argc);
                goto err;
            }
        }
        sdsfreesplitres(field_argv, field_argc);
        if (!found) goto err;
    }

    /* ip:port@cport */
    char *p = strrchr(aux_argv[0], ':');
    if (!p) goto err;
    *p = '\0';
    memcpy(n->ip, aux_argv[0], strlen(aux_argv[0]) + 1);
    char *port = p + 1;
    char *busp = strchr(port, '@');
    if (busp) {
        *busp = '\0';
        busp++;
    }
    if (!aux_tcp_port && !aux_tls_port) {
        if (server.tls_cluster)
            n->tls_port = atoi(port);
        else
            n->tcp_port = atoi(port);
    } else if (!aux_tcp_port) {
        n->tcp_port = atoi(port);
    } else if (!aux_tls_port) {
        n->tls_port = atoi(port);
    }
    n->cport = busp ? atoi(busp) : (getNodeDefaultClientPort(n) + CLUSTER_PORT_INCR);

    sdsfreesplitres(aux_argv, aux_argc);
    return C_OK;

err:
    sdsfreesplitres(aux_argv, aux_argc);
    return C_ERR;
}

/* Generate a csv-alike representation of the specified cluster node.
 * See clusterGenNodesDescription() top comment for more information.
 *
 * If a client is provided, we're creating a reply to the CLUSTER NODES command.
 * If client is NULL, we are creating the content of nodes.conf.
 *
 * The function returns the string representation as an SDS string. */
sds clusterGenNodeDescription(client *c, clusterNode *node, int tls_primary) {
    int j, start;
    sds ci;

    /* Node coordinates */
    ci = sdscatlen(sdsempty(), node->name, CLUSTER_NAMELEN);
    ci = sdscatlen(ci, " ", 1);
    if (c == NULL) {
        /* nodes.conf: use the canonical address+aux format */
        ci = clusterNodeAppendAddressString(ci, node, tls_primary);
    } else {
        /* CLUSTER NODES reply: use client-facing IP/port, no aux fields */
        int port = clusterNodeClientPort(node, tls_primary, c);
        char *ip = clusterNodeIp(node, c);
        ci = sdscatfmt(ci, "%s:%i@%i", ip, port, node->cport);
        if (sdslen(node->hostname) != 0) {
            ci = sdscatfmt(ci, ",%s", node->hostname);
        }
    }

    /* Flags */
    ci = sdscatlen(ci, " ", 1);
    ci = representClusterNodeFlags(ci, node->flags);

    /* Replica of... or just "-" */
    ci = sdscatlen(ci, " ", 1);
    if (node->replicaof)
        ci = sdscatlen(ci, node->replicaof->name, CLUSTER_NAMELEN);
    else
        ci = sdscatlen(ci, "-", 1);

    /* Latency from the POV of this node, config epoch, link status.
     * Protocol implementations provide these via getNodePingPongEpoch.
     * If the callback is NULL, all three are reported as 0. */
    long long ping_sent = 0, pong_received = 0;
    uint64_t config_epoch = 0;
    if (clusterCurrentBus->getNodePingPongEpoch)
        clusterCurrentBus->getNodePingPongEpoch(node, &ping_sent, &pong_received, &config_epoch);
    ci = sdscatfmt(ci, " %I %I %U %s", ping_sent, pong_received, config_epoch,
                   (node->link || node->flags & CLUSTER_NODE_MYSELF) ? "connected" : "disconnected");

    /* Slots served by this instance. If we already have slots info,
     * append it directly, otherwise, generate slots only if it has. */
    if (node->slot_info_pairs) {
        ci = representSlotInfo(ci, node->slot_info_pairs, node->slot_info_pairs_count);
    } else if (node->numslots > 0) {
        start = -1;
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            int bit;

            if ((bit = clusterNodeCoversSlot(node, j)) != 0) {
                if (start == -1) start = j;
            }
            if (start != -1 && (!bit || j == CLUSTER_SLOTS - 1)) {
                if (bit && j == CLUSTER_SLOTS - 1) j++;

                if (start == j - 1) {
                    ci = sdscatfmt(ci, " %i", start);
                } else {
                    ci = sdscatfmt(ci, " %i-%i", start, j - 1);
                }
                start = -1;
            }
        }
    }

    /* Just for MYSELF node we also dump info about slots that
     * we are migrating to other instances or importing from other
     * instances. */
    if (node->flags & CLUSTER_NODE_MYSELF) {
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            clusterNode *mn = getMigratingSlotDest(j);
            clusterNode *in = getImportingSlotSource(j);
            if (mn) {
                ci = sdscatfmt(ci, " [%i->-", j);
                ci = sdscatlen(ci, mn->name, CLUSTER_NAMELEN);
                ci = sdscat(ci, "]");
            } else if (in) {
                ci = sdscatfmt(ci, " [%i-<-", j);
                ci = sdscatlen(ci, in->name, CLUSTER_NAMELEN);
                ci = sdscat(ci, "]");
            }
        }
    }
    return ci;
}

/* Generate a csv-alike representation of the nodes we are aware of,
 * including the "myself" node, and return an SDS string containing the
 * representation (it is up to the caller to free it).
 *
 * All the nodes matching at least one of the node flags specified in
 * "filter" are excluded from the output, so using zero as a filter will
 * include all the known nodes in the representation, including nodes in
 * the HANDSHAKE state.
 *
 * Setting tls_primary to 1 to put TLS port in the main <ip>:<port>
 * field and put TCP port in aux field, instead of the opposite way.
 *
 * The representation obtained using this function is used for the output
 * of the CLUSTER NODES function, and as format for the cluster
 * configuration file (nodes.conf) for a given node. */
sds clusterGenNodesDescription(client *c, int filter, int tls_primary) {
    sds ci = sdsempty(), ni;
    dictIterator *di;
    dictEntry *de;

    /* Generate all nodes slots info firstly. */
    clusterGenNodesSlotsInfo(filter);

    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node->flags & filter) continue;
        ni = clusterGenNodeDescription(c, node, tls_primary);
        ci = sdscatsds(ci, ni);
        sdsfree(ni);
        ci = sdscatlen(ci, "\n", 1);

        /* Release slots info. */
        clusterFreeNodesSlotsInfo(node);
    }
    dictReleaseIterator(di);
    return ci;
}

/* Generate the slot topology for all nodes and store the slot range information
 * in the slot_info_pairs array on the node. This is used to improve the efficiency
 * of clusterGenNodesDescription() because it removes looping of the slot space
 * for generating the slot info for each node individually. */
void clusterGenNodesSlotsInfo(int filter) {
    clusterNode *n = NULL;
    int start = -1;

    for (int i = 0; i <= CLUSTER_SLOTS; i++) {
        /* Find start node and slot id. */
        if (n == NULL) {
            if (i == CLUSTER_SLOTS) break;
            n = server.cluster->slots[i];
            start = i;
            continue;
        }

        /* Generate slots info when occur different node with start
         * or end of slot. */
        if (i == CLUSTER_SLOTS || n != server.cluster->slots[i]) {
            if (!(n->flags & filter)) {
                if (!n->slot_info_pairs) {
                    n->slot_info_pairs = zmalloc(2 * n->numslots * sizeof(uint16_t));
                }
                serverAssert((n->slot_info_pairs_count + 1) < (2 * n->numslots));
                n->slot_info_pairs[n->slot_info_pairs_count++] = start;
                n->slot_info_pairs[n->slot_info_pairs_count++] = i - 1;
            }
            if (i == CLUSTER_SLOTS) break;
            n = server.cluster->slots[i];
            start = i;
        }
    }
}

void clusterFreeNodesSlotsInfo(clusterNode *n) {
    zfree(n->slot_info_pairs);
    n->slot_info_pairs = NULL;
    n->slot_info_pairs_count = 0;
}

/* -----------------------------------------------------------------------------
 * Cluster config file loading and saving
 * -------------------------------------------------------------------------- */

int clusterLoadConfig(char *filename) {
    FILE *fp = fopen(filename, "r");
    struct stat sb;
    char *line;
    int maxline, j;
    dict *tmp_cluster_nodes;

    if (fp == NULL) {
        if (errno == ENOENT) {
            return C_ERR;
        } else {
            serverLog(LL_WARNING, "Loading the cluster node config from %s: %s", filename, strerror(errno));
            exit(1);
        }
    }

    if (valkey_fstat(fileno(fp), &sb) == -1) {
        serverLog(LL_WARNING, "Unable to obtain the cluster node config file stat %s: %s", filename, strerror(errno));
        exit(1);
    }
    /* Check if the file is zero-length: if so return C_ERR to signal
     * we have to write the config. */
    if (sb.st_size == 0) {
        fclose(fp);
        return C_ERR;
    }

    /* Parse the file. Note that single lines of the cluster config file can
     * be really long as they include all the hash slots of the node.
     * This means in the worst possible case, half of the slots will be
     * present in a single line, possibly in importing or migrating state, so
     * together with the node ID of the sender/receiver.
     *
     * To simplify we allocate 1024+CLUSTER_SLOTS*128 bytes per line. */
    maxline = 1024 + CLUSTER_SLOTS * 128;
    line = zmalloc(maxline);
    tmp_cluster_nodes = dictCreate(&clusterNodesDictType);
    while (fgets(line, maxline, fp) != NULL) {
        int argc;
        sds *argv;
        clusterNode *n, *primary;
        char *p, *s;

        /* Skip blank lines, they can be created either by users manually
         * editing nodes.conf or by the config writing process if stopped
         * before the truncate() call. */
        if (line[0] == '\n' || line[0] == '\0') continue;

        /* A line without a trailing newline means the write was interrupted
         * (crash during append). Stop reading — this and anything after is
         * potentially incomplete. */
        size_t linelen = strlen(line);
        if (linelen > 0 && line[linelen - 1] != '\n') break;

        /* Split the line into arguments for processing. */
        argv = sdssplitargs(line, &argc);
        if (argv == NULL) goto fmterr;

        /* Handle the special "vars" line. Don't pretend it is the last
         * line even if it actually is when generated by the server. */
        if (strcasecmp(argv[0], "vars") == 0) {
            if (!(argc % 2)) goto fmterr;
            for (j = 1; j < argc; j += 2) {
                if (clusterCurrentBus->parseVarsLine &&
                    clusterCurrentBus->parseVarsLine(argv[j], argv[j + 1])) {
                    continue;
                }
                serverLog(LL_NOTICE, "Skipping unknown cluster config variable '%s'", argv[j]);
            }
            sdsfreesplitres(argv, argc);
            continue;
        }

        /* Handle "log" lines (protocol-specific WAL entries). */
        if (strcasecmp(argv[0], "log") == 0) {
            if (clusterCurrentBus->parseLogLine)
                clusterCurrentBus->parseLogLine(argv, argc);
            sdsfreesplitres(argv, argc);
            continue;
        }

        /* Regular config lines have at least eight fields */
        if (argc < 8) {
            sdsfreesplitres(argv, argc);
            goto fmterr;
        }

        /* Create this node if it does not exist */
        if (verifyClusterNodeId(argv[0], sdslen(argv[0])) == C_ERR) {
            sdsfreesplitres(argv, argc);
            goto fmterr;
        }
        n = clusterLookupNode(argv[0], sdslen(argv[0]));
        if (!n) {
            n = createClusterNode(argv[0], 0);
            clusterAddNode(n);
            dictAdd(tmp_cluster_nodes, sdsnewlen(argv[0], sdslen(argv[0])), NULL);
        } else {
            /* Check if the node (nodeid) has already been loaded. The nodeid is used to
             * identify every node across the entire cluster, we do not expect to find
             * duplicate nodeids in nodes.conf. */
            dictEntry *de = dictFind(tmp_cluster_nodes, argv[0]);
            if (de != NULL) {
                serverLog(LL_WARNING, "Duplicate nodeid detected: %s", argv[0]);
                sdsfreesplitres(argv, argc);
                goto fmterr;
            }
        }
        /* Format for the node address and auxiliary argument information:
         * ip:port[@cport][,hostname][,aux=val]*] */
        if (clusterNodeParseAddressString(n, argv[1]) == C_ERR) {
            sdsfreesplitres(argv, argc);
            goto fmterr;
        }

        /* Parse flags */
        p = s = argv[2];
        while (p) {
            p = strchr(s, ',');
            if (p) *p = '\0';
            if (!strcasecmp(s, "myself")) {
                serverAssert(server.cluster->myself == NULL);
                server.cluster->myself = n;
                n->flags |= CLUSTER_NODE_MYSELF;
            } else if (!strcasecmp(s, "master") || !strcasecmp(s, "primary")) {
                n->flags |= CLUSTER_NODE_PRIMARY;
            } else if (!strcasecmp(s, "slave") || !strcasecmp(s, "replica")) {
                n->flags |= CLUSTER_NODE_REPLICA;
            } else if (!strcasecmp(s, "fail?")) {
                n->flags |= CLUSTER_NODE_PFAIL;
            } else if (!strcasecmp(s, "fail")) {
                n->flags |= CLUSTER_NODE_FAIL;
                if (clusterCurrentBus->setNodeFailed) clusterCurrentBus->setNodeFailed(n);
            } else if (!strcasecmp(s, "handshake")) {
                n->flags |= CLUSTER_NODE_HANDSHAKE;
            } else if (!strcasecmp(s, "noaddr")) {
                n->flags |= (CLUSTER_NODE_NOADDR | CLUSTER_NODE_FAIL);
                if (clusterCurrentBus->setNodeFailed) clusterCurrentBus->setNodeFailed(n);
            } else if (!strcasecmp(s, "nofailover")) {
                n->flags |= CLUSTER_NODE_NOFAILOVER;
            } else if (!strcasecmp(s, "noflags")) {
                /* nothing to do */
            } else {
                serverPanic("Unknown flag in %s cluster config file", SERVER_TITLE);
            }
            if (p) s = p + 1;
        }

        /* Get primary if any. Set the primary and populate primary's
         * replica list. */
        if (argv[3][0] != '-') {
            if (verifyClusterNodeId(argv[3], sdslen(argv[3])) == C_ERR) {
                sdsfreesplitres(argv, argc);
                goto fmterr;
            }
            primary = clusterLookupNode(argv[3], sdslen(argv[3]));
            if (!primary) {
                primary = createClusterNode(argv[3], 0);
                clusterAddNode(primary);
            }
            /* shard_id can be absent if we are loading a nodes.conf generated
             * by an older version; we should follow the primary's
             * shard_id in this case */
            if (auxFieldHandlers[af_shard_id].isPresent(n) == 0) {
                memcpy(n->shard_id, primary->shard_id, CLUSTER_NAMELEN);
                clusterAddNodeToShard(primary->shard_id, n);
            } else if (clusterGetNodesInMyShard(primary) != NULL &&
                       memcmp(primary->shard_id, n->shard_id, CLUSTER_NAMELEN) != 0) {
                /* If the primary has been added to a shard and this replica has
                 * a different shard id stored in nodes.conf, update it to match
                 * the primary instead of aborting the startup. */
                serverLog(LL_NOTICE,
                          "Node %.40s has a different shard id (%.40s) than its primary %.40s (%.40s). "
                          "Updating replica's shard id to match primary's shard id.",
                          n->name, n->shard_id, primary->name, primary->shard_id);
                clusterRemoveNodeFromShard(n);
                memcpy(n->shard_id, primary->shard_id, CLUSTER_NAMELEN);
                clusterAddNodeToShard(primary->shard_id, n);
            }
            n->replicaof = primary;
            clusterNodeAddReplica(primary, n);
        } else if (auxFieldHandlers[af_shard_id].isPresent(n) == 0) {
            /* n is a primary but it does not have a persisted shard_id.
             * This happens if we are loading a nodes.conf generated by
             * an older version of the server. We should manually update the
             * shard membership in this case */
            clusterAddNodeToShard(n->shard_id, n);
        }

        /* Set ping sent / pong received timestamps and configEpoch via
         * the protocol implementation. */
        {
            uint64_t config_epoch = (nodeIsReplica(n) && n->replicaof) ? 0 : strtoull(argv[6], NULL, 10);
            if (clusterCurrentBus->setNodePingPongEpoch)
                clusterCurrentBus->setNodePingPongEpoch(n, atoi(argv[4]), atoi(argv[5]), config_epoch);
        }

        /* Populate hash slots served by this instance. */
        for (j = 8; j < argc; j++) {
            int start, stop;

            if (argv[j][0] == '[') {
                /* Here we handle migrating / importing slots */
                int slot;
                char direction;
                clusterNode *cn;

                p = strchr(argv[j], '-');
                serverAssert(p != NULL);
                *p = '\0';
                direction = p[1]; /* Either '>' or '<' */
                slot = atoi(argv[j] + 1);
                if (slot < 0 || slot >= CLUSTER_SLOTS) {
                    sdsfreesplitres(argv, argc);
                    goto fmterr;
                }
                p += 3;

                char *pr = strchr(p, ']');
                size_t node_len = pr - p;
                if (pr == NULL || verifyClusterNodeId(p, node_len) == C_ERR) {
                    sdsfreesplitres(argv, argc);
                    goto fmterr;
                }
                cn = clusterLookupNode(p, CLUSTER_NAMELEN);
                if (!cn) {
                    cn = createClusterNode(p, 0);
                    clusterAddNode(cn);
                }
                if (direction == '>') {
                    setMigratingSlotDest(slot, cn);
                } else {
                    setImportingSlotSource(slot, cn);
                }
                continue;
            } else if ((p = strchr(argv[j], '-')) != NULL) {
                *p = '\0';
                start = atoi(argv[j]);
                stop = atoi(p + 1);
            } else {
                start = stop = atoi(argv[j]);
            }
            if (start < 0 || start >= CLUSTER_SLOTS || stop < 0 || stop >= CLUSTER_SLOTS) {
                sdsfreesplitres(argv, argc);
                goto fmterr;
            }
            while (start <= stop) clusterAddSlot(n, start++);
        }

        sdsfreesplitres(argv, argc);
    }
    /* Config sanity check */
    if (server.cluster->myself == NULL) goto fmterr;

    zfree(line);
    fclose(fp);
    serverAssert(tmp_cluster_nodes != NULL);
    dictRelease(tmp_cluster_nodes);

    serverLog(LL_NOTICE, "Node configuration loaded, I'm %.40s", server.cluster->myself->name);

    /* Post-load fixups (e.g. ensuring currentEpoch >= max configEpoch). */
    if (clusterCurrentBus->postLoad) clusterCurrentBus->postLoad();
    return C_OK;

fmterr:
    serverPanic("Unrecoverable error: corrupted cluster config file \"%s\".", line);
}

/* Cluster node configuration is exactly the same as CLUSTER NODES output.
 *
 * This function writes the node config and returns C_OK, on error C_ERR
 * is returned.
 *
 * Note: we need to write the file in an atomic way from the point of view
 * of the POSIX filesystem semantics, so that if the server is stopped, or
 * the computer crashes, the file will still be consistent. The worst case
 * scenario is that the new file is not written, or written partially, in
 * which case the old file will be used by the server at the next restart.
 *
 * The approach we use is to create a new file, write the content, fsync it,
 * and then rename it to the original file name using rename(2). This way
 * if the server crashes before the rename, the old file will be used, and
 * if the server crashes after the rename, the new file will be used.
 *
 * We don't use rewrite-on-tmpfile-and-rename because we want to avoid
 * creating a new file every time the cluster config changes, since the
 * cluster config can change very frequently. Instead, we overwrite the
 * existing file with the new content, padding with newlines if the
 * new one. Since we have the full payload to write available we can use
 * a single write to write the whole file. If the pre-existing file was
 * bigger we pad our payload with newlines that are anyway ignored and truncate
 * the file afterward. */
int clusterSaveConfig(int do_fsync) {
    sds ci, tmpfilename;
    size_t content_size, offset = 0;
    ssize_t written_bytes;
    int fd = -1;
    int retval = C_ERR;
    mstime_t latency;

    /* Get the nodes description and concatenate protocol-specific vars. */
    ci = clusterGenNodesDescription(NULL, CLUSTER_NODE_HANDSHAKE, 0);
    if (clusterCurrentBus->appendVarsLine)
        ci = clusterCurrentBus->appendVarsLine(ci);
    if (clusterCurrentBus->appendLogLines)
        ci = clusterCurrentBus->appendLogLines(ci);
    content_size = sdslen(ci);

    /* Create a temp file with the new content. */
    tmpfilename = sdscatfmt(sdsempty(), "%s.tmp-%i-%I", server.cluster_configfile, (int)getpid(), mstime());
    latencyStartMonitor(latency);
    if ((fd = open(tmpfilename, O_WRONLY | O_CREAT, 0644)) == -1) {
        serverLog(LL_WARNING, "Could not open temp cluster config file: %s", strerror(errno));
        goto cleanup;
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("cluster-config-open", latency);
    latencyTraceIfNeeded(cluster, cluster_config_open, latency);
    latencyStartMonitor(latency);
    while (offset < content_size) {
        written_bytes = write(fd, ci + offset, content_size - offset);
        if (written_bytes <= 0) {
            if (errno == EINTR) continue;
            serverLog(LL_WARNING, "Failed after writing (%zd) bytes to tmp cluster config file: %s", offset,
                      strerror(errno));
            goto cleanup;
        }
        offset += written_bytes;
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("cluster-config-write", latency);
    latencyTraceIfNeeded(cluster, cluster_config_write, latency);
    if (do_fsync) {
        latencyStartMonitor(latency);
        if (valkey_fsync(fd) == -1) {
            serverLog(LL_WARNING, "Could not sync tmp cluster config file: %s", strerror(errno));
            goto cleanup;
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("cluster-config-fsync", latency);
        latencyTraceIfNeeded(cluster, cluster_config_fsync, latency);
    }

    latencyStartMonitor(latency);
    if (rename(tmpfilename, server.cluster_configfile) == -1) {
        serverLog(LL_WARNING, "Could not rename tmp cluster config file: %s", strerror(errno));
        goto cleanup;
    }
    latencyEndMonitor(latency);
    latencyAddSampleIfNeeded("cluster-config-rename", latency);
    latencyTraceIfNeeded(cluster, cluster_config_rename, latency);
    if (do_fsync) {
        latencyStartMonitor(latency);
        if (fsyncFileDir(server.cluster_configfile) == -1) {
            serverLog(LL_WARNING, "Could not sync cluster config file dir: %s", strerror(errno));
            goto cleanup;
        }
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("cluster-config-dir-fsync", latency);
        latencyTraceIfNeeded(cluster, cluster_config_dir_fsync, latency);
    }
    retval = C_OK; /* If we reached this point, everything is fine. */

cleanup:
    if (fd != -1) {
        latencyStartMonitor(latency);
        close(fd);
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("cluster-config-close", latency);
        latencyTraceIfNeeded(cluster, cluster_config_close, latency);
    }
    if (retval == C_ERR) {
        latencyStartMonitor(latency);
        unlink(tmpfilename);
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("cluster-config-unlink", latency);
        latencyTraceIfNeeded(cluster, cluster_config_unlink, latency);
    }
    sdsfree(tmpfilename);
    sdsfree(ci);
    return retval;
}

/* Save the cluster configuration file. If the save fails, exit the process. */
void clusterSaveConfigOrDie(int do_fsync) {
    if (clusterSaveConfig(do_fsync) == C_ERR) {
        serverLog(LL_WARNING, "Fatal: can't update cluster config file.");
        exit(1);
    }
}

/* Save the cluster configuration file. If the save fails, print the log. */
#define CONFIG_SAVE_LOG_ERROR_RATE 30 /* Seconds between errors logging. */
void clusterSaveConfigOrLog(int do_fsync) {
    if (clusterSaveConfig(do_fsync) == C_ERR) {
        static time_t last_save_error_log = 0;
        /* Limit logging rate to 1 line per CONFIG_SAVE_LOG_ERROR_RATE seconds. */
        if ((server.unixtime - last_save_error_log) > CONFIG_SAVE_LOG_ERROR_RATE) {
            serverLog(LL_WARNING, "Cluster config updated even though writing "
                                  "the cluster config file to disk failed.");
            last_save_error_log = server.unixtime;
        }
    }
}

/* Lock the cluster config using flock(), and retain the file descriptor used to
 * acquire the lock so that the file will be locked as long as the process is up.
 *
 * This works because we always update nodes.conf with a new version
 * in-place, reopening the file, and writing to it in place (later adjusting
 * the length with ftruncate()).
 *
 * On success C_OK is returned, otherwise an error is logged and
 * the function returns C_ERR to signal a lock was not acquired. */
int clusterLockConfig(char *filename) {
/* flock() does not exist on Solaris
 * and a fcntl-based solution won't help, as we constantly re-open that file,
 * which will release _all_ locks anyway
 */
#if !defined(__sun)
    /* To lock it, we need to open the file in a way it is created if
     * it does not exist, otherwise there is a race condition with other
     * processes. */
    int fd = open(filename, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd == -1) {
        serverLog(LL_WARNING, "Can't open %s in order to acquire a lock: %s", filename, strerror(errno));
        return C_ERR;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            serverLog(LL_WARNING,
                      "Sorry, the cluster configuration file %s is already used "
                      "by a different Cluster node. Please make sure that "
                      "different nodes use different cluster configuration "
                      "files.",
                      filename);
        } else {
            serverLog(LL_WARNING, "Impossible to lock %s: %s", filename, strerror(errno));
        }
        close(fd);
        return C_ERR;
    }
    /* Lock acquired: leak the 'fd' by not closing it until shutdown time, so that
     * we'll retain the lock to the file as long as the process exists.
     *
     * After fork, the child process will get the fd opened by the parent process,
     * we need save `fd` to `cluster_config_file_lock_fd`, so that in serverFork(),
     * it will be closed in the child process.
     * If it is not closed, when the main process is killed -9, but the child process
     * (valkey-aof-rewrite) is still alive, the fd(lock) will still be held by the
     * child process, and the main process will fail to get lock, means fail to start. */
    server.cluster_config_file_lock_fd = fd;
#else
    UNUSED(filename);
#endif /* __sun */

    return C_OK;
}
