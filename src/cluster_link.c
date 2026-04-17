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
#include "cluster_bus.h"
#include "cluster_state.h"
#include "cluster_link.h"
#include "connhelpers.h"
#include "io_threads.h"

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

    /* Threaded I/O state */
    link->io_read_state = CLUSTER_LINK_IO_IDLE;
    link->io_write_state = CLUSTER_LINK_IO_IDLE;
    link->async_close = 0;
    link->io_refs = 0;
    link->io_result = CLUSTER_IO_OK;

    /* Async write snapshot/result */
    link->io_last_send_block = NULL;
    link->io_head_offset = 0;
    link->io_nodes_sent = 0;

    link->rcvbuf_alloc_at_dispatch = 0;
    link->io_complete_bytes = 0;
    link->io_complete_packets = 0;
    link->io_read_deferred = 0;

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

/* Free a cluster link, but does not free the associated node of course.
 * This function will just make sure that the original node associated
 * with this link will have the 'link' field set to NULL.
 *
 * If I/O jobs are in flight (io_refs > 0), the link is not freed immediately.
 * Instead, async_close is set, the link is detached from node fields, and any
 * read/write handlers are removed so no new I/O work is scheduled. The actual
 * connClose() happens later on the main thread when the last completion
 * decrements io_refs to 0, mirroring the client close flow.
 *
 * Returns 1 if the link was freed immediately, 0 if teardown was deferred. */
int freeClusterLink(clusterLink *link) {
    serverAssert(link != NULL);
    serverLog(LL_DEBUG, "Freeing cluster link for node: %.40s:%s (%s)",
              clusterLinkGetNodeName(link),
              link->inbound ? "inbound" : "outbound",
              clusterLinkGetHumanNodeName(link));

    /* Detach from node regardless of whether we free now or defer. */
    if (link->node) {
        if (link->node->link == link) {
            serverAssert(!link->inbound);
            link->node->link = NULL;
        } else if (link->node->inbound_link == link) {
            serverAssert(link->inbound);
            link->node->inbound_link = NULL;
            link->node->inbound_link_freed_time = mstime();
        }
        link->node = NULL;
    }

    /* If I/O jobs are in flight, defer the actual free. */
    if (link->io_refs > 0) {
        serverAssert(link->io_read_state == CLUSTER_LINK_IO_PENDING ||
                     link->io_write_state == CLUSTER_LINK_IO_PENDING);
        if (!link->async_close) {
            if (link->conn) {
                connSetReadHandler(link->conn, NULL);
                connSetWriteHandler(link->conn, NULL);
            }
            link->async_close = 1;
        }
        return 0;
    }

    /* Close the connection now that no I/O jobs are in flight. */
    if (link->conn) {
        connClose(link->conn);
        link->conn = NULL;
    }

    /* Immediate free path — both states must be idle. */
    serverAssert(link->io_read_state == CLUSTER_LINK_IO_IDLE);
    serverAssert(link->io_write_state == CLUSTER_LINK_IO_IDLE);
    serverAssert(link->io_refs == 0);
    server.stat_cluster_links_memory -=
        sizeof(list) + listLength(link->send_msg_queue) * sizeof(listNode);
    listRelease(link->send_msg_queue);

    /* Discard any complete packets the worker framed but we never applied. */
    link->io_complete_bytes = 0;
    link->io_complete_packets = 0;

    server.stat_cluster_links_memory -= link->rcvbuf_alloc;
    zfree(link->rcvbuf);
    zfree(link);
    return 1;
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

    if (listLength(link->send_msg_queue) == 0) {
        connSetWriteHandler(link->conn, NULL);
        return;
    }

    if (trySendClusterWriteToIOThreads(link) == C_OK) return;

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

    /* Unregister the write handler when the queue is empty to avoid
     * burning CPU on spurious writable events. */
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

/* Allocate a zeroed send block that can hold msglen bytes of payload. */
clusterMsgSendBlock *clusterAllocMsgSendBlock(uint32_t msglen) {
    uint32_t blocklen = sizeof(clusterMsgSendBlock) + msglen;
    clusterMsgSendBlock *msgblock = zcalloc(blocklen);
    msgblock->refcount = 1;
    msgblock->totlen = blocklen;
    msgblock->len = msglen;
    server.stat_cluster_links_memory += blocklen;
    return msgblock;
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

/* ========================== Wrapper Functions for Testing ========================== */
void testOnlyFreeClusterLinkOnBufferLimitReached(clusterLink *link) {
    freeClusterLinkOnBufferLimitReached(link);
}

/* -----------------------------------------------------------------------------
 * Cluster link connection and message handling
 * -------------------------------------------------------------------------- */

/* Called when an outbound connection to a cluster node is established. */
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

    /* Count a successfully connected (outbound) cluster link. This reflects
     * how often we (re)connect to peers. */
    server.cluster->stat_cluster_links_established_outbound++;

    /* Protocol-specific post-connect action (e.g. send initial PING). */
    if (clusterCurrentBus->postConnect) clusterCurrentBus->postConnect(link);

    serverLog(LL_DEBUG, "Connecting with Node %.40s at %s:%d", node->name, node->ip, node->cport);
}

/* Called when a new inbound connection is accepted on the cluster bus. */
void clusterConnAcceptHandler(connection *conn) {
    clusterLink *link;

    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_VERBOSE, "Error accepting cluster node connection: %s", connGetLastError(conn));
        connClose(conn);
        return;
    }

    serverAssert(connGetOwnerKind(conn) == CONN_OWNER_CLUSTER_LINK);
    serverAssert(connGetPrivateData(conn) == NULL);

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

    /* Count a successfully accepted (inbound) cluster link. This reflects
     * how often peers (re)establish connections to us. */
    server.cluster->stat_cluster_links_established_inbound++;
}

