/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include "fake_connection.hpp"

#include <cstddef>
#include <cstring>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
#include "anet.h"
#include "cluster.h"
#include "cluster_legacy.h"
#include "connhelpers.h"
#include "io_threads.h"
#include "server.h"

clusterLink *createClusterLink(clusterNode *node);
int freeClusterLink(clusterLink *link);
void testOnlyFreeClusterLinkOnBufferLimitReached(clusterLink *link);
}

/* Mirrors clusterMsgSendBlock, which is private to cluster_legacy.c. The
 * layout must match exactly: the write job and its completion both read the
 * message's own totlen and type out of the block. */
typedef struct TestMsgBlock {
    size_t totlen;
    int refcount;
    union {
        clusterMsg msg;
        clusterMsgLight msg_light;
    } data[1];
} TestMsgBlock;

/* A fake connection type that, like TLS, has no stateless I/O, so a link
 * built on it takes the half duplex dispatch path. */
static int fakeTlsConnGetType(void) {
    return CONN_TYPE_TLS;
}

static ConnectionType *fakeTlsConnType(void) {
    static ConnectionType ct;
    ct = *fakeConnType();
    ct.get_type = fakeTlsConnGetType;
    ct.write_stateless = NULL;
    ct.read_stateless = NULL;
    return &ct;
}

class ClusterIOOffloadTest : public ::testing::Test {
  protected:
    static const int MAX_OWNED = 64;
    fakeConnection *owned_conns[MAX_OWNED];
    int owned_conns_count;
    clusterLink *owned_links[MAX_OWNED];
    int owned_links_count;

    void SetUp() override {
        owned_conns_count = 0;
        owned_links_count = 0;
        memset(&server, 0, sizeof(server));
        server.el = aeCreateEventLoop(1024);
        server.io_threads_num = 2;
        server.active_io_threads_num = 2;
        testOnlyInitIOThreadQueues();
        server.cluster_link_msg_queue_limit_bytes = 1024;
        server.logfile = zstrdup("");
        server.verbosity = LL_WARNING;
        server.cluster = (clusterState *)zcalloc(sizeof(clusterState));
    }

    void TearDown() override {
        for (int i = 0; i < owned_links_count; i++) {
            if (owned_links[i]) freeClusterLink(owned_links[i]);
        }
        for (int i = 0; i < owned_conns_count; i++) {
            connFreeFake(owned_conns[i]);
        }
        if (server.cluster) {
            zfree(server.cluster);
            server.cluster = NULL;
        }
        zfree(server.logfile);
        server.logfile = NULL;
        /* Every test must leave the cluster pending-response count balanced.
         * A dispatch that returns without publishing a result would strand it
         * forever and stall processIOThreadsResponses(). */
        EXPECT_EQ(testOnlyGetClusterIOPendingResponses(), 0u) << "leaked a cluster I/O pending response";
        testOnlyFreeIOThreadQueues();
        if (server.el) {
            aeDeleteEventLoop(server.el);
            server.el = NULL;
        }
    }

    /* A connection in the state cluster code expects post-accept: established,
     * cluster-owned, one reference held. */
    fakeConnection *makeConn(ConnectionOwnerKind owner_kind = CONN_OWNER_CLUSTER_LINK) {
        fakeConnection *fc = connCreateFake(4096);
        fc->conn.state = CONN_STATE_CONNECTED;
        fc->conn.refs = 1;
        fc->conn.owner_kind = owner_kind;
        owned_conns[owned_conns_count++] = fc;
        return fc;
    }

    /* As clusterAcceptHandler() leaves it: conn_handler installed before dispatch. */
    fakeConnection *makeAcceptConn() {
        fakeConnection *fc = makeConn(CONN_OWNER_CLUSTER_LINK);
        fc->conn.conn_handler = clusterConnAcceptHandler;
        return fc;
    }

    clusterLink *makeLink() {
        clusterLink *link = createClusterLink(NULL);
        fakeConnection *fc = makeConn();
        link->conn = &fc->conn;
        connSetPrivateData(link->conn, link);
        owned_links[owned_links_count++] = link;
        return link;
    }

    /* A link whose read and write jobs stay mutually exclusive. */
    clusterLink *makeHalfDuplexLink() {
        clusterLink *link = makeLink();
        link->conn->type = fakeTlsConnType();
        return link;
    }

    /* A link on a real TCP connection type over one end of a socketpair. The
     * other end is returned in peer_fd and must be closed by the caller. */
    clusterLink *makeSocketLink(int *peer_fd) {
        if (connectionByType(CONN_TYPE_SOCKET) == NULL) connTypeInitialize();
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return NULL;
        anetNonBlock(NULL, fds[0]);
        connection *conn = connCreateAccepted(connectionByType(CONN_TYPE_SOCKET), fds[0], NULL);
        conn->state = CONN_STATE_CONNECTED;
        conn->owner_kind = CONN_OWNER_CLUSTER_LINK;
        clusterLink *link = createClusterLink(NULL);
        link->conn = conn;
        connSetPrivateData(conn, link);
        owned_links[owned_links_count++] = link;
        *peer_fd = fds[1];
        return link;
    }

    void trackLink(clusterLink *link) {
        owned_links[owned_links_count++] = link;
    }

    void releaseLinkOwnership(clusterLink *link) {
        for (int i = 0; i < owned_links_count; i++) {
            if (owned_links[i] == link) {
                owned_links[i] = NULL;
                break;
            }
        }
    }

    /* Queue one message of msg_len wire bytes. The block is sized to hold the
     * message, since the write job reads msg_len bytes starting at data[0]. */
    void enqueueFakeMsg(clusterLink *link, uint32_t msg_len = 64) {
        size_t alloc = offsetof(TestMsgBlock, data) + msg_len;
        if (alloc < sizeof(TestMsgBlock)) alloc = sizeof(TestMsgBlock);
        TestMsgBlock *blk = (TestMsgBlock *)zcalloc(alloc);
        blk->refcount = 1;
        blk->totlen = alloc;
        clusterMsg *msg = &blk->data[0].msg;
        memcpy(msg->sig, "RCmb", 4);
        msg->totlen = htonl(msg_len);
        msg->ver = htons(CLUSTER_PROTO_VER);
        msg->type = htons(CLUSTERMSG_TYPE_PING);
        listAddNodeTail(link->send_msg_queue, blk);
        link->send_msg_queue_mem += sizeof(listNode) + blk->totlen;
    }

