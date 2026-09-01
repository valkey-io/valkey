/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

/* Ensure assert() is never compiled out, even in Release builds. */
#undef NDEBUG
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "allocator_defrag.h"
#include "entry.h"
#include "fmacros.h"
#include "vset.h"
#include "zmalloc.h"
}

typedef entry mock_entry;

static mock_entry *mockCreateEntry(const char *keystr, long long expiry) {
    sds field = sdsnew(keystr);
    mock_entry *e = entryCreate(field, sdsnew("value"), expiry);
    sdsfree(field);
    return e;
}

static void mockFreeEntry(void *entry) {
    entryFree((mock_entry *)entry);
}

static mock_entry *mockEntryUpdate(mock_entry *entry, long long expiry) {
    sds field = entryGetField(entry);
    size_t len;
    mock_entry *new_entry = entryCreate(field, sdsdup(entryGetValue(entry, &len)), expiry);
    entryFree(entry);
    return new_entry;
}

static long long mockGetExpiry(const void *entry) {
    return entryGetExpiry((const mock_entry *)entry);
}

/* Global array to simulate a test database */
static mock_entry *mock_entries[10000];
static int mock_entry_count = 0;

/* --------- volatileEntryType Callbacks --------- */
static long long mock_entry_get_expiry(const void *entry) {
    return mockGetExpiry(entry);
}

static int mock_entry_expire(void *entry, void *ctx) {
    mock_entry *e = (mock_entry *)entry;
    long long now = *(long long *)ctx;
    (void)now;
    serverAssert(mock_entry_get_expiry(entry) <= now);
    for (int i = 0; i < mock_entry_count; i++) {
        if (mock_entries[i] == e) {
            mockFreeEntry(e);
            mock_entries[i] = mock_entries[--mock_entry_count];
            return 1;
        }
    }
    return 0;
}

/* --------- Helper Functions --------- */
static mock_entry *mock_entry_create(const char *keystr, long long expiry) {
    return mockCreateEntry(keystr, expiry);
}

static int insert_mock_entry(vset *set) {
    if (mock_entry_count >= 10000) return 0;
    char keybuf[32];
    snprintf(keybuf, sizeof(keybuf), "key_%d", mock_entry_count);

    long long expiry = rand() % 10000 + 100;
    mock_entry *e = mock_entry_create(keybuf, expiry);
    assert(vsetAddEntry(set, mockGetExpiry, e));
    mock_entries[mock_entry_count++] = e;
    return 0;
}

static int insert_mock_entry_with_expiry(vset *set, long long expiry) {
    if (mock_entry_count >= 10000) return 0;
    char keybuf[32];
    snprintf(keybuf, sizeof(keybuf), "key_%d", mock_entry_count);

    mock_entry *e = mock_entry_create(keybuf, expiry);
    assert(vsetAddEntry(set, mockGetExpiry, e));
    mock_entries[mock_entry_count++] = e;
    return 0;
}

static int update_mock_entry(vset *set) {
    if (mock_entry_count == 0) return 0;
    int idx = rand() % mock_entry_count;
    mock_entry *old = mock_entries[idx];
    long long old_expiry = mockGetExpiry(old);
    long long new_expiry = old_expiry + (rand() % 500);
    mock_entry *updated = mockEntryUpdate(old, new_expiry);
    mock_entries[idx] = updated;
    assert(vsetUpdateEntry(set, mockGetExpiry, old, updated, old_expiry, new_expiry));
    return 0;
}

static int remove_mock_entry(vset *set) {
    if (mock_entry_count == 0) return 0;
    int idx = rand() % mock_entry_count;
    mock_entry *e = mock_entries[idx];
    assert(vsetRemoveEntry(set, mockGetExpiry, e));
    mockFreeEntry(e);
    mock_entries[idx] = mock_entries[--mock_entry_count];
    return 0;
}

static int expire_mock_entries(vset *set, mstime_t now) {
    vsetRemoveExpired(set, mockGetExpiry, mock_entry_expire, now, mock_entry_count, &now);
    return 0;
}

