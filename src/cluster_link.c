/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * cluster_link.c — cluster transport link management.
 *
 * This file contains protocol-agnostic cluster link (TCP connection)
 * management: creation, teardown, send queue writing, and the
 * CLUSTER LINKS command.
 */

#include "server.h"
#include "cluster.h"
#include "cluster_state.h"
#include "cluster_link.h"

/* Decrement the reference count of a send block and free it when it reaches
 * zero. Takes void * because it is used as a list free method callback. */
void clusterMsgSendBlockDecrRefCount(void *ptr) {
    clusterMsgSendBlock *block = (clusterMsgSendBlock *)ptr;
    block->refcount--;
    serverAssert(block->refcount >= 0);
    if (block->refcount == 0) {
        server.stat_cluster_links_memory -= block->totlen;
        zfree(block);
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
    link->inbound = (node == NULL);
    if (!link->inbound) {
        node->link = link;
    }
    link->flags = 0;
    return link;
}

void freeClusterLink(clusterLink *link) {
    serverAssert(link != NULL);
    serverLog(LL_DEBUG, "Freeing cluster link for node: %.40s:%s (%s)",
              clusterLinkGetNodeName(link),
              link->inbound ? "inbound" : "outbound",
              clusterLinkGetHumanNodeName(link));

    if (link->conn) {
        connClose(link->conn);
        link->conn = NULL;
    }
    server.stat_cluster_links_memory -=
        sizeof(list) + listLength(link->send_msg_queue) * sizeof(listNode);
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
            link->node->inbound_link_freed_time = mstime();
        }
    }
    zfree(link);
}

void setClusterNodeToInboundClusterLink(clusterNode *node, clusterLink *link) {
    serverAssert(!link->node);
    serverAssert(link->inbound);
    if (node->inbound_link) {
        serverLog(LL_DEBUG,
                  "Replacing inbound link fd %d from node %.40s with fd %d",
                  node->inbound_link->conn->fd, node->name, link->conn->fd);
        freeClusterLink(node->inbound_link);
    }
    serverAssert(!node->inbound_link);
    node->inbound_link = link;
    link->node = node;
    if (server.verbosity <= LL_VERBOSE) {
        char ip[NET_IP_STR_LEN];
        int port;
        if (connAddrPeerName(link->conn, ip, sizeof(ip), &port) != -1) {
            serverLog(LL_VERBOSE,
                      "Bound cluster node %.40s (%s) to connection "
                      "of client %s:%d",
                      node->name, humanNodename(node), ip, port);
        } else {
            serverLog(LL_VERBOSE,
                      "Error resolving the inbound connection address "
                      "of node %.40s (%s)",
                      node->name, humanNodename(node));
        }
    }
}

/* This function is called when we detect the link with this node is lost.
   We set the node as no longer connected. The Cluster Cron will detect
   this connection and will try to get it connected again.

   Instead if the node is a temporary node used to accept a query, we
   completely free the node on error. */
static void handleLinkIOError(clusterLink *link) {
    freeClusterLink(link);
}

/* Send the messages queued for the link. */
void clusterWriteHandler(connection *conn) {
    clusterLink *link = connGetPrivateData(conn);
    ssize_t nwritten;
    size_t totwritten = 0;

    while (totwritten < NET_MAX_WRITES_PER_EVENT &&
           listLength(link->send_msg_queue) > 0) {
        listNode *head = listFirst(link->send_msg_queue);
        clusterMsgSendBlock *block = (clusterMsgSendBlock *)head->value;
        size_t offset = link->head_msg_send_offset;

        nwritten = connWrite(conn, (char *)block->data + offset,
                             block->len - offset);
        if (nwritten <= 0) {
            if (nwritten == -1 && connGetState(conn) == CONN_STATE_CONNECTED) return; /* equivalent to EAGAIN */
            serverLog(LL_DEBUG, "I/O error writing to node link: %s",
                      (nwritten == -1) ? connGetLastError(conn)
                                       : "short write");
            handleLinkIOError(link);
            return;
        }
        if (offset + nwritten < block->len) {
            link->head_msg_send_offset += nwritten;
            return;
        }
        serverAssert((offset + nwritten) == block->len);
        link->head_msg_send_offset = 0;

        uint32_t blocklen = block->totlen;
        listDelNode(link->send_msg_queue, head);
        server.stat_cluster_links_memory -= sizeof(listNode);
        link->send_msg_queue_mem -= sizeof(listNode) + blocklen;

        totwritten += nwritten;
    }

    if (listLength(link->send_msg_queue) == 0)
        connSetWriteHandler(link->conn, NULL);
}

