/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * cluster_state.c contains protocol-agnostic cluster state management:
 * node accessors, node registry, shard management, slot ownership,
 * and topology queries. These functions operate on the common clusterNode
 * and clusterState fields and do not depend on any specific cluster bus
 * protocol implementation.
 */

#include "server.h"
#include "cluster.h"
#include "cluster_state.h"

/* -----------------------------------------------------------------------------
 * Bitmap helpers
 * -------------------------------------------------------------------------- */

int bitmapTestBit(unsigned char *bitmap, int pos) {
    off_t byte = pos / 8;
    int bit = pos & 7;
    return (bitmap[byte] & (1 << bit)) != 0;
}

void bitmapSetBit(unsigned char *bitmap, int pos) {
    off_t byte = pos / 8;
    int bit = pos & 7;
    bitmap[byte] |= 1 << bit;
}

void bitmapClearBit(unsigned char *bitmap, int pos) {
    off_t byte = pos / 8;
    int bit = pos & 7;
    bitmap[byte] &= ~(1 << bit);
}

/* -----------------------------------------------------------------------------
 * Node accessors
 * -------------------------------------------------------------------------- */

int clusterNodeIsPrimary(clusterNode *n) {
    return n->flags & CLUSTER_NODE_PRIMARY;
}

/* A "voting primary" is a primary node serving at least one slot.
 * This determines whether the node is relevant for cluster availability:
 * losing it may cause the cluster to go down. The name originates from
 * the legacy cluster protocol where these nodes participate in failover
 * voting, but the concept applies to any protocol. */
int clusterNodeIsVotingPrimary(clusterNode *n) {
    return (n->flags & CLUSTER_NODE_PRIMARY) && n->numslots;
}

int clusterNodeIsReplica(clusterNode *node) {
    return node->flags & CLUSTER_NODE_REPLICA;
}

char *clusterNodeGetName(clusterNode *node) {
    return node->name;
}

char *clusterNodeGetShardId(clusterNode *node) {
    return node->shard_id;
}

clusterNode *clusterNodeGetPrimary(clusterNode *node) {
    clusterNode *primary = node;
    while (primary->replicaof != NULL) {
        primary = primary->replicaof;
        if (primary == node) break;
    }
    /* Assert that a node's replicaof/primary chain does not form a cycle. */
    debugServerAssert(primary->replicaof == NULL);
    return primary;
}

int clusterNodePending(clusterNode *node) {
    return node->flags & (CLUSTER_NODE_NOADDR | CLUSTER_NODE_HANDSHAKE);
}

int clusterNodeTimedOut(clusterNode *node) {
    return node->flags & CLUSTER_NODE_PFAIL;
}

int clusterNodeIsFailing(clusterNode *node) {
    return node->flags & CLUSTER_NODE_FAIL;
}

int clusterNodeIsNoFailover(clusterNode *node) {
    return node->flags & CLUSTER_NODE_NOFAILOVER;
}

int clusterNodeNumReplicas(clusterNode *node) {
    return node->num_replicas;
}

clusterNode *clusterNodeGetReplica(clusterNode *node, int replica_idx) {
    return node->replicas[replica_idx];
}

char *clusterNodeHostname(clusterNode *node) {
    return node->hostname;
}

/* Returns the IP of the node as seen by the given client, or by the cluster
 * node if c is NULL. */
char *clusterNodeIp(clusterNode *node, client *c) {
    if (c == NULL) {
        return node->ip;
    }
    if (isClientConnIpV6(c)) {
        if (sdslen(node->announce_client_ipv6) != 0) return node->announce_client_ipv6;
    } else {
        if (sdslen(node->announce_client_ipv4) != 0) return node->announce_client_ipv4;
    }
    return node->ip;
}

const char *clusterNodePreferredEndpoint(clusterNode *n, client *c) {
    char *hostname = clusterNodeHostname(n);
    switch (server.cluster_preferred_endpoint_type) {
    case CLUSTER_ENDPOINT_TYPE_IP: return clusterNodeIp(n, c);
    case CLUSTER_ENDPOINT_TYPE_HOSTNAME: return (hostname != NULL && hostname[0] != '\0') ? hostname : "?";
    case CLUSTER_ENDPOINT_TYPE_UNKNOWN_ENDPOINT: return "";
    }
    return "unknown";
}

