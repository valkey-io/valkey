/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include "fake_connection.hpp"

extern "C" {
int shouldStartChildReplication(int *mincapa_out, int *req_out, int *rdbver_out);
void rdbPipeWriteHandler(struct connection *conn);
static monotime fakeGetMonotonicUs(void);
static monotime (*origGetMonotonicUs)(void);
}

static monotime fakeMonotimeUs;

static monotime fakeGetMonotonicUs(void) {
    return fakeMonotimeUs;
}

class ReplicationTest : public ::testing::Test {
  protected:
    client replica;
    ClientReplicationData repl_data;

    static void SetUpTestSuite() {
        origGetMonotonicUs = getMonotonicUs;
        getMonotonicUs = fakeGetMonotonicUs;
    }

    static void TearDownTestSuite() {
        getMonotonicUs = origGetMonotonicUs;
    }

    void SetUp() override {
        fakeMonotimeUs = 100000000;

        memset(&server, 0, sizeof(server));
        server.unixtime = 1000;
        server.child_pid = -1;
        server.verbosity = LL_NOTHING;
        server.replicas = listCreate();

        memset(&replica, 0, sizeof(replica));
        memset(&repl_data, 0, sizeof(repl_data));
        replica.repl_data = &repl_data;
        listAddNodeTail(server.replicas, &replica);
    }

    void TearDown() override {
        listRelease(server.replicas);
        server.replicas = NULL;
    }
};

TEST_F(ReplicationTest, DisklessSyncDelayAfterWallClockStepBack) {
    server.repl_diskless_sync = 1;
    server.repl_diskless_sync_delay = 5;

    replica.last_interaction = server.unixtime;
    repl_data.repl_state = REPLICA_STATE_WAIT_BGSAVE_START;
    repl_data.replica_capa = REPLICA_CAPA_EOF;
    elapsedStart(&repl_data.repl_wait_start);

    /* A wall-clock step must neither extend nor bypass the batching delay. */
    server.unixtime -= 120;
    fakeMonotimeUs += 4000000;
    EXPECT_FALSE(shouldStartChildReplication(NULL, NULL, NULL));
    fakeMonotimeUs += 1000000;
    EXPECT_TRUE(shouldStartChildReplication(NULL, NULL, NULL));
}

TEST_F(ReplicationTest, MinReplicasMaxLagAfterWallClockStepBack) {
    server.repl_min_replicas_to_write = 1;
    server.repl_min_replicas_max_lag = 3;

    robj *argv[3] = {createStringObject("REPLCONF", 8), createStringObject("ACK", 3), createStringObject("0", 1)};
    replica.flag.replica = 1;
    replica.argc = 3;
    replica.argv = argv;
    repl_data.repl_state = REPLICA_STATE_ONLINE;

    replconfCommand(&replica);
    refreshGoodReplicasCount();
    EXPECT_EQ(server.repl_good_replicas_count, 1);

    /* The grace period must not change when wall time precedes the ACK. */
    server.unixtime -= 120;
    fakeMonotimeUs += 3000000;
    refreshGoodReplicasCount();
    EXPECT_EQ(server.repl_good_replicas_count, 1);
    fakeMonotimeUs += 1000000;
    refreshGoodReplicasCount();
    EXPECT_EQ(server.repl_good_replicas_count, 0);

    replconfCommand(&replica);
    refreshGoodReplicasCount();
    EXPECT_EQ(server.repl_good_replicas_count, 1);

    for (int i = 0; i < 3; i++) decrRefCount(argv[i]);
}

TEST_F(ReplicationTest, FullSyncPartialWriteTimeoutAfterWallClockStepBack) {
    MockValkey mock;
    server.repl_timeout = 10;
    server.repl_ping_replica_period = 10;
    server.rdb_write_target = RDB_WRITE_TARGET_SOCKET;
    server.repl_backlog_size = CONFIG_REPL_BACKLOG_MIN_SIZE;
    resetReplicationBuffer();
    server.replicas_waiting_psync = raxNew();
    createReplicationBacklog();

    robj *orig_ping = shared.ping;
    shared.ping = createStringObject("PING", 4);

    fakeConnection *fc = connCreateFake(4);
    fc->conn.state = CONN_STATE_CONNECTED;
    connSetPrivateData(&fc->conn, &replica);

    char pipe_buf[8] = "1234567";
    server.rdb_pipe_buff = pipe_buf;
    server.rdb_pipe_bufflen = sizeof(pipe_buf);

    replica.conn = &fc->conn;
    replica.flag.replica = 1;
    replica.flag.repl_rdbonly = 1;
    repl_data.repl_state = REPLICA_STATE_WAIT_BGSAVE_END;

    rdbPipeWriteHandler(&fc->conn);
    EXPECT_EQ(repl_data.repl_last_partial_write, fakeMonotimeUs);

    /* A backward wall-clock step must not postpone the full-sync timeout. */
    server.unixtime -= 120;
    fakeMonotimeUs += 10000000;
    EXPECT_CALL(mock, freeClient(&replica)).Times(0);
    replicationCron();
    Mock::VerifyAndClearExpectations(&mock);

    fakeMonotimeUs += 1000000;
    EXPECT_CALL(mock, freeClient(&replica)).WillOnce(Return(0));
    replicationCron();
    Mock::VerifyAndClearExpectations(&mock);

    server.rdb_pipe_buff = NULL;
    server.rdb_pipe_bufflen = 0;
    listEmpty(server.replicas);
    freeReplicationBacklog();
    listRelease(server.repl_buffer_blocks);
    server.repl_buffer_blocks = NULL;
    raxFree(server.replicas_waiting_psync);
    server.replicas_waiting_psync = NULL;
    decrRefCount(shared.ping);
    shared.ping = orig_ping;
    connFreeFake(fc);
}
