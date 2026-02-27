/*
 * Copyright (c) 2015-2017, Ieshen Zheng <ieshen.zheng at 163 dot com>
 * Copyright (c) 2020, Nick <heronr1 at gmail dot com>
 * Copyright (c) 2020-2021, Bjorn Svensson <bjorn.a.svensson at est dot tech>
 * Copyright (c) 2020-2021, Viktor Söderqvist <viktor.soderqvist at est dot tech>
 * Copyright (c) 2021, Red Hat
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
#include "fmacros.h"
#include "win32.h"

#include "cluster.h"

#include "adlist.h"
#include "alloc.h"
#include "command.h"
#include "vkutil.h"

#include <dict.h>
#include <sds.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Make sure standalone and cluster options don't overlap. */
vk_static_assert(VALKEY_OPT_USE_CLUSTER_NODES > VALKEY_OPT_LAST_SA_OPTION);

/* Internal option flags. */
#define VALKEY_FLAG_USE_CLUSTER_NODES 0x1
#define VALKEY_FLAG_PARSE_REPLICAS 0x2
#define VALKEY_FLAG_DISCONNECTING 0x4
#define VALKEY_FLAG_BLOCKING_INITIAL_UPDATE 0x8

// Cluster errors are offset by 100 to be sufficiently out of range of
// standard Valkey errors
#define VALKEY_ERR_CLUSTER_TOO_MANY_RETRIES 100

#define VALKEY_COMMAND_CLUSTER_NODES "CLUSTER NODES"
#define VALKEY_COMMAND_CLUSTER_SLOTS "CLUSTER SLOTS"
#define VALKEY_COMMAND_ASKING "ASKING"

#define CLUSTER_DEFAULT_MAX_RETRY_COUNT 5
#define NO_RETRY -1

#define SLOTMAP_UPDATE_THROTTLE_USEC 1000000
#define SLOTMAP_UPDATE_ONGOING INT64_MAX

typedef struct cluster_async_data {
    valkeyClusterAsyncContext *acc;
    struct cmd *command;
    valkeyClusterCallbackFn *callback;
    int retry_count;
    void *privdata;
} cluster_async_data;

typedef enum {
    CLUSTER_NO_ERROR = 0,
    CLUSTER_ERR_MOVED,
    CLUSTER_ERR_ASK,
    CLUSTER_ERR_TRYAGAIN,
    CLUSTER_ERR_CLUSTERDOWN,
    CLUSTER_ERR_OTHER
} replyErrorType;

static void freeValkeyClusterNode(valkeyClusterNode *node);
static void cluster_slot_destroy(cluster_slot *slot);
static int updateNodesAndSlotmap(valkeyClusterContext *cc, dict *nodes);
static int updateSlotMapAsync(valkeyClusterAsyncContext *acc,
                              valkeyAsyncContext *ac);
static int valkeyClusterSetOptionAddNodes(valkeyClusterContext *cc, const char *addrs);
static int valkeyClusterSetOptionConnectTimeout(valkeyClusterContext *cc, const struct timeval tv);
static int valkeyClusterSetOptionPassword(valkeyClusterContext *cc, const char *password);
static int valkeyClusterSetOptionUsername(valkeyClusterContext *cc, const char *username);
static int valkeyClusterAsyncConnect(valkeyClusterAsyncContext *acc);

void listClusterNodeDestructor(void *val) { freeValkeyClusterNode(val); }

void listClusterSlotDestructor(void *val) { cluster_slot_destroy(val); }

static uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char *)key, sdslen((char *)key));
}

static int dictSdsKeyCompare(const void *key1, const void *key2) {
    int l1, l2;

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2)
        return 0;
    return memcmp(key1, key2, l1) == 0;
}

static void dictSdsDestructor(void *val) {
    sdsfree(val);
}

static void dictClusterNodeDestructor(void *val) {
    freeValkeyClusterNode(val);
}

/* Destructor function for clusterNodeListDictType. */
static void dictClusterNodeListDestructor(void *val) {
    listRelease(val);
}

/* Cluster node hash table
 * maps node address (1.2.3.4:6379) to a valkeyClusterNode
 * Has ownership of valkeyClusterNode memory
 */
dictType clusterNodesDictType = {
    .hashFunction = dictSdsHash,
    .keyCompare = dictSdsKeyCompare,
    .keyDestructor = dictSdsDestructor,
    .valDestructor = dictClusterNodeDestructor};

/* Hash table dictType to map node address to a list of valkeyClusterNodes. */
dictType clusterNodeListDictType = {
    .hashFunction = dictSdsHash,
    .keyCompare = dictSdsKeyCompare,
    .keyDestructor = dictSdsDestructor,
    .valDestructor = dictClusterNodeListDestructor};

void listCommandFree(void *command) {
    struct cmd *cmd = command;
    command_destroy(cmd);
}

/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
static unsigned int keyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */

    for (s = 0; s < keylen; s++)
        if (key[s] == '{')
            break;

    /* No '{' ? Hash the whole key. This is the base case. */
    if (s == keylen)
        return crc16(key, keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    for (e = s + 1; e < keylen; e++)
        if (key[e] == '}')
            break;

    /* No '}' or nothing between {} ? Hash the whole key. */
    if (e == keylen || e == s + 1)
        return crc16(key, keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    return crc16(key + s + 1, e - s - 1) & 0x3FFF;
}

static void valkeyClusterSetError(valkeyClusterContext *cc, int type,
                                  const char *str) {
    cc->err = type;

    assert(str != NULL);
    if (str != NULL && str != cc->errstr) {
        size_t len = strlen(str);
        len = len < (sizeof(cc->errstr) - 1) ? len : (sizeof(cc->errstr) - 1);
        memcpy(cc->errstr, str, len);
        cc->errstr[len] = '\0';
    }
}

static inline void valkeyClusterClearError(valkeyClusterContext *cc) {
    cc->err = 0;
    cc->errstr[0] = '\0';
}

static replyErrorType getReplyErrorType(valkeyReply *reply) {
    assert(reply);

    if (reply->type != VALKEY_REPLY_ERROR)
        return CLUSTER_NO_ERROR;
    if (memcmp(reply->str, "MOVED", 5) == 0)
        return CLUSTER_ERR_MOVED;
    if (memcmp(reply->str, "ASK", 3) == 0)
        return CLUSTER_ERR_ASK;
    if (memcmp(reply->str, "TRYAGAIN", 8) == 0)
        return CLUSTER_ERR_TRYAGAIN;
    if (memcmp(reply->str, "CLUSTERDOWN", 11) == 0)
        return CLUSTER_ERR_CLUSTERDOWN;
    return CLUSTER_ERR_OTHER;
}

/* Create and initiate the cluster node structure */
static valkeyClusterNode *createValkeyClusterNode(void) {
    /* use calloc to guarantee all fields are zeroed */
    return vk_calloc(1, sizeof(valkeyClusterNode));
}

/* Cleanup the cluster node structure */
static void freeValkeyClusterNode(valkeyClusterNode *node) {
    if (node == NULL) {
        return;
    }

    sdsfree(node->name);
    sdsfree(node->addr);
    sdsfree(node->host);
    valkeyFree(node->con);

    if (node->acon != NULL) {
        /* Detach this cluster node from the async context. This makes sure
         * that valkeyAsyncFree() wont attempt to update the pointer via its
         * dataCleanup and unlinkAsyncContextAndNode() */
        node->acon->data = NULL;
        valkeyAsyncFree(node->acon);
    }
    listRelease(node->slots);
    listRelease(node->replicas);
    vk_free(node);
}

static cluster_slot *cluster_slot_create(valkeyClusterNode *node) {
    cluster_slot *slot;

    slot = vk_calloc(1, sizeof(*slot));
    if (slot == NULL) {
        return NULL;
    }
    slot->node = node;

    if (node != NULL) {
        assert(node->role == VALKEY_ROLE_PRIMARY);
        if (node->slots == NULL) {
            node->slots = listCreate();
            if (node->slots == NULL) {
                cluster_slot_destroy(slot);
                return NULL;
            }

            node->slots->free = listClusterSlotDestructor;
        }

        if (listAddNodeTail(node->slots, slot) == NULL) {
            cluster_slot_destroy(slot);
            return NULL;
        }
    }

    return slot;
}

static int cluster_slot_ref_node(cluster_slot *slot, valkeyClusterNode *node) {
    if (slot == NULL || node == NULL) {
        return VALKEY_ERR;
    }

    if (node->role != VALKEY_ROLE_PRIMARY) {
        return VALKEY_ERR;
    }

    if (node->slots == NULL) {
        node->slots = listCreate();
        if (node->slots == NULL) {
            return VALKEY_ERR;
        }

        node->slots->free = listClusterSlotDestructor;
    }

    if (listAddNodeTail(node->slots, slot) == NULL) {
        return VALKEY_ERR;
    }
    slot->node = node;

    return VALKEY_OK;
}

static void cluster_slot_destroy(cluster_slot *slot) {
    if (slot == NULL)
        return;
    slot->start = 0;
    slot->end = 0;
    slot->node = NULL;

    vk_free(slot);
}

/**
 * Handle password authentication in the synchronous API
 */
static int authenticate(valkeyClusterContext *cc, valkeyContext *c) {
    if (cc == NULL || c == NULL) {
        return VALKEY_ERR;
    }

    // Skip if no password configured
    if (cc->password == NULL) {
        return VALKEY_OK;
    }

    valkeyReply *reply;
    if (cc->username != NULL) {
        reply = valkeyCommand(c, "AUTH %s %s", cc->username, cc->password);
    } else {
        reply = valkeyCommand(c, "AUTH %s", cc->password);
    }

    if (reply == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "Command AUTH reply error (NULL)");
        goto error;
    }

    if (reply->type == VALKEY_REPLY_ERROR) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, reply->str);
        goto error;
    }

    freeReplyObject(reply);
    return VALKEY_OK;

error:
    freeReplyObject(reply);

    return VALKEY_ERR;
}

/* Select a logical database by sending the SELECT command. */
static int select_db(valkeyClusterContext *cc, valkeyContext *c) {
    if (cc->select_db == 0)
        return VALKEY_OK;

    valkeyReply *reply = valkeyCommand(c, "SELECT %d", cc->select_db);
    if (reply == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Failed to select logical database");
        return VALKEY_ERR;
    }
    if (reply->type == VALKEY_REPLY_ERROR) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, reply->str);
        freeReplyObject(reply);
        return VALKEY_ERR;
    }
    freeReplyObject(reply);
    return VALKEY_OK;
}

/**
 * Return a new node with the "cluster slots" command reply.
 */
static valkeyClusterNode *node_get_with_slots(valkeyClusterContext *cc,
                                              char *host, int port,
                                              uint8_t role) {
    valkeyClusterNode *node = createValkeyClusterNode();
    if (node == NULL) {
        goto oom;
    }

    if (role == VALKEY_ROLE_PRIMARY) {
        node->slots = listCreate();
        if (node->slots == NULL) {
            goto oom;
        }

        node->slots->free = listClusterSlotDestructor;
    }

    node->addr = sdsnew(host);
    if (node->addr == NULL) {
        goto oom;
    }
    node->addr = sdscatfmt(node->addr, ":%i", port);
    if (node->addr == NULL) {
        goto oom;
    }
    node->host = sdsnew(host);
    if (node->host == NULL) {
        goto oom;
    }
    node->name = NULL;
    node->port = port;
    node->role = role;

    return node;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    if (node != NULL) {
        sdsfree(node->addr);
        sdsfree(node->host);
        vk_free(node);
    }
    return NULL;
}