int getNodeDefaultClientPort(clusterNode *n) {
    return server.tls_cluster ? n->tls_port : n->tcp_port;
}

int getNodeDefaultReplicationPort(clusterNode *n) {
    return server.tls_replication ? n->tls_port : n->tcp_port;
}

int clusterNodeClientPort(clusterNode *n, int use_tls, client *c) {
    if (use_tls && c != NULL && n->announce_client_tls_port) {
        return n->announce_client_tls_port;
    } else if (use_tls) {
        return n->tls_port;
    } else if (c != NULL && n->announce_client_tcp_port) {
        return n->announce_client_tcp_port;
    } else {
        return n->tcp_port;
    }
}

long long getNodeReplicationOffset(clusterNode *node) {
    if (node->flags & CLUSTER_NODE_MYSELF) {
        return nodeIsReplica(node) ? replicationGetReplicaOffset() : server.primary_repl_offset;
    } else {
        return node->repl_offset;
    }
}

/* By default, a server doesn't have a human-readable nodename unless explicitly
 * assigned by CONFIG SET cluster-announce-human-nodename command or config file
 * edit, so we simply fall back to using the node's IP and port as the nodename.
 *
 * WARNING: THIS IS ONLY USED FOR LOGGING PURPOSE.
 *
 * Returns either the SDS field or a pointer to a thread-local scratch buffer. */
char *humanNodename(clusterNode *node) {
    if (sdslen(node->human_nodename) > 0) {
        return node->human_nodename;
    }

    /* Avoid allocating heap memory so that users can call the function with ease.
     * Use a small ring of thread-local buffers here so that multiple function calls
     * in the same logging statement are safe. */
    enum { BUF_COUNT = 8 };
    static _Thread_local char buffers[BUF_COUNT][CONN_ADDR_STR_LEN];
    static _Thread_local int idx;

    char *buffer = buffers[idx];
    idx = (idx + 1) % BUF_COUNT;

    const int port = server.tls_cluster ? node->tls_port : node->tcp_port;
    formatAddr(buffer, CONN_ADDR_STR_LEN, node->ip, port);
    return buffer;
}

/* -----------------------------------------------------------------------------
 * Topology queries
 * -------------------------------------------------------------------------- */

clusterNode *getMyClusterNode(void) {
    return server.cluster->myself;
}

int getClusterSize(void) {
    return dictSize(server.cluster->nodes);
}

int getMyShardSlotCount(void) {
    clusterNode *myself = server.cluster->myself;
    clusterNode *primary = clusterNodeGetPrimary(myself);
    return primary->numslots;
}

int isClusterHealthy(void) {
    return server.cluster->state == CLUSTER_OK;
}

clusterNode *getNodeBySlot(int slot) {
    return server.cluster->slots[slot];
}

clusterNode *getMigratingSlotDest(int slot) {
    dictEntry *de = dictFind(server.cluster->migrating_slots_to,
                             (void *)(intptr_t)slot);
    return de ? dictGetVal(de) : NULL;
}

clusterNode *getImportingSlotSource(int slot) {
    dictEntry *de = dictFind(server.cluster->importing_slots_from,
                             (void *)(intptr_t)slot);
    return de ? dictGetVal(de) : NULL;
}

int clusterNodeCoversSlot(clusterNode *n, int slot) {
    return bitmapTestBit(n->slots, slot);
}

char **getClusterNodesList(size_t *numnodes) {
    size_t count = dictSize(server.cluster->nodes);
    char **ids = zmalloc((count + 1) * sizeof(char *));
    dictIterator *di = dictGetIterator(server.cluster->nodes);
    dictEntry *de;
    int j = 0;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->flags & (CLUSTER_NODE_NOADDR | CLUSTER_NODE_HANDSHAKE)) continue;
        ids[j] = zmalloc(CLUSTER_NAMELEN);
        memcpy(ids[j], node->name, CLUSTER_NAMELEN);
        j++;
    }
    *numnodes = j;
    ids[j] = NULL; /* Null term so that FreeClusterNodesList does not need
                    * to also get the count argument. */
    dictReleaseIterator(di);
    return ids;
}

