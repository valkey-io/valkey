/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "cluster.h"
#include "cluster_legacy.h"
#include "server.h"

extern dictType clusterNodesDictType;
clusterNode *createClusterNode(char *nodename, int flags);
void clusterAddNode(clusterNode *node);
void freeClusterNode(clusterNode *n);
void clusterProcessFailoverAuthAck(clusterNode *sender);
void clusterProcessFailoverAuthNack(clusterNode *sender, clusterMsg *request);
}

/* Election-time accounting of FAILOVER_AUTH_ACK / FAILOVER_AUTH_NACK replies
 * on the replica running for election. Each test stands up a cluster view of
 * NUM_VOTERS voting primaries (quorum = NUM_VOTERS / 2 + 1) with an election
 * in flight, then feeds replies to the handlers in a chosen order and checks
 * whether the fast-fail bound resets the election (failover_auth_time == 0)
 * or lets it keep running. */
class ClusterFailoverAuthTest : public ::testing::Test {
  protected:
    static const int NUM_VOTERS = 5;
    static const mstime_t ELECTION_TIME = 123456;
    static const uint64_t ELECTION_EPOCH = 7;
    clusterNode *voters[NUM_VOTERS];
    clusterMsg nack_msg;

    void SetUp() override {
        memset(&server, 0, sizeof(server));
        server.logfile = zstrdup("");
        server.verbosity = LL_WARNING;
        server.cluster = (clusterState *)zcalloc(sizeof(clusterState));
        server.cluster->nodes = dictCreate(&clusterNodesDictType);

        for (int i = 0; i < NUM_VOTERS; i++) {
            voters[i] = createClusterNode(NULL, CLUSTER_NODE_PRIMARY);
            voters[i]->numslots = 1; /* Owning slots is what grants the vote. */
            clusterAddNode(voters[i]);
        }
        server.cluster->size = NUM_VOTERS;
        server.cluster->size_fail = 0;

        /* An election is in flight and the AUTH_REQUEST has been broadcast. */
        server.cluster->currentEpoch = ELECTION_EPOCH;
        server.cluster->failover_auth_epoch = ELECTION_EPOCH;
        server.cluster->failover_auth_time = ELECTION_TIME;
        server.cluster->failover_auth_sent = 1;
        server.cluster->failover_auth_count = 0;
        server.cluster->failover_auth_nack_count = 0;

        memset(&nack_msg, 0, sizeof(nack_msg));
        nack_msg.data.failover_nack.nack.reason = CLUSTERMSG_FAILOVER_AUTH_NACK_REASON_PRIMARY_UP;
    }

    void TearDown() override {
        for (int i = 0; i < NUM_VOTERS; i++) {
            if (voters[i]) freeClusterNode(voters[i]);
        }
        dictRelease(server.cluster->nodes);
        zfree(server.cluster);
        server.cluster = NULL;
        zfree(server.logfile);
        server.logfile = NULL;
    }

    /* What clusterUpdateState() does once a voting primary is marked FAIL. */
    void markVoterFailed(clusterNode *voter) {
        voter->flags |= CLUSTER_NODE_FAIL;
        server.cluster->size_fail++;
    }

    int quorum() {
        return (NUM_VOTERS / 2) + 1;
    }

    bool electionReset() {
        return server.cluster->failover_auth_time == 0;
    }
};

/* Regression test for valkey-io/valkey#4626.
 *
 * Five voters, quorum three. V1 NACKs while reachable, then is marked FAIL
 * during the same election, then V2 NACKs. V3, V4 and V5 have neither failed
 * nor NACKed, so three votes are still achievable and the election must keep
 * running. The scalar bound size - size_fail - nack_count subtracts V1 twice
 * (once as a NACK, once as a FAIL voter), evaluates to 2 < 3 and resets a
 * winnable election. */
TEST_F(ClusterFailoverAuthTest, NackThenFailVoterIsNotDoubleSubtracted) {
    ASSERT_EQ(quorum(), 3);

    /* V1 NACKs while still reachable. Bound is 4 >= 3: keep running. */
    clusterProcessFailoverAuthNack(voters[0], &nack_msg);
    EXPECT_FALSE(electionReset());

    /* V1 is marked FAIL later in the same election. */
    markVoterFailed(voters[0]);

    /* V2 NACKs. True bound: V3, V4, V5 can still ACK = 3 >= quorum. */
    clusterProcessFailoverAuthNack(voters[1], &nack_msg);
    EXPECT_FALSE(electionReset()) << "election reset although V3, V4 and V5 can still vote";
}

/* The bound must still fast-fail when the election really is lost: with V1
 * NACKed-then-FAILed and both V2 and V3 NACKing, only V4 and V5 remain, which
 * is below the quorum of three. */
