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
 * cluster_legacy.c contains the implementation of the cluster API that is
 * specific to the standard, cluster-bus based clustering mechanism.
 */

#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"
#include "cluster_slot_stats.h"
#include "endianconv.h"
#include "connection.h"

#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <math.h>
#include <sys/file.h>

/* A global reference to myself is handy to make code more clear.
 * Myself always points to server.cluster->myself, that is, the clusterNode
 * that represents this node. */
clusterNode *myself = NULL;

clusterNode *createClusterNode(char *nodename, int flags);
void clusterAddNode(clusterNode *node);
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterReadHandler(connection *conn);
void clusterSendPing(clusterLink *link, int type);
void clusterSendFail(char *nodename);
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request);
void clusterUpdateState(void);
list *clusterGetNodesInMyShard(clusterNode *node);
int clusterNodeAddReplica(clusterNode *primary, clusterNode *replica);
int clusterAddSlot(clusterNode *n, int slot);
int clusterDelSlot(int slot);
int clusterDelNodeSlots(clusterNode *node);
int clusterNodeSetSlotBit(clusterNode *n, int slot);
static void clusterSetPrimary(clusterNode *n, int closeSlots, int full_sync_required);
void clusterHandleReplicaFailover(void);
void clusterHandleReplicaMigration(int max_replicas);
int bitmapTestBit(unsigned char *bitmap, int pos);
void bitmapSetBit(unsigned char *bitmap, int pos);
void bitmapClearBit(unsigned char *bitmap, int pos);
void clusterDoBeforeSleep(int flags);
void clusterSendUpdate(clusterLink *link, clusterNode *node);
void resetManualFailover(void);
void clusterCloseAllSlots(void);
void clusterSetNodeAsPrimary(clusterNode *n);
void clusterDelNode(clusterNode *delnode);
sds representClusterNodeFlags(sds ci, uint16_t flags);
sds representSlotInfo(sds ci, uint16_t *slot_info_pairs, int slot_info_pairs_count);
void clusterFreeNodesSlotsInfo(clusterNode *n);
uint64_t clusterGetMaxEpoch(void);
int clusterBumpConfigEpochWithoutConsensus(void);
void moduleCallClusterReceivers(const char *sender_id,
                                uint64_t module_id,
                                uint8_t type,
                                const unsigned char *payload,
                                uint32_t len);
const char *clusterGetMessageTypeString(int type);
void removeChannelsInSlot(unsigned int slot);
unsigned int countChannelsInSlot(unsigned int hashslot);
unsigned int delKeysInSlot(unsigned int hashslot);
void clusterAddNodeToShard(const char *shard_id, clusterNode *node);
list *clusterLookupNodeListByShardId(const char *shard_id);
void clusterRemoveNodeFromShard(clusterNode *node);
int auxShardIdSetter(clusterNode *n, void *value, size_t length);
sds auxShardIdGetter(clusterNode *n, sds s);
int auxShardIdPresent(clusterNode *n);
int auxHumanNodenameSetter(clusterNode *n, void *value, size_t length);
sds auxHumanNodenameGetter(clusterNode *n, sds s);
int auxHumanNodenamePresent(clusterNode *n);
int auxAnnounceClientIpV4Setter(clusterNode *n, void *value, size_t length);
sds auxAnnounceClientIpV4Getter(clusterNode *n, sds s);
int auxAnnounceClientIpV4Present(clusterNode *n);
int auxAnnounceClientIpV6Setter(clusterNode *n, void *value, size_t length);
sds auxAnnounceClientIpV6Getter(clusterNode *n, sds s);
int auxAnnounceClientIpV6Present(clusterNode *n);
int auxTcpPortSetter(clusterNode *n, void *value, size_t length);
sds auxTcpPortGetter(clusterNode *n, sds s);
int auxTcpPortPresent(clusterNode *n);
int auxTlsPortSetter(clusterNode *n, void *value, size_t length);
sds auxTlsPortGetter(clusterNode *n, sds s);
int auxTlsPortPresent(clusterNode *n);
static void clusterBuildMessageHdrLight(clusterMsgLight *hdr, int type, size_t msglen);
static void clusterBuildMessageHdr(clusterMsg *hdr, int type, size_t msglen);
void freeClusterLink(clusterLink *link);
int verifyClusterNodeId(const char *name, int length);
sds clusterEncodeOpenSlotsAuxField(int rdbflags);
int clusterDecodeOpenSlotsAuxField(int rdbflags, sds s);

/* Only primaries that own slots have voting rights.
 * Returns 1 if the node has voting rights, otherwise returns 0. */
static inline int clusterNodeIsVotingPrimary(clusterNode *n) {
    return (n->flags & CLUSTER_NODE_PRIMARY) && n->numslots;
}

int getNodeDefaultClientPort(clusterNode *n) {
    return server.tls_cluster ? n->tls_port : n->tcp_port;
}

static inline int getNodeDefaultReplicationPort(clusterNode *n) {
    return server.tls_replication ? n->tls_port : n->tcp_port;
}

int clusterNodeClientPort(clusterNode *n, int use_tls) {
    return use_tls ? n->tls_port : n->tcp_port;
}

static inline int defaultClientPort(void) {
    return server.tls_cluster ? server.tls_port : server.port;
}

#define isSlotUnclaimed(slot) \
    (server.cluster->slots[slot] == NULL || bitmapTestBit(server.cluster->owner_not_claiming_slot, slot))

#define RCVBUF_INIT_LEN 1024
#define RCVBUF_MIN_READ_LEN 14
static_assert(offsetof(clusterMsg, type) + sizeof(uint16_t) == RCVBUF_MIN_READ_LEN,
              "Incorrect length to read to identify type");

#define RCVBUF_MAX_PREALLOC (1 << 20) /* 1MB */

/* Fixed timeout value for cluster operations (milliseconds) */
#define CLUSTER_OPERATION_TIMEOUT 2000

/* Cluster nodes hash table, mapping nodes addresses 1.2.3.4:6379 to
 * clusterNode structures. */
dictType clusterNodesDictType = {
    dictSdsHash,       /* hash function */
    NULL,              /* key dup */
    dictSdsKeyCompare, /* key compare */
    dictSdsDestructor, /* key destructor */
    NULL,              /* val destructor */
    NULL               /* allow to expand */
};

/* Cluster re-addition blacklist. This maps node IDs to the time
 * we can re-add this node. The goal is to avoid reading a removed
 * node for some time. */
dictType clusterNodesBlackListDictType = {
    dictSdsCaseHash,       /* hash function */
    NULL,                  /* key dup */
    dictSdsKeyCaseCompare, /* key compare */
    dictSdsDestructor,     /* key destructor */
    NULL,                  /* val destructor */
    NULL                   /* allow to expand */
};

/* Cluster shards hash table, mapping shard id to list of nodes */
dictType clusterSdsToListType = {
    dictSdsHash,        /* hash function */
    NULL,               /* key dup */
    dictSdsKeyCompare,  /* key compare */
    dictSdsDestructor,  /* key destructor */
    dictListDestructor, /* val destructor */
    NULL                /* allow to expand */
};

typedef struct {
    enum {
        ITER_DICT,
        ITER_LIST
    } type;
    union {
        dictIterator di;
        listIter li;
    };
} ClusterNodeIterator;

static void clusterNodeIterInitAllNodes(ClusterNodeIterator *iter) {
    iter->type = ITER_DICT;
    dictInitSafeIterator(&iter->di, server.cluster->nodes);
}

static void clusterNodeIterInitMyShard(ClusterNodeIterator *iter) {
    list *nodes = clusterGetNodesInMyShard(server.cluster->myself);
    serverAssert(nodes != NULL);
    iter->type = ITER_LIST;
    listRewind(nodes, &iter->li);
}

static clusterNode *clusterNodeIterNext(ClusterNodeIterator *iter) {
    switch (iter->type) {
    case ITER_DICT: {
        /* Get the next entry in the dictionary */
        dictEntry *de = dictNext(&iter->di);
        /* Return the value associated with the entry, or NULL if no more entries */
        return de ? dictGetVal(de) : NULL;
    }
    case ITER_LIST: {
        /* Get the next node in the list */
        listNode *ln = listNext(&iter->li);
        /* Return the value associated with the node, or NULL if no more nodes */
        return ln ? listNodeValue(ln) : NULL;
    }
    }
    serverPanic("Unknown iterator type %d", iter->type);
}

static void clusterNodeIterReset(ClusterNodeIterator *iter) {
    if (iter->type == ITER_DICT) {
        dictResetIterator(&iter->di);
    }
}

/* Aux fields were introduced in Redis OSS 7.2 to support the persistence
 * of various important node properties, such as shard id, in nodes.conf.
 * Aux fields take an explicit format of name=value pairs and have no
 * intrinsic order among them. Aux fields are always grouped together
 * at the end of the second column of each row after the node's IP
 * address/port/cluster_port and the optional hostname. Aux fields
 * are separated by ','. */

/* Aux field setter function prototype
 * return C_OK when the update is successful; C_ERR otherwise */
typedef int(aux_value_setter)(clusterNode *n, void *value, size_t length);
/* Aux field getter function prototype
 * return an sds that is a concatenation of the input sds string and
 * the aux value */
typedef sds(aux_value_getter)(clusterNode *n, sds s);

typedef int(aux_value_present)(clusterNode *n);

typedef struct {
    char *field;
    aux_value_setter *setter;
    aux_value_getter *getter;
    aux_value_present *isPresent;
} auxFieldHandler;

/* Assign index to each aux field */
typedef enum {
    af_shard_id,
    af_human_nodename,
    af_tcp_port,
    af_tls_port,
    af_announce_client_ipv4,
    af_announce_client_ipv6,
    af_count, /* must be the last field */
} auxFieldIndex;

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
};

int auxShardIdSetter(clusterNode *n, void *value, size_t length) {
    if (verifyClusterNodeId(value, length) == C_ERR) {
        return C_ERR;
    }
    memcpy(n->shard_id, value, CLUSTER_NAMELEN);
    /* if n already has replicas, make sure they all agree
     * on the shard id */
    for (int i = 0; i < n->num_replicas; i++) {
        if (memcmp(n->replicas[i]->shard_id, n->shard_id, CLUSTER_NAMELEN) != 0) {
            return C_ERR;
        }
    }
    clusterAddNodeToShard(value, n);
    return C_OK;
}

sds auxShardIdGetter(clusterNode *n, sds s) {
    return sdscatprintf(s, "%.40s", n->shard_id);
}

int auxShardIdPresent(clusterNode *n) {
    return strlen(n->shard_id);
}

int auxHumanNodenameSetter(clusterNode *n, void *value, size_t length) {
    if (sdslen(n->human_nodename) == length && !strncmp(value, n->human_nodename, length)) {
        return C_OK;
    }

    n->human_nodename = sdscpylen(n->human_nodename, value, length);
    return C_OK;
}

sds auxHumanNodenameGetter(clusterNode *n, sds s) {
    return sdscatprintf(s, "%s", n->human_nodename);
}

int auxHumanNodenamePresent(clusterNode *n) {
    return sdslen(n->human_nodename);
}

int auxAnnounceClientIpV4Setter(clusterNode *n, void *value, size_t length) {
    if (sdslen(n->announce_client_ipv4) == length && !strncmp(value, n->announce_client_ipv4, length)) {
        /* Unchanged value */
        return C_OK;
    }

    if (length != 0) {
        /* Validate IPv4 address */
        struct sockaddr_in sa;
        if (inet_pton(AF_INET, (const char *)value, &(sa.sin_addr)) == 0) {
            return C_ERR;
        }
    }

    n->announce_client_ipv4 = sdscpylen(n->announce_client_ipv4, value, length);
    return C_OK;
}

sds auxAnnounceClientIpV4Getter(clusterNode *n, sds s) {
    return sdscatprintf(s, "%s", n->announce_client_ipv4);
}

int auxAnnounceClientIpV4Present(clusterNode *n) {
    return sdslen(n->announce_client_ipv4) != 0;
}

int auxAnnounceClientIpV6Setter(clusterNode *n, void *value, size_t length) {
    if (sdslen(n->announce_client_ipv6) == length && !strncmp(value, n->announce_client_ipv6, length)) {
        /* Unchanged value */
        return C_OK;
    }

    if (length != 0) {
        /* Validate IPv6 address */
        struct sockaddr_in6 sa;
        if (inet_pton(AF_INET6, (const char *)value, &(sa.sin6_addr)) == 0) {
            return C_ERR;
        }
    }

    n->announce_client_ipv6 = sdscpylen(n->announce_client_ipv6, value, length);
    return C_OK;
}

sds auxAnnounceClientIpV6Getter(clusterNode *n, sds s) {
    return sdscatprintf(s, "%s", n->announce_client_ipv6);
}

int auxAnnounceClientIpV6Present(clusterNode *n) {
    return sdslen(n->announce_client_ipv6) != 0;
}

int auxTcpPortSetter(clusterNode *n, void *value, size_t length) {
    if (length > 5 || length < 1) {
        return C_ERR;
    }
    char buf[length + 1];
    memcpy(buf, (char *)value, length);
    buf[length] = '\0';
    n->tcp_port = atoi(buf);
    return (n->tcp_port < 0 || n->tcp_port >= 65536) ? C_ERR : C_OK;
}

sds auxTcpPortGetter(clusterNode *n, sds s) {
    return sdscatprintf(s, "%d", n->tcp_port);
}

int auxTcpPortPresent(clusterNode *n) {
    return n->tcp_port >= 0 && n->tcp_port < 65536;
}

int auxTlsPortSetter(clusterNode *n, void *value, size_t length) {
    if (length > 5 || length < 1) {
        return C_ERR;
    }
    char buf[length + 1];
    memcpy(buf, (char *)value, length);
    buf[length] = '\0';
    n->tls_port = atoi(buf);
    return (n->tls_port < 0 || n->tls_port >= 65536) ? C_ERR : C_OK;
}

sds auxTlsPortGetter(clusterNode *n, sds s) {
    return sdscatprintf(s, "%d", n->tls_port);
}

int auxTlsPortPresent(clusterNode *n) {
    return n->tls_port >= 0 && n->tls_port < 65536;
}

/* clusterLink send queue blocks */
typedef struct {
    size_t totlen; /* Total length of this block including the message */
    int refcount;  /* Number of cluster link send msg queues containing the message */
    union {
        clusterMsg msg;
        clusterMsgLight msg_light;
    };
} clusterMsgSendBlock;

/* -----------------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------------- */

/* Load the cluster config from 'filename'.
 *
 * If the file does not exist or is zero-length (this may happen because
 * when we lock the nodes.conf file, we create a zero-length one for the
 * sake of locking if it does not already exist), C_ERR is returned.
 * If the configuration was loaded from the file, C_OK is returned. */
int clusterLoadConfig(char *filename) {
    FILE *fp = fopen(filename, "r");
    struct stat sb;
    char *line;
    int maxline, j;

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
    while (fgets(line, maxline, fp) != NULL) {
        int argc, aux_argc;
        sds *argv, *aux_argv;
        clusterNode *n, *primary;
        char *p, *s;

        /* Skip blank lines, they can be created either by users manually
         * editing nodes.conf or by the config writing process if stopped
         * before the truncate() call. */
        if (line[0] == '\n' || line[0] == '\0') continue;

        /* Split the line into arguments for processing. */
        argv = sdssplitargs(line, &argc);
        if (argv == NULL) goto fmterr;

        /* Handle the special "vars" line. Don't pretend it is the last
         * line even if it actually is when generated by the server. */
        if (strcasecmp(argv[0], "vars") == 0) {
            if (!(argc % 2)) goto fmterr;
            for (j = 1; j < argc; j += 2) {
                if (strcasecmp(argv[j], "currentEpoch") == 0) {
                    server.cluster->currentEpoch = strtoull(argv[j + 1], NULL, 10);
                } else if (strcasecmp(argv[j], "lastVoteEpoch") == 0) {
                    server.cluster->lastVoteEpoch = strtoull(argv[j + 1], NULL, 10);
                } else {
                    serverLog(LL_NOTICE, "Skipping unknown cluster config variable '%s'", argv[j]);
                }
            }
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
        }
        /* Format for the node address and auxiliary argument information:
         * ip:port[@cport][,hostname][,aux=val]*] */

        aux_argv = sdssplitlen(argv[1], sdslen(argv[1]), ",", 1, &aux_argc);
        if (aux_argv == NULL) {
            sdsfreesplitres(argv, argc);
            goto fmterr;
        }

        /* Hostname is an optional argument that defines the endpoint
         * that can be reported to clients instead of IP. */
        if (aux_argc > 1 && sdslen(aux_argv[1]) > 0) {
            n->hostname = sdscpy(n->hostname, aux_argv[1]);
        } else if (sdslen(n->hostname) != 0) {
            sdsclear(n->hostname);
        }

        /* All fields after hostname are auxiliary and they take on
         * the format of "aux=val" where both aux and val can contain
         * characters that pass the isValidAuxChar check only. The order
         * of the aux fields is insignificant. */
        int aux_tcp_port = 0;
        int aux_tls_port = 0;
        for (int i = 2; i < aux_argc; i++) {
            int field_argc;
            sds *field_argv;
            field_argv = sdssplitlen(aux_argv[i], sdslen(aux_argv[i]), "=", 1, &field_argc);
            if (field_argv == NULL || field_argc != 2) {
                /* Invalid aux field format */
                if (field_argv != NULL) sdsfreesplitres(field_argv, field_argc);
                sdsfreesplitres(aux_argv, aux_argc);
                sdsfreesplitres(argv, argc);
                goto fmterr;
            }

            /* Validate that both aux and value contain valid characters only */
            for (unsigned j = 0; j < 2; j++) {
                if (!isValidAuxString(field_argv[j], sdslen(field_argv[j]))) {
                    /* Invalid aux field format */
                    sdsfreesplitres(field_argv, field_argc);
                    sdsfreesplitres(aux_argv, aux_argc);
                    sdsfreesplitres(argv, argc);
                    goto fmterr;
                }
            }

            /* Note that we don't expect lots of aux fields in the foreseeable
             * future so a linear search is completely fine. */
            int field_found = 0;
            for (unsigned j = 0; j < numElements(auxFieldHandlers); j++) {
                if (sdslen(field_argv[0]) != strlen(auxFieldHandlers[j].field) ||
                    memcmp(field_argv[0], auxFieldHandlers[j].field, sdslen(field_argv[0])) != 0) {
                    continue;
                }
                field_found = 1;
                aux_tcp_port |= j == af_tcp_port;
                aux_tls_port |= j == af_tls_port;
                if (auxFieldHandlers[j].setter(n, field_argv[1], sdslen(field_argv[1])) != C_OK) {
                    /* Invalid aux field format */
                    sdsfreesplitres(field_argv, field_argc);
                    sdsfreesplitres(aux_argv, aux_argc);
                    sdsfreesplitres(argv, argc);
                    goto fmterr;
                }
            }

            if (field_found == 0) {
                /* Invalid aux field format */
                sdsfreesplitres(field_argv, field_argc);
                sdsfreesplitres(aux_argv, aux_argc);
                sdsfreesplitres(argv, argc);
                goto fmterr;
            }

            sdsfreesplitres(field_argv, field_argc);
        }
        /* Address and port */
        if ((p = strrchr(aux_argv[0], ':')) == NULL) {
            sdsfreesplitres(aux_argv, aux_argc);
            sdsfreesplitres(argv, argc);
            goto fmterr;
        }
        *p = '\0';
        memcpy(n->ip, aux_argv[0], strlen(aux_argv[0]) + 1);
        char *port = p + 1;
        char *busp = strchr(port, '@');
        if (busp) {
            *busp = '\0';
            busp++;
        }
        /* If neither TCP or TLS port is found in aux field, it is considered
         * an old version of nodes.conf file.*/
        if (!aux_tcp_port && !aux_tls_port) {
            if (server.tls_cluster) {
                n->tls_port = atoi(port);
            } else {
                n->tcp_port = atoi(port);
            }
        } else if (!aux_tcp_port) {
            n->tcp_port = atoi(port);
        } else if (!aux_tls_port) {
            n->tls_port = atoi(port);
        }
        /* In older versions of nodes.conf the "@busport" part is missing.
         * In this case we set it to the default offset of 10000 from the
         * base port. */
        n->cport = busp ? atoi(busp) : (getNodeDefaultClientPort(n) + CLUSTER_PORT_INCR);

        /* The plaintext port for client in a TLS cluster (n->pport) is not
         * stored in nodes.conf. It is received later over the bus protocol. */

        sdsfreesplitres(aux_argv, aux_argc);

        /* Parse flags */
        p = s = argv[2];
        while (p) {
            p = strchr(s, ',');
            if (p) *p = '\0';
            if (!strcasecmp(s, "myself")) {
                serverAssert(server.cluster->myself == NULL);
                myself = server.cluster->myself = n;
                n->flags |= CLUSTER_NODE_MYSELF;
            } else if (!strcasecmp(s, "master") || !strcasecmp(s, "primary")) {
                n->flags |= CLUSTER_NODE_PRIMARY;
            } else if (!strcasecmp(s, "slave") || !strcasecmp(s, "replica")) {
                n->flags |= CLUSTER_NODE_REPLICA;
            } else if (!strcasecmp(s, "fail?")) {
                n->flags |= CLUSTER_NODE_PFAIL;
            } else if (!strcasecmp(s, "fail")) {
                n->flags |= CLUSTER_NODE_FAIL;
                n->fail_time = mstime();
            } else if (!strcasecmp(s, "handshake")) {
                n->flags |= CLUSTER_NODE_HANDSHAKE;
            } else if (!strcasecmp(s, "noaddr")) {
                n->flags |= CLUSTER_NODE_NOADDR;
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
                /* If the primary has been added to a shard, make sure this
                 * node has the same persisted shard id as the primary. */
                sdsfreesplitres(argv, argc);
                goto fmterr;
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

        /* Set ping sent / pong received timestamps */
        if (atoi(argv[4])) n->ping_sent = mstime();
        if (atoi(argv[5])) n->pong_received = mstime();

        /* Set configEpoch for this node.
         * If the node is a replica, set its config epoch to 0.
         * If it's a primary, load the config epoch from the configuration file. */
        n->configEpoch = (nodeIsReplica(n) && n->replicaof) ? 0 : strtoull(argv[6], NULL, 10);

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
                    server.cluster->migrating_slots_to[slot] = cn;
                } else {
                    server.cluster->importing_slots_from[slot] = cn;
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

    serverLog(LL_NOTICE, "Node configuration loaded, I'm %.40s", myself->name);

    /* Something that should never happen: currentEpoch smaller than
     * the max epoch found in the nodes configuration. However we handle this
     * as some form of protection against manual editing of critical files. */
    if (clusterGetMaxEpoch() > server.cluster->currentEpoch) {
        server.cluster->currentEpoch = clusterGetMaxEpoch();
    }
    return C_OK;

fmterr:
    serverLog(LL_WARNING, "Unrecoverable error: corrupted cluster config file \"%s\".", line);
    zfree(line);
    if (fp) fclose(fp);
    exit(1);
}

/* Cluster node configuration is exactly the same as CLUSTER NODES output.
 *
 * This function writes the node config and returns C_OK, on error C_ERR
 * is returned.
 *
 * Note: we need to write the file in an atomic way from the point of view
 * of the POSIX filesystem semantics, so that if the server is stopped
 * or crashes during the write, we'll end with either the old file or the
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

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_SAVE_CONFIG;

    /* Get the nodes description and concatenate our "vars" directive to
     * save currentEpoch and lastVoteEpoch. */
    ci = clusterGenNodesDescription(NULL, CLUSTER_NODE_HANDSHAKE, 0);
    ci = sdscatprintf(ci, "vars currentEpoch %llu lastVoteEpoch %llu\n",
                      (unsigned long long)server.cluster->currentEpoch,
                      (unsigned long long)server.cluster->lastVoteEpoch);
    content_size = sdslen(ci);

    /* Create a temp file with the new content. */
    tmpfilename = sdscatfmt(sdsempty(), "%s.tmp-%i-%I", server.cluster_configfile, (int)getpid(), mstime());
    if ((fd = open(tmpfilename, O_WRONLY | O_CREAT, 0644)) == -1) {
        serverLog(LL_WARNING, "Could not open temp cluster config file: %s", strerror(errno));
        goto cleanup;
    }

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

    if (do_fsync) {
        server.cluster->todo_before_sleep &= ~CLUSTER_TODO_FSYNC_CONFIG;
        if (valkey_fsync(fd) == -1) {
            serverLog(LL_WARNING, "Could not sync tmp cluster config file: %s", strerror(errno));
            goto cleanup;
        }
    }

    if (rename(tmpfilename, server.cluster_configfile) == -1) {
        serverLog(LL_WARNING, "Could not rename tmp cluster config file: %s", strerror(errno));
        goto cleanup;
    }

    if (do_fsync) {
        if (fsyncFileDir(server.cluster_configfile) == -1) {
            serverLog(LL_WARNING, "Could not sync cluster config file dir: %s", strerror(errno));
            goto cleanup;
        }
    }
    retval = C_OK; /* If we reached this point, everything is fine. */

cleanup:
    if (fd != -1) close(fd);
    if (retval) unlink(tmpfilename);
    sdsfree(tmpfilename);
    sdsfree(ci);
    return retval;
}

void clusterSaveConfigOrDie(int do_fsync) {
    if (clusterSaveConfig(do_fsync) == C_ERR) {
        serverLog(LL_WARNING, "Fatal: can't update cluster config file.");
        exit(1);
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

/* Derives our ports to be announced in the cluster bus. */
void deriveAnnouncedPorts(int *announced_tcp_port, int *announced_tls_port, int *announced_cport) {
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
}

/* Some flags (currently just the NOFAILOVER flag) may need to be updated
 * in the "myself" node based on the current configuration of the node,
 * that may change at runtime via CONFIG SET. This function changes the
 * set of flags in myself->flags accordingly. */
void clusterUpdateMyselfFlags(void) {
    if (!myself) return;
    int oldflags = myself->flags;
    int nofailover = server.cluster_replica_no_failover ? CLUSTER_NODE_NOFAILOVER : 0;
    myself->flags &= ~CLUSTER_NODE_NOFAILOVER;
    myself->flags |= nofailover;
    myself->flags |= CLUSTER_NODE_LIGHT_HDR_SUPPORTED;
    if (myself->flags != oldflags) {
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
    }
}


/* We want to take myself->port/cport/pport in sync with the
 * cluster-announce-port/cluster-announce-bus-port/cluster-announce-tls-port option.
 * The option can be set at runtime via CONFIG SET. */
void clusterUpdateMyselfAnnouncedPorts(void) {
    if (!myself) return;
    deriveAnnouncedPorts(&myself->tcp_port, &myself->tls_port, &myself->cport);
}

/* We want to take myself->ip in sync with the cluster-announce-ip option.
 * The option can be set at runtime via CONFIG SET. */
void clusterUpdateMyselfIp(void) {
    if (!myself) return;
    static char *prev_ip = NULL;
    char *curr_ip = server.cluster_announce_ip;
    int changed = 0;

    if (prev_ip == NULL && curr_ip != NULL)
        changed = 1;
    else if (prev_ip != NULL && curr_ip == NULL)
        changed = 1;
    else if (prev_ip && curr_ip && strcmp(prev_ip, curr_ip))
        changed = 1;

    if (changed) {
        if (prev_ip) zfree(prev_ip);
        prev_ip = curr_ip;

        if (curr_ip) {
            /* We always take a copy of the previous IP address, by
             * duplicating the string. This way later we can check if
             * the address really changed. */
            prev_ip = zstrdup(prev_ip);
            valkey_strlcpy(myself->ip, server.cluster_announce_ip, NET_IP_STR_LEN);
        } else {
            myself->ip[0] = '\0'; /* Force autodetection. */
        }
    }
}

static void updateSdsExtensionField(char **field, const char *value) {
    if (value != NULL && !strcmp(value, *field)) {
        return;
    } else if (value == NULL && sdslen(*field) == 0) {
        return;
    }

    if (value != NULL) {
        *field = sdscpy(*field, value);
    } else {
        sdsclear(*field);
    }
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
}

/* Update the hostname for the specified node with the provided C string. */
static void updateAnnouncedHostname(clusterNode *node, char *value) {
    updateSdsExtensionField(&node->hostname, value);
}

static void updateAnnouncedHumanNodename(clusterNode *node, char *value) {
    updateSdsExtensionField(&node->human_nodename, value);
}

static void updateAnnouncedClientIpV4(clusterNode *node, char *value) {
    updateSdsExtensionField(&node->announce_client_ipv4, value);
}

static void updateAnnouncedClientIpV6(clusterNode *node, char *value) {
    updateSdsExtensionField(&node->announce_client_ipv6, value);
}

static void updateShardId(clusterNode *node, const char *shard_id) {
    if (shard_id && memcmp(node->shard_id, shard_id, CLUSTER_NAMELEN) != 0) {
        clusterRemoveNodeFromShard(node);
        memcpy(node->shard_id, shard_id, CLUSTER_NAMELEN);
        clusterAddNodeToShard(shard_id, node);
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
    }
    if (shard_id && myself != node && myself->replicaof == node) {
        if (memcmp(myself->shard_id, shard_id, CLUSTER_NAMELEN) != 0) {
            /* shard-id can diverge right after a rolling upgrade
             * from pre-7.2 releases */
            clusterRemoveNodeFromShard(myself);
            memcpy(myself->shard_id, shard_id, CLUSTER_NAMELEN);
            clusterAddNodeToShard(shard_id, myself);
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_FSYNC_CONFIG);
        }
    }
}

static inline int areInSameShard(clusterNode *node1, clusterNode *node2) {
    return memcmp(node1->shard_id, node2->shard_id, CLUSTER_NAMELEN) == 0;
}

static inline uint64_t nodeEpoch(clusterNode *n) {
    return n->replicaof ? n->replicaof->configEpoch : n->configEpoch;
}

/* Update my hostname based on server configuration values */
void clusterUpdateMyselfHostname(void) {
    if (!myself) return;
    updateAnnouncedHostname(myself, server.cluster_announce_hostname);
}

void clusterUpdateMyselfHumanNodename(void) {
    if (!myself) return;
    updateAnnouncedHumanNodename(myself, server.cluster_announce_human_nodename);
}

void clusterUpdateMyselfClientIpV4(void) {
    if (!myself) return;
    updateAnnouncedClientIpV4(myself, server.cluster_announce_client_ipv4);
}

void clusterUpdateMyselfClientIpV6(void) {
    if (!myself) return;
    updateAnnouncedClientIpV6(myself, server.cluster_announce_client_ipv6);
}

void clusterInit(void) {
    int saveconf = 0;

    server.cluster = zmalloc(sizeof(struct clusterState));
    server.cluster->myself = NULL;
    server.cluster->currentEpoch = 0;
    server.cluster->state = CLUSTER_FAIL;
    server.cluster->size = 0;
    server.cluster->todo_before_sleep = 0;
    server.cluster->nodes = dictCreate(&clusterNodesDictType);
    server.cluster->shards = dictCreate(&clusterSdsToListType);
    server.cluster->nodes_black_list = dictCreate(&clusterNodesBlackListDictType);
    server.cluster->failover_auth_time = 0;
    server.cluster->failover_auth_count = 0;
    server.cluster->failover_auth_rank = 0;
    server.cluster->failover_auth_epoch = 0;
    server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
    server.cluster->lastVoteEpoch = 0;

    /* Initialize stats */
    for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
        server.cluster->stats_bus_messages_sent[i] = 0;
        server.cluster->stats_bus_messages_received[i] = 0;
    }
    server.cluster->stats_pfail_nodes = 0;
    server.cluster->stat_cluster_links_buffer_limit_exceeded = 0;

    memset(server.cluster->slots, 0, sizeof(server.cluster->slots));
    clusterCloseAllSlots();

    memset(server.cluster->owner_not_claiming_slot, 0, sizeof(server.cluster->owner_not_claiming_slot));

    /* Lock the cluster config file to make sure every node uses
     * its own nodes.conf. */
    server.cluster_config_file_lock_fd = -1;
    if (clusterLockConfig(server.cluster_configfile) == C_ERR) exit(1);

    /* Load or create a new nodes configuration. */
    if (clusterLoadConfig(server.cluster_configfile) == C_ERR) {
        /* No configuration found. We will just use the random name provided
         * by the createClusterNode() function. */
        myself = server.cluster->myself = createClusterNode(NULL, CLUSTER_NODE_MYSELF | CLUSTER_NODE_PRIMARY);
        serverLog(LL_NOTICE, "No cluster configuration found, I'm %.40s", myself->name);
        clusterAddNode(myself);
        clusterAddNodeToShard(myself->shard_id, myself);
        saveconf = 1;
    }
    if (saveconf) clusterSaveConfigOrDie(1);

    /* Port sanity check II
     * The other handshake port check is triggered too late to stop
     * us from trying to use a too-high cluster port number. */
    int port = defaultClientPort();
    if (!server.cluster_port && port > (65535 - CLUSTER_PORT_INCR)) {
        serverLog(LL_WARNING,
                  "%s port number too high. "
                  "Cluster communication port is 10,000 port "
                  "numbers higher than your %s port. "
                  "Your %s port number must be 55535 or less.",
                  SERVER_TITLE, SERVER_TITLE, SERVER_TITLE);
        exit(1);
    }
    if (!server.bindaddr_count) {
        serverLog(LL_WARNING, "No bind address is configured, but it is required for the Cluster bus.");
        exit(1);
    }

    /* Register our own rdb aux fields */
    serverAssert(rdbRegisterAuxField("cluster-slot-states", clusterEncodeOpenSlotsAuxField,
                                     clusterDecodeOpenSlotsAuxField) == C_OK);

    /* Set myself->port/cport/pport to my listening ports, we'll just need to
     * discover the IP address via MEET messages. */
    deriveAnnouncedPorts(&myself->tcp_port, &myself->tls_port, &myself->cport);

    server.cluster->mf_end = 0;
    server.cluster->mf_replica = NULL;
    for (int conn_type = 0; conn_type < CACHE_CONN_TYPE_MAX; conn_type++) {
        server.cached_cluster_slot_info[conn_type] = NULL;
    }
    resetManualFailover();
    clusterUpdateMyselfFlags();
    clusterUpdateMyselfIp();
    clusterUpdateMyselfClientIpV4();
    clusterUpdateMyselfClientIpV6();
    clusterUpdateMyselfHostname();
    clusterUpdateMyselfHumanNodename();
    resetClusterStats();
}

void clusterInitLast(void) {
    if (connectionIndexByType(connTypeOfCluster()->get_type(NULL)) < 0) {
        serverLog(LL_WARNING, "Missing connection type %s, but it is required for the Cluster bus.",
                  connTypeOfCluster()->get_type(NULL));
        exit(1);
    }

    int port = defaultClientPort();
    connListener *listener = &server.clistener;
    listener->count = 0;
    listener->bindaddr = server.bindaddr;
    listener->bindaddr_count = server.bindaddr_count;
    listener->port = server.cluster_port ? server.cluster_port : port + CLUSTER_PORT_INCR;
    listener->ct = connTypeOfCluster();
    if (connListen(listener) == C_ERR) {
        /* Note: the following log text is matched by the test suite. */
        serverLog(LL_WARNING, "Failed listening on port %u (cluster), aborting.", listener->port);
        exit(1);
    }

    if (createSocketAcceptHandler(&server.clistener, clusterAcceptHandler) != C_OK) {
        serverPanic("Unrecoverable error creating Cluster socket accept handler.");
    }
}

/* Called when a cluster node receives SHUTDOWN. */
void clusterHandleServerShutdown(void) {
    /* The error logs have been logged in the save function if the save fails. */
    serverLog(LL_NOTICE, "Saving the cluster configuration file before exiting.");
    clusterSaveConfig(1);

#if !defined(__sun)
    /* Unlock the cluster config file before shutdown, see clusterLockConfig.
     *
     * This is needed if you shutdown a very large server process, it will take
     * a while for the OS to release resources and unlock the cluster configuration
     * file. Therefore, if we immediately try to restart the server process, it
     * may not be able to acquire the lock on the cluster configuration file and
     * fail to start. We explicitly releases the lock on the cluster configuration
     * file on shutdown, rather than relying on the OS to release the lock, which
     * is a cleaner and safer way to release acquired resources. */
    if (server.cluster_config_file_lock_fd != -1) {
        flock(server.cluster_config_file_lock_fd, LOCK_UN | LOCK_NB);
    }
#endif /* __sun */
}

/* Reset a node performing a soft or hard reset:
 *
 * 1) All other nodes are forgotten.
 * 2) All the assigned / open slots are released.
 * 3) If the node is a replica, it turns into a primary.
 * 4) Only for hard reset: a new Node ID is generated.
 * 5) Only for hard reset: currentEpoch and configEpoch are set to 0.
 * 6) The new configuration is saved and the cluster state updated.
 * 7) If the node was a replica, the whole data set is flushed away. */
void clusterReset(int hard) {
    dictIterator *di;
    dictEntry *de;
    int j;

    /* Turn into primary. */
    if (nodeIsReplica(myself)) {
        clusterSetNodeAsPrimary(myself);
        replicationUnsetPrimary();
        emptyData(-1, EMPTYDB_NO_FLAGS, NULL);
    }

    /* Close slots, reset manual failover state. */
    clusterCloseAllSlots();
    resetManualFailover();

    /* Unassign all the slots. */
    for (j = 0; j < CLUSTER_SLOTS; j++) clusterDelSlot(j);

    /* Recreate shards dict */
    dictEmpty(server.cluster->shards, NULL);

    /* Forget all the nodes, but myself. */
    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node == myself) continue;
        clusterDelNode(node);
    }
    dictReleaseIterator(di);

    /* Empty the nodes blacklist. */
    dictEmpty(server.cluster->nodes_black_list, NULL);

    /* Hard reset only: set epochs to 0, change node ID. */
    if (hard) {
        sds oldname;

        server.cluster->currentEpoch = 0;
        server.cluster->lastVoteEpoch = 0;
        myself->configEpoch = 0;
        serverLog(LL_NOTICE, "configEpoch set to 0 via CLUSTER RESET HARD");

        /* To change the Node ID we need to remove the old name from the
         * nodes table, change the ID, and re-add back with new name. */
        oldname = sdsnewlen(myself->name, CLUSTER_NAMELEN);
        dictDelete(server.cluster->nodes, oldname);
        sdsfree(oldname);
        getRandomHexChars(myself->name, CLUSTER_NAMELEN);
        getRandomHexChars(myself->shard_id, CLUSTER_NAMELEN);
        clusterAddNode(myself);
        serverLog(LL_NOTICE, "Node hard reset, now I'm %.40s", myself->name);
    }

    /* Re-populate shards */
    clusterAddNodeToShard(myself->shard_id, myself);

    /* Make sure to persist the new config and update the state. */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_FSYNC_CONFIG);
}