/* Establish outbound links to nodes that don't have one.
 * Throttles reconnection attempts and cleans up timed-out handshakes. */
void clusterConnectNodes(void) {
    mstime_t now = mstime();
    mstime_t handshake_timeout = max(server.cluster_node_timeout, 1000);
    mstime_t reconnect_interval = server.cluster_node_timeout / 2;
    if (reconnect_interval <= 0) reconnect_interval = 1;

    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node == myself) continue;
        if (node->link) continue;

        /* Remove handshake nodes that have timed out. */
        if (nodeInHandshake(node) && now - node->ctime > handshake_timeout) {
            clusterDelNode(node);
            continue;
        }

        /* Throttle reconnection attempts. */
        if (now - node->outbound_link_attempt_time < reconnect_interval) continue;
        node->outbound_link_attempt_time = now;

        clusterLink *link = createClusterLink(node);
        link->conn = connCreate(connTypeOfCluster());
        /* Tag outbound cluster bus link as high-priority so node reconnects, gossip ping/pong,
         * and failure detection heartbeats operate with QoS priority. */
        connSetPriority(link->conn, true);
        connSetPrivateData(link->conn, link);
        connSetOwnerKind(link->conn, CONN_OWNER_CLUSTER_LINK);
        if (connConnect(link->conn, node->ip, node->cport, server.bind_source_addr, 0,
                        clusterLinkConnectHandler) == C_ERR) {
            serverLog(LL_DEBUG, "Unable to connect to Cluster Node [%s]:%d -> %s", node->ip, node->cport,
                      server.neterr);
            freeClusterLink(link);
        }
    }
    dictReleaseIterator(di);
}