static void cluster_nodes_swap_ctx(dict *nodes_f, dict *nodes_t) {
    dictEntry *de_f, *de_t;
    valkeyClusterNode *node_f, *node_t;
    valkeyContext *c;
    valkeyAsyncContext *ac;

    if (nodes_f == NULL || nodes_t == NULL) {
        return;
    }

    dictIterator di;
    dictInitIterator(&di, nodes_t);

    while ((de_t = dictNext(&di)) != NULL) {
        node_t = dictGetVal(de_t);
        if (node_t == NULL) {
            continue;
        }

        de_f = dictFind(nodes_f, node_t->addr);
        if (de_f == NULL) {
            continue;
        }

        node_f = dictGetVal(de_f);
        if (node_f->con != NULL) {
            c = node_f->con;
            node_f->con = node_t->con;
            node_t->con = c;
        }

        if (node_f->acon != NULL) {
            ac = node_f->acon;
            node_f->acon = node_t->acon;
            node_t->acon = ac;

            node_t->acon->data = node_t;
            if (node_f->acon)
                node_f->acon->data = node_f;
        }
    }
}

/**
 * Parse the "cluster slots" command reply to nodes dict.
 */
static dict *parse_cluster_slots(valkeyClusterContext *cc, valkeyContext *c,
                                 valkeyReply *reply) {
    int ret;
    cluster_slot *slot = NULL;
    dict *nodes = NULL;
    dictEntry *den;
    valkeyReply *elem_slots;
    valkeyReply *elem_slots_begin, *elem_slots_end;
    valkeyReply *elem_nodes;
    valkeyReply *elem_ip, *elem_port;
    valkeyClusterNode *primary = NULL, *replica;
    uint32_t i, idx;

    if (reply->type != VALKEY_REPLY_ARRAY) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Unexpected reply type");
        goto error;
    }
    if (reply->elements == 0) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "No slot information");
        goto error;
    }

    nodes = dictCreate(&clusterNodesDictType);
    if (nodes == NULL) {
        goto oom;
    }

    for (i = 0; i < reply->elements; i++) {
        elem_slots = reply->element[i];
        if (elem_slots->type != VALKEY_REPLY_ARRAY ||
            elem_slots->elements < 3) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "Command(cluster slots) reply error: "
                                  "first sub_reply is not an array.");
            goto error;
        }

        slot = cluster_slot_create(NULL);
        if (slot == NULL) {
            goto oom;
        }

        // one slots region
        for (idx = 0; idx < elem_slots->elements; idx++) {
            if (idx == 0) {
                elem_slots_begin = elem_slots->element[idx];
                if (elem_slots_begin->type != VALKEY_REPLY_INTEGER) {
                    valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                          "Command(cluster slots) reply error: "
                                          "slot begin is not an integer.");
                    goto error;
                }
                slot->start = (int)(elem_slots_begin->integer);
            } else if (idx == 1) {
                elem_slots_end = elem_slots->element[idx];
                if (elem_slots_end->type != VALKEY_REPLY_INTEGER) {
                    valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                          "Command(cluster slots) reply error: "
                                          "slot end is not an integer.");
                    goto error;
                }

                slot->end = (int)(elem_slots_end->integer);

                if (slot->start > slot->end) {
                    valkeyClusterSetError(
                        cc, VALKEY_ERR_OTHER,
                        "Command(cluster slots) reply error: "
                        "slot begin is bigger than slot end.");
                    goto error;
                }
            } else {
                elem_nodes = elem_slots->element[idx];
                if (elem_nodes->type != VALKEY_REPLY_ARRAY ||
                    elem_nodes->elements < 2) {
                    valkeyClusterSetError(
                        cc, VALKEY_ERR_OTHER,
                        "Command(cluster slots) reply error: "
                        "nodes sub_reply is not a correct array.");
                    goto error;
                }

                elem_ip = elem_nodes->element[0];
                elem_port = elem_nodes->element[1];

                /* Validate ip element. Accept a NULL value ip (NIL type) since
                 * we will handle the unknown endpoint special. */
                if (elem_ip == NULL || (elem_ip->type != VALKEY_REPLY_STRING &&
                                        elem_ip->type != VALKEY_REPLY_NIL)) {
                    valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid node address");
                    goto error;
                }

                /* Validate port element. */
                if (elem_port == NULL || elem_port->type != VALKEY_REPLY_INTEGER ||
                    (elem_port->integer < 1 || elem_port->integer > UINT16_MAX)) {
                    valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid port");
                    goto error;
                }

                /* Get the received ip/host. According to the docs an unknown
                 * endpoint or an empty string can be treated as it means
                 * the same address as we sent this command to.
                 * An unknown endpoint has the type VALKEY_REPLY_NIL and its
                 * length is initiated to zero. */
                char *host = (elem_ip->len > 0) ? elem_ip->str : c->tcp.host;
                if (host == NULL) {
                    goto oom;
                }
                int port = elem_port->integer;

                if (idx == 2) {
                    /* Parse a primary node. */
                    sds address = sdsnew(host);
                    if (address == NULL) {
                        goto oom;
                    }
                    address = sdscatfmt(address, ":%i", port);
                    if (address == NULL) {
                        goto oom;
                    }

                    den = dictFind(nodes, address);
                    sdsfree(address);
                    if (den != NULL) {
                        /* Skip parsing this primary node since it's already known. */
                        primary = dictGetVal(den);
                        ret = cluster_slot_ref_node(slot, primary);
                        if (ret != VALKEY_OK) {
                            goto oom;
                        }

                        slot = NULL;
                        break;
                    }

                    primary = node_get_with_slots(cc, host, port, VALKEY_ROLE_PRIMARY);
                    if (primary == NULL) {
                        goto error;
                    }

                    sds key = sdsnewlen(primary->addr, sdslen(primary->addr));
                    if (key == NULL) {
                        freeValkeyClusterNode(primary);
                        goto oom;
                    }

                    ret = dictAdd(nodes, key, primary);
                    if (ret != DICT_OK) {
                        sdsfree(key);
                        freeValkeyClusterNode(primary);
                        goto oom;
                    }

                    ret = cluster_slot_ref_node(slot, primary);
                    if (ret != VALKEY_OK) {
                        goto oom;
                    }

                    slot = NULL;
                } else if (cc->flags & VALKEY_FLAG_PARSE_REPLICAS) {
                    replica = node_get_with_slots(cc, host, port,
                                                  VALKEY_ROLE_REPLICA);
                    if (replica == NULL) {
                        goto error;
                    }

                    if (primary->replicas == NULL) {
                        primary->replicas = listCreate();
                        if (primary->replicas == NULL) {
                            freeValkeyClusterNode(replica);
                            goto oom;
                        }

                        primary->replicas->free = listClusterNodeDestructor;
                    }

                    if (listAddNodeTail(primary->replicas, replica) == NULL) {
                        freeValkeyClusterNode(replica);
                        goto oom;
                    }
                }
            }
        }
    }

    return nodes;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    dictRelease(nodes);
    cluster_slot_destroy(slot);
    return NULL;
}

/* Keep lists of parsed replica nodes in a dict using the primary_id as key. */
static int retain_replica_node(dict *replicas, char *primary_id, valkeyClusterNode *node) {
    sds key = sdsnew(primary_id);
    if (key == NULL)
        return VALKEY_ERR;

    struct hilist *replicaList;

    dictEntry *de = dictFind(replicas, key);
    if (de == NULL) {
        /* Create list to hold replicas for a primary. */
        replicaList = listCreate();
        if (replicaList == NULL) {
            sdsfree(key);
            return VALKEY_ERR;
        }
        replicaList->free = listClusterNodeDestructor;
        if (dictAdd(replicas, key, replicaList) != DICT_OK) {
            sdsfree(key);
            listRelease(replicaList);
            return VALKEY_ERR;
        }
    } else {
        sdsfree(key);
        replicaList = dictGetVal(de);
    }

    if (listAddNodeTail(replicaList, node) == NULL)
        return VALKEY_ERR;

    return VALKEY_OK;
}

/* Store parsed replica nodes in the primary nodes, which holds a list of replica
 * nodes. The `replicas` dict shall contain lists of nodes with primary_id as key. */
static int store_replica_nodes(dict *nodes, dict *replicas) {
    if (replicas == NULL)
        return VALKEY_OK;

    dictIterator di;
    dictInitIterator(&di, nodes);
    dictEntry *de;
    while ((de = dictNext(&di))) {
        valkeyClusterNode *primary = dictGetVal(de);

        /* Move replica nodes related to this primary. */
        dictEntry *der = dictFind(replicas, primary->name);
        if (der != NULL) {
            assert(primary->replicas == NULL);
            /* Move replica list from replicas dict to nodes dict. */
            primary->replicas = dictGetVal(der);
            dictSetVal(replicas, der, NULL);
        }
    }
    return VALKEY_OK;
}

/* Parse a node from a single CLUSTER NODES line.
 * Returns VALKEY_OK and an allocated valkeyClusterNode as a pointer in
 * `parsed_node`, or VALKEY_ERR when the parsing fails.
 * Only parse primary nodes if the `parsed_primary_id` argument is NULL,
 * otherwise replicas are also parsed and its primary_id is returned by pointer
 * via 'parsed_primary_id'.
 * The valkeyContext used when sending the CLUSTER NODES command should be
 * provided in `c` since its destination IP address is used when no IP address
 * is found in the parsed string. */
static int parse_cluster_nodes_line(valkeyClusterContext *cc, valkeyContext *c, char *line,
                                    valkeyClusterNode **parsed_node, char **parsed_primary_id) {
    char *p, *id = NULL, *addr = NULL, *flags = NULL, *primary_id = NULL,
             *link_state = NULL, *slots = NULL;
    /* Find required fields and keep a pointer to each field:
     * <id> <addr> <flags> <primary_id> <ping-sent> <pong-recv> <config-epoch> <link-state> [<slot> ...]
     */
    // clang-format off
    int i = 0;
    while ((p = strchr(line, ' ')) != NULL) {
        *p = '\0';
        switch (i++) {
            case 0: id = line; break;
            case 1: addr = line; break;
            case 2: flags = line; break;
            case 3: primary_id = line; break;
            case 7: link_state = line; break;
        }
        line = p + 1; /* Start of next field. */
        if (i == 8) { slots = line; break; }
    }
    if (i == 7 && line[0] != '\0') link_state = line;
    // clang-format on

    if (link_state == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Mandatory fields missing");
        return VALKEY_ERR;
    }

    /* Parse flags, a comma separated list of following flags:
     * myself, master, slave, fail?, fail, handshake, noaddr, nofailover, noflags. */
    uint8_t role = VALKEY_ROLE_UNKNOWN;
    while (*flags != '\0') {
        if ((p = strchr(flags, ',')) != NULL)
            *p = '\0';
        if (memcmp(flags, "master", 6) == 0)
            role = VALKEY_ROLE_PRIMARY;
        else if (memcmp(flags, "slave", 5) == 0)
            role = VALKEY_ROLE_REPLICA;
        else if (memcmp(flags, "noaddr", 6) == 0) {
            *parsed_node = NULL;
            return VALKEY_OK; /* Skip nodes with 'noaddr'. */
        }
        if (p == NULL) /* No more flags. */
            break;
        flags = p + 1; /* Start of next flag. */
    }
    if (role == VALKEY_ROLE_UNKNOWN) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Unknown role");
        return VALKEY_ERR;
    }

    /* Only parse replicas when requested. */
    if (role == VALKEY_ROLE_REPLICA && parsed_primary_id == NULL) {
        *parsed_node = NULL;
        return VALKEY_OK;
    }

    valkeyClusterNode *node = createValkeyClusterNode();
    if (node == NULL) {
        goto oom;
    }
    node->role = role;
    node->name = sdsnew(id);
    if (node->name == NULL)
        goto oom;

    /* Parse the address field: <ip:port@cport[,hostname]>
     * Remove @cport.. to get <ip>:<port> which is our dict key. */
    if ((p = strchr(addr, '@')) != NULL) {
        *p = '\0';
    }

    /* Find the required port separator. */
    if ((p = strrchr(addr, ':')) == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid node address");
        freeValkeyClusterNode(node);
        return VALKEY_ERR;
    }

    /* Get the port (skip the found port separator). */
    int port = vk_atoi(p + 1, strlen(p + 1));
    if (port < 1 || port > UINT16_MAX) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid port");
        freeValkeyClusterNode(node);
        return VALKEY_ERR;
    }
    node->port = port;

    /* Check that we received an ip/host address, i.e. the field
     * does not start with the port separator. */
    if (p != addr) {
        node->addr = sdsnew(addr);
        if (node->addr == NULL)
            goto oom;

        *p = '\0'; /* Cut port separator. */

        node->host = sdsnew(addr);
        if (node->host == NULL)
            goto oom;

    } else {
        /* We received an ip/host that is an empty string. According to the docs
         * we can treat this as it means the same address we sent this command to. */
        node->host = sdsnew(c->tcp.host);
        if (node->host == NULL) {
            goto oom;
        }
        /* Create a new addr field using correct host:port */
        node->addr = sdsnew(node->host);
        if (node->addr == NULL) {
            goto oom;
        }
        node->addr = sdscatfmt(node->addr, ":%i", node->port);
        if (node->addr == NULL) {
            goto oom;
        }
    }

    /* No slot parsing needed for replicas, but return primary id. */
    if (node->role == VALKEY_ROLE_REPLICA) {
        *parsed_primary_id = primary_id;
        *parsed_node = node;
        return VALKEY_OK;
    }

    node->slots = listCreate();
    if (node->slots == NULL)
        goto oom;
    node->slots->free = listClusterSlotDestructor;

    /* Parse slots when available. */
    if (slots == NULL) {
        *parsed_node = node;
        return VALKEY_OK;
    }
    /* Parse each slot element. */
    while (*slots != '\0') {
        if ((p = strchr(slots, ' ')) != NULL)
            *p = '\0';
        char *entry = slots;
        if (entry[0] == '[')
            break; /* Skip importing/migrating slots at string end. */

        int slot_start, slot_end;
        char *sp = strchr(entry, '-');
        if (sp == NULL) {
            slot_start = vk_atoi(entry, strlen(entry));
            slot_end = slot_start;
        } else {
            *sp = '\0';
            slot_start = vk_atoi(entry, strlen(entry));
            entry = sp + 1; // Skip '-'
            slot_end = vk_atoi(entry, strlen(entry));
        }

        /* Create a slot entry owned by the node. */
        cluster_slot *slot = cluster_slot_create(node);
        if (slot == NULL)
            goto oom;
        slot->start = (uint32_t)slot_start;
        slot->end = (uint32_t)slot_end;

        if (p == NULL) /* Check if this was the last entry. */
            break;
        slots = p + 1; /* Start of next entry. */
    }
    *parsed_node = node;
    return VALKEY_OK;