/* -----------------------------------------------------------------------------
 * CLUSTER communication link
 * -------------------------------------------------------------------------- */
clusterMsgSendBlock *createClusterMsgSendBlock(int type, uint32_t msglen) {
    uint32_t blocklen = msglen + offsetof(clusterMsgSendBlock, msg);
    clusterMsgSendBlock *msgblock = zcalloc(blocklen);
    msgblock->refcount = 1;
    msgblock->totlen = blocklen;
    server.stat_cluster_links_memory += blocklen;
    if (IS_LIGHT_MESSAGE(type)) {
        clusterBuildMessageHdrLight(&msgblock->msg_light, type, msglen);
    } else {
        clusterBuildMessageHdr(&msgblock->msg, type, msglen);
    }
    return msgblock;
}

static void clusterMsgSendBlockDecrRefCount(void *node) {
    clusterMsgSendBlock *msgblock = (clusterMsgSendBlock *)node;
    msgblock->refcount--;
    serverAssert(msgblock->refcount >= 0);
    if (msgblock->refcount == 0) {
        server.stat_cluster_links_memory -= msgblock->totlen;
        zfree(msgblock);
    }
}

clusterLink *createClusterLink(clusterNode *node) {
    clusterLink *link = zmalloc(sizeof(*link));
    link->ctime = mstime();
    link->send_msg_queue = listCreate();
    listSetFreeMethod(link->send_msg_queue, clusterMsgSendBlockDecrRefCount);
    link->head_msg_send_offset = 0;
    link->send_msg_queue_mem = sizeof(list);
    link->rcvbuf = zmalloc(link->rcvbuf_alloc = RCVBUF_INIT_LEN);
    link->rcvbuf_len = 0;
    server.stat_cluster_links_memory += link->rcvbuf_alloc + link->send_msg_queue_mem;
    link->conn = NULL;
    link->node = node;
    /* Related node can only possibly be known at link creation time if this is an outbound link */
    link->inbound = (node == NULL);
    if (!link->inbound) {
        node->link = link;
    }
    return link;
}

/* Free a cluster link, but does not free the associated node of course.
 * This function will just make sure that the original node associated
 * with this link will have the 'link' field set to NULL. */
void freeClusterLink(clusterLink *link) {
    serverAssert(link != NULL);
    if (link->conn) {
        connClose(link->conn);
        link->conn = NULL;
    }
    server.stat_cluster_links_memory -= sizeof(list) + listLength(link->send_msg_queue) * sizeof(listNode);
    listRelease(link->send_msg_queue);
    server.stat_cluster_links_memory -= link->rcvbuf_alloc;
    zfree(link->rcvbuf);
    if (link->node) {
        if (link->node->link == link) {
            serverAssert(!link->inbound);
            link->node->link = NULL;
        } else if (link->node->inbound_link == link) {
            serverAssert(link->inbound);
            link->node->inbound_link = NULL;
        }
    }
    zfree(link);
}

void setClusterNodeToInboundClusterLink(clusterNode *node, clusterLink *link) {
    serverAssert(!link->node);
    serverAssert(link->inbound);
    if (node->inbound_link) {
        /* A peer may disconnect and then reconnect with us, and it's not guaranteed that
         * we would always process the disconnection of the existing inbound link before
         * accepting a new existing inbound link. Therefore, it's possible to have more than
         * one inbound link from the same node at the same time. Our cleanup logic assumes
         * a one to one relationship between nodes and inbound links, so we need to kill
         * one of the links. The existing link is more likely the outdated one, but it's
         * possible the other node may need to open another link. */
        serverLog(LL_DEBUG, "Replacing inbound link fd %d from node %.40s with fd %d", node->inbound_link->conn->fd,
                  node->name, link->conn->fd);
        freeClusterLink(node->inbound_link);
    }
    serverAssert(!node->inbound_link);
    node->inbound_link = link;
    link->node = node;
}

static void clusterConnAcceptHandler(connection *conn) {
    clusterLink *link;

    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_VERBOSE, "Error accepting cluster node connection: %s", connGetLastError(conn));
        connClose(conn);
        return;
    }

    /* Create a link object we use to handle the connection.
     * It gets passed to the readable handler when data is available.
     * Initially the link->node pointer is set to NULL as we don't know
     * which node is, but the right node is references once we know the
     * node identity. */
    link = createClusterLink(NULL);
    link->conn = conn;
    connSetPrivateData(conn, link);

    /* Register read handler */
    connSetReadHandler(conn, clusterReadHandler);
}

#define MAX_CLUSTER_ACCEPTS_PER_CALL 1000
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = MAX_CLUSTER_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    int require_auth = TLS_CLIENT_AUTH_YES;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    /* If the server is starting up, don't accept cluster connections:
     * UPDATE messages may interact with the database content. */
    if (server.primary_host == NULL && server.loading) return;

    while (max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK) serverLog(LL_VERBOSE, "Error accepting cluster node: %s", server.neterr);
            return;
        }

        connection *conn = connCreateAccepted(connTypeOfCluster(), cfd, &require_auth);

        /* Make sure connection is not in an error state */
        if (connGetState(conn) != CONN_STATE_ACCEPTING) {
            serverLog(LL_VERBOSE, "Error creating an accepting connection for cluster node: %s",
                      connGetLastError(conn));
            connClose(conn);
            return;
        }
        connEnableTcpNoDelay(conn);
        connKeepAlive(conn, server.cluster_node_timeout / 1000 * 2);

        /* Use non-blocking I/O for cluster messages. */
        serverLog(LL_VERBOSE, "Accepting cluster node connection from %s:%d", cip, cport);

        /* Accept the connection now.  connAccept() may call our handler directly
         * or schedule it for later depending on connection implementation.
         */
        if (connAccept(conn, clusterConnAcceptHandler) == C_ERR) {
            if (connGetState(conn) == CONN_STATE_ERROR)
                serverLog(LL_VERBOSE, "Error accepting cluster node connection: %s", connGetLastError(conn));
            connClose(conn);
            return;
        }
    }
}

/* Return the approximated number of sockets we are using in order to
 * take the cluster bus connections. */
unsigned long getClusterConnectionsCount(void) {
    /* We decrement the number of nodes by one, since there is the
     * "myself" node too in the list. Each node uses two file descriptors,
     * one incoming and one outgoing, thus the multiplication by 2. */
    return server.cluster_enabled ? ((dictSize(server.cluster->nodes) - 1) * 2) : 0;
}

/* -----------------------------------------------------------------------------
 * CLUSTER node API
 * -------------------------------------------------------------------------- */

/* Create a new cluster node, with the specified flags.
 * If "nodename" is NULL this is considered a first handshake and a random
 * node name is assigned to this node (it will be fixed later when we'll
 * receive the first pong).
 *
 * The node is created and returned to the user, but it is not automatically
 * added to the nodes hash table. */
clusterNode *createClusterNode(char *nodename, int flags) {
    clusterNode *node = zmalloc(sizeof(*node));

    if (nodename)
        memcpy(node->name, nodename, CLUSTER_NAMELEN);
    else
        getRandomHexChars(node->name, CLUSTER_NAMELEN);
    getRandomHexChars(node->shard_id, CLUSTER_NAMELEN);
    node->ctime = mstime();
    node->configEpoch = 0;
    node->flags = flags;
    memset(node->slots, 0, sizeof(node->slots));
    node->slot_info_pairs = NULL;
    node->slot_info_pairs_count = 0;
    node->numslots = 0;
    node->num_replicas = 0;
    node->replicas = NULL;
    node->replicaof = NULL;
    node->last_in_ping_gossip = 0;
    node->ping_sent = node->pong_received = 0;
    node->data_received = 0;
    node->fail_time = 0;
    node->link = NULL;
    node->inbound_link = NULL;
    memset(node->ip, 0, sizeof(node->ip));
    node->announce_client_ipv4 = sdsempty();
    node->announce_client_ipv6 = sdsempty();
    node->hostname = sdsempty();
    node->human_nodename = sdsempty();
    node->tcp_port = 0;
    node->cport = 0;
    node->tls_port = 0;
    node->fail_reports = listCreate();
    node->voted_time = 0;
    node->orphaned_time = 0;
    node->repl_offset_time = 0;
    node->repl_offset = 0;
    listSetFreeMethod(node->fail_reports, zfree);
    node->is_node_healthy = 0;
    return node;
}

/* This function is called every time we get a failure report from a node.
 * The side effect is to populate the fail_reports list (or to update
 * the timestamp of an existing report).
 *
 * 'failing' is the node that is in failure state according to the
 * 'sender' node.
 *
 * The function returns 0 if it just updates a timestamp of an existing
 * failure report from the same sender. 1 is returned if a new failure
 * report is created. */
int clusterNodeAddFailureReport(clusterNode *failing, clusterNode *sender) {
    list *l = failing->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* If a failure report from the same sender already exists, just update
     * the timestamp. */
    listRewind(l, &li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (fr->node == sender) {
            fr->time = mstime();
            return 0;
        }
    }

    /* Otherwise create a new report. */
    fr = zmalloc(sizeof(*fr));
    fr->node = sender;
    fr->time = mstime();
    listAddNodeTail(l, fr);
    return 1;
}

/* Remove failure reports that are too old, where too old means reasonably
 * older than the global node timeout. Note that anyway for a node to be
 * flagged as FAIL we need to have a local PFAIL state that is at least
 * older than the global node timeout, so we don't just trust the number
 * of failure reports from other nodes. */
void clusterNodeCleanupFailureReports(clusterNode *node) {
    list *l = node->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;
    mstime_t maxtime = server.cluster_node_timeout * CLUSTER_FAIL_REPORT_VALIDITY_MULT;
    mstime_t now = mstime();

    listRewind(l, &li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (now - fr->time > maxtime) listDelNode(l, ln);
    }
}

/* Remove the failing report for 'node' if it was previously considered
 * failing by 'sender'. This function is called when a node informs us via
 * gossip that a node is OK from its point of view (no FAIL or PFAIL flags).
 *
 * Note that this function is called relatively often as it gets called even
 * when there are no nodes failing, and is O(N), however when the cluster is
 * fine the failure reports list is empty so the function runs in constant
 * time.
 *
 * The function returns 1 if the failure report was found and removed.
 * Otherwise 0 is returned. */
int clusterNodeDelFailureReport(clusterNode *node, clusterNode *sender) {
    list *l = node->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* Search for a failure report from this sender. */
    listRewind(l, &li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (fr->node == sender) break;
    }
    if (!ln) return 0; /* No failure report from this sender. */

    /* Remove the failure report. */
    listDelNode(l, ln);
    clusterNodeCleanupFailureReports(node);
    return 1;
}

/* Return the number of external nodes that believe 'node' is failing,
 * not including this node, that may have a PFAIL or FAIL state for this
 * node as well. */
int clusterNodeFailureReportsCount(clusterNode *node) {
    clusterNodeCleanupFailureReports(node);
    return listLength(node->fail_reports);
}

static int clusterNodeNameComparator(const void *node1, const void *node2) {
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

/* Low level cleanup of the node structure. Only called by clusterDelNode(). */
void freeClusterNode(clusterNode *n) {
    sds nodename;
    int j;

    /* If the node has associated replicas, we have to set
     * all the replicas->replicaof fields to NULL (unknown). */
    for (j = 0; j < n->num_replicas; j++) n->replicas[j]->replicaof = NULL;

    /* Remove this node from the list of replicas of its primary. */
    if (nodeIsReplica(n) && n->replicaof) clusterNodeRemoveReplica(n->replicaof, n);

    /* Unlink from the set of nodes. */
    nodename = sdsnewlen(n->name, CLUSTER_NAMELEN);
    serverAssert(dictDelete(server.cluster->nodes, nodename) == DICT_OK);
    sdsfree(nodename);
    sdsfree(n->hostname);
    sdsfree(n->human_nodename);
    sdsfree(n->announce_client_ipv4);
    sdsfree(n->announce_client_ipv6);

    /* Release links and associated data structures. */
    if (n->link) freeClusterLink(n->link);
    if (n->inbound_link) freeClusterLink(n->inbound_link);
    listRelease(n->fail_reports);
    zfree(n->replicas);
    zfree(n);
}

/* Add a node to the nodes hash table */
void clusterAddNode(clusterNode *node) {
    int retval;

    retval = dictAdd(server.cluster->nodes, sdsnewlen(node->name, CLUSTER_NAMELEN), node);
    serverAssert(retval == DICT_OK);
}

/* Remove a node from the cluster. The function performs the high level
 * cleanup, calling freeClusterNode() for the low level cleanup.
 * Here we do the following:
 *
 * 1) Mark all the slots handled by it as unassigned.
 * 2) Remove all the failure reports sent by this node and referenced by
 *    other nodes.
 * 3) Remove the node from the owning shard
 * 4) Free the node with freeClusterNode() that will in turn remove it
 *    from the hash table and from the list of replicas of its primary, if
 *    it is a replica node.
 */
void clusterDelNode(clusterNode *delnode) {
    int j;
    dictIterator *di;
    dictEntry *de;

    /* 1) Mark slots as unassigned. */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (server.cluster->importing_slots_from[j] == delnode) server.cluster->importing_slots_from[j] = NULL;
        if (server.cluster->migrating_slots_to[j] == delnode) server.cluster->migrating_slots_to[j] = NULL;
        if (server.cluster->slots[j] == delnode) clusterDelSlot(j);
    }

    /* 2) Remove failure reports. */
    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node == delnode) continue;
        clusterNodeDelFailureReport(node, delnode);
    }
    dictReleaseIterator(di);

    /* 3) Remove the node from the owning shard */
    clusterRemoveNodeFromShard(delnode);

    /* 4) Free the node, unlinking it from the cluster. */
    freeClusterNode(delnode);
}

/* Node lookup by name */
clusterNode *clusterLookupNode(const char *name, int length) {
    if (verifyClusterNodeId(name, length) != C_OK) return NULL;
    sds s = sdsnewlen(name, length);
    dictEntry *de = dictFind(server.cluster->nodes, s);
    sdsfree(s);
    if (de == NULL) return NULL;
    return dictGetVal(de);
}

/* Get all the nodes in my shard.
 * Note that the list returned is not computed on the fly
 * via replicaof; rather, it is maintained permanently to
 * track the shard membership and its life cycle is tied
 * to this process. Therefore, the caller must not
 * release the list. */
list *clusterGetNodesInMyShard(clusterNode *node) {
    sds s = sdsnewlen(node->shard_id, CLUSTER_NAMELEN);
    dictEntry *de = dictFind(server.cluster->shards, s);
    sdsfree(s);
    return (de != NULL) ? dictGetVal(de) : NULL;
}

/* This is only used after the handshake. When we connect a given IP/PORT
 * as a result of CLUSTER MEET we don't have the node name yet, so we
 * pick a random one, and will fix it when we receive the PONG request using
 * this function. */
void clusterRenameNode(clusterNode *node, char *newname) {
    int retval;
    sds s = sdsnewlen(node->name, CLUSTER_NAMELEN);

    serverLog(LL_DEBUG, "Renaming node %.40s (%s) into %.40s", node->name, node->human_nodename, newname);
    retval = dictDelete(server.cluster->nodes, s);
    sdsfree(s);
    serverAssert(retval == DICT_OK);
    memcpy(node->name, newname, CLUSTER_NAMELEN);
    clusterAddNode(node);
    clusterAddNodeToShard(node->shard_id, node);
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
 * CLUSTER config epoch handling
 * -------------------------------------------------------------------------- */

/* Return the greatest configEpoch found in the cluster, or the current
 * epoch if greater than any node configEpoch. */
uint64_t clusterGetMaxEpoch(void) {
    uint64_t max = 0;
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->configEpoch > max) max = node->configEpoch;
    }
    dictReleaseIterator(di);
    if (max < server.cluster->currentEpoch) max = server.cluster->currentEpoch;
    return max;
}

/* If this node epoch is zero or is not already the greatest across the
 * cluster (from the POV of the local configuration), this function will:
 *
 * 1) Generate a new config epoch, incrementing the current epoch.
 * 2) Assign the new epoch to this node, WITHOUT any consensus.
 * 3) Persist the configuration on disk before sending packets with the
 *    new configuration.
 *
 * If the new config epoch is generated and assigned, C_OK is returned,
 * otherwise C_ERR is returned (since the node has already the greatest
 * configuration around) and no operation is performed.
 *
 * Important note: this function violates the principle that config epochs
 * should be generated with consensus and should be unique across the cluster.
 * However the cluster uses this auto-generated new config epochs in two
 * cases:
 *
 * 1) When slots are closed after importing. Otherwise resharding would be
 *    too expensive.
 * 2) When CLUSTER FAILOVER is called with options that force a replica to
 *    failover its primary even if there is not primary majority able to
 *    create a new configuration epoch.
 *
 * The cluster will not explode using this function, even in the case of
 * a collision between this node and another node, generating the same
 * configuration epoch unilaterally, because the config epoch conflict
 * resolution algorithm will eventually move colliding nodes to different
 * config epochs. However using this function may violate the "last failover
 * wins" rule, so should only be used with care. */
int clusterBumpConfigEpochWithoutConsensus(void) {
    uint64_t maxEpoch = clusterGetMaxEpoch();

    if (myself->configEpoch == 0 || myself->configEpoch != maxEpoch) {
        server.cluster->currentEpoch++;
        myself->configEpoch = server.cluster->currentEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_FSYNC_CONFIG);
        serverLog(LL_NOTICE, "New configEpoch set to %llu", (unsigned long long)myself->configEpoch);
        return C_OK;
    } else {
        return C_ERR;
    }
}

/* This function is called when this node is a primary, and we receive from
 * another primary a configuration epoch that is equal to our configuration
 * epoch.
 *
 * BACKGROUND
 *
 * It is not possible that different replicas get the same config
 * epoch during a failover election, because the replicas need to get voted
 * by a majority. However when we perform a manual resharding of the cluster
 * the node will assign a configuration epoch to itself without to ask
 * for agreement. Usually resharding happens when the cluster is working well
 * and is supervised by the sysadmin, however it is possible for a failover
 * to happen exactly while the node we are resharding a slot to assigns itself
 * a new configuration epoch, but before it is able to propagate it.
 *
 * So technically it is possible in this condition that two nodes end with
 * the same configuration epoch.
 *
 * Another possibility is that there are bugs in the implementation causing
 * this to happen.
 *
 * Moreover when a new cluster is created, all the nodes start with the same
 * configEpoch. This collision resolution code allows nodes to automatically
 * end with a different configEpoch at startup automatically.
 *
 * In all the cases, we want a mechanism that resolves this issue automatically
 * as a safeguard. The same configuration epoch for primaries serving different
 * set of slots is not harmful, but it is if the nodes end serving the same
 * slots for some reason (manual errors or software bugs) without a proper
 * failover procedure.
 *
 * In general we want a system that eventually always ends with different
 * primaries having different configuration epochs whatever happened, since
 * nothing is worse than a split-brain condition in a distributed system.
 *
 * BEHAVIOR
 *
 * When this function gets called, what happens is that if this node
 * has the lexicographically smaller Node ID compared to the other node
 * with the conflicting epoch (the 'sender' node), it will assign itself
 * the greatest configuration epoch currently detected among nodes plus 1.
 *
 * This means that even if there are multiple nodes colliding, the node
 * with the greatest Node ID never moves forward, so eventually all the nodes
 * end with a different configuration epoch.
 */
void clusterHandleConfigEpochCollision(clusterNode *sender) {
    /* Prerequisites: nodes have the same configEpoch and are both primaries. */
    if (sender->configEpoch != myself->configEpoch || !clusterNodeIsPrimary(sender) || !clusterNodeIsPrimary(myself))
        return;
    /* Don't act if the colliding node has a smaller Node ID. */
    if (memcmp(sender->name, myself->name, CLUSTER_NAMELEN) <= 0) return;
    /* Get the next ID available at the best of this node knowledge. */
    server.cluster->currentEpoch++;
    myself->configEpoch = server.cluster->currentEpoch;
    clusterSaveConfigOrDie(1);
    serverLog(LL_NOTICE, "configEpoch collision with node %.40s (%s). configEpoch set to %llu", sender->name,
              sender->human_nodename, (unsigned long long)myself->configEpoch);
}

/* -----------------------------------------------------------------------------
 * CLUSTER nodes blacklist
 *
 * The nodes blacklist is just a way to ensure that a given node with a given
 * Node ID is not re-added before some time elapsed (this time is specified
 * in seconds by the configurable cluster-blacklist-ttl).
 *
 * This is useful when we want to remove a node from the cluster completely:
 * when CLUSTER FORGET is called, it also puts the node into the blacklist so
 * that even if we receive gossip messages from other nodes that still remember
 * about the node we want to remove, we don't re-add it before some time.
 *
 * The default blacklist ttl is 1 minute which means
 * that valkey-cli has 60 seconds to send CLUSTER FORGET messages to nodes
 * in the cluster without dealing with the problem of other nodes re-adding
 * back the node to nodes we already sent the FORGET command to.
 *
 * The data structure used is a hash table with an sds string representing
 * the node ID as key, and the time when it is ok to re-add the node as
 * value.
 * -------------------------------------------------------------------------- */

/* Before of the addNode() or Exists() operations we always remove expired
 * entries from the black list. This is an O(N) operation but it is not a
 * problem since add / exists operations are called very infrequently and
 * the hash table is supposed to contain very little elements at max.
 * However without the cleanup during long uptime and with some automated
 * node add/removal procedures, entries could accumulate. */
void clusterBlacklistCleanup(void) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes_black_list);
    while ((de = dictNext(di)) != NULL) {
        int64_t expire = dictGetUnsignedIntegerVal(de);

        if (expire < server.unixtime) dictDelete(server.cluster->nodes_black_list, dictGetKey(de));
    }
    dictReleaseIterator(di);
}

/* Cleanup the blacklist and add a new node ID to the black list. */
void clusterBlacklistAddNode(clusterNode *node) {
    dictEntry *de;
    sds id = sdsnewlen(node->name, CLUSTER_NAMELEN);

    clusterBlacklistCleanup();
    if (dictAdd(server.cluster->nodes_black_list, id, NULL) == DICT_OK) {
        /* If the key was added, duplicate the sds string representation of
         * the key for the next lookup. We'll free it at the end. */
        id = sdsdup(id);
    }
    de = dictFind(server.cluster->nodes_black_list, id);
    dictSetUnsignedIntegerVal(de, time(NULL) + server.cluster_blacklist_ttl);
    sdsfree(id);
}

/* Return non-zero if the specified node ID exists in the blacklist.
 * You don't need to pass an sds string here, any pointer to 40 bytes
 * will work. */
int clusterBlacklistExists(char *nodeid) {
    sds id = sdsnewlen(nodeid, CLUSTER_NAMELEN);
    int retval;

    clusterBlacklistCleanup();
    retval = dictFind(server.cluster->nodes_black_list, id) != NULL;
    sdsfree(id);
    return retval;
}

/* -----------------------------------------------------------------------------
 * CLUSTER messages exchange - PING/PONG and gossip
 * -------------------------------------------------------------------------- */

/* This function checks if a given node should be marked as FAIL.
 * It happens if the following conditions are met:
 *
 * 1) We received enough failure reports from other primary nodes via gossip.
 *    Enough means that the majority of the primaries signaled the node is
 *    down recently.
 * 2) We believe this node is in PFAIL state.
 *
 * If a failure is detected we also inform the whole cluster about this
 * event trying to force every other node to set the FAIL flag for the node.
 *
 * Note that the form of agreement used here is weak, as we collect the majority
 * of primaries state during some time, and even if we force agreement by
 * propagating the FAIL message, because of partitions we may not reach every
 * node. However:
 *
 * 1) Either we reach the majority and eventually the FAIL state will propagate
 *    to all the cluster.
 * 2) Or there is no majority so no replica promotion will be authorized and the
 *    FAIL flag will be cleared after some time.
 */
void markNodeAsFailingIfNeeded(clusterNode *node) {
    int failures;
    int needed_quorum = (server.cluster->size / 2) + 1;

    if (!nodeTimedOut(node)) return; /* We can reach it. */
    if (nodeFailed(node)) return;    /* Already FAILing. */

    failures = clusterNodeFailureReportsCount(node);
    /* Also count myself as a voter if I'm a voting primary. */
    if (clusterNodeIsVotingPrimary(myself)) failures++;
    if (failures < needed_quorum) return; /* No weak agreement from primaries. */

    serverLog(LL_NOTICE, "Marking node %.40s (%s) as failing (quorum reached).", node->name, node->human_nodename);

    /* Mark the node as failing. */
    node->flags &= ~CLUSTER_NODE_PFAIL;
    node->flags |= CLUSTER_NODE_FAIL;
    node->fail_time = mstime();

    /* Broadcast the failing node name to everybody, forcing all the other
     * reachable nodes to flag the node as FAIL.
     * We do that even if this node is a replica and not a primary: anyway
     * the failing state is triggered collecting failure reports from primaries,
     * so here the replica is only helping propagating this status. */
    clusterSendFail(node->name);
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
}

/* This function is called only if a node is marked as FAIL, but we are able
 * to reach it again. It checks if there are the conditions to undo the FAIL
 * state. */
void clearNodeFailureIfNeeded(clusterNode *node) {
    mstime_t now = mstime();

    serverAssert(nodeFailed(node));

    /* For replicas or primaries without slots, that is, nodes without voting
     * right, we always clear the FAIL flag if we can contact the node again. */
    if (!clusterNodeIsVotingPrimary(node)) {
        serverLog(LL_NOTICE, "Clear FAIL state for node %.40s (%s): %s is reachable again.", node->name,
                  node->human_nodename, nodeIsReplica(node) ? "replica" : "primary without slots");
        node->flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
    }

    /* If it is a primary and...
     * 1) The FAIL state is old enough.
     * 2) It is yet serving slots from our point of view (not failed over).
     * Apparently no one is going to fix these slots, clear the FAIL flag. */
    if (clusterNodeIsVotingPrimary(node) &&
        (now - node->fail_time) > (server.cluster_node_timeout * CLUSTER_FAIL_UNDO_TIME_MULT)) {
        serverLog(
            LL_NOTICE,
            "Clear FAIL state for node %.40s (%s): is reachable again and nobody is serving its slots after some time.",
            node->name, node->human_nodename);
        node->flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
    }
}