    /* A minimal well-formed cluster packet. The type is chosen so that
     * clusterProcessPacket() accepts it from an unknown sender and only bumps
     * the per-type received counter. */
    unsigned char *buildRawPacket(uint32_t totlen) {
        unsigned char *raw = (unsigned char *)zcalloc(totlen);
        clusterMsgHeader *hdr = (clusterMsgHeader *)(void *)raw;
        memcpy(hdr->sig, "RCmb", 4);
        hdr->totlen = htonl(totlen);
        hdr->ver = htons(CLUSTER_PROTO_VER);
        hdr->type = htons(CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK);
        return raw;
    }

    /* A packet of arbitrary length. FAILOVER_AUTH_ACK is length-checked exactly,
     * so an unknown type is used instead: clusterIsValidPacket() accepts any
     * totlen for one, and the stats index is guarded by CLUSTERMSG_TYPE_COUNT. */
    unsigned char *buildOversizedPacket(uint32_t totlen) {
        unsigned char *raw = buildRawPacket(totlen);
        ((clusterMsgHeader *)(void *)raw)->type = htons(CLUSTERMSG_TYPE_COUNT + 1);
        return raw;
    }

    /* Exactly one complete packet, nothing after it. */
    void seedOneCompletePacket(fakeConnection *fc) {
        unsigned char *pkt = buildRawPacket(CLUSTERMSG_MIN_LEN);
        fakeConnSetReadData(fc, pkt, CLUSTERMSG_MIN_LEN);
        zfree(pkt);
    }

    /* One complete packet followed by a header that fails signature validation,
     * so framing reports a protocol error with a valid prefix in front of it.
     * The garbage must be long enough for the framing step to inspect it as a
     * header rather than treat it as a partial read. */
    void seedCompletePacketFollowedByGarbage(fakeConnection *fc) {
        const size_t garbage_len = 64;
        size_t len = CLUSTERMSG_MIN_LEN + garbage_len;
        unsigned char *pkt = buildRawPacket(CLUSTERMSG_MIN_LEN);
        unsigned char *buf = (unsigned char *)zcalloc(len);
        memcpy(buf, pkt, CLUSTERMSG_MIN_LEN);
        memset(buf + CLUSTERMSG_MIN_LEN, 'Z', garbage_len);
        fakeConnSetReadData(fc, buf, len);
        zfree(pkt);
        zfree(buf);
    }

    /* Feed the connection one complete packet plus a partial tail, so a read
     * job frames exactly one packet. */
    void seedReadableSocket(fakeConnection *fc) {
        unsigned char *pkt = buildRawPacket(CLUSTERMSG_MIN_LEN);
        unsigned char *buf = (unsigned char *)zmalloc(CLUSTERMSG_MIN_LEN + 1);
        memcpy(buf, pkt, CLUSTERMSG_MIN_LEN);
        buf[CLUSTERMSG_MIN_LEN] = 'T';
        fakeConnSetReadData(fc, buf, CLUSTERMSG_MIN_LEN + 1);
        zfree(pkt);
        zfree(buf);
    }

    /* Two complete packets plus a partial tail larger than RCVBUF_INIT_LEN. */
    size_t seedPacketsAndLargePartialTail(fakeConnection *fc) {
        const uint32_t whole = CLUSTERMSG_MIN_LEN;
        const size_t partial = RCVBUF_INIT_LEN + 512;
        size_t len = whole * 2 + partial;
        unsigned char *buf = (unsigned char *)zcalloc(len);
        unsigned char *pkt = buildRawPacket(whole);
        memcpy(buf, pkt, whole);
        memcpy(buf + whole, pkt, whole);
        /* A valid header whose packet has not fully arrived yet. */
        unsigned char *tail = buildRawPacket(whole);
        memcpy(buf + whole * 2, tail, partial);
        fakeConnSetReadData(fc, buf, len);
        zfree(pkt);
        zfree(tail);
        zfree(buf);
        return partial;
    }

    /* Three complete packets plus a partial tail. The middle packet is larger
     * than the first so that sliding it to the front is an overlapping copy.
     * Each packet carries a distinct type, so a packet landing at the wrong
     * offset shows up as a wrong per-type counter. */
    void seedThreePacketsAndTail(fakeConnection *fc) {
        const uint32_t small = CLUSTERMSG_MIN_LEN;
        const uint32_t large = CLUSTERMSG_MIN_LEN + 512;
        size_t len = small + large + small + 1;
        unsigned char *buf = (unsigned char *)zcalloc(len);
        unsigned char *p1 = buildRawPacket(small);
        unsigned char *p2 = buildOversizedPacket(large);
        unsigned char *p3 = buildRawPacket(small);
        ((clusterMsgHeader *)(void *)p3)->type = htons(CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST);
        memcpy(buf, p1, small);
        memcpy(buf + small, p2, large);
        memcpy(buf + small + large, p3, small);
        buf[small + large + small] = 'T';
        fakeConnSetReadData(fc, buf, len);
        zfree(p1);
        zfree(p2);
        zfree(p3);
        zfree(buf);
    }

    /* Run the worker side of a dispatched job inline, then let the main thread
     * consume the completion exactly as the event loop would. */
    void runInlineWorkerAndDrain(void (*job)(clusterLink *), clusterLink *link) {
        job(link);
        processIOThreadsResponses();
    }
};

/* --- Read path -------------------------------------------------------- */