oom:
    freeValkeyClusterNode(node);
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    return VALKEY_ERR;
}

/**
 * Parse the "cluster nodes" command reply to nodes dict.
 */
static dict *parse_cluster_nodes(valkeyClusterContext *cc, valkeyContext *c, valkeyReply *reply) {
    dict *nodes = NULL;
    int slot_ranges_found = 0;
    int add_replicas = cc->flags & VALKEY_FLAG_PARSE_REPLICAS;
    dict *replicas = NULL;

    if (reply->type != VALKEY_REPLY_STRING && reply->type != VALKEY_REPLY_VERB) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Unexpected reply type");
        goto error;
    }

    nodes = dictCreate(&clusterNodesDictType);
    if (nodes == NULL) {
        goto oom;
    }

    char *lines = reply->str; /* NULL terminated string. */
    char *p, *line;
    while ((p = strchr(lines, '\n')) != NULL) {
        *p = '\0';
        line = lines;
        lines = p + 1; /* Start of next line. */

        char *primary_id;
        valkeyClusterNode *node;
        if (parse_cluster_nodes_line(cc, c, line, &node, add_replicas ? &primary_id : NULL) != VALKEY_OK)
            goto error;
        if (node == NULL)
            continue; /* Line skipped. */
        if (node->role == VALKEY_ROLE_PRIMARY) {
            sds key = sdsnew(node->addr);
            if (key == NULL) {
                freeValkeyClusterNode(node);
                goto oom;
            }
            if (dictFind(nodes, key) != NULL) {
                valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                      "Duplicate addresses in cluster nodes response");
                sdsfree(key);
                freeValkeyClusterNode(node);
                goto error;
            }
            if (dictAdd(nodes, key, node) != DICT_OK) {
                sdsfree(key);
                freeValkeyClusterNode(node);
                goto oom;
            }
            slot_ranges_found += listLength(node->slots);

        } else {
            assert(node->role == VALKEY_ROLE_REPLICA);
            if (replicas == NULL) {
                if ((replicas = dictCreate(&clusterNodeListDictType)) == NULL) {
                    freeValkeyClusterNode(node);
                    goto oom;
                }
            }
            /* Retain parsed replica nodes until all primaries are parsed. */
            if (retain_replica_node(replicas, primary_id, node) != VALKEY_OK) {
                freeValkeyClusterNode(node);
                goto oom;
            }
        }
    }

    if (slot_ranges_found == 0) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "No slot information");
        goto error;
    }

    /* Store the retained replica nodes in primary nodes. */
    if (store_replica_nodes(nodes, replicas) != VALKEY_OK) {
        goto oom;
    }
    dictRelease(replicas);

    return nodes;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    dictRelease(replicas);
    dictRelease(nodes);
    return NULL;
}

/* Sends CLUSTER SLOTS or CLUSTER NODES to the node with context c. */
static int clusterUpdateRouteSendCommand(valkeyClusterContext *cc,
                                         valkeyContext *c) {
    const char *cmd = (cc->flags & VALKEY_FLAG_USE_CLUSTER_NODES ?
                           VALKEY_COMMAND_CLUSTER_NODES :
                           VALKEY_COMMAND_CLUSTER_SLOTS);
    if (valkeyAppendCommand(c, cmd) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }
    /* Flush buffer to socket. */
    if (valkeyBufferWrite(c, NULL) == VALKEY_ERR) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

/* Receives and handles a CLUSTER SLOTS or CLUSTER NODES reply from node with
 * context c. */
static int clusterUpdateRouteHandleReply(valkeyClusterContext *cc,
                                         valkeyContext *c) {
    valkeyReply *reply = NULL;
    if (valkeyGetReply(c, (void **)&reply) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }
    if (reply->type == VALKEY_REPLY_ERROR) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, reply->str);
        freeReplyObject(reply);
        return VALKEY_ERR;
    }

    dict *nodes;
    if (cc->flags & VALKEY_FLAG_USE_CLUSTER_NODES) {
        nodes = parse_cluster_nodes(cc, c, reply);
    } else {
        nodes = parse_cluster_slots(cc, c, reply);
    }
    freeReplyObject(reply);
    return updateNodesAndSlotmap(cc, nodes);
}

/* Update known cluster nodes with a new collection of valkeyClusterNodes.
 * Will also update the slot-to-node lookup table for the new nodes. */
static int updateNodesAndSlotmap(valkeyClusterContext *cc, dict *nodes) {
    if (nodes == NULL) {
        return VALKEY_ERR;
    }

    /* Create a slot to valkeyClusterNode lookup table */
    valkeyClusterNode **table;
    table = vk_calloc(VALKEYCLUSTER_SLOTS, sizeof(valkeyClusterNode *));
    if (table == NULL) {
        goto oom;
    }

    dictIterator di;
    dictInitIterator(&di, nodes);

    dictEntry *de;
    while ((de = dictNext(&di))) {
        valkeyClusterNode *node = dictGetVal(de);
        if (node->role != VALKEY_ROLE_PRIMARY) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "Node role must be primary");
            goto error;
        }

        if (node->slots == NULL) {
            continue;
        }

        listIter li;
        listRewind(node->slots, &li);

        listNode *ln;
        while ((ln = listNext(&li))) {
            cluster_slot *slot = listNodeValue(ln);
            if (slot->start > slot->end || slot->end >= VALKEYCLUSTER_SLOTS) {
                valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                      "Slot region for node is invalid");
                goto error;
            }
            for (uint32_t i = slot->start; i <= slot->end; i++) {
                if (table[i] != NULL) {
                    valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                          "Different node holds same slot");
                    goto error;
                }
                table[i] = node;
            }
        }
    }

    /* Update slot-to-node table before changing cc->nodes since
     * removal of nodes might trigger user callbacks which may
     * send commands, which depend on the slot-to-node table. */
    if (cc->table != NULL) {
        vk_free(cc->table);
    }
    cc->table = table;

    cc->route_version++;

    // Move all libvalkey contexts in cc->nodes to nodes
    cluster_nodes_swap_ctx(cc->nodes, nodes);

    /* Replace cc->nodes before releasing the old dict since
     * the release procedure might access cc->nodes. */
    dict *oldnodes = cc->nodes;
    cc->nodes = nodes;
    dictRelease(oldnodes);

    if (cc->event_callback != NULL) {
        cc->event_callback(cc, VALKEYCLUSTER_EVENT_SLOTMAP_UPDATED,
                           cc->event_privdata);
        if (cc->route_version == 1) {
            /* Special event the first time the slotmap was updated. */
            cc->event_callback(cc, VALKEYCLUSTER_EVENT_READY,
                               cc->event_privdata);
        }
    }
    cc->need_update_route = 0;
    return VALKEY_OK;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough
error:
    vk_free(table);
    dictRelease(nodes);
    return VALKEY_ERR;
}

int valkeyClusterUpdateSlotmap(valkeyClusterContext *cc) {
    if (cc == NULL) {
        return VALKEY_ERR;
    }

    valkeyClusterNode *node;
    dictEntry *de;

    dictIterator di;
    dictInitIterator(&di, cc->nodes);

    while ((de = dictNext(&di)) != NULL) {
        node = dictGetVal(de);

        /* Use existing connection or (re)connect to the node. */
        valkeyContext *c = valkeyClusterGetValkeyContext(cc, node);
        if (c == NULL)
            continue;
        if (clusterUpdateRouteSendCommand(cc, c) != VALKEY_OK ||
            clusterUpdateRouteHandleReply(cc, c) != VALKEY_OK) {
            valkeyFree(node->con);
            node->con = NULL;
            continue;
        }
        valkeyClusterClearError(cc);
        return VALKEY_OK;
    }

    return VALKEY_ERR;
}

static int valkeyClusterContextInit(valkeyClusterContext *cc,
                                    const valkeyClusterOptions *options) {
    cc->nodes = dictCreate(&clusterNodesDictType);
    if (cc->nodes == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }
    cc->requests = listCreate();
    if (cc->requests == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }
    cc->requests->free = listCommandFree;

    int supported_options = (VALKEY_OPT_USE_CLUSTER_NODES | VALKEY_OPT_USE_REPLICAS |
                             VALKEY_OPT_BLOCKING_INITIAL_UPDATE | VALKEY_OPT_REUSEADDR |
                             VALKEY_OPT_PREFER_IPV4 | VALKEY_OPT_PREFER_IPV6 |
                             VALKEY_OPT_PREFER_IP_UNSPEC | VALKEY_OPT_MPTCP);
    if (options->options & ~supported_options) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Unsupported options");
        return VALKEY_ERR;
    }
    cc->options = options->options;

    if (options->options & VALKEY_OPT_USE_CLUSTER_NODES) {
        cc->flags |= VALKEY_FLAG_USE_CLUSTER_NODES;
    }
    if (options->options & VALKEY_OPT_USE_REPLICAS) {
        cc->flags |= VALKEY_FLAG_PARSE_REPLICAS;
    }
    if (options->options & VALKEY_OPT_BLOCKING_INITIAL_UPDATE) {
        cc->flags |= VALKEY_FLAG_BLOCKING_INITIAL_UPDATE;
    }
    if (options->max_retry > 0) {
        cc->max_retry_count = options->max_retry;
    } else {
        cc->max_retry_count = CLUSTER_DEFAULT_MAX_RETRY_COUNT;
    }
    if (options->select_db > 0) {
        cc->select_db = options->select_db;
    }
    if (options->initial_nodes != NULL &&
        valkeyClusterSetOptionAddNodes(cc, options->initial_nodes) != VALKEY_OK) {
        return VALKEY_ERR; /* err and errstr already set. */
    }
    if (options->connect_timeout != NULL &&
        valkeyClusterSetOptionConnectTimeout(cc, *options->connect_timeout) != VALKEY_OK) {
        return VALKEY_ERR; /* err and errstr already set. */
    }
    if (options->command_timeout != NULL &&
        valkeyClusterSetOptionTimeout(cc, *options->command_timeout) != VALKEY_OK) {
        return VALKEY_ERR; /* err and errstr already set. */
    }
    if (options->username != NULL &&
        valkeyClusterSetOptionUsername(cc, options->username) != VALKEY_OK) {
        return VALKEY_ERR; /* err and errstr already set. */
    }
    if (options->password != NULL &&
        valkeyClusterSetOptionPassword(cc, options->password) != VALKEY_OK) {
        return VALKEY_ERR; /* err and errstr already set. */
    }
    if (options->connect_callback) {
        cc->on_connect = options->connect_callback;
    }
    if (options->event_callback) {
        cc->event_callback = options->event_callback;
        cc->event_privdata = options->event_privdata;
    }
    if (options->tls) {
        cc->tls = options->tls;
        cc->tls_init_fn = options->tls_init_fn;
    }

    return VALKEY_OK;
}