/* Return 1 if we already have a node in HANDSHAKE state matching the
 * specified ip address and port number. This function is used in order to
 * avoid adding a new handshake node for the same address multiple times. */
int clusterHandshakeInProgress(char *ip, int port, int cport) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (!nodeInHandshake(node)) continue;
        if (!strcasecmp(node->ip, ip) && getNodeDefaultClientPort(node) == port && node->cport == cport) break;
    }
    dictReleaseIterator(di);
    return de != NULL;
}

/* Start a handshake with the specified address if there is not one
 * already in progress. Returns non-zero if the handshake was actually
 * started. On error zero is returned and errno is set to one of the
 * following values:
 *
 * EAGAIN - There is already a handshake in progress for this address.
 * EINVAL - IP or port are not valid. */
int clusterStartHandshake(char *ip, int port, int cport) {
    clusterNode *n;
    char norm_ip[NET_IP_STR_LEN];
    struct sockaddr_storage sa;

    /* IP sanity check */
    if (inet_pton(AF_INET, ip, &(((struct sockaddr_in *)&sa)->sin_addr))) {
        sa.ss_family = AF_INET;
    } else if (inet_pton(AF_INET6, ip, &(((struct sockaddr_in6 *)&sa)->sin6_addr))) {
        sa.ss_family = AF_INET6;
    } else {
        errno = EINVAL;
        return 0;
    }

    /* Port sanity check */
    if (port <= 0 || port > 65535 || cport <= 0 || cport > 65535) {
        errno = EINVAL;
        return 0;
    }

    /* Set norm_ip as the normalized string representation of the node
     * IP address. */
    memset(norm_ip, 0, NET_IP_STR_LEN);
    if (sa.ss_family == AF_INET)
        inet_ntop(AF_INET, (void *)&(((struct sockaddr_in *)&sa)->sin_addr), norm_ip, NET_IP_STR_LEN);
    else
        inet_ntop(AF_INET6, (void *)&(((struct sockaddr_in6 *)&sa)->sin6_addr), norm_ip, NET_IP_STR_LEN);

    if (clusterHandshakeInProgress(norm_ip, port, cport)) {
        errno = EAGAIN;
        return 0;
    }

    /* Add the node with a random address (NULL as first argument to
     * createClusterNode()). Everything will be fixed during the
     * handshake. */
    n = createClusterNode(NULL, CLUSTER_NODE_HANDSHAKE | CLUSTER_NODE_MEET);
    memcpy(n->ip, norm_ip, sizeof(n->ip));
    if (server.tls_cluster) {
        n->tls_port = port;
    } else {
        n->tcp_port = port;
    }
    n->cport = cport;
    clusterAddNode(n);
    return 1;
}

static void getClientPortFromClusterMsg(clusterMsg *hdr, int *tls_port, int *tcp_port) {
    if (server.tls_cluster) {
        *tls_port = ntohs(hdr->port);
        *tcp_port = ntohs(hdr->pport);
    } else {
        *tls_port = ntohs(hdr->pport);
        *tcp_port = ntohs(hdr->port);
    }
}

static void getClientPortFromGossip(clusterMsgDataGossip *g, int *tls_port, int *tcp_port) {
    if (server.tls_cluster) {
        *tls_port = ntohs(g->port);
        *tcp_port = ntohs(g->pport);
    } else {
        *tls_port = ntohs(g->pport);
        *tcp_port = ntohs(g->port);
    }
}

/* Returns a string with the byte representation of the node ID (i.e. nodename)
 * along with 8 trailing bytes for debugging purposes. */
char *getCorruptedNodeIdByteString(clusterMsgDataGossip *gossip_msg) {
    const int num_bytes = CLUSTER_NAMELEN + 8;
    /* Allocate enough room for 4 chars per byte + null terminator */
    char *byte_string = (char *)zmalloc((num_bytes * 4) + 1);
    const char *name_ptr = gossip_msg->nodename;

    /* Ensure we won't print beyond the bounds of the message */
    serverAssert(name_ptr + num_bytes <= (char *)gossip_msg + sizeof(clusterMsgDataGossip));

    for (int i = 0; i < num_bytes; i++) {
        snprintf(byte_string + 4 * i, 5, "\\x%02hhX", name_ptr[i]);
    }
    return byte_string;
}

/* Returns the number of nodes in the gossip with invalid IDs. */
int verifyGossipSectionNodeIds(clusterMsgDataGossip *g, uint16_t count) {
    int invalid_ids = 0;
    for (int i = 0; i < count; i++) {
        const char *nodename = g[i].nodename;
        if (verifyClusterNodeId(nodename, CLUSTER_NAMELEN) != C_OK) {
            invalid_ids++;
            char *raw_node_id = getCorruptedNodeIdByteString(g);
            serverLog(LL_WARNING,
                      "Received gossip about a node with invalid ID %.40s. For debugging purposes, "
                      "the 48 bytes including the invalid ID and 8 trailing bytes are: %s",
                      nodename, raw_node_id);
            zfree(raw_node_id);
        }
    }
    return invalid_ids;
}

/* Process the gossip section of PING or PONG packets.
 * Note that this function assumes that the packet is already sanity-checked
 * by the caller, not in the content of the gossip section, but in the
 * length. */
void clusterProcessGossipSection(clusterMsg *hdr, clusterLink *link) {
    uint16_t count = ntohs(hdr->count);
    clusterMsgDataGossip *g = (clusterMsgDataGossip *)hdr->data.ping.gossip;
    clusterNode *sender = link->node ? link->node : clusterLookupNode(hdr->sender, CLUSTER_NAMELEN);

    /* Abort if the gossip contains invalid node IDs to avoid adding incorrect information to
     * the nodes dictionary. An invalid ID indicates memory corruption on the sender side. */
    int invalid_ids = verifyGossipSectionNodeIds(g, count);
    if (invalid_ids) {
        if (sender) {
            serverLog(LL_WARNING, "Node %.40s (%s) gossiped %d nodes with invalid IDs.", sender->name,
                      sender->human_nodename, invalid_ids);
        } else {
            serverLog(LL_WARNING, "Unknown node gossiped %d nodes with invalid IDs.", invalid_ids);
        }
        return;
    }

    while (count--) {
        uint16_t flags = ntohs(g->flags);
        clusterNode *node;
        sds ci;

        if (server.verbosity == LL_DEBUG) {
            ci = representClusterNodeFlags(sdsempty(), flags);
            serverLog(LL_DEBUG, "GOSSIP %.40s %s:%d@%d %s", g->nodename, g->ip, ntohs(g->port), ntohs(g->cport), ci);
            sdsfree(ci);
        }

        /* Convert port and pport into TCP port and TLS port. */
        int msg_tls_port, msg_tcp_port;
        getClientPortFromGossip(g, &msg_tls_port, &msg_tcp_port);

        /* Update our state accordingly to the gossip sections */
        node = clusterLookupNode(g->nodename, CLUSTER_NAMELEN);
        /* Ignore gossips about self. */
        if (node && node != myself) {
            /* We already know this node.
               Handle failure reports, only when the sender is a voting primary. */
            if (sender && clusterNodeIsVotingPrimary(sender)) {
                if (flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_PFAIL)) {
                    if (clusterNodeAddFailureReport(node, sender)) {
                        serverLog(LL_NOTICE, "Node %.40s (%s) reported node %.40s (%s) as not reachable.", sender->name,
                                  sender->human_nodename, node->name, node->human_nodename);
                    }
                    markNodeAsFailingIfNeeded(node);
                } else {
                    if (clusterNodeDelFailureReport(node, sender)) {
                        serverLog(LL_NOTICE, "Node %.40s (%s) reported node %.40s (%s) is back online.", sender->name,
                                  sender->human_nodename, node->name, node->human_nodename);
                    }
                }
            }

            /* If from our POV the node is up (no failure flags are set),
             * we have no pending ping for the node, nor we have failure
             * reports for this node, update the last pong time with the
             * one we see from the other nodes. */
            if (!(flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_PFAIL)) && node->ping_sent == 0 &&
                clusterNodeFailureReportsCount(node) == 0) {
                mstime_t pongtime = ntohl(g->pong_received);
                pongtime *= 1000; /* Convert back to milliseconds. */

                /* Replace the pong time with the received one only if
                 * it's greater than our view but is not in the future
                 * (with 500 milliseconds tolerance) from the POV of our
                 * clock. */
                if (pongtime <= (server.mstime + 500) && pongtime > node->pong_received) {
                    node->pong_received = pongtime;
                }
            }

            /* If we already know this node, but it is not reachable, and
             * we see a different address in the gossip section of a node that
             * can talk with this other node, update the address, disconnect
             * the old link if any, so that we'll attempt to connect with the
             * new address. */
            if (node->flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_PFAIL) && !(flags & CLUSTER_NODE_NOADDR) &&
                !(flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_PFAIL)) &&
                (strcasecmp(node->ip, g->ip) ||
                 node->tls_port != (server.tls_cluster ? ntohs(g->port) : ntohs(g->pport)) ||
                 node->tcp_port != (server.tls_cluster ? ntohs(g->pport) : ntohs(g->port)) ||
                 node->cport != ntohs(g->cport))) {
                if (node->link) freeClusterLink(node->link);
                memcpy(node->ip, g->ip, NET_IP_STR_LEN);
                node->tcp_port = msg_tcp_port;
                node->tls_port = msg_tls_port;
                node->cport = ntohs(g->cport);
                node->flags &= ~CLUSTER_NODE_NOADDR;

                serverLog(LL_NOTICE, "Address updated for node %.40s (%s), now %s:%d", node->name, node->human_nodename,
                          node->ip, getNodeDefaultClientPort(node));

                /* Check if this is our primary and we have to change the
                 * replication target as well.
                 *
                 * This is needed in case the check in nodeUpdateAddressIfNeeded
                 * failed due to a race condition. For example, if the replica just
                 * received a packet from another node that contains new address
                 * about the primary, we will update primary node address in here,
                 * when the replica receive the packet from the primary, the check
                 * in nodeUpdateAddressIfNeeded will fail since the address has been
                 * updated correctly, and we will not have the opportunity to call
                 * replicationSetPrimary and update the primary host. */
                if (nodeIsReplica(myself) && myself->replicaof == node)
                    replicationSetPrimary(node->ip, getNodeDefaultReplicationPort(node), 0);
            }
        } else if (!node) {
            /* If it's not in NOADDR state and we don't have it, we
             * add it to our trusted dict with exact nodeid and flag.
             * Note that we cannot simply start a handshake against
             * this IP/PORT pairs, since IP/PORT can be reused already,
             * otherwise we risk joining another cluster.
             *
             * Note that we require that the sender of this gossip message
             * is a well known node in our cluster, otherwise we risk
             * joining another cluster. */
            if (sender && !(flags & CLUSTER_NODE_NOADDR) && !clusterBlacklistExists(g->nodename)) {
                clusterNode *node;
                node = createClusterNode(g->nodename, flags);
                memcpy(node->ip, g->ip, NET_IP_STR_LEN);
                node->tcp_port = msg_tcp_port;
                node->tls_port = msg_tls_port;
                node->cport = ntohs(g->cport);
                clusterAddNode(node);
                clusterAddNodeToShard(node->shard_id, node);
            }
        }

        /* Next node */
        g++;
    }
}

/* IP -> string conversion. 'buf' is supposed to at least be 46 bytes.
 * If 'announced_ip' length is non-zero, it is used instead of extracting
 * the IP from the socket peer address. */
int nodeIp2String(char *buf, clusterLink *link, char *announced_ip) {
    if (announced_ip[0] != '\0') {
        memcpy(buf, announced_ip, NET_IP_STR_LEN);
        buf[NET_IP_STR_LEN - 1] = '\0'; /* We are not sure the input is sane. */
        return C_OK;
    } else {
        if (connAddrPeerName(link->conn, buf, NET_IP_STR_LEN, NULL) == -1) {
            serverLog(LL_NOTICE, "Error converting peer IP to string: %s",
                      link->conn ? connGetLastError(link->conn) : "no link");
            return C_ERR;
        }
        return C_OK;
    }
}

/* Update the node address to the IP address that can be extracted
 * from link->fd, or if hdr->myip is non empty, to the address the node
 * is announcing us. The port is taken from the packet header as well.
 *
 * If the address or port changed, disconnect the node link so that we'll
 * connect again to the new address.
 *
 * If the ip/port pair are already correct no operation is performed at
 * all.
 *
 * The function returns 0 if the node address is still the same,
 * otherwise 1 is returned. */
int nodeUpdateAddressIfNeeded(clusterNode *node, clusterLink *link, clusterMsg *hdr) {
    char ip[NET_IP_STR_LEN] = {0};
    int cport = ntohs(hdr->cport);
    int tcp_port, tls_port;
    getClientPortFromClusterMsg(hdr, &tls_port, &tcp_port);

    /* We don't proceed if the link is the same as the sender link, as this
     * function is designed to see if the node link is consistent with the
     * symmetric link that is used to receive PINGs from the node.
     *
     * As a side effect this function never frees the passed 'link', so
     * it is safe to call during packet processing. */
    if (link == node->link) return 0;

    /* If the peer IP is unavailable for some reasons like invalid fd or closed
     * link, just give up the update this time, and the update will be retried
     * in the next round of PINGs */
    if (nodeIp2String(ip, link, hdr->myip) == C_ERR) return 0;

    if (node->tcp_port == tcp_port && node->cport == cport && node->tls_port == tls_port && strcmp(ip, node->ip) == 0)
        return 0;

    /* IP / port is different, update it. */
    memcpy(node->ip, ip, sizeof(ip));
    node->tcp_port = tcp_port;
    node->tls_port = tls_port;
    node->cport = cport;
    if (node->link) freeClusterLink(node->link);
    node->flags &= ~CLUSTER_NODE_NOADDR;
    serverLog(LL_NOTICE, "Address updated for node %.40s (%s), now %s:%d", node->name, node->human_nodename, node->ip,
              getNodeDefaultClientPort(node));

    /* Check if this is our primary and we have to change the
     * replication target as well. */
    if (nodeIsReplica(myself) && myself->replicaof == node)
        replicationSetPrimary(node->ip, getNodeDefaultReplicationPort(node), 0);
    return 1;
}

/* Reconfigure the specified node 'n' as a primary. This function is called when
 * a node that we believed to be a replica is now acting as primary in order to
 * update the state of the node. */
void clusterSetNodeAsPrimary(clusterNode *n) {
    if (clusterNodeIsPrimary(n)) return;

    if (n->replicaof) {
        clusterNodeRemoveReplica(n->replicaof, n);
        if (n != myself) n->flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    n->flags &= ~CLUSTER_NODE_REPLICA;
    n->flags |= CLUSTER_NODE_PRIMARY;
    n->replicaof = NULL;

    /* Update config and state. */
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
}

/* This function is called when we receive a primary configuration via a
 * PING, PONG or UPDATE packet. What we receive is a node, a configEpoch of the
 * node, and the set of slots claimed under this configEpoch.
 *
 * What we do is to rebind the slots with newer configuration compared to our
 * local configuration, and if needed, we turn ourself into a replica of the
 * node (see the function comments for more info).
 *
 * The 'sender' is the node for which we received a configuration update.
 * Sometimes it is not actually the "Sender" of the information, like in the
 * case we receive the info via an UPDATE packet. */
void clusterUpdateSlotsConfigWith(clusterNode *sender, uint64_t senderConfigEpoch, unsigned char *slots) {
    int j;
    clusterNode *cur_primary = NULL, *new_primary = NULL;
    /* The dirty slots list is a list of slots for which we lose the ownership
     * while having still keys inside. This usually happens after a failover
     * or after a manual cluster reconfiguration operated by the admin.
     *
     * If the update message is not able to demote a primary to replica (in this
     * case we'll resync with the primary updating the whole key space), we
     * need to delete all the keys in the slots we lost ownership. */
    uint16_t dirty_slots[CLUSTER_SLOTS];
    int dirty_slots_count = 0;

    /* We should detect if sender is new primary of our shard.
     * We will know it if all our slots were migrated to sender, and sender
     * has no slots except ours */
    int sender_slots = 0;
    int migrated_our_slots = 0;

    /* Here we set cur_primary to this node or the node this node
     * replicates to if it's a replica. In the for loop we are
     * interested to check if slots are taken away from cur_primary. */
    cur_primary = clusterNodeIsPrimary(myself) ? myself : myself->replicaof;

    if (sender == myself) {
        serverLog(LL_NOTICE, "Discarding UPDATE message about myself.");
        return;
    }

    /* Sender and myself in the same shard? */
    int are_in_same_shard = areInSameShard(sender, myself);

    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (bitmapTestBit(slots, j)) {
            sender_slots++;

            /* The slot is already bound to the sender of this message. */
            if (server.cluster->slots[j] == sender) {
                bitmapClearBit(server.cluster->owner_not_claiming_slot, j);
                continue;
            }

            /* We rebind the slot to the new node claiming it if
             * the slot was unassigned or the new node claims it with a
             * greater configEpoch. */
            if (isSlotUnclaimed(j) || server.cluster->slots[j]->configEpoch < senderConfigEpoch) {
                if (!isSlotUnclaimed(j) && !areInSameShard(server.cluster->slots[j], sender)) {
                    serverLog(LL_NOTICE,
                              "Slot %d is migrated from node %.40s (%s) in shard %.40s"
                              " to node %.40s (%s) in shard %.40s.",
                              j, server.cluster->slots[j]->name, server.cluster->slots[j]->human_nodename,
                              server.cluster->slots[j]->shard_id, sender->name, sender->human_nodename,
                              sender->shard_id);
                }

                /* Was this slot mine, and still contains keys? Mark it as
                 * a dirty slot. */
                if (server.cluster->slots[j] == myself && countKeysInSlot(j) && sender != myself) {
                    dirty_slots[dirty_slots_count] = j;
                    dirty_slots_count++;
                }

                if (server.cluster->slots[j] == cur_primary) {
                    new_primary = sender;
                    migrated_our_slots++;
                }

                /* If the sender who claims this slot is not in the same shard,
                 * it must be a result of deliberate operator actions. Therefore,
                 * we should honor it and clear the outstanding migrating_slots_to
                 * state for the slot. Otherwise, we are looking at a failover within
                 * the same shard and we should retain the migrating_slots_to state
                 * for the slot in question */
                if (server.cluster->migrating_slots_to[j] != NULL) {
                    if (!are_in_same_shard) {
                        serverLog(LL_NOTICE, "Slot %d is no longer being migrated to node %.40s (%s) in shard %.40s.",
                                  j, server.cluster->migrating_slots_to[j]->name,
                                  server.cluster->migrating_slots_to[j]->human_nodename,
                                  server.cluster->migrating_slots_to[j]->shard_id);
                        server.cluster->migrating_slots_to[j] = NULL;
                    }
                }

                /* Handle the case where we are importing this slot and the ownership changes */
                if (server.cluster->importing_slots_from[j] != NULL &&
                    server.cluster->importing_slots_from[j] != sender) {
                    /* Update importing_slots_from to point to the sender, if it is in the
                     * same shard as the previous slot owner */
                    if (areInSameShard(sender, server.cluster->importing_slots_from[j])) {
                        serverLog(LL_NOTICE,
                                  "Failover occurred in migration source. Update importing "
                                  "source for slot %d to node %.40s (%s) in shard %.40s.",
                                  j, sender->name, sender->human_nodename, sender->shard_id);
                        server.cluster->importing_slots_from[j] = sender;
                    } else {
                        /* If the sender is from a different shard, it must be a result
                         * of deliberate operator actions. We should clear the importing
                         * state to conform to the operator's will. */
                        serverLog(LL_NOTICE, "Slot %d is no longer being imported from node %.40s (%s) in shard %.40s.",
                                  j, server.cluster->importing_slots_from[j]->name,
                                  server.cluster->importing_slots_from[j]->human_nodename,
                                  server.cluster->importing_slots_from[j]->shard_id);
                        server.cluster->importing_slots_from[j] = NULL;
                    }
                }

                clusterDelSlot(j);
                clusterAddSlot(sender, j);
                bitmapClearBit(server.cluster->owner_not_claiming_slot, j);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_FSYNC_CONFIG);
            }
        } else {
            if (server.cluster->slots[j] == sender) {
                /* The slot is currently bound to the sender but the sender is no longer
                 * claiming it. We don't want to unbind the slot yet as it can cause the cluster
                 * to move to FAIL state and also throw client error. Keeping the slot bound to
                 * the previous owner will cause a few client side redirects, but won't throw
                 * any errors. We will keep track of the uncertainty in ownership to avoid
                 * propagating misinformation about this slot's ownership using UPDATE
                 * messages. */
                bitmapSetBit(server.cluster->owner_not_claiming_slot, j);
            }

            /* If the sender doesn't claim the slot, check if we are migrating
             * any slot to its shard and if there is a primaryship change in
             * the shard. Update the migrating_slots_to state to point to the
             * sender if it has just taken over the primary role. */
            if (server.cluster->migrating_slots_to[j] != NULL && server.cluster->migrating_slots_to[j] != sender &&
                (server.cluster->migrating_slots_to[j]->configEpoch < senderConfigEpoch ||
                 nodeIsReplica(server.cluster->migrating_slots_to[j])) &&
                areInSameShard(server.cluster->migrating_slots_to[j], sender)) {
                serverLog(LL_NOTICE,
                          "Failover occurred in migration target."
                          " Slot %d is now being migrated to node %.40s (%s) in shard %.40s.",
                          j, sender->name, sender->human_nodename, sender->shard_id);
                server.cluster->migrating_slots_to[j] = sender;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_FSYNC_CONFIG);
            }

            /* If the sender is no longer the owner of the slot, and I am a primary
             * and I am still in the process of importing the slot from the sender,
             * there are two possibilities:
             *
             * 1. I could be a replica of the target primary and missed the slot
             *    finalization step on my primary due to my primary crashing during
             *    the slot migration process.
             * 2. I could be the original primary and missed the slot finalization
             *    step entirely.
             *
             * To ensure complete slot coverage in either case, the following steps
             * will be taken:
             *
             * 1. Remove the importing state for the specific slot.
             * 2. Finalize the slot's ownership, if I am not already the owner of
             *    the slot. */
            if (nodeIsPrimary(myself) && server.cluster->importing_slots_from[j] == sender) {
                serverLog(LL_NOTICE,
                          "Slot %d is no longer being imported from node %.40s (%s) in shard %.40s;"
                          " Clear my importing source for the slot.",
                          j, sender->name, sender->human_nodename, sender->shard_id);
                server.cluster->importing_slots_from[j] = NULL;
                /* Take over the slot ownership if I am not the owner yet*/
                if (server.cluster->slots[j] != myself) {
                    /* A primary reason why we are here is likely due to my primary crashing during the
                     * slot finalization process, leading me to become the new primary without
                     * inheriting the slot ownership, while the source shard continued and relinquished
                     * theslot to its old primary. Under such circumstances, the node would undergo
                     * an election and have its config epoch increased with consensus. That said, we
                     * will still explicitly bump the config epoch here to be consistent with the
                     * existing practice.
                     * Nevertheless, there are scenarios where the source shard may have transferred slot
                     * to a different shard. In these cases, the bumping of the config epoch
                     * could result in that slot assignment getting reverted. However, we consider
                     * this as a very rare case and err on the side of being consistent with the current
                     * practice. */
                    clusterDelSlot(j);
                    clusterAddSlot(myself, j);
                    clusterBumpConfigEpochWithoutConsensus();
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE |
                                         CLUSTER_TODO_FSYNC_CONFIG);
                }
            }
        }
    }

    /* After updating the slots configuration, don't do any actual change
     * in the state of the server if a module disabled Cluster
     * keys redirections. */
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION) return;

    /* Handle a special case where new_primary is not set but both sender
     * and myself own no slots and in the same shard. Set the sender as
     * the new primary if my current config epoch is lower than the
     * sender's. */
    if (!new_primary && myself->replicaof != sender && sender_slots == 0 && myself->numslots == 0 &&
        nodeEpoch(myself) < senderConfigEpoch && are_in_same_shard) {
        new_primary = sender;
    }

    /* If the shard to which this node (myself) belongs loses all of
     * its slots, this node should become a replica of the sender if
     * one of the following conditions is true:
     *
     * 1. cluster-allow-replication-migration is enabled
     * 2. all the lost slots go to the sender and the sender belongs
     *    to this node's shard
     *
     * Note that case 2 can happen in one of the following scenarios:
     * 1) we were a primary and the sender was a replica in the same
     *    shard but just became the primary after a failover
     * 2) we were a replica and our primary lost all of its slots to
     *    the sender who was another replica in the same shard and has
     *    just become the primary after a failover
     *
     * It is also possible that the sender is a primary in a different
     * shard and our primary just had its last slot migrated to the
     * sender. In this case we don't reconfigure ourselves as a replica
     * of the sender. */
    if (new_primary && cur_primary->numslots == 0) {
        if (server.cluster_allow_replica_migration || are_in_same_shard) {
            serverLog(LL_NOTICE,
                      "Configuration change detected. Reconfiguring myself "
                      "as a replica of node %.40s (%s) in shard %.40s",
                      sender->name, sender->human_nodename, sender->shard_id);
            /* Don't clear the migrating/importing states if this is a replica that
             * just gets promoted to the new primary in the shard.
             *
             * If the sender and myself are in the same shard, try psync. */
            clusterSetPrimary(sender, !are_in_same_shard, !are_in_same_shard);
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_FSYNC_CONFIG);
        } else if (nodeIsPrimary(myself) && (sender_slots >= migrated_our_slots) && !are_in_same_shard) {
            /* When all our slots are lost to the sender and the sender belongs to
             * a different shard, this is likely due to a client triggered slot
             * migration. Don't reconfigure this node to migrate to the new shard
             * in this case. */
            serverLog(LL_NOTICE,
                      "My last slot was migrated to node %.40s (%s) in shard %.40s. I am now an empty primary.",
                      sender->name, sender->human_nodename, sender->shard_id);
        }
    } else if (dirty_slots_count) {
        /* If we are here, we received an update message which removed
         * ownership for certain slots we still have keys about, but still
         * we are serving some slots, so this primary node was not demoted to
         * a replica.
         *
         * In order to maintain a consistent state between keys and slots
         * we need to remove all the keys from the slots we lost. */
        for (int j = 0; j < dirty_slots_count; j++) {
            serverLog(LL_NOTICE, "Deleting keys in dirty slot %d on node %.40s (%s) in shard %.40s", dirty_slots[j],
                      myself->name, myself->human_nodename, myself->shard_id);
            delKeysInSlot(dirty_slots[j]);
        }
    }
}

/* Cluster ping extensions.
 *
 * The ping/pong/meet messages support arbitrary extensions to add additional
 * metadata to the messages that are sent between the various nodes in the
 * cluster. The extensions take the form:
 * [ Header length + type (8 bytes) ]
 * [ Extension information (Arbitrary length, but must be 8 byte padded) ]
 */


/* Returns the length of a given extension */
static uint32_t getPingExtLength(clusterMsgPingExt *ext) {
    return ntohl(ext->length);
}

/* Returns the initial position of ping extensions. May return an invalid
 * address if there are no ping extensions. */
static clusterMsgPingExt *getInitialPingExt(clusterMsg *hdr, int count) {
    clusterMsgPingExt *initial = (clusterMsgPingExt *)&(hdr->data.ping.gossip[count]);
    return initial;
}

/* Given a current ping extension, returns the start of the next extension. May return
 * an invalid address if there are no further ping extensions. */
static clusterMsgPingExt *getNextPingExt(clusterMsgPingExt *ext) {
    clusterMsgPingExt *next = (clusterMsgPingExt *)(((char *)ext) + getPingExtLength(ext));
    return next;
}

/* All PING extensions must be 8-byte aligned */
static uint32_t getAlignedPingExtSize(uint32_t dataSize) {
    return sizeof(clusterMsgPingExt) + EIGHT_BYTE_ALIGN(dataSize);
}

static uint32_t getShardIdPingExtSize(void) {
    return getAlignedPingExtSize(sizeof(clusterMsgPingExtShardId));
}

static uint32_t getForgottenNodeExtSize(void) {
    return getAlignedPingExtSize(sizeof(clusterMsgPingExtForgottenNode));
}

static void *preparePingExt(clusterMsgPingExt *ext, uint16_t type, uint32_t length) {
    ext->type = htons(type);
    ext->length = htonl(length);
    return &ext->ext[0];
}

/* If value is nonempty and cursor_ptr points to a non-NULL cursor, writes a
 * ping extension at the cursor, advances the cursor, increments totlen and
 * returns 1. If value is nonempty and cursor_ptr points to NULL, just computes
 * the size, increments totlen and returns 1. If value is empty, returns 0. */
static uint32_t
writeSdsPingExtIfNonempty(uint32_t *totlen_ptr, clusterMsgPingExt **cursor_ptr, clusterMsgPingtypes type, sds value) {
    size_t len = sdslen(value);
    if (len == 0) return 0;
    size_t size = getAlignedPingExtSize(len + 1);
    if (*cursor_ptr != NULL) {
        void *ext = preparePingExt(*cursor_ptr, type, size);
        memcpy(ext, value, len);
        *cursor_ptr = getNextPingExt(*cursor_ptr);
    }
    *totlen_ptr += size;
    return 1;
}

/* 1. If a NULL hdr is provided, compute the extension size;
 * 2. If a non-NULL hdr is provided, write the ping
 *    extensions at the start of the cursor. This function
 *    will update the cursor to point to the end of the
 *    written extension and will return the amount of bytes
 *    written. */
static uint32_t writePingExtensions(clusterMsg *hdr, int gossipcount) {
    uint16_t extensions = 0;
    uint32_t totlen = 0;
    clusterMsgPingExt *cursor = NULL;
    /* Set the initial extension position */
    if (hdr != NULL) {
        cursor = getInitialPingExt(hdr, gossipcount);
    }

    /* Write simple optional SDS ping extensions. */
    extensions += writeSdsPingExtIfNonempty(&totlen, &cursor, CLUSTERMSG_EXT_TYPE_HOSTNAME, myself->hostname);
    extensions +=
        writeSdsPingExtIfNonempty(&totlen, &cursor, CLUSTERMSG_EXT_TYPE_HUMAN_NODENAME, myself->human_nodename);
    extensions +=
        writeSdsPingExtIfNonempty(&totlen, &cursor, CLUSTERMSG_EXT_TYPE_CLIENT_IPV4, myself->announce_client_ipv4);
    extensions +=
        writeSdsPingExtIfNonempty(&totlen, &cursor, CLUSTERMSG_EXT_TYPE_CLIENT_IPV6, myself->announce_client_ipv6);

    /* Gossip forgotten nodes */
    if (dictSize(server.cluster->nodes_black_list) > 0) {
        dictIterator *di = dictGetIterator(server.cluster->nodes_black_list);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            if (cursor != NULL) {
                uint64_t expire = dictGetUnsignedIntegerVal(de);
                if ((time_t)expire < server.unixtime) continue; /* already expired */
                uint64_t ttl = expire - server.unixtime;
                clusterMsgPingExtForgottenNode *ext =
                    preparePingExt(cursor, CLUSTERMSG_EXT_TYPE_FORGOTTEN_NODE, getForgottenNodeExtSize());
                memcpy(ext->name, dictGetKey(de), CLUSTER_NAMELEN);
                ext->ttl = htonu64(ttl);

                /* Move the write cursor */
                cursor = getNextPingExt(cursor);
            }
            totlen += getForgottenNodeExtSize();
            extensions++;
        }
        dictReleaseIterator(di);
    }

    /* Populate shard_id */
    if (cursor != NULL) {
        clusterMsgPingExtShardId *ext = preparePingExt(cursor, CLUSTERMSG_EXT_TYPE_SHARDID, getShardIdPingExtSize());
        memcpy(ext->shard_id, myself->shard_id, CLUSTER_NAMELEN);

        /* Move the write cursor */
        cursor = getNextPingExt(cursor);
    }
    totlen += getShardIdPingExtSize();
    extensions++;

    if (hdr != NULL) {
        hdr->mflags[0] |= CLUSTERMSG_FLAG0_EXT_DATA;
        hdr->extensions = htons(extensions);
    }

    return totlen;
}

/* We previously validated the extensions, so this function just needs to
 * handle the extensions. */