/* -----------------------------------------------------------------------------
 * Server port configuration
 * -------------------------------------------------------------------------- */

/* Returns the default client-facing port (TLS port if TLS cluster, else TCP). */
int defaultClientPort(void) {
    return server.tls_cluster ? server.tls_port : server.port;
}

/* Derives our ports to be announced in the cluster bus. */
void deriveAnnouncedPorts(int *announced_tcp_port,
                          int *announced_tls_port,
                          int *announced_cport,
                          int *announced_client_tcp_port,
                          int *announced_client_tls_port) {
    /* Config overriding announced ports. */
    *announced_tcp_port = server.cluster_announce_port ? server.cluster_announce_port : server.port;
    *announced_tls_port = server.cluster_announce_tls_port ? server.cluster_announce_tls_port : server.tls_port;
    /* Derive cluster bus port. */
    if (server.cluster_announce_bus_port) {
        *announced_cport = server.cluster_announce_bus_port;
    } else if (server.cluster_port) {
        *announced_cport = server.cluster_port;
    } else {
        *announced_cport = defaultClientPort() + CLUSTER_PORT_INCR;
    }

    *announced_client_tcp_port = server.cluster_announce_client_port;
    *announced_client_tls_port = server.cluster_announce_client_tls_port;
}

/* -----------------------------------------------------------------------------
 * Node registry
 * -------------------------------------------------------------------------- */

/* Node lookup by name */
clusterNode *clusterLookupNode(const char *name, int length) {
    if (verifyClusterNodeId(name, length) != C_OK) return NULL;
    sds s = sdsnewlen(name, length);
    dictEntry *de = dictFind(server.cluster->nodes, s);
    sdsfree(s);
    if (de == NULL) return NULL;
    return dictGetVal(de);
}

void clusterAddNode(clusterNode *node) {
    int retval;
    retval = dictAdd(server.cluster->nodes, sdsnewlen(node->name, CLUSTER_NAMELEN), node);
    serverAssert(retval == DICT_OK);
}

/* -----------------------------------------------------------------------------
 * Shard management
 * -------------------------------------------------------------------------- */

list *clusterGetNodesInMyShard(clusterNode *node) {
    sds s = sdsnewlen(node->shard_id, CLUSTER_NAMELEN);
    dictEntry *de = dictFind(server.cluster->shards, s);
    sdsfree(s);
    return (de != NULL) ? dictGetVal(de) : NULL;
}

void clusterAddNodeToShard(const char *shard_id, clusterNode *node) {
    sds s = sdsnewlen(shard_id, CLUSTER_NAMELEN);
    dictEntry *de = dictFind(server.cluster->shards, s);
    if (de == NULL) {
        list *l = listCreate();
        listAddNodeTail(l, node);
        serverAssert(dictAdd(server.cluster->shards, s, l) == DICT_OK);
    } else {
        list *l = dictGetVal(de);
        if (listSearchKey(l, node) == NULL) {
            listAddNodeTail(l, node);
        }
        sdsfree(s);
    }
}

void clusterRemoveNodeFromShard(clusterNode *node) {
    sds s = sdsnewlen(node->shard_id, CLUSTER_NAMELEN);
    dictEntry *de = dictFind(server.cluster->shards, s);
    if (de != NULL) {
        list *l = dictGetVal(de);
        listNode *ln = listSearchKey(l, node);
        if (ln != NULL) {
            listDelNode(l, ln);
        }
        if (listLength(l) == 0) {
            dictDelete(server.cluster->shards, s);
        }
    }
    sdsfree(s);
}

/* -----------------------------------------------------------------------------
 * Slot migration state
 * -------------------------------------------------------------------------- */