void clusterListenerInit(void) {
    if (!connectionByType(connTypeOfCluster()->get_type())) {
        serverLog(LL_WARNING, "Missing connection type %s, but it is required for the Cluster bus.",
                  getConnectionTypeName(connTypeOfCluster()->get_type()));
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

void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = server.tls_cluster ? server.max_new_tls_conns_per_cycle : server.max_new_conns_per_cycle;
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
            if (anetRetryAcceptOnError(errno)) continue;
            if (errno != EWOULDBLOCK) serverLog(LL_VERBOSE, "Error accepting cluster node: %s", server.neterr);
            return;
        }

        connection *conn = connCreateAccepted(connTypeOfCluster(), cfd, &require_auth);
        /* Tag inbound cluster bus link as high-priority so cluster gossip and heartbeats
         * are processed via QoS ahead of normal client traffic. */
        connSetPriority(conn, true);
        /* Mark as cluster-owned before any TLS accept retries so generic
         * accept offload routing can safely avoid client assumptions. */
        connSetOwnerKind(conn, CONN_OWNER_CLUSTER_LINK);
        /* Only a TLS accept is worth offloading: it runs the handshake. A
         * plain TCP accept just flips the connection state, so offloading it
         * would cost a worker round trip and an inbox slot for no work. */
        if (connGetType(conn) == CONN_TYPE_TLS) conn->flags |= CONN_FLAG_ALLOW_ACCEPT_OFFLOAD;

        /* Make sure connection is not in an error state */
        if (connGetState(conn) != CONN_STATE_ACCEPTING) {
            serverLog(LL_VERBOSE, "Error creating an accepting connection for cluster node: %s",
                      connGetLastError(conn));
            connClose(conn);
            return;
        }

        connKeepAlive(conn, server.cluster_node_timeout / 1000 * 2);

        /* Use non-blocking I/O for cluster messages. */
        serverLog(LL_VERBOSE, "Accepting cluster node connection from %s:%d", cip, cport);

        /* Install before offloading: that path skips connAccept() below, and a
         * TLS retry that cannot re-offload only ever invokes conn_handler. */
        conn->conn_handler = clusterConnAcceptHandler;

        /* Try to offload the TLS accept handshake to an I/O thread.
         * If offload succeeds, the completion handler will create the
         * clusterLink and install the read handler. */
        if (trySendClusterAcceptToIOThreads(conn) == C_OK) continue;

        /* Synchronous fallback: accept inline. connAccept() may call our
         * handler directly or schedule it for later depending on
         * connection implementation. */
        if (connAccept(conn, clusterConnAcceptHandler) == C_ERR) {
            if (connGetState(conn) == CONN_STATE_ERROR)
                serverLog(LL_VERBOSE, "Error accepting cluster node connection: %s", connGetLastError(conn));
            connClose(conn);
            return;
        }
    }
}

/* Drain complete packets queued at the start of rcvbuf.
 *
 * io_complete_bytes marks the bytes the I/O thread determined contain
 * only complete packets. The protocol's processMessage() reads from the front
 * of rcvbuf, so each packet is slid down to offset 0 in turn and the unparsed
 * tail is compacted once at the end.
 *
 * Returns 1 if the link is still valid after all packets were applied, or
 * 0 if packet processing freed the link. */
static int clusterDrainCompletePackets(clusterLink *link) {
    size_t buf_len = link->rcvbuf_len;
    size_t consumed = 0;

    while (link->io_complete_bytes > 0) {
        uint32_t totlen = clusterCurrentBus->validateMessageHeader(link->rcvbuf + consumed);

        serverAssert(totlen > 0);
        serverAssert(link->io_complete_bytes >= totlen);
        serverAssert(link->io_complete_packets > 0);

        link->io_complete_bytes -= totlen;
        link->io_complete_packets--;

        /* Copy just this packet, not the whole remaining tail, which would make
         * the drain quadratic. Safe because the copy writes [0, totlen) while
         * the bytes not yet consumed start at consumed + totlen. */
        if (consumed > 0) memmove(link->rcvbuf, link->rcvbuf + consumed, totlen);
        consumed += totlen;

        link->rcvbuf_len = totlen;
        if (!clusterCurrentBus->processMessage(link)) {
            return 0;
        }
    }

    link->rcvbuf_len = buf_len - consumed;
    if (consumed > 0 && link->rcvbuf_len > 0) {
        memmove(link->rcvbuf, link->rcvbuf + consumed, link->rcvbuf_len);
    }

    return 1;
}

static void clusterShrinkRcvbuf(clusterLink *link) {
    /* Shrink around any leftover partial packet, plus headroom. Requiring an
     * empty buffer would pin a busy link at its high-water mark. */
    size_t target = link->rcvbuf_len + RCVBUF_INIT_LEN;
    if (target < RCVBUF_INIT_LEN) target = RCVBUF_INIT_LEN;
    if (link->rcvbuf_alloc <= target) return;

    size_t prev_rcvbuf_alloc = link->rcvbuf_alloc;
    link->rcvbuf = zrealloc(link->rcvbuf, target);
    link->rcvbuf_alloc = target;
    server.stat_cluster_links_memory += link->rcvbuf_alloc - prev_rcvbuf_alloc;
}

/* Find the maximal prefix of rcvbuf that contains only complete packets.
 *
 * Scans the buffer using the protocol's validateMessageHeader() to check each
 * header and get its total length. complete_bytes is the number of bytes at
 * the start of rcvbuf that contain complete packets, and complete_packets is
 * the number of packets in that prefix.
 *
 * Thread-safe as long as validateMessageHeader() is: reads only from the
 * provided buffer and writes only to the output parameters. Does not touch
 * clusterNode, clusterState, or any main-thread structure. */