void clusterProcessPingExtensions(clusterMsg *hdr, clusterLink *link) {
    clusterNode *sender = link->node ? link->node : clusterLookupNode(hdr->sender, CLUSTER_NAMELEN);
    char *ext_hostname = NULL;
    char *ext_humannodename = NULL;
    char *ext_clientipv4 = NULL;
    char *ext_clientipv6 = NULL;
    char *ext_shardid = NULL;
    uint16_t extensions = ntohs(hdr->extensions);
    /* Loop through all the extensions and process them */
    clusterMsgPingExt *ext = getInitialPingExt(hdr, ntohs(hdr->count));
    while (extensions--) {
        uint16_t type = ntohs(ext->type);
        if (type == CLUSTERMSG_EXT_TYPE_HOSTNAME) {
            clusterMsgPingExtHostname *hostname_ext = (clusterMsgPingExtHostname *)&(ext->ext[0].hostname);
            ext_hostname = hostname_ext->hostname;
        } else if (type == CLUSTERMSG_EXT_TYPE_HUMAN_NODENAME) {
            clusterMsgPingExtHumanNodename *humannodename_ext =
                (clusterMsgPingExtHumanNodename *)&(ext->ext[0].human_nodename);
            ext_humannodename = humannodename_ext->human_nodename;
        } else if (type == CLUSTERMSG_EXT_TYPE_CLIENT_IPV4) {
            clusterMsgPingExtClientIpV4 *clientipv4_ext =
                (clusterMsgPingExtClientIpV4 *)&(ext->ext[0].announce_client_ipv4);
            ext_clientipv4 = clientipv4_ext->announce_client_ipv4;
        } else if (type == CLUSTERMSG_EXT_TYPE_CLIENT_IPV6) {
            clusterMsgPingExtClientIpV6 *clientipv6_ext =
                (clusterMsgPingExtClientIpV6 *)&(ext->ext[0].announce_client_ipv6);
            ext_clientipv6 = clientipv6_ext->announce_client_ipv6;
        } else if (type == CLUSTERMSG_EXT_TYPE_FORGOTTEN_NODE) {
            clusterMsgPingExtForgottenNode *forgotten_node_ext = &(ext->ext[0].forgotten_node);
            clusterNode *n = clusterLookupNode(forgotten_node_ext->name, CLUSTER_NAMELEN);
            if (n && n != myself && !(nodeIsReplica(myself) && myself->replicaof == n)) {
                sds id = sdsnewlen(forgotten_node_ext->name, CLUSTER_NAMELEN);
                dictEntry *de = dictAddOrFind(server.cluster->nodes_black_list, id);
                uint64_t expire = server.unixtime + ntohu64(forgotten_node_ext->ttl);
                dictSetUnsignedIntegerVal(de, expire);
                clusterDelNode(n);
                clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
            }
        } else if (type == CLUSTERMSG_EXT_TYPE_SHARDID) {
            clusterMsgPingExtShardId *shardid_ext = (clusterMsgPingExtShardId *)&(ext->ext[0].shard_id);
            ext_shardid = shardid_ext->shard_id;
        } else {
            /* Unknown type, we will ignore it but log what happened. */
            serverLog(LL_WARNING, "Received unknown extension type %d", type);
        }

        /* We know this will be valid since we validated it ahead of time */
        ext = getNextPingExt(ext);
    }

    /* If the node did not send us a hostname extension, assume
     * they don't have an announced hostname. Otherwise, we'll
     * set it now. */
    updateAnnouncedHostname(sender, ext_hostname);
    updateAnnouncedHumanNodename(sender, ext_humannodename);
    updateAnnouncedClientIpV4(sender, ext_clientipv4);
    updateAnnouncedClientIpV6(sender, ext_clientipv6);
    /* If the node did not send us a shard-id extension, it means the sender
     * does not support it (old version), node->shard_id is randomly generated.
     * A cluster-wide consensus for the node's shard_id is not necessary.
     * The key is maintaining consistency of the shard_id on each individual 7.2 node.
     * As the cluster progressively upgrades to version 7.2, we can expect the shard_ids
     * across all nodes to naturally converge and align.
     *
     * If sender is a replica, set the shard_id to the shard_id of its primary.
     * Otherwise, we'll set it now. */
    if (ext_shardid == NULL) ext_shardid = clusterNodeGetPrimary(sender)->shard_id;

    updateShardId(sender, ext_shardid);
}

static clusterNode *getNodeFromLinkAndMsg(clusterLink *link, clusterMsg *hdr) {
    clusterNode *sender;
    if (link->node && !nodeInHandshake(link->node)) {
        /* If the link has an associated node, use that so that we don't have to look it
         * up every time, except when the node is still in handshake, the node still has
         * a random name thus not truly "known". */
        sender = link->node;
    } else {
        /* Otherwise, fetch sender based on the message */
        sender = clusterLookupNode(hdr->sender, CLUSTER_NAMELEN);
        /* We know the sender node but haven't associate it with the link. This must
         * be an inbound link because only for inbound links we didn't know which node
         * to associate when they were created. */
        if (sender && !link->node) {
            setClusterNodeToInboundClusterLink(sender, link);
        }
    }
    return sender;
}

static void clusterProcessPublishPacket(clusterMsgDataPublish *publish_data, uint16_t type) {
    robj *channel, *message;
    uint32_t channel_len, message_len;

    /* Don't bother creating useless objects if there are no
     * Pub/Sub subscribers. */
    if ((type == CLUSTERMSG_TYPE_PUBLISH && serverPubsubSubscriptionCount() > 0) ||
        (type == CLUSTERMSG_TYPE_PUBLISHSHARD && serverPubsubShardSubscriptionCount() > 0)) {
        channel_len = ntohl(publish_data->channel_len);
        message_len = ntohl(publish_data->message_len);
        channel = createStringObject((char *)publish_data->bulk_data, channel_len);
        message = createStringObject((char *)publish_data->bulk_data + channel_len, message_len);
        pubsubPublishMessage(channel, message, type == CLUSTERMSG_TYPE_PUBLISHSHARD);
        decrRefCount(channel);
        decrRefCount(message);
    }
}

static void clusterProcessLightPacket(clusterLink *link, uint16_t type) {
    clusterMsgLight *hdr = (clusterMsgLight *)link->rcvbuf;

    if (type == CLUSTERMSG_TYPE_PUBLISH || type == CLUSTERMSG_TYPE_PUBLISHSHARD) {
        clusterProcessPublishPacket(&hdr->data.publish.msg, type);
    }
}

static inline int messageTypeSupportsLightHdr(uint16_t type) {
    switch (type) {
    case CLUSTERMSG_TYPE_PUBLISH: return 1;
    case CLUSTERMSG_TYPE_PUBLISHSHARD: return 1;
    }
    return 0;
}


int clusterIsValidPacket(clusterLink *link) {
    clusterMsg *hdr = (clusterMsg *)link->rcvbuf;
    uint32_t totlen = ntohl(hdr->totlen);
    int is_light = IS_LIGHT_MESSAGE(ntohs(hdr->type));
    uint16_t type = ntohs(hdr->type) & ~CLUSTERMSG_MODIFIER_MASK;

    if (is_light && !messageTypeSupportsLightHdr(type)) {
        serverLog(LL_NOTICE,
                  "Packet of type '%s' (%u) does not support light cluster header. Marking packet as invalid.",
                  clusterGetMessageTypeString(type), type);
        return 0;
    }

    if (type < CLUSTERMSG_TYPE_COUNT) server.cluster->stats_bus_messages_received[type]++;

    serverLog(LL_DEBUG, "--- Processing packet of type %s, %lu bytes", clusterGetMessageTypeString(type),
              (unsigned long)totlen);

    /* Perform sanity checks */
    if (totlen < 16) return 0; /* At least signature, version, totlen, count. */
    if (totlen > link->rcvbuf_len) return 0;

    if (ntohs(hdr->ver) != CLUSTER_PROTO_VER) {
        /* Can't handle messages of different versions. */
        return 0;
    }

    if (type == server.cluster_drop_packet_filter) {
        serverLog(LL_WARNING, "Dropping packet that matches debug drop filter");
        return 0;
    }

    uint32_t explen; /* expected length of this packet */

    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG || type == CLUSTERMSG_TYPE_MEET) {
        uint16_t extensions = ntohs(hdr->extensions);
        uint16_t count = ntohs(hdr->count);

        explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
        explen += (sizeof(clusterMsgDataGossip) * count);

        /* If there is extension data, which doesn't have a fixed length,
         * loop through them and validate the length of it now. */
        if (hdr->mflags[0] & CLUSTERMSG_FLAG0_EXT_DATA) {
            clusterMsgPingExt *ext = getInitialPingExt(hdr, count);
            while (extensions--) {
                uint16_t extlen = getPingExtLength(ext);
                if (extlen % 8 != 0) {
                    serverLog(LL_WARNING, "Received a %s packet without proper padding (%d bytes)",
                              clusterGetMessageTypeString(type), (int)extlen);
                    return 0;
                }
                if ((totlen - explen) < extlen) {
                    serverLog(LL_WARNING,
                              "Received invalid %s packet with extension data that exceeds "
                              "total packet length (%lld)",
                              clusterGetMessageTypeString(type), (unsigned long long)totlen);
                    return 0;
                }
                explen += extlen;
                ext = getNextPingExt(ext);
            }
        }
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
        explen += sizeof(clusterMsgDataFail);
    } else if (type == CLUSTERMSG_TYPE_PUBLISH || type == CLUSTERMSG_TYPE_PUBLISHSHARD) {
        clusterMsgDataPublish *publish_data;
        if (is_light) {
            clusterMsgLight *hdr_light = (clusterMsgLight *)link->rcvbuf;
            publish_data = &hdr_light->data.publish.msg;
            explen = sizeof(clusterMsgLight);
        } else {
            publish_data = &hdr->data.publish.msg;
            explen = sizeof(clusterMsg);
        }
        explen -= sizeof(union clusterMsgData);
        explen +=
            sizeof(clusterMsgDataPublish) - 8 + ntohl(publish_data->channel_len) + ntohl(publish_data->message_len);
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST || type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK ||
               type == CLUSTERMSG_TYPE_MFSTART) {
        explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
        explen += sizeof(clusterMsgDataUpdate);
    } else if (type == CLUSTERMSG_TYPE_MODULE) {
        explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
        explen += sizeof(clusterMsgModule) - 3 + ntohl(hdr->data.module.msg.len);
    } else {
        /* We don't know this type of packet, so we assume it's well formed. */
        explen = totlen;
    }

    if (totlen != explen) {
        serverLog(LL_WARNING, "Received invalid %s packet of length %lld but expected length %lld",
                  clusterGetMessageTypeString(type), (unsigned long long)totlen, (unsigned long long)explen);
        return 0;
    }

    return 1;
}

/* When this function is called, there is a packet to process starting
 * at link->rcvbuf. Releasing the buffer is up to the caller, so this
 * function should just handle the higher level stuff of processing the
 * packet, modifying the cluster state if needed.
 *
 * The function returns 1 if the link is still valid after the packet
 * was processed, otherwise 0 if the link was freed since the packet
 * processing lead to some inconsistency error (for instance a PONG
 * received from the wrong sender ID). */
int clusterProcessPacket(clusterLink *link) {
    /* Validate that the packet is well-formed */
    if (!clusterIsValidPacket(link)) {
        clusterMsg *hdr = (clusterMsg *)link->rcvbuf;
        uint16_t type = ntohs(hdr->type);
        if (server.debug_cluster_close_link_on_packet_drop && type == server.cluster_drop_packet_filter) {
            freeClusterLink(link);
            serverLog(LL_WARNING, "Closing link for matching packet type %hu", type);
            return 0;
        }
        return 1;
    }

    clusterMsg *hdr = (clusterMsg *)link->rcvbuf;
    mstime_t now = mstime();
    int is_light = IS_LIGHT_MESSAGE(ntohs(hdr->type));
    uint16_t type = ntohs(hdr->type) & ~CLUSTERMSG_MODIFIER_MASK;

    if (is_light) {
        if (!link->node || nodeInHandshake(link->node)) {
            freeClusterLink(link);
            serverLog(
                LL_NOTICE,
                "Closing link for node that sent a lightweight message of type %hu as its first message on the link",
                type);
            return 0;
        }
        clusterNode *sender = link->node;
        sender->data_received = now;
        clusterProcessLightPacket(link, type);
        return 1;
    }

    uint16_t flags = ntohs(hdr->flags);
    uint64_t sender_claimed_current_epoch = 0, sender_claimed_config_epoch = 0;
    clusterNode *sender = getNodeFromLinkAndMsg(link, hdr);
    int sender_claims_to_be_primary = !memcmp(hdr->replicaof, CLUSTER_NODE_NULL_NAME, CLUSTER_NAMELEN);
    int sender_last_reported_as_replica = sender && nodeIsReplica(sender);
    int sender_last_reported_as_primary = sender && nodeIsPrimary(sender);

    if (sender && (hdr->mflags[0] & CLUSTERMSG_FLAG0_EXT_DATA)) {
        sender->flags |= CLUSTER_NODE_EXTENSIONS_SUPPORTED;
    }

    /* Checks if the node supports light message hdr */
    if (sender) {
        if (flags & CLUSTER_NODE_LIGHT_HDR_SUPPORTED) {
            sender->flags |= CLUSTER_NODE_LIGHT_HDR_SUPPORTED;
        } else {
            sender->flags &= ~CLUSTER_NODE_LIGHT_HDR_SUPPORTED;
        }
    }

    /* Update the last time we saw any data from this node. We
     * use this in order to avoid detecting a timeout from a node that
     * is just sending a lot of data in the cluster bus, for instance
     * because of Pub/Sub. */
    if (sender) sender->data_received = now;

    if (sender && !nodeInHandshake(sender)) {
        /* Update our currentEpoch if we see a newer epoch in the cluster. */
        sender_claimed_current_epoch = ntohu64(hdr->currentEpoch);
        sender_claimed_config_epoch = ntohu64(hdr->configEpoch);
        if (sender_claimed_current_epoch > server.cluster->currentEpoch)
            server.cluster->currentEpoch = sender_claimed_current_epoch;
        /* Update the sender configEpoch if it is a primary publishing a newer one. */
        if (sender_claims_to_be_primary && sender_claimed_config_epoch > sender->configEpoch) {
            sender->configEpoch = sender_claimed_config_epoch;
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_FSYNC_CONFIG);
        }
        /* Update the replication offset info for this node. */
        sender->repl_offset = ntohu64(hdr->offset);
        sender->repl_offset_time = now;
        /* If we are a replica performing a manual failover and our primary
         * sent its offset while already paused, populate the MF state. */
        if (server.cluster->mf_end && nodeIsReplica(myself) && myself->replicaof == sender &&
            hdr->mflags[0] & CLUSTERMSG_FLAG0_PAUSED && server.cluster->mf_primary_offset == -1) {
            server.cluster->mf_primary_offset = sender->repl_offset;
            clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_MANUALFAILOVER);
            serverLog(LL_NOTICE,
                      "Received replication offset for paused "
                      "primary manual failover: %lld",
                      server.cluster->mf_primary_offset);
        }
    }

    /* Initial processing of PING and MEET requests replying with a PONG. */
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_MEET) {
        /* We use incoming MEET messages in order to set the address
         * for 'myself', since only other cluster nodes will send us
         * MEET messages on handshakes, when the cluster joins, or
         * later if we changed address, and those nodes will use our
         * official address to connect to us. So by obtaining this address
         * from the socket is a simple way to discover / update our own
         * address in the cluster without it being hardcoded in the config.
         *
         * However if we don't have an address at all, we update the address
         * even with a normal PING packet. If it's wrong it will be fixed
         * by MEET later. */
        if ((type == CLUSTERMSG_TYPE_MEET || myself->ip[0] == '\0') && server.cluster_announce_ip == NULL) {
            char ip[NET_IP_STR_LEN];

            if (connAddrSockName(link->conn, ip, sizeof(ip), NULL) != -1 && strcmp(ip, myself->ip)) {
                memcpy(myself->ip, ip, NET_IP_STR_LEN);
                serverLog(LL_NOTICE, "IP address for this node updated to %s", myself->ip);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            }
        }

        /* Add this node if it is new for us and the msg type is MEET.
         * In this stage we don't try to add the node with the right
         * flags, replicaof pointer, and so forth, as this details will be
         * resolved when we'll receive PONGs from the node. The exception
         * to this is the flag that indicates extensions are supported, as
         * we want to send extensions right away in the return PONG in order
         * to reduce the amount of time needed to stabilize the shard ID. */
        if (!sender && type == CLUSTERMSG_TYPE_MEET) {
            clusterNode *node;

            node = createClusterNode(NULL, CLUSTER_NODE_HANDSHAKE);
            serverAssert(nodeIp2String(node->ip, link, hdr->myip) == C_OK);
            getClientPortFromClusterMsg(hdr, &node->tls_port, &node->tcp_port);
            node->cport = ntohs(hdr->cport);
            if (hdr->mflags[0] & CLUSTERMSG_FLAG0_EXT_DATA) {
                node->flags |= CLUSTER_NODE_EXTENSIONS_SUPPORTED;
            }
            setClusterNodeToInboundClusterLink(node, link);
            clusterAddNode(node);
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
        }

        /* If this is a MEET packet from an unknown node, we still process
         * the gossip section here since we have to trust the sender because
         * of the message type. */
        if (!sender && type == CLUSTERMSG_TYPE_MEET) clusterProcessGossipSection(hdr, link);

        /* Anyway reply with a PONG */
        clusterSendPing(link, CLUSTERMSG_TYPE_PONG);
    }

    /* PING, PONG, MEET: process config information. */
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG || type == CLUSTERMSG_TYPE_MEET) {
        serverLog(LL_DEBUG, "%s packet received: %.40s", clusterGetMessageTypeString(type),
                  link->node ? link->node->name : "NULL");

        if (sender && (sender->flags & CLUSTER_NODE_MEET)) {
            /* Once we get a response for MEET from the sender, we can stop sending more MEET. */
            sender->flags &= ~CLUSTER_NODE_MEET;
            serverLog(LL_NOTICE, "Successfully completed handshake with %.40s (%s)", sender->name,
                      sender->human_nodename);
        }
        if (!link->inbound) {
            if (nodeInHandshake(link->node)) {
                /* If we already have this node, try to change the
                 * IP/port of the node with the new one. */
                if (sender) {
                    serverLog(LL_VERBOSE,
                              "Handshake: we already know node %.40s (%s), "
                              "updating the address if needed.",
                              sender->name, sender->human_nodename);
                    if (nodeUpdateAddressIfNeeded(sender, link, hdr)) {
                        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
                    }
                    /* Free this node as we already have it. This will
                     * cause the link to be freed as well. */
                    clusterDelNode(link->node);
                    return 0;
                }

                /* First thing to do is replacing the random name with the
                 * right node name if this was a handshake stage. */
                clusterRenameNode(link->node, hdr->sender);
                serverLog(LL_DEBUG, "Handshake with node %.40s completed.", link->node->name);
                link->node->flags &= ~CLUSTER_NODE_HANDSHAKE;
                link->node->flags |= flags & (CLUSTER_NODE_PRIMARY | CLUSTER_NODE_REPLICA);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            } else if (memcmp(link->node->name, hdr->sender, CLUSTER_NAMELEN) != 0) {
                /* If the reply has a non matching node ID we
                 * disconnect this node and set it as not having an associated
                 * address. */
                serverLog(LL_NOTICE,
                          "PONG contains mismatching sender ID. About node %.40s (%s) in shard %.40s added %d ms ago, "
                          "having flags %d",
                          link->node->name, link->node->human_nodename, link->node->shard_id,
                          (int)(now - (link->node->ctime)), link->node->flags);
                link->node->flags |= CLUSTER_NODE_NOADDR;
                link->node->ip[0] = '\0';
                link->node->tcp_port = 0;
                link->node->tls_port = 0;
                link->node->cport = 0;
                freeClusterLink(link);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                return 0;
            }
        }

        /* Copy the CLUSTER_NODE_NOFAILOVER flag from what the sender
         * announced. This is a dynamic flag that we receive from the
         * sender, and the latest status must be trusted. We need it to
         * be propagated because the replica ranking used to understand the
         * delay of each replica in the voting process, needs to know
         * what are the instances really competing. */
        if (sender) {
            int nofailover = flags & CLUSTER_NODE_NOFAILOVER;
            sender->flags &= ~CLUSTER_NODE_NOFAILOVER;
            sender->flags |= nofailover;
        }

        /* Update the node address if it changed. */
        if (sender && type == CLUSTERMSG_TYPE_PING && !nodeInHandshake(sender) &&
            nodeUpdateAddressIfNeeded(sender, link, hdr)) {
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
        }

        /* Update our info about the node */
        if (!link->inbound && type == CLUSTERMSG_TYPE_PONG) {
            link->node->pong_received = now;
            link->node->ping_sent = 0;

            /* The PFAIL condition can be reversed without external
             * help if it is momentary (that is, if it does not
             * turn into a FAIL state).
             *
             * The FAIL condition is also reversible under specific
             * conditions detected by clearNodeFailureIfNeeded(). */
            if (nodeTimedOut(link->node)) {
                link->node->flags &= ~CLUSTER_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
            } else if (nodeFailed(link->node)) {
                clearNodeFailureIfNeeded(link->node);
            }
        }

        /* Check for role switch: replica -> primary or primary -> replica. */
        if (sender) {
            serverLog(LL_DEBUG, "node %.40s (%s) announces that it is a %s in shard %.40s", sender->name,
                      sender->human_nodename, sender_claims_to_be_primary ? "primary" : "replica", sender->shard_id);
            if (sender_claims_to_be_primary) {
                /* Node is a primary. */
                clusterSetNodeAsPrimary(sender);
            } else {
                /* Node is a replica. */
                clusterNode *sender_claimed_primary = clusterLookupNode(hdr->replicaof, CLUSTER_NAMELEN);

                if (sender_last_reported_as_primary) {
                    /* Primary turned into a replica! Reconfigure the node. */
                    if (sender_claimed_primary && areInSameShard(sender_claimed_primary, sender)) {
                        /* `sender` was a primary and was in the same shard as its new primary */
                        if (sender->configEpoch > sender_claimed_config_epoch) {
                            serverLog(LL_NOTICE,
                                      "Ignore stale message from %.40s (%s) in shard %.40s;"
                                      " gossip config epoch: %llu, current config epoch: %llu",
                                      sender->name, sender->human_nodename, sender->shard_id,
                                      (unsigned long long)sender_claimed_config_epoch,
                                      (unsigned long long)sender->configEpoch);
                        } else {
                            /* `primary` is still a `replica` in this observer node's view;
                             * update its role and configEpoch */
                            clusterSetNodeAsPrimary(sender_claimed_primary);
                            sender_claimed_primary->configEpoch = sender_claimed_config_epoch;
                            serverLog(LL_NOTICE,
                                      "A failover occurred in shard %.40s; node %.40s (%s)"
                                      " failed over to node %.40s (%s) with a config epoch of %llu",
                                      sender->shard_id, sender->name, sender->human_nodename,
                                      sender_claimed_primary->name, sender_claimed_primary->human_nodename,
                                      (unsigned long long)sender_claimed_primary->configEpoch);
                        }
                    } else {
                        /* `sender` was moved to another shard and has become a replica, remove its slot assignment */
                        int slots = clusterDelNodeSlots(sender);
                        serverLog(LL_NOTICE,
                                  "Node %.40s (%s) is no longer primary of shard %.40s;"
                                  " removed all %d slot(s) it used to own",
                                  sender->name, sender->human_nodename, sender->shard_id, slots);
                        if (sender_claimed_primary != NULL) {
                            serverLog(LL_NOTICE, "Node %.40s (%s) is now part of shard %.40s", sender->name,
                                      sender->human_nodename, sender_claimed_primary->shard_id);
                        }
                    }

                    sender->flags &= ~(CLUSTER_NODE_PRIMARY | CLUSTER_NODE_MIGRATE_TO);
                    sender->flags |= CLUSTER_NODE_REPLICA;

                    /* Update config and state. */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
                }

                /* Primary node changed for this replica? */
                if (sender_claimed_primary && sender->replicaof != sender_claimed_primary) {
                    if (sender->replicaof) clusterNodeRemoveReplica(sender->replicaof, sender);
                    serverLog(LL_NOTICE, "Node %.40s (%s) is now a replica of node %.40s (%s) in shard %.40s",
                              sender->name, sender->human_nodename, sender_claimed_primary->name,
                              sender_claimed_primary->human_nodename, sender_claimed_primary->shard_id);
                    clusterNodeAddReplica(sender_claimed_primary, sender);
                    sender->replicaof = sender_claimed_primary;

                    /* The chain reduction logic requires correctly establishing the replication relationship.
                     * A key decision when designating a new primary for 'myself' is determining whether
                     * 'myself' and the new primary belong to the same shard, which would imply shared
                     * replication history and allow a safe partial synchronization (psync).
                     *
                     * This decision hinges on the shard_id, a per-node property that helps verify if the
                     * two nodes share the same replication history. It's critical not to update myself's
                     * shard_id prematurely during this process. Doing so could incorrectly associate
                     * 'myself' with the sender's shard_id, leading the subsequent clusterSetPrimary call
                     * to falsely assume that 'myself' and the new primary have been in the same shard.
                     * This mistake could result in data loss by incorrectly permitting a psync.
                     *
                     * Therefore, it's essential to delay any shard_id updates until after the replication
                     * relationship has been properly established and verified. */
                    if (myself->replicaof && myself->replicaof->replicaof && myself->replicaof->replicaof != myself) {
                        /* Safeguard against sub-replicas.
                         *
                         * A replica's primary can turn itself into a replica if its last slot
                         * is removed. If no other node takes over the slot, there is nothing
                         * else to trigger replica migration. In this case, they are not in the
                         * same shard, so a full sync is required.
                         *
                         * Or a replica's primary can turn itself into a replica of its other
                         * replica during a failover. In this case, they are in the same shard,
                         * so we can try a psync. */
                        serverLog(LL_NOTICE, "I'm a sub-replica! Reconfiguring myself as a replica of %.40s from %.40s",
                                  myself->replicaof->replicaof->name, myself->replicaof->name);
                        clusterSetPrimary(myself->replicaof->replicaof, 1,
                                          !areInSameShard(myself->replicaof->replicaof, myself));
                        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE |
                                             CLUSTER_TODO_FSYNC_CONFIG);
                    }

                    /* Update the shard_id when a replica is connected to its
                     * primary in the very first time. */
                    updateShardId(sender, sender_claimed_primary->shard_id);

                    /* Update config. */
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                }
            }
        }

        /* Update our info about served slots.
         *
         * Note: this MUST happen after we update the primary/replica state
         * so that CLUSTER_NODE_PRIMARY flag will be set. */

        /* Many checks are only needed if the set of served slots this
         * instance claims is different compared to the set of slots we have
         * for it or if there was a failover in the sender's shard. Check
         * this ASAP to avoid other computational expensive checks later.*/

        if (sender && sender_claims_to_be_primary &&
            (sender_last_reported_as_replica || memcmp(sender->slots, hdr->myslots, sizeof(hdr->myslots)))) {
            /* Make sure CLUSTER_NODE_PRIMARY has already been set by now on sender */
            serverAssert(nodeIsPrimary(sender));

            /* 1) If the sender of the message is a primary, and we detected that
             *    the set of slots it claims changed, scan the slots to see if we
             *    need to update our configuration. */
            clusterUpdateSlotsConfigWith(sender, sender_claimed_config_epoch, hdr->myslots);

            /* 2) We also check for the reverse condition, that is, the sender
             *    claims to serve slots we know are served by a primary with a
             *    greater configEpoch. If this happens we inform the sender.
             *
             * This is useful because sometimes after a partition heals, a
             * reappearing primary may be the last one to claim a given set of
             * hash slots, but with a configuration that other instances know to
             * be deprecated. Example:
             *
             * A and B are primary and replica for slots 1,2,3.
             * A is partitioned away, B gets promoted.
             * B is partitioned away, and A returns available.
             *
             * Usually B would PING A publishing its set of served slots and its
             * configEpoch, but because of the partition B can't inform A of the
             * new configuration, so other nodes that have an updated table must
             * do it. In this way A will stop to act as a primary (or can try to
             * failover if there are the conditions to win the election). */
            for (int j = 0; j < CLUSTER_SLOTS; j++) {
                if (bitmapTestBit(hdr->myslots, j)) {
                    if (server.cluster->slots[j] == sender || isSlotUnclaimed(j)) continue;
                    if (server.cluster->slots[j]->configEpoch > sender_claimed_config_epoch) {
                        serverLog(LL_VERBOSE,
                                  "Node %.40s has old slots configuration, sending "
                                  "an UPDATE message about %.40s",
                                  sender->name, server.cluster->slots[j]->name);
                        clusterSendUpdate(sender->link, server.cluster->slots[j]);

                        /* TODO: instead of exiting the loop send every other
                         * UPDATE packet for other nodes that are the new owner
                         * of sender's slots. */
                        break;
                    }
                }
            }
        }

        /* If our config epoch collides with the sender's try to fix
         * the problem. */
        if (sender && nodeIsPrimary(myself) && nodeIsPrimary(sender) &&
            sender_claimed_config_epoch == myself->configEpoch) {
            clusterHandleConfigEpochCollision(sender);
        }

        /* Get info from the gossip section */
        if (sender) {
            clusterProcessGossipSection(hdr, link);
            clusterProcessPingExtensions(hdr, link);
        }
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        clusterNode *failing;

        if (sender) {
            failing = clusterLookupNode(hdr->data.fail.about.nodename, CLUSTER_NAMELEN);
            if (failing && !(failing->flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_MYSELF))) {
                serverLog(LL_NOTICE, "FAIL message received from %.40s (%s) about %.40s (%s)", hdr->sender,
                          sender->human_nodename, hdr->data.fail.about.nodename, failing->human_nodename);
                failing->flags |= CLUSTER_NODE_FAIL;
                failing->fail_time = now;
                failing->flags &= ~CLUSTER_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
            }
        } else {
            serverLog(LL_NOTICE, "Ignoring FAIL message from unknown node %.40s about %.40s", hdr->sender,
                      hdr->data.fail.about.nodename);
        }
    } else if (type == CLUSTERMSG_TYPE_PUBLISH || type == CLUSTERMSG_TYPE_PUBLISHSHARD) {
        if (!sender) return 1; /* We don't know that node. */
        clusterProcessPublishPacket(&hdr->data.publish.msg, type);
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST) {
        if (!sender) return 1; /* We don't know that node. */
        clusterSendFailoverAuthIfNeeded(sender, hdr);
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK) {
        if (!sender) return 1; /* We don't know that node. */
        /* We consider this vote only if the sender is a primary serving
         * a non zero number of slots, and its currentEpoch is greater or
         * equal to epoch where this node started the election. */
        if (clusterNodeIsVotingPrimary(sender) && sender_claimed_current_epoch >= server.cluster->failover_auth_epoch) {
            server.cluster->failover_auth_count++;
            /* Maybe we reached a quorum here, set a flag to make sure
             * we check ASAP. */
            clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        }
    } else if (type == CLUSTERMSG_TYPE_MFSTART) {
        /* This message is acceptable only if I'm a primary and the sender
         * is one of my replicas. */
        if (!sender || sender->replicaof != myself) return 1;
        /* Manual failover requested from replicas. Initialize the state
         * accordingly. */
        resetManualFailover();
        server.cluster->mf_end = now + CLUSTER_MF_TIMEOUT;
        server.cluster->mf_replica = sender;
        pauseActions(PAUSE_DURING_FAILOVER, now + (CLUSTER_MF_TIMEOUT * CLUSTER_MF_PAUSE_MULT),
                     PAUSE_ACTIONS_CLIENT_WRITE_SET);
        serverLog(LL_NOTICE, "Manual failover requested by replica %.40s (%s).", sender->name, sender->human_nodename);
        /* We need to send a ping message to the replica, as it would carry
         * `server.cluster->mf_primary_offset`, which means the primary paused clients
         * at offset `server.cluster->mf_primary_offset`, so that the replica would
         * know that it is safe to set its `server.cluster->mf_can_start` to 1 so as
         * to complete failover as quickly as possible. */
        clusterSendPing(link, CLUSTERMSG_TYPE_PING);
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        clusterNode *n; /* The node the update is about. */
        uint64_t reportedConfigEpoch = ntohu64(hdr->data.update.nodecfg.configEpoch);

        if (!sender) return 1; /* We don't know the sender. */
        n = clusterLookupNode(hdr->data.update.nodecfg.nodename, CLUSTER_NAMELEN);
        if (!n) return 1;                                    /* We don't know the reported node. */
        if (n->configEpoch >= reportedConfigEpoch) return 1; /* Nothing new. */

        /* If in our current config the node is a replica, set it as a primary. */
        if (nodeIsReplica(n)) clusterSetNodeAsPrimary(n);

        /* Update the node's configEpoch. */
        n->configEpoch = reportedConfigEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_FSYNC_CONFIG);

        /* Check the bitmap of served slots and update our
         * config accordingly. */
        clusterUpdateSlotsConfigWith(n, reportedConfigEpoch, hdr->data.update.nodecfg.slots);
    } else if (type == CLUSTERMSG_TYPE_MODULE) {
        if (!sender) return 1; /* Protect the module from unknown nodes. */
        /* We need to route this message back to the right module subscribed
         * for the right message type. */
        uint64_t module_id = hdr->data.module.msg.module_id; /* Endian-safe ID */
        uint32_t len = ntohl(hdr->data.module.msg.len);
        uint8_t type = hdr->data.module.msg.type;
        unsigned char *payload = hdr->data.module.msg.bulk_data;
        moduleCallClusterReceivers(sender->name, module_id, type, payload, len);
    } else {
        serverLog(LL_WARNING, "Received unknown packet type: %d", type);
    }
    return 1;
}

