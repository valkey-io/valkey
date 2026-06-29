/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cerrno>
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

typedef struct FakeConn {
    connection conn;
    unsigned char *read_data;
    size_t read_len;
    size_t read_pos;
    unsigned char *written;
    size_t written_cap;
    size_t written_len;
    int close_calls;
    int postpone_state;
    int update_calls;
} FakeConn;

static int fakeConnGetType(void) {
    return CONN_TYPE_SOCKET;
}

static int fakeWrite(connection *conn, const void *data, size_t len) {
    FakeConn *fc = (FakeConn *)conn;
    if (fc->written_len + len > fc->written_cap) {
        len = fc->written_cap - fc->written_len;
    }
    memcpy(fc->written + fc->written_len, data, len);
    fc->written_len += len;
    return (int)len;
}

static int fakeRead(connection *conn, void *buf, size_t len) {
    FakeConn *fc = (FakeConn *)conn;
    if (fc->read_pos >= fc->read_len) {
        errno = EAGAIN;
        return -1;
    }
    size_t avail = fc->read_len - fc->read_pos;
    size_t n = (len < avail) ? len : avail;
    memcpy(buf, fc->read_data + fc->read_pos, n);
    fc->read_pos += n;
    return (int)n;
}

static int fakeSetWriteHandler(connection *conn, ConnectionCallbackFunc handler, int barrier) {
    UNUSED(barrier);
    conn->write_handler = handler;
    return C_OK;
}

static int fakeSetReadHandler(connection *conn, ConnectionCallbackFunc handler) {
    conn->read_handler = handler;
    return C_OK;
}

static void fakePostponeUpdateState(connection *conn, int val) {
    FakeConn *fc = (FakeConn *)conn;
    fc->postpone_state = val;
}

static void fakeUpdateState(connection *conn) {
    FakeConn *fc = (FakeConn *)conn;
    fc->update_calls++;
}

static void fakeClose(connection *conn) {
    FakeConn *fc = (FakeConn *)conn;
    fc->close_calls++;
    conn->state = CONN_STATE_CLOSED;
}

static ConnectionType CT_Fake;

typedef struct TestMsgBlock {
    size_t totlen;
    int refcount;
    unsigned char payload[64];
} TestMsgBlock;

class ClusterIOOffloadTest : public ::testing::Test {
  protected:
    static const int MAX_OWNED = 64;
    FakeConn *owned_conns[MAX_OWNED];
    int owned_conns_count;
    clusterLink *owned_links[MAX_OWNED];
    int owned_links_count;

    static void SetUpTestSuite() {
        memset(&CT_Fake, 0, sizeof(CT_Fake));
        CT_Fake.get_type = fakeConnGetType;
        CT_Fake.close = fakeClose;
        CT_Fake.write = fakeWrite;
        CT_Fake.read = fakeRead;
        CT_Fake.set_write_handler = fakeSetWriteHandler;
        CT_Fake.set_read_handler = fakeSetReadHandler;
        CT_Fake.postpone_update_state = fakePostponeUpdateState;
        CT_Fake.update_state = fakeUpdateState;
    }

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
            if (owned_conns[i]) {
                zfree(owned_conns[i]->read_data);
                zfree(owned_conns[i]->written);
                zfree(owned_conns[i]);
            }
        }
        if (server.cluster) {
            zfree(server.cluster);
            server.cluster = NULL;
        }
        zfree(server.logfile);
        server.logfile = NULL;
        testOnlyFreeIOThreadQueues();
    }

    FakeConn *makeConn(ConnectionOwnerKind owner_kind = CONN_OWNER_CLUSTER_LINK) {
        FakeConn *fc = (FakeConn *)zcalloc(sizeof(FakeConn));
        fc->conn.type = &CT_Fake;
        fc->conn.state = CONN_STATE_CONNECTED;
        fc->conn.fd = -1;
        fc->conn.refs = 1;
        fc->conn.owner_kind = owner_kind;
        fc->written_cap = 4096;
        fc->written = (unsigned char *)zmalloc(fc->written_cap);
        owned_conns[owned_conns_count++] = fc;
        return fc;
    }

    clusterLink *makeLink() {
        clusterLink *link = createClusterLink(NULL);
        FakeConn *fc = makeConn();
        link->conn = &fc->conn;
        connSetPrivateData(link->conn, link);
        owned_links[owned_links_count++] = link;
        return link;
    }

    void releaseLinkOwnership(clusterLink *link) {
        for (int i = 0; i < owned_links_count; i++) {
            if (owned_links[i] == link) {
                owned_links[i] = NULL;
                break;
            }
        }
    }

    void setReadData(FakeConn *fc, const unsigned char *data, size_t len) {
        zfree(fc->read_data);
        fc->read_data = (unsigned char *)zmalloc(len);
        memcpy(fc->read_data, data, len);
        fc->read_len = len;
        fc->read_pos = 0;
    }

    void enqueueFakeMsg(clusterLink *link, uint32_t totlen = 32) {
        TestMsgBlock *msg = (TestMsgBlock *)zcalloc(sizeof(TestMsgBlock));
        msg->totlen = totlen;
        msg->refcount = 1;
        listAddNodeTail(link->send_msg_queue, msg);
        link->send_msg_queue_mem += sizeof(listNode) + totlen;
    }

    void ensureRcvbufCapacity(clusterLink *link, size_t len) {
        if (link->rcvbuf_alloc >= len) return;
        link->rcvbuf = (char *)zrealloc(link->rcvbuf, len);
        link->rcvbuf_alloc = len;
    }

    unsigned char *buildRawPacket(uint32_t totlen) {
        unsigned char *raw = (unsigned char *)zcalloc(totlen);
        clusterMsgHeader *hdr = (clusterMsgHeader *)(void *)raw;
        memcpy(hdr->sig, "RCmb", 4);
        hdr->totlen = htonl(totlen);
        hdr->ver = htons(CLUSTER_PROTO_VER);
        hdr->type = htons(CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK);
        return raw;
    }

    void seedQueuedSnapshot(clusterLink *link) {
        unsigned char *pkt = buildRawPacket(CLUSTERMSG_MIN_LEN);
        ensureRcvbufCapacity(link, CLUSTERMSG_MIN_LEN + 1);
        memcpy(link->rcvbuf, pkt, CLUSTERMSG_MIN_LEN);
        link->rcvbuf[CLUSTERMSG_MIN_LEN] = 'T';
        link->rcvbuf_len = CLUSTERMSG_MIN_LEN + 1;
        link->io_rcvbuf_snapshot_len = CLUSTERMSG_MIN_LEN;
        link->io_rcvbuf_snapshot_packets = 1;
        zfree(pkt);
    }
};