void setMigratingSlotDest(int slot, clusterNode *node) {
    dictEntry *de = dictFind(server.cluster->migrating_slots_to,
                             (void *)(intptr_t)slot);
    if (node == NULL) {
        if (de) dictDelete(server.cluster->migrating_slots_to,
                           (void *)(intptr_t)slot);
        return;
    }
    if (de) {
        dictSetVal(server.cluster->migrating_slots_to, de, node);
    } else {
        dictAdd(server.cluster->migrating_slots_to,
                (void *)(intptr_t)slot, node);
    }
}

void setImportingSlotSource(int slot, clusterNode *node) {
    dictEntry *de = dictFind(server.cluster->importing_slots_from,
                             (void *)(intptr_t)slot);
    if (node == NULL) {
        if (de) dictDelete(server.cluster->importing_slots_from,
                           (void *)(intptr_t)slot);
        return;
    }
    if (de) {
        dictSetVal(server.cluster->importing_slots_from, de, node);
    } else {
        dictAdd(server.cluster->importing_slots_from,
                (void *)(intptr_t)slot, node);
    }
}

/* Clear the migrating / importing state for all the slots.
 * This is useful at initialization and when turning a primary into replica. */
void clusterCloseAllSlots(void) {
    dictEmpty(server.cluster->migrating_slots_to, NULL);
    dictEmpty(server.cluster->importing_slots_from, NULL);
}

int clusterNodeNameComparator(const void *node1, const void *node2) {
    return strncasecmp((*(clusterNode **)node1)->name, (*(clusterNode **)node2)->name, CLUSTER_NAMELEN);
}

int clusterNodeRemoveReplica(clusterNode *primary, clusterNode *replica) {
    int j;

    for (j = 0; j < primary->num_replicas; j++) {
        if (primary->replicas[j] == replica) {
            if ((j + 1) < primary->num_replicas) {
                int remaining_replicas = (primary->num_replicas - j) - 1;
                memmove(primary->replicas + j, primary->replicas + (j + 1),
                        (sizeof(*primary->replicas) * remaining_replicas));
            }
            primary->num_replicas--;
            if (primary->num_replicas == 0) primary->flags &= ~CLUSTER_NODE_MIGRATE_TO;
            return C_OK;
        }
    }
    return C_ERR;
}

int clusterNodeAddReplica(clusterNode *primary, clusterNode *replica) {
    int j;

    /* If it's already a replica, don't add it again. */
    for (j = 0; j < primary->num_replicas; j++)
        if (primary->replicas[j] == replica) return C_ERR;
    primary->replicas = zrealloc(primary->replicas, sizeof(clusterNode *) * (primary->num_replicas + 1));
    primary->replicas[primary->num_replicas] = replica;
    primary->num_replicas++;
    qsort(primary->replicas, primary->num_replicas, sizeof(clusterNode *), clusterNodeNameComparator);
    primary->flags |= CLUSTER_NODE_MIGRATE_TO;
    return C_OK;
}

int clusterCountNonFailingReplicas(clusterNode *n) {
    int j, ok_replicas = 0;

    for (j = 0; j < n->num_replicas; j++)
        if (!nodeFailed(n->replicas[j])) ok_replicas++;
    return ok_replicas;
}

/* Return non-zero if there is at least one primary with replicas in the cluster.
 * Otherwise zero is returned. Used by clusterNodeSetSlotBit() to set the
 * MIGRATE_TO flag the when a primary gets the first slot. */
int clusterPrimariesHaveReplicas(void) {
    dictIterator di;
    dictInitIterator(&di, server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(&di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (nodeIsReplica(node)) continue;
        if (node->num_replicas) return 1;
    }
    return 0;
}

/* Clear the slot bit and return the old value. */
int clusterNodeClearSlotBit(clusterNode *n, int slot) {
    int old = bitmapTestBit(n->slots, slot);
    if (old) {
        bitmapClearBit(n->slots, slot);
        n->numslots--;
    }
    return old;
}

bool isAnySlotInManualImportingState(void) {
    return dictSize(server.cluster->importing_slots_from) > 0;
}

bool isAnySlotInManualMigratingState(void) {
    return dictSize(server.cluster->migrating_slots_to) > 0;
}