/* This function is called when we detect the link with this node is lost.
   We set the node as no longer connected. The Cluster Cron will detect
   this connection and will try to get it connected again.

   Instead if the node is a temporary node used to accept a query, we
   completely free the node on error. */
void handleLinkIOError(clusterLink *link) {
    freeClusterLink(link);
}

/* Send the messages queued for the link. */
void clusterWriteHandler(connection *conn) {
    clusterLink *link = connGetPrivateData(conn);
    ssize_t nwritten;
    size_t totwritten = 0;

    while (totwritten < NET_MAX_WRITES_PER_EVENT && listLength(link->send_msg_queue) > 0) {
        listNode *head = listFirst(link->send_msg_queue);
        clusterMsgSendBlock *msgblock = (clusterMsgSendBlock *)head->value;
        clusterMsg *msg = &msgblock->msg;
        size_t msg_offset = link->head_msg_send_offset;
        size_t msg_len = ntohl(msg->totlen);

        nwritten = connWrite(conn, (char *)msg + msg_offset, msg_len - msg_offset);
        if (nwritten <= 0) {
            serverLog(LL_DEBUG, "I/O error writing to node link: %s",
                      (nwritten == -1) ? connGetLastError(conn) : "short write");
            handleLinkIOError(link);
            return;
        }
        if (msg_offset + nwritten < msg_len) {
            /* If full message wasn't written, record the offset
             * and continue sending from this point next time */
            link->head_msg_send_offset += nwritten;
            return;
        }
        serverAssert((msg_offset + nwritten) == msg_len);
        link->head_msg_send_offset = 0;

        /* Delete the node and update our memory tracking */
        uint32_t blocklen = msgblock->totlen;
        listDelNode(link->send_msg_queue, head);
        server.stat_cluster_links_memory -= sizeof(listNode);
        link->send_msg_queue_mem -= sizeof(listNode) + blocklen;

        totwritten += nwritten;
    }

    if (listLength(link->send_msg_queue) == 0) connSetWriteHandler(link->conn, NULL);
}

/* A connect handler that gets called when a connection to another node
 * gets established.
 */
void clusterLinkConnectHandler(connection *conn) {
    clusterLink *link = connGetPrivateData(conn);
    clusterNode *node = link->node;

    /* Check if connection succeeded */
    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_VERBOSE, "Connection with Node %.40s at %s:%d failed: %s", node->name, node->ip, node->cport,
                  connGetLastError(conn));
        freeClusterLink(link);
        return;
    }

    /* Register a read handler from now on */
    connSetReadHandler(conn, clusterReadHandler);

    /* Queue a PING in the new connection ASAP: this is crucial
     * to avoid false positives in failure detection.
     *
     * If the node is flagged as MEET, we send a MEET message instead
     * of a PING one, to force the receiver to add us in its node
     * table. */
    mstime_t old_ping_sent = node->ping_sent;
    clusterSendPing(link, node->flags & CLUSTER_NODE_MEET ? CLUSTERMSG_TYPE_MEET : CLUSTERMSG_TYPE_PING);
    if (old_ping_sent) {
        /* If there was an active ping before the link was
         * disconnected, we want to restore the ping time, otherwise
         * replaced by the clusterSendPing() call. */
        node->ping_sent = old_ping_sent;
    }
    /* NOTE: Assume the current node is A and is asked to MEET another node B.
     * Once A sends MEET to B, it cannot clear the MEET flag for B until it
     * gets a response from B. If the MEET packet is not accepted by B due to
     * link failure, A must continue sending MEET. If A doesn't continue sending
     * MEET, A will know about B, but B will never add A. Every node always
     * responds to PINGs from unknown nodes with a PONG, so A will know about B
     * and continue sending PINGs. But B won't add A until it sees a MEET (or it
     * gets to know about A from a trusted third node C). In this case, clearing
     * the MEET flag here leads to asymmetry in the cluster membership. So, we
     * clear the MEET flag in clusterProcessPacket.
     */

    serverLog(LL_DEBUG, "Connecting with Node %.40s at %s:%d", node->name, node->ip, node->cport);
}

/* Performs sanity check on the message signature and length depending on the type. */
static inline int isClusterMsgSignatureAndLengthValid(clusterMsg *hdr) {
    if (memcmp(hdr->sig, "RCmb", 4) != 0) return 0;
    uint16_t type = ntohs(hdr->type);
    uint32_t totlen = ntohl(hdr->totlen);
    uint32_t minlen = IS_LIGHT_MESSAGE(type) ? CLUSTERMSG_LIGHT_MIN_LEN : CLUSTERMSG_MIN_LEN;
    if (totlen < minlen) return 0;
    return 1;
}

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. */
void clusterReadHandler(connection *conn) {
    clusterMsg buf[1];
    ssize_t nread;
    clusterMsg *hdr;
    clusterLink *link = connGetPrivateData(conn);
    unsigned int readlen, rcvbuflen;

    while (1) { /* Read as long as there is data to read. */
        rcvbuflen = link->rcvbuf_len;
        if (rcvbuflen < RCVBUF_MIN_READ_LEN) {
            /* First, obtain the first 16 bytes to get the full message
             * length and type. */
            readlen = RCVBUF_MIN_READ_LEN - rcvbuflen;
        } else {
            /* Finally read the full message. */
            hdr = (clusterMsg *)link->rcvbuf;
            if (rcvbuflen == RCVBUF_MIN_READ_LEN) {
                /* Perform some sanity check on the message signature
                 * and length. */
                if (!isClusterMsgSignatureAndLengthValid(hdr)) {
                    char ip[NET_IP_STR_LEN];
                    int port;
                    if (connAddrPeerName(conn, ip, sizeof(ip), &port) == -1) {
                        serverLog(LL_WARNING, "Bad message length or signature received "
                                              "on the Cluster bus.");
                    } else {
                        serverLog(LL_WARNING,
                                  "Bad message length or signature received "
                                  "on the Cluster bus from %s:%d",
                                  ip, port);
                    }
                    handleLinkIOError(link);
                    return;
                }
            }
            readlen = ntohl(hdr->totlen) - rcvbuflen;
            if (readlen > sizeof(buf)) readlen = sizeof(buf);
        }

        nread = connRead(conn, buf, readlen);
        if (nread == -1 && (connGetState(conn) == CONN_STATE_CONNECTED)) return; /* No more data ready. */

        if (nread <= 0) {
            /* I/O error... */
            serverLog(LL_DEBUG, "I/O error reading from node link: %s",
                      (nread == 0) ? "connection closed" : connGetLastError(conn));
            handleLinkIOError(link);
            return;
        } else {
            /* Read data and recast the pointer to the new buffer. */
            size_t unused = link->rcvbuf_alloc - link->rcvbuf_len;
            if ((size_t)nread > unused) {
                size_t required = link->rcvbuf_len + nread;
                size_t prev_rcvbuf_alloc = link->rcvbuf_alloc;
                /* If less than 1mb, grow to twice the needed size, if larger grow by 1mb. */
                link->rcvbuf_alloc = required < RCVBUF_MAX_PREALLOC ? required * 2 : required + RCVBUF_MAX_PREALLOC;
                link->rcvbuf = zrealloc(link->rcvbuf, link->rcvbuf_alloc);
                server.stat_cluster_links_memory += link->rcvbuf_alloc - prev_rcvbuf_alloc;
            }
            memcpy(link->rcvbuf + link->rcvbuf_len, buf, nread);
            link->rcvbuf_len += nread;
            hdr = (clusterMsg *)link->rcvbuf;
            rcvbuflen += nread;
        }

        /* Total length obtained? Process this packet. */
        if (rcvbuflen >= RCVBUF_MIN_READ_LEN && rcvbuflen == ntohl(hdr->totlen)) {
            if (clusterProcessPacket(link)) {
                if (link->rcvbuf_alloc > RCVBUF_INIT_LEN) {
                    size_t prev_rcvbuf_alloc = link->rcvbuf_alloc;
                    zfree(link->rcvbuf);
                    link->rcvbuf = zmalloc(link->rcvbuf_alloc = RCVBUF_INIT_LEN);
                    server.stat_cluster_links_memory += link->rcvbuf_alloc - prev_rcvbuf_alloc;
                }
                link->rcvbuf_len = 0;
            } else {
                return; /* Link no longer valid. */
            }
        }
    }
}

/* Put the message block into the link's send queue.
 *
 * It is guaranteed that this function will never have as a side effect
 * the link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with the same link later. */
void clusterSendMessage(clusterLink *link, clusterMsgSendBlock *msgblock) {
    if (!link) {
        return;
    }
    if (listLength(link->send_msg_queue) == 0 && msgblock->msg.totlen != 0)
        connSetWriteHandlerWithBarrier(link->conn, clusterWriteHandler, 1);

    listAddNodeTail(link->send_msg_queue, msgblock);
    msgblock->refcount++;

    /* Update memory tracking */
    link->send_msg_queue_mem += sizeof(listNode) + msgblock->totlen;
    server.stat_cluster_links_memory += sizeof(listNode);

    /* Populate sent messages stats. */
    uint16_t type = ntohs(msgblock->msg.type);
    if (type < CLUSTERMSG_TYPE_COUNT) server.cluster->stats_bus_messages_sent[type]++;
}

/* Send a message to all the nodes that are part of the cluster having
 * a connected link.
 *
 * It is guaranteed that this function will never have as a side effect
 * some node->link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with node links later. */
void clusterBroadcastMessage(clusterMsgSendBlock *msgblock) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node->flags & (CLUSTER_NODE_MYSELF | CLUSTER_NODE_HANDSHAKE)) continue;
        clusterSendMessage(node->link, msgblock);
    }
    dictReleaseIterator(di);
}

static void clusterBuildMessageHdrLight(clusterMsgLight *hdr, int type, size_t msglen) {
    hdr->ver = htons(CLUSTER_PROTO_VER);
    hdr->sig[0] = 'R';
    hdr->sig[1] = 'C';
    hdr->sig[2] = 'm';
    hdr->sig[3] = 'b';
    hdr->type = htons(type);
    hdr->notused1 = 0;
    hdr->notused2 = 0;
    hdr->totlen = htonl(msglen);
}

/* Build the message header. hdr must point to a buffer at least
 * sizeof(clusterMsg) in bytes. */
static void clusterBuildMessageHdr(clusterMsg *hdr, int type, size_t msglen) {
    uint64_t offset;
    clusterNode *primary;

    /* If this node is a primary, we send its slots bitmap and configEpoch.
     * If this node is a replica we send the primary's information instead (the
     * node is flagged as replica so the receiver knows that it is NOT really
     * in charge for this slots. */
    primary = (nodeIsReplica(myself) && myself->replicaof) ? myself->replicaof : myself;

    hdr->ver = htons(CLUSTER_PROTO_VER);
    hdr->sig[0] = 'R';
    hdr->sig[1] = 'C';
    hdr->sig[2] = 'm';
    hdr->sig[3] = 'b';
    hdr->type = htons(type);
    memcpy(hdr->sender, myself->name, CLUSTER_NAMELEN);

    /* If cluster-announce-ip option is enabled, force the receivers of our
     * packets to use the specified address for this node. Otherwise if the
     * first byte is zero, they'll do auto discovery. */
    memset(hdr->myip, 0, NET_IP_STR_LEN);
    if (server.cluster_announce_ip) {
        valkey_strlcpy(hdr->myip, server.cluster_announce_ip, NET_IP_STR_LEN);
    }

    /* Handle cluster-announce-[tls-|bus-]port. */
    int announced_tcp_port, announced_tls_port, announced_cport;
    deriveAnnouncedPorts(&announced_tcp_port, &announced_tls_port, &announced_cport);

    memcpy(hdr->myslots, primary->slots, sizeof(hdr->myslots));
    memset(hdr->replicaof, 0, CLUSTER_NAMELEN);
    if (myself->replicaof != NULL) memcpy(hdr->replicaof, myself->replicaof->name, CLUSTER_NAMELEN);
    if (server.tls_cluster) {
        hdr->port = htons(announced_tls_port);
        hdr->pport = htons(announced_tcp_port);
    } else {
        hdr->port = htons(announced_tcp_port);
        hdr->pport = htons(announced_tls_port);
    }
    hdr->cport = htons(announced_cport);
    hdr->flags = htons(myself->flags);
    hdr->state = server.cluster->state;

    /* Set the currentEpoch and configEpochs. */
    hdr->currentEpoch = htonu64(server.cluster->currentEpoch);
    hdr->configEpoch = htonu64(primary->configEpoch);

    /* Set the replication offset. */
    if (nodeIsReplica(myself))
        offset = replicationGetReplicaOffset();
    else
        offset = server.primary_repl_offset;
    hdr->offset = htonu64(offset);

    /* Set the message flags. */
    if (clusterNodeIsPrimary(myself) && server.cluster->mf_end) hdr->mflags[0] |= CLUSTERMSG_FLAG0_PAUSED;

    hdr->totlen = htonl(msglen);
}

/* Set the i-th entry of the gossip section in the message pointed by 'hdr'
 * to the info of the specified node 'n'. */
void clusterSetGossipEntry(clusterMsg *hdr, int i, clusterNode *n) {
    clusterMsgDataGossip *gossip;
    gossip = &(hdr->data.ping.gossip[i]);
    memcpy(gossip->nodename, n->name, CLUSTER_NAMELEN);
    gossip->ping_sent = htonl(n->ping_sent / 1000);
    gossip->pong_received = htonl(n->pong_received / 1000);
    memcpy(gossip->ip, n->ip, sizeof(n->ip));
    if (server.tls_cluster) {
        gossip->port = htons(n->tls_port);
        gossip->pport = htons(n->tcp_port);
    } else {
        gossip->port = htons(n->tcp_port);
        gossip->pport = htons(n->tls_port);
    }
    gossip->cport = htons(n->cport);
    gossip->flags = htons(n->flags);
    gossip->notused1 = 0;
}

/* Send a PING or PONG packet to the specified node, making sure to add enough
 * gossip information. */
void clusterSendPing(clusterLink *link, int type) {
    static unsigned long long cluster_pings_sent = 0;
    cluster_pings_sent++;
    int gossipcount = 0; /* Number of gossip sections added so far. */
    int wanted;          /* Number of gossip sections we want to append if possible. */
    int estlen;          /* Upper bound on estimated packet length */
    /* freshnodes is the max number of nodes we can hope to append at all:
     * nodes available minus two (ourself and the node we are sending the
     * message to). However practically there may be less valid nodes since
     * nodes in handshake state, disconnected, are not considered. */
    int freshnodes = dictSize(server.cluster->nodes) - 2;

    /* How many gossip sections we want to add? 1/10 of the number of nodes
     * and anyway at least 3. Why 1/10?
     *
     * If we have N primaries, with N/10 entries, and we consider that in
     * node_timeout we exchange with each other node at least 4 packets
     * (we ping in the worst case in node_timeout/2 time, and we also
     * receive two pings from the host), we have a total of 8 packets
     * in the node_timeout*2 failure reports validity time. So we have
     * that, for a single PFAIL node, we can expect to receive the following
     * number of failure reports (in the specified window of time):
     *
     * PROB * GOSSIP_ENTRIES_PER_PACKET * TOTAL_PACKETS:
     *
     * PROB = probability of being featured in a single gossip entry,
     *        which is 1 / NUM_OF_NODES.
     * ENTRIES = 10.
     * TOTAL_PACKETS = 2 * 4 * NUM_OF_PRIMARIES.
     *
     * If we assume we have just primaries (so num of nodes and num of primaries
     * is the same), with 1/10 we always get over the majority, and specifically
     * 80% of the number of nodes, to account for many primaries failing at the
     * same time.
     *
     * Since we have non-voting replicas that lower the probability of an entry
     * to feature our node, we set the number of entries per packet as
     * 10% of the total nodes we have. */
    wanted = floor(dictSize(server.cluster->nodes) / 10);
    if (wanted < 3) wanted = 3;
    if (wanted > freshnodes) wanted = freshnodes;

    /* Include all the nodes in PFAIL state, so that failure reports are
     * faster to propagate to go from PFAIL to FAIL state. */
    int pfail_wanted = server.cluster->stats_pfail_nodes;

    /* Compute the maximum estlen to allocate our buffer. We'll fix the estlen
     * later according to the number of gossip sections we really were able
     * to put inside the packet. */
    estlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    estlen += (sizeof(clusterMsgDataGossip) * (wanted + pfail_wanted));
    if (link->node && nodeSupportsExtensions(link->node)) {
        estlen += writePingExtensions(NULL, 0);
    }
    /* Note: clusterBuildMessageHdr() expects the buffer to be always at least
     * sizeof(clusterMsg) or more. */
    if (estlen < (int)sizeof(clusterMsg)) estlen = sizeof(clusterMsg);
    clusterMsgSendBlock *msgblock = createClusterMsgSendBlock(type, estlen);
    clusterMsg *hdr = &msgblock->msg;

    if (!link->inbound && type == CLUSTERMSG_TYPE_PING) link->node->ping_sent = mstime();

    /* Populate the gossip fields */
    int maxiterations = wanted * 3;
    while (freshnodes > 0 && gossipcount < wanted && maxiterations--) {
        dictEntry *de = dictGetRandomKey(server.cluster->nodes);
        clusterNode *this = dictGetVal(de);

        /* Don't include this node: the whole packet header is about us
         * already, so we just gossip about other nodes.
         * Also, don't include the receiver. Receiver will not update its state
         * based on gossips about itself. */
        if (this == myself || this == link->node) continue;

        /* PFAIL nodes will be added later. */
        if (this->flags & CLUSTER_NODE_PFAIL) continue;

        /* In the gossip section don't include:
         * 1) Nodes in HANDSHAKE state.
         * 3) Nodes with the NOADDR flag set.
         * 4) Disconnected nodes if they don't have configured slots.
         */
        if (this->flags & (CLUSTER_NODE_HANDSHAKE | CLUSTER_NODE_NOADDR) ||
            (this->link == NULL && this->numslots == 0)) {
            freshnodes--; /* Technically not correct, but saves CPU. */
            continue;
        }

        /* Do not add a node we already have. */
        if (this->last_in_ping_gossip == cluster_pings_sent) continue;

        /* Add it */
        clusterSetGossipEntry(hdr, gossipcount, this);
        this->last_in_ping_gossip = cluster_pings_sent;
        freshnodes--;
        gossipcount++;
    }

    /* If there are PFAIL nodes, add them at the end. */
    if (pfail_wanted) {
        dictIterator *di;
        dictEntry *de;

        di = dictGetSafeIterator(server.cluster->nodes);
        while ((de = dictNext(di)) != NULL && pfail_wanted > 0) {
            clusterNode *node = dictGetVal(de);
            if (node->flags & CLUSTER_NODE_HANDSHAKE) continue;
            if (node->flags & CLUSTER_NODE_NOADDR) continue;
            if (!(node->flags & CLUSTER_NODE_PFAIL)) continue;
            clusterSetGossipEntry(hdr, gossipcount, node);
            gossipcount++;
            /* We take the count of the slots we allocated, since the
             * PFAIL stats may not match perfectly with the current number
             * of PFAIL nodes. */
            pfail_wanted--;
        }
        dictReleaseIterator(di);
    }

    /* Compute the actual total length and send! */
    uint32_t totlen = 0;

    if (link->node && nodeSupportsExtensions(link->node)) {
        totlen += writePingExtensions(hdr, gossipcount);
    } else {
        serverLog(LL_DEBUG, "Unable to send extensions data, however setting ext data flag to true");
        hdr->mflags[0] |= CLUSTERMSG_FLAG0_EXT_DATA;
    }
    totlen += sizeof(clusterMsg) - sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip) * gossipcount);
    serverAssert(gossipcount < USHRT_MAX);
    hdr->count = htons(gossipcount);
    hdr->totlen = htonl(totlen);

    clusterSendMessage(link, msgblock);
    clusterMsgSendBlockDecrRefCount(msgblock);
}

/* Send a PONG packet to every connected node that's not in handshake state
 * and for which we have a valid link.
 *
 * In Cluster mode, pongs are not used just for failure detection, but also
 * to carry important configuration information. So broadcasting a pong is
 * useful when something changes in the configuration and we want to make
 * the cluster aware ASAP (for instance after a replica promotion).
 *
 * The 'target' argument specifies the receiving instances using the
 * defines below:
 *
 * CLUSTER_BROADCAST_ALL -> All known instances.
 * CLUSTER_BROADCAST_LOCAL_REPLICAS -> All replicas in my primary-replicas ring.
 */
#define CLUSTER_BROADCAST_ALL 0
#define CLUSTER_BROADCAST_LOCAL_REPLICAS 1
void clusterBroadcastPong(int target) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (!node->link) continue;
        if (node == myself || nodeInHandshake(node)) continue;
        if (target == CLUSTER_BROADCAST_LOCAL_REPLICAS) {
            int local_replica = nodeIsReplica(node) && node->replicaof &&
                                (node->replicaof == myself || node->replicaof == myself->replicaof);
            if (!local_replica) continue;
        }
        clusterSendPing(node->link, CLUSTERMSG_TYPE_PONG);
    }
    dictReleaseIterator(di);
}

/* Create a PUBLISH message block.
 *
 * Sanitizer suppression: In clusterMsgDataPublish, sizeof(bulk_data) is 8.
 * As all the struct is used as a buffer, when more than 8 bytes are copied into
 * the 'bulk_data', sanitizer generates an out-of-bounds error which is a false
 * positive in this context. */
VALKEY_NO_SANITIZE("bounds")
clusterMsgSendBlock *clusterCreatePublishMsgBlock(robj *channel, robj *message, int is_light, int is_sharded) {
    uint32_t channel_len, message_len;
    uint16_t type = is_sharded ? CLUSTERMSG_TYPE_PUBLISHSHARD : CLUSTERMSG_TYPE_PUBLISH;

    channel = getDecodedObject(channel);
    message = getDecodedObject(message);
    channel_len = sdslen(channel->ptr);
    message_len = sdslen(message->ptr);
    size_t msglen;

    if (is_light) {
        /* We set the MSB for message that needs to sent using light header */
        type |= CLUSTERMSG_LIGHT;
        msglen = sizeof(clusterMsgLight);
    } else {
        msglen = sizeof(clusterMsg);
    }
    msglen -= sizeof(union clusterMsgData);
    msglen += sizeof(clusterMsgDataPublish) - 8 + channel_len + message_len;
    clusterMsgSendBlock *msgblock = createClusterMsgSendBlock(type, msglen);
    clusterMsgDataPublish *hdr_data_msg;
    if (is_light) {
        clusterMsgLight *hdr_light = &msgblock->msg_light;
        hdr_data_msg = &hdr_light->data.publish.msg;
    } else {
        clusterMsg *hdr = &msgblock->msg;
        hdr_data_msg = &hdr->data.publish.msg;
    }
    hdr_data_msg->channel_len = htonl(channel_len);
    hdr_data_msg->message_len = htonl(message_len);
    memcpy(hdr_data_msg->bulk_data, channel->ptr, sdslen(channel->ptr));
    memcpy(hdr_data_msg->bulk_data + sdslen(channel->ptr), message->ptr, sdslen(message->ptr));

    decrRefCount(channel);
    decrRefCount(message);

    return msgblock;
}

/* Send a FAIL message to all the nodes we are able to contact.
 * The FAIL message is sent when we detect that a node is failing
 * (CLUSTER_NODE_PFAIL) and we also receive a gossip confirmation of this:
 * we switch the node state to CLUSTER_NODE_FAIL and ask all the other
 * nodes to do the same ASAP. */
void clusterSendFail(char *nodename) {
    uint32_t msglen = sizeof(clusterMsg) - sizeof(union clusterMsgData) + sizeof(clusterMsgDataFail);
    clusterMsgSendBlock *msgblock = createClusterMsgSendBlock(CLUSTERMSG_TYPE_FAIL, msglen);

    clusterMsg *hdr = &msgblock->msg;
    memcpy(hdr->data.fail.about.nodename, nodename, CLUSTER_NAMELEN);

    clusterBroadcastMessage(msgblock);
    clusterMsgSendBlockDecrRefCount(msgblock);
}

/* Send an UPDATE message to the specified link carrying the specified 'node'
 * slots configuration. The node name, slots bitmap, and configEpoch info
 * are included. */
void clusterSendUpdate(clusterLink *link, clusterNode *node) {
    if (link == NULL) return;

    uint32_t msglen = sizeof(clusterMsg) - sizeof(union clusterMsgData) + sizeof(clusterMsgDataUpdate);
    clusterMsgSendBlock *msgblock = createClusterMsgSendBlock(CLUSTERMSG_TYPE_UPDATE, msglen);

    clusterMsg *hdr = &msgblock->msg;
    memcpy(hdr->data.update.nodecfg.nodename, node->name, CLUSTER_NAMELEN);
    hdr->data.update.nodecfg.configEpoch = htonu64(node->configEpoch);
    memcpy(hdr->data.update.nodecfg.slots, node->slots, sizeof(node->slots));
    for (unsigned int i = 0; i < sizeof(node->slots); i++) {
        /* Don't advertise slots that the node stopped claiming */
        hdr->data.update.nodecfg.slots[i] =
            hdr->data.update.nodecfg.slots[i] & (~server.cluster->owner_not_claiming_slot[i]);
    }

    clusterSendMessage(link, msgblock);
    clusterMsgSendBlockDecrRefCount(msgblock);
}

/* Send a MODULE message.
 *
 * If link is NULL, then the message is broadcasted to the whole cluster. */
void clusterSendModule(clusterLink *link, uint64_t module_id, uint8_t type, const char *payload, uint32_t len) {
    uint32_t msglen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    msglen += sizeof(clusterMsgModule) - 3 + len;
    clusterMsgSendBlock *msgblock = createClusterMsgSendBlock(CLUSTERMSG_TYPE_MODULE, msglen);

    clusterMsg *hdr = &msgblock->msg;
    hdr->data.module.msg.module_id = module_id; /* Already endian adjusted. */
    hdr->data.module.msg.type = type;
    hdr->data.module.msg.len = htonl(len);
    memcpy(hdr->data.module.msg.bulk_data, payload, len);

    if (link)
        clusterSendMessage(link, msgblock);
    else
        clusterBroadcastMessage(msgblock);

    clusterMsgSendBlockDecrRefCount(msgblock);
}

/* This function gets a cluster node ID string as target, the same way the nodes
 * addresses are represented in the modules side, resolves the node, and sends
 * the message. If the target is NULL the message is broadcasted.
 *
 * The function returns C_OK if the target is valid, otherwise C_ERR is
 * returned. */
int clusterSendModuleMessageToTarget(const char *target,
                                     uint64_t module_id,
                                     uint8_t type,
                                     const char *payload,
                                     uint32_t len) {
    clusterNode *node = NULL;

    if (target != NULL) {
        node = clusterLookupNode(target, strlen(target));
        if (node == NULL || node->link == NULL) return C_ERR;
    }

    clusterSendModule(target ? node->link : NULL, module_id, type, payload, len);
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * CLUSTER Pub/Sub support
 *
 * If `sharded` is 0:
 * For now we do very little, just propagating [S]PUBLISH messages across the whole
 * cluster. In the future we'll try to get smarter and avoiding propagating those
 * messages to hosts without receives for a given channel.
 * Otherwise:
 * Publish this message across the slot (primary/replica).
 * -------------------------------------------------------------------------- */
void clusterPropagatePublish(robj *channel, robj *message, int sharded) {
    clusterMsgSendBlock *msgblock, *msgblock_light;
    msgblock_light = clusterCreatePublishMsgBlock(channel, message, 1, sharded);
    /* We will only create msgblock with normal hdr if there are any nodes that do not support light hdr */
    msgblock = NULL;
    ClusterNodeIterator iter;
    if (sharded) {
        clusterNodeIterInitMyShard(&iter);
    } else {
        clusterNodeIterInitAllNodes(&iter);
    }

    clusterNode *node;
    while ((node = clusterNodeIterNext(&iter)) != NULL) {
        if (node->flags & (CLUSTER_NODE_MYSELF | CLUSTER_NODE_HANDSHAKE)) continue;
        if (nodeSupportsLightMsgHdr(node)) {
            clusterSendMessage(node->link, msgblock_light);
        } else {
            if (msgblock == NULL) {
                msgblock = clusterCreatePublishMsgBlock(channel, message, 0, sharded);
            }
            clusterSendMessage(node->link, msgblock);
        }
    }
    clusterNodeIterReset(&iter);
    if (msgblock != NULL) clusterMsgSendBlockDecrRefCount(msgblock);
    clusterMsgSendBlockDecrRefCount(msgblock_light);
}

/* -----------------------------------------------------------------------------
 * REPLICA node specific functions
 * -------------------------------------------------------------------------- */

/* This function sends a FAILOVER_AUTH_REQUEST message to every node in order to
 * see if there is the quorum for this replica instance to failover its failing
 * primary.
 *
 * Note that we send the failover request to everybody, primary and replica nodes,
 * but only the primaries are supposed to reply to our query. */
void clusterRequestFailoverAuth(void) {
    uint32_t msglen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    clusterMsgSendBlock *msgblock = createClusterMsgSendBlock(CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST, msglen);

    clusterMsg *hdr = &msgblock->msg;
    /* If this is a manual failover, set the CLUSTERMSG_FLAG0_FORCEACK bit
     * in the header to communicate the nodes receiving the message that
     * they should authorized the failover even if the primary is working. */
    if (server.cluster->mf_end) hdr->mflags[0] |= CLUSTERMSG_FLAG0_FORCEACK;
    clusterBroadcastMessage(msgblock);
    clusterMsgSendBlockDecrRefCount(msgblock);
}

/* Send a FAILOVER_AUTH_ACK message to the specified node. */
void clusterSendFailoverAuth(clusterNode *node) {
    if (!node->link) return;

    uint32_t msglen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    clusterMsgSendBlock *msgblock = createClusterMsgSendBlock(CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK, msglen);

    clusterSendMessage(node->link, msgblock);
    clusterMsgSendBlockDecrRefCount(msgblock);
}

/* Send a MFSTART message to the specified node. */
void clusterSendMFStart(clusterNode *node) {
    if (!node->link) return;

    uint32_t msglen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
    clusterMsgSendBlock *msgblock = createClusterMsgSendBlock(CLUSTERMSG_TYPE_MFSTART, msglen);

    clusterSendMessage(node->link, msgblock);
    clusterMsgSendBlockDecrRefCount(msgblock);
}

/* Vote for the node asking for our vote if there are the conditions. */
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request) {
    clusterNode *primary = node->replicaof;
    uint64_t requestCurrentEpoch = ntohu64(request->currentEpoch);
    uint64_t requestConfigEpoch = ntohu64(request->configEpoch);
    unsigned char *claimed_slots = request->myslots;
    int force_ack = request->mflags[0] & CLUSTERMSG_FLAG0_FORCEACK;
    int j;

    /* IF we are not a primary serving at least 1 slot, we don't have the
     * right to vote, as the cluster size is the number
     * of primaries serving at least one slot, and quorum is the cluster
     * size + 1 */
    if (!clusterNodeIsVotingPrimary(myself)) return;

    /* Request epoch must be >= our currentEpoch.
     * Note that it is impossible for it to actually be greater since
     * our currentEpoch was updated as a side effect of receiving this
     * request, if the request epoch was greater. */
    if (requestCurrentEpoch < server.cluster->currentEpoch) {
        serverLog(LL_WARNING, "Failover auth denied to %.40s (%s): reqEpoch (%llu) < curEpoch(%llu)", node->name,
                  node->human_nodename, (unsigned long long)requestCurrentEpoch,
                  (unsigned long long)server.cluster->currentEpoch);
        return;
    }

    /* I already voted for this epoch? Return ASAP. */
    if (server.cluster->lastVoteEpoch == server.cluster->currentEpoch) {
        serverLog(LL_WARNING, "Failover auth denied to %.40s (%s): already voted for epoch %llu", node->name,
                  node->human_nodename, (unsigned long long)server.cluster->currentEpoch);
        return;
    }

    /* Node must be a replica and its primary down.
     * The primary can be non failing if the request is flagged
     * with CLUSTERMSG_FLAG0_FORCEACK (manual failover). */
    if (clusterNodeIsPrimary(node) || primary == NULL || (!nodeFailed(primary) && !force_ack)) {
        if (clusterNodeIsPrimary(node)) {
            serverLog(LL_WARNING, "Failover auth denied to %.40s (%s) for epoch %llu: it is a primary node", node->name,
                      node->human_nodename, (unsigned long long)requestCurrentEpoch);
        } else if (primary == NULL) {
            serverLog(LL_WARNING, "Failover auth denied to %.40s (%s) for epoch %llu: I don't know its primary",
                      node->name, node->human_nodename, (unsigned long long)requestCurrentEpoch);
        } else if (!nodeFailed(primary)) {
            serverLog(LL_WARNING, "Failover auth denied to %.40s (%s) for epoch %llu: its primary is up", node->name,
                      node->human_nodename, (unsigned long long)requestCurrentEpoch);
        }
        return;
    }

    /* We did not voted for a replica about this primary for two
     * times the node timeout. This is not strictly needed for correctness
     * of the algorithm but makes the base case more linear. */
    if (mstime() - node->replicaof->voted_time < server.cluster_node_timeout * 2) {
        serverLog(LL_WARNING,
                  "Failover auth denied to %.40s %s: "
                  "can't vote about this primary before %lld milliseconds",
                  node->name, node->human_nodename,
                  (long long)((server.cluster_node_timeout * 2) - (mstime() - node->replicaof->voted_time)));
        return;
    }

    /* The replica requesting the vote must have a configEpoch for the claimed
     * slots that is >= the one of the primaries currently serving the same
     * slots in the current configuration. */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (bitmapTestBit(claimed_slots, j) == 0) continue;
        if (isSlotUnclaimed(j) || server.cluster->slots[j]->configEpoch <= requestConfigEpoch) {
            continue;
        }
        /* If we reached this point we found a slot that in our current slots
         * is served by a primary with a greater configEpoch than the one claimed
         * by the replica requesting our vote. Refuse to vote for this replica. */
        serverLog(LL_WARNING,
                  "Failover auth denied to %.40s (%s): "
                  "slot %d epoch (%llu) > reqEpoch (%llu)",
                  node->name, node->human_nodename, j, (unsigned long long)server.cluster->slots[j]->configEpoch,
                  (unsigned long long)requestConfigEpoch);
        return;
    }

    /* We can vote for this replica. */
    server.cluster->lastVoteEpoch = server.cluster->currentEpoch;
    node->replicaof->voted_time = mstime();
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_FSYNC_CONFIG);
    clusterSendFailoverAuth(node);
    serverLog(LL_NOTICE, "Failover auth granted to %.40s (%s) for epoch %llu", node->name, node->human_nodename,
              (unsigned long long)server.cluster->currentEpoch);
}