void valkeyClusterFree(valkeyClusterContext *cc) {
    if (cc == NULL)
        return;

    if (cc->event_callback) {
        cc->event_callback(cc, VALKEYCLUSTER_EVENT_FREE_CONTEXT,
                           cc->event_privdata);
    }

    vk_free(cc->connect_timeout);
    vk_free(cc->command_timeout);
    vk_free(cc->username);
    vk_free(cc->password);
    vk_free(cc->table);
    dictRelease(cc->nodes);
    listRelease(cc->requests);

    memset(cc, 0xff, sizeof(*cc));
    vk_free(cc);
}

valkeyClusterContext *valkeyClusterConnectWithOptions(const valkeyClusterOptions *options) {
    valkeyClusterContext *cc;

    cc = vk_calloc(1, sizeof(valkeyClusterContext));
    if (cc == NULL)
        return NULL;

    if (valkeyClusterContextInit(cc, options) == VALKEY_OK) {
        /* Only connect if options are ok. */
        valkeyClusterUpdateSlotmap(cc);
    }
    return cc;
}

valkeyClusterContext *valkeyClusterConnect(const char *addrs) {
    valkeyClusterOptions options = {0};
    options.initial_nodes = addrs;

    return valkeyClusterConnectWithOptions(&options);
}

valkeyClusterContext *valkeyClusterConnectWithTimeout(const char *addrs,
                                                      const struct timeval tv) {
    valkeyClusterOptions options = {0};
    options.initial_nodes = addrs;
    options.connect_timeout = &tv;

    return valkeyClusterConnectWithOptions(&options);
}

static int valkeyClusterSetOptionAddNode(valkeyClusterContext *cc, const char *addr) {
    dictEntry *node_entry;
    valkeyClusterNode *node = NULL;
    int port, ret;
    sds ip = NULL;

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    sds addr_sds = sdsnew(addr);
    if (addr_sds == NULL) {
        goto oom;
    }
    node_entry = dictFind(cc->nodes, addr_sds);
    sdsfree(addr_sds);
    if (node_entry == NULL) {
        char *p;

        /* Find the last occurrence of the port separator since
         * IPv6 addresses can contain ':' */
        if ((p = strrchr(addr, ':')) == NULL) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "server address is incorrect, port separator missing.");
            return VALKEY_ERR;
        }
        // p includes separator

        if (p - addr <= 0) { /* length until separator */
            valkeyClusterSetError(
                cc, VALKEY_ERR_OTHER,
                "server address is incorrect, address part missing.");
            return VALKEY_ERR;
        }

        ip = sdsnewlen(addr, p - addr);
        if (ip == NULL) {
            goto oom;
        }
        p++; // remove separator character

        if (strlen(p) <= 0) {
            valkeyClusterSetError(
                cc, VALKEY_ERR_OTHER,
                "server address is incorrect, port part missing.");
            goto error;
        }

        port = vk_atoi(p, strlen(p));
        if (port <= 0) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "server port is incorrect");
            goto error;
        }

        node = createValkeyClusterNode();
        if (node == NULL) {
            goto oom;
        }

        node->addr = sdsnew(addr);
        if (node->addr == NULL) {
            goto oom;
        }

        node->host = ip;
        node->port = port;

        sds key = sdsnewlen(node->addr, sdslen(node->addr));
        if (key == NULL) {
            goto oom;
        }
        ret = dictAdd(cc->nodes, key, node);
        if (ret != DICT_OK) {
            sdsfree(key);
            goto oom;
        }
    }

    return VALKEY_OK;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    sdsfree(ip);
    if (node != NULL) {
        sdsfree(node->addr);
        vk_free(node);
    }
    return VALKEY_ERR;
}

static int valkeyClusterSetOptionAddNodes(valkeyClusterContext *cc,
                                          const char *addrs) {
    int ret;
    sds *address = NULL;
    int address_count = 0;
    int i;

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    /* Split into individual addresses. */
    address = sdssplitlen(addrs, strlen(addrs), ",", strlen(","), &address_count);
    if (address == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    if (address_count <= 0) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "invalid server addresses (example format: "
                              "127.0.0.1:1234,127.0.0.2:5678)");
        sdsfreesplitres(address, address_count);
        return VALKEY_ERR;
    }

    for (i = 0; i < address_count; i++) {
        ret = valkeyClusterSetOptionAddNode(cc, address[i]);
        if (ret != VALKEY_OK) {
            sdsfreesplitres(address, address_count);
            return VALKEY_ERR;
        }
    }

    sdsfreesplitres(address, address_count);

    return VALKEY_OK;
}

/**
 * Configure a username used during authentication, see
 * the Valkey AUTH command.
 * Disabled by default. Can be disabled again by providing an
 * empty string or a null pointer.
 */
static int valkeyClusterSetOptionUsername(valkeyClusterContext *cc,
                                          const char *username) {
    if (cc == NULL) {
        return VALKEY_ERR;
    }

    // Disabling option
    if (username == NULL || username[0] == '\0') {
        vk_free(cc->username);
        cc->username = NULL;
        return VALKEY_OK;
    }

    vk_free(cc->username);
    cc->username = vk_strdup(username);
    if (cc->username == NULL) {
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

/**
 * Configure a password used when connecting to password-protected
 * Valkey instances. (See Valkey AUTH command)
 */
static int valkeyClusterSetOptionPassword(valkeyClusterContext *cc,
                                          const char *password) {

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    // Disabling use of password
    if (password == NULL || password[0] == '\0') {
        vk_free(cc->password);
        cc->password = NULL;
        return VALKEY_OK;
    }

    vk_free(cc->password);
    cc->password = vk_strdup(password);
    if (cc->password == NULL) {
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

static int valkeyClusterSetOptionConnectTimeout(valkeyClusterContext *cc,
                                                const struct timeval tv) {

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    if (cc->connect_timeout == NULL) {
        cc->connect_timeout = vk_malloc(sizeof(struct timeval));
        if (cc->connect_timeout == NULL) {
            valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
            return VALKEY_ERR;
        }
    }

    memcpy(cc->connect_timeout, &tv, sizeof(struct timeval));

    return VALKEY_OK;
}

int valkeyClusterSetOptionTimeout(valkeyClusterContext *cc,
                                  const struct timeval tv) {
    if (cc == NULL) {
        return VALKEY_ERR;
    }

    if (cc->command_timeout == NULL ||
        cc->command_timeout->tv_sec != tv.tv_sec ||
        cc->command_timeout->tv_usec != tv.tv_usec) {

        if (cc->command_timeout == NULL) {
            cc->command_timeout = vk_malloc(sizeof(struct timeval));
            if (cc->command_timeout == NULL) {
                valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
                return VALKEY_ERR;
            }
        }

        memcpy(cc->command_timeout, &tv, sizeof(struct timeval));

        /* Set timeout on already connected nodes */
        if (dictSize(cc->nodes) > 0) {
            dictEntry *de;
            valkeyClusterNode *node;

            dictIterator di;
            dictInitIterator(&di, cc->nodes);

            while ((de = dictNext(&di)) != NULL) {
                node = dictGetVal(de);
                if (node->acon) {
                    valkeyAsyncSetTimeout(node->acon, tv);
                }
                if (node->con && node->con->err == 0) {
                    valkeySetTimeout(node->con, tv);
                }

                if (node->replicas && listLength(node->replicas) > 0) {
                    valkeyClusterNode *replica;
                    listNode *ln;

                    listIter li;
                    listRewind(node->replicas, &li);

                    while ((ln = listNext(&li)) != NULL) {
                        replica = listNodeValue(ln);
                        if (replica->acon) {
                            valkeyAsyncSetTimeout(replica->acon, tv);
                        }
                        if (replica->con && replica->con->err == 0) {
                            valkeySetTimeout(replica->con, tv);
                        }
                    }
                }
            }
        }
    }

    return VALKEY_OK;
}

valkeyContext *valkeyClusterGetValkeyContext(valkeyClusterContext *cc,
                                             valkeyClusterNode *node) {
    valkeyContext *c = NULL;
    if (node == NULL) {
        return NULL;
    }

    c = node->con;
    if (c != NULL) {
        if (c->err) {
            valkeyReconnect(c);

            if (cc->on_connect) {
                cc->on_connect(c, c->err ? VALKEY_ERR : VALKEY_OK);
            }

            if (cc->tls && cc->tls_init_fn(c, cc->tls) != VALKEY_OK) {
                valkeyClusterSetError(cc, c->err, c->errstr);
            }
            /* Authenticate and select a logical database when configured.
             * cc->err and cc->errstr are set when failing. */
            authenticate(cc, c);
            select_db(cc, c);
        }

        return c;
    }

    if (node->host == NULL || node->port <= 0) {
        return NULL;
    }

    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, node->host, node->port);
    options.connect_timeout = cc->connect_timeout;
    options.command_timeout = cc->command_timeout;
    options.options = cc->options;

    c = valkeyConnectWithOptions(&options);
    if (c == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return NULL;
    }

    if (cc->on_connect) {
        cc->on_connect(c, c->err ? VALKEY_ERR : VALKEY_OK);
    }

    if (c->err) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        valkeyFree(c);
        return NULL;
    }

    if (cc->tls && cc->tls_init_fn(c, cc->tls) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        valkeyFree(c);
        return NULL;
    }

    if (authenticate(cc, c) != VALKEY_OK) {
        valkeyFree(c);
        return NULL;
    }
    if (select_db(cc, c) != VALKEY_OK) {
        valkeyFree(c);
        return NULL;
    }

    node->con = c;

    return c;
}

static valkeyClusterNode *node_get_by_table(valkeyClusterContext *cc,
                                            uint32_t slot_num) {
    if (cc == NULL) {
        return NULL;
    }

    if (slot_num >= VALKEYCLUSTER_SLOTS) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "invalid slot");
        return NULL;
    }

    if (cc->table == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "slotmap not available");
        return NULL;
    }

    if (cc->table[slot_num] == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "slot not served by any node");
        return NULL;
    }

    return cc->table[slot_num];
}

/* Helper function for the valkeyClusterAppendCommand* family of functions.
 *
 * Write a formatted command to the output buffer. When this family
 * is used, you need to call valkeyGetReply yourself to retrieve
 * the reply (or replies in pub/sub).
 */