static void *mock_defragfn(void *ptr) {
    size_t size = zmalloc_usable_size(ptr);
    void *newptr = zmalloc(size);
    memcpy(newptr, ptr, size);
    zfree(ptr);
    /* Update mock_entries to track the new pointer so that expire/remove
     * callbacks can still find the entry after defrag. */
    for (int i = 0; i < mock_entry_count; i++) {
        if ((void *)mock_entries[i] == ptr) {
            mock_entries[i] = (mock_entry *)newptr;
            break;
        }
    }
    return newptr;
}

static size_t defrag_vset(vset *set, size_t cursor, size_t steps) {
    if (steps == 0) steps = ULONG_MAX;
    do {
        cursor = vsetScanDefrag(set, cursor, mock_defragfn);
        steps--;
    } while (cursor != 0 && steps > 0);
    return cursor;
}

static int free_mock_entries(void) {
    for (int i = 0; i < mock_entry_count; i++) {
        mock_entry *e = mock_entries[i];
        mockFreeEntry(e);
    }
    mock_entry_count = 0;
    return 0;
}

class VsetTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        allocatorDefragInit();
    }

    void TearDown() override {
        free_mock_entries();
    }
};

TEST_F(VsetTest, TestVsetAddAndIterate) {
    vset set;
    vsetInit(&set);

    mock_entry *e1 = mockCreateEntry("item1", 123);
    mock_entry *e2 = mockCreateEntry("item2", 456);

    ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, e1));
    ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, e2));

    ASSERT_FALSE(vsetIsEmpty(&set));

    vsetIterator it;
    vsetInitIterator(&set, &it);

    void *entry;
    int count = 0;
    while (vsetNext(&it, &entry)) {
        ASSERT_NE(entry, nullptr);
        count++;
    }

    ASSERT_EQ(count, 2);

    vsetResetIterator(&it);
    vsetRelease(&set);
    mockFreeEntry(e1);
    mockFreeEntry(e2);
}

/* Exercises vsetEstimatedEarliestExpiry() across every reachable bucket
 * encoding: NONE, SINGLE, VECTOR, and RAX (both a single time-bucket and
 * multiple time-buckets).
 *
 * Note on RAX: the estimate is the timestamp of the earliest time-bucket
 * (the entry's expiry rounded up to a bucket-interval boundary), not the
 * exact entry expiry. raxSeek("^") already positions the iterator on the
 * smallest key and populates it.key, so the RAX case reads the earliest
 * bucket directly.
 *
 * A top-level HT bucket is intentionally not tested: vsetAddEntry() always
 * wraps an over-full vector into a RAX (HT exists only as a rax sub-bucket),
 * and shrinkRaxBucketIfPossible() never collapses a rax to a bare HT, so a
 * top-level HT set is unreachable. */