/* This function returns the "rank" of this instance, a replica, in the context
 * of its primary-replicas ring. The rank of the replica is given by the number of
 * other replicas for the same primary that have a better replication offset
 * compared to the local one (better means, greater, so they claim more data).
 *
 * A replica with rank 0 is the one with the greatest (most up to date)
 * replication offset, and so forth. Note that because how the rank is computed
 * multiple replicas may have the same rank, in case they have the same offset.
 *
 * If the replication offsets are the same, the one with the lexicographically
 * smaller node id will have a lower rank to avoid simultaneous elections
 * of replicas.
 *
 * The replica rank is used to add a delay to start an election in order to
 * get voted and replace a failing primary. Replicas with better replication
 * offsets are more likely to win. */
int clusterGetReplicaRank(void) {
    long long myoffset;
    int j, rank = 0;
    clusterNode *primary;

    serverAssert(nodeIsReplica(myself));
    primary = myself->replicaof;
    if (primary == NULL) return 0; /* Never called by replicas without primary. */

    myoffset = replicationGetReplicaOffset();
    for (j = 0; j < primary->num_replicas; j++) {
        if (primary->replicas[j] == myself) continue;
        if (nodeCantFailover(primary->replicas[j])) continue;

        if (primary->replicas[j]->repl_offset > myoffset) {
            rank++;
        } else if (primary->replicas[j]->repl_offset == myoffset &&
                   memcmp(primary->replicas[j]->name, myself->name, CLUSTER_NAMELEN) < 0) {
            rank++;
        }
    }
    return rank;
}

/* This function is called by clusterHandleReplicaFailover() in order to
 * let the replica log why it is not able to failover. Sometimes there are
 * not the conditions, but since the failover function is called again and
 * again, we can't log the same things continuously.
 *
 * This function works by logging only if a given set of conditions are
 * true:
 *
 * 1) The reason for which the failover can't be initiated changed.
 *    The reasons also include a NONE reason we reset the state to
 *    when the replica finds that its primary is fine (no FAIL flag).
 * 2) Also, the log is emitted again if the primary is still down and
 *    the reason for not failing over is still the same, but more than
 *    CLUSTER_CANT_FAILOVER_RELOG_PERIOD seconds elapsed.
 *
 * The function is called with the reason why the replica can't failover
 * which is one of the integer macros CLUSTER_CANT_FAILOVER_*.
 *
 * The function is guaranteed to be called only if 'myself' is a replica. */
void clusterLogCantFailover(int reason) {
    char *msg;
    static time_t lastlog_time = 0;

    /* Don't log if we have the same reason for some time. */
    if (reason == server.cluster->cant_failover_reason &&
        time(NULL) - lastlog_time < CLUSTER_CANT_FAILOVER_RELOG_PERIOD)
        return;

    server.cluster->cant_failover_reason = reason;

    switch (reason) {
    case CLUSTER_CANT_FAILOVER_DATA_AGE:
        msg = "Disconnected from primary for longer than allowed. "
              "Please check the 'cluster-replica-validity-factor' configuration "
              "option.";
        break;
    case CLUSTER_CANT_FAILOVER_WAITING_DELAY: msg = "Waiting the delay before I can start a new failover."; break;
    case CLUSTER_CANT_FAILOVER_EXPIRED: msg = "Failover attempt expired."; break;
    case CLUSTER_CANT_FAILOVER_WAITING_VOTES: msg = "Waiting for votes, but majority still not reached."; break;
    default: msg = "Unknown reason code."; break;
    }
    lastlog_time = time(NULL);
    serverLog(LL_NOTICE, "Currently unable to failover: %s", msg);

    int cur_vote = server.cluster->failover_auth_count;
    int cur_quorum = (server.cluster->size / 2) + 1;
    /* Emits a log when an election is in progress and waiting for votes or when the failover attempt expired. */
    if (reason == CLUSTER_CANT_FAILOVER_WAITING_VOTES || reason == CLUSTER_CANT_FAILOVER_EXPIRED) {
        serverLog(LL_NOTICE, "Needed quorum: %d. Number of votes received so far: %d", cur_quorum, cur_vote);
    }
}

/* This function implements the final part of automatic and manual failovers,
 * where the replica grabs its primary's hash slots, and propagates the new
 * configuration.
 *
 * Note that it's up to the caller to be sure that the node got a new
 * configuration epoch already. */
void clusterFailoverReplaceYourPrimary(void) {
    int j;
    clusterNode *old_primary = myself->replicaof;

    if (clusterNodeIsPrimary(myself) || old_primary == NULL) return;

    serverLog(LL_NOTICE, "Setting myself to primary in shard %.40s after failover; my old primary is %.40s (%s)",
              myself->shard_id, old_primary->name, old_primary->human_nodename);

    /* 1) Turn this node into a primary. */
    clusterSetNodeAsPrimary(myself);
    replicationUnsetPrimary();

    /* 2) Claim all the slots assigned to our primary. */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (clusterNodeCoversSlot(old_primary, j)) {
            clusterDelSlot(j);
            clusterAddSlot(myself, j);
        }
    }

    /* 3) Update state and save config. */
    clusterUpdateState();
    clusterSaveConfigOrDie(1);

    /* 4) Pong all the other nodes so that they can update the state
     *    accordingly and detect that we switched to primary role. */
    clusterBroadcastPong(CLUSTER_BROADCAST_ALL);

    /* 5) If there was a manual failover in progress, clear the state. */
    resetManualFailover();
}

/* This function is called if we are a replica node and our primary serving
 * a non-zero amount of hash slots is in FAIL state.
 *
 * The goal of this function is:
 * 1) To check if we are able to perform a failover, is our data updated?
 * 2) Try to get elected by primaries.
 * 3) Perform the failover informing all the other nodes.
 */
void clusterHandleReplicaFailover(void) {
    mstime_t data_age;
    mstime_t auth_age = mstime() - server.cluster->failover_auth_time;
    int needed_quorum = (server.cluster->size / 2) + 1;
    int manual_failover = server.cluster->mf_end != 0 && server.cluster->mf_can_start;
    mstime_t auth_timeout, auth_retry_time;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_HANDLE_FAILOVER;

    /* Compute the failover timeout (the max time we have to send votes
     * and wait for replies), and the failover retry time (the time to wait
     * before trying to get voted again).
     *
     * Timeout is MAX(NODE_TIMEOUT*2,2000) milliseconds.
     * Retry is two times the Timeout.
     */
    auth_timeout = server.cluster_node_timeout * 2;
    if (auth_timeout < CLUSTER_OPERATION_TIMEOUT) auth_timeout = CLUSTER_OPERATION_TIMEOUT;
    auth_retry_time = auth_timeout * 2;

    /* Pre conditions to run the function, that must be met both in case
     * of an automatic or manual failover:
     * 1) We are a replica.
     * 2) Our primary is flagged as FAIL, or this is a manual failover.
     * 3) We don't have the no failover configuration set, and this is
     *    not a manual failover. */
    if (clusterNodeIsPrimary(myself) || myself->replicaof == NULL ||
        (!nodeFailed(myself->replicaof) && !manual_failover) ||
        (server.cluster_replica_no_failover && !manual_failover)) {
        /* There are no reasons to failover, so we set the reason why we
         * are returning without failing over to NONE. */
        server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
        return;
    }

    /* Set data_age to the number of milliseconds we are disconnected from
     * the primary. */
    if (server.repl_state == REPL_STATE_CONNECTED) {
        data_age = (mstime_t)(server.unixtime - server.primary->last_interaction) * 1000;
    } else {
        data_age = (mstime_t)(server.unixtime - server.repl_down_since) * 1000;
    }

    /* Remove the node timeout from the data age as it is fine that we are
     * disconnected from our primary at least for the time it was down to be
     * flagged as FAIL, that's the baseline. */
    if (data_age > server.cluster_node_timeout) data_age -= server.cluster_node_timeout;

    /* Check if our data is recent enough according to the replica validity
     * factor configured by the user.
     *
     * Check bypassed for manual failovers. */
    if (server.cluster_replica_validity_factor &&
        data_age > (((mstime_t)server.repl_ping_replica_period * 1000) +
                    (server.cluster_node_timeout * server.cluster_replica_validity_factor))) {
        if (!manual_failover) {
            clusterLogCantFailover(CLUSTER_CANT_FAILOVER_DATA_AGE);
            return;
        }
    }

    /* If the previous failover attempt timeout and the retry time has
     * elapsed, we can setup a new one. */
    if (auth_age > auth_retry_time) {
        server.cluster->failover_auth_time = mstime() +
                                             500 +           /* Fixed delay of 500 milliseconds, let FAIL msg propagate. */
                                             random() % 500; /* Random delay between 0 and 500 milliseconds. */
        server.cluster->failover_auth_count = 0;
        server.cluster->failover_auth_sent = 0;
        server.cluster->failover_auth_rank = clusterGetReplicaRank();
        /* We add another delay that is proportional to the replica rank.
         * Specifically 1 second * rank. This way replicas that have a probably
         * less updated replication offset, are penalized. */
        server.cluster->failover_auth_time += server.cluster->failover_auth_rank * 1000;
        /* However if this is a manual failover, no delay is needed. */
        if (server.cluster->mf_end) {
            server.cluster->failover_auth_time = mstime();
            server.cluster->failover_auth_rank = 0;
            clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        }
        serverLog(LL_NOTICE,
                  "Start of election delayed for %lld milliseconds "
                  "(rank #%d, offset %lld).",
                  server.cluster->failover_auth_time - mstime(), server.cluster->failover_auth_rank,
                  replicationGetReplicaOffset());
        /* Now that we have a scheduled election, broadcast our offset
         * to all the other replicas so that they'll updated their offsets
         * if our offset is better. */
        clusterBroadcastPong(CLUSTER_BROADCAST_LOCAL_REPLICAS);
        return;
    }

    /* It is possible that we received more updated offsets from other
     * replicas for the same primary since we computed our election delay.
     * Update the delay if our rank changed.
     *
     * Not performed if this is a manual failover. */
    if (server.cluster->failover_auth_sent == 0 && server.cluster->mf_end == 0) {
        int newrank = clusterGetReplicaRank();
        if (newrank != server.cluster->failover_auth_rank) {
            long long added_delay = (newrank - server.cluster->failover_auth_rank) * 1000;
            server.cluster->failover_auth_time += added_delay;
            server.cluster->failover_auth_rank = newrank;
            serverLog(LL_NOTICE, "Replica rank updated to #%d, added %lld milliseconds of delay.", newrank,
                      added_delay);
        }
    }

    /* Return ASAP if we can't still start the election. */
    if (mstime() < server.cluster->failover_auth_time) {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_DELAY);
        return;
    }

    /* Return ASAP if the election is too old to be valid. */
    if (auth_age > auth_timeout) {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_EXPIRED);
        return;
    }

    /* Ask for votes if needed. */
    if (server.cluster->failover_auth_sent == 0) {
        server.cluster->currentEpoch++;
        server.cluster->failover_auth_epoch = server.cluster->currentEpoch;
        serverLog(LL_NOTICE, "Starting a failover election for epoch %llu.",
                  (unsigned long long)server.cluster->currentEpoch);
        clusterRequestFailoverAuth();
        server.cluster->failover_auth_sent = 1;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_FSYNC_CONFIG);
        return; /* Wait for replies. */
    }

    /* Check if we reached the quorum. */
    if (server.cluster->failover_auth_count >= needed_quorum) {
        /* We have the quorum, we can finally failover the primary. */

        serverLog(LL_NOTICE, "Failover election won: I'm the new primary.");

        /* Update my configEpoch to the epoch of the election. */
        if (myself->configEpoch < server.cluster->failover_auth_epoch) {
            myself->configEpoch = server.cluster->failover_auth_epoch;
            serverLog(LL_NOTICE, "configEpoch set to %llu after successful failover",
                      (unsigned long long)myself->configEpoch);
        }

        /* Take responsibility for the cluster slots. */
        clusterFailoverReplaceYourPrimary();
    } else {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_VOTES);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER replica migration
 *
 * Replica migration is the process that allows a replica of a primary that is
 * already covered by at least another replica, to "migrate" to a primary that
 * is orphaned, that is, left with no working replicas.
 * ------------------------------------------------------------------------- */

/* This function is responsible to decide if this replica should be migrated
 * to a different (orphaned) primary. It is called by the clusterCron() function
 * only if:
 *
 * 1) We are a replica node.
 * 2) It was detected that there is at least one orphaned primary in
 *    the cluster.
 * 3) We are a replica of one of the primaries with the greatest number of
 *    replicas.
 *
 * This checks are performed by the caller since it requires to iterate
 * the nodes anyway, so we spend time into clusterHandleReplicaMigration()
 * if definitely needed.
 *
 * The function is called with a pre-computed max_replicas, that is the max
 * number of working (not in FAIL state) replicas for a single primary.
 *
 * Additional conditions for migration are examined inside the function.
 */
void clusterHandleReplicaMigration(int max_replicas) {
    int j, ok_replicas = 0;
    clusterNode *my_primary = myself->replicaof, *target = NULL, *candidate = NULL;
    dictIterator *di;
    dictEntry *de;

    /* Step 1: Don't migrate if the cluster state is not ok. */
    if (server.cluster->state != CLUSTER_OK) return;

    /* Step 2: Don't migrate if my primary will not be left with at least
     *         'migration-barrier' replicas after my migration. */
    if (my_primary == NULL) return;
    for (j = 0; j < my_primary->num_replicas; j++)
        if (!nodeFailed(my_primary->replicas[j]) && !nodeTimedOut(my_primary->replicas[j])) ok_replicas++;
    if (ok_replicas <= server.cluster_migration_barrier) return;

    /* Step 3: Identify a candidate for migration, and check if among the
     * primaries with the greatest number of ok replicas, I'm the one with the
     * smallest node ID (the "candidate replica").
     *
     * Note: this means that eventually a replica migration will occur
     * since replicas that are reachable again always have their FAIL flag
     * cleared, so eventually there must be a candidate.
     * There is a possible race condition causing multiple
     * replicas to migrate at the same time, but this is unlikely to
     * happen and relatively harmless when it does. */
    candidate = myself;
    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        int ok_replicas = 0, is_orphaned = 1;

        /* We want to migrate only if this primary is working, orphaned, and
         * used to have replicas or if failed over a primary that had replicas
         * (MIGRATE_TO flag). This way we only migrate to instances that were
         * supposed to have replicas. */
        if (nodeIsReplica(node) || nodeFailed(node)) is_orphaned = 0;
        if (!(node->flags & CLUSTER_NODE_MIGRATE_TO)) is_orphaned = 0;

        /* Check number of working replicas. */
        if (clusterNodeIsPrimary(node)) ok_replicas = clusterCountNonFailingReplicas(node);
        if (ok_replicas > 0) is_orphaned = 0;

        if (is_orphaned) {
            if (!target && node->numslots > 0) target = node;

            /* Track the starting time of the orphaned condition for this
             * primary. */
            if (!node->orphaned_time) node->orphaned_time = mstime();
        } else {
            node->orphaned_time = 0;
        }

        /* Check if I'm the replica candidate for the migration: attached
         * to a primary with the maximum number of replicas and with the smallest
         * node ID. */
        if (ok_replicas == max_replicas) {
            for (j = 0; j < node->num_replicas; j++) {
                if (memcmp(node->replicas[j]->name, candidate->name, CLUSTER_NAMELEN) < 0) {
                    candidate = node->replicas[j];
                }
            }
        }
    }
    dictReleaseIterator(di);

    /* Step 4: perform the migration if there is a target, and if I'm the
     * candidate, but only if the primary is continuously orphaned for a
     * couple of seconds, so that during failovers, we give some time to
     * the natural replicas of this instance to advertise their switch from
     * the old primary to the new one. */
    if (target && candidate == myself && (mstime() - target->orphaned_time) > CLUSTER_REPLICA_MIGRATION_DELAY &&
        !(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER)) {
        serverLog(LL_NOTICE, "Migrating to orphaned primary %.40s (%s) in shard %.40s", target->name,
                  target->human_nodename, target->shard_id);
        /* We are migrating to a different shard that has a completely different
         * replication history, so a full sync is required. */
        clusterSetPrimary(target, 1, 1);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER manual failover
 *
 * This are the important steps performed by replicas during a manual failover:
 * 1) User send CLUSTER FAILOVER command. The failover state is initialized
 *    setting mf_end to the millisecond unix time at which we'll abort the
 *    attempt.
 * 2) Replica sends a MFSTART message to the primary requesting to pause clients
 *    for two times the manual failover timeout CLUSTER_MF_TIMEOUT.
 *    When primary is paused for manual failover, it also starts to flag
 *    packets with CLUSTERMSG_FLAG0_PAUSED.
 * 3) Replica waits for primary to send its replication offset flagged as PAUSED.
 * 4) If replica received the offset from the primary, and its offset matches,
 *    mf_can_start is set to 1, and clusterHandleReplicaFailover() will perform
 *    the failover as usually, with the difference that the vote request
 *    will be modified to force primaries to vote for a replica that has a
 *    working primary.
 *
 * From the point of view of the primary things are simpler: when a
 * PAUSE_CLIENTS packet is received the primary sets mf_end as well and
 * the sender in mf_replica. During the time limit for the manual failover
 * the primary will just send PINGs more often to this replica, flagged with
 * the PAUSED flag, so that the replica will set mf_primary_offset when receiving
 * a packet from the primary with this flag set.
 *
 * The goal of the manual failover is to perform a fast failover without
 * data loss due to the asynchronous primary-replica replication.
 * -------------------------------------------------------------------------- */

/* Reset the manual failover state. This works for both primaries and replicas
 * as all the state about manual failover is cleared.
 *
 * The function can be used both to initialize the manual failover state at
 * startup or to abort a manual failover in progress. */
void resetManualFailover(void) {
    if (server.cluster->mf_replica) {
        /* We were a primary failing over, so we paused clients and related actions.
         * Regardless of the outcome we unpause now to allow traffic again. */
        unpauseActions(PAUSE_DURING_FAILOVER);
    }
    server.cluster->mf_end = 0; /* No manual failover in progress. */
    server.cluster->mf_can_start = 0;
    server.cluster->mf_replica = NULL;
    server.cluster->mf_primary_offset = -1;
}

/* If a manual failover timed out, abort it. */
void manualFailoverCheckTimeout(void) {
    if (server.cluster->mf_end && server.cluster->mf_end < mstime()) {
        serverLog(LL_WARNING, "Manual failover timed out.");
        resetManualFailover();
    }
}

/* This function is called from the cluster cron function in order to go
 * forward with a manual failover state machine. */
void clusterHandleManualFailover(void) {
    /* Return ASAP if no manual failover is in progress. */
    if (server.cluster->mf_end == 0) return;

    /* If mf_can_start is non-zero, the failover was already triggered so the
     * next steps are performed by clusterHandleReplicaFailover(). */
    if (server.cluster->mf_can_start) return;

    if (server.cluster->mf_primary_offset == -1) return; /* Wait for offset... */

    if (server.cluster->mf_primary_offset == replicationGetReplicaOffset()) {
        /* Our replication offset matches the primary replication offset
         * announced after clients were paused. We can start the failover. */
        server.cluster->mf_can_start = 1;
        serverLog(LL_NOTICE, "All primary replication stream processed, "
                             "manual failover can start.");
        clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        return;
    }
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_MANUALFAILOVER);
}

/* -----------------------------------------------------------------------------
 * CLUSTER cron job
 * -------------------------------------------------------------------------- */

/* Check if the node is disconnected and re-establish the connection.
 * Also update a few stats while we are here, that can be used to make
 * better decisions in other part of the code. */
static int clusterNodeCronHandleReconnect(clusterNode *node, mstime_t handshake_timeout, mstime_t now) {
    /* Not interested in reconnecting the link with myself or nodes
     * for which we have no address. */
    if (node->flags & (CLUSTER_NODE_MYSELF | CLUSTER_NODE_NOADDR)) return 1;

    if (node->flags & CLUSTER_NODE_PFAIL) server.cluster->stats_pfail_nodes++;

    /* A Node in HANDSHAKE state has a limited lifespan equal to the
     * configured node timeout. */
    if (nodeInHandshake(node) && now - node->ctime > handshake_timeout) {
        clusterDelNode(node);
        return 1;
    }

    if (node->link == NULL) {
        clusterLink *link = createClusterLink(node);
        link->conn = connCreate(connTypeOfCluster());
        connSetPrivateData(link->conn, link);
        if (connConnect(link->conn, node->ip, node->cport, server.bind_source_addr, clusterLinkConnectHandler) ==
            C_ERR) {
            /* We got a synchronous error from connect before
             * clusterSendPing() had a chance to be called.
             * If node->ping_sent is zero, failure detection can't work,
             * so we claim we actually sent a ping now (that will
             * be really sent as soon as the link is obtained). */
            if (node->ping_sent == 0) node->ping_sent = mstime();
            serverLog(LL_DEBUG,
                      "Unable to connect to "
                      "Cluster Node [%s]:%d -> %s",
                      node->ip, node->cport, server.neterr);

            freeClusterLink(link);
            return 0;
        }
    }
    return 0;
}

static void freeClusterLinkOnBufferLimitReached(clusterLink *link) {
    if (link == NULL || server.cluster_link_msg_queue_limit_bytes == 0) {
        return;
    }

    unsigned long long mem_link = link->send_msg_queue_mem;
    if (mem_link > server.cluster_link_msg_queue_limit_bytes) {
        serverLog(LL_WARNING,
                  "Freeing cluster link(%s node %.40s, used memory: %llu) due to "
                  "exceeding send buffer memory limit.",
                  link->inbound ? "from" : "to", link->node ? link->node->name : "", mem_link);
        freeClusterLink(link);
        server.cluster->stat_cluster_links_buffer_limit_exceeded++;
    }
}

/* Free outbound link to a node if its send buffer size exceeded limit. */
static void clusterNodeCronFreeLinkOnBufferLimitReached(clusterNode *node) {
    freeClusterLinkOnBufferLimitReached(node->link);
    freeClusterLinkOnBufferLimitReached(node->inbound_link);
}

/* This is executed 10 times every second */
void clusterCron(void) {
    dictIterator *di;
    dictEntry *de;
    int update_state = 0;
    int orphaned_primaries; /* How many primaries there are without ok replicas. */
    int max_replicas;       /* Max number of ok replicas for a single primary. */
    int this_replicas;      /* Number of ok replicas for our primary (if we are replica). */
    mstime_t min_pong = 0, now = mstime();
    clusterNode *min_pong_node = NULL;
    static unsigned long long iteration = 0;
    mstime_t handshake_timeout;

    iteration++; /* Number of times this function was called so far. */

    clusterUpdateMyselfHostname();

    /* The handshake timeout is the time after which a handshake node that was
     * not turned into a normal node is removed from the nodes. Usually it is
     * just the NODE_TIMEOUT value, but when NODE_TIMEOUT is too small we use
     * the value of 1 second. */
    handshake_timeout = server.cluster_node_timeout;
    if (handshake_timeout < 1000) handshake_timeout = 1000;

    /* Clear so clusterNodeCronHandleReconnect can count the number of nodes in PFAIL. */
    server.cluster->stats_pfail_nodes = 0;
    /* Run through some of the operations we want to do on each cluster node. */
    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        /* We free the inbound or outboud link to the node if the link has an
         * oversized message send queue and immediately try reconnecting. */
        clusterNodeCronFreeLinkOnBufferLimitReached(node);
        /* The protocol is that function(s) below return non-zero if the node was
         * terminated.
         */
        if (clusterNodeCronHandleReconnect(node, handshake_timeout, now)) continue;
    }
    dictReleaseIterator(di);

    /* Ping some random node 1 time every 10 iterations, so that we usually ping
     * one random node every second. */
    if (!(iteration % 10)) {
        int j;

        /* Check a few random nodes and ping the one with the oldest
         * pong_received time. */
        for (j = 0; j < 5; j++) {
            de = dictGetRandomKey(server.cluster->nodes);
            clusterNode *this = dictGetVal(de);

            /* Don't ping nodes disconnected or with a ping currently active. */
            if (this->link == NULL || this->ping_sent != 0) continue;
            if (this->flags & (CLUSTER_NODE_MYSELF | CLUSTER_NODE_HANDSHAKE)) continue;
            if (min_pong_node == NULL || min_pong > this->pong_received) {
                min_pong_node = this;
                min_pong = this->pong_received;
            }
        }
        if (min_pong_node) {
            serverLog(LL_DEBUG, "Pinging node %.40s", min_pong_node->name);
            clusterSendPing(min_pong_node->link, CLUSTERMSG_TYPE_PING);
        }
    }

    /* Iterate nodes to check if we need to flag something as failing.
     * This loop is also responsible to:
     * 1) Check if there are orphaned primaries (primaries without non failing
     *    replicas).
     * 2) Count the max number of non failing replicas for a single primary.
     * 3) Count the number of replicas for our primary, if we are a replica. */
    orphaned_primaries = 0;
    max_replicas = 0;
    this_replicas = 0;
    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        now = mstime(); /* Use an updated time at every iteration. */

        if (node->flags & (CLUSTER_NODE_MYSELF | CLUSTER_NODE_NOADDR | CLUSTER_NODE_HANDSHAKE)) continue;

        /* Orphaned primary check, useful only if the current instance
         * is a replica that may migrate to another primary. */
        if (nodeIsReplica(myself) && clusterNodeIsPrimary(node) && !nodeFailed(node)) {
            int ok_replicas = clusterCountNonFailingReplicas(node);

            /* A primary is orphaned if it is serving a non-zero number of
             * slots, have no working replicas, but used to have at least one
             * replica, or failed over a primary that used to have replicas. */
            if (ok_replicas == 0 && node->numslots > 0 && node->flags & CLUSTER_NODE_MIGRATE_TO) {
                orphaned_primaries++;
            }
            if (ok_replicas > max_replicas) max_replicas = ok_replicas;
            if (myself->replicaof == node) this_replicas = ok_replicas;
        }

        /* If we are not receiving any data for more than half the cluster
         * timeout, reconnect the link: maybe there is a connection
         * issue even if the node is alive. */
        mstime_t ping_delay = now - node->ping_sent;
        mstime_t data_delay = now - node->data_received;
        if (node->link &&                                            /* is connected */
            now - node->link->ctime > server.cluster_node_timeout && /* was not already reconnected */
            node->ping_sent &&                                       /* we already sent a ping */
            /* and we are waiting for the pong more than timeout/2 */
            ping_delay > server.cluster_node_timeout / 2 &&
            /* and in such interval we are not seeing any traffic at all. */
            data_delay > server.cluster_node_timeout / 2) {
            /* Disconnect the link, it will be reconnected automatically. */
            freeClusterLink(node->link);
        }

        /* If we have currently no active ping in this instance, and the
         * received PONG is older than half the cluster timeout, send
         * a new ping now, to ensure all the nodes are pinged without
         * a too big delay. */
        mstime_t ping_interval =
            server.cluster_ping_interval ? server.cluster_ping_interval : server.cluster_node_timeout / 2;
        if (node->link && node->ping_sent == 0 && (now - node->pong_received) > ping_interval) {
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* If we are a primary and one of the replicas requested a manual
         * failover, ping it continuously. */
        if (server.cluster->mf_end && clusterNodeIsPrimary(myself) && server.cluster->mf_replica == node &&
            node->link) {
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* Check only if we have an active ping for this instance. */
        if (node->ping_sent == 0) continue;

        /* Check if this node looks unreachable.
         * Note that if we already received the PONG, then node->ping_sent
         * is zero, so can't reach this code at all, so we don't risk of
         * checking for a PONG delay if we didn't sent the PING.
         *
         * We also consider every incoming data as proof of liveness, since
         * our cluster bus link is also used for data: under heavy data
         * load pong delays are possible. */
        mstime_t node_delay = (ping_delay < data_delay) ? ping_delay : data_delay;

        if (node_delay > server.cluster_node_timeout) {
            /* Timeout reached. Set the node as possibly failing if it is
             * not already in this state. */
            if (!(node->flags & (CLUSTER_NODE_PFAIL | CLUSTER_NODE_FAIL))) {
                node->flags |= CLUSTER_NODE_PFAIL;
                update_state = 1;
                if (server.cluster->size == 1 && clusterNodeIsVotingPrimary(myself)) {
                    markNodeAsFailingIfNeeded(node);
                } else {
                    serverLog(LL_NOTICE, "NODE %.40s (%s) possibly failing.", node->name, node->human_nodename);
                }
            }
        }
    }
    dictReleaseIterator(di);

    /* If we are a replica node but the replication is still turned off,
     * enable it if we know the address of our primary and it appears to
     * be up. */
    if (nodeIsReplica(myself) && server.primary_host == NULL && myself->replicaof && nodeHasAddr(myself->replicaof)) {
        replicationSetPrimary(myself->replicaof->ip, getNodeDefaultReplicationPort(myself->replicaof), 0);
    }

    /* Abort a manual failover if the timeout is reached. */
    manualFailoverCheckTimeout();

    if (nodeIsReplica(myself)) {
        clusterHandleManualFailover();
        if (!(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER)) clusterHandleReplicaFailover();
        /* If there are orphaned replicas, and we are a replica among the primaries
         * with the max number of non-failing replicas, consider migrating to
         * the orphaned primaries. Note that it does not make sense to try
         * a migration if there is no primary with at least *two* working
         * replicas. */
        if (orphaned_primaries && max_replicas >= 2 && this_replicas == max_replicas &&
            server.cluster_allow_replica_migration)
            clusterHandleReplicaMigration(max_replicas);
    }

    if (update_state || server.cluster->state == CLUSTER_FAIL) clusterUpdateState();
}

