/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
int shouldStartChildReplication(int *mincapa_out, int *req_out, int *rdbver_out);
static monotime fakeGetMonotonicUs(void);
}

static monotime fakeMonotimeUs;

static monotime fakeGetMonotonicUs(void) {
    return fakeMonotimeUs;
}

TEST(ReplicationTest, DisklessSyncDelayAfterWallClockStepBack) {
    monotime (*origGetMonotonicUs)(void) = getMonotonicUs;
    getMonotonicUs = fakeGetMonotonicUs;
    fakeMonotimeUs = 100000000;

    memset(&server, 0, sizeof(server));
    server.unixtime = 1000;
    server.child_pid = -1;
    server.repl_diskless_sync = 1;
    server.repl_diskless_sync_delay = 5;
    server.replicas = listCreate();

    client replica = {};
    ClientReplicationData repl_data = {};
    replica.repl_data = &repl_data;
    replica.last_interaction = server.unixtime;
    repl_data.repl_state = REPLICA_STATE_WAIT_BGSAVE_START;
    repl_data.replica_capa = REPLICA_CAPA_EOF;
    elapsedStart(&repl_data.repl_wait_start);
    listAddNodeTail(server.replicas, &replica);

    /* A wall-clock step must neither extend nor bypass the batching delay. */
    server.unixtime -= 120;
    fakeMonotimeUs += 4000000;
    EXPECT_FALSE(shouldStartChildReplication(NULL, NULL, NULL));
    fakeMonotimeUs += 1000000;
    EXPECT_TRUE(shouldStartChildReplication(NULL, NULL, NULL));

    listRelease(server.replicas);
    server.replicas = NULL;
    getMonotonicUs = origGetMonotonicUs;
}

TEST(ReplicationTest, MinReplicasMaxLagAfterWallClockStepBack) {
    monotime (*origGetMonotonicUs)(void) = getMonotonicUs;
    getMonotonicUs = fakeGetMonotonicUs;
    fakeMonotimeUs = 100000000;

    memset(&server, 0, sizeof(server));
    server.unixtime = 1000;
    server.repl_min_replicas_to_write = 1;
    server.repl_min_replicas_max_lag = 3;
    server.replicas = listCreate();

    robj *argv[3] = {createStringObject("REPLCONF", 8), createStringObject("ACK", 3), createStringObject("0", 1)};
    client replica = {};
    ClientReplicationData repl_data = {};
    replica.repl_data = &repl_data;
    replica.flag.replica = 1;
    replica.argc = 3;
    replica.argv = argv;
    repl_data.repl_state = REPLICA_STATE_ONLINE;
    listAddNodeTail(server.replicas, &replica);

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
    listRelease(server.replicas);
    server.replicas = NULL;
    getMonotonicUs = origGetMonotonicUs;
}