/* Return node name if the link has the node associated to it
 * or else return "<unknown>". */
char *clusterLinkGetNodeName(clusterLink *link) {
    return link->node ? link->node->name : "<unknown>";
}

/* Return human assigned node name if the link has the node associated to it
 * or else return "<unknown>". */
char *clusterLinkGetHumanNodeName(clusterLink *link) {
    return link->node ? humanNodename(link->node) : "<unknown>";
}

/* --- CLUSTER LINKS command --- */

/* Add to the output buffer of the given client the description of the given cluster link.
 * The description is a map with each entry being an attribute of the link. */
static void addReplyClusterLinkDescription(client *c, clusterLink *link) {
    addReplyMapLen(c, 6);

    addReplyBulkCString(c, "direction");
    addReplyBulkCString(c, link->inbound ? "from" : "to");

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
static void addReplyClusterLinksDescription(client *c) {
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

void clusterCommandLinks(client *c) {
    addReplyClusterLinksDescription(c);
}

int clusterLinkDebugCommand(client *c) {
    if (c->argc != 5 ||
        strcasecmp(objectGetVal(c->argv[1]), "CLUSTERLINK") ||
        strcasecmp(objectGetVal(c->argv[2]), "KILL")) {
        return 0;
    }

    if (!server.cluster_enabled) {
        addReplyError(c,
                      "Debug option only available for cluster mode "
                      "enabled setup!");
        return 1;
    }

    clusterNode *n = clusterLookupNode(objectGetVal(c->argv[4]),
                                       sdslen(objectGetVal(c->argv[4])));
    if (!n) {
        addReplyErrorFormat(c, "Unknown node %s",
                            (char *)objectGetVal(c->argv[4]));
        return 1;
    }
    if (n == server.cluster->myself) {
        addReplyErrorFormat(c, "Cannot free cluster link(s) to myself");
        return 1;
    }

    if (!strcasecmp(objectGetVal(c->argv[3]), "from")) {
        if (n->inbound_link) freeClusterLink(n->inbound_link);
    } else if (!strcasecmp(objectGetVal(c->argv[3]), "to")) {
        if (n->link) freeClusterLink(n->link);
    } else if (!strcasecmp(objectGetVal(c->argv[3]), "all")) {
        if (n->link) freeClusterLink(n->link);
        if (n->inbound_link) freeClusterLink(n->inbound_link);
    } else {
        addReplyErrorFormat(c, "Unknown direction %s",
                            (char *)objectGetVal(c->argv[3]));
    }
    addReply(c, shared.ok);
    return 1;
}

/* Queue a send block on a link for sending. The caller must set
 * msgblock->len to the number of bytes to send before calling this. */
void clusterLinkSendBlock(clusterLink *link, clusterMsgSendBlock *msgblock) {
    if (!link) return;
    if (listLength(link->send_msg_queue) == 0 && msgblock->len != 0)
        connSetWriteHandlerWithBarrier(link->conn, clusterWriteHandler, 1);

    listAddNodeTail(link->send_msg_queue, msgblock);
    msgblock->refcount++;

    /* Update memory tracking */
    link->send_msg_queue_mem += sizeof(listNode) + msgblock->totlen;
    server.stat_cluster_links_memory += sizeof(listNode);
}

/* Free a cluster link if its send buffer exceeds the configured limit.
 * Returns 1 if the link was freed, 0 otherwise. */
int freeClusterLinkOnBufferLimitReached(clusterLink *link) {
    if (link == NULL || server.cluster_link_msg_queue_limit_bytes == 0) {
        return 0;
    }
    if (link->send_msg_queue_mem > server.cluster_link_msg_queue_limit_bytes) {
        serverLog(LL_WARNING,
                  "Freeing cluster link(%s node %.40s (%s), used memory: %llu) due to "
                  "exceeding send buffer memory limit.",
                  link->inbound ? "from" : "to", clusterLinkGetNodeName(link),
                  clusterLinkGetHumanNodeName(link),
                  (unsigned long long)link->send_msg_queue_mem);
        freeClusterLink(link);
        server.cluster->stat_cluster_links_buffer_limit_exceeded++;
        return 1;
    }
    return 0;
}

const char **clusterLinkDebugHelp(void) {
    static const char *help[] = {
        "CLUSTERLINK KILL <to|from|all> <node-id>",
        "    Kills the link based on the direction to/from (both) "
        "with the provided node.",
        NULL};
    return help;
}