TEST_F(VsetTest, TestVsetEstimatedEarliestExpiry) {
    /* Bucket keys round an expiry up to a bucket-interval boundary, so a
     * RAX bucket's timestamp lies within VOLATILESET_BUCKET_INTERVAL_MAX of
     * the entries' expiry. vset.h does not export that constant, so mirror it
     * here (white-box). */
    const long long BUCKET_MAX = 1LL << 13; /* VOLATILESET_BUCKET_INTERVAL_MAX */

    /* --- VSET_BUCKET_NONE: empty set returns -1 --- */
    {
        vset set;
        vsetInit(&set);
        ASSERT_TRUE(vsetIsEmpty(&set));
        ASSERT_EQ(vsetEstimatedEarliestExpiry(&set, mockGetExpiry), -1);
        vsetRelease(&set);
    }

    /* --- VSET_BUCKET_SINGLE: returns the sole entry's exact expiry --- */
    {
        vset set;
        vsetInit(&set);
        mock_entry *e = mockCreateEntry("only", 12345LL);
        ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, e));
        ASSERT_EQ(vsetEstimatedEarliestExpiry(&set, mockGetExpiry), 12345LL);
        vsetRelease(&set);
        mockFreeEntry(e);
    }

    /* --- VSET_BUCKET_VECTOR: returns the earliest (sorted index 0) expiry,
     * regardless of insertion order. Stays a vector while < 128 entries. --- */
    {
        vset set;
        vsetInit(&set);
        const long long expiries[] = {500, 100, 300, 700, 200};
        const int n = (int)(sizeof(expiries) / sizeof(expiries[0]));
        mock_entry *entries[n];
        for (int i = 0; i < n; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "v_%d", i);
            entries[i] = mockCreateEntry(buf, expiries[i]);
            ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[i]));
        }
        /* 100 is the minimum -> exact expiry returned for a vector. */
        ASSERT_EQ(vsetEstimatedEarliestExpiry(&set, mockGetExpiry), 100LL);
        vsetRelease(&set);
        for (int i = 0; i < n; i++) mockFreeEntry(entries[i]);
    }

    /* --- VSET_BUCKET_RAX, single time-bucket: > 127 entries with the same
     * expiry force a vector -> RAX conversion (one HT sub-bucket). The
     * estimate is the earliest bucket's timestamp: the bucket key rounds the
     * expiry up to a bucket-interval boundary, so it lies in
     * [expiry, expiry + BUCKET_MAX]. With the raxNext() bug it instead reads
     * an unpopulated key (typically 0), which fails the lower bound. --- */
    {
        vset set;
        vsetInit(&set);
        const long long expiry = 1000LL;
        const int n = 200;
        mock_entry *entries[n];
        for (int i = 0; i < n; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "r_%d", i);
            entries[i] = mockCreateEntry(buf, expiry);
            ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[i]));
        }
        long long est = vsetEstimatedEarliestExpiry(&set, mockGetExpiry);
        ASSERT_GE(est, expiry);
        ASSERT_LE(est, expiry + BUCKET_MAX);
        vsetRelease(&set);
        for (int i = 0; i < n; i++) mockFreeEntry(entries[i]);
    }

    /* --- VSET_BUCKET_RAX, multiple time-buckets: the estimate must come from
     * the EARLIEST bucket, proving raxSeek("^") + raxNext() selects the
     * smallest key (not a later one or garbage). --- */
    {
        vset set;
        vsetInit(&set);
        const long long early = 1000LL;
        const long long late = 100LL * 1000 * 1000; /* a far later, distinct bucket */
        const int n_each = 128;                     /* force RAX */
        mock_entry *entries[2 * n_each];
        int idx = 0;
        /* Insert the LATE bucket first to ensure ordering is by key, not by
         * insertion order. */
        for (int i = 0; i < n_each; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "late_%d", i);
            entries[idx] = mockCreateEntry(buf, late);
            ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[idx]));
            idx++;
        }
        for (int i = 0; i < n_each; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "early_%d", i);
            entries[idx] = mockCreateEntry(buf, early);
            ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[idx]));
            idx++;
        }
        long long est = vsetEstimatedEarliestExpiry(&set, mockGetExpiry);
        /* Must reflect the early bucket, not the late one. */
        ASSERT_GE(est, early);
        ASSERT_LE(est, early + BUCKET_MAX);
        ASSERT_LT(est, late);
        vsetRelease(&set);
        for (int i = 0; i < idx; i++) mockFreeEntry(entries[i]);
    }
}

TEST_F(VsetTest, TestVsetLargeBatchSameExpiry) {
    vset set;
    vsetInit(&set);

    const long long expiry_time = 1000LL;
    const int total_entries = 200;

    mock_entry **entries = (mock_entry **)zmalloc(sizeof(mock_entry *) * total_entries);
    ASSERT_NE(entries, nullptr);

    for (int i = 0; i < total_entries; i++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "entry_%d", i);
        entries[i] = mockCreateEntry(key_buf, expiry_time);
        ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[i]));
    }

    ASSERT_FALSE(vsetIsEmpty(&set));

    vsetIterator it;
    vsetInitIterator(&set, &it);

    void *entry;
    int count = 0;
    while (vsetNext(&it, &entry)) {
        ASSERT_NE(entry, nullptr);
        count++;
    }
    ASSERT_EQ(count, total_entries);

    vsetResetIterator(&it);
    vsetRelease(&set);

    for (int i = 0; i < total_entries; i++) {
        mockFreeEntry(entries[i]);
    }
    zfree(entries);
}

