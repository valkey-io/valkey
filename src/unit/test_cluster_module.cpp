/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstddef>
#include <cstdint>

extern "C" {
#include "cluster.h"
#include "cluster_legacy.h"
#include "server.h"
}

/* Mirrors clusterMsgSendBlock, which is private to cluster_legacy.c. */
typedef struct TestClusterMsgSendBlock {
    size_t totlen;
    int refcount;
    union {
        clusterMsg msg;
        clusterMsgLight msg_light;
    } data[1];
} TestClusterMsgSendBlock;

static uint32_t maxModulePayloadLen(void) {
    size_t overhead = offsetof(TestClusterMsgSendBlock, data) + CLUSTERMSG_MIN_LEN +
                      offsetof(clusterMsgModule, bulk_data);
    return (uint32_t)((size_t)UINT32_MAX - overhead);
}

TEST(ClusterModuleTest, ValidatesFullAllocationLength) {
    uint32_t max_payload_len = maxModulePayloadLen();

    EXPECT_EQ(clusterModuleMessageLengthValid(0), 1);
    EXPECT_EQ(clusterModuleMessageLengthValid(3), 1);
    EXPECT_EQ(clusterModuleMessageLengthValid(max_payload_len), 1);
    EXPECT_EQ(clusterModuleMessageLengthValid(max_payload_len + 1), 0);
    EXPECT_EQ(clusterModuleMessageLengthValid(UINT32_MAX), 0);
}

TEST(ClusterModuleTest, NormalHeaderLimitAlsoProtectsLightHeaders) {
    size_t normal_overhead = offsetof(TestClusterMsgSendBlock, data) + CLUSTERMSG_MIN_LEN +
                             offsetof(clusterMsgModule, bulk_data);
    size_t light_overhead = offsetof(TestClusterMsgSendBlock, data) + CLUSTERMSG_LIGHT_MIN_LEN +
                            offsetof(clusterMsgModule, bulk_data);
    uint32_t max_payload_len = maxModulePayloadLen();

    EXPECT_GT(normal_overhead, light_overhead);
    EXPECT_EQ((size_t)max_payload_len + normal_overhead, (size_t)UINT32_MAX);
    EXPECT_LE((size_t)max_payload_len + light_overhead, (size_t)UINT32_MAX);
}
