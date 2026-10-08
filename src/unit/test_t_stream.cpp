/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "listpack.h"
#include "stream.h"
}

/* Mirrors of the record flags private to t_stream.c. */
#define TEST_STREAM_ITEM_FLAG_DELETED (1 << 0)
#define TEST_STREAM_ITEM_FLAG_SAMEFIELDS (1 << 1)

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

/* Build a structurally valid two-record stream listpack, then re-encode the
 * first record's flags with a valid but NON-minimal 16-bit integer encoding --
 * the shape a crafted RESTORE payload can take. The listpack stays well-formed
 * (it still passes lpValidateIntegrity and streamValidateListpackIntegrity), but
 * the wider flags element makes streamTrim's lpReplaceInteger shrink the
 * listpack when it marks the record deleted. */
static unsigned char *buildStreamListpackWithWideFirstFlags(void) {
    unsigned char *lp = buildTwoRecordStreamListpack(2, 0, 0, 0);
    /* Element index 5 is the first record's flags; indexes 0..4 are the primary
     * entry (count, deleted, num-fields, field, terminator). */
    unsigned char *p = lpSeek(lp, 5);
    unsigned char *pnext = lpNext(lp, p);
    size_t off = (size_t)(p - lp);
    size_t oldelen = (size_t)(pnext - p); /* 7-bit int + backlen == 2 bytes */
    size_t oldtotal = lpBytes(lp);
    /* 16-bit int encoding of SAMEFIELDS(2): 0xF1, value little-endian in two
     * bytes, then a one-byte backlen of 3 (the encoded element length). */
    const unsigned char wide[] = {0xF1, 0x02, 0x00, 0x03};
    size_t newtotal = oldtotal - oldelen + sizeof(wide);
    unsigned char *nlp = lpNew(newtotal);
    memcpy(nlp, lp, off);                   /* header + earlier elements */
    memcpy(nlp + off, wide, sizeof(wide));  /* widened flags element */
    memcpy(nlp + off + sizeof(wide), pnext, oldtotal - off - oldelen); /* rest + terminator */
    /* Patch the four-byte little-endian total-bytes header. */
    nlp[0] = (unsigned char)(newtotal & 0xFF);
    nlp[1] = (unsigned char)((newtotal >> 8) & 0xFF);
    nlp[2] = (unsigned char)((newtotal >> 16) & 0xFF);
    nlp[3] = (unsigned char)((newtotal >> 24) & 0xFF);
    lpFree(lp);
    return nlp;
}

/* Regression: trimming a stream whose record flags use a non-minimal integer
 * encoding used to abort the server. streamTrim() saved the parse cursor as a
 * byte offset, then lpReplaceInteger() shrank the listpack when re-encoding the
 * flags in canonical form, so restoring the cursor from the stale offset
 * overshot the next entry and the following parse tripped
 * serverAssert(ret != 0) in lpGetIntegerIfValid. */
TEST_F(StreamListpackIntegrityTest, TrimSurvivesNonMinimalFlagsEncoding) {
    unsigned char *lp = buildStreamListpackWithWideFirstFlags();

    /* The crafted listpack is still well-formed and stream-valid, so RESTORE
     * would accept it. */
    uint64_t valid_count = 0;
    ASSERT_EQ(lpValidateIntegrity(lp, lpBytes(lp), NULL, NULL, 0), 1);
    ASSERT_EQ(streamValidateListpackIntegrity(lp, lpBytes(lp), &valid_count), 1);
    ASSERT_EQ(valid_count, 2u);

    stream *s = streamNew();
    streamID master_id = {0, 0};
    uint64_t rax_key[2];
    streamEncodeID(rax_key, &master_id);
    raxInsert(s->rax, (unsigned char *)rax_key, sizeof(rax_key), lp, NULL);
    s->length = 2;

    /* XTRIM MAXLEN = 1 marks the first record deleted; the server must not
     * abort and the trim must report one removed entry. */
    int64_t deleted = streamTrimByLength(s, 1, 0);
    ASSERT_EQ(deleted, 1);
    ASSERT_EQ(s->length, 1u);

    freeStream(s);
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