TEST_F(VsetTest, TestVsetLargeBatchUpdateEntrySameExpiry) {
    vset set;
    vsetInit(&set);

    const long long expiry_time = 1000LL;
    const unsigned int total_entries = 1000;

    mock_entry *entries[total_entries];

    for (unsigned int i = 0; i < total_entries; i++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "entry_%d", i);
        entries[i] = mockCreateEntry(key_buf, expiry_time);
        ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[i]));
    }
    ASSERT_FALSE(vsetIsEmpty(&set));

    for (unsigned int i = 0; i < total_entries; i++) {
        mock_entry *old_entry = entries[i];
        entries[i] = mockEntryUpdate(entries[i], expiry_time);
        ASSERT_TRUE(vsetUpdateEntry(&set, mockGetExpiry, old_entry, entries[i], expiry_time, expiry_time));
    }

    for (unsigned int i = 0; i < total_entries; i++) {
        ASSERT_TRUE(vsetRemoveEntry(&set, mockGetExpiry, entries[i]));
    }

    ASSERT_TRUE(vsetIsEmpty(&set));

    for (unsigned int i = 0; i < total_entries; i++) {
        mockFreeEntry(entries[i]);
    }
}

TEST_F(VsetTest, TestVsetLargeBatchUpdateEntryMultipleExpiries) {
    const unsigned int total_entries = 1000;

    vset set;
    vsetInit(&set);

    mock_entry *entries[total_entries];

    for (unsigned int i = 0; i < total_entries; i++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "entry_%d", i);
        long long expiry_time = rand() % 10000;
        entries[i] = mockCreateEntry(key_buf, expiry_time);
        ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[i]));
    }
    ASSERT_FALSE(vsetIsEmpty(&set));

    for (unsigned int i = 0; i < total_entries; i++) {
        mock_entry *old_entry = entries[i];
        long long old_expiry = entryGetExpiry(entries[i]);
        long long new_expiry = old_expiry + rand() % 100000;
        entries[i] = mockEntryUpdate(entries[i], new_expiry);
        ASSERT_TRUE(vsetUpdateEntry(&set, mockGetExpiry, old_entry, entries[i], old_expiry, new_expiry));
    }

    for (unsigned int i = 0; i < total_entries; i++) {
        ASSERT_TRUE(vsetRemoveEntry(&set, mockGetExpiry, entries[i]));
    }

    ASSERT_TRUE(vsetIsEmpty(&set));

    for (unsigned int i = 0; i < total_entries; i++) {
        mockFreeEntry(entries[i]);
    }
}

TEST_F(VsetTest, TestVsetIterateMultipleExpiries) {
    const unsigned int total_entries = 5;

    vset set;
    vsetInit(&set);

    mock_entry *entries[total_entries];

    for (unsigned int i = 0; i < total_entries; i++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "entry_%d", i);
        long long expiry_time = rand() % 10000;
        entries[i] = mockCreateEntry(key_buf, expiry_time);
        ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[i]));
    }

    vsetIterator it;
    vsetInitIterator(&set, &it);

    int found[5] = {0};
    int total = 0;

    void *entry;
    while (vsetNext(&it, &entry)) {
        ASSERT_NE(entry, nullptr);
        mock_entry *e = (mock_entry *)entry;

        for (int i = 0; i < 5; i++) {
            if (strcmp(entryGetField(e), entryGetField(entries[i])) == 0) {
                found[i] = 1;
                break;
            }
        }
        total++;
    }

    ASSERT_EQ(total, 5);

    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(found[i]);
    }

    vsetResetIterator(&it);
    vsetRelease(&set);
    for (int i = 0; i < 5; i++) mockFreeEntry(entries[i]);
}