static int valkeyClusterAppendCommandInternal(valkeyClusterContext *cc,
                                              struct cmd *command) {

    valkeyClusterNode *node;
    valkeyContext *c = NULL;

    if (cc == NULL || command == NULL) {
        return VALKEY_ERR;
    }

    node = node_get_by_table(cc, (uint32_t)command->slot_num);
    if (node == NULL) {
        return VALKEY_ERR;
    }

    c = valkeyClusterGetValkeyContext(cc, node);
    if (c == NULL) {
        return VALKEY_ERR;
    } else if (c->err) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }

    if (valkeyAppendFormattedCommand(c, command->cmd, command->clen) !=
        VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

/* Helper functions for the valkeyClusterGetReply* family of functions.
 */
static int valkeyClusterGetReplyFromNode(valkeyClusterContext *cc,
                                         valkeyClusterNode *node,
                                         void **reply) {
    valkeyContext *c;

    if (cc == NULL || node == NULL || reply == NULL)
        return VALKEY_ERR;

    c = node->con;
    if (c == NULL) {
        return VALKEY_ERR;
    } else if (c->err) {
        if (cc->need_update_route == 0) {
            cc->retry_count++;
            if (cc->retry_count > cc->max_retry_count) {
                cc->need_update_route = 1;
                cc->retry_count = 0;
            }
        }
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }

    if (valkeyGetReply(c, reply) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }

    if (getReplyErrorType(*reply) == CLUSTER_ERR_MOVED)
        cc->need_update_route = 1;

    return VALKEY_OK;
}

/* Parses a MOVED or ASK error reply and returns the destination node. The slot
 * is returned by pointer, if provided. When the parsed endpoint/IP is an empty
 * string the address from which the reply was sent from is used instead, as
 * described in the Valkey Cluster Specification. This address is provided via
 * the valkeyContext given in 'c'. */
static valkeyClusterNode *getNodeFromRedirectReply(valkeyClusterContext *cc,
                                                   valkeyContext *c,
                                                   valkeyReply *reply,
                                                   int *slotptr) {
    valkeyClusterNode *node = NULL;
    sds key = NULL;
    sds endpoint = NULL;
    char *str = reply->str;

    /* Expecting ["ASK" | "MOVED", "<slot>", "<endpoint>:<port>"] */
    char *p, *slot = NULL, *addr = NULL;
    int field = 0;
    while (*str != '\0') {
        // clang-format off
        switch (field++) {
            // Skip field 0, i.e. ASK/MOVED
            case 1: slot = str; break;
            case 2: addr = str; break;
        }
        // clang-format on
        if ((p = strchr(str, ' ')) == NULL)
            break; /* No more fields. */
        *p = '\0';
        str = p + 1; /* Start of next field. */
    }

    /* Make sure all expected fields are found. */
    if (addr == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Failed to parse redirect");
        return NULL;
    }
    /* Find the last occurrence of the port separator since
     * IPv6 addresses can contain ':' */
    if ((p = strrchr(addr, ':')) == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid address in redirect");
        return NULL;
    }
    /* Get the port (skip the found port separator). */
    int port = vk_atoi(p + 1, strlen(p + 1));
    if (port < 1 || port > UINT16_MAX) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid port in redirect");
        return NULL;
    }

    /* Check that we received an ip/host address, i.e. the field
     * does not start with the port separator. */
    if (p != addr) {
        key = sdsnew(addr);
        if (key == NULL) {
            goto oom;
        }
        *p = '\0'; /* Cut port separator and port. */

        endpoint = sdsnew(addr);
        if (endpoint == NULL) {
            goto oom;
        }
    } else {
        /* We received an ip/host that is an empty string. According to the docs
         * we can treat this as it means the same address we received the reply from. */
        endpoint = sdsnew(c->tcp.host);
        if (endpoint == NULL) {
            goto oom;
        }

        key = sdsdup(endpoint);
        if (key == NULL) {
            goto oom;
        }
        key = sdscatfmt(key, ":%i", port);
        if (key == NULL) {
            goto oom;
        }
    }

    /* Parse slot if requested. */
    if (slotptr != NULL) {
        *slotptr = vk_atoi(slot, strlen(slot));
    }

    /* Get the node if already known. */
    dictEntry *de = dictFind(cc->nodes, key);
    if (de != NULL) {
        sdsfree(key);
        sdsfree(endpoint);
        return dictGetVal(de);
    }

    /* Add this node since it was unknown */
    node = createValkeyClusterNode();
    if (node == NULL) {
        goto oom;
    }
    node->role = VALKEY_ROLE_PRIMARY;
    node->host = endpoint;
    node->port = port;
    node->addr = sdsdup(key);
    if (node->addr == NULL) {
        goto oom;
    }

    if (dictAdd(cc->nodes, key, node) != DICT_OK) {
        goto oom;
    }
    return node;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    sdsfree(key);
    sdsfree(endpoint);
    if (node != NULL) {
        sdsfree(node->addr);
        vk_free(node);
    }
    return NULL;
}

static void *valkey_cluster_command_execute(valkeyClusterContext *cc,
                                            struct cmd *command) {
    void *reply = NULL;
    valkeyClusterNode *node;
    valkeyContext *c = NULL;
    valkeyContext *c_updating_route = NULL;

retry:

    node = node_get_by_table(cc, (uint32_t)command->slot_num);
    if (node == NULL) {
        /* Update the slotmap since the slot is not served. */
        if (valkeyClusterUpdateSlotmap(cc) != VALKEY_OK) {
            goto error;
        }
        node = node_get_by_table(cc, (uint32_t)command->slot_num);
        if (node == NULL) {
            /* Return error since the slot is still not served. */
            goto error;
        }
    }

    c = valkeyClusterGetValkeyContext(cc, node);
    if (c == NULL || c->err) {
        /* Failed to connect. Maybe there was a failover and this node is gone.
         * Update slotmap to find out. */
        if (valkeyClusterUpdateSlotmap(cc) != VALKEY_OK) {
            goto error;
        }

        node = node_get_by_table(cc, (uint32_t)command->slot_num);
        if (node == NULL) {
            goto error;
        }
        c = valkeyClusterGetValkeyContext(cc, node);
        if (c == NULL) {
            goto error;
        } else if (c->err) {
            valkeyClusterSetError(cc, c->err, c->errstr);
            goto error;
        }
    }

moved_retry:
ask_retry:

    if (valkeyAppendFormattedCommand(c, command->cmd, command->clen) !=
        VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        goto error;
    }

    /* If update slotmap has been scheduled, do that in the same pipeline. */
    if (cc->need_update_route && c_updating_route == NULL) {
        if (clusterUpdateRouteSendCommand(cc, c) == VALKEY_OK) {
            c_updating_route = c;
        }
    }

    if (valkeyGetReply(c, &reply) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        /* We may need to update the slotmap if this node is removed from the
         * cluster, but the current request may have already timed out so we
         * schedule it for later. */
        if (c->err != VALKEY_ERR_OOM)
            cc->need_update_route = 1;
        goto error;
    }

    replyErrorType error_type = getReplyErrorType(reply);
    if (error_type > CLUSTER_NO_ERROR && error_type < CLUSTER_ERR_OTHER) {
        cc->retry_count++;
        if (cc->retry_count > cc->max_retry_count) {
            valkeyClusterSetError(cc, VALKEY_ERR_CLUSTER_TOO_MANY_RETRIES,
                                  "too many cluster retries");
            goto error;
        }

        int slot = -1;
        switch (error_type) {
        case CLUSTER_ERR_MOVED:
            node = getNodeFromRedirectReply(cc, c, reply, &slot);
            freeReplyObject(reply);
            reply = NULL;

            if (node == NULL) {
                /* Failed to parse redirect. Specific error already set. */
                goto error;
            }

            /* Update the slot mapping entry for this slot. */
            if (slot >= 0) {
                cc->table[slot] = node;
            }

            if (c_updating_route == NULL) {
                if (clusterUpdateRouteSendCommand(cc, c) == VALKEY_OK) {
                    /* Deferred update route using the node that sent the
                     * redirect. */
                    c_updating_route = c;
                } else if (valkeyClusterUpdateSlotmap(cc) == VALKEY_OK) {
                    /* Synchronous update route successful using new connection. */
                    valkeyClusterClearError(cc);
                } else {
                    /* Failed to update route. Specific error already set. */
                    goto error;
                }
            }

            c = valkeyClusterGetValkeyContext(cc, node);
            if (c == NULL) {
                goto error;
            } else if (c->err) {
                valkeyClusterSetError(cc, c->err, c->errstr);
                goto error;
            }

            goto moved_retry;
        case CLUSTER_ERR_ASK:
            node = getNodeFromRedirectReply(cc, c, reply, NULL);
            if (node == NULL) {
                goto error;
            }

            freeReplyObject(reply);
            reply = NULL;

            c = valkeyClusterGetValkeyContext(cc, node);
            if (c == NULL) {
                goto error;
            } else if (c->err) {
                valkeyClusterSetError(cc, c->err, c->errstr);
                goto error;
            }

            reply = valkeyCommand(c, VALKEY_COMMAND_ASKING);
            if (reply == NULL) {
                valkeyClusterSetError(cc, c->err, c->errstr);
                goto error;
            }

            freeReplyObject(reply);
            reply = NULL;

            goto ask_retry;
        case CLUSTER_ERR_TRYAGAIN:
        case CLUSTER_ERR_CLUSTERDOWN:
            freeReplyObject(reply);
            reply = NULL;
            goto retry;

        default:

            break;
        }
    }

    goto done;

error:
    if (reply) {
        freeReplyObject(reply);
        reply = NULL;
    }

done:
    if (c_updating_route) {
        /* Deferred CLUSTER SLOTS or CLUSTER NODES in progress. Wait for the
         * reply and handle it. */
        if (clusterUpdateRouteHandleReply(cc, c_updating_route) != VALKEY_OK) {
            /* Clear error and update synchronously using another node. */
            valkeyClusterClearError(cc);
            if (valkeyClusterUpdateSlotmap(cc) != VALKEY_OK) {
                /* Clear the reply to indicate failure. */
                freeReplyObject(reply);
                reply = NULL;
            }
        }
    }

    return reply;
}

/* Prepare command by parsing the string to find the key and to get the slot. */
static int prepareCommand(valkeyClusterContext *cc, struct cmd *command) {
    if (command->cmd == NULL || command->clen <= 0) {
        return VALKEY_ERR;
    }

    valkey_parse_cmd(command);
    if (command->result == CMD_PARSE_ENOMEM) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }
    if (command->result != CMD_PARSE_OK) {
        valkeyClusterSetError(cc, VALKEY_ERR_PROTOCOL, command->errstr);
        return VALKEY_ERR;
    }
    if (command->key.len == 0) {
        valkeyClusterSetError(
            cc, VALKEY_ERR_OTHER,
            "No keys in command(must have keys for valkey cluster mode)");
        return VALKEY_ERR;
    }
    command->slot_num = keyHashSlot(command->key.start, command->key.len);
    return VALKEY_OK;
}

void *valkeyClusterFormattedCommand(valkeyClusterContext *cc, char *cmd,
                                    int len) {
    valkeyReply *reply = NULL;
    struct cmd *command = NULL;

    if (cc == NULL) {
        return NULL;
    }

    valkeyClusterClearError(cc);

    command = command_get();
    if (command == NULL) {
        goto oom;
    }
    command->cmd = cmd;
    command->clen = len;

    if (prepareCommand(cc, command) != VALKEY_OK) {
        goto error;
    }

    reply = valkey_cluster_command_execute(cc, command);
    command->cmd = NULL;
    command_destroy(command);
    cc->retry_count = 0;
    return reply;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    if (command != NULL) {
        command->cmd = NULL;
        command_destroy(command);
    }
    cc->retry_count = 0;
    return NULL;
}

void *valkeyClustervCommand(valkeyClusterContext *cc, const char *format,
                            va_list ap) {
    valkeyReply *reply;
    char *cmd;
    int len;

    if (cc == NULL) {
        return NULL;
    }

    len = valkeyvFormatCommand(&cmd, format, ap);

    if (len == -1) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return NULL;
    } else if (len == -2) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid format string");
        return NULL;
    }

    reply = valkeyClusterFormattedCommand(cc, cmd, len);

    vk_free(cmd);

    return reply;
}

void *valkeyClusterCommand(valkeyClusterContext *cc, const char *format, ...) {
    va_list ap;
    valkeyReply *reply = NULL;

    va_start(ap, format);
    reply = valkeyClustervCommand(cc, format, ap);
    va_end(ap);

    return reply;
}