void clusterFindCompletePackets(char *rcvbuf,
                                size_t rcvbuf_len,
                                size_t *complete_bytes,
                                size_t *complete_packets,
                                clusterIOResult *result) {
    size_t offset = 0;

    *complete_bytes = 0;
    *complete_packets = 0;
    *result = CLUSTER_IO_OK;

    while (offset < rcvbuf_len) {
        size_t remaining = rcvbuf_len - offset;

        /* Need at least the header to determine message length. */
        if (remaining < RCVBUF_MIN_READ_LEN) break;

        /* Validate the header and get the message length. */
        uint32_t totlen = clusterCurrentBus->validateMessageHeader(rcvbuf + offset);
        if (totlen == 0) {
            *complete_bytes = offset; /* preserve any valid prefix already scanned */
            *result = CLUSTER_IO_BAD_HEADER;
            return;
        }

        /* Wait for the full message to arrive. */
        if (remaining < totlen) break;

        offset += totlen;
        (*complete_packets)++;
    }

    *complete_bytes = offset;
}

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. */
void clusterReadHandler(connection *conn) {
    char buf[sizeof(clusterMsgSendBlock)];
    ssize_t nread;
    clusterLink *link = connGetPrivateData(conn);
    unsigned int readlen, rcvbuflen;
    size_t totread = 0;

    /* A worker read job is still in flight or its completion hasn't been
     * consumed yet. Do not touch the framed packets or rcvbuf from the main
     * thread until clusterHandleReadCompletion() transitions the link back
     * to idle. */
    if (link->io_read_state != CLUSTER_LINK_IO_IDLE) return;

    if (!clusterDrainCompletePackets(link)) return;

    /* Try to offload the read first. If offload is unavailable (pool inactive,
     * queue full), fall back to the synchronous path below. */
    if (trySendClusterReadToIOThreads(link) == C_OK) return;

    /* Bounded like clusterWriteHandler(), so a peer that keeps the socket
     * readable cannot hold the main thread. The rest is read on the next
     * event. */
    while (totread < NET_MAX_WRITES_PER_EVENT) {
        rcvbuflen = link->rcvbuf_len;
        if (rcvbuflen < RCVBUF_MIN_READ_LEN) {
            /* First, obtain the first bytes to get the full message
             * length. */
            readlen = RCVBUF_MIN_READ_LEN - rcvbuflen;
        } else {
            uint32_t totlen;
            if (rcvbuflen == RCVBUF_MIN_READ_LEN) {
                /* Perform some sanity check on the message signature
                 * and length. */
                totlen = clusterCurrentBus->validateMessageHeader(link->rcvbuf);
                if (!totlen) {
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
                    freeClusterLink(link);
                    return;
                }
            } else {
                /* We already validated; re-read totlen from the buffer.
                 * The protocol must store it as a 32-bit big-endian value
                 * at a fixed offset within the first RCVBUF_MIN_READ_LEN bytes. */
                totlen = clusterCurrentBus->validateMessageHeader(link->rcvbuf);
            }
            readlen = totlen - rcvbuflen;
            if (readlen > sizeof(buf)) readlen = sizeof(buf);
        }

        nread = connRead(conn, buf, readlen);
        if (nread == -1 && (connGetState(conn) == CONN_STATE_CONNECTED)) return; /* No more data ready. */

        if (nread <= 0) {
            /* I/O error... */
            serverLog(LL_DEBUG, "I/O error reading from node link (%.40s:%s) (%s): %s",
                      clusterLinkGetNodeName(link), link->inbound ? "inbound" : "outbound",
                      clusterLinkGetHumanNodeName(link),
                      (nread == 0) ? "connection closed" : connGetLastError(conn));
            freeClusterLink(link);
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
            rcvbuflen += nread;
            totread += nread;
        }

        /* Total length obtained? Process this packet. */
        if (rcvbuflen >= RCVBUF_MIN_READ_LEN) {
            uint32_t totlen = clusterCurrentBus->validateMessageHeader(link->rcvbuf);
            if (totlen && rcvbuflen == totlen) {
                if (clusterCurrentBus->processMessage(link)) {
                    link->rcvbuf_len = 0;
                    clusterShrinkRcvbuf(link);
                } else {
                    return; /* Link no longer valid. */
                }
            }
        }
    }
}