TEST_F(VsetTest, TestVsetAddAndRemoveAll) {
    vset set;
    vsetInit(&set);

    const int total_entries = 130;
    mock_entry *entries[total_entries];
    long long expiry = 5000;

    for (int i = 0; i < total_entries; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%d", i);
        entries[i] = mockCreateEntry(key, expiry);
        ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[i]));
    }

    for (int i = 0; i < total_entries; i++) {
        ASSERT_TRUE(vsetRemoveEntry(&set, mockGetExpiry, entries[i]));
        mockFreeEntry(entries[i]);
    }

    ASSERT_TRUE(vsetIsEmpty(&set));
    vsetRelease(&set);
}

TEST_F(VsetTest, TestVsetRemoveExpireShrink) {
    vset set;
    vsetInit(&set);

    const long long expiry_time = 1000LL;
    const size_t total_entries = 200;

    for (size_t i = 0; i < total_entries; i++) {
        insert_mock_entry_with_expiry(&set, expiry_time);
    }

    ASSERT_FALSE(vsetIsEmpty(&set));
    mstime_t now = expiry_time + 10000;
    size_t count = vsetRemoveExpired(&set, mockGetExpiry, mock_entry_expire, now, mock_entry_count - 1, &now);

    ASSERT_EQ(count, total_entries - 1);

    ASSERT_FALSE(vsetIsEmpty(&set));

    ASSERT_EQ(vsetRemoveExpired(&set, mockGetExpiry, mock_entry_expire, now, mock_entry_count, &now), 1u);

    ASSERT_TRUE(vsetIsEmpty(&set));

    vsetRelease(&set);
}

/* Regression test for the HT-bucket-size-1 invariant violation.
 *
 * vsetBucketRemoveExpired_HASHTABLE() drains an HT-encoded time-bucket up to
 * a caller-supplied quota but only collapses the bucket when it becomes
 * fully empty -- it never downgrades HT -> SINGLE when a single entry
 * survives. This mirrors the production active-expire path
 * (dbReclaimExpiredFields -> hashTypeDeleteExpiredFields -> vsetRemoveExpired),
 * whose per-key quota can stop one entry short of draining the bucket.
 *
 * Once an HT bucket is left holding exactly one entry, removing that entry
 * through the *normal* removal path (vsetRemoveEntry -> removeFromBucket_HASHTABLE,
 * the same path HDEL and a cross-bucket HSETEX update take) deletes the sole
 * entry and trips:
 *
 *     assert(hashtableSize(ht) > 0);   // vset.c, removeFromBucket_HASHTABLE
 *
 * because the size goes 1 -> 0. The production crash hit this via
 * hsetexCommand -> hashTypeSet -> hashTypeTrackUpdateEntry -> vsetUpdateEntry
 * -> vsetBucketUpdateEntry_RAX -> removeEntryFromRaxBucket -> removeFromBucket_HASHTABLE.
 *
 * This test reproduces the precondition deterministically (no timing race):
 * fill one time-bucket past the VECTOR->HT threshold, expire all but one
 * entry, then remove the survivor via vsetRemoveEntry. */
TEST_F(VsetTest, TestVsetRemoveExpiredLeavesHtBucketSizeOne) {
    vset set;
    vsetInit(&set);

    /* 200 > VOLATILESET_VECTOR_BUCKET_MAX_SIZE (127): same expiry forces a
     * single time-bucket that converts to HT encoding. */
    const long long expiry_time = 1000LL;
    const size_t total_entries = 200;

    for (size_t i = 0; i < total_entries; i++) {
        insert_mock_entry_with_expiry(&set, expiry_time);
    }
    ASSERT_FALSE(vsetIsEmpty(&set));

    /* Active-expire style drain that stops one entry short of empty,
     * exactly as the per-key quota does in dbReclaimExpiredFields. This
     * leaves the HT bucket holding a single entry. */
    mstime_t now = expiry_time + 10000;
    size_t count = vsetRemoveExpired(&set, mockGetExpiry, mock_entry_expire, now, mock_entry_count - 1, &now);
    ASSERT_EQ(count, total_entries - 1);
    ASSERT_FALSE(vsetIsEmpty(&set));
    ASSERT_EQ((size_t)mock_entry_count, 1u);

    /* Remove the lone survivor through the normal removal path. With the
     * bug present this hits removeFromBucket_HASHTABLE on a size-1 HT
     * bucket and aborts on assert(hashtableSize(ht) > 0). With the fix
     * (downgrade HT -> SINGLE when one entry remains during expiry) this
     * succeeds and empties the set. */
    mock_entry *survivor = mock_entries[0];
    ASSERT_TRUE(vsetRemoveEntry(&set, mockGetExpiry, survivor));
    mockFreeEntry(survivor);
    mock_entries[0] = mock_entries[--mock_entry_count];

    ASSERT_TRUE(vsetIsEmpty(&set));

    vsetRelease(&set);
}

