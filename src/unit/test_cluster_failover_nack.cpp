/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include "fake_connection.hpp"

#include <cstdint>
#include <cstring>

extern "C" {
#include "cluster.h"
#include "cluster_legacy.h"
#include "server.h"

clusterLink *createClusterLink(clusterNode *node);
int freeClusterLink(clusterLink *link);
clusterNode *createClusterNode(char *nodename, int flags);
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request);
int clusterProcessPacket(clusterLink *link);

extern clusterNode *myself;
}

/* A captured cluster-bus packet. A NACK is always smaller than a full
 * clusterMsg, so a fixed buffer is enough. len == 0 means nothing was captured. */
typedef struct TestPacket {
    unsigned char bytes[sizeof(clusterMsg)];
    size_t len;
} TestPacket;

/* Mirrors clusterMsgSendBlock, which is private to cluster_legacy.c. */
typedef struct TestMsgBlock {
    size_t totlen;
    int refcount;
    union {
        clusterMsg msg;
        clusterMsgLight msg_light;
    } data[1];
} TestMsgBlock;

/* FAILOVER_AUTH_NACK accounting on a candidate replica.
 *
 * One process plays both roles of the exchange, one after the other:
 *  1. As the VOTER, it runs clusterSendFailoverAuthIfNeeded() against a
 *     FAILOVER_AUTH_REQUEST and captures the NACK it queues on the wire.
 *  2. As the CANDIDATE, it feeds those exact bytes through
 *     clusterProcessPacket() while an election is in progress and inspects
 *     the NACK tally.
 * Capturing the real wire bytes (rather than hand-building a NACK) keeps
 * the test honest about what the sender actually encodes. */
class ClusterFailoverNackTest : public ::testing::Test {
  protected:
    clusterNode *voter;
    clusterNode *candidate;

    void SetUp() override {
        memset(&server, 0, sizeof(server));
        server.logfile = zstrdup("");
        server.verbosity = LL_NOTHING;
        server.cluster_drop_packet_filter = -1;
        server.cluster = (clusterState *)zcalloc(sizeof(clusterState));
        server.cluster->state = CLUSTER_OK;
        server.cluster->safe_to_join = 1;

        /* A voting primary: PRIMARY flag and at least one slot. */
        voter = createClusterNode(NULL, CLUSTER_NODE_PRIMARY);
        voter->numslots = 1;
        voter->slots[0] = 1;

        /* The replica running the election. It advertises NACK support so the
         * voter is willing to send it a NACK at all. */
        candidate = createClusterNode(NULL, CLUSTER_NODE_REPLICA | CLUSTER_NODE_FAILOVER_AUTH_NACK_SUPPORTED);
    }

    void TearDown() override {
        freeNode(voter);
        freeNode(candidate);
        myself = NULL;
        zfree(server.cluster);
        server.cluster = NULL;
        zfree(server.logfile);
        server.logfile = NULL;
    }

    /* Nodes are never added to server.cluster->nodes, so freeClusterNode()
     * (which asserts on dictDelete) cannot be used. Release what
     * createClusterNode() allocated. */
    static void freeNode(clusterNode *n) {
        if (n->link) freeClusterLink(n->link);
        if (n->inbound_link) freeClusterLink(n->inbound_link);
        sdsfree(n->hostname);
        sdsfree(n->human_nodename);
        sdsfree(n->availability_zone);
        sdsfree(n->announce_client_ipv4);
        sdsfree(n->announce_client_ipv6);
        raxFree(n->fail_reports);
        zfree(n->replicas);
        zfree(n);
    }

    static fakeConnection *makeConn() {
        fakeConnection *fc = connCreateFake(4096);
        fc->conn.state = CONN_STATE_CONNECTED;
        fc->conn.refs = 1;
        fc->conn.owner_kind = CONN_OWNER_CLUSTER_LINK;
        return fc;
    }

    /* Act as the voter: at currentEpoch voter_epoch, process a
     * FAILOVER_AUTH_REQUEST for request_epoch from the candidate and store the
     * wire bytes of the NACK it sends in *out (out->len stays 0 when it sends
     * nothing or something other than a NACK). */
    void voterRespondsTo(uint64_t voter_epoch, uint64_t request_epoch, TestPacket *out, uint8_t *reason_out) {
        out->len = 0;

        myself = voter;
        server.cluster->myself = voter;
        server.cluster->currentEpoch = voter_epoch;

        /* The voter's outbound link to the candidate, where the NACK is queued. */
        clusterLink *link = createClusterLink(candidate);
        fakeConnection *fc = makeConn();
        link->conn = &fc->conn;
        connSetPrivateData(link->conn, link);

        clusterMsg request;
        memset(&request, 0, sizeof(request));
        memcpy(request.sig, "RCmb", 4);
        request.ver = htons(CLUSTER_PROTO_VER);
        request.type = htons(CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST);
        request.totlen = htonl(sizeof(clusterMsg) - sizeof(union clusterMsgData));
        request.currentEpoch = htonu64(request_epoch);
        request.configEpoch = htonu64(0);
        memcpy(request.sender, candidate->name, CLUSTER_NAMELEN);

        clusterSendFailoverAuthIfNeeded(candidate, &request);

        if (listLength(link->send_msg_queue) == 1) {
            TestMsgBlock *blk = (TestMsgBlock *)listNodeValue(listFirst(link->send_msg_queue));
            clusterMsg *msg = &blk->data[0].msg;
            if ((ntohs(msg->type) & ~CLUSTERMSG_MODIFIER_MASK) == CLUSTERMSG_TYPE_FAILOVER_AUTH_NACK) {
                uint32_t len = ntohl(msg->totlen);
                ASSERT_LE(len, sizeof(out->bytes));
                memcpy(out->bytes, msg, len);
                out->len = len;
                if (reason_out) *reason_out = msg->data.failover_nack.nack.reason;
            }
        }

        freeClusterLink(link); /* Also clears candidate->link. */
        connFreeFake(fc);
    }

