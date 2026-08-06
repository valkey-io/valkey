#include "../vset.h"
#include "../entry.h"
#include "test_help.h"
#include "../zmalloc.h"
#include "../allocator_defrag.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

typedef entry mock_entry;

static mock_entry *mockCreateEntry(const char *keystr, long long expiry) {
    sds field = sdsnew(keystr);
    mock_entry *e = entryCreate(field, sdsnew("value"), expiry);
    sdsfree(field);
    return e;
}

static void mockFreeEntry(void *entry) {
    // printf("mockFreeEntry: %p\n", entry);
    entryFree(entry);
}

static mock_entry *mockEntryUpdate(mock_entry *entry, long long expiry) {
    mock_entry *new_entry = entryCreate(entryGetField(entry), sdsdup(entryGetValue(entry)), expiry);
    entryFree(entry);
    return new_entry;
}

static long long mockGetExpiry(const void *entry) {
    return entryGetExpiry(entry);
}

int test_vset_add_and_iterate(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;

    vset set;
    vsetInit(&set);

    mock_entry *e1 = mockCreateEntry("item1", 123);
    mock_entry *e2 = mockCreateEntry("item2", 456);

    TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, e1));
    TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, e2));

    TEST_ASSERT(!vsetIsEmpty(&set));

    vsetIterator it;
    vsetInitIterator(&set, &it);

    void *entry;
    int count = 0;
    while (vsetNext(&it, &entry)) {
        TEST_EXPECT(entry != NULL);
        count++;
    }

    TEST_ASSERT(count == 2);

    vsetResetIterator(&it);
    vsetRelease(&set);
    mockFreeEntry(e1);
    mockFreeEntry(e2);

    TEST_PRINT_INFO("Test passed with %d expects", failed_expects);
    return 0;
}

int test_vset_large_batch_same_expiry(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;

    vset set;
    vsetInit(&set);

    const long long expiry_time = 1000LL;
    const int total_entries = 200;

    // Allocate and add 200 entries with same expiry
    mock_entry **entries = zmalloc(sizeof(mock_entry *) * total_entries);
    TEST_ASSERT(entries != NULL);

    for (int i = 0; i < total_entries; i++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "entry_%d", i);
        entries[i] = mockCreateEntry(key_buf, expiry_time);
        TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, entries[i]));
    }

    // Verify set is not empty
    TEST_ASSERT(!vsetIsEmpty(&set));

    // Iterate all entries and count them
    vsetIterator it;
    vsetInitIterator(&set, &it);

    void *entry;
    int count = 0;
    while (vsetNext(&it, &entry)) {
        TEST_EXPECT(entry != NULL);
        count++;
    }
    TEST_ASSERT(count == total_entries);

    // Cleanup
    vsetResetIterator(&it);
    vsetRelease(&set);

    for (int i = 0; i < total_entries; i++) {
        mockFreeEntry(entries[i]);
    }
    zfree(entries);

    TEST_PRINT_INFO("Inserted and iterated %d entries with same expiry", total_entries);
    return 0;
}

int test_vset_large_batch_update_entry_same_expiry(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;

    vset set;
    vsetInit(&set);

    const long long expiry_time = 1000LL;
    const unsigned int total_entries = 1000;

    mock_entry **entries = zmalloc(sizeof(mock_entry *) * total_entries);
    TEST_ASSERT(entries != NULL);

    for (unsigned int i = 0; i < total_entries; i++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "entry_%d", i);
        entries[i] = mockCreateEntry(key_buf, expiry_time);
        TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, entries[i]));
    }
    // Verify set is not empty
    TEST_ASSERT(!vsetIsEmpty(&set));

    // Now iterate and replace all entries
    for (unsigned int i = 0; i < total_entries; i++) {
        mock_entry *old_entry = entries[i];
        entries[i] = mockEntryUpdate(entries[i], expiry_time);
        TEST_ASSERT(vsetUpdateEntry(&set, mockGetExpiry, old_entry, entries[i], expiry_time, expiry_time));
    }

    for (unsigned int i = 0; i < total_entries; i++) {
        TEST_ASSERT(vsetRemoveEntry(&set, mockGetExpiry, entries[i]));
    }

    // Verify set is empty
    TEST_ASSERT(vsetIsEmpty(&set));

    // Cleanup
    for (unsigned int i = 0; i < total_entries; i++) {
        mockFreeEntry(entries[i]);
    }
    zfree(entries);

    TEST_PRINT_INFO("Inserted, updated and deleted %d entries with same expiry", total_entries);
    return 0;
}

