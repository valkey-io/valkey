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