/* ===================== Cluster I/O Thread Worker Functions ==================
 * These run on I/O threads. They must NOT touch clusterNode, clusterState,
 * server.stat_cluster_links_memory, or any main-thread-only structure.
 *
 * Read and write jobs are mutually exclusive per link, so the shared
 * io_result field still has only one writer at a time. */

/* I/O thread worker: read bytes from a cluster link's connection, grow the
 * receive buffer as needed, frame packets, and post a completion.
 *
 * Buffer growth follows the same logic as clusterReadHandler:
 *   - If < 1 MB, grow to twice the required size.
 *   - If >= 1 MB, grow by 1 MB.
 * stat_cluster_links_memory adjustment is deferred to the main-thread
 * completion handler. */
void clusterReadJob(clusterLink *link) {
    connection *conn = link->conn;
    clusterIOResult result = CLUSTER_IO_OK;
    ssize_t total_read = 0;

    /* I/O thread invariant: we must be in PENDING state. */
    serverAssert(link->io_read_state == CLUSTER_LINK_IO_PENDING);
    serverAssert(link->io_write_state == CLUSTER_LINK_IO_IDLE);

    /* The link holds a connection for as long as an I/O job can be in flight:
     * link->conn is only cleared on the immediate free path, which is
     * unreachable while io_refs > 0, and the async-close path keeps the
     * connection alive until the last completion. */
    serverAssert(conn != NULL);

    /* Bounded so one job cannot balloon rcvbuf or hold a worker; the socket stays
     * readable and the next event continues. */
    while (total_read < (ssize_t)RCVBUF_MAX_PREALLOC) {
        /* Ensure at least some space in rcvbuf. */
        size_t rcvbuf_len = link->rcvbuf_len;
        if (rcvbuf_len == link->rcvbuf_alloc) {
            size_t required = link->rcvbuf_alloc + 1;
            link->rcvbuf_alloc = required < RCVBUF_MAX_PREALLOC ? required * 2 : required + RCVBUF_MAX_PREALLOC;
            link->rcvbuf = zrealloc(link->rcvbuf, link->rcvbuf_alloc);
        }

        size_t avail = link->rcvbuf_alloc - rcvbuf_len;
        ssize_t nread = connRead(conn, link->rcvbuf + rcvbuf_len, avail);

        if (nread > 0) {
            link->rcvbuf_len = rcvbuf_len + nread;
            total_read += nread;
            continue;
        }

        if (nread == 0) {
            /* EOF */
            result = CLUSTER_IO_EOF;
            break;
        }

        /* nread == -1 */
        if (connGetState(conn) == CONN_STATE_CONNECTED) {
            /* EAGAIN — no more data right now, that's fine. */
            break;
        }
        /* Real read error. */
        result = CLUSTER_IO_READ_ERROR;
        break;
    }

    /* If we read something, frame the complete packet prefix. */
    if (total_read > 0) {
        size_t complete_bytes = 0;
        size_t complete_packets = 0;
        clusterIOResult frame_result;
        serverAssert(link->io_complete_bytes == 0);
        serverAssert(link->io_complete_packets == 0);
        clusterFindCompletePackets(link->rcvbuf, link->rcvbuf_len,
                                   &complete_bytes, &complete_packets, &frame_result);
        link->io_complete_bytes = complete_bytes;
        link->io_complete_packets = complete_packets;

        /* If framing found a protocol error, that takes priority. */
        if (frame_result != CLUSTER_IO_OK) {
            result = frame_result;
        }
    }

    /* Post result and completion to the main thread. */
    link->io_result = result;
    sendToMainThread(link, JOB_RES_CLUSTER_READ);
}

/* I/O thread worker: write bytes from the canonical send queue to the
 * connection, starting at io_head_offset and stopping once it reaches
 * io_last_send_block.
 *
 * The worker does NOT pop nodes or decrement refcounts — clusterMsgSendBlock
 * refcounts are non-atomic and blocks can be shared across links. The
 * worker records how many head nodes were fully sent (io_nodes_sent) and
 * the byte offset into the next partially-sent node (io_head_offset). The
 * main-thread completion handler uses these to pop nodes and update memory
 * accounting. */
