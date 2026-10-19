/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "listpack.h"
#include "stream.h"
}

/* Mirrors of the record flags private to t_stream.c. */
#define TEST_STREAM_ITEM_FLAG_DELETED (1 << 0)
#define TEST_STREAM_ITEM_FLAG_SAMEFIELDS (1 << 1)

/* Two nodes with three integer-encoded fields per entry. ID 30-0 is deleted.
 * Exercise both primary field reuse and entries carrying their own fields. */
static stream *buildForwardSearchStream(int samefields) {
    stream *s = streamNew();
    for (int node = 0; node < 2; node++) {
        unsigned char *lp = lpNew(0);
        lp = lpAppendInteger(lp, node == 0 ? 3 : 4);
        lp = lpAppendInteger(lp, node == 0 ? 1 : 0);
        lp = lpAppendInteger(lp, 3);
        for (int field = 1; field <= 3; field++) lp = lpAppendInteger(lp, field);
        lp = lpAppendInteger(lp, 0);
        for (int item = 0; item < 4; item++) {
            int flags = samefields ? TEST_STREAM_ITEM_FLAG_SAMEFIELDS : 0;
            if (node == 0 && item == 2) flags |= TEST_STREAM_ITEM_FLAG_DELETED;
            lp = lpAppendInteger(lp, flags);
            lp = lpAppendInteger(lp, item * 10);
            lp = lpAppendInteger(lp, 0);
            if (!samefields) lp = lpAppendInteger(lp, 3);
            for (int field = 1; field <= 3; field++) {
                if (!samefields) lp = lpAppendInteger(lp, field + 3);
                lp = lpAppendInteger(lp, (node * 4 + item + 1) * 10 + field);
            }
            lp = lpAppendInteger(lp, samefields ? 6 : 10);
        }
        streamID primary = {node == 0 ? 10u : 100u, 0};
        unsigned char key[sizeof(streamID)];
        streamEncodeID(key, &primary);
        raxInsert(s->rax, key, sizeof(key), lp, NULL);
    }
    s->length = 7;
    return s;
}

static void expectEntryField(streamForwardEntry *entry, int field, int value) {
    unsigned char *fieldptr, *valueptr;
    int64_t fieldlen, valuelen;
    ASSERT_EQ(streamForwardEntryNextField(entry, &fieldptr, &valueptr, &fieldlen, &valuelen), 1);
    char expected_field[32], expected_value[32];
    int expected_fieldlen = snprintf(expected_field, sizeof(expected_field), "%d", field);
    int expected_valuelen = snprintf(expected_value, sizeof(expected_value), "%d", value);
    ASSERT_EQ(fieldlen, expected_fieldlen);
    ASSERT_EQ(valuelen, expected_valuelen);
    EXPECT_EQ(memcmp(fieldptr, expected_field, fieldlen), 0);
    EXPECT_EQ(memcmp(valueptr, expected_value, valuelen), 0);
}

/* Reading none, some, or all fields must not change the next search result.
 * Test both encoding layouts within a node and across node boundaries. */
TEST(StreamForwardIteratorTest, FindsEntriesAfterAnyFieldReadCount) {
    for (int samefields = 0; samefields <= 1; samefields++) {
        for (int read_count = 0; read_count <= 3; read_count++) {
            stream *s = buildForwardSearchStream(samefields);
            streamForwardIterator it;
            streamID target = {10, 0};
            streamForwardIteratorStart(&it, s, &target);
            streamForwardEntry *entry = streamForwardIteratorFind(&it, &target);
            ASSERT_NE(entry, nullptr);
            EXPECT_EQ(entry->id.ms, 10u);
            ASSERT_EQ(entry->numfields, 3);
            for (int field = 1; field <= read_count; field++) {
                expectEntryField(entry, samefields ? field : field + 3, 10 + field);
            }

            target.ms = 20;
            entry = streamForwardIteratorFind(&it, &target);
            ASSERT_NE(entry, nullptr);
            EXPECT_EQ(entry->id.ms, 20u);
            for (int field = 1; field <= read_count; field++) {
                expectEntryField(entry, samefields ? field : field + 3, 20 + field);
            }

            /* Crossing a node must also handle a partially read entry. */
            target.ms = 100;
            entry = streamForwardIteratorFind(&it, &target);
            ASSERT_NE(entry, nullptr);
            EXPECT_EQ(entry->id.ms, 100u);
            for (int field = 1; field <= 3; field++) {
                expectEntryField(entry, samefields ? field : field + 3, 50 + field);
            }
            unsigned char *fieldptr, *valueptr;
            int64_t fieldlen, valuelen;
            EXPECT_EQ(streamForwardEntryNextField(entry, &fieldptr, &valueptr, &fieldlen, &valuelen), 0);
            EXPECT_EQ(streamForwardEntryNextField(entry, &fieldptr, &valueptr, &fieldlen, &valuelen), 0);

            target.ms = 110;
            entry = streamForwardIteratorFind(&it, &target);
            ASSERT_NE(entry, nullptr);
            expectEntryField(entry, samefields ? 1 : 4, 61);
            target.ms = 140;
            EXPECT_EQ(streamForwardIteratorFind(&it, &target), nullptr);
            target.ms = 150;
            EXPECT_EQ(streamForwardIteratorFind(&it, &target), nullptr);
            streamForwardIteratorStop(&it);
            freeStream(s);
        }
    }
}