TEST_F(ClusterIOOffloadTest, ReadJobFramesCompletePrefixAndLeavesTail) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedReadableSocket(fc);

    link->io_read_state = CLUSTER_LINK_IO_PENDING;
    clusterReadJob(link);

    EXPECT_EQ(link->io_complete_bytes, (size_t)CLUSTERMSG_MIN_LEN);
    EXPECT_EQ(link->io_complete_packets, 1u);
    /* The partial tail is read but deliberately not published. */
    EXPECT_EQ(link->rcvbuf_len, (size_t)CLUSTERMSG_MIN_LEN + 1);
    link->io_read_state = CLUSTER_LINK_IO_IDLE;
}

TEST_F(ClusterIOOffloadTest, ReadOffloadRoundTrip) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedReadableSocket(fc);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    EXPECT_EQ(link->io_read_state, CLUSTER_LINK_IO_PENDING);
    EXPECT_EQ(link->io_refs, 1);
    EXPECT_EQ(testOnlyGetClusterIOPendingResponses(), 1u);
    /* The counter tracks completions, so nothing is counted at dispatch. */
    EXPECT_EQ(server.stat_cluster_threaded_reads_processed, 0LL);

    runInlineWorkerAndDrain(clusterReadJob, link);

    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
    EXPECT_EQ(link->io_read_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_refs, 0);
    EXPECT_EQ(link->io_complete_bytes, 0u);
    EXPECT_EQ(link->io_complete_packets, 0u);
    /* Only the unparsed tail is left, compacted to the front. */
    EXPECT_EQ(link->rcvbuf_len, 1u);
    EXPECT_EQ(link->rcvbuf[0], 'T');
    EXPECT_EQ(server.stat_cluster_threaded_reads_processed, 1LL);
    EXPECT_EQ(fc->postpone_state, 0);
}

/* A peer that sends a valid packet and then hangs up must have that packet
 * applied before the link is torn down. Same for a hard read error and for a
 * malformed trailing header. All three drive the real worker so the result code
 * comes from clusterReadJob() rather than being injected. */
TEST_F(ClusterIOOffloadTest, ReadOffloadOnEofDrainsThenCloses) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedOneCompletePacket(fc);
    fc->eof = 1;

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterReadJob, link);
    releaseLinkOwnership(link);

    EXPECT_GE(fc->close_calls, 1);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
}

TEST_F(ClusterIOOffloadTest, ReadOffloadOnReadErrorDrainsThenCloses) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedOneCompletePacket(fc);
    fc->fail_read = 1;

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterReadJob, link);
    releaseLinkOwnership(link);

    EXPECT_GE(fc->close_calls, 1);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
}

TEST_F(ClusterIOOffloadTest, ReadOffloadOnProtocolErrorDrainsValidPrefixThenCloses) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedCompletePacketFollowedByGarbage(fc);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterReadJob, link);
    releaseLinkOwnership(link);

    EXPECT_GE(fc->close_calls, 1);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
}

/* --- Write path ------------------------------------------------------- */

TEST_F(ClusterIOOffloadTest, ReadOffloadDrainsMultiplePacketsAndCompactsTail) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedThreePacketsAndTail(fc);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterReadJob, link);

    /* Each packet must be seen exactly once, in its own right: a wrong slide
     * offset would put some other packet's header at rcvbuf[0]. */
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST], 1LL);
    EXPECT_EQ(link->io_complete_bytes, 0u);
    EXPECT_EQ(link->io_complete_packets, 0u);
    /* Only the unparsed tail survives, compacted to the front. */
    EXPECT_EQ(link->rcvbuf_len, 1u);
    EXPECT_EQ(link->rcvbuf[0], 'T');
}

/* A leftover partial packet must not pin rcvbuf at its high-water mark. */
TEST_F(ClusterIOOffloadTest, ReadCompletionShrinksAroundPartialTail) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    /* Bigger than RCVBUF_INIT_LEN, as a real partial packet usually is. */
    size_t partial = seedPacketsAndLargePartialTail(fc);
    ASSERT_GT(partial, (size_t)RCVBUF_INIT_LEN);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    clusterReadJob(link);
    size_t grown = link->rcvbuf_alloc;
    ASSERT_GT(grown, partial + RCVBUF_INIT_LEN);
    processIOThreadsResponses();

    /* Both packets applied; the tail survives and the buffer shrank around it. */
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 2LL);
    EXPECT_EQ(link->rcvbuf_len, partial);
    EXPECT_EQ(link->rcvbuf_alloc, partial + RCVBUF_INIT_LEN);
    EXPECT_LT(link->rcvbuf_alloc, grown);
    EXPECT_EQ(memcmp(link->rcvbuf, "RCmb", 4), 0);
}

/* One job must not read an unbounded stream; it stops on the budget. */
TEST_F(ClusterIOOffloadTest, ReadJobStopsAtReadBudget) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;

    const size_t stream = (size_t)RCVBUF_MAX_PREALLOC * 2;
    unsigned char *buf = (unsigned char *)zcalloc(stream);
    memset(buf, 'x', stream);
    fakeConnSetReadData(fc, buf, stream);
    zfree(buf);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    clusterReadJob(link);

    /* Stopped on the budget rather than draining the whole stream. */
    EXPECT_GE(fc->read_pos, (size_t)RCVBUF_MAX_PREALLOC);
    EXPECT_LT(fc->read_pos, stream);

    /* Garbage bytes, so framing reports a bad header and the link is torn down. */
    EXPECT_EQ(link->io_read_result, CLUSTER_IO_BAD_HEADER);
    processIOThreadsResponses();
    releaseLinkOwnership(link);
}

TEST_F(ClusterIOOffloadTest, WriteDispatchSnapshotsBoundary) {
    clusterLink *link = makeLink();
    enqueueFakeMsg(link);
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);

    EXPECT_NE(link->io_last_send_block, (listNode *)NULL);
    EXPECT_EQ(link->io_head_offset, 0u);

    /* Let the job run to completion rather than unwinding the state by hand. */
    runInlineWorkerAndDrain(clusterWriteJob, link);
    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_refs, 0);
}