/* This function is called before the event handler returns to sleep for
 * events. It is useful to perform operations that must be done ASAP in
 * reaction to events fired but that are not safe to perform inside event
 * handlers, or to perform potentially expansive tasks that we need to do
 * a single time before replying to clients. */
void clusterBeforeSleep(void) {
    int flags = server.cluster->todo_before_sleep;

    /* Reset our flags (not strictly needed since every single function
     * called for flags set should be able to clear its flag). */
    server.cluster->todo_before_sleep = 0;

    if (flags & CLUSTER_TODO_HANDLE_MANUALFAILOVER) {
        /* Handle manual failover as soon as possible so that won't have a 100ms
         * as it was handled only in clusterCron */
        if (nodeIsReplica(myself)) {
            clusterHandleManualFailover();
            if (!(server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_FAILOVER)) clusterHandleReplicaFailover();
        }
    } else if (flags & CLUSTER_TODO_HANDLE_FAILOVER) {
        /* Handle failover, this is needed when it is likely that there is already
         * the quorum from primaries in order to react fast. */
        clusterHandleReplicaFailover();
    }

    /* Update the cluster state. */
    if (flags & CLUSTER_TODO_UPDATE_STATE) clusterUpdateState();

    /* Save the config, possibly using fsync. */
    if (flags & CLUSTER_TODO_SAVE_CONFIG) {
        int fsync = flags & CLUSTER_TODO_FSYNC_CONFIG;
        clusterSaveConfigOrDie(fsync);
    }
}

void clusterDoBeforeSleep(int flags) {
    /* Clear the cache if there are config changes here. */
    if (flags & CLUSTER_TODO_SAVE_CONFIG) clearCachedClusterSlotsResponse();

    server.cluster->todo_before_sleep |= flags;
}

/* -----------------------------------------------------------------------------
 * Slots management
 * -------------------------------------------------------------------------- */

/* Test bit 'pos' in a generic bitmap. Return 1 if the bit is set,
 * otherwise 0. */
int bitmapTestBit(unsigned char *bitmap, int pos) {
    off_t byte = pos / 8;
    int bit = pos & 7;
    return (bitmap[byte] & (1 << bit)) != 0;
}

/* Set the bit at position 'pos' in a bitmap. */
void bitmapSetBit(unsigned char *bitmap, int pos) {
    off_t byte = pos / 8;
    int bit = pos & 7;
    bitmap[byte] |= 1 << bit;
}

/* Clear the bit at position 'pos' in a bitmap. */
void bitmapClearBit(unsigned char *bitmap, int pos) {
    off_t byte = pos / 8;
    int bit = pos & 7;
    bitmap[byte] &= ~(1 << bit);
}

/* Return non-zero if there is at least one primary with replicas in the cluster.
 * Otherwise zero is returned. Used by clusterNodeSetSlotBit() to set the
 * MIGRATE_TO flag the when a primary gets the first slot. */
int clusterPrimariesHaveReplicas(void) {
    dictIterator di;
    dictInitIterator(&di, server.cluster->nodes);
    dictEntry *de;
    int replicas = 0;
    while ((de = dictNext(&di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (nodeIsReplica(node)) continue;
        replicas += node->num_replicas;
    }
    return replicas != 0;
}

/* Set the slot bit and return the old value. */
int clusterNodeSetSlotBit(clusterNode *n, int slot) {
    int old = bitmapTestBit(n->slots, slot);
    if (!old) {
        bitmapSetBit(n->slots, slot);
        n->numslots++;
        /* When a primary gets its first slot, even if it has no replicas,
         * it gets flagged with MIGRATE_TO, that is, the primary is a valid
         * target for replicas migration, if and only if at least one of
         * the other primaries has replicas right now.
         *
         * Normally primaries are valid targets of replica migration if:
         * 1. The used to have replicas (but no longer have).
         * 2. They are replicas failing over a primary that used to have replicas.
         *
         * However new primaries with slots assigned are considered valid
         * migration targets if the rest of the cluster is not a replica-less.
         *
         * See https://github.com/redis/redis/issues/3043 for more info. */
        if (n->numslots == 1 && clusterPrimariesHaveReplicas()) n->flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    return old;
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

/* Return the slot bit from the cluster node structure. */
int clusterNodeCoversSlot(clusterNode *n, int slot) {
    return bitmapTestBit(n->slots, slot);
}

/* Add the specified slot to the list of slots that node 'n' will
 * serve. Return C_OK if the operation ended with success.
 * If the slot is already assigned to another instance this is considered
 * an error and C_ERR is returned. */
int clusterAddSlot(clusterNode *n, int slot) {
    if (server.cluster->slots[slot]) return C_ERR;
    clusterNodeSetSlotBit(n, slot);
    server.cluster->slots[slot] = n;
    bitmapClearBit(server.cluster->owner_not_claiming_slot, slot);
    clusterSlotStatReset(slot);
    return C_OK;
}

/* Delete the specified slot marking it as unassigned.
 * Returns C_OK if the slot was assigned, otherwise if the slot was
 * already unassigned C_ERR is returned. */
int clusterDelSlot(int slot) {
    clusterNode *n = server.cluster->slots[slot];

    if (!n) return C_ERR;

    /* Cleanup the channels in primary/replica as part of slot deletion. */
    removeChannelsInSlot(slot);
    /* Clear the slot bit. */
    serverAssert(clusterNodeClearSlotBit(n, slot) == 1);
    server.cluster->slots[slot] = NULL;
    /* Make owner_not_claiming_slot flag consistent with slot ownership information. */
    bitmapClearBit(server.cluster->owner_not_claiming_slot, slot);
    clusterSlotStatReset(slot);
    return C_OK;
}

/* Delete all the slots associated with the specified node.
 * The number of deleted slots is returned. */
int clusterDelNodeSlots(clusterNode *node) {
    int deleted = 0, j;

    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (clusterNodeCoversSlot(node, j)) {
            clusterDelSlot(j);
            deleted++;
        }
    }
    return deleted;
}

/* Clear the migrating / importing state for all the slots.
 * This is useful at initialization and when turning a primary into replica. */
void clusterCloseAllSlots(void) {
    memset(server.cluster->migrating_slots_to, 0, sizeof(server.cluster->migrating_slots_to));
    memset(server.cluster->importing_slots_from, 0, sizeof(server.cluster->importing_slots_from));
}

/* -----------------------------------------------------------------------------
 * Cluster state evaluation function
 * -------------------------------------------------------------------------- */

/* The following are defines that are only used in the evaluation function
 * and are based on heuristics. Actually the main point about the rejoin and
 * writable delay is that they should be a few orders of magnitude larger
 * than the network latency. */
#define CLUSTER_MAX_REJOIN_DELAY 5000
#define CLUSTER_MIN_REJOIN_DELAY 500
#define CLUSTER_WRITABLE_DELAY 2000

void clusterUpdateState(void) {
    int j, new_state;
    int reachable_primaries = 0;
    static mstime_t among_minority_time;
    static mstime_t first_call_time = 0;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_UPDATE_STATE;

    /* If this is a primary node, wait some time before turning the state
     * into OK, since it is not a good idea to rejoin the cluster as a writable
     * primary, after a reboot, without giving the cluster a chance to
     * reconfigure this node. Note that the delay is calculated starting from
     * the first call to this function and not since the server start, in order
     * to not count the DB loading time. */
    if (first_call_time == 0) first_call_time = mstime();
    if (clusterNodeIsPrimary(myself) && server.cluster->state == CLUSTER_FAIL &&
        mstime() - first_call_time < CLUSTER_WRITABLE_DELAY)
        return;

    /* Start assuming the state is OK. We'll turn it into FAIL if there
     * are the right conditions. */
    new_state = CLUSTER_OK;

    /* Check if all the slots are covered. */
    if (server.cluster_require_full_coverage) {
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            if (server.cluster->slots[j] == NULL || server.cluster->slots[j]->flags & (CLUSTER_NODE_FAIL)) {
                new_state = CLUSTER_FAIL;
                break;
            }
        }
    }

    /* Compute the cluster size, that is the number of primary nodes
     * serving at least a single slot.
     *
     * At the same time count the number of reachable primaries having
     * at least one slot. */
    {
        dictIterator *di;
        dictEntry *de;

        server.cluster->size = 0;
        di = dictGetSafeIterator(server.cluster->nodes);
        while ((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);

            if (clusterNodeIsVotingPrimary(node)) {
                server.cluster->size++;
                if ((node->flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_PFAIL)) == 0) reachable_primaries++;
            }
        }
        dictReleaseIterator(di);
    }

    /* If we are in a minority partition, change the cluster state
     * to FAIL. */
    {
        int needed_quorum = (server.cluster->size / 2) + 1;

        if (reachable_primaries < needed_quorum) {
            new_state = CLUSTER_FAIL;
            among_minority_time = mstime();
        }
    }

    /* Log a state change */
    if (new_state != server.cluster->state) {
        mstime_t rejoin_delay = server.cluster_node_timeout;

        /* If the instance is a primary and was partitioned away with the
         * minority, don't let it accept queries for some time after the
         * partition heals, to make sure there is enough time to receive
         * a configuration update. */
        if (rejoin_delay > CLUSTER_MAX_REJOIN_DELAY) rejoin_delay = CLUSTER_MAX_REJOIN_DELAY;
        if (rejoin_delay < CLUSTER_MIN_REJOIN_DELAY) rejoin_delay = CLUSTER_MIN_REJOIN_DELAY;

        if (new_state == CLUSTER_OK && clusterNodeIsPrimary(myself) && mstime() - among_minority_time < rejoin_delay) {
            return;
        }

        /* Change the state and log the event. */
        serverLog(new_state == CLUSTER_OK ? LL_NOTICE : LL_WARNING, "Cluster state changed: %s",
                  new_state == CLUSTER_OK ? "ok" : "fail");
        server.cluster->state = new_state;
    }
}

/* This function is called after the node startup in order to verify that data
 * loaded from disk is in agreement with the cluster configuration:
 *
 * 1) If we find keys about hash slots we have no responsibility for and
 *    no other node is in charge according to the current cluster
 *    configuration, we add these slots to our node.
 * 2) If we find data in a DB different than DB0 we return C_ERR to
 *    signal the caller it should quit the server with an error message
 *    or take other actions.
 *
 * The function always returns C_OK even if it will try to correct
 * the error described in "1". However if data is found in DB different
 * from DB0, C_ERR is returned.
 *
 * The function also uses the logging facility in order to warn the user
 * about desynchronizations between the data we have in memory and the
 * cluster configuration. */
int verifyClusterConfigWithData(void) {
    int j;
    int update_config = 0;

    /* Return ASAP if a module disabled cluster redirections. In that case
     * every primary can store keys about every possible hash slot. */
    if (server.cluster_module_flags & CLUSTER_MODULE_FLAG_NO_REDIRECTION) return C_OK;

    /* If this node is a replica, don't perform the check at all as we
     * completely depend on the replication stream. */
    if (nodeIsReplica(myself)) return C_OK;

    /* Make sure we only have keys in DB0. */
    for (j = 1; j < server.dbnum; j++) {
        if (kvstoreSize(server.db[j].keys)) return C_ERR;
    }

    /* Check that all the slots we see populated memory have a corresponding
     * entry in the cluster table. Otherwise fix the table. */
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (!countKeysInSlot(j)) continue; /* No keys in this slot. */
        /* Check if we are assigned to this slot or if we are importing it.
         * In both cases check the next slot as the configuration makes
         * sense. */
        if (server.cluster->slots[j] == myself || server.cluster->importing_slots_from[j] != NULL) continue;

        /* If we are here data and cluster config don't agree, and we have
         * slot 'j' populated even if we are not importing it, nor we are
         * assigned to this slot. Fix this condition. */

        update_config++;
        /* slot is unassigned. Take responsibility for it. */
        if (server.cluster->slots[j] == NULL) {
            serverLog(LL_NOTICE,
                      "I have keys for unassigned slot %d. "
                      "Taking responsibility for it.",
                      j);
            clusterAddSlot(myself, j);
        } else if (server.cluster->importing_slots_from[j] != server.cluster->slots[j]) {
            if (server.cluster->importing_slots_from[j] == NULL) {
                serverLog(LL_NOTICE,
                          "I have keys for slot %d, but the slot is "
                          "assigned to another node. Deleting keys in the slot.",
                          j);
            } else {
                serverLog(LL_NOTICE,
                          "I am importing keys from node %.40s (%s) in shard %.40s to slot %d, "
                          "but the slot is now owned by node %.40s (%s) in shard %.40s. Deleting keys in the slot",
                          server.cluster->importing_slots_from[j]->name,
                          server.cluster->importing_slots_from[j]->human_nodename,
                          server.cluster->importing_slots_from[j]->shard_id, j, server.cluster->slots[j]->name,
                          server.cluster->slots[j]->human_nodename, server.cluster->slots[j]->shard_id);
            }
            delKeysInSlot(j);
        }
    }
    if (update_config) clusterSaveConfigOrDie(1);
    return C_OK;
}

/* Remove all the shard channel related information not owned by the current shard. */
static inline void removeAllNotOwnedShardChannelSubscriptions(void) {
    if (!kvstoreSize(server.pubsubshard_channels)) return;
    clusterNode *cur_primary = clusterNodeIsPrimary(myself) ? myself : myself->replicaof;
    for (int j = 0; j < CLUSTER_SLOTS; j++) {
        if (server.cluster->slots[j] != cur_primary) {
            removeChannelsInSlot(j);
        }
    }
}

/* -----------------------------------------------------------------------------
 * REPLICA nodes handling
 * -------------------------------------------------------------------------- */

/* Set the specified node 'n' as primary for this node.
 * If this node is currently a primary, it is turned into a replica. */
static void clusterSetPrimary(clusterNode *n, int closeSlots, int full_sync_required) {
    serverAssert(n != myself);
    serverAssert(myself->numslots == 0);

    if (clusterNodeIsPrimary(myself)) {
        myself->flags &= ~(CLUSTER_NODE_PRIMARY | CLUSTER_NODE_MIGRATE_TO);
        myself->flags |= CLUSTER_NODE_REPLICA;
    } else {
        if (myself->replicaof) clusterNodeRemoveReplica(myself->replicaof, myself);
    }
    if (closeSlots) clusterCloseAllSlots();
    myself->replicaof = n;
    updateShardId(myself, n->shard_id);
    clusterNodeAddReplica(n, myself);
    replicationSetPrimary(n->ip, getNodeDefaultReplicationPort(n), full_sync_required);
    removeAllNotOwnedShardChannelSubscriptions();
    resetManualFailover();

    if (server.cluster->failover_auth_time) {
        /* Since we have changed to a new primary node, the previously set
         * failover_auth_time should no longer be used, whether it is in
         * progress or timed out. */
        server.cluster->failover_auth_time = 0;
    }
}

/* -----------------------------------------------------------------------------
 * Nodes to string representation functions.
 * -------------------------------------------------------------------------- */

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
sds representSlotInfo(sds ci, uint16_t *slot_info_pairs, int slot_info_pairs_count) {
    for (int i = 0; i < slot_info_pairs_count; i += 2) {
        unsigned long start = slot_info_pairs[i];
        unsigned long end = slot_info_pairs[i + 1];
        if (start == end) {
            ci = sdscatfmt(ci, " %i", start);
        } else {
            ci = sdscatfmt(ci, " %i-%i", start, end);
        }
    }
    return ci;
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
    int port = clusterNodeClientPort(node, tls_primary);
    char *ip = clusterNodeIp(node, c);

    /* Node coordinates */
    ci = sdscatlen(sdsempty(), node->name, CLUSTER_NAMELEN);
    ci = sdscatfmt(ci, " %s:%i@%i", ip, port, node->cport);
    if (sdslen(node->hostname) != 0) {
        ci = sdscatfmt(ci, ",%s", node->hostname);
    }
    /* Don't expose aux fields to any clients yet but do allow them
     * to be persisted to nodes.conf */
    if (c == NULL) {
        if (sdslen(node->hostname) == 0) {
            ci = sdscatfmt(ci, ",", 1);
        }
        for (int i = af_count - 1; i >= 0; i--) {
            if ((tls_primary && i == af_tls_port) || (!tls_primary && i == af_tcp_port)) {
                continue;
            }
            if (auxFieldHandlers[i].isPresent(node)) {
                ci = sdscatprintf(ci, ",%s=", auxFieldHandlers[i].field);
                ci = auxFieldHandlers[i].getter(node, ci);
            }
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

    /* Latency from the POV of this node, config epoch, link status */
    ci = sdscatfmt(ci, " %I %I %U %s", (long long)node->ping_sent, (long long)node->pong_received, nodeEpoch(node),
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
            if (server.cluster->migrating_slots_to[j]) {
                ci = sdscatprintf(ci, " [%d->-%.40s]", j, server.cluster->migrating_slots_to[j]->name);
            } else if (server.cluster->importing_slots_from[j]) {
                ci = sdscatprintf(ci, " [%d-<-%.40s]", j, server.cluster->importing_slots_from[j]->name);
            }
        }
    }
    return ci;
}

/* Generate the slot topology for all nodes and store the string representation
 * in the slots_info struct on the node. This is used to improve the efficiency
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

/* Add to the output buffer of the given client the description of the given cluster link.
 * The description is a map with each entry being an attribute of the link. */
void addReplyClusterLinkDescription(client *c, clusterLink *link) {
    addReplyMapLen(c, 6);

    addReplyBulkCString(c, "direction");
    addReplyBulkCString(c, link->inbound ? "from" : "to");

    /* addReplyClusterLinkDescription is only called for links that have been
     * associated with nodes. The association is always bi-directional, so
     * in addReplyClusterLinkDescription, link->node should never be NULL. */
    serverAssert(link->node);
    sds node_name = sdsnewlen(link->node->name, CLUSTER_NAMELEN);
    addReplyBulkCString(c, "node");
    addReplyBulkCString(c, node_name);
    sdsfree(node_name);

    addReplyBulkCString(c, "create-time");
    addReplyLongLong(c, link->ctime);

    char events[3], *p;
    p = events;
    if (link->conn) {
        if (connHasReadHandler(link->conn)) *p++ = 'r';
        if (connHasWriteHandler(link->conn)) *p++ = 'w';
    }
    *p = '\0';
    addReplyBulkCString(c, "events");
    addReplyBulkCString(c, events);

    addReplyBulkCString(c, "send-buffer-allocated");
    addReplyLongLong(c, link->send_msg_queue_mem);

    addReplyBulkCString(c, "send-buffer-used");
    addReplyLongLong(c, link->send_msg_queue_mem);
}

/* Add to the output buffer of the given client an array of cluster link descriptions,
 * with array entry being a description of a single current cluster link. */
void addReplyClusterLinksDescription(client *c) {
    dictIterator *di;
    dictEntry *de;
    void *arraylen_ptr = NULL;
    int num_links = 0;

    arraylen_ptr = addReplyDeferredLen(c);

    di = dictGetSafeIterator(server.cluster->nodes);
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->link) {
            num_links++;
            addReplyClusterLinkDescription(c, node->link);
        }
        if (node->inbound_link) {
            num_links++;
            addReplyClusterLinkDescription(c, node->inbound_link);
        }
    }
    dictReleaseIterator(di);

    setDeferredArrayLen(c, arraylen_ptr, num_links);
}

/* -----------------------------------------------------------------------------
 * CLUSTER command
 * -------------------------------------------------------------------------- */

const char *clusterGetMessageTypeString(int type) {
    switch (type) {
    case CLUSTERMSG_TYPE_PING: return "ping";
    case CLUSTERMSG_TYPE_PONG: return "pong";
    case CLUSTERMSG_TYPE_MEET: return "meet";
    case CLUSTERMSG_TYPE_FAIL: return "fail";
    case CLUSTERMSG_TYPE_PUBLISH: return "publish";
    case CLUSTERMSG_TYPE_PUBLISHSHARD: return "publishshard";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST: return "auth-req";
    case CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK: return "auth-ack";
    case CLUSTERMSG_TYPE_UPDATE: return "update";
    case CLUSTERMSG_TYPE_MFSTART: return "mfstart";
    case CLUSTERMSG_TYPE_MODULE: return "module";
    }
    return "unknown";
}

int getSlotOrReply(client *c, robj *o) {
    long long slot;

    if (getLongLongFromObject(o, &slot) != C_OK || slot < 0 || slot >= CLUSTER_SLOTS) {
        addReplyError(c, "Invalid or out of range slot");
        return -1;
    }
    return (int)slot;
}

int checkSlotAssignmentsOrReply(client *c, unsigned char *slots, int del, int start_slot, int end_slot) {
    int slot;
    for (slot = start_slot; slot <= end_slot; slot++) {
        if (del && server.cluster->slots[slot] == NULL) {
            addReplyErrorFormat(c, "Slot %d is already unassigned", slot);
            return C_ERR;
        } else if (!del && server.cluster->slots[slot]) {
            addReplyErrorFormat(c, "Slot %d is already busy", slot);
            return C_ERR;
        }
        if (slots[slot]++ == 1) {
            addReplyErrorFormat(c, "Slot %d specified multiple times", (int)slot);
            return C_ERR;
        }
    }
    return C_OK;
}

void clusterUpdateSlots(client *c, unsigned char *slots, int del) {
    int j;
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        if (slots[j]) {
            int retval;

            /* If this slot was set as importing we can clear this
             * state as now we are the real owner of the slot. */
            if (server.cluster->importing_slots_from[j]) server.cluster->importing_slots_from[j] = NULL;

            retval = del ? clusterDelSlot(j) : clusterAddSlot(myself, j);
            serverAssertWithInfo(c, NULL, retval == C_OK);
        }
    }
}

long long getNodeReplicationOffset(clusterNode *node) {
    if (node->flags & CLUSTER_NODE_MYSELF) {
        return nodeIsReplica(node) ? replicationGetReplicaOffset() : server.primary_repl_offset;
    } else {
        return node->repl_offset;
    }
}

/* Add detailed information of a node to the output buffer of the given client. */
void addNodeDetailsToShardReply(client *c, clusterNode *node) {
    int reply_count = 0;
    void *node_replylen = addReplyDeferredLen(c);
    addReplyBulkCString(c, "id");
    addReplyBulkCBuffer(c, node->name, CLUSTER_NAMELEN);
    reply_count++;

    if (node->tcp_port) {
        addReplyBulkCString(c, "port");
        addReplyLongLong(c, node->tcp_port);
        reply_count++;
    }

    if (node->tls_port) {
        addReplyBulkCString(c, "tls-port");
        addReplyLongLong(c, node->tls_port);
        reply_count++;
    }

    addReplyBulkCString(c, "ip");
    addReplyBulkCString(c, clusterNodeIp(node, c));
    reply_count++;

    addReplyBulkCString(c, "endpoint");
    addReplyBulkCString(c, clusterNodePreferredEndpoint(node, c));
    reply_count++;

    if (sdslen(node->hostname) != 0) {
        addReplyBulkCString(c, "hostname");
        addReplyBulkCBuffer(c, node->hostname, sdslen(node->hostname));
        reply_count++;
    }

    long long node_offset = getNodeReplicationOffset(node);

    addReplyBulkCString(c, "role");
    addReplyBulkCString(c, nodeIsReplica(node) ? "replica" : "master");
    reply_count++;

    addReplyBulkCString(c, "replication-offset");
    addReplyLongLong(c, node_offset);
    reply_count++;

    addReplyBulkCString(c, "health");
    const char *health_msg = NULL;
    if (nodeFailed(node)) {
        health_msg = "fail";
    } else if (nodeIsReplica(node) && node_offset == 0) {
        health_msg = "loading";
    } else {
        health_msg = "online";
    }
    addReplyBulkCString(c, health_msg);
    reply_count++;

    setDeferredMapLen(c, node_replylen, reply_count);
}

/* Add to the output buffer of the given client, an array of slot (start, end)
 * pair owned by the shard, also the primary and set of replica(s) along with
 * information about each node. */
void clusterCommandShards(client *c) {
    addReplyArrayLen(c, dictSize(server.cluster->shards));
    /* This call will add slot_info_pairs to all nodes */
    clusterGenNodesSlotsInfo(0);
    dictIterator *di = dictGetSafeIterator(server.cluster->shards);
    for (dictEntry *de = dictNext(di); de != NULL; de = dictNext(di)) {
        list *nodes = dictGetVal(de);
        serverAssert(listLength(nodes) > 0);
        addReplyMapLen(c, 2);
        addReplyBulkCString(c, "slots");

        /* Find a node which has the slot information served by this shard. */
        clusterNode *n = NULL;
        listIter li;
        listRewind(nodes, &li);
        for (listNode *ln = listNext(&li); ln != NULL; ln = listNext(&li)) {
            n = listNodeValue(ln);
            if (n->slot_info_pairs) {
                break;
            }
        }

        if (n && n->slot_info_pairs != NULL) {
            serverAssert((n->slot_info_pairs_count % 2) == 0);
            addReplyArrayLen(c, n->slot_info_pairs_count);
            for (int i = 0; i < n->slot_info_pairs_count; i++) {
                addReplyLongLong(c, (unsigned long)n->slot_info_pairs[i]);
            }
        } else {
            /* If no slot info pair is provided, the node owns no slots */
            addReplyArrayLen(c, 0);
        }

        addReplyBulkCString(c, "nodes");
        addReplyArrayLen(c, listLength(nodes));
        listRewind(nodes, &li);
        for (listNode *ln = listNext(&li); ln != NULL; ln = listNext(&li)) {
            clusterNode *n = listNodeValue(ln);
            addNodeDetailsToShardReply(c, n);
            clusterFreeNodesSlotsInfo(n);
        }
    }
    dictReleaseIterator(di);
}

sds genClusterInfoString(void) {
    sds info = sdsempty();
    char *statestr[] = {"ok", "fail"};
    int slots_assigned = 0, slots_ok = 0, slots_pfail = 0, slots_fail = 0;
    int j;

    for (j = 0; j < CLUSTER_SLOTS; j++) {
        clusterNode *n = server.cluster->slots[j];

        if (n == NULL) continue;
        slots_assigned++;
        if (nodeFailed(n)) {
            slots_fail++;
        } else if (nodeTimedOut(n)) {
            slots_pfail++;
        } else {
            slots_ok++;
        }
    }

    info = sdscatprintf(info,
                        "cluster_state:%s\r\n"
                        "cluster_slots_assigned:%d\r\n"
                        "cluster_slots_ok:%d\r\n"
                        "cluster_slots_pfail:%d\r\n"
                        "cluster_slots_fail:%d\r\n"
                        "cluster_known_nodes:%lu\r\n"
                        "cluster_size:%d\r\n"
                        "cluster_current_epoch:%llu\r\n"
                        "cluster_my_epoch:%llu\r\n",
                        statestr[server.cluster->state], slots_assigned, slots_ok, slots_pfail, slots_fail,
                        dictSize(server.cluster->nodes), server.cluster->size,
                        (unsigned long long)server.cluster->currentEpoch, (unsigned long long)nodeEpoch(myself));

    /* Show stats about messages sent and received. */
    long long tot_msg_sent = 0;
    long long tot_msg_received = 0;

    for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
        if (server.cluster->stats_bus_messages_sent[i] == 0) continue;
        tot_msg_sent += server.cluster->stats_bus_messages_sent[i];
        info = sdscatprintf(info, "cluster_stats_messages_%s_sent:%lld\r\n", clusterGetMessageTypeString(i),
                            server.cluster->stats_bus_messages_sent[i]);
    }
    info = sdscatprintf(info, "cluster_stats_messages_sent:%lld\r\n", tot_msg_sent);

    for (int i = 0; i < CLUSTERMSG_TYPE_COUNT; i++) {
        if (server.cluster->stats_bus_messages_received[i] == 0) continue;
        tot_msg_received += server.cluster->stats_bus_messages_received[i];
        info = sdscatprintf(info, "cluster_stats_messages_%s_received:%lld\r\n", clusterGetMessageTypeString(i),
                            server.cluster->stats_bus_messages_received[i]);
    }
    info = sdscatprintf(info, "cluster_stats_messages_received:%lld\r\n", tot_msg_received);

    info = sdscatprintf(info, "total_cluster_links_buffer_limit_exceeded:%llu\r\n",
                        server.cluster->stat_cluster_links_buffer_limit_exceeded);

    return info;
}


void removeChannelsInSlot(unsigned int slot) {
    if (countChannelsInSlot(slot) == 0) return;

    pubsubShardUnsubscribeAllChannelsInSlot(slot);
}

/* Remove all the keys in the specified hash slot.
 * The number of removed items is returned. */
unsigned int delKeysInSlot(unsigned int hashslot) {
    if (!kvstoreHashsetSize(server.db->keys, hashslot)) return 0;

    unsigned int j = 0;

    kvstoreHashsetIterator *kvs_di = NULL;
    valkey *valkey = NULL;
    kvs_di = kvstoreGetHashsetSafeIterator(server.db->keys, hashslot);
    while (kvstoreHashsetIteratorNext(kvs_di, (void **)&valkey)) {
        enterExecutionUnit(1, 0);
        sds sdskey = valkeyGetKey(valkey);
        robj *key = createStringObject(sdskey, sdslen(sdskey));
        dbDelete(&server.db[0], key);
        propagateDeletion(&server.db[0], key, server.lazyfree_lazy_server_del);
        signalModifiedKey(NULL, &server.db[0], key);
        /* The keys are not actually logically deleted from the database, just moved to another node.
         * The modules needs to know that these keys are no longer available locally, so just send the
         * keyspace notification to the modules, but not to clients. */
        moduleNotifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, server.db[0].id);
        exitExecutionUnit();
        postExecutionUnitOperations();
        decrRefCount(key);
        j++;
        server.dirty++;
    }
    kvstoreReleaseHashsetIterator(kvs_di);

    return j;
}

/* Get the count of the channels for a given slot. */
unsigned int countChannelsInSlot(unsigned int hashslot) {
    return kvstoreHashsetSize(server.pubsubshard_channels, hashslot);
}

clusterNode *getMyClusterNode(void) {
    return server.cluster->myself;
}

int clusterManualFailoverTimeLimit(void) {
    return server.cluster->mf_end;
}

int getClusterSize(void) {
    return dictSize(server.cluster->nodes);
}

int getMyShardSlotCount(void) {
    if (!nodeIsReplica(server.cluster->myself)) {
        return server.cluster->myself->numslots;
    } else if (server.cluster->myself->replicaof) {
        return server.cluster->myself->replicaof->numslots;
    } else {
        return 0;
    }
}