TEST(StreamForwardIteratorTest, PreservesFutureEntriesAfterMissingTargets) {
    for (int samefields = 0; samefields <= 1; samefields++) {
        stream *s = buildForwardSearchStream(samefields);
        streamForwardIterator it;
        streamID target = {10, 0};
        streamForwardIteratorStart(&it, s, &target);
        streamForwardEntry *entry = streamForwardIteratorFind(&it, &target);
        ASSERT_NE(entry, nullptr);
        expectEntryField(entry, samefields ? 1 : 4, 11);
        target.ms = 30; /* Deleted entry; 40-0 must remain cached. */
        EXPECT_EQ(streamForwardIteratorFind(&it, &target), nullptr);
        target.ms = 35;
        EXPECT_EQ(streamForwardIteratorFind(&it, &target), nullptr);
        target.ms = 40;
        entry = streamForwardIteratorFind(&it, &target);
        ASSERT_NE(entry, nullptr);
        expectEntryField(entry, samefields ? 1 : 4, 41);
        target.ms = 90; /* Absent target in the gap between nodes. */
        EXPECT_EQ(streamForwardIteratorFind(&it, &target), nullptr);
        target.ms = 100;
        entry = streamForwardIteratorFind(&it, &target);
        ASSERT_NE(entry, nullptr);
        expectEntryField(entry, samefields ? 1 : 4, 51);
        streamForwardIteratorStop(&it);
        freeStream(s);
    }
}

TEST(StreamForwardIteratorTest, FindsFirstTargetSeveralNodesAfterStart) {
    for (int samefields = 0; samefields <= 1; samefields++) {
        stream *s = buildForwardSearchStream(samefields);
        streamID primary = {100, 0};
        unsigned char key[sizeof(streamID)];
        streamEncodeID(key, &primary);
        void *value;
        ASSERT_EQ(raxFind(s->rax, key, sizeof(key), &value), 1);
        unsigned char *lp = (unsigned char *)value;
        for (int node = 2; node < 10; node++) {
            primary.ms = node * 100;
            streamEncodeID(key, &primary);
            raxInsert(s->rax, key, sizeof(key), lpDup(lp), NULL);
            s->length += 4;
        }

        for (int missing = 0; missing <= 1; missing++) {
            streamForwardIterator it;
            streamID start = {0, 0};
            streamForwardIteratorStart(&it, s, &start);
            streamID target = {missing ? 899u : 900u, 0};
            if (missing) {
                EXPECT_EQ(streamForwardIteratorFind(&it, &target), nullptr);
                target.ms = 900;
            }
            streamForwardEntry *entry = streamForwardIteratorFind(&it, &target);
            ASSERT_NE(entry, nullptr);
            EXPECT_EQ(entry->id.ms, 900u);
            expectEntryField(entry, samefields ? 1 : 4, 51);
            target.ms = 920;
            entry = streamForwardIteratorFind(&it, &target);
            ASSERT_NE(entry, nullptr);
            EXPECT_EQ(entry->id.ms, 920u);
            expectEntryField(entry, samefields ? 1 : 4, 71);
            target.ms = 940;
            EXPECT_EQ(streamForwardIteratorFind(&it, &target), nullptr);
            target.ms = 950;
            EXPECT_EQ(streamForwardIteratorFind(&it, &target), nullptr);
            streamForwardIteratorStop(&it);
        }
        freeStream(s);
    }
}