TEST_F(ClusterIOOffloadTest, WriteOffloadRoundTripDrainsQueue) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    enqueueFakeMsg(link);
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    EXPECT_EQ(testOnlyGetClusterIOPendingResponses(), 1u);
    EXPECT_EQ(server.stat_cluster_threaded_writes_processed, 0LL);

    runInlineWorkerAndDrain(clusterWriteJob, link);

    /* Both messages fit in the 4096-byte sink, so the queue drains fully and
     * the write handler is uninstalled. */
    EXPECT_EQ(listLength(link->send_msg_queue), 0UL);
    EXPECT_EQ(link->head_msg_send_offset, 0u);
    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_refs, 0);
    EXPECT_EQ(link->conn->write_handler, (ConnectionCallbackFunc)NULL);
    EXPECT_EQ(server.stat_cluster_threaded_writes_processed, 1LL);
    EXPECT_EQ(fc->postpone_state, 0);
}

TEST_F(ClusterIOOffloadTest, WriteOffloadRoundTripPartialSendKeepsHandler) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    /* A sink smaller than the message forces a partial write. */
    fc->buf_size = 8;
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterWriteJob, link);

    EXPECT_EQ(listLength(link->send_msg_queue), 1UL);
    EXPECT_EQ(link->head_msg_send_offset, 8u);
    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    /* More to send, so the write handler must stay armed. */
    EXPECT_NE(link->conn->write_handler, (ConnectionCallbackFunc)NULL);
}

/* One job must not drain an arbitrarily large backlog: a worker is shared, so it
 * stops at NET_MAX_WRITES_PER_EVENT and the rest goes out on the next event. */
TEST_F(ClusterIOOffloadTest, WriteJobStopsAtWriteBudget) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    /* A sink far larger than the budget, so only the budget bounds the job. */
    zfree(fc->buffer);
    fc->buf_size = NET_MAX_WRITES_PER_EVENT * 4;
    fc->buffer = (char *)zmalloc(fc->buf_size);

    const uint32_t msg_len = 16 * 1024;
    const int msgs = NET_MAX_WRITES_PER_EVENT / msg_len + 4;
    for (int i = 0; i < msgs; i++) enqueueFakeMsg(link, msg_len);

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterWriteJob, link);

    /* Stopped on the budget, so messages are left and the handler stays armed. */
    EXPECT_LT(fc->written, (size_t)NET_MAX_WRITES_PER_EVENT + msg_len);
    EXPECT_GT(listLength(link->send_msg_queue), 0UL);
    EXPECT_NE(link->conn->write_handler, (ConnectionCallbackFunc)NULL);
    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_refs, 0);

    /* The next dispatch resumes from where it stopped and drains the rest. */
    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterWriteJob, link);
    EXPECT_EQ(listLength(link->send_msg_queue), 0UL);
    EXPECT_EQ(fc->written, (size_t)msg_len * msgs);
}

/* A hard write error must tear the link down. A -1 with the connection still
 * CONNECTED is EAGAIN instead, which the completion has to tell apart. */
TEST_F(ClusterIOOffloadTest, WriteOffloadHardErrorClosesLink) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    enqueueFakeMsg(link);
    fc->fail_write = 1;

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterWriteJob, link);
    releaseLinkOwnership(link);

    EXPECT_GE(fc->close_calls, 1);
}

/* EAGAIN is not an error: the message stays queued and the link survives. */
TEST_F(ClusterIOOffloadTest, WriteOffloadEagainKeepsLink) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    enqueueFakeMsg(link);
    fc->error = 1; /* The write returns -1 with errno set to EAGAIN. */

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterWriteJob, link);

    EXPECT_EQ(fc->close_calls, 0);
    EXPECT_EQ(listLength(link->send_msg_queue), 1UL);
    EXPECT_NE(link->conn->write_handler, (ConnectionCallbackFunc)NULL);
}

TEST_F(ClusterIOOffloadTest, WriteCompletionPopsOnlyVisibleNodes) {
    clusterLink *link = makeLink();
    enqueueFakeMsg(link);
    enqueueFakeMsg(link);

    link->io_write_state = CLUSTER_LINK_IO_PENDING;
    link->io_refs = 1;
    link->io_nodes_sent = 1;
    link->io_head_offset = 0;
    link->io_write_result = CLUSTER_IO_OK;

    clusterHandleWriteCompletion(link);

    EXPECT_EQ(listLength(link->send_msg_queue), 1UL);
}

TEST_F(ClusterIOOffloadTest, WriteCompletionPartialSendUpdatesHeadOffset) {
    clusterLink *link = makeLink();
    enqueueFakeMsg(link);

    link->io_write_state = CLUSTER_LINK_IO_PENDING;
    link->io_refs = 1;
    link->io_nodes_sent = 0;
    link->io_head_offset = 7;
    link->io_write_result = CLUSTER_IO_OK;

    clusterHandleWriteCompletion(link);

    EXPECT_EQ(listLength(link->send_msg_queue), 1UL);
    EXPECT_EQ(link->head_msg_send_offset, 7u);
}

/* --- Dispatch is skipped while the connection is not established ------ */

TEST_F(ClusterIOOffloadTest, WriteDispatchSkippedWhileConnecting) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    fc->conn.state = CONN_STATE_CONNECTING;
    enqueueFakeMsg(link);

    /* C_OK, so the caller does not fall back to a synchronous write that would
     * fail the same way and tear the link down. */
    EXPECT_EQ(trySendClusterWriteToIOThreads(link), C_OK);

    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_refs, 0);
    EXPECT_EQ(link->conn->refs, 1);
    EXPECT_EQ(fc->postpone_state, 0);
    /* The message stays queued for the next dispatch. */
    EXPECT_EQ(listLength(link->send_msg_queue), 1UL);
    /* Not a fallback: no I/O was attempted anywhere. */
    EXPECT_EQ(server.stat_cluster_io_main_thread_fallbacks, 0LL);
}