void *valkeyClustervCommandToNode(valkeyClusterContext *cc,
                                  valkeyClusterNode *node, const char *format,
                                  va_list ap) {
    valkeyContext *c;
    int ret;
    void *reply;
    int updating_slotmap = 0;

    c = valkeyClusterGetValkeyContext(cc, node);
    if (c == NULL) {
        return NULL;
    } else if (c->err) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return NULL;
    }

    valkeyClusterClearError(cc);

    ret = valkeyvAppendCommand(c, format, ap);

    if (ret != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return NULL;
    }

    if (cc->need_update_route) {
        /* Pipeline slotmap update on the same connection. */
        if (clusterUpdateRouteSendCommand(cc, c) == VALKEY_OK) {
            updating_slotmap = 1;
        }
    }

    if (valkeyGetReply(c, &reply) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        if (c->err != VALKEY_ERR_OOM)
            cc->need_update_route = 1;
        return NULL;
    }

    if (updating_slotmap) {
        /* Handle reply from pipelined CLUSTER SLOTS or CLUSTER NODES. */
        if (clusterUpdateRouteHandleReply(cc, c) != VALKEY_OK) {
            /* Ignore error. Update will be triggered on the next command. */
            valkeyClusterClearError(cc);
        }
    }

    return reply;
}

void *valkeyClusterCommandToNode(valkeyClusterContext *cc,
                                 valkeyClusterNode *node, const char *format,
                                 ...) {
    va_list ap;
    valkeyReply *reply = NULL;

    va_start(ap, format);
    reply = valkeyClustervCommandToNode(cc, node, format, ap);
    va_end(ap);

    return reply;
}

void *valkeyClusterCommandArgv(valkeyClusterContext *cc, int argc,
                               const char **argv, const size_t *argvlen) {
    valkeyReply *reply = NULL;
    char *cmd;
    int len;

    len = valkeyFormatCommandArgv(&cmd, argc, argv, argvlen);
    if (len == -1) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return NULL;
    }

    reply = valkeyClusterFormattedCommand(cc, cmd, len);

    vk_free(cmd);

    return reply;
}

int valkeyClusterAppendFormattedCommand(valkeyClusterContext *cc, char *cmd,
                                        int len) {
    struct cmd *command = NULL;

    command = command_get();
    if (command == NULL) {
        goto oom;
    }
    command->cmd = cmd;
    command->clen = len;

    if (prepareCommand(cc, command) != VALKEY_OK) {
        goto error;
    }

    if (valkeyClusterAppendCommandInternal(cc, command) != VALKEY_OK) {
        goto error;
    }

    command->cmd = NULL;

    if (listAddNodeTail(cc->requests, command) == NULL) {
        goto oom;
    }
    return VALKEY_OK;

oom:
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    if (command != NULL) {
        command->cmd = NULL;
        command_destroy(command);
    }
    return VALKEY_ERR;
}

int valkeyClustervAppendCommand(valkeyClusterContext *cc, const char *format,
                                va_list ap) {
    int ret;
    char *cmd;
    int len;

    len = valkeyvFormatCommand(&cmd, format, ap);
    if (len == -1) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    } else if (len == -2) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid format string");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAppendFormattedCommand(cc, cmd, len);

    vk_free(cmd);

    return ret;
}

int valkeyClusterAppendCommand(valkeyClusterContext *cc, const char *format,
                               ...) {

    int ret;
    va_list ap;

    if (cc == NULL || format == NULL) {
        return VALKEY_ERR;
    }

    va_start(ap, format);
    ret = valkeyClustervAppendCommand(cc, format, ap);
    va_end(ap);

    return ret;
}

int valkeyClustervAppendCommandToNode(valkeyClusterContext *cc,
                                      valkeyClusterNode *node,
                                      const char *format, va_list ap) {
    valkeyContext *c;
    struct cmd *command = NULL;
    char *cmd = NULL;
    int len;

    c = valkeyClusterGetValkeyContext(cc, node);
    if (c == NULL) {
        return VALKEY_ERR;
    } else if (c->err) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        return VALKEY_ERR;
    }

    len = valkeyvFormatCommand(&cmd, format, ap);

    if (len == -1) {
        goto oom;
    } else if (len == -2) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER, "Invalid format string");
        return VALKEY_ERR;
    }

    // Append the command to the outgoing valkey buffer
    if (valkeyAppendFormattedCommand(c, cmd, len) != VALKEY_OK) {
        valkeyClusterSetError(cc, c->err, c->errstr);
        vk_free(cmd);
        return VALKEY_ERR;
    }

    // Keep the command in the outstanding request list
    command = command_get();
    if (command == NULL) {
        vk_free(cmd);
        goto oom;
    }
    command->cmd = cmd;
    command->clen = len;
    command->node_addr = sdsnew(node->addr);
    if (command->node_addr == NULL)
        goto oom;

    if (listAddNodeTail(cc->requests, command) == NULL)
        goto oom;

    return VALKEY_OK;

oom:
    command_destroy(command);
    valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
    return VALKEY_ERR;
}

int valkeyClusterAppendCommandToNode(valkeyClusterContext *cc,
                                     valkeyClusterNode *node,
                                     const char *format, ...) {
    int ret;
    va_list ap;

    if (cc == NULL || node == NULL || format == NULL) {
        return VALKEY_ERR;
    }

    va_start(ap, format);
    ret = valkeyClustervAppendCommandToNode(cc, node, format, ap);
    va_end(ap);

    return ret;
}

int valkeyClusterAppendCommandArgv(valkeyClusterContext *cc, int argc,
                                   const char **argv, const size_t *argvlen) {
    int ret;
    char *cmd;
    int len;

    len = valkeyFormatCommandArgv(&cmd, argc, argv, argvlen);
    if (len == -1) {
        valkeyClusterSetError(cc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAppendFormattedCommand(cc, cmd, len);

    vk_free(cmd);

    return ret;
}

VALKEY_UNUSED
static int valkeyClusterSendAll(valkeyClusterContext *cc) {
    dictEntry *de;
    valkeyClusterNode *node;
    valkeyContext *c = NULL;
    int wdone = 0;

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    dictIterator di;
    dictInitIterator(&di, cc->nodes);

    while ((de = dictNext(&di)) != NULL) {
        node = dictGetVal(de);
        if (node == NULL) {
            continue;
        }

        c = valkeyClusterGetValkeyContext(cc, node);
        if (c == NULL) {
            continue;
        }

        /* Write until done */
        do {
            if (valkeyBufferWrite(c, &wdone) == VALKEY_ERR) {
                return VALKEY_ERR;
            }
        } while (!wdone);
    }

    return VALKEY_OK;
}

VALKEY_UNUSED
static int valkeyClusterClearAll(valkeyClusterContext *cc) {
    dictEntry *de;
    valkeyClusterNode *node;
    valkeyContext *c = NULL;

    if (cc == NULL) {
        return VALKEY_ERR;
    }

    valkeyClusterClearError(cc);

    dictIterator di;
    dictInitIterator(&di, cc->nodes);

    while ((de = dictNext(&di)) != NULL) {
        node = dictGetVal(de);
        if (node == NULL) {
            continue;
        }

        c = node->con;
        if (c == NULL) {
            continue;
        }

        valkeyFree(c);
        node->con = NULL;
    }

    return VALKEY_OK;
}

int valkeyClusterGetReply(valkeyClusterContext *cc, void **reply) {
    struct cmd *command;
    listNode *list_command;
    int slot_num;

    if (cc == NULL || reply == NULL)
        return VALKEY_ERR;

    valkeyClusterClearError(cc);
    *reply = NULL;

    list_command = listFirst(cc->requests);

    /* No queued requests. */
    if (list_command == NULL) {
        *reply = NULL;
        return VALKEY_OK;
    }

    command = list_command->value;
    if (command == NULL) {
        valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                              "command in the requests list is null");
        goto error;
    }

    /* Get reply when the command was sent via slot */
    slot_num = command->slot_num;
    if (slot_num >= 0) {
        valkeyClusterNode *node;
        if ((node = node_get_by_table(cc, (uint32_t)slot_num)) == NULL)
            goto error;

        listDelNode(cc->requests, list_command);
        return valkeyClusterGetReplyFromNode(cc, node, reply);
    }
    /* Get reply when the command was sent to a given node */
    if (command->node_addr != NULL) {
        dictEntry *de = dictFind(cc->nodes, command->node_addr);
        if (de == NULL) {
            valkeyClusterSetError(cc, VALKEY_ERR_OTHER,
                                  "command was sent to a now unknown node");
            goto error;
        }

        listDelNode(cc->requests, list_command);
        return valkeyClusterGetReplyFromNode(cc, dictGetVal(de), reply);
    }

error:
    listDelNode(cc->requests, list_command);
    return VALKEY_ERR;
}

/**
 * Resets cluster state after pipeline.
 * Resets Valkey node connections if pipeline commands were not called beforehand.
 */
void valkeyClusterReset(valkeyClusterContext *cc) {
    int status;
    void *reply;

    if (cc == NULL) {
        return;
    }

    if (cc->err) {
        valkeyClusterClearAll(cc);
    } else {
        /* Write/flush each nodes output buffer to socket */
        valkeyClusterSendAll(cc);

        /* Expect a reply for each pipelined request */
        do {
            status = valkeyClusterGetReply(cc, &reply);
            if (status == VALKEY_OK) {
                freeReplyObject(reply);
            } else {
                valkeyClusterClearAll(cc);
                break;
            }
        } while (reply != NULL);
    }

    listIter li;
    listRewind(cc->requests, &li);
    listNode *ln;
    while ((ln = listNext(&li))) {
        listDelNode(cc->requests, ln);
    }

    if (cc->need_update_route) {
        status = valkeyClusterUpdateSlotmap(cc);
        if (status != VALKEY_OK) {
            /* Specific error already set */
            return;
        }
        cc->need_update_route = 0;
    }
}

/*############valkey cluster async############*/

static void valkeyClusterAsyncSetError(valkeyClusterAsyncContext *acc, int type,
                                       const char *str) {
    valkeyClusterSetError(&acc->cc, type, str); /* Keep error flags identical. */
    acc->err = acc->cc.err;
}

static inline void valkeyClusterAsyncClearError(valkeyClusterAsyncContext *acc) {
    valkeyClusterClearError(&acc->cc);
    acc->err = acc->cc.err;
}

static cluster_async_data *cluster_async_data_create(void) {
    /* use calloc to guarantee all fields are zeroed */
    return vk_calloc(1, sizeof(cluster_async_data));
}

static void cluster_async_data_free(cluster_async_data *cad) {
    if (cad == NULL) {
        return;
    }

    command_destroy(cad->command);

    vk_free(cad);
}

static void unlinkAsyncContextAndNode(void *data) {
    valkeyClusterNode *node;

    if (data) {
        node = (valkeyClusterNode *)(data);
        node->acon = NULL;
    }
}

/* Reply callback function for SELECT */
void selectReplyCallback(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    valkeyClusterAsyncContext *acc = (valkeyClusterAsyncContext *)privdata;

    if (reply == NULL || reply->type == VALKEY_REPLY_ERROR) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER,
                                   "Failed to select logical database");
        valkeyAsyncDisconnect(ac);
    }
}

