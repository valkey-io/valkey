/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "server.h"
}

class TrackingTest : public ::testing::Test {
  protected:
    void SetUp() override {
        memset(&server, 0, sizeof(valkeyServer));
        server.errors = raxNew();
        server.clients_pending_write = listCreate();
    }

    void TearDown() override {
        listRelease(server.clients_pending_write);
        raxFree(server.errors);
    }

    /* Create a minimal client suitable for checkPrefixCollisionsOrReply.
     * The client needs a reply buffer (for addReplyErrorFormat) and
     * initialized pubsub_data (for client_tracking_prefixes). */
    static client *createTrackingTestClient(void) {
        client *c = (client *)(zcalloc(sizeof(client)));
        c->buf = (char *)zmalloc_usable(PROTO_REPLY_CHUNK_BYTES, &c->buf_usable_size);
        c->reply = listCreate();
        listSetFreeMethod(c->reply, freeClientReplyValue);
        listSetDupMethod(c->reply, dupClientReplyValue);
        c->conn = (connection *)c; /* dummy, avoids NULL dereference */
        c->deferred_reply_bytes = ULLONG_MAX;
        listInitNode(&c->clients_pending_write_node, c);
        initClientPubSubData(c);
        return c;
    }

    static void freeTrackingTestClient(client *c) {
        /* Remove from pending-write list to avoid dangling pointers;
         * addReplyErrorFormat may have added the client there. */
        if (c->flag.pending_write) {
            listUnlinkNode(server.clients_pending_write, &c->clients_pending_write_node);
            c->flag.pending_write = 0;
        }
        if (c->pubsub_data) {
            if (c->pubsub_data->client_tracking_prefixes) {
                raxFree(c->pubsub_data->client_tracking_prefixes);
                c->pubsub_data->client_tracking_prefixes = NULL;
            }
            hashtableRelease(c->pubsub_data->pubsub_channels);
            hashtableRelease(c->pubsub_data->pubsub_patterns);
            hashtableRelease(c->pubsub_data->pubsubshard_channels);
            zfree(c->pubsub_data);
        }
        listRelease(c->reply);
        zfree(c->buf);
        zfree(c);
    }

    /* Helper to build an robj* array of string prefixes from C strings. */
    static robj **createPrefixArray(const char **strs, size_t count) {
        robj **arr = (robj **)zmalloc(sizeof(robj *) * count);
        for (size_t i = 0; i < count; i++) {
            arr[i] = createStringObject(strs[i], strlen(strs[i]));
        }
        return arr;
    }

    static void freePrefixArray(robj **arr, size_t count) {
        for (size_t i = 0; i < count; i++) {
            decrRefCount(arr[i]);
        }
        zfree(arr);
    }

    /* Add a prefix to the client's existing tracking prefixes rax. */
    static void addExistingPrefix(client *c, const char *prefix) {
        if (c->pubsub_data->client_tracking_prefixes == NULL) {
            c->pubsub_data->client_tracking_prefixes = raxNew();
        }
        raxInsert(c->pubsub_data->client_tracking_prefixes,
                  (unsigned char *)prefix, strlen(prefix),
                  NULL, NULL);
    }
};

/* No prefixes at all — should succeed. */
TEST_F(TrackingTest, NoPrefixes) {
    client *c = createTrackingTestClient();
    int result = checkPrefixCollisionsOrReply(c, NULL, 0);
    EXPECT_EQ(result, 1);
    EXPECT_EQ(c->bufpos, 0u); /* No error reply written. */
    freeTrackingTestClient(c);
}

/* Single prefix with no existing prefixes — should succeed. */
TEST_F(TrackingTest, SinglePrefixNoExisting) {
    client *c = createTrackingTestClient();
    const char *strs[] = {"foo"};
    robj **prefixes = createPrefixArray(strs, 1);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 1);
    EXPECT_EQ(result, 1);
    EXPECT_EQ(c->bufpos, 0u);

    freePrefixArray(prefixes, 1);
    freeTrackingTestClient(c);
}

/* Multiple non-overlapping prefixes — should succeed. */
TEST_F(TrackingTest, MultipleNonOverlappingPrefixes) {
    client *c = createTrackingTestClient();
    const char *strs[] = {"foo", "bar", "baz"};
    robj **prefixes = createPrefixArray(strs, 3);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 3);
    EXPECT_EQ(result, 1);
    EXPECT_EQ(c->bufpos, 0u);

    freePrefixArray(prefixes, 3);
    freeTrackingTestClient(c);
}

/* Self-collision: "foo" is a prefix of "foobar". */
TEST_F(TrackingTest, SelfCollisionShorterFirst) {
    client *c = createTrackingTestClient();
    const char *strs[] = {"foo", "foobar"};
    robj **prefixes = createPrefixArray(strs, 2);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 2);
    /* Returns the index of the first prefix in the colliding pair (i=0). */
    EXPECT_EQ(result, 0);

    freePrefixArray(prefixes, 2);
    freeTrackingTestClient(c);
}

/* Self-collision: "foobar" then "foo" — longer first. */
TEST_F(TrackingTest, SelfCollisionLongerFirst) {
    client *c = createTrackingTestClient();
    const char *strs[] = {"foobar", "foo"};
    robj **prefixes = createPrefixArray(strs, 2);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 2);
    EXPECT_EQ(result, 0);

    freePrefixArray(prefixes, 2);
    freeTrackingTestClient(c);
}

/* Self-collision: identical prefixes. */
TEST_F(TrackingTest, SelfCollisionIdentical) {
    client *c = createTrackingTestClient();
    const char *strs[] = {"foo", "foo"};
    robj **prefixes = createPrefixArray(strs, 2);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 2);
    EXPECT_EQ(result, 0);

    freePrefixArray(prefixes, 2);
    freeTrackingTestClient(c);
}