TEST_F(ClusterIOOffloadTest, ReadDispatchSkippedWhileConnecting) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    fc->conn.state = CONN_STATE_CONNECTING;

    EXPECT_EQ(trySendClusterReadToIOThreads(link), C_OK);

    EXPECT_EQ(link->io_read_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_refs, 0);
    EXPECT_EQ(link->conn->refs, 1);
    EXPECT_EQ(fc->postpone_state, 0);
    EXPECT_EQ(server.stat_cluster_io_main_thread_fallbacks, 0LL);
}

TEST_F(ClusterIOOffloadTest, WriteDispatchSkippedWhileAccepting) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    fc->conn.state = CONN_STATE_ACCEPTING;
    enqueueFakeMsg(link);

    EXPECT_EQ(trySendClusterWriteToIOThreads(link), C_OK);

    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(listLength(link->send_msg_queue), 1UL);
    EXPECT_EQ(server.stat_cluster_io_main_thread_fallbacks, 0LL);
}

/* --- Fallback paths --------------------------------------------------- */

TEST_F(ClusterIOOffloadTest, ReadDispatchInboxFullUnwindsAndCountsFallback) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    testOnlyFillIOThreadInbox();

    EXPECT_EQ(trySendClusterReadToIOThreads(link), C_ERR);

    EXPECT_EQ(link->io_read_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_refs, 0);
    EXPECT_EQ(link->conn->refs, 1);
    EXPECT_EQ(fc->postpone_state, 0);
    EXPECT_EQ(server.stat_cluster_io_main_thread_fallbacks, 1LL);
}

TEST_F(ClusterIOOffloadTest, WriteDispatchInboxFullUnwindsAndCountsFallback) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    enqueueFakeMsg(link);
    enqueueFakeMsg(link);
    link->head_msg_send_offset = 5;
    testOnlyFillIOThreadInbox();

    EXPECT_EQ(trySendClusterWriteToIOThreads(link), C_ERR);

    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_refs, 0);
    EXPECT_EQ(link->io_last_send_block, (listNode *)NULL);
    EXPECT_EQ(link->io_head_offset, 0u);
    EXPECT_EQ(link->io_nodes_sent, 0);
    /* The caller still owns the queue and its offset. */
    EXPECT_EQ(listLength(link->send_msg_queue), 2UL);
    EXPECT_EQ(link->head_msg_send_offset, 5u);
    EXPECT_EQ(link->conn->refs, 1);
    EXPECT_EQ(fc->postpone_state, 0);
    EXPECT_EQ(server.stat_cluster_io_main_thread_fallbacks, 1LL);
}

TEST_F(ClusterIOOffloadTest, AcceptDispatchInboxFullUnwindsAndCountsFallback) {
    fakeConnection *fc = makeConn(CONN_OWNER_CLUSTER_LINK);
    fc->conn.flags |= CONN_FLAG_ALLOW_ACCEPT_OFFLOAD;
    testOnlyFillIOThreadInbox();

    EXPECT_EQ(trySendClusterAcceptToIOThreads(&fc->conn), C_ERR);

    EXPECT_EQ(fc->conn.flags & CONN_FLAG_ACCEPT_OFFLOAD_PENDING, 0);
    EXPECT_EQ(fc->conn.refs, 1);
    EXPECT_EQ(fc->postpone_state, 0);
    EXPECT_EQ(server.stat_cluster_io_main_thread_fallbacks, 1LL);
}

TEST_F(ClusterIOOffloadTest, PoolInactiveCountsFallback) {
    clusterLink *link = makeLink();
    enqueueFakeMsg(link);
    server.active_io_threads_num = 1;

    /* An established connection with the pool disabled must still report a
     * fallback, i.e. the connecting-state guard did not swallow this path. */
    EXPECT_EQ(trySendClusterWriteToIOThreads(link), C_ERR);
    EXPECT_EQ(trySendClusterReadToIOThreads(link), C_ERR);

    EXPECT_EQ(server.stat_cluster_io_main_thread_fallbacks, 2LL);
    EXPECT_EQ(link->io_refs, 0);
}

TEST_F(ClusterIOOffloadTest, DispatchDeferredWhileJobInFlight) {
    clusterLink *link = makeHalfDuplexLink();
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    ASSERT_EQ(testOnlyGetClusterIOPendingResponses(), 1u);

    /* A second dispatch of either kind must not enqueue anything while a job is
     * in flight, and must not push the caller to a synchronous retry. */
    EXPECT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    EXPECT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    EXPECT_EQ(testOnlyGetClusterIOPendingResponses(), 1u);
    EXPECT_EQ(link->io_refs, 1);
    EXPECT_EQ(server.stat_cluster_io_main_thread_fallbacks, 0LL);

    runInlineWorkerAndDrain(clusterWriteJob, link);
}

/* --- Full duplex links ------------------------------------------------ */

/* A plain link dispatches a read while a write is in flight, but still never
 * runs two jobs of the same kind. */
TEST_F(ClusterIOOffloadTest, FullDuplexDispatchesReadAndWriteConcurrently) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedReadableSocket(fc);
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_PENDING);
    EXPECT_EQ(link->io_read_state, CLUSTER_LINK_IO_PENDING);
    EXPECT_EQ(link->io_refs, 2);
    EXPECT_EQ(link->io_read_deferred, 0);
    EXPECT_EQ(testOnlyGetClusterIOPendingResponses(), 2u);
    EXPECT_EQ(fc->postpone_state, CONN_POSTPONE_READ | CONN_POSTPONE_WRITE);

    EXPECT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    EXPECT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    EXPECT_EQ(testOnlyGetClusterIOPendingResponses(), 2u);
    EXPECT_EQ(link->io_refs, 2);
    EXPECT_EQ(server.stat_cluster_io_main_thread_fallbacks, 0LL);

    /* Both workers finish before either completion is consumed. */
    clusterReadJob(link);
    clusterWriteJob(link);
    processIOThreadsResponses();

    EXPECT_EQ(link->io_read_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_refs, 0);
    EXPECT_EQ(listLength(link->send_msg_queue), 0UL);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
    EXPECT_EQ(fc->postpone_state, 0);
    EXPECT_EQ(fc->update_calls, 1);
}