TEST_F(ClusterIOOffloadTest, ReadJobSnapshotsCompletePrefixAndLeavesTail) {
    clusterLink *link = makeLink();
    FakeConn *fc = (FakeConn *)link->conn;

    unsigned char *pkt = buildRawPacket(CLUSTERMSG_MIN_LEN);
    unsigned char *buf = (unsigned char *)zmalloc(CLUSTERMSG_MIN_LEN + 1);
    memcpy(buf, pkt, CLUSTERMSG_MIN_LEN);
    buf[CLUSTERMSG_MIN_LEN] = 'X';
    setReadData(fc, buf, CLUSTERMSG_MIN_LEN + 1);

    link->io_read_state = CLUSTER_LINK_IO_PENDING;
    clusterReadJob(link);

    EXPECT_EQ(link->io_rcvbuf_snapshot_len, (size_t)CLUSTERMSG_MIN_LEN);
    EXPECT_EQ(link->io_rcvbuf_snapshot_packets, 1u);
    EXPECT_EQ(link->rcvbuf_len, (size_t)CLUSTERMSG_MIN_LEN + 1);
    link->io_read_state = CLUSTER_LINK_IO_IDLE;

    zfree(pkt);
    zfree(buf);
}

TEST_F(ClusterIOOffloadTest, ReadCompletionDrainsSnapshotAndCompactsTail) {
    clusterLink *link = makeLink();
    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    seedQueuedSnapshot(link);
    link->io_result = CLUSTER_IO_OK;

    clusterHandleReadCompletion(link);

    EXPECT_EQ(link->io_rcvbuf_snapshot_len, 0u);
    EXPECT_EQ(link->io_rcvbuf_snapshot_packets, 0u);
    EXPECT_EQ(link->rcvbuf_len, 1u);
    EXPECT_EQ(link->rcvbuf[0], 'T');
}

TEST_F(ClusterIOOffloadTest, ReadCompletionOnEofDrainsThenCloses) {
    clusterLink *link = makeLink();
    FakeConn *fc = (FakeConn *)link->conn;
    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    seedQueuedSnapshot(link);
    link->io_result = CLUSTER_IO_EOF;

    clusterHandleReadCompletion(link);
    releaseLinkOwnership(link);

    EXPECT_GE(fc->close_calls, 1);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
}

TEST_F(ClusterIOOffloadTest, ReadCompletionOnReadErrorDrainsThenCloses) {
    clusterLink *link = makeLink();
    FakeConn *fc = (FakeConn *)link->conn;
    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    seedQueuedSnapshot(link);
    link->io_result = CLUSTER_IO_READ_ERROR;

    clusterHandleReadCompletion(link);
    releaseLinkOwnership(link);

    EXPECT_GE(fc->close_calls, 1);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
}

TEST_F(ClusterIOOffloadTest, ReadCompletionOnProtocolErrorDrainsThenCloses) {
    clusterLink *link = makeLink();
    FakeConn *fc = (FakeConn *)link->conn;
    ASSERT_EQ(trySendClusterReadToIOThreads(link), C_OK);
    seedQueuedSnapshot(link);
    link->io_result = CLUSTER_IO_BAD_HEADER;

    clusterHandleReadCompletion(link);
    releaseLinkOwnership(link);

    EXPECT_GE(fc->close_calls, 1);
    EXPECT_EQ(server.cluster->stats_bus_messages_received[CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK], 1LL);
}

