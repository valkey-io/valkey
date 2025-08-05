#include "../vset.h"
#include "../entry.h"
#include "test_help.h"
#include "../zmalloc.h"

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

    mock_entry *entries[total_entries];

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
    mock_entry *entries[total_entries];

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
    mock_entry *entries[total_entries];

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
    mock_entry *entries[total_entries];
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
    size_t size = zmalloc_size(ptr);
    void *newptr = zmalloc(size);
    memcpy(newptr, ptr, size);
    zfree(ptr);
    return newptr;
}

int mock_defrag_rax_node(raxNode **noderef) {
    raxNode *newnode = mock_defragfn(*noderef);
    if (newnode) {
        *noderef = newnode;
        return 1;
    }
    return 0;
}

size_t defrag_vset(vset *set, size_t cursor, size_t steps) {
    if (steps == 0) steps = ULONG_MAX;
    do {
        cursor = vsetScanDefrag(set, cursor, mock_defragfn, mock_defrag_rax_node);
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

/* --------- Defrag Test --------- */
int test_vset_defrag(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
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
    expire_mock_entries(&set, LONG_LONG_MAX);
    TEST_ASSERT(vsetIsEmpty(&set) && mock_entry_count == 0);
    vsetRelease(&set);
    free_mock_entries(); /* Just in case */
    return 0;
}