/* The first completion must not resume connection updates while the other
 * job still uses the connection. */
TEST_F(ClusterIOOffloadTest, FullDuplexKeepsUpdatesPostponedUntilLastCompletion) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedReadableSocket(fc);
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);

    runInlineWorkerAndDrain(clusterWriteJob, link);
    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_read_state, CLUSTER_LINK_IO_PENDING);
    EXPECT_EQ(link->io_refs, 1);
    EXPECT_EQ(fc->postpone_state, CONN_POSTPONE_READ);
    EXPECT_EQ(fc->update_calls, 0);

    runInlineWorkerAndDrain(clusterReadJob, link);
    EXPECT_EQ(link->io_refs, 0);
    EXPECT_EQ(fc->postpone_state, 0);
    EXPECT_EQ(fc->update_calls, 1);
}

/* A read that hits EOF must not clobber the result of a concurrent write, and
 * the teardown it triggers must wait for the write to complete. */
TEST_F(ClusterIOOffloadTest, FullDuplexReadEofDefersFreeUntilWriteCompletes) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedOneCompletePacket(fc);
    fc->eof = 1;
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);

    clusterReadJob(link);
    clusterWriteJob(link);
    EXPECT_EQ(link->io_read_result, CLUSTER_IO_EOF);
    EXPECT_EQ(link->io_write_result, CLUSTER_IO_OK);
    EXPECT_EQ(link->io_nodes_sent, 1);

    processIOThreadsResponses();
    releaseLinkOwnership(link);

    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
    EXPECT_GE(fc->close_calls, 1);
}

/* The jobs of a full duplex link share the connection, so neither may write
 * its state: a hard error is only reported through the job result. */
TEST_F(ClusterIOOffloadTest, FullDuplexJobsLeaveConnectionStateAlone) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedOneCompletePacket(fc);
    fc->fail_read = 1;
    fc->fail_write = 1;
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);

    clusterReadJob(link);
    clusterWriteJob(link);
    EXPECT_EQ(link->io_read_result, CLUSTER_IO_READ_ERROR);
    EXPECT_EQ(link->io_write_result, CLUSTER_IO_WRITE_ERROR);
    EXPECT_EQ(fc->conn.state, CONN_STATE_CONNECTED);

    processIOThreadsResponses();
    releaseLinkOwnership(link);

    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
    EXPECT_GE(fc->close_calls, 1);
}

/* Nor may a job read the state to tell EAGAIN from an error, since the main
 * thread may change it while the job runs. */
TEST_F(ClusterIOOffloadTest, FullDuplexJobsClassifyEagainByErrno) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedReadableSocket(fc);
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);

    fc->conn.state = CONN_STATE_ERROR;
    clusterReadJob(link);
    fc->error = 1; /* The write makes no progress: -1 with errno set to EAGAIN. */
    clusterWriteJob(link);
    fc->error = 0;
    fc->conn.state = CONN_STATE_CONNECTED;

    EXPECT_EQ(link->io_read_result, CLUSTER_IO_OK);
    EXPECT_EQ(link->io_write_result, CLUSTER_IO_OK);
    EXPECT_EQ(link->io_nodes_sent, 0);
    EXPECT_EQ(link->io_head_offset, 0u);
    processIOThreadsResponses();
    EXPECT_EQ(fc->close_calls, 0);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
    EXPECT_EQ(listLength(link->send_msg_queue), 1UL);
    EXPECT_EQ(link->head_msg_send_offset, 0u);
    EXPECT_NE(link->conn->write_handler, (ConnectionCallbackFunc)NULL);
}

static void *runClusterWriteJob(void *link) {
    clusterWriteJob((clusterLink *)link);
    return NULL;
}

/* Both jobs on real threads at once. Under a thread sanitizer build this
 * catches any state the two jobs share. */
TEST_F(ClusterIOOffloadTest, FullDuplexJobsRunOnSeparateThreads) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedThreePacketsAndTail(fc);
    for (int i = 0; i < 8; i++) enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);

    pthread_t writer;
    ASSERT_EQ(pthread_create(&writer, NULL, runClusterWriteJob, link), 0);
    clusterReadJob(link);
    ASSERT_EQ(pthread_join(writer, NULL), 0);
    processIOThreadsResponses();

    EXPECT_EQ(link->io_read_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_refs, 0);
    EXPECT_EQ(listLength(link->send_msg_queue), 0UL);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST], 1LL);
}

/* The same over the real TCP connection type: a socket read and write run at
 * once, EAGAIN on the drained socket is not an error, and EOF still is. */
TEST_F(ClusterIOOffloadTest, FullDuplexJobsOnRealSocket) {
    int peer;
    clusterLink *link = makeSocketLink(&peer);
    ASSERT_NE(link, (clusterLink *)NULL);
    ASSERT_TRUE(clusterLinkIOFullDuplex(link));

    unsigned char *pkt = buildRawPacket(CLUSTERMSG_MIN_LEN);
    for (int i = 0; i < 2; i++) ASSERT_EQ(write(peer, pkt, CLUSTERMSG_MIN_LEN), (ssize_t)CLUSTERMSG_MIN_LEN);
    zfree(pkt);
    const int msgs = 8;
    const uint32_t msg_len = 64;
    for (int i = 0; i < msgs; i++) enqueueFakeMsg(link, msg_len);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    pthread_t writer;
    ASSERT_EQ(pthread_create(&writer, NULL, runClusterWriteJob, link), 0);
    clusterReadJob(link);
    ASSERT_EQ(pthread_join(writer, NULL), 0);
    EXPECT_EQ(link->io_read_result, CLUSTER_IO_OK);
    EXPECT_EQ(link->io_write_result, CLUSTER_IO_OK);
    EXPECT_EQ(link->conn->state, CONN_STATE_CONNECTED);
    processIOThreadsResponses();

    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 2LL);
    EXPECT_EQ(listLength(link->send_msg_queue), 0UL);
    char sink[1024];
    EXPECT_EQ(read(peer, sink, sizeof(sink)), (ssize_t)(msgs * msg_len));

    /* The peer hangs up: the next read job reports EOF and frees the link. */
    close(peer);
    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    clusterReadJob(link);
    EXPECT_EQ(link->io_read_result, CLUSTER_IO_EOF);
    processIOThreadsResponses();
    releaseLinkOwnership(link);
}

