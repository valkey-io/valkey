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
}

class ClusterNodeGetPrimaryTest : public ::testing::Test {
  protected:
    static const int NUM_NODES = 4;
    clusterNode nodes[NUM_NODES];

    void SetUp() override {
        server.logfile = zstrdup("");
        server.verbosity = LL_WARNING;
        server.cluster = (clusterState *)zcalloc(sizeof(clusterState));
        server.cluster->nodes = dictCreate(&clusterNodesDictType);
        memset(nodes, 0, sizeof(nodes));
        for (int i = 0; i < NUM_NODES; i++) {
            memset(nodes[i].name, 'a' + i, CLUSTER_NAMELEN);
            dictAdd(server.cluster->nodes, sdsnewlen(nodes[i].name, CLUSTER_NAMELEN), &nodes[i]);
        }
    }

    void TearDown() override {
        dictRelease(server.cluster->nodes);
        zfree(server.cluster);
        server.cluster = NULL;
        zfree(server.logfile);
        server.logfile = NULL;
    }
};

TEST_F(ClusterNodeGetPrimaryTest, PrimaryReturnsItself) {
    EXPECT_EQ(clusterNodeGetPrimary(&nodes[0]), &nodes[0]);
}

/* A replica of a replica resolves to the primary at the top of the chain. */
TEST_F(ClusterNodeGetPrimaryTest, ChainResolvesToTopPrimary) {
    nodes[2].replicaof = &nodes[1];
    nodes[1].replicaof = &nodes[0];
    EXPECT_EQ(clusterNodeGetPrimary(&nodes[2]), &nodes[0]);
}

/* A <-> B: the cycle goes through the starting node. */
TEST_F(ClusterNodeGetPrimaryTest, CycleThroughStartReturnsItself) {
    nodes[0].replicaof = &nodes[1];
    nodes[1].replicaof = &nodes[0];
    EXPECT_EQ(clusterNodeGetPrimary(&nodes[0]), &nodes[0]);
}

/* C -> A <-> B: out-of-order gossip made A and B replicas of each other, and
 * C, a replica of A, isn't part of the cycle. This used to loop forever. */
TEST_F(ClusterNodeGetPrimaryTest, CycleNotThroughStartReturnsItself) {
    nodes[2].replicaof = &nodes[0];
    nodes[0].replicaof = &nodes[1];
    nodes[1].replicaof = &nodes[0];
    EXPECT_EQ(clusterNodeGetPrimary(&nodes[2]), &nodes[2]);
}
