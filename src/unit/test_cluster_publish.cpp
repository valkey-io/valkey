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
#include "cluster_legacy.h"
#include "server.h"

void *clusterCreatePublishMsgBlock(robj *channel, robj *message, int is_light, int is_sharded);
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

static size_t maxPublishPayloadLen(void) {
    return (size_t)UINT32_MAX - offsetof(TestClusterMsgSendBlock, data) - CLUSTERMSG_MIN_LEN -
           offsetof(clusterMsgDataPublish, bulk_data);
}

static robj *createStringObjectWithFakeLength(size_t len) {
    struct sdshdr64 *sh = (struct sdshdr64 *)zcalloc(sizeof(*sh) + 1);
    sh->len = len;
    sh->alloc = 0;
    sh->flags = SDS_TYPE_64;
    sh->buf[0] = '\0';
    return createObject(OBJ_STRING, sh->buf);
}

TEST(ClusterPublishTest, BuildsRepresentableLightMessage) {
    const char channel_value[] = "channel";
    const char message_value[] = "message";
    robj *channel = createStringObject(channel_value, strlen(channel_value));
    robj *message = createStringObject(message_value, strlen(message_value));
    size_t memory_before = server.stat_cluster_links_memory;

    TestClusterMsgSendBlock *msgblock =
        (TestClusterMsgSendBlock *)clusterCreatePublishMsgBlock(channel, message, 1, 0);

    if (msgblock == NULL) {
        decrRefCount(channel);
        decrRefCount(message);
        FAIL() << "failed to build a representable cluster publish message";
    }
    clusterMsgLight *hdr = &msgblock->data[0].msg_light;
    EXPECT_EQ(ntohl(hdr->totlen),
              CLUSTERMSG_LIGHT_MIN_LEN + offsetof(clusterMsgDataPublish, bulk_data) + strlen(channel_value) +
                  strlen(message_value));
    EXPECT_EQ(ntohl(hdr->data.publish.msg.channel_len), strlen(channel_value));
    EXPECT_EQ(ntohl(hdr->data.publish.msg.message_len), strlen(message_value));
    EXPECT_EQ(memcmp(hdr->data.publish.msg.bulk_data, channel_value, strlen(channel_value)), 0);
    EXPECT_EQ(memcmp(hdr->data.publish.msg.bulk_data + strlen(channel_value), message_value, strlen(message_value)), 0);

    server.stat_cluster_links_memory -= msgblock->totlen;
    zfree(msgblock);
    decrRefCount(channel);
    decrRefCount(message);
    EXPECT_EQ(server.stat_cluster_links_memory, memory_before);
}

TEST(ClusterPublishTest, RejectsPayloadThatExceedsClusterBusLimit) {
    size_t max_payload_len = maxPublishPayloadLen();
    robj *channel = createStringObjectWithFakeLength(max_payload_len);
    robj *message = createStringObjectWithFakeLength(1);

    EXPECT_EQ(clusterCreatePublishMsgBlock(channel, message, 1, 0), (void *)NULL);
    EXPECT_EQ(clusterCreatePublishMsgBlock(channel, message, 0, 0), (void *)NULL);

    decrRefCount(channel);
    decrRefCount(message);
}

TEST(ClusterPublishTest, AcceptsPayloadAtClusterBusLimit) {
    size_t max_payload_len = maxPublishPayloadLen();
    robj *channel = createStringObjectWithFakeLength(1);
    robj *message = createStringObjectWithFakeLength(max_payload_len - 1);

    EXPECT_EQ(clusterPublishMessageLengthValid(channel, message), 1);

    decrRefCount(channel);
    decrRefCount(message);
}

TEST(ClusterPublishTest, RejectsOversizedPayloadBeforePublishingLocally) {
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
TEST(ClusterPublishTest, RejectsIndividualLengthThatCannotFitWireField) {
    robj *channel = createStringObjectWithFakeLength((size_t)UINT32_MAX + 1);
    robj *message = createStringObject("", 0);

    EXPECT_EQ(clusterCreatePublishMsgBlock(channel, message, 1, 0), (void *)NULL);

    decrRefCount(channel);
    decrRefCount(message);
}
#endif
