/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include "cluster.h"
#include "cluster_bus.h"
#include "cluster_link.h"
#include "server.h"

extern clusterBusType clusterLegacyBus;

clusterMsgSendBlock *clusterCreatePublishMsgBlock(robj *channel, robj *message, int is_light, int is_sharded);
size_t testOnlyClusterPublishMaxPayloadLen(void);
void testOnlyClusterLightPublishLayout(size_t *hdr_len,
                                       size_t *totlen_offset,
                                       size_t *channel_len_offset,
                                       size_t *message_len_offset,
                                       size_t *bulk_data_offset);
}

/* The gossip wire format is private to cluster_legacy.c, so fields are read at
 * the offsets it reports. */
static uint32_t readBe32(const unsigned char *msg, size_t offset) {
    uint32_t v;
    memcpy(&v, msg + offset, sizeof(v));
    return ntohl(v);
}

static size_t maxPublishPayloadLen(void) {
    return testOnlyClusterPublishMaxPayloadLen();
}

static robj *createStringObjectWithFakeLength(size_t len) {
    struct sdshdr64 *sh = (struct sdshdr64 *)zcalloc(sizeof(*sh) + 1);
    sh->len = len;
    sh->alloc = 0;
    sh->flags = SDS_TYPE_64;
    sh->buf[0] = '\0';
    return createObject(OBJ_STRING, sh->buf);
}

/* clusterPublishMessageLengthValid() dispatches to the current cluster bus. */
class ClusterPublishTest : public ::testing::Test {
  protected:
    clusterBusType *saved_bus;

    void SetUp() override {
        saved_bus = clusterCurrentBus;
        clusterCurrentBus = &clusterLegacyBus;
    }

    void TearDown() override {
        clusterCurrentBus = saved_bus;
    }
};

TEST_F(ClusterPublishTest, BuildsRepresentableLightMessage) {
    const char channel_value[] = "channel";
    const char message_value[] = "message";
    robj *channel = createStringObject(channel_value, strlen(channel_value));
    robj *message = createStringObject(message_value, strlen(message_value));
    size_t memory_before = server.stat_cluster_links_memory;

    clusterMsgSendBlock *msgblock = clusterCreatePublishMsgBlock(channel, message, 1, 0);

    if (msgblock == NULL) {
        decrRefCount(channel);
        decrRefCount(message);
        FAIL() << "failed to build a representable cluster publish message";
    }
    size_t hdr_len, totlen_off, channel_len_off, message_len_off, bulk_off;
    testOnlyClusterLightPublishLayout(&hdr_len, &totlen_off, &channel_len_off, &message_len_off, &bulk_off);
    const unsigned char *hdr = (const unsigned char *)msgblock->data;
    EXPECT_EQ(readBe32(hdr, totlen_off), hdr_len + strlen(channel_value) + strlen(message_value));
    EXPECT_EQ(readBe32(hdr, channel_len_off), strlen(channel_value));
    EXPECT_EQ(readBe32(hdr, message_len_off), strlen(message_value));
    EXPECT_EQ(memcmp(hdr + bulk_off, channel_value, strlen(channel_value)), 0);
    EXPECT_EQ(memcmp(hdr + bulk_off + strlen(channel_value), message_value, strlen(message_value)), 0);

    server.stat_cluster_links_memory -= msgblock->totlen;
    zfree(msgblock);
    decrRefCount(channel);
    decrRefCount(message);
    EXPECT_EQ(server.stat_cluster_links_memory, memory_before);
}

TEST_F(ClusterPublishTest, RejectsPayloadThatExceedsClusterBusLimit) {
    size_t max_payload_len = maxPublishPayloadLen();
    robj *channel = createStringObjectWithFakeLength(max_payload_len);
    robj *message = createStringObjectWithFakeLength(1);

    EXPECT_EQ(clusterCreatePublishMsgBlock(channel, message, 1, 0), (clusterMsgSendBlock *)NULL);
    EXPECT_EQ(clusterCreatePublishMsgBlock(channel, message, 0, 0), (clusterMsgSendBlock *)NULL);

    decrRefCount(channel);
    decrRefCount(message);
}

TEST_F(ClusterPublishTest, AcceptsPayloadAtClusterBusLimit) {
    size_t max_payload_len = maxPublishPayloadLen();
    robj *channel = createStringObjectWithFakeLength(1);
    robj *message = createStringObjectWithFakeLength(max_payload_len - 1);

    EXPECT_EQ(clusterPublishMessageLengthValid(channel, message), 1);

    decrRefCount(channel);
    decrRefCount(message);
}

TEST_F(ClusterPublishTest, RejectsOversizedPayloadBeforePublishingLocally) {
    size_t max_payload_len = maxPublishPayloadLen();
    robj *channel = createStringObjectWithFakeLength(max_payload_len);
    robj *message = createStringObjectWithFakeLength(1);
    int cluster_enabled = server.cluster_enabled;
    server.cluster_enabled = 1;

    EXPECT_EQ(pubsubPublishMessageAndPropagateToCluster(channel, message, 0), 0);

    server.cluster_enabled = cluster_enabled;
    decrRefCount(channel);
    decrRefCount(message);
}

#if SIZE_MAX > UINT32_MAX
TEST_F(ClusterPublishTest, RejectsIndividualLengthThatCannotFitWireField) {
    robj *channel = createStringObjectWithFakeLength((size_t)UINT32_MAX + 1);
    robj *message = createStringObject("", 0);

    EXPECT_EQ(clusterCreatePublishMsgBlock(channel, message, 1, 0), (clusterMsgSendBlock *)NULL);

    decrRefCount(channel);
    decrRefCount(message);
}
#endif
