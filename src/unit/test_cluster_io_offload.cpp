/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include "fake_connection.hpp"

#include <cstring>

extern "C" {
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

    clusterLink *makeLink() {
        clusterLink *link = createClusterLink(NULL);
        fakeConnection *fc = makeConn();
        link->conn = &fc->conn;
        connSetPrivateData(link->conn, link);
        owned_links[owned_links_count++] = link;
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

    /* Queue one message of msg_len wire bytes. */
    void enqueueFakeMsg(clusterLink *link, uint32_t msg_len = 64) {
        TestMsgBlock *blk = (TestMsgBlock *)zcalloc(sizeof(TestMsgBlock));
        blk->refcount = 1;
        blk->totlen = sizeof(TestMsgBlock);
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

TEST_F(ClusterIOOffloadTest, WriteCompletionPopsOnlyVisibleNodes) {
    clusterLink *link = makeLink();
    enqueueFakeMsg(link);
    enqueueFakeMsg(link);

    link->io_write_state = CLUSTER_LINK_IO_PENDING;
    link->io_refs = 1;
    link->io_nodes_sent = 1;
    link->io_head_offset = 0;
    link->io_result = CLUSTER_IO_OK;

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
    link->io_result = CLUSTER_IO_OK;

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
    clusterLink *link = makeLink();
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

/* --- Buffer limit and deferred teardown ------------------------------- */

/* 'cluster-link-sendbuf-limit' bounds the send queue only. A link holding a
 * large receive buffer is not a slow peer and must survive, otherwise an
 * offloaded read that got ahead of the main thread would tear down a healthy
 * inbound link. */
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
    link->io_result = CLUSTER_IO_OK;

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
    fakeConnection *fc = makeConn(CONN_OWNER_CLUSTER_LINK);
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
    fakeConnection *fc = makeConn(CONN_OWNER_CLUSTER_LINK);
    fc->conn.flags |= CONN_FLAG_ALLOW_ACCEPT_OFFLOAD;

    ASSERT_EQ(trySendClusterAcceptToIOThreads(&fc->conn), C_OK);
    EXPECT_NE(fc->conn.flags & CONN_FLAG_ACCEPT_OFFLOAD_PENDING, 0);
    EXPECT_EQ(server.stat_cluster_threaded_accepts_processed, 0LL);

    /* The fake connection has no accept callback, so the state stays CONNECTED
     * and the completion handler creates the link. */
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
    fakeConnection *fc = makeConn(CONN_OWNER_CLUSTER_LINK);
    fc->conn.flags |= CONN_FLAG_ACCEPT_OFFLOAD_PENDING;
    fc->conn.state = CONN_STATE_ACCEPTING;

    clusterHandleAcceptCompletion(&fc->conn);

    /* Handshake still in progress: no link yet, connection kept open. */
    EXPECT_EQ(fc->close_calls, 0);
    EXPECT_EQ(fc->conn.flags & CONN_FLAG_ACCEPT_OFFLOAD_PENDING, 0);
    EXPECT_EQ(connGetPrivateData(&fc->conn), (void *)NULL);
}

TEST_F(ClusterIOOffloadTest, AcceptCompletionConnectedCreatesLink) {
    fakeConnection *fc = makeConn(CONN_OWNER_CLUSTER_LINK);
    fc->conn.flags |= CONN_FLAG_ACCEPT_OFFLOAD_PENDING;

    clusterHandleAcceptCompletion(&fc->conn);

    ASSERT_NE(connGetPrivateData(&fc->conn), (void *)NULL);
    trackLink((clusterLink *)connGetPrivateData(&fc->conn));
    EXPECT_NE(fc->conn.read_handler, (ConnectionCallbackFunc)NULL);
}

TEST_F(ClusterIOOffloadTest, AcceptCompletionAssertsPrivateDataStillNull) {
    fakeConnection *fc = makeConn(CONN_OWNER_CLUSTER_LINK);
    fc->conn.flags |= CONN_FLAG_ACCEPT_OFFLOAD_PENDING;
    connSetPrivateData(&fc->conn, (void *)0x1);

    EXPECT_DEATH(clusterHandleAcceptCompletion(&fc->conn), "");
}