int test_vset_large_batch_update_entry_multiple_expiries(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;
    const unsigned int total_entries = 1000;

    vset set;
    vsetInit(&set);

    // Prepare entries with mixed expiry times, some duplicates
    mock_entry **entries = zmalloc(sizeof(mock_entry *) * total_entries);
    TEST_ASSERT(entries != NULL);

    // Initialize keys
    for (unsigned int i = 0; i < total_entries; i++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "entry_%d", i);
        long long expiry_time = rand() % 10000;
        entries[i] = mockCreateEntry(key_buf, expiry_time);
        TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, entries[i]));
    }
    // Verify set is not empty
    TEST_ASSERT(!vsetIsEmpty(&set));

    // Now iterate and replace all entries
    for (unsigned int i = 0; i < total_entries; i++) {
        mock_entry *old_entry = entries[i];
        long long old_expiry = entryGetExpiry(entries[i]);
        long long new_expiry = old_expiry + rand() % 100000;
        entries[i] = mockEntryUpdate(entries[i], new_expiry);
        TEST_ASSERT(vsetUpdateEntry(&set, mockGetExpiry, old_entry, entries[i], old_expiry, new_expiry));
    }

    for (unsigned int i = 0; i < total_entries; i++) {
        TEST_ASSERT(vsetRemoveEntry(&set, mockGetExpiry, entries[i]));
    }

    // Verify set is empty
    TEST_ASSERT(vsetIsEmpty(&set));

    // Cleanup
    for (unsigned int i = 0; i < total_entries; i++) {
        mockFreeEntry(entries[i]);
    }
    zfree(entries);

    TEST_PRINT_INFO("Inserted, updated and deleted %d entries with different expiry", total_entries);
    return 0;
}

int test_vset_iterate_multiple_expiries(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;
    const unsigned int total_entries = 5;

    vset set;
    vsetInit(&set);

    // Prepare entries with mixed expiry times, some duplicates
    mock_entry **entries = zmalloc(sizeof(mock_entry *) * total_entries);
    TEST_ASSERT(entries != NULL);

    // Initialize keys
    for (unsigned int i = 0; i < total_entries; i++) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "entry_%d", i);
        long long expiry_time = rand() % 10000;
        entries[i] = mockCreateEntry(key_buf, expiry_time);
        TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, entries[i]));
    }

    vsetIterator it;
    vsetInitIterator(&set, &it);

    int found[5] = {0};
    int total = 0;

    void *entry;
    while (vsetNext(&it, &entry)) {
        TEST_EXPECT(entry != NULL);
        mock_entry *e = (mock_entry *)entry;

        // Match the entries we inserted
        for (int i = 0; i < 5; i++) {
            if (strcmp(entryGetField(e), entryGetField(entries[i])) == 0) {
                found[i] = 1;
                break;
            }
        }
        total++;
    }

    TEST_ASSERT(total == 5);

    for (int i = 0; i < 5; i++) {
        TEST_EXPECT(found[i]);
    }

    vsetResetIterator(&it);
    vsetRelease(&set);
    for (int i = 0; i < 5; i++) mockFreeEntry(entries[i]);
    zfree(entries);

    TEST_PRINT_INFO("Iterated all %d mixed expiry entries successfully", total);
    return 0;
}

int test_vset_add_and_remove_all(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    vset set;
    vsetInit(&set);

    const int total_entries = 130;
    mock_entry **entries = zmalloc(sizeof(mock_entry *) * total_entries);
    TEST_ASSERT(entries != NULL);
    long long expiry = 5000;

    for (int i = 0; i < total_entries; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key_%d", i);
        entries[i] = mockCreateEntry(key, expiry);
        TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, entries[i]));
    }

    for (int i = 0; i < total_entries; i++) {
        TEST_ASSERT(vsetRemoveEntry(&set, mockGetExpiry, entries[i]));
        mockFreeEntry(entries[i]);
    }
    zfree(entries);

    TEST_ASSERT(vsetIsEmpty(&set));
    vsetRelease(&set);

    TEST_PRINT_INFO("Add/remove %d entries, set size now 0", total_entries);
    return 0;
}