    /* Act as the candidate: with an election for election_epoch in flight,
     * deliver the given packet from the voter on the voter's inbound link and
     * run it through clusterProcessPacket(). Cluster shape: 3 primaries, one
     * of them (the candidate's own) FAIL, so a single counted NACK is enough
     * to trip the achievable-vote bound and reset the election. */
    void candidateReceives(uint64_t election_epoch, const TestPacket *packet) {
        myself = candidate;
        server.cluster->myself = candidate;
        server.cluster->currentEpoch = election_epoch;
        server.cluster->failover_auth_epoch = election_epoch;
        server.cluster->failover_auth_time = mstime();
        server.cluster->failover_auth_sent = 1;
        server.cluster->failover_auth_count = 0;
        server.cluster->failover_auth_nack_count = 0;
        server.cluster->size = 3;
        server.cluster->size_fail = 1;

        clusterLink *link = createClusterLink(NULL);
        link->node = voter;
        voter->inbound_link = link;
        fakeConnection *fc = makeConn();
        link->conn = &fc->conn;
        connSetPrivateData(link->conn, link);

        /* Hand the link a fully-read packet. */
        server.stat_cluster_links_memory -= link->rcvbuf_alloc;
        zfree(link->rcvbuf);
        link->rcvbuf_alloc = packet->len;
        link->rcvbuf = (char *)zmalloc(link->rcvbuf_alloc);
        server.stat_cluster_links_memory += link->rcvbuf_alloc;
        memcpy(link->rcvbuf, packet->bytes, packet->len);
        link->rcvbuf_len = packet->len;

        EXPECT_EQ(clusterProcessPacket(link), 1);

        freeClusterLink(link); /* Also clears voter->inbound_link. */
        connFreeFake(fc);
    }
};

/* Control: a NACK rejecting the request of the election in progress is
 * counted, and (given the cluster shape above) fast-fails the election. */
TEST_F(ClusterFailoverNackTest, NackForCurrentElectionIsCounted) {
    const uint64_t E = 10;
    uint8_t reason = 0;

    /* Voter is already at E+1 when the epoch-E request arrives: REQ_EPOCH_OLD. */
    TestPacket nack;
    voterRespondsTo(E + 1, E, &nack, &reason);
    ASSERT_NE(nack.len, 0u) << "voter did not send a FAILOVER_AUTH_NACK";
    EXPECT_EQ(reason, CLUSTERMSG_FAILOVER_AUTH_NACK_REASON_REQ_EPOCH_OLD);

    /* The candidate is still running the election that request belonged to. */
    candidateReceives(E, &nack);
    EXPECT_EQ(server.cluster->failover_auth_nack_count, 1);
    EXPECT_EQ(server.cluster->failover_auth_time, 0) << "one NACK must fast-fail this 3-primary/1-FAIL election";
}

/* Regression for valkey-io/valkey#4627.
 *
 * A voter whose currentEpoch has already advanced to E+1 rejects the
 * candidate's stale epoch-E request with REQ_EPOCH_OLD. That NACK claims
 * epoch E+1 in its cluster-bus header (the voter's state), even though the
 * request it rejects is from epoch E. If the candidate has meanwhile moved on
 * to election E+1, that NACK must not be counted against E+1: the voter has
 * not rejected the E+1 request yet, and may well grant it. Counting it makes
 * failover_auth_nack_count over-count, shrinks the achievable-vote bound and
 * spuriously resets a winnable election. */
TEST_F(ClusterFailoverNackTest, StaleNackFromOlderElectionIsNotCountedAgainstNewerElection) {
    const uint64_t E = 10;
    uint8_t reason = 0;

    /* Voter at E+1 rejects the delayed epoch-E request. */
    TestPacket stale_nack;
    voterRespondsTo(E + 1, E, &stale_nack, &reason);
    ASSERT_NE(stale_nack.len, 0u) << "voter did not send a FAILOVER_AUTH_NACK";
    EXPECT_EQ(reason, CLUSTERMSG_FAILOVER_AUTH_NACK_REASON_REQ_EPOCH_OLD);

    /* Candidate has already started election E+1 when the stale NACK lands. */
    candidateReceives(E + 1, &stale_nack);
    EXPECT_EQ(server.cluster->failover_auth_nack_count, 0) << "NACK for epoch E was counted against election E+1";
    EXPECT_NE(server.cluster->failover_auth_time, 0) << "election E+1 was reset by a NACK that did not reject it";
}

/* The NACK payload must identify the rejected request, not the voter: a voter
 * at E+1 rejecting an epoch-E request echoes E, while its header still claims
 * its own currentEpoch E+1. */
TEST_F(ClusterFailoverNackTest, NackEchoesRejectedRequestEpoch) {
    const uint64_t E = 10;

    TestPacket nack;
    voterRespondsTo(E + 1, E, &nack, NULL);
    ASSERT_GE(nack.len, sizeof(clusterMsg) - sizeof(union clusterMsgData) + sizeof(clusterMsgDataFailoverNack));

    clusterMsg *msg = (clusterMsg *)(void *)nack.bytes;
    EXPECT_EQ(ntohu64(msg->currentEpoch), E + 1) << "header must still carry the voter's currentEpoch";
    EXPECT_EQ(ntohu64(msg->data.failover_nack.nack.epoch), E) << "payload must echo the rejected request epoch";
    EXPECT_EQ(msg->data.failover_nack.nack.reason, CLUSTERMSG_FAILOVER_AUTH_NACK_REASON_REQ_EPOCH_OLD);
}