/* Regression test for signed-overflow in the bucket-timestamp math.
 *
 * get_bucket_ts()/get_max_bucket_ts() round an expiry up to the next
 * 16ms / 8192ms window boundary via `(expiry & ~(INTERVAL-1)) + INTERVAL`.
 * For any expiry inside the top 8192ms window below 2^63 this addition
 * overflows signed long long and wraps negative. The negative value is then
 * used as a RAX bucket key; because RAX keys are compared as unsigned
 * big-endian bytes it sorts *after* every real timestamp, and when a full
 * VECTOR bucket is forced to split, findSplitPosition() returns a positive
 * target while the poisoned bucket_ts is negative, tripping:
 *
 *     assert(target_bucket_ts < bucket_ts);   // vset.c, splitBucketIfPossible
 *
 * Recipe (mirrors the HPEXPIREAT repro): fill one over-2^63 max-window bucket
 * with 126 entries at timestamp A and 1 at a later sub-window B, so the VECTOR
 * holds 127 entries spanning two 16ms sub-windows, then add a 128th entry. The
 * 128th tips the VECTOR over its size limit and forces the vector->rax
 * conversion + split that overflows. Aborts without the fix; with the fix
 * (saturating get_bucket_ts/get_max_bucket_ts) it succeeds. */
TEST_F(VsetTest, TestVsetLargeExpiryBucketOverflow) {
    vset set;
    vsetInit(&set);

    /* A = 2^63 - 8192 (start of the last 8192ms window); B = A + 8000 (same
     * 8192ms window, a different 16ms sub-window). Both are <= LLONG_MAX. */
    const long long A = 9223372036854767616LL; /* 2^63 - 8192 */
    const long long B = 9223372036854775616LL; /* 2^63 - 192  */
    const long long SMALL = 100LL;             /* tiny TTL; must iterate first */
    const long long MAXTS = LLONG_MAX;         /* highest TTL HPEXPIREAT accepts */

    const int total_entries = 131;
    mock_entry *entries[total_entries];
    int n = 0;

    /* 126 entries at A. */
    for (int i = 0; i < 126; i++) {
        char key[32];
        snprintf(key, sizeof(key), "a_%d", i);
        entries[n] = mockCreateEntry(key, A);
        ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[n]));
        n++;
    }
    /* 1 entry at B -> the VECTOR now holds 127 entries across two sub-windows. */
    entries[n] = mockCreateEntry("b_0", B);
    ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[n]));
    n++;

    /* 128th entry at A: tips the VECTOR past 127 and forces the vector->rax
     * conversion + split. This is the operation that aborts without the fix. */
    entries[n] = mockCreateEntry("a_last", A);
    ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[n]));
    n++;

    /* A tiny-TTL entry, added LAST. After the fix the huge-TTL entries live in
     * the saturated LLONG_MAX bucket (the largest RAX key), so this small
     * entry must sort into an earlier bucket and be scanned FIRST. This guards
     * against the overflow re-inverting scan order (a negative/overflowed key
     * would sort the huge-TTL bucket *before* this one). */
    entries[n] = mockCreateEntry("small", SMALL);
    ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[n]));
    n++;

    /* Two entries at exactly LLONG_MAX. get_bucket_ts(LLONG_MAX) ==
     * get_max_bucket_ts(LLONG_MAX) == LLONG_MAX, so their bucket key equals
     * their own expiry -- the one value where the exclusive-end invariant
     * (expiry < bucket_ts) cannot hold. findBucket() seeks strictly-greater-
     * than the expiry, so once the first LLONG_MAX bucket exists the second
     * insert must still resolve it via the inclusive terminal-bucket probe;
     * without it the second insert raxInsert()s over the first (old=NULL) and
     * silently drops it, so the count comes up one short (130, not 131). */
    entries[n] = mockCreateEntry("max_0", MAXTS);
    ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[n]));
    n++;
    entries[n] = mockCreateEntry("max_1", MAXTS);
    ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, entries[n]));
    n++;

    ASSERT_EQ(n, total_entries);

    /* Every entry must be iterable, and the tiny-TTL entry must be scanned
     * first: buckets are walked in ascending bucket_ts order, so its earlier
     * bucket precedes the huge-TTL bucket. Order *within* a bucket is not
     * guaranteed (HT buckets are unordered), so we only assert this
     * cross-bucket property. */
    vsetIterator it;
    vsetInitIterator(&set, &it);
    void *entry;
    int count = 0;
    while (vsetNext(&it, &entry)) {
        ASSERT_NE(entry, nullptr);
        if (count == 0) {
            ASSERT_EQ(mockGetExpiry(entry), SMALL);
        } /* earliest bucket first */
        count++;
    }
    ASSERT_EQ(count, total_entries);
    vsetResetIterator(&it);

    /* Remove every entry, last to first. This exercises findBucket() on the
     * remove path for all expiries -- including the two LLONG_MAX entries,
     * which resolve only through the inclusive terminal-bucket probe (without
     * it removeFromBucket_RAX trips serverAssert(bucket != VSET_NONE_BUCKET_PTR)).
     * The set must be empty afterwards. */
    for (int i = n - 1; i >= 0; i--) {
        ASSERT_TRUE(vsetRemoveEntry(&set, mockGetExpiry, entries[i]));
    }
    ASSERT_TRUE(vsetIsEmpty(&set));

    vsetRelease(&set);
    for (int i = 0; i < total_entries; i++) mockFreeEntry(entries[i]);
}