TEST(StreamForwardIteratorTest, StartsAndRestartsOnNonzeroStorage) {
    stream *empty = streamNew();
    stream *s = buildForwardSearchStream(1);
    streamForwardIterator it;
    memset(&it, 0xa5, sizeof(it));
    streamID target = {10, 0};

    /* Stopping before the first search must also release initialized state. */
    streamForwardIteratorStart(&it, s, &target);
    streamForwardIteratorStop(&it);
    streamForwardIteratorStart(&it, empty, &target);
    EXPECT_EQ(streamForwardIteratorFind(&it, &target), nullptr);
    target.ms = 20;
    EXPECT_EQ(streamForwardIteratorFind(&it, &target), nullptr);
    streamForwardIteratorStop(&it);

    target.ms = 10;
    streamForwardIteratorStart(&it, s, &target);
    streamForwardEntry *entry = streamForwardIteratorFind(&it, &target);
    ASSERT_NE(entry, nullptr);
    expectEntryField(entry, 1, 11);
    target.ms = 100;
    entry = streamForwardIteratorFind(&it, &target);
    ASSERT_NE(entry, nullptr);
    expectEntryField(entry, 1, 51);
    streamForwardIteratorStop(&it);

    /* Restart after a populated cache, including the successor-node state. */
    target.ms = 10;
    streamForwardIteratorStart(&it, s, &target);
    entry = streamForwardIteratorFind(&it, &target);
    ASSERT_NE(entry, nullptr);
    expectEntryField(entry, 1, 11);
    streamForwardIteratorStop(&it);

    streamForwardIteratorStart(&it, s, NULL);
    target.ms = 100;
    entry = streamForwardIteratorFind(&it, &target);
    ASSERT_NE(entry, nullptr);
    expectEntryField(entry, 1, 51);
    streamForwardIteratorStop(&it);
    freeStream(s);
    freeStream(empty);
}

static void findInvalidForwardTarget(int kind) {
    server.enable_debug_assert = 1;
    stream *s = buildForwardSearchStream(1);
    streamForwardIterator it;
    streamID target = {20, 0};
    streamForwardIteratorStart(&it, s, &target);
    if (kind == 0) {
        target.ms = 10; /* Before the initial lower bound. */
    } else {
        target.ms = 30; /* Missing targets must also advance the lower bound. */
        streamForwardIteratorFind(&it, &target);
        target.ms = kind == 1 ? 30 : 20;
    }
    streamForwardIteratorFind(&it, &target);
    streamForwardIteratorStop(&it);
    freeStream(s);
}

TEST(StreamForwardIteratorDeathTest, RejectsInvalidTargetOrder) {
    EXPECT_DEATH(findInvalidForwardTarget(0), "");
    EXPECT_DEATH(findInvalidForwardTarget(1), "");
    EXPECT_DEATH(findInvalidForwardTarget(2), "");
}

/* Build a structurally valid stream listpack holding two same-fields records.
 * The declared header counts and the per-record deleted flags are supplied
 * separately so a caller can describe a header whose live/deleted split
 * disagrees with the records that actually follow. */
static unsigned char *buildTwoRecordStreamListpack(long long declared_live,
                                                   long long declared_deleted,
                                                   int first_deleted,
                                                   int second_deleted) {
    unsigned char *lp = lpNew(0);

    /* Primary entry: count, deleted, num-primary-fields, field, terminator. */
    lp = lpAppendInteger(lp, declared_live);
    lp = lpAppendInteger(lp, declared_deleted);
    lp = lpAppendInteger(lp, 1);
    lp = lpAppend(lp, (unsigned char *)"f", 1);
    lp = lpAppendInteger(lp, 0);

    /* Two records, each reusing the primary field, so lp-count is 1 field
     * plus the three fixed elements. */
    for (int i = 0; i < 2; i++) {
        int deleted = i == 0 ? first_deleted : second_deleted;
        long long flags = TEST_STREAM_ITEM_FLAG_SAMEFIELDS;
        if (deleted) flags |= TEST_STREAM_ITEM_FLAG_DELETED;

        lp = lpAppendInteger(lp, flags);
        lp = lpAppendInteger(lp, 0); /* ms diff */
        lp = lpAppendInteger(lp, i); /* seq diff */
        lp = lpAppend(lp, (unsigned char *)(i == 0 ? "v1" : "v2"), 2);
        lp = lpAppendInteger(lp, 4);
    }

    return lp;
}

class StreamListpackIntegrityTest : public ::testing::Test {};

/* Control: the header split matches the records, so the payload is accepted.
 * This keeps the mismatch tests below honest by proving the hand-built
 * fixture is otherwise structurally valid. */
TEST_F(StreamListpackIntegrityTest, TestAcceptsMatchingLiveAndDeletedCounts) {
    unsigned char *lp = buildTwoRecordStreamListpack(2, 0, 0, 0);
    uint64_t valid_count = 0;
    ASSERT_EQ(lpLength(lp), 15u);
    ASSERT_EQ(streamValidateListpackIntegrity(lp, lpBytes(lp), &valid_count), 1);
    ASSERT_EQ(valid_count, 2u);
    lpFree(lp);
}