/* --- Half duplex links ------------------------------------------------ */

/* Without stateless I/O the jobs fall back to the connection state, as the
 * synchronous path does: a hard error moves it out of CONNECTED. */
TEST_F(ClusterIOOffloadTest, HalfDuplexReadErrorClosesLink) {
    clusterLink *link = makeHalfDuplexLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedOneCompletePacket(fc);
    fc->fail_read = 1;

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    clusterReadJob(link);
    EXPECT_EQ(link->io_read_result, CLUSTER_IO_READ_ERROR);
    processIOThreadsResponses();
    releaseLinkOwnership(link);

    EXPECT_GE(fc->close_calls, 1);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
}

TEST_F(ClusterIOOffloadTest, HalfDuplexWriteEagainKeepsLink) {
    clusterLink *link = makeHalfDuplexLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    enqueueFakeMsg(link);
    fc->error = 1; /* connWrite returns -1 with the state left CONNECTED. */

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterWriteJob, link);

    EXPECT_EQ(fc->close_calls, 0);
    EXPECT_EQ(listLength(link->send_msg_queue), 1UL);
}

TEST_F(ClusterIOOffloadTest, HalfDuplexWriteHardErrorClosesLink) {
    clusterLink *link = makeHalfDuplexLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    enqueueFakeMsg(link);
    fc->fail_write = 1;

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterWriteJob, link);
    releaseLinkOwnership(link);

    EXPECT_GE(fc->close_calls, 1);
}

/* With the read side busy, an inbox-full write rollback must leave the read's
 * postpone in place. */
TEST_F(ClusterIOOffloadTest, FullDuplexWriteRollbackKeepsReadPostponed) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    seedReadableSocket(fc);
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    testOnlyFillIOThreadInbox();

    EXPECT_EQ(trySendClusterWriteToIOThreads(link), C_ERR);
    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_read_state, CLUSTER_LINK_IO_PENDING);
    EXPECT_EQ(link->io_refs, 1);
    EXPECT_EQ(fc->postpone_state, CONN_POSTPONE_READ);

    runInlineWorkerAndDrain(clusterReadJob, link);
    EXPECT_EQ(fc->postpone_state, 0);
}

/* --- Buffer limit and deferred teardown ------------------------------- */

/* 'cluster-link-sendbuf-limit' bounds the send queue only, so a link holding a
 * large receive buffer must survive. */
TEST_F(ClusterIOOffloadTest, BufferLimitIgnoresRcvbuf) {
    clusterLink *link = makeLink();
    link->send_msg_queue_mem = 8;
    link->rcvbuf_len = 4096;
    server.cluster_link_msg_queue_limit_bytes = 64;

    testOnlyFreeClusterLinkOnBufferLimitReached(link);

    EXPECT_EQ(server.cluster->stat_cluster_links_buffer_limit_exceeded, 0ULL);
}

TEST_F(ClusterIOOffloadTest, BufferLimitCountsSendQueue) {
    clusterLink *link = makeLink();
    link->send_msg_queue_mem = 4096;
    server.cluster_link_msg_queue_limit_bytes = 64;

    testOnlyFreeClusterLinkOnBufferLimitReached(link);
    releaseLinkOwnership(link);

    EXPECT_EQ(server.cluster->stat_cluster_links_buffer_limit_exceeded, 1ULL);
}

/* Without the fairness yield, a half duplex link whose send queue never drains
 * re-claims the link on every iteration and inbound packets are never applied. */
TEST_F(ClusterIOOffloadTest, BusySendQueueDoesNotStarveReads) {
    clusterLink *link = makeHalfDuplexLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    fc->buf_size = 8; /* Smaller than the message, so the queue stays backlogged. */
    seedReadableSocket(fc);
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    ASSERT_EQ(link->io_write_state, CLUSTER_LINK_IO_PENDING);

    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    ASSERT_EQ(link->io_read_state, CLUSTER_LINK_IO_IDLE);
    ASSERT_EQ(link->io_read_deferred, 1);

    runInlineWorkerAndDrain(clusterWriteJob, link);
    ASSERT_EQ(listLength(link->send_msg_queue), 1UL);

    /* The write yields one turn, so the read gets the link and applies the packet. */
    EXPECT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_IDLE);
    EXPECT_EQ(link->io_read_deferred, 0);

    EXPECT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    EXPECT_EQ(link->io_read_state, CLUSTER_LINK_IO_PENDING);
    runInlineWorkerAndDrain(clusterReadJob, link);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
}

/* The yield is one-shot: with no read waiting, writes dispatch back to back. */
TEST_F(ClusterIOOffloadTest, WriteDispatchNotYieldedWithoutDeferredRead) {
    clusterLink *link = makeHalfDuplexLink();
    enqueueFakeMsg(link);

    ASSERT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    runInlineWorkerAndDrain(clusterWriteJob, link);

    enqueueFakeMsg(link);
    EXPECT_EQ(trySendClusterWriteToIOThreads(link), C_OK);
    EXPECT_EQ(link->io_write_state, CLUSTER_LINK_IO_PENDING);
    runInlineWorkerAndDrain(clusterWriteJob, link);
}