TEST_F(VsetTest, TestVsetDefrag) {
    srand(time(nullptr));

    vset set;
    vsetInit(&set);

    /* defrag empty set */
    ASSERT_EQ(defrag_vset(&set, 0, 0), 0u);

    /* defrag when single entry */
    insert_mock_entry(&set);
    ASSERT_EQ(defrag_vset(&set, 0, 0), 0u);

    /* defrag when vector */
    for (int i = 0; i < 127 - 1; i++)
        insert_mock_entry(&set);
    ASSERT_EQ(defrag_vset(&set, 0, 0), 0u);

    long long expiry = rand() % 10000 + 100;
    for (int i = 0; i < 127 * 2; i++) {
        insert_mock_entry_with_expiry(&set, expiry);
    }
    ASSERT_EQ(defrag_vset(&set, 0, 0), 0u);

    size_t cursor = 0;
    for (int i = 0; i < 100000; i++) {
        if (i % 100 == 0)
            cursor = defrag_vset(&set, cursor, 100);
        insert_mock_entry_with_expiry(&set, expiry);
    }
    ASSERT_EQ(defrag_vset(&set, 0, 0), 0u);

    vsetRelease(&set);
}

TEST_F(VsetTest, TestVsetFuzzer) {
    srand(time(nullptr));

    vset set;
    vsetInit(&set);

    for (int i = 0; i < 100000; i++) {
        int op = rand() % 5;
        switch (op) {
        case 0:
        case 1:
            insert_mock_entry(&set);
            break;
        case 2:
            update_mock_entry(&set);
            break;
        case 3:
            remove_mock_entry(&set);
            break;
        case 4:
            ASSERT_EQ(defrag_vset(&set, 0, 0), 0u);
            break;
        }

        if (i % 100 == 0) {
            mstime_t now = rand() % 10000;
            expire_mock_entries(&set, now);
        }
    }
    /* now expire all the entries and check that we have no entries left */
    expire_mock_entries(&set, LLONG_MAX);
    ASSERT_TRUE(vsetIsEmpty(&set) && mock_entry_count == 0);
    vsetRelease(&set);
}