TEST_F(ClusterIOOffloadTest, BufferLimitCountsAtomicRcvbufLen) {
    clusterLink *link = makeLink();
    link->send_msg_queue_mem = 8;
    link->rcvbuf_len = 4096;
    server.cluster_link_msg_queue_limit_bytes = 64;

    testOnlyFreeClusterLinkOnBufferLimitReached(link);
    releaseLinkOwnership(link);

    EXPECT_EQ(server.cluster->stat_cluster_links_buffer_limit_exceeded, 1ULL);
}

TEST_F(ClusterIOOffloadTest, FreeClusterLinkDefersWhenIoRefOutstanding) {
    clusterLink *link = makeLink();
    link->io_refs = 1;
    link->io_read_state = CLUSTER_LINK_IO_PENDING;

    int freed_now = freeClusterLink(link);

    EXPECT_EQ(freed_now, 0);
    EXPECT_EQ(link->async_close, 1);

    /* Restore the synthetic in-flight state so fixture teardown can free it. */
    link->io_refs = 0;
    link->io_read_state = CLUSTER_LINK_IO_IDLE;
}

TEST_F(ClusterIOOffloadTest, ReadCompletionFinalizesDeferredFree) {
    clusterLink *link = makeLink();
    FakeConn *fc = (FakeConn *)link->conn;
    link->async_close = 1;
    link->io_read_state = CLUSTER_LINK_IO_PENDING;
    link->io_refs = 1;
    link->io_result = CLUSTER_IO_OK;

    clusterHandleReadCompletion(link);
    releaseLinkOwnership(link);

    EXPECT_GE(fc->close_calls, 1);
}

TEST_F(ClusterIOOffloadTest, WriteDispatchSnapshotsBoundary) {
    clusterLink *link = makeLink();
    enqueueFakeMsg(link);
    enqueueFakeMsg(link);

    int res = trySendClusterWriteToIOThreads(link);

    EXPECT_EQ(res, C_OK);
    EXPECT_NE(link->io_last_send_block, (listNode *)NULL);
    EXPECT_EQ(link->io_head_offset, 0u);

    /* Undo the synthetic dispatch state so fixture teardown can free the link. */
    connSetPostponeUpdateState(link->conn, 0);
    link->conn->refs--;
    link->io_write_state = CLUSTER_LINK_IO_IDLE;
    link->io_refs = 0;
    link->io_last_send_block = NULL;
    link->io_head_offset = 0;
    link->io_nodes_sent = 0;
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

TEST_F(ClusterIOOffloadTest, AcceptDispatchSetsPendingFlag) {
    FakeConn *fc = makeConn(CONN_OWNER_CLIENT);
    fc->conn.flags |= CONN_FLAG_ALLOW_ACCEPT_OFFLOAD;

    int res = trySendClusterAcceptToIOThreads(&fc->conn);

    EXPECT_EQ(res, C_OK);
    EXPECT_NE(fc->conn.flags & CONN_FLAG_ACCEPT_OFFLOAD_PENDING, 0);

    /* Undo synthetic dispatch state so TearDown can free the connection cleanly. */
    connSetPostponeUpdateState(&fc->conn, 0);
    connDecrRefs(&fc->conn);
    fc->conn.flags &= ~CONN_FLAG_ACCEPT_OFFLOAD_PENDING;
}

TEST_F(ClusterIOOffloadTest, AcceptCompletionAcceptingKeepsConnectionOpen) {
    FakeConn *fc = makeConn(CONN_OWNER_CLIENT);
    fc->conn.flags |= CONN_FLAG_ACCEPT_OFFLOAD_PENDING;
    fc->conn.state = CONN_STATE_ACCEPTING;
    fc->conn.refs = 1;

    clusterHandleAcceptCompletion(&fc->conn);

    EXPECT_EQ(fc->close_calls, 0);
    EXPECT_EQ(fc->conn.flags & CONN_FLAG_ACCEPT_OFFLOAD_PENDING, 0);
}

TEST_F(ClusterIOOffloadTest, AcceptCompletionConnectedCreatesLink) {
    FakeConn *fc = makeConn(CONN_OWNER_CLUSTER_LINK);
    fc->conn.flags |= CONN_FLAG_ACCEPT_OFFLOAD_PENDING;
    fc->conn.state = CONN_STATE_CONNECTED;
    fc->conn.refs = 1;

    clusterHandleAcceptCompletion(&fc->conn);

    EXPECT_NE(connGetPrivateData(&fc->conn), (void *)NULL);
    clusterLink *link = (clusterLink *)connGetPrivateData(&fc->conn);
    owned_links[owned_links_count++] = link;
    EXPECT_NE(fc->conn.read_handler, (ConnectionCallbackFunc)NULL);
}

TEST_F(ClusterIOOffloadTest, AcceptCompletionAssertsPrivateDataStillNull) {
    FakeConn *fc = makeConn(CONN_OWNER_CLIENT);
    fc->conn.flags |= CONN_FLAG_ACCEPT_OFFLOAD_PENDING;
    fc->conn.state = CONN_STATE_CONNECTED;
    fc->conn.refs = 1;
    connSetPrivateData(&fc->conn, (void *)0x1);

    EXPECT_DEATH(clusterHandleAcceptCompletion(&fc->conn), "");
}