TEST_F(ClusterIOOffloadTest, FreeClusterLinkDefersWhenIoRefOutstanding) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);

    int freed_now = freeClusterLink(link);

    EXPECT_EQ(freed_now, 0);
    EXPECT_EQ(link->async_close, 1);
    /* The connection must outlive the deferred free, since the worker is still
     * using it. */
    EXPECT_EQ(fc->close_calls, 0);

    /* The pending completion drops the last reference and finalizes the free. */
    runInlineWorkerAndDrain(clusterReadJob, link);
    releaseLinkOwnership(link);
    EXPECT_GE(fc->close_calls, 1);
}

TEST_F(ClusterIOOffloadTest, ReadCompletionFinalizesDeferredFree) {
    clusterLink *link = makeLink();
    fakeConnection *fc = (fakeConnection *)link->conn;
    link->async_close = 1;
    link->io_read_state = CLUSTER_LINK_IO_PENDING;
    link->io_refs = 1;
    link->io_read_result = CLUSTER_IO_OK;

    clusterHandleReadCompletion(link);
    releaseLinkOwnership(link);

    EXPECT_GE(fc->close_calls, 1);
}

/* --- Accept path ------------------------------------------------------ */

TEST_F(ClusterIOOffloadTest, AcceptDispatchRequiresOffloadAllowedFlag) {
    fakeConnection *fc = makeConn(CONN_OWNER_CLUSTER_LINK);
    fc->conn.state = CONN_STATE_ACCEPTING;

    /* Plain TCP cluster accepts never set the flag, so they are not offloaded
     * and are not counted as a fallback either. */
    EXPECT_EQ(trySendClusterAcceptToIOThreads(&fc->conn), C_ERR);
    EXPECT_EQ(fc->conn.flags & CONN_FLAG_ACCEPT_OFFLOAD_PENDING, 0);
    EXPECT_EQ(server.stat_cluster_io_main_thread_fallbacks, 0LL);
}

TEST_F(ClusterIOOffloadTest, AcceptDispatchIsIdempotentWhilePending) {
    fakeConnection *fc = makeAcceptConn();
    fc->conn.state = CONN_STATE_ACCEPTING;
    fc->conn.flags |= CONN_FLAG_ALLOW_ACCEPT_OFFLOAD;

    ASSERT_EQ(trySendClusterAcceptToIOThreads(&fc->conn), C_OK);
    int refs_after_first = fc->conn.refs;
    ASSERT_EQ(testOnlyGetClusterIOPendingResponses(), 1u);

    /* TLS retries re-enter this path; only one job may be in flight. */
    EXPECT_EQ(trySendClusterAcceptToIOThreads(&fc->conn), C_OK);
    EXPECT_EQ(fc->conn.refs, refs_after_first);
    EXPECT_EQ(testOnlyGetClusterIOPendingResponses(), 1u);

    /* Drain through the real path so nothing is left referenced by the queue. */
    clusterAcceptJob(&fc->conn);
    processIOThreadsResponses();
    trackLink((clusterLink *)connGetPrivateData(&fc->conn));
}

TEST_F(ClusterIOOffloadTest, AcceptOffloadRoundTripCreatesLink) {
    fakeConnection *fc = makeAcceptConn();
    fc->conn.flags |= CONN_FLAG_ALLOW_ACCEPT_OFFLOAD;

    ASSERT_EQ(trySendClusterAcceptToIOThreads(&fc->conn), C_OK);
    EXPECT_NE(fc->conn.flags & CONN_FLAG_ACCEPT_OFFLOAD_PENDING, 0);
    EXPECT_EQ(server.stat_cluster_threaded_accepts_processed, 0LL);

    /* State ends CONNECTED, so applying the deferred state runs conn_handler. */
    clusterAcceptJob(&fc->conn);
    processIOThreadsResponses();

    EXPECT_EQ(fc->conn.flags & CONN_FLAG_ACCEPT_OFFLOAD_PENDING, 0);
    EXPECT_EQ(fc->postpone_state, 0);
    ASSERT_NE(connGetPrivateData(&fc->conn), (void *)NULL);
    trackLink((clusterLink *)connGetPrivateData(&fc->conn));
    EXPECT_NE(fc->conn.read_handler, (ConnectionCallbackFunc)NULL);
    EXPECT_EQ(server.stat_cluster_threaded_accepts_processed, 1LL);
}

TEST_F(ClusterIOOffloadTest, AcceptCompletionAcceptingKeepsConnectionOpen) {
    fakeConnection *fc = makeAcceptConn();
    fc->conn.flags |= CONN_FLAG_ACCEPT_OFFLOAD_PENDING;
    fc->conn.state = CONN_STATE_ACCEPTING;

    clusterHandleAcceptCompletion(&fc->conn);

    /* Handshake still pending: conn_handler must not have run. */
    EXPECT_EQ(fc->close_calls, 0);
    EXPECT_EQ(fc->conn.flags & CONN_FLAG_ACCEPT_OFFLOAD_PENDING, 0);
    EXPECT_EQ(connGetPrivateData(&fc->conn), (void *)NULL);
    EXPECT_EQ(fc->conn.conn_handler, clusterConnAcceptHandler);
}

TEST_F(ClusterIOOffloadTest, AcceptCompletionConnectedCreatesLink) {
    fakeConnection *fc = makeAcceptConn();
    fc->conn.flags |= CONN_FLAG_ACCEPT_OFFLOAD_PENDING;

    clusterHandleAcceptCompletion(&fc->conn);

    ASSERT_NE(connGetPrivateData(&fc->conn), (void *)NULL);
    trackLink((clusterLink *)connGetPrivateData(&fc->conn));
    EXPECT_NE(fc->conn.read_handler, (ConnectionCallbackFunc)NULL);
}

TEST_F(ClusterIOOffloadTest, AcceptCompletionAssertsPrivateDataStillNull) {
    fakeConnection *fc = makeAcceptConn();
    fc->conn.flags |= CONN_FLAG_ACCEPT_OFFLOAD_PENDING;
    connSetPrivateData(&fc->conn, (void *)0x1);

    EXPECT_DEATH(clusterHandleAcceptCompletion(&fc->conn), "");
}