/********************* Fuzzer tests ********************************/

#define NUM_ITERATIONS 100000
#define MAX_ENTRIES 10000
#define NUM_DEFRAG_STEPS 100

/* Global array to simulate a test database */
mock_entry *mock_entries[MAX_ENTRIES];
int mock_entry_count = 0;

/* --------- volatileEntryType Callbacks --------- */
sds mock_entry_get_key(const void *entry) {
    return (sds)entry;
}

long long mock_entry_get_expiry(const void *entry) {
    return mockGetExpiry(entry);
}

int mock_entry_expire(void *entry, void *ctx) {
    mock_entry *e = (mock_entry *)entry;
    long long now = *(long long *)ctx;
    TEST_ASSERT(mock_entry_get_expiry(entry) <= now);
    for (int i = 0; i < mock_entry_count; i++) {
        if (mock_entries[i] == e) {
            // printf("expire entry %p with expiry %llu\n", e, mockGetExpiry(e));
            mockFreeEntry(e);
            mock_entries[i] = mock_entries[--mock_entry_count];
            return 1;
        }
    }
    return 0;
}

/* --------- Helper Functions --------- */
mock_entry *mock_entry_create(const char *keystr, long long expiry) {
    return mockCreateEntry(keystr, expiry);
}

int insert_mock_entry(vset *set) {
    if (mock_entry_count >= MAX_ENTRIES) return 0;
    char keybuf[32];
    snprintf(keybuf, sizeof(keybuf), "key_%d", mock_entry_count);

    long long expiry = rand() % 10000 + 100;
    mock_entry *e = mock_entry_create(keybuf, expiry);
    // printf("adding entry %p with expiry %llu\n", e, expiry);
    TEST_ASSERT(vsetAddEntry(set, mockGetExpiry, e));
    mock_entries[mock_entry_count++] = e;
    return 0;
}

int insert_mock_entry_with_expiry(vset *set, long long expiry) {
    if (mock_entry_count >= MAX_ENTRIES) return 0;
    char keybuf[32];
    snprintf(keybuf, sizeof(keybuf), "key_%d", mock_entry_count);

    mock_entry *e = mock_entry_create(keybuf, expiry);
    // printf("adding entry %p with expiry %llu\n", e, expiry);
    TEST_ASSERT(vsetAddEntry(set, mockGetExpiry, e));
    mock_entries[mock_entry_count++] = e;
    return 0;
}

int update_mock_entry(vset *set) {
    if (mock_entry_count == 0) return 0;
    int idx = rand() % mock_entry_count;
    mock_entry *old = mock_entries[idx];
    long long old_expiry = mockGetExpiry(old);
    long long new_expiry = old_expiry + (rand() % 500);
    mock_entry *updated = mockEntryUpdate(old, new_expiry);
    mock_entries[idx] = updated;
    // printf("Update entry %p with entry %p with old expiry %llu new expiry %llu\n", old, updated, old_expiry, new_expiry);
    TEST_ASSERT(vsetUpdateEntry(set, mockGetExpiry, old, updated, old_expiry, new_expiry));
    return 0;
}

int remove_mock_entry(vset *set) {
    if (mock_entry_count == 0) return 0;
    int idx = rand() % mock_entry_count;
    mock_entry *e = mock_entries[idx];
    // printf("removing entry %p with expiry %llu\n", e, mockGetExpiry(e));
    TEST_ASSERT(vsetRemoveEntry(set, mockGetExpiry, e));
    mockFreeEntry(e);
    mock_entries[idx] = mock_entries[--mock_entry_count];

    return 0;
}


int expire_mock_entries(vset *set, mstime_t now) {
    // printf("Before expired entries entries: %d\n", mock_entry_count);
    vsetRemoveExpired(set, mockGetExpiry, mock_entry_expire, now, mock_entry_count, &now);
    // printf("After expired %zu entries left entries: %d and set is empty: %s\n", count, mock_entry_count, vsetIsEmpty(set) ? "true" : "false");
    return 0;
}

void *mock_defragfn(void *ptr) {
    size_t size = zmalloc_usable_size(ptr);
    void *newptr = zmalloc(size);
    memcpy(newptr, ptr, size);
    zfree(ptr);
    return newptr;
}