void clusterWriteJob(clusterLink *link) {
    connection *conn = link->conn;
    clusterIOResult result = CLUSTER_IO_OK;
    int nodes_sent = 0;
    listNode *node = listFirst(link->send_msg_queue);
    size_t head_offset = link->io_head_offset;
    size_t totwritten = 0;

    /* I/O thread invariant: we must be in PENDING state. */
    serverAssert(link->io_write_state == CLUSTER_LINK_IO_PENDING);
    serverAssert(link->io_read_state == CLUSTER_LINK_IO_IDLE);

    /* See clusterReadJob(): link->conn outlives any in-flight I/O job. */
    serverAssert(conn != NULL);

    /* Bounded like the synchronous path. A worker is shared, so a link with a
     * large backlog must not hold it while other jobs wait; whatever is left
     * goes out on the next writable event. */
    while (node && totwritten < NET_MAX_WRITES_PER_EVENT) {
        clusterMsgSendBlock *block = (clusterMsgSendBlock *)node->value;
        size_t msg_len = block->len;
        size_t msg_offset = head_offset;

        ssize_t nwritten = connWrite(conn, (char *)block->data + msg_offset, msg_len - msg_offset);
        if (nwritten <= 0) {
            if (nwritten == -1 && connGetState(conn) == CONN_STATE_CONNECTED) {
                break; /* EAGAIN */
            }
            result = CLUSTER_IO_WRITE_ERROR;
            break;
        }

        head_offset += nwritten;
        totwritten += nwritten;
        if (head_offset < msg_len) {
            break; /* Partial write */
        }

        /* Fully sent this message — advance to next. */
        head_offset = 0;
        nodes_sent++;
        if (node == link->io_last_send_block) break;
        node = listNextNode(node);
    }

    link->io_nodes_sent = nodes_sent;
    link->io_head_offset = head_offset;
    link->io_result = result;
    sendToMainThread(link, JOB_RES_CLUSTER_WRITE);
}

/* I/O thread worker: perform TLS accept handshake on a cluster connection.
 * No clusterLink exists yet — it is created by the main thread on success. */
void clusterAcceptJob(connection *conn) {
    /* The dispatcher holds a reference on the connection for the whole job, so
     * it cannot be NULL here. Returning early instead would skip
     * sendToMainThread() and leak a pending response forever. */
    serverAssert(conn != NULL);
    connAccept(conn, NULL);
    sendToMainThread(conn, JOB_RES_CLUSTER_ACCEPT);
}

/* ===================== Cluster I/O Completion Handlers =====================
 * These handlers are called from processIOThreadsResponses() when cluster
 * I/O completions are dequeued from the response queue. The tagged pointer
 * is the clusterLink* (read/write) or connection* (accept) directly. */

void clusterHandleReadCompletion(clusterLink *link) {
    connection *conn = link->conn;

    /* Apply deferred connection state transitions. Even if freeClusterLink()
     * was called while the job was in flight, link->conn remains valid until
     * the final async_close teardown runs after the last completion. */
    if (conn) {
        connSetPostponeUpdateState(conn, 0);
        connUpdateState(conn);
    }

    /* Transition back to idle and release the I/O ref. */
    serverAssert(link->io_read_state == CLUSTER_LINK_IO_PENDING);
    serverAssert(link->io_write_state == CLUSTER_LINK_IO_IDLE);
    serverAssert(link->io_refs > 0);
    link->io_read_state = CLUSTER_LINK_IO_IDLE;
    link->io_refs--;

    /* Update stat_cluster_links_memory for rcvbuf growth that occurred on
     * the I/O thread (the I/O thread grows rcvbuf_alloc but does not touch
     * the global stat). */
    if (link->rcvbuf_alloc > link->rcvbuf_alloc_at_dispatch) {
        server.stat_cluster_links_memory += link->rcvbuf_alloc - link->rcvbuf_alloc_at_dispatch;
    }

    /* If the link was already marked for async close, check if we can
     * perform the final free now that io_refs has been decremented. */
    if (link->async_close) {
        if (link->io_refs == 0) {
            freeClusterLink(link);
        }
        return;
    }

    clusterIOResult result = link->io_result;

    /* Handle error results: log and tear down the link. */
    if (result == CLUSTER_IO_BAD_HEADER || result == CLUSTER_IO_BAD_LENGTH) {
        /* Drain any valid packets that preceded the bad header/length before
         * closing, so we don't silently drop already-complete messages. */
        if (link->io_complete_bytes > 0) {
            if (!clusterDrainCompletePackets(link)) return;
        }
        serverLog(LL_WARNING, "Bad cluster packet header/length from node %.40s:%s (%s)",
                  clusterLinkGetNodeName(link),
                  link->inbound ? "inbound" : "outbound",
                  clusterLinkGetHumanNodeName(link));
        freeClusterLink(link);
        return;
    }

    if (result == CLUSTER_IO_READ_ERROR || result == CLUSTER_IO_EOF) {
        serverLog(LL_DEBUG, "I/O error reading from node link (%.40s:%s) (%s): %s",
                  clusterLinkGetNodeName(link),
                  link->inbound ? "inbound" : "outbound",
                  clusterLinkGetHumanNodeName(link),
                  (result == CLUSTER_IO_EOF) ? "connection closed" : "read error");
    }

    if (!clusterDrainCompletePackets(link)) return;

    clusterShrinkRcvbuf(link);

    if (result == CLUSTER_IO_READ_ERROR || result == CLUSTER_IO_EOF) {
        freeClusterLink(link);
    }
}