char **getClusterNodesList(size_t *numnodes) {
    size_t count = dictSize(server.cluster->nodes);
    char **ids = zmalloc((count + 1) * CLUSTER_NAMELEN);
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

int clusterNodeIsPrimary(clusterNode *n) {
    return n->flags & CLUSTER_NODE_PRIMARY;
}

int handleDebugClusterCommand(client *c) {
    if (strcasecmp(c->argv[1]->ptr, "CLUSTERLINK") || strcasecmp(c->argv[2]->ptr, "KILL") || c->argc != 5) {
        return 0;
    }

    if (!server.cluster_enabled) {
        addReplyError(c, "Debug option only available for cluster mode enabled setup!");
        return 1;
    }

    /* Find the node. */
    clusterNode *n = clusterLookupNode(c->argv[4]->ptr, sdslen(c->argv[4]->ptr));
    if (!n) {
        addReplyErrorFormat(c, "Unknown node %s", (char *)c->argv[4]->ptr);
        return 1;
    }
    if (n == server.cluster->myself) {
        addReplyErrorFormat(c, "Cannot free cluster link(s) to myself");
        return 1;
    }

    /* Terminate the link based on the direction or all. */
    if (!strcasecmp(c->argv[3]->ptr, "from")) {
        if (n->inbound_link) freeClusterLink(n->inbound_link);
    } else if (!strcasecmp(c->argv[3]->ptr, "to")) {
        if (n->link) freeClusterLink(n->link);
    } else if (!strcasecmp(c->argv[3]->ptr, "all")) {
        if (n->link) freeClusterLink(n->link);
        if (n->inbound_link) freeClusterLink(n->inbound_link);
    } else {
        addReplyErrorFormat(c, "Unknown direction %s", (char *)c->argv[3]->ptr);
    }
    addReply(c, shared.ok);

    return 1;
}

int clusterNodePending(clusterNode *node) {
    return node->flags & (CLUSTER_NODE_NOADDR | CLUSTER_NODE_HANDSHAKE);
}

/* Returns the IP of the node as seen by the given client, or by the cluster node if c is NULL. */
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

int clusterNodeIsReplica(clusterNode *node) {
    return node->flags & CLUSTER_NODE_REPLICA;
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

char *clusterNodeGetName(clusterNode *node) {
    return node->name;
}

int clusterNodeTimedOut(clusterNode *node) {
    return nodeTimedOut(node);
}

int clusterNodeIsFailing(clusterNode *node) {
    return nodeFailed(node);
}

int clusterNodeIsNoFailover(clusterNode *node) {
    return node->flags & CLUSTER_NODE_NOFAILOVER;
}

const char **clusterDebugCommandExtendedHelp(void) {
    static const char *help[] = {"CLUSTERLINK KILL <to|from|all> <node-id>",
                                 "    Kills the link based on the direction to/from (both) with the provided node.",
                                 NULL};

    return help;
}

char *clusterNodeGetShardId(clusterNode *node) {
    return node->shard_id;
}

/* clusterParseSetSlotCommand validates the arguments of the CLUSTER SETSLOT command,
 * extracts the target slot number (slot_out), and determines the target node (node_out)
 * if applicable. It also calculates a timeout value (timeout_out) based on an optional
 * timeout argument. If provided, the timeout is added to the current time to obtain an
 * absolute timestamp; if omitted, the default timeout CLUSTER_OPERATION_TIMEOUT is used;
 * if set to 0, it indicates no timeout. The function returns 1 if successful, and 0
 * otherwise, after sending an error message to the client. */
int clusterParseSetSlotCommand(client *c, int *slot_out, clusterNode **node_out, mstime_t *timeout_out) {
    int slot = -1;
    clusterNode *n = NULL;
    mstime_t timeout = commandTimeSnapshot() + CLUSTER_OPERATION_TIMEOUT;
    int optarg_pos = 0;

    /* Allow primaries to replicate "CLUSTER SETSLOT" */
    if (!c->flag.primary && nodeIsReplica(myself)) {
        addReplyError(c, "Please use SETSLOT only with masters.");
        return 0;
    }

    /* If 'myself' is a replica, 'c' must be the primary client. */
    serverAssert(!nodeIsReplica(myself) || c == server.primary);

    if ((slot = getSlotOrReply(c, c->argv[2])) == -1) return 0;

    if (!strcasecmp(c->argv[3]->ptr, "migrating") && c->argc >= 5) {
        /* CLUSTER SETSLOT <SLOT> MIGRATING <NODE> */
        if (nodeIsPrimary(myself) && server.cluster->slots[slot] != myself) {
            addReplyErrorFormat(c, "I'm not the owner of hash slot %u", slot);
            return 0;
        }
        n = clusterLookupNode(c->argv[4]->ptr, sdslen(c->argv[4]->ptr));
        if (n == NULL) {
            addReplyErrorFormat(c, "I don't know about node %s", (char *)c->argv[4]->ptr);
            return 0;
        }
        if (nodeIsReplica(n)) {
            addReplyError(c, "Target node is not a master");
            return 0;
        }
        if (c->argc > 5) optarg_pos = 5;
    } else if (!strcasecmp(c->argv[3]->ptr, "importing") && c->argc >= 5) {
        /* CLUSTER SETSLOT <SLOT> IMPORTING <NODE> */
        if (server.cluster->slots[slot] == myself) {
            addReplyErrorFormat(c, "I'm already the owner of hash slot %u", slot);
            return 0;
        }
        n = clusterLookupNode(c->argv[4]->ptr, sdslen(c->argv[4]->ptr));
        if (n == NULL) {
            addReplyErrorFormat(c, "I don't know about node %s", (char *)c->argv[4]->ptr);
            return 0;
        }
        if (nodeIsReplica(n)) {
            addReplyError(c, "Target node is not a master");
            return 0;
        }
        if (c->argc > 5) optarg_pos = 5;
    } else if (!strcasecmp(c->argv[3]->ptr, "stable") && c->argc >= 4) {
        /* CLUSTER SETSLOT <SLOT> STABLE */
        if (c->argc > 4) optarg_pos = 4;
    } else if (!strcasecmp(c->argv[3]->ptr, "node") && c->argc >= 5) {
        /* CLUSTER SETSLOT <SLOT> NODE <NODE ID> */
        n = clusterLookupNode(c->argv[4]->ptr, sdslen(c->argv[4]->ptr));
        if (!n) {
            addReplyErrorFormat(c, "Unknown node %s", (char *)c->argv[4]->ptr);
            return 0;
        }
        if (nodeIsReplica(n)) {
            addReplyError(c, "Target node is not a master");
            return 0;
        }
        /* If this hash slot was served by 'myself' before to switch
         * make sure there are no longer local keys for this hash slot. */
        if (server.cluster->slots[slot] == myself && n != myself) {
            if (countKeysInSlot(slot) != 0) {
                addReplyErrorFormat(c,
                                    "Can't assign hashslot %d to a different node "
                                    "while I still hold keys for this hash slot.",
                                    slot);
                return 0;
            }
        }
        if (c->argc > 5) optarg_pos = 5;
    } else {
        addReplyError(c, "Invalid CLUSTER SETSLOT action or number of arguments. Try CLUSTER HELP");
        return 0;
    }

    /* Process optional arguments */
    for (int i = optarg_pos; i < c->argc; i++) {
        if (!strcasecmp(c->argv[i]->ptr, "timeout")) {
            if (i + 1 >= c->argc) {
                addReplyError(c, "Missing timeout value");
                return 0;
            }
            if (getTimeoutFromObjectOrReply(c, c->argv[i + 1], &timeout, UNIT_MILLISECONDS) != C_OK) return 0;
        }
    }

    *slot_out = slot;
    *node_out = n;
    *timeout_out = timeout;
    return 1;
}

void clusterCommandSetSlot(client *c) {
    int slot;
    mstime_t timeout_ms;
    clusterNode *n;

    if (!clusterParseSetSlotCommand(c, &slot, &n, &timeout_ms)) return;

    /* Enhance cluster topology change resilience against primary failures by
     * replicating SETSLOT before execution.
     *
     * Cluster topology changes such slot ownership and migrating states must
     * be replicated to replicas before applying them to the primary. This
     * guarantees that after a command is successfully executed, the new state
     * won't be lost due to a primary node failure. The following example
     * illustrates how a cluster state can be lost during slot ownership
     * finalization:
     *
     * When finalizing the slot, the target primary node B might send a cluster
     * PONG to the source primary node A before the SETSLOT command is replicated
     * to replica node B'. If primary node B crashes at this point, B' will be in
     * the importing state and the slot will have no owner.
     *
     * To mitigate this issue, the following order needs to be enforced for slot
     * migration finalization such that the replicas finalize the slot ownership
     * before the primary:
     *
     * 1. Client C issues SETSLOT n NODE B against node B.
     * 2. Primary B replicates `SETSLOT n NODE B` to all of its replicas (e.g., B', B'').
     * 3. Upon replication completion, primary B executes `SETSLOT n NODE B` and
     *    returns success to client C.
     * 4. The following steps can happen in parallel:
     *   a. Client C issues `SETSLOT n NODE B` against primary A.
     *   b. Primary B gossips its new slot ownership to the cluster (including A, A', etc.).
     *
     * This ensures that all replicas have the latest topology information, enabling
     * a reliable slot ownership transfer even if the primary node went down during
     * the process. */
    if (nodeIsPrimary(myself) && myself->num_replicas != 0 && !c->flag.replication_done) {
        /* Iterate through the list of replicas to check if there are any running
         * version 7.2 or older. Replicas running on these versions do
         * not support the CLUSTER SETSLOT command on replicas. If such a replica
         * is found, we should skip the replication and fall back to the old
         * non-replicated behavior.*/
        listIter li;
        listNode *ln;
        int num_eligible_replicas = 0;
        listRewind(server.replicas, &li);
        while ((ln = listNext(&li))) {
            client *r = ln->value;

            /* We think that when the command comes in, the primary only needs to
             * wait for the online replicas. The admin can easily check if there
             * are replicas that are down for an extended period of time. If they
             * decide to move forward anyways, we should not block it. If a replica
             * failed right before the replication and was not included in the
             * replication, it would also unlikely win the election.
             *
             * And 0x702ff is 7.2.255, we only support new versions in this case. */
            if (r->repl_state == REPLICA_STATE_ONLINE && r->replica_version > 0x702ff) {
                num_eligible_replicas++;
            }
        }

        if (num_eligible_replicas != 0) {
            forceCommandPropagation(c, PROPAGATE_REPL);
            /* We are a primary and this is the first time we see this `SETSLOT`
             * command. Force-replicate the command to all of our replicas
             * first and only on success will we handle the command.
             * Note that
             * 1. All replicas are expected to ack the replication within the given timeout
             * 2. The repl offset target is set to the primary's current repl offset + 1.
             *    There is no concern of partial replication because replicas always
             *    ack the repl offset at the command boundary. */
            blockClientForReplicaAck(c, timeout_ms, server.primary_repl_offset + 1, num_eligible_replicas, 0);
            /* Mark client as pending command for execution after replication to replicas. */
            c->flag.pending_command = 1;
            replicationRequestAckFromReplicas();
            return;
        }
    }

    /* Slot states have been updated on the compatible replicas (if any).
     * Now execute the command on the primary. */
    if (!strcasecmp(c->argv[3]->ptr, "migrating")) {
        serverLog(LL_NOTICE, "Migrating slot %d to node %.40s (%s)", slot, n->name, n->human_nodename);
        server.cluster->migrating_slots_to[slot] = n;
    } else if (!strcasecmp(c->argv[3]->ptr, "importing")) {
        serverLog(LL_NOTICE, "Importing slot %d from node %.40s (%s)", slot, n->name, n->human_nodename);
        server.cluster->importing_slots_from[slot] = n;
    } else if (!strcasecmp(c->argv[3]->ptr, "stable")) {
        /* CLUSTER SETSLOT <SLOT> STABLE */
        serverLog(LL_NOTICE, "Marking slot %d stable", slot);
        server.cluster->importing_slots_from[slot] = NULL;
        server.cluster->migrating_slots_to[slot] = NULL;
    } else if (!strcasecmp(c->argv[3]->ptr, "node")) {
        /* CLUSTER SETSLOT <SLOT> NODE <NODE ID> */
        serverLog(LL_NOTICE, "Assigning slot %d to node %.40s (%s) in shard %.40s", slot, n->name, n->human_nodename,
                  n->shard_id);

        /* If this slot is in migrating status but we have no keys
         * for it assigning the slot to another node will clear
         * the migrating status. */
        if (countKeysInSlot(slot) == 0 && server.cluster->migrating_slots_to[slot]) {
            server.cluster->migrating_slots_to[slot] = NULL;
        }

        clusterNode *my_primary = clusterNodeGetPrimary(myself);
        int slot_was_mine = server.cluster->slots[slot] == my_primary;
        clusterDelSlot(slot);
        clusterAddSlot(n, slot);
        int shard_is_empty = my_primary->numslots == 0;

        /* If replica migration is allowed, check if the primary of this shard
         * loses its last slot and the shard becomes empty. In this case, we
         * should turn into a replica of the new primary. */
        if (server.cluster_allow_replica_migration && slot_was_mine && shard_is_empty) {
            serverAssert(n != my_primary);
            serverLog(LL_NOTICE,
                      "Lost my last slot during slot migration. Reconfiguring myself "
                      "as a replica of %.40s (%s) in shard %.40s",
                      n->name, n->human_nodename, n->shard_id);
            /* `c` is the primary client if `myself` is a replica, prevent it
             * from being freed by clusterSetPrimary. */
            if (nodeIsReplica(myself)) protectClient(c);
            /* We are migrating to a different shard that has a completely different
             * replication history, so a full sync is required. */
            clusterSetPrimary(n, 1, 1);
            if (nodeIsReplica(myself)) unprotectClient(c);
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_FSYNC_CONFIG);
        }

        /* If replica migration is not allowed, check if the primary of this shard
         * loses its last slot and the shard becomes empty. In this case, we will
         * print the exact same log as during the gossip process. */
        if (!server.cluster_allow_replica_migration && nodeIsPrimary(myself) && slot_was_mine && shard_is_empty) {
            serverAssert(n != my_primary);
            serverLog(LL_NOTICE,
                      "My last slot was migrated to node %.40s (%s) in shard %.40s. I am now an empty primary.",
                      n->name, n->human_nodename, n->shard_id);
        }

        /* If this node or this node's primary was importing this slot,
         * assigning the slot to itself also clears the importing status. */
        if ((n == myself || n == myself->replicaof) && server.cluster->importing_slots_from[slot]) {
            server.cluster->importing_slots_from[slot] = NULL;

            /* Only primary broadcasts the updates */
            if (n == myself) {
                /* This slot was manually migrated, set this node configEpoch
                 * to a new epoch so that the new version can be propagated
                 * by the cluster.
                 *
                 * Note that if this ever results in a collision with another
                 * node getting the same configEpoch, for example because a
                 * failover happens at the same time we close the slot, the
                 * configEpoch collision resolution will fix it assigning
                 * a different epoch to each node. */
                if (clusterBumpConfigEpochWithoutConsensus() == C_OK) {
                    serverLog(LL_NOTICE, "ConfigEpoch updated after importing slot %d", slot);
                }
                /* After importing this slot, let the other nodes know as
                 * soon as possible. */
                clusterBroadcastPong(CLUSTER_BROADCAST_ALL);
            }
        }
    }

    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
    addReply(c, shared.ok);
}

int clusterCommandSpecial(client *c) {
    if (!strcasecmp(c->argv[1]->ptr, "meet") && (c->argc == 4 || c->argc == 5)) {
        /* CLUSTER MEET <ip> <port> [cport] */
        long long port, cport;

        if (getLongLongFromObject(c->argv[3], &port) != C_OK) {
            addReplyErrorFormat(c, "Invalid base port specified: %s", (char *)c->argv[3]->ptr);
            return 1;
        }

        if (c->argc == 5) {
            if (getLongLongFromObject(c->argv[4], &cport) != C_OK) {
                addReplyErrorFormat(c, "Invalid bus port specified: %s", (char *)c->argv[4]->ptr);
                return 1;
            }
        } else {
            cport = port + CLUSTER_PORT_INCR;
        }

        if (clusterStartHandshake(c->argv[2]->ptr, port, cport) == 0 && errno == EINVAL) {
            addReplyErrorFormat(c, "Invalid node address specified: %s:%s", (char *)c->argv[2]->ptr,
                                (char *)c->argv[3]->ptr);
        } else {
            addReply(c, shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr, "flushslots") && c->argc == 2) {
        /* CLUSTER FLUSHSLOTS */
        if (kvstoreSize(server.db[0].keys) != 0) {
            addReplyError(c, "DB must be empty to perform CLUSTER FLUSHSLOTS.");
            return 1;
        }
        clusterDelNodeSlots(myself);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
        addReply(c, shared.ok);
    } else if ((!strcasecmp(c->argv[1]->ptr, "addslots") || !strcasecmp(c->argv[1]->ptr, "delslots")) && c->argc >= 3) {
        /* CLUSTER ADDSLOTS <slot> [slot] ... */
        /* CLUSTER DELSLOTS <slot> [slot] ... */
        int j, slot;
        unsigned char *slots = zmalloc(CLUSTER_SLOTS);
        int del = !strcasecmp(c->argv[1]->ptr, "delslots");

        memset(slots, 0, CLUSTER_SLOTS);
        /* Check that all the arguments are parseable.*/
        for (j = 2; j < c->argc; j++) {
            if ((slot = getSlotOrReply(c, c->argv[j])) == C_ERR) {
                zfree(slots);
                return 1;
            }
        }
        /* Check that the slots are not already busy. */
        for (j = 2; j < c->argc; j++) {
            slot = getSlotOrReply(c, c->argv[j]);
            if (checkSlotAssignmentsOrReply(c, slots, del, slot, slot) == C_ERR) {
                zfree(slots);
                return 1;
            }
        }
        clusterUpdateSlots(c, slots, del);
        zfree(slots);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
        addReply(c, shared.ok);
    } else if ((!strcasecmp(c->argv[1]->ptr, "addslotsrange") || !strcasecmp(c->argv[1]->ptr, "delslotsrange")) &&
               c->argc >= 4) {
        if (c->argc % 2 == 1) {
            addReplyErrorArity(c);
            return 1;
        }
        /* CLUSTER ADDSLOTSRANGE <start slot> <end slot> [<start slot> <end slot> ...] */
        /* CLUSTER DELSLOTSRANGE <start slot> <end slot> [<start slot> <end slot> ...] */
        int j, startslot, endslot;
        unsigned char *slots = zmalloc(CLUSTER_SLOTS);
        int del = !strcasecmp(c->argv[1]->ptr, "delslotsrange");

        memset(slots, 0, CLUSTER_SLOTS);
        /* Check that all the arguments are parseable and that all the
         * slots are not already busy. */
        for (j = 2; j < c->argc; j += 2) {
            if ((startslot = getSlotOrReply(c, c->argv[j])) == C_ERR) {
                zfree(slots);
                return 1;
            }
            if ((endslot = getSlotOrReply(c, c->argv[j + 1])) == C_ERR) {
                zfree(slots);
                return 1;
            }
            if (startslot > endslot) {
                addReplyErrorFormat(c, "start slot number %d is greater than end slot number %d", startslot, endslot);
                zfree(slots);
                return 1;
            }

            if (checkSlotAssignmentsOrReply(c, slots, del, startslot, endslot) == C_ERR) {
                zfree(slots);
                return 1;
            }
        }
        clusterUpdateSlots(c, slots, del);
        zfree(slots);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
        addReply(c, shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr, "setslot") && c->argc >= 4) {
        /* SETSLOT 10 MIGRATING <node ID> */
        /* SETSLOT 10 IMPORTING <node ID> */
        /* SETSLOT 10 STABLE */
        /* SETSLOT 10 NODE <node ID> */
        clusterCommandSetSlot(c);
    } else if (!strcasecmp(c->argv[1]->ptr, "bumpepoch") && c->argc == 2) {
        /* CLUSTER BUMPEPOCH */
        int retval = clusterBumpConfigEpochWithoutConsensus();
        sds reply = sdscatprintf(sdsempty(), "+%s %llu\r\n", (retval == C_OK) ? "BUMPED" : "STILL",
                                 (unsigned long long)myself->configEpoch);
        addReplySds(c, reply);
    } else if (!strcasecmp(c->argv[1]->ptr, "saveconfig") && c->argc == 2) {
        int retval = clusterSaveConfig(1);

        if (retval == C_OK)
            addReply(c, shared.ok);
        else
            addReplyErrorFormat(c, "error saving the cluster node config: %s", strerror(errno));
    } else if (!strcasecmp(c->argv[1]->ptr, "forget") && c->argc == 3) {
        /* CLUSTER FORGET <NODE ID> */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
        if (!n) {
            if (clusterBlacklistExists((char *)c->argv[2]->ptr))
                /* Already forgotten. The deletion may have been gossipped by
                 * another node, so we pretend it succeeded. */
                addReply(c, shared.ok);
            else
                addReplyErrorFormat(c, "Unknown node %s", (char *)c->argv[2]->ptr);
            return 1;
        } else if (n == myself) {
            addReplyError(c, "I tried hard but I can't forget myself...");
            return 1;
        } else if (nodeIsReplica(myself) && myself->replicaof == n) {
            addReplyError(c, "Can't forget my master!");
            return 1;
        }
        clusterBlacklistAddNode(n);
        clusterDelNode(n);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
        addReply(c, shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr, "replicate") && c->argc == 3) {
        /* CLUSTER REPLICATE <NODE ID> */
        /* Lookup the specified node in our table. */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr, sdslen(c->argv[2]->ptr));
        if (!n) {
            addReplyErrorFormat(c, "Unknown node %s", (char *)c->argv[2]->ptr);
            return 1;
        }

        /* I can't replicate myself. */
        if (n == myself) {
            addReplyError(c, "Can't replicate myself");
            return 1;
        }

        /* Can't replicate a replica. */
        if (nodeIsReplica(n)) {
            addReplyError(c, "I can only replicate a master, not a replica.");
            return 1;
        }

        /* If the instance is currently a primary, it should have no assigned
         * slots nor keys to accept to replicate some other node.
         * Replicas can switch to another primary without issues. */
        if (clusterNodeIsPrimary(myself) && (myself->numslots != 0 || kvstoreSize(server.db[0].keys) != 0)) {
            addReplyError(c, "To set a master the node must be empty and "
                             "without assigned slots.");
            return 1;
        }

        /* If `n` is already my primary, there is no need to re-establish the
         * replication connection. */
        if (myself->replicaof == n) {
            addReply(c, shared.ok);
            return 1;
        }

        /* Set the primary.
         * If the instance is a primary, it is an empty primary.
         * If the instance is a replica, it had a totally different replication history.
         * In these both cases, myself as a replica has to do a full sync. */
        clusterSetPrimary(n, 1, 1);
        clusterBroadcastPong(CLUSTER_BROADCAST_ALL);
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
        addReply(c, shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr, "count-failure-reports") && c->argc == 3) {
        /* CLUSTER COUNT-FAILURE-REPORTS <NODE ID> */
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr, sdslen(c->argv[2]->ptr));

        if (!n) {
            addReplyErrorFormat(c, "Unknown node %s", (char *)c->argv[2]->ptr);
            return 1;
        } else {
            addReplyLongLong(c, clusterNodeFailureReportsCount(n));
        }
    } else if (!strcasecmp(c->argv[1]->ptr, "failover") && (c->argc == 2 || c->argc == 3)) {
        /* CLUSTER FAILOVER [FORCE|TAKEOVER] */
        int force = 0, takeover = 0;

        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr, "force")) {
                force = 1;
            } else if (!strcasecmp(c->argv[2]->ptr, "takeover")) {
                takeover = 1;
                force = 1; /* Takeover also implies force. */
            } else {
                addReplyErrorObject(c, shared.syntaxerr);
                return 1;
            }
        }

        /* Check preconditions. */
        if (clusterNodeIsPrimary(myself)) {
            addReplyError(c, "You should send CLUSTER FAILOVER to a replica");
            return 1;
        } else if (myself->replicaof == NULL) {
            addReplyError(c, "I'm a replica but my master is unknown to me");
            return 1;
        } else if (!force && (nodeFailed(myself->replicaof) || myself->replicaof->link == NULL)) {
            addReplyError(c, "Master is down or failed, "
                             "please use CLUSTER FAILOVER FORCE");
            return 1;
        }
        resetManualFailover();
        server.cluster->mf_end = mstime() + CLUSTER_MF_TIMEOUT;
        sds client = catClientInfoString(sdsempty(), c, server.hide_user_data_from_log);

        if (takeover) {
            /* A takeover does not perform any initial check. It just
             * generates a new configuration epoch for this node without
             * consensus, claims the primary's slots, and broadcast the new
             * configuration. */
            serverLog(LL_NOTICE, "Taking over the primary (user request from '%s').", client);
            clusterBumpConfigEpochWithoutConsensus();
            clusterFailoverReplaceYourPrimary();
        } else if (force) {
            /* If this is a forced failover, we don't need to talk with our
             * primary to agree about the offset. We just failover taking over
             * it without coordination. */
            serverLog(LL_NOTICE, "Forced failover user request accepted (user request from '%s').", client);
            server.cluster->mf_can_start = 1;
            /* We can start a manual failover as soon as possible, setting a flag
             * here so that we don't need to waiting for the cron to kick in. */
            clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_MANUALFAILOVER);
        } else {
            serverLog(LL_NOTICE, "Manual failover user request accepted (user request from '%s').", client);
            clusterSendMFStart(myself->replicaof);
        }
        sdsfree(client);
        addReply(c, shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr, "set-config-epoch") && c->argc == 3) {
        /* CLUSTER SET-CONFIG-EPOCH <epoch>
         *
         * The user is allowed to set the config epoch only when a node is
         * totally fresh: no config epoch, no other known node, and so forth.
         * This happens at cluster creation time to start with a cluster where
         * every node has a different node ID, without to rely on the conflicts
         * resolution system which is too slow when a big cluster is created. */
        long long epoch;

        if (getLongLongFromObjectOrReply(c, c->argv[2], &epoch, NULL) != C_OK) return 1;

        if (epoch < 0) {
            addReplyErrorFormat(c, "Invalid config epoch specified: %lld", epoch);
        } else if (dictSize(server.cluster->nodes) > 1) {
            addReplyError(c, "The user can assign a config epoch only when the "
                             "node does not know any other node.");
        } else if (myself->configEpoch != 0) {
            addReplyError(c, "Node config epoch is already non-zero");
        } else {
            myself->configEpoch = epoch;
            serverLog(LL_NOTICE, "configEpoch set to %llu via CLUSTER SET-CONFIG-EPOCH",
                      (unsigned long long)myself->configEpoch);

            if (server.cluster->currentEpoch < (uint64_t)epoch) server.cluster->currentEpoch = epoch;
            /* No need to fsync the config here since in the unlucky event
             * of a failure to persist the config, the conflict resolution code
             * will assign a unique config to this node. */
            clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
            addReply(c, shared.ok);
        }
    } else if (!strcasecmp(c->argv[1]->ptr, "reset") && (c->argc == 2 || c->argc == 3)) {
        /* CLUSTER RESET [SOFT|HARD] */
        int hard = 0;

        /* Parse soft/hard argument. Default is soft. */
        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr, "hard")) {
                hard = 1;
            } else if (!strcasecmp(c->argv[2]->ptr, "soft")) {
                hard = 0;
            } else {
                addReplyErrorObject(c, shared.syntaxerr);
                return 1;
            }
        }

        /* Replicas can be reset while containing data, but not primary nodes
         * that must be empty. */
        if (clusterNodeIsPrimary(myself) && kvstoreSize(c->db->keys) != 0) {
            addReplyError(c, "CLUSTER RESET can't be called with "
                             "master nodes containing keys");
            return 1;
        }
        clusterReset(hard);
        addReply(c, shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr, "links") && c->argc == 2) {
        /* CLUSTER LINKS */
        addReplyClusterLinksDescription(c);
    } else {
        return 0;
    }

    return 1;
}

const char **clusterCommandExtendedHelp(void) {
    static const char *help[] = {
        "ADDSLOTS <slot> [<slot> ...]",
        "    Assign slots to current node.",
        "ADDSLOTSRANGE <start slot> <end slot> [<start slot> <end slot> ...]",
        "    Assign slots which are between <start-slot> and <end-slot> to current node.",
        "BUMPEPOCH",
        "    Advance the cluster config epoch.",
        "COUNT-FAILURE-REPORTS <node-id>",
        "    Return number of failure reports for <node-id>.",
        "DELSLOTS <slot> [<slot> ...]",
        "    Delete slots information from current node.",
        "DELSLOTSRANGE <start slot> <end slot> [<start slot> <end slot> ...]",
        "    Delete slots information which are between <start-slot> and <end-slot> from current node.",
        "FAILOVER [FORCE|TAKEOVER]",
        "    Promote current replica node to being a master.",
        "FORGET <node-id>",
        "    Remove a node from the cluster.",
        "FLUSHSLOTS",
        "    Delete current node own slots information.",
        "MEET <ip> <port> [<bus-port>]",
        "    Connect nodes into a working cluster.",
        "REPLICATE <node-id>",
        "    Configure current node as replica to <node-id>.",
        "RESET [HARD|SOFT]",
        "    Reset current node (default: soft).",
        "SET-CONFIG-EPOCH <epoch>",
        "    Set config epoch of current node.",
        "SETSLOT <slot> (IMPORTING <node-id>|MIGRATING <node-id>|STABLE|NODE <node-id>)",
        "    Set slot state.",
        "SAVECONFIG",
        "    Force saving cluster configuration on disk.",
        "LINKS",
        "    Return information about all network links between this node and its peers.",
        "    Output format is an array where each array element is a map containing attributes of a link",
        NULL};

    return help;
}

int clusterNodeNumReplicas(clusterNode *node) {
    return node->num_replicas;
}

clusterNode *clusterNodeGetReplica(clusterNode *node, int replica_idx) {
    return node->replicas[replica_idx];
}

clusterNode *getMigratingSlotDest(int slot) {
    return server.cluster->migrating_slots_to[slot];
}

clusterNode *getImportingSlotSource(int slot) {
    return server.cluster->importing_slots_from[slot];
}

int isClusterHealthy(void) {
    return server.cluster->state == CLUSTER_OK;
}

clusterNode *getNodeBySlot(int slot) {
    return server.cluster->slots[slot];
}

char *clusterNodeHostname(clusterNode *node) {
    return node->hostname;
}

long long clusterNodeReplOffset(clusterNode *node) {
    return node->repl_offset;
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

int clusterAllowFailoverCmd(client *c) {
    if (!server.cluster_enabled) {
        return 1;
    }
    addReplyError(c, "FAILOVER not allowed in cluster mode. "
                     "Use CLUSTER FAILOVER command instead.");
    return 0;
}

void clusterPromoteSelfToPrimary(void) {
    replicationUnsetPrimary();
}

int detectAndUpdateCachedNodeHealth(void) {
    dictIterator di;
    dictInitIterator(&di, server.cluster->nodes);
    dictEntry *de;
    clusterNode *node;
    int overall_health_changed = 0;
    while ((de = dictNext(&di)) != NULL) {
        node = dictGetVal(de);
        int present_is_node_healthy = isNodeAvailable(node);
        if (present_is_node_healthy != node->is_node_healthy) {
            overall_health_changed = 1;
            node->is_node_healthy = present_is_node_healthy;
        }
    }

    return overall_health_changed;
}

/* Encode open slot states into an sds string to be persisted as an aux field in RDB. */
sds clusterEncodeOpenSlotsAuxField(int rdbflags) {
    if (!server.cluster_enabled) return NULL;

    /* Open slots should not be persisted to an RDB file. This data is intended only for full sync. */
    if ((rdbflags & RDBFLAGS_REPLICATION) == 0) return NULL;

    sds s = NULL;

    for (int i = 0; i < 2; i++) {
        clusterNode **nodes_ptr;
        if (i == 0) {
            nodes_ptr = server.cluster->importing_slots_from;
        } else {
            nodes_ptr = server.cluster->migrating_slots_to;
        }

        for (int j = 0; j < CLUSTER_SLOTS; j++) {
            if (nodes_ptr[j] == NULL) continue;
            if (s == NULL) s = sdsempty();
            s = sdscatfmt(s, "%i%s", j, (i == 0) ? "<" : ">");
            s = sdscatlen(s, nodes_ptr[j]->name, CLUSTER_NAMELEN);
            s = sdscatlen(s, ",", 1);
        }
    }

    return s;
}

/* Decode the open slot aux field and restore the in-memory slot states. */
int clusterDecodeOpenSlotsAuxField(int rdbflags, sds s) {
    if (!server.cluster_enabled || s == NULL) return C_OK;

    /* Open slots should not be loaded from a persisted RDB file, but only from a full sync. */
    if ((rdbflags & RDBFLAGS_REPLICATION) == 0) return C_OK;

    while (*s) {
        /* Extract slot number */
        int slot = atoi(s);
        if (slot < 0 || slot >= CLUSTER_SLOTS) return C_ERR;

        while (*s && *s != '<' && *s != '>') s++;
        if (*s != '<' && *s != '>') return C_ERR;

        /* Determine if it's an importing or migrating slot */
        int is_importing = (*s == '<');
        s++;

        /* Extract the node name */
        char node_name[CLUSTER_NAMELEN];
        int k = 0;
        while (*s && *s != ',' && k < CLUSTER_NAMELEN) {
            node_name[k++] = *s++;
        }

        /* Ensure the node name is of the correct length */
        if (k != CLUSTER_NAMELEN || *s != ',') return C_ERR;

        /* Move to the next slot */
        s++;

        /* Find the corresponding node */
        clusterNode *node = clusterLookupNode(node_name, CLUSTER_NAMELEN);
        if (!node) {
            /* Create a new node if not found */
            node = createClusterNode(node_name, 0);
            clusterAddNode(node);
        }

        /* Set the slot state */
        if (is_importing) {
            server.cluster->importing_slots_from[slot] = node;
        } else {
            server.cluster->migrating_slots_to[slot] = node;
        }
    }

    return C_OK;
}