size_t defrag_vset(vset *set, size_t cursor, size_t steps) {
    if (steps == 0) steps = ULONG_MAX;
    do {
        cursor = vsetScanDefrag(set, cursor, mock_defragfn);
        steps--;
    } while (cursor != 0 && steps > 0);
    return cursor;
}

int free_mock_entries(void) {
    for (int i = 0; i < mock_entry_count; i++) {
        mock_entry *e = mock_entries[i];
        mockFreeEntry(e);
    }
    mock_entry_count = 0;
    return 0;
}

int test_vset_remove_expire_shrink(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;

    vset set;
    vsetInit(&set);

    const long long expiry_time = 1000LL;
    const size_t total_entries = 200;

    for (size_t i = 0; i < total_entries; i++) {
        insert_mock_entry_with_expiry(&set, expiry_time);
    }

    // Verify set is not empty
    TEST_ASSERT(!vsetIsEmpty(&set));
    mstime_t now = expiry_time + 10000;
    size_t count = vsetRemoveExpired(&set, mockGetExpiry, mock_entry_expire, now, mock_entry_count - 1, &now);

    TEST_ASSERT(count == total_entries - 1);

    // Verify set is not empty
    TEST_ASSERT(!vsetIsEmpty(&set));

    // Now complete the expiration
    TEST_ASSERT(vsetRemoveExpired(&set, mockGetExpiry, mock_entry_expire, now, mock_entry_count, &now) == 1);

    // Verify set is empty
    TEST_ASSERT(vsetIsEmpty(&set));

    vsetRelease(&set);
    free_mock_entries();
    return 0;
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
int test_vset_remove_expired_leaves_ht_bucket_size_one(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;

    vset set;
    vsetInit(&set);

    /* 200 > VOLATILESET_VECTOR_BUCKET_MAX_SIZE (127): same expiry forces a
     * single time-bucket that converts to HT encoding. */
    const long long expiry_time = 1000LL;
    const size_t total_entries = 200;

    for (size_t i = 0; i < total_entries; i++) {
        insert_mock_entry_with_expiry(&set, expiry_time);
    }
    TEST_ASSERT(!vsetIsEmpty(&set));

    /* Active-expire style drain that stops one entry short of empty,
     * exactly as the per-key quota does in dbReclaimExpiredFields. This
     * leaves the HT bucket holding a single entry. */
    mstime_t now = expiry_time + 10000;
    size_t count = vsetRemoveExpired(&set, mockGetExpiry, mock_entry_expire, now, mock_entry_count - 1, &now);
    TEST_ASSERT(count == total_entries - 1);
    TEST_ASSERT(!vsetIsEmpty(&set));
    TEST_ASSERT((size_t)mock_entry_count == 1);

    /* Remove the lone survivor through the normal removal path. With the
     * bug present this hits removeFromBucket_HASHTABLE on a size-1 HT
     * bucket and aborts on assert(hashtableSize(ht) > 0). With the fix
     * (downgrade HT -> SINGLE when one entry remains during expiry) this
     * succeeds and empties the set. */
    mock_entry *survivor = mock_entries[0];
    TEST_ASSERT(vsetRemoveEntry(&set, mockGetExpiry, survivor));
    mockFreeEntry(survivor);
    mock_entries[0] = mock_entries[--mock_entry_count];

    TEST_ASSERT(vsetIsEmpty(&set));

    vsetRelease(&set);
    free_mock_entries();
    return 0;
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
int test_vset_large_expiry_bucket_overflow(int argc, char **argv, int flags) {
    (void)argc;
    (void)argv;
    (void)flags;

    vset set;
    vsetInit(&set);

    /* A = 2^63 - 8192 (start of the last 8192ms window); B = A + 8000 (same
     * 8192ms window, a different 16ms sub-window). Both are <= LLONG_MAX. */
    const long long A = 9223372036854767616LL; /* 2^63 - 8192 */
    const long long B = 9223372036854775616LL; /* 2^63 - 192  */
    const long long SMALL = 100LL;             /* tiny TTL; must iterate first */
    const long long MAXTS = LLONG_MAX;         /* highest TTL HPEXPIREAT accepts */

    const int total_entries = 131;
    mock_entry **entries = zmalloc(sizeof(mock_entry *) * total_entries);
    TEST_ASSERT(entries != NULL);
    int n = 0;

    /* 126 entries at A. */
    for (int i = 0; i < 126; i++) {
        char key[32];
        snprintf(key, sizeof(key), "a_%d", i);
        entries[n] = mockCreateEntry(key, A);
        TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, entries[n]));
        n++;
    }
    /* 1 entry at B -> the VECTOR now holds 127 entries across two sub-windows. */
    entries[n] = mockCreateEntry("b_0", B);
    TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, entries[n]));
    n++;

    /* 128th entry at A: tips the VECTOR past 127 and forces the vector->rax
     * conversion + split. This is the operation that aborts without the fix. */
    entries[n] = mockCreateEntry("a_last", A);
    TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, entries[n]));
    n++;

    /* A tiny-TTL entry, added LAST. After the fix the huge-TTL entries live in
     * the saturated LLONG_MAX bucket (the largest RAX key), so this small
     * entry must sort into an earlier bucket and be scanned FIRST. This guards
     * against the overflow re-inverting scan order (a negative/overflowed key
     * would sort the huge-TTL bucket *before* this one). */
    entries[n] = mockCreateEntry("small", SMALL);
    TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, entries[n]));
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
    TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, entries[n]));
    n++;
    entries[n] = mockCreateEntry("max_1", MAXTS);
    TEST_ASSERT(vsetAddEntry(&set, mockGetExpiry, entries[n]));
    n++;

    TEST_ASSERT(n == total_entries);

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
        TEST_ASSERT(entry != NULL);
        if (count == 0) {
            TEST_ASSERT(mockGetExpiry(entry) == SMALL);
        } /* earliest bucket first */
        count++;
    }
    TEST_ASSERT(count == total_entries);
    vsetResetIterator(&it);

    /* Remove every entry, last to first. This exercises findBucket() on the
     * remove path for all expiries -- including the two LLONG_MAX entries,
     * which resolve only through the inclusive terminal-bucket probe (without
     * it removeFromBucket_RAX trips serverAssert(bucket != VSET_NONE_BUCKET_PTR)).
     * The set must be empty afterwards. */
    for (int i = n - 1; i >= 0; i--) {
        TEST_ASSERT(vsetRemoveEntry(&set, mockGetExpiry, entries[i]));
    }
    TEST_ASSERT(vsetIsEmpty(&set));

    vsetRelease(&set);
    for (int i = 0; i < total_entries; i++) mockFreeEntry(entries[i]);
    zfree(entries);
    return 0;
}