void clusterHandleWriteCompletion(clusterLink *link) {
    connection *conn = link->conn;

    /* Apply deferred connection state transitions. */
    if (conn) {
        connSetPostponeUpdateState(conn, 0);
        connUpdateState(conn);
    }

    /* Transition back to idle and release the I/O ref. */
    serverAssert(link->io_write_state == CLUSTER_LINK_IO_PENDING);
    serverAssert(link->io_read_state == CLUSTER_LINK_IO_IDLE);
    serverAssert(link->io_refs > 0);
    link->io_write_state = CLUSTER_LINK_IO_IDLE;
    link->io_refs--;

    /* Pop fully-sent nodes from the canonical send queue. The I/O thread
     * recorded how many nodes it fully sent (io_nodes_sent) without
     * modifying the list. We pop them here on the main thread where
     * refcount decrements and memory accounting are safe. Sent bytes are
     * counted by the protocol when the message is queued. */
    for (int i = 0; i < link->io_nodes_sent; i++) {
        listNode *head = listFirst(link->send_msg_queue);
        serverAssert(head != NULL);
        clusterMsgSendBlock *block = (clusterMsgSendBlock *)head->value;
        uint32_t blocklen = block->totlen;
        listDelNode(link->send_msg_queue, head);
        link->send_msg_queue_mem -= sizeof(listNode) + blocklen;
        server.stat_cluster_links_memory -= sizeof(listNode);
    }

    link->head_msg_send_offset = listLength(link->send_msg_queue) > 0 ? link->io_head_offset : 0;
    link->io_last_send_block = NULL;
    link->io_head_offset = 0;
    link->io_nodes_sent = 0;

    /* If the link was already marked for async close, check if we can
     * perform the final free now that io_refs has been decremented. */
    if (link->async_close) {
        if (link->io_refs == 0) {
            freeClusterLink(link);
        }
        return;
    }

    clusterIOResult result = link->io_result;

    /* Handle write error: log and tear down the link. */
    if (result == CLUSTER_IO_WRITE_ERROR) {
        serverLog(LL_DEBUG, "I/O error writing to node link (%.40s:%s) (%s)",
                  clusterLinkGetNodeName(link),
                  link->inbound ? "inbound" : "outbound",
                  clusterLinkGetHumanNodeName(link));
        freeClusterLink(link);
        return;
    }

    /* If data remains, wait for the next writable event before attempting
     * another offload. This avoids a tight completion -> offload loop when
     * the transport reports EAGAIN with no write progress. */
    if (listLength(link->send_msg_queue) > 0) {
        if (link->conn) {
            connSetWriteHandlerWithBarrier(link->conn, clusterWriteHandler, 1);
        }
    } else if (link->conn && connHasWriteHandler(link->conn)) {
        connSetWriteHandler(link->conn, NULL);
    }
}

void clusterHandleAcceptCompletion(connection *conn) {
    conn->flags &= ~CONN_FLAG_ACCEPT_OFFLOAD_PENDING;
    /* Runs conn_handler if the handshake finished, re-arms the TLS event if not. */
    connSetPostponeUpdateState(conn, 0);
    connUpdateState(conn);
    connDecrRefs(conn);
    if ((conn->flags & CONN_FLAG_CLOSE_SCHEDULED) && !connHasRefs(conn)) {
        connClose(conn);
    }
}