TEST_F(ClusterFailoverAuthTest, NackThenFailVoterStillFastFailsWhenTrulyLost) {
    clusterProcessFailoverAuthNack(voters[0], &nack_msg);
    markVoterFailed(voters[0]);
    clusterProcessFailoverAuthNack(voters[1], &nack_msg);
    EXPECT_FALSE(electionReset());

    clusterProcessFailoverAuthNack(voters[2], &nack_msg);
    EXPECT_TRUE(electionReset()) << "only V4 and V5 remain, quorum of 3 is unreachable";
}

/* A NACK from a voter that is already FAIL must not lower the bound: that
 * voter is already excluded by virtue of having failed. */
TEST_F(ClusterFailoverAuthTest, NackFromAlreadyFailedVoterIsIgnored) {
    markVoterFailed(voters[0]);
    clusterProcessFailoverAuthNack(voters[0], &nack_msg);
    EXPECT_EQ(server.cluster->failover_auth_nack_count, 0);
    EXPECT_FALSE(electionReset());

    /* V2 NACKs: V3, V4, V5 remain = 3 >= quorum. */
    clusterProcessFailoverAuthNack(voters[1], &nack_msg);
    EXPECT_FALSE(electionReset());
}

/* A repeated NACK from the same voter in the same election is one rejection,
 * not two. Two NACKs from V1 plus one from V2 must leave V3, V4, V5 as three
 * achievable votes and keep the election running. */
TEST_F(ClusterFailoverAuthTest, DuplicateNackFromSameVoterCountsOnce) {
    clusterProcessFailoverAuthNack(voters[0], &nack_msg);
    clusterProcessFailoverAuthNack(voters[0], &nack_msg);
    EXPECT_EQ(server.cluster->failover_auth_nack_count, 1);

    clusterProcessFailoverAuthNack(voters[1], &nack_msg);
    EXPECT_FALSE(electionReset()) << "V1's duplicate NACK was counted as a second rejection";
}

/* The symmetric ordering: a voter that ACKs and is then marked FAIL has cast
 * a vote that remains valid. With V1's ACK in hand and V2, V3 NACKing, the
 * achievable total is 1 (V1) + 2 (V4, V5) = 3, which meets the quorum, so the
 * election must keep running. The old bound dropped V1 through size_fail. */
TEST_F(ClusterFailoverAuthTest, AckThenFailVoterKeepsItsVote) {
    clusterProcessFailoverAuthAck(voters[0]);
    EXPECT_EQ(server.cluster->failover_auth_count, 1);
    markVoterFailed(voters[0]);

    clusterProcessFailoverAuthNack(voters[1], &nack_msg);
    clusterProcessFailoverAuthNack(voters[2], &nack_msg);
    EXPECT_FALSE(electionReset()) << "V1's received ACK plus V4 and V5 still reach quorum";

    /* One more NACK and the election really is lost: 1 + 1 < 3. */
    clusterProcessFailoverAuthNack(voters[3], &nack_msg);
    EXPECT_TRUE(electionReset());
}

/* A repeated ACK from the same voter in the same election is one vote. */
TEST_F(ClusterFailoverAuthTest, DuplicateAckFromSameVoterCountsOnce) {
    clusterProcessFailoverAuthAck(voters[0]);
    clusterProcessFailoverAuthAck(voters[0]);
    EXPECT_EQ(server.cluster->failover_auth_count, 1);
}

/* Stamps are election-local: responses recorded in an earlier election must
 * not carry over once a new election epoch starts. */
TEST_F(ClusterFailoverAuthTest, ResponsesDoNotCarryOverToNextElection) {
    clusterProcessFailoverAuthNack(voters[0], &nack_msg);
    clusterProcessFailoverAuthNack(voters[1], &nack_msg);
    clusterProcessFailoverAuthAck(voters[2]);
    EXPECT_EQ(server.cluster->failover_auth_nack_count, 2);
    EXPECT_EQ(server.cluster->failover_auth_count, 1);

    /* New election, as set up by clusterHandleReplicaFailover(). */
    server.cluster->failover_auth_epoch = ELECTION_EPOCH + 1;
    server.cluster->failover_auth_count = 0;
    server.cluster->failover_auth_nack_count = 0;
    server.cluster->failover_auth_time = ELECTION_TIME;

    /* Every voter can respond again and is counted afresh. */
    clusterProcessFailoverAuthNack(voters[0], &nack_msg);
    EXPECT_EQ(server.cluster->failover_auth_nack_count, 1);
    clusterProcessFailoverAuthAck(voters[2]);
    EXPECT_EQ(server.cluster->failover_auth_count, 1);
    EXPECT_FALSE(electionReset());
}
