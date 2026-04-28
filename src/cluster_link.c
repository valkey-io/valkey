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

const char **clusterLinkDebugHelp(void) {
    static const char *help[] = {
        "CLUSTERLINK KILL <to|from|all> <node-id>",
        "    Kills the link based on the direction to/from (both) "
        "with the provided node.",
        NULL};
    return help;
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

    /* Protocol-specific post-connect action (e.g. send initial PING). */
    if (clusterCurrentBus->postConnect) clusterCurrentBus->postConnect(link);

    serverLog(LL_DEBUG, "Connecting with Node %.40s at %s:%d", node->name, node->ip, node->cport);
}

/* Called when a new inbound connection is accepted on the cluster bus. */
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

/* Establish outbound links to nodes that don't have one.
 * Throttles reconnection attempts and cleans up timed-out handshakes. */
void clusterConnectNodes(void) {
    mstime_t now = mstime();
    mstime_t handshake_timeout = max(server.cluster_node_timeout, 1000);
    mstime_t reconnect_interval = server.cluster_node_timeout / 2;
    if (reconnect_interval <= 0) reconnect_interval = 1;

    /* Budget: try to contact every node NODE_CONNECTION_RETRIES_PER_TIMEOUT
     * times within node_timeout. Each cron tick gets a proportional share. */
    long long budget = (long long)dictSize(server.cluster->nodes) *
                       CLUSTER_CRON_PERIOD_MS / reconnect_interval *
                       NODE_CONNECTION_RETRIES_PER_TIMEOUT;
    if (budget < 1) budget = 1;

    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->flags & (CLUSTER_NODE_MYSELF | CLUSTER_NODE_NOADDR)) continue;

        /* Free links that exceeded the send buffer limit. */
        freeClusterLinkOnBufferLimitReached(node->link);
        freeClusterLinkOnBufferLimitReached(node->inbound_link);

        if (node->link) continue;

        /* Remove handshake nodes that have timed out. */
        if (nodeInHandshake(node) && now - node->ctime > handshake_timeout) {
            clusterDelNode(node);
            continue;
        }

        /* Throttle reconnection attempts. If an inbound link exists the peer
         * already knows us, so reconnect immediately without throttling. */
        if (!node->inbound_link) {
            mstime_t backoff = reconnect_interval / NODE_CONNECTION_RETRIES_PER_TIMEOUT;
            if (now - node->outbound_link_attempt_time < backoff && budget <= 0) continue;
        }
        node->outbound_link_attempt_time = now;
        budget--;

        clusterLink *link = createClusterLink(node);
        link->conn = connCreate(connTypeOfCluster());
        connSetPrivateData(link->conn, link);
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

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. */
void clusterReadHandler(connection *conn) {
    char buf[sizeof(clusterMsgSendBlock)];
    ssize_t nread;
    clusterLink *link = connGetPrivateData(conn);
    unsigned int readlen, rcvbuflen;

    while (1) { /* Read as long as there is data to read. */
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
        }

        /* Total length obtained? Process this packet. */
        if (rcvbuflen >= RCVBUF_MIN_READ_LEN) {
            uint32_t totlen = clusterCurrentBus->validateMessageHeader(link->rcvbuf);
            if (totlen && rcvbuflen == totlen) {
                if (clusterCurrentBus->processMessage(link)) {
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
}