TEST_F(StreamListpackIntegrityTest, TestAcceptsMatchingDeletedRecord) {
    unsigned char *lp = buildTwoRecordStreamListpack(1, 1, 0, 1);
    uint64_t valid_count = 0;
    ASSERT_EQ(streamValidateListpackIntegrity(lp, lpBytes(lp), &valid_count), 1);
    ASSERT_EQ(valid_count, 1u);
    lpFree(lp);
}

/* Both records are live, but the header claims one live and one deleted. The
 * total still equals two, so a total-only check accepts this payload while the
 * live count is understated. XDEL then sees a declared live count of 1, frees
 * the whole node and destroys the record the header did not account for. */
TEST_F(StreamListpackIntegrityTest, TestRejectsUnderstatedLiveCount) {
    unsigned char *lp = buildTwoRecordStreamListpack(1, 1, 0, 0);
    uint64_t valid_count = 0;
    ASSERT_EQ(lpLength(lp), 15u);
    ASSERT_EQ(streamValidateListpackIntegrity(lp, lpBytes(lp), &valid_count), 0);
    lpFree(lp);
}

/* The mirrored case: one record is flagged deleted while the header claims
 * two live records and no deleted ones. The total again matches. */
TEST_F(StreamListpackIntegrityTest, TestRejectsOverstatedLiveCount) {
    unsigned char *lp = buildTwoRecordStreamListpack(2, 0, 0, 1);
    uint64_t valid_count = 0;
    ASSERT_EQ(streamValidateListpackIntegrity(lp, lpBytes(lp), &valid_count), 0);
    lpFree(lp);
}

/* Every record is flagged deleted while the header claims both are live. */
TEST_F(StreamListpackIntegrityTest, TestRejectsAllRecordsDeletedWithLiveHeader) {
    unsigned char *lp = buildTwoRecordStreamListpack(2, 0, 1, 1);
    uint64_t valid_count = 0;
    ASSERT_EQ(streamValidateListpackIntegrity(lp, lpBytes(lp), &valid_count), 0);
    lpFree(lp);
}

class StreamIdTest : public ::testing::Test {};

TEST_F(StreamIdTest, TestStreamEncodeDecodeRoundtrip) {
    streamID id = {0x0102030405060708ULL, 0x090a0b0c0d0e0f10ULL};
    unsigned char buf[16];

    streamEncodeID(buf, &id);

    streamID decoded;
    streamDecodeID(buf, &decoded);

    ASSERT_EQ(decoded.ms, id.ms);
    ASSERT_EQ(decoded.seq, id.seq);
}

TEST_F(StreamIdTest, TestStreamEncodeIDBigEndian) {
    streamID id = {0x0102030405060708ULL, 0x090a0b0c0d0e0f10ULL};
    unsigned char buf[16];

    streamEncodeID(buf, &id);

    /* Verify big-endian byte order for ms (first 8 bytes) */
    ASSERT_EQ(buf[0], 0x01);
    ASSERT_EQ(buf[1], 0x02);
    ASSERT_EQ(buf[2], 0x03);
    ASSERT_EQ(buf[3], 0x04);
    ASSERT_EQ(buf[4], 0x05);
    ASSERT_EQ(buf[5], 0x06);
    ASSERT_EQ(buf[6], 0x07);
    ASSERT_EQ(buf[7], 0x08);

    /* Verify big-endian byte order for seq (next 8 bytes) */
    ASSERT_EQ(buf[8], 0x09);
    ASSERT_EQ(buf[9], 0x0a);
    ASSERT_EQ(buf[10], 0x0b);
    ASSERT_EQ(buf[11], 0x0c);
    ASSERT_EQ(buf[12], 0x0d);
    ASSERT_EQ(buf[13], 0x0e);
    ASSERT_EQ(buf[14], 0x0f);
    ASSERT_EQ(buf[15], 0x10);
}

TEST_F(StreamIdTest, TestStreamIDLexicographicOrdering) {
    /* Big-endian encoding ensures memcmp preserves numeric order */
    streamID id_a = {100, 0};
    streamID id_b = {200, 0};
    unsigned char buf_a[16], buf_b[16];

    streamEncodeID(buf_a, &id_a);
    streamEncodeID(buf_b, &id_b);

    ASSERT_LT(memcmp(buf_a, buf_b, 16), 0);

    /* Test sequence ordering when ms is equal */
    streamID id_c = {100, 1};
    streamID id_d = {100, 2};
    unsigned char buf_c[16], buf_d[16];

    streamEncodeID(buf_c, &id_c);
    streamEncodeID(buf_d, &id_d);

    ASSERT_LT(memcmp(buf_c, buf_d, 16), 0);
}