valkeyAsyncContext *
valkeyClusterGetValkeyAsyncContext(valkeyClusterAsyncContext *acc,
                                   valkeyClusterNode *node) {
    valkeyAsyncContext *ac;
    int ret;

    if (node == NULL) {
        return NULL;
    }

    ac = node->acon;
    if (ac != NULL) {
        if (ac->c.err == 0) {
            return ac;
        } else {
            /* The cluster node has a valkey context with errors. Libvalkey
             * will asynchronously destruct the context and unlink it from
             * the cluster node object. Return an error until done.
             * An example scenario is when sending a command from a command
             * callback, which has a NULL reply due to a disconnect. */
            valkeyClusterAsyncSetError(acc, ac->c.err, ac->c.errstr);
            return NULL;
        }
    }

    // No async context exists, perform a connect

    if (node->host == NULL || node->port <= 0) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER,
                                   "node host or port is error");
        return NULL;
    }

    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, node->host, node->port);
    options.connect_timeout = acc->cc.connect_timeout;
    options.command_timeout = acc->cc.command_timeout;
    options.options = acc->cc.options;

    node->lastConnectionAttempt = vk_usec_now();

    ac = valkeyAsyncConnectWithOptions(&options);
    if (ac == NULL) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OOM, "Out of memory");
        return NULL;
    }

    if (ac->err) {
        valkeyClusterAsyncSetError(acc, ac->err, ac->errstr);
        valkeyAsyncFree(ac);
        return NULL;
    }

    if (acc->cc.tls &&
        acc->cc.tls_init_fn(&ac->c, acc->cc.tls) != VALKEY_OK) {
        valkeyClusterAsyncSetError(acc, ac->c.err, ac->c.errstr);
        valkeyAsyncFree(ac);
        return NULL;
    }

    // Authenticate when needed
    if (acc->cc.password != NULL) {
        if (acc->cc.username != NULL) {
            ret = valkeyAsyncCommand(ac, NULL, NULL, "AUTH %s %s",
                                     acc->cc.username, acc->cc.password);
        } else {
            ret = valkeyAsyncCommand(ac, NULL, NULL, "AUTH %s",
                                     acc->cc.password);
        }

        if (ret != VALKEY_OK) {
            valkeyClusterAsyncSetError(acc, ac->c.err, ac->c.errstr);
            valkeyAsyncFree(ac);
            return NULL;
        }
    }
    // Select logical database when needed
    if (acc->cc.select_db > 0) {
        ret = valkeyAsyncCommand(ac, selectReplyCallback, acc, "SELECT %d", acc->cc.select_db);
        if (ret != VALKEY_OK) {
            valkeyClusterAsyncSetError(acc, ac->c.err, ac->c.errstr);
            valkeyAsyncFree(ac);
            return NULL;
        }
    }

    if (acc->attach_fn) {
        ret = acc->attach_fn(ac, acc->attach_data);
        if (ret != VALKEY_OK) {
            valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER,
                                       "Failed to attach event adapter");
            valkeyAsyncFree(ac);
            return NULL;
        }
    }

    if (acc->onConnect) {
        valkeyAsyncSetConnectCallback(ac, acc->onConnect);
    }

    if (acc->onDisconnect) {
        valkeyAsyncSetDisconnectCallback(ac, acc->onDisconnect);
    }

    ac->data = node;
    ac->dataCleanup = unlinkAsyncContextAndNode;
    node->acon = ac;

    return ac;
}

static int valkeyClusterAsyncContextInit(valkeyClusterAsyncContext *acc,
                                         const valkeyClusterOptions *options) {
    /* Setup errstr to point to common error string in valkeyClusterContext. */
    acc->errstr = acc->cc.errstr;

    if (valkeyClusterContextInit(&acc->cc, options) != VALKEY_OK) {
        valkeyClusterAsyncSetError(acc, acc->cc.err, acc->cc.errstr);
        return VALKEY_ERR;
    }

    if (options->async_connect_callback != NULL) {
        acc->onConnect = options->async_connect_callback;
    }
    if (options->async_disconnect_callback != NULL) {
        acc->onDisconnect = options->async_disconnect_callback;
    }
    if (options->attach_fn == NULL) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER,
                                   "No event library configured");
        return VALKEY_ERR;
    }
    acc->attach_fn = options->attach_fn;
    acc->attach_data = options->attach_data;
    return VALKEY_OK;
}

valkeyClusterAsyncContext *valkeyClusterAsyncConnectWithOptions(const valkeyClusterOptions *options) {
    valkeyClusterAsyncContext *acc;

    acc = vk_calloc(1, sizeof(valkeyClusterAsyncContext));
    if (acc == NULL)
        return NULL;

    if (valkeyClusterAsyncContextInit(acc, options) == VALKEY_OK) {
        /* Only connect if options are ok. */
        valkeyClusterAsyncConnect(acc);
    }

    return acc;
}

static int valkeyClusterAsyncConnect(valkeyClusterAsyncContext *acc) {
    /* Use blocking initial slotmap update when configured. */
    if (acc->cc.flags & VALKEY_FLAG_BLOCKING_INITIAL_UPDATE) {
        if (valkeyClusterUpdateSlotmap(&acc->cc) != VALKEY_OK) {
            valkeyClusterAsyncSetError(acc, acc->cc.err, acc->cc.errstr);
            return VALKEY_ERR;
        }

        /* Disconnect any non-async context used for the initial update. */
        dictIterator di;
        dictInitIterator(&di, acc->cc.nodes);
        dictEntry *de;
        while ((de = dictNext(&di)) != NULL) {
            valkeyClusterNode *node = dictGetVal(de);
            if (node->con) {
                valkeyFree(node->con);
                node->con = NULL;
            }
        }

        return VALKEY_OK;
    }
    /* Use non-blocking initial slotmap update. */
    return updateSlotMapAsync(acc, NULL /*any node*/);
}

/* Reply callback function for CLUSTER SLOTS */
void clusterSlotsReplyCallback(valkeyAsyncContext *ac, void *r,
                               void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    valkeyClusterAsyncContext *acc = (valkeyClusterAsyncContext *)privdata;
    acc->lastSlotmapUpdateAttempt = vk_usec_now();

    if (reply == NULL) {
        /* Retry using available nodes */
        updateSlotMapAsync(acc, NULL);
        return;
    }

    valkeyClusterContext *cc = &acc->cc;
    dict *nodes = parse_cluster_slots(cc, &ac->c, reply);
    if (updateNodesAndSlotmap(cc, nodes) != VALKEY_OK) {
        /* Retry using available nodes */
        updateSlotMapAsync(acc, NULL);
    }
}

/* Reply callback function for CLUSTER NODES */
void clusterNodesReplyCallback(valkeyAsyncContext *ac, void *r,
                               void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    valkeyClusterAsyncContext *acc = (valkeyClusterAsyncContext *)privdata;
    acc->lastSlotmapUpdateAttempt = vk_usec_now();

    if (reply == NULL) {
        /* Retry using available nodes */
        updateSlotMapAsync(acc, NULL);
        return;
    }

    valkeyClusterContext *cc = &acc->cc;
    dict *nodes = parse_cluster_nodes(cc, &ac->c, reply);
    if (updateNodesAndSlotmap(cc, nodes) != VALKEY_OK) {
        /* Retry using available nodes */
        updateSlotMapAsync(acc, NULL);
    }
}

#define nodeIsConnected(n)                       \
    ((n)->acon != NULL && (n)->acon->err == 0 && \
     (n)->acon->c.flags & VALKEY_CONNECTED)

/* Select a node.
 * Primarily selects a connected node found close to a randomly picked index of
 * all known nodes. The random index should give a more even distribution of
 * selected nodes. If no connected node is found while iterating to this index
 * the remaining nodes are also checked until a connected node is found.
 * If no connected node is found a node for which a connect has not been attempted
 * within throttle-time, and is found near the picked index, is selected.
 */
static valkeyClusterNode *selectNode(dict *nodes) {
    valkeyClusterNode *node, *selected = NULL;
    dictIterator di;
    dictInitIterator(&di, nodes);

    int64_t throttleLimit = vk_usec_now() - SLOTMAP_UPDATE_THROTTLE_USEC;
    unsigned long currentIndex = 0;
    unsigned long checkIndex = random() % dictSize(nodes);

    dictEntry *de;
    while ((de = dictNext(&di)) != NULL) {
        node = dictGetVal(de);

        if (nodeIsConnected(node)) {
            /* Keep any connected node */
            selected = node;
        } else if (node->lastConnectionAttempt < throttleLimit &&
                   (selected == NULL || (currentIndex < checkIndex &&
                                         !nodeIsConnected(selected)))) {
            /* Keep an accepted node when none is yet found, or
               any accepted node until the chosen index is reached */
            selected = node;
        }

        /* Return a found connected node when chosen index is reached. */
        if (currentIndex >= checkIndex && selected != NULL &&
            nodeIsConnected(selected))
            break;
        currentIndex += 1;
    }
    return selected;
}

/* Update the slot map by querying a selected cluster node. If ac is NULL, an
 * arbitrary connected node is selected. */
static int updateSlotMapAsync(valkeyClusterAsyncContext *acc,
                              valkeyAsyncContext *ac) {
    if (acc->lastSlotmapUpdateAttempt == SLOTMAP_UPDATE_ONGOING) {
        /* Don't allow concurrent slot map updates. */
        return VALKEY_ERR;
    }
    if (acc->cc.flags & VALKEY_FLAG_DISCONNECTING) {
        /* No slot map updates during a cluster client disconnect. */
        return VALKEY_ERR;
    }

    if (ac == NULL) {
        valkeyClusterNode *node = selectNode(acc->cc.nodes);
        if (node == NULL) {
            goto error;
        }

        /* Get libvalkey context, connect if needed */
        ac = valkeyClusterGetValkeyAsyncContext(acc, node);
    }
    if (ac == NULL)
        goto error; /* Specific error already set */

    /* Send a command depending of config */
    int status;
    if (acc->cc.flags & VALKEY_FLAG_USE_CLUSTER_NODES) {
        status = valkeyAsyncCommand(ac, clusterNodesReplyCallback, acc,
                                    VALKEY_COMMAND_CLUSTER_NODES);
    } else {
        status = valkeyAsyncCommand(ac, clusterSlotsReplyCallback, acc,
                                    VALKEY_COMMAND_CLUSTER_SLOTS);
    }

    if (status == VALKEY_OK) {
        acc->lastSlotmapUpdateAttempt = SLOTMAP_UPDATE_ONGOING;
        return VALKEY_OK;
    }

error:
    acc->lastSlotmapUpdateAttempt = vk_usec_now();
    return VALKEY_ERR;
}

/* Start a slotmap update if the throttling allows. */
static void throttledUpdateSlotMapAsync(valkeyClusterAsyncContext *acc,
                                        valkeyAsyncContext *ac) {
    if (acc->lastSlotmapUpdateAttempt != SLOTMAP_UPDATE_ONGOING &&
        (acc->lastSlotmapUpdateAttempt + SLOTMAP_UPDATE_THROTTLE_USEC) <
            vk_usec_now()) {
        updateSlotMapAsync(acc, ac);
    }
}