/* --------- Defrag Test --------- */
int test_vset_defrag(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    allocatorDefragInit();
    srand(time(NULL));

    vset set;
    vsetInit(&set);

    /* defrag empty set */
    TEST_ASSERT(defrag_vset(&set, 0, 0) == 0);

    /* defrag when single entry */
    insert_mock_entry(&set);
    TEST_ASSERT(defrag_vset(&set, 0, 0) == 0);

    /* defrag when vector */
    for (int i = 0; i < 127 - 1; i++)
        insert_mock_entry(&set);
    TEST_ASSERT(defrag_vset(&set, 0, 0) == 0);

    long long expiry = rand() % 10000 + 100;
    for (int i = 0; i < 127 * 2; i++) {
        insert_mock_entry_with_expiry(&set, expiry);
    }
    TEST_ASSERT(defrag_vset(&set, 0, 0) == 0);

    size_t cursor = 0;
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        if (i % NUM_DEFRAG_STEPS == 0)
            cursor = defrag_vset(&set, cursor, NUM_DEFRAG_STEPS);
        insert_mock_entry_with_expiry(&set, expiry);
    }
    TEST_ASSERT(defrag_vset(&set, 0, 0) == 0);

    vsetRelease(&set);
    free_mock_entries();

    return 0;
}

/* --------- Fuzzer Test --------- */
int test_vset_fuzzer(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    srand(time(NULL));

    vset set;
    vsetInit(&set);

    for (int i = 0; i < NUM_ITERATIONS; i++) {
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
            TEST_ASSERT(defrag_vset(&set, 0, 0) == 0);
            break;
        }

        if (i % 100 == 0) {
            mstime_t now = rand() % 10000;
            expire_mock_entries(&set, now);
        }
    }
    /* now expire all the entries and check that we have no entries left */
    expire_mock_entries(&set, LLONG_MAX);
    TEST_ASSERT(vsetIsEmpty(&set) && mock_entry_count == 0);
    vsetRelease(&set);
    free_mock_entries(); /* Just in case */
    return 0;
}