/* Self-collision detected at a later index (first pair is fine).
 * This is a regression test for a bug where the self-collision check
 * did "return i" instead of "return 0". When i > 0, the caller in
 * networking.c treated the truthy return as success, enabling tracking
 * with overlapping prefixes and sending a double reply (protocol
 * violation). The fix is to always return 0 on collision. */
TEST_F(TrackingTest, SelfCollisionAtLaterIndex) {
    client *c = createTrackingTestClient();
    const char *strs[] = {"aaa", "bbb", "bbbc"};
    robj **prefixes = createPrefixArray(strs, 3);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 3);
    /* Collision between index 1 ("bbb") and index 2 ("bbbc").
     * Must return 0 (collision), not i (which would be 1 = success). */
    EXPECT_EQ(result, 0);

    freePrefixArray(prefixes, 3);
    freeTrackingTestClient(c);
}

/* Collision with an existing prefix on the client. */
TEST_F(TrackingTest, CollisionWithExistingPrefix) {
    client *c = createTrackingTestClient();
    addExistingPrefix(c, "foo");

    const char *strs[] = {"foobar"};
    robj **prefixes = createPrefixArray(strs, 1);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 1);
    /* Returns 0 when colliding with an existing prefix. */
    EXPECT_EQ(result, 0);

    freePrefixArray(prefixes, 1);
    freeTrackingTestClient(c);
}

/* Existing prefix is longer than the new one — still a collision. */
TEST_F(TrackingTest, CollisionWithExistingLongerPrefix) {
    client *c = createTrackingTestClient();
    addExistingPrefix(c, "foobar");

    const char *strs[] = {"foo"};
    robj **prefixes = createPrefixArray(strs, 1);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 1);
    EXPECT_EQ(result, 0);

    freePrefixArray(prefixes, 1);
    freeTrackingTestClient(c);
}

/* Existing prefix is identical to the new one — collision. */
TEST_F(TrackingTest, CollisionWithExistingIdentical) {
    client *c = createTrackingTestClient();
    addExistingPrefix(c, "foo");

    const char *strs[] = {"foo"};
    robj **prefixes = createPrefixArray(strs, 1);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 1);
    EXPECT_EQ(result, 0);

    freePrefixArray(prefixes, 1);
    freeTrackingTestClient(c);
}

/* No collision with existing prefix when prefixes are disjoint. */
TEST_F(TrackingTest, NoCollisionWithExistingDisjoint) {
    client *c = createTrackingTestClient();
    addExistingPrefix(c, "abc");

    const char *strs[] = {"xyz"};
    robj **prefixes = createPrefixArray(strs, 1);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 1);
    EXPECT_EQ(result, 1);
    EXPECT_EQ(c->bufpos, 0u);

    freePrefixArray(prefixes, 1);
    freeTrackingTestClient(c);
}

/* Multiple existing prefixes, collision with one of them. */
TEST_F(TrackingTest, CollisionWithOneOfMultipleExisting) {
    client *c = createTrackingTestClient();
    addExistingPrefix(c, "aaa");
    addExistingPrefix(c, "bbb");

    const char *strs[] = {"bbbc"};
    robj **prefixes = createPrefixArray(strs, 1);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 1);
    EXPECT_EQ(result, 0);

    freePrefixArray(prefixes, 1);
    freeTrackingTestClient(c);
}

/* Existing prefix check takes priority over self-collision check.
 * If the first prefix collides with an existing one, we get 0 (not i). */
TEST_F(TrackingTest, ExistingCollisionPriorityOverSelf) {
    client *c = createTrackingTestClient();
    addExistingPrefix(c, "foo");

    /* "foobar" collides with existing "foo", and also "foobar"/"foo" would
     * self-collide. The existing check runs first, so we expect 0. */
    const char *strs[] = {"foobar", "foo"};
    robj **prefixes = createPrefixArray(strs, 2);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 2);
    EXPECT_EQ(result, 0);

    freePrefixArray(prefixes, 2);
    freeTrackingTestClient(c);
}

/* Empty string prefix overlaps with everything. */
TEST_F(TrackingTest, EmptyPrefixOverlapsWithNonEmpty) {
    client *c = createTrackingTestClient();
    const char *strs[] = {"", "foo"};
    robj **prefixes = createPrefixArray(strs, 2);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 2);
    EXPECT_EQ(result, 0);

    freePrefixArray(prefixes, 2);
    freeTrackingTestClient(c);
}

/* Single-character prefixes that don't overlap. */
TEST_F(TrackingTest, SingleCharNonOverlapping) {
    client *c = createTrackingTestClient();
    const char *strs[] = {"a", "b", "c"};
    robj **prefixes = createPrefixArray(strs, 3);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 3);
    EXPECT_EQ(result, 1);
    EXPECT_EQ(c->bufpos, 0u);

    freePrefixArray(prefixes, 3);
    freeTrackingTestClient(c);
}

/* No existing prefixes rax (NULL) — should not crash and should succeed. */
TEST_F(TrackingTest, NullExistingPrefixesRax) {
    client *c = createTrackingTestClient();
    /* Explicitly ensure client_tracking_prefixes is NULL. */
    c->pubsub_data->client_tracking_prefixes = NULL;

    const char *strs[] = {"foo", "bar"};
    robj **prefixes = createPrefixArray(strs, 2);

    int result = checkPrefixCollisionsOrReply(c, prefixes, 2);
    EXPECT_EQ(result, 1);
    EXPECT_EQ(c->bufpos, 0u);

    freePrefixArray(prefixes, 2);
    freeTrackingTestClient(c);
}