static void valkeyClusterAsyncCallback(valkeyAsyncContext *ac, void *r,
                                       void *privdata) {
    int ret;
    valkeyReply *reply = r;
    cluster_async_data *cad = privdata;
    valkeyClusterAsyncContext *acc;
    valkeyClusterContext *cc;
    valkeyAsyncContext *ac_retry = NULL;
    valkeyClusterNode *node;
    struct cmd *command;

    if (cad == NULL) {
        goto error;
    }

    acc = cad->acc;
    if (acc == NULL) {
        goto error;
    }

    cc = &acc->cc;
    if (cc == NULL) {
        goto error;
    }

    command = cad->command;
    if (command == NULL) {
        goto error;
    }

    if (reply == NULL) {
        /* Copy reply specific error from libvalkey */
        valkeyClusterAsyncSetError(acc, ac->err, ac->errstr);

        node = (valkeyClusterNode *)ac->data;
        if (node == NULL)
            goto done; /* Node already removed from topology */

        /* Start a slotmap update when the throttling allows */
        throttledUpdateSlotMapAsync(acc, NULL);
        goto done;
    }

    /* Skip retry handling when not expected, or during a client disconnect. */
    if (cad->retry_count == NO_RETRY || cc->flags & VALKEY_FLAG_DISCONNECTING)
        goto done;

    replyErrorType error_type = getReplyErrorType(reply);
    if (error_type > CLUSTER_NO_ERROR && error_type < CLUSTER_ERR_OTHER) {
        cad->retry_count++;
        if (cad->retry_count > cc->max_retry_count) {
            cad->retry_count = 0;
            valkeyClusterAsyncSetError(acc, VALKEY_ERR_CLUSTER_TOO_MANY_RETRIES,
                                       "too many cluster retries");
            goto done;
        }

        int slot = -1;
        switch (error_type) {
        case CLUSTER_ERR_MOVED:
            /* Initiate slot mapping update using the node that sent MOVED. */
            throttledUpdateSlotMapAsync(acc, ac);

            node = getNodeFromRedirectReply(cc, &ac->c, reply, &slot);
            if (node == NULL) {
                valkeyClusterAsyncSetError(acc, cc->err, cc->errstr);
                goto done;
            }
            /* Update the slot mapping entry for this slot. */
            if (slot >= 0) {
                cc->table[slot] = node;
            }
            ac_retry = valkeyClusterGetValkeyAsyncContext(acc, node);

            break;
        case CLUSTER_ERR_ASK:
            node = getNodeFromRedirectReply(cc, &ac->c, reply, NULL);
            if (node == NULL) {
                valkeyClusterAsyncSetError(acc, cc->err, cc->errstr);
                goto done;
            }

            ac_retry = valkeyClusterGetValkeyAsyncContext(acc, node);
            if (ac_retry == NULL) {
                /* Specific error already set */
                goto done;
            }

            ret =
                valkeyAsyncCommand(ac_retry, NULL, NULL, VALKEY_COMMAND_ASKING);
            if (ret != VALKEY_OK) {
                goto error;
            }

            break;
        case CLUSTER_ERR_TRYAGAIN:
        case CLUSTER_ERR_CLUSTERDOWN:
            ac_retry = ac;

            break;
        default:

            goto done;
        }

        goto retry;
    }

done:

    if (acc->err) {
        cad->callback(acc, NULL, cad->privdata);
    } else {
        cad->callback(acc, r, cad->privdata);
    }

    valkeyClusterAsyncClearError(acc);

    cluster_async_data_free(cad);

    return;

retry:

    ret = valkeyAsyncFormattedCommand(ac_retry, valkeyClusterAsyncCallback, cad,
                                      command->cmd, command->clen);
    if (ret != VALKEY_OK) {
        goto error;
    }

    return;

error:

    cluster_async_data_free(cad);
}

int valkeyClusterAsyncFormattedCommand(valkeyClusterAsyncContext *acc,
                                       valkeyClusterCallbackFn *fn,
                                       void *privdata, char *cmd, int len) {

    valkeyClusterContext *cc;
    int status = VALKEY_OK;
    valkeyClusterNode *node;
    valkeyAsyncContext *ac;
    struct cmd *command = NULL;
    cluster_async_data *cad = NULL;

    if (acc == NULL) {
        return VALKEY_ERR;
    }

    cc = &acc->cc;

    /* Don't accept new commands when the client is about to disconnect. */
    if (cc->flags & VALKEY_FLAG_DISCONNECTING) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER, "disconnecting");
        return VALKEY_ERR;
    }

    valkeyClusterAsyncClearError(acc);

    command = command_get();
    if (command == NULL) {
        goto oom;
    }

    command->cmd = vk_calloc(len, sizeof(*command->cmd));
    if (command->cmd == NULL) {
        goto oom;
    }
    memcpy(command->cmd, cmd, len);
    command->clen = len;

    if (prepareCommand(cc, command) != VALKEY_OK) {
        valkeyClusterAsyncSetError(acc, cc->err, cc->errstr);
        goto error;
    }

    node = node_get_by_table(cc, (uint32_t)command->slot_num);
    if (node == NULL) {
        /* Initiate a slotmap update since the slot is not served. */
        throttledUpdateSlotMapAsync(acc, NULL);

        /* node_get_by_table() has set the error on cc. */
        valkeyClusterAsyncSetError(acc, cc->err, cc->errstr);
        goto error;
    }

    ac = valkeyClusterGetValkeyAsyncContext(acc, node);
    if (ac == NULL) {
        /* Specific error already set */
        goto error;
    }

    cad = cluster_async_data_create();
    if (cad == NULL) {
        goto oom;
    }

    cad->acc = acc;
    cad->command = command;
    command = NULL; /* Memory ownership moved. */
    cad->callback = fn;
    cad->privdata = privdata;

    status = valkeyAsyncFormattedCommand(ac, valkeyClusterAsyncCallback, cad,
                                         cmd, len);
    if (status != VALKEY_OK) {
        valkeyClusterAsyncSetError(acc, ac->err, ac->errstr);
        goto error;
    }
    return VALKEY_OK;

oom:
    valkeyClusterAsyncSetError(acc, VALKEY_ERR_OOM, "Out of memory");
    // passthrough

error:
    cluster_async_data_free(cad);
    command_destroy(command);
    return VALKEY_ERR;
}

int valkeyClusterAsyncFormattedCommandToNode(valkeyClusterAsyncContext *acc,
                                             valkeyClusterNode *node,
                                             valkeyClusterCallbackFn *fn,
                                             void *privdata, char *cmd,
                                             int len) {
    valkeyClusterContext *cc = &acc->cc;
    valkeyAsyncContext *ac;
    int status;
    cluster_async_data *cad = NULL;
    struct cmd *command = NULL;

    /* Don't accept new commands when the client is about to disconnect. */
    if (cc->flags & VALKEY_FLAG_DISCONNECTING) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER, "disconnecting");
        return VALKEY_ERR;
    }

    ac = valkeyClusterGetValkeyAsyncContext(acc, node);
    if (ac == NULL) {
        /* Specific error already set */
        return VALKEY_ERR;
    }

    valkeyClusterAsyncClearError(acc);

    command = command_get();
    if (command == NULL) {
        goto oom;
    }

    command->cmd = vk_calloc(len, sizeof(*command->cmd));
    if (command->cmd == NULL) {
        goto oom;
    }
    memcpy(command->cmd, cmd, len);
    command->clen = len;

    cad = cluster_async_data_create();
    if (cad == NULL)
        goto oom;

    cad->acc = acc;
    cad->command = command;
    command = NULL; /* Memory ownership moved. */
    cad->callback = fn;
    cad->privdata = privdata;
    cad->retry_count = NO_RETRY;

    status = valkeyAsyncFormattedCommand(ac, valkeyClusterAsyncCallback, cad,
                                         cmd, len);
    if (status != VALKEY_OK) {
        valkeyClusterAsyncSetError(acc, ac->err, ac->errstr);
        goto error;
    }

    return VALKEY_OK;

oom:
    valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER, "Out of memory");
    // passthrough

error:
    cluster_async_data_free(cad);
    command_destroy(command);
    return VALKEY_ERR;
}

int valkeyClustervAsyncCommand(valkeyClusterAsyncContext *acc,
                               valkeyClusterCallbackFn *fn, void *privdata,
                               const char *format, va_list ap) {
    int ret;
    char *cmd;
    int len;

    if (acc == NULL) {
        return VALKEY_ERR;
    }

    len = valkeyvFormatCommand(&cmd, format, ap);
    if (len == -1) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    } else if (len == -2) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER,
                                   "Invalid format string");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAsyncFormattedCommand(acc, fn, privdata, cmd, len);

    vk_free(cmd);

    return ret;
}

int valkeyClusterAsyncCommand(valkeyClusterAsyncContext *acc,
                              valkeyClusterCallbackFn *fn, void *privdata,
                              const char *format, ...) {
    int ret;
    va_list ap;

    va_start(ap, format);
    ret = valkeyClustervAsyncCommand(acc, fn, privdata, format, ap);
    va_end(ap);

    return ret;
}

int valkeyClusterAsyncCommandToNode(valkeyClusterAsyncContext *acc,
                                    valkeyClusterNode *node,
                                    valkeyClusterCallbackFn *fn, void *privdata,
                                    const char *format, ...) {
    int ret;
    va_list ap;
    int len;
    char *cmd = NULL;

    /* Allocate cmd and encode the variadic command */
    va_start(ap, format);
    len = valkeyvFormatCommand(&cmd, format, ap);
    va_end(ap);

    if (len == -1) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER, "Out of memory");
        return VALKEY_ERR;
    } else if (len == -2) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OTHER,
                                   "Invalid format string");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAsyncFormattedCommandToNode(acc, node, fn, privdata, cmd,
                                                   len);
    vk_free(cmd);
    return ret;
}

int valkeyClusterAsyncCommandArgv(valkeyClusterAsyncContext *acc,
                                  valkeyClusterCallbackFn *fn, void *privdata,
                                  int argc, const char **argv,
                                  const size_t *argvlen) {
    int ret;
    char *cmd;
    int len;

    len = valkeyFormatCommandArgv(&cmd, argc, argv, argvlen);
    if (len == -1) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAsyncFormattedCommand(acc, fn, privdata, cmd, len);

    vk_free(cmd);

    return ret;
}

int valkeyClusterAsyncCommandArgvToNode(valkeyClusterAsyncContext *acc,
                                        valkeyClusterNode *node,
                                        valkeyClusterCallbackFn *fn,
                                        void *privdata, int argc,
                                        const char **argv,
                                        const size_t *argvlen) {

    int ret;
    char *cmd;
    int len;

    len = valkeyFormatCommandArgv(&cmd, argc, argv, argvlen);
    if (len == -1) {
        valkeyClusterAsyncSetError(acc, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    ret = valkeyClusterAsyncFormattedCommandToNode(acc, node, fn, privdata, cmd,
                                                   len);

    vk_free(cmd);

    return ret;
}

void valkeyClusterAsyncDisconnect(valkeyClusterAsyncContext *acc) {
    valkeyClusterContext *cc;
    valkeyAsyncContext *ac;
    dictEntry *de;
    valkeyClusterNode *node;

    if (acc == NULL) {
        return;
    }

    cc = &acc->cc;
    cc->flags |= VALKEY_FLAG_DISCONNECTING;

    dictIterator di;
    dictInitIterator(&di, cc->nodes);

    while ((de = dictNext(&di)) != NULL) {
        node = dictGetVal(de);

        ac = node->acon;

        if (ac == NULL) {
            continue;
        }

        valkeyAsyncDisconnect(ac);
    }
}

void valkeyClusterAsyncFree(valkeyClusterAsyncContext *acc) {
    if (acc == NULL)
        return;

    valkeyClusterContext *cc = &acc->cc;
    cc->flags |= VALKEY_FLAG_DISCONNECTING;
    valkeyClusterFree(cc);
}

struct nodeIterator {
    uint64_t route_version;
    valkeyClusterContext *cc;
    int retries_left;
    dictIterator di;
};
/* Make sure the opaque memory blob can contain a nodeIterator. */
vk_static_assert(sizeof(valkeyClusterNodeIterator) >= sizeof(struct nodeIterator));

/* Initiate an iterator for iterating over current cluster nodes */
void valkeyClusterInitNodeIterator(valkeyClusterNodeIterator *iter,
                                   valkeyClusterContext *cc) {
    struct nodeIterator *ni = (struct nodeIterator *)iter;
    ni->cc = cc;
    ni->route_version = cc->route_version;
    dictInitIterator(&ni->di, cc->nodes);
    ni->retries_left = 1;
}

/* Get next node from the iterator
 * The iterator will restart if the routing table is updated
 * before all nodes have been iterated. */
valkeyClusterNode *valkeyClusterNodeNext(valkeyClusterNodeIterator *iter) {
    struct nodeIterator *ni = (struct nodeIterator *)iter;
    if (ni->retries_left <= 0)
        return NULL;

    if (ni->route_version != ni->cc->route_version) {
        // The routing table has changed and current iterator
        // is invalid. The nodes dict has been recreated in
        // the cluster context. We need to re-init the dictIter.
        dictInitIterator(&ni->di, ni->cc->nodes);
        ni->route_version = ni->cc->route_version;
        ni->retries_left--;
    }

    dictEntry *de;
    if ((de = dictNext(&ni->di)) != NULL)
        return dictGetVal(de);
    else
        return NULL;
}

/* Get hash slot for given key string, which can include hash tags */
unsigned int valkeyClusterGetSlotByKey(char *key) {
    return keyHashSlot(key, strlen(key));
}

/* Get node that handles given key string, which can include hash tags */
valkeyClusterNode *valkeyClusterGetNodeByKey(valkeyClusterContext *cc,
                                             char *key) {
    return node_get_by_table(cc, keyHashSlot(key, strlen(key)));
}