TEST_F(VsetTest, TestVsetMemUsage) {
    vset set;

    /* NONE: memory usage should be 0 */
    vsetInit(&set);
    ASSERT_EQ(vsetMemUsage(&set), 0u);

    /* SINGLE: memory usage should be 0 (entry pointer stored inline) */
    insert_mock_entry_with_expiry(&set, 100);
    ASSERT_EQ(vsetMemUsage(&set), 0u);

    /* VECTOR: second entry forces SINGLE → VECTOR, memory now non-zero */
    insert_mock_entry_with_expiry(&set, 200);
    ASSERT_GT(vsetMemUsage(&set), 0u);

    vsetRelease(&set);

    /* RAX with one bucket: 200 entries all with same expiry time
     * overflow the VECTOR limit (127) and land in a single HT-encoded
     * time bucket inside the RAX. */
    vsetInit(&set);
    for (int i = 0; i < 200; i++) {
        insert_mock_entry_with_expiry(&set, 1000LL);
    }
    size_t mem_one_bucket = vsetMemUsage(&set);
    ASSERT_GT(mem_one_bucket, 0u);
    vsetRelease(&set);

    /* RAX with multiple buckets: spread entries across many time windows
     * so the RAX holds several distinct time buckets. */
    vsetInit(&set);
    for (int i = 0; i < 200; i++) {
        long long expiry = 1000LL + i * 10000LL;
        insert_mock_entry_with_expiry(&set, expiry);
    }
    size_t mem_multi_bucket = vsetMemUsage(&set);
    ASSERT_GT(mem_multi_bucket, 0u);
    ASSERT_GT(mem_multi_bucket, mem_one_bucket);

    vsetRelease(&set);
}

TEST_F(VsetTest, TestVsetClear) {
    vset set;
    vsetInit(&set);

    /* Clear an already-empty set: should be safe and leave it empty */
    vsetClear(&set);
    ASSERT_TRUE(vsetIsEmpty(&set));

    /* Clear a set with a couple of entries */
    mock_entry *e1 = mockCreateEntry("clear_key1", 100);
    mock_entry *e2 = mockCreateEntry("clear_key2", 200);
    ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, e1));
    ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, e2));
    ASSERT_FALSE(vsetIsEmpty(&set));
    vsetClear(&set);
    ASSERT_TRUE(vsetIsEmpty(&set));

    /* Entries survive clear — the vset only holds references,
     * the caller still owns and is responsible for the entry objects. */
    ASSERT_EQ(mockGetExpiry(e1), 100);
    ASSERT_EQ(mockGetExpiry(e2), 200);
    mockFreeEntry(e1);
    mockFreeEntry(e2);

    /* Clear a set with many entries (RAX encoding) */
    for (int i = 0; i < 200; i++) {
        insert_mock_entry_with_expiry(&set, 1000LL);
    }
    ASSERT_FALSE(vsetIsEmpty(&set));
    vsetClear(&set);
    ASSERT_TRUE(vsetIsEmpty(&set));

    /* vsetRelease makes vsetIsValid return false */
    ASSERT_TRUE(vsetIsValid(&set));
    vsetRelease(&set);
    ASSERT_FALSE(vsetIsValid(&set));
}

TEST_F(VsetTest, TestVsetIsValid) {
    /* Uninitialized (zeroed stack) set is invalid */
    vset set;
    memset(&set, 0, sizeof(set));
    ASSERT_FALSE(vsetIsValid(&set));

    /* vsetInit produces a valid set */
    vsetInit(&set);
    ASSERT_TRUE(vsetIsValid(&set));

    /* A populated set is still valid */
    mock_entry *e1 = mockCreateEntry("valid_key1", 100);
    ASSERT_TRUE(vsetAddEntry(&set, mockGetExpiry, e1));
    ASSERT_TRUE(vsetIsValid(&set));

    /* Released set is invalid */
    vsetRelease(&set);
    ASSERT_FALSE(vsetIsValid(&set));

    /* Re-initialized after release is valid again */
    vsetInit(&set);
    ASSERT_TRUE(vsetIsValid(&set));

    vsetRelease(&set);
    mockFreeEntry(e1);
}
