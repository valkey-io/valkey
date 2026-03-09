/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "entry.h"
#include "expire.h"
#include "fmacros.h"
#include "monotonic.h"
#include "server.h"
}

/* Constants for test values */
#define SHORT_FIELD "foo"
#define SHORT_VALUE "bar"
#define LONG_FIELD "k:123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
#define LONG_VALUE "v:12345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"

class EntryTest : public ::testing::Test {
  protected:
    /* Verify entry properties */
    void verify_entry_properties(entry *e, sds field, sds value_copy, long long expiry, bool has_expiry, bool has_valueptr) {
        ASSERT_EQ(sdscmp(entryGetField(e), field), 0);
        size_t len;
        ASSERT_EQ(sdscmp(entryGetValue(e, &len), value_copy), 0);
        ASSERT_EQ(len, sdslen(value_copy));
        ASSERT_EQ(entryGetExpiry(e), expiry);
        ASSERT_EQ(entryHasExpiry(e), has_expiry);
        ASSERT_EQ(entryHasEmbeddedValue(e), !has_valueptr);
    }
};

/**
 * Test entryCreate functionality:
 * 1. embedded with expiry
 * 2. embedded without expiry
 * 3. non-embedded with expiry
 * 4. non-embedded without expiry
 */
TEST_F(EntryTest, entryCreate) {
    // Test with embedded value with expiry
    sds field1 = sdsnew(SHORT_FIELD);
    sds value1 = sdsnew(SHORT_VALUE);
    sds value_copy1 = sdsdup(value1); // Keep a copy since entryCreate takes ownership of value
    long long expiry1 = 100;
    entry *e1 = entryCreate(field1, value1, expiry1);
    verify_entry_properties(e1, field1, value_copy1, expiry1, true, false);

    // Test with embedded value with no expiry
    sds field2 = sdsnew(SHORT_FIELD);
    sds value2 = sdsnew(SHORT_VALUE);
    sds value_copy2 = sdsdup(value2);
    long long expiry2 = EXPIRY_NONE;
    entry *e2 = entryCreate(field2, value2, expiry2);
    verify_entry_properties(e2, field2, value_copy2, expiry2, false, false);

    // Test with non-embedded field and value with expiry
    sds field3 = sdsnew(LONG_FIELD);
    sds value3 = sdsnew(LONG_VALUE);
    sds value_copy3 = sdsdup(value3);
    long long expiry3 = 100;
    entry *e3 = entryCreate(field3, value3, expiry3);
    verify_entry_properties(e3, field3, value_copy3, expiry3, true, true);

    // Test with non-embedded field and value with no expiry
    sds field4 = sdsnew(LONG_FIELD);
    sds value4 = sdsnew(LONG_VALUE);
    sds value_copy4 = sdsdup(value4);
    long long expiry4 = EXPIRY_NONE;
    entry *e4 = entryCreate(field4, value4, expiry4);
    verify_entry_properties(e4, field4, value_copy4, expiry4, false, true);

    entryFree(e1);
    entryFree(e2);
    entryFree(e3);
    entryFree(e4);

    // Free field as entryCreate doesn't take ownership
    sdsfree(field1);
    sdsfree(field2);
    sdsfree(field3);
    sdsfree(field4);

    sdsfree(value_copy1);
    sdsfree(value_copy2);
    sdsfree(value_copy3);
    sdsfree(value_copy4);
}

/**
 * Test entryUpdate with various combinations of value and expiry changes:
 * 1. Update only the value (keeping embedded)
 * 2. Update only the expiry (keeping embedded)
 * 3. Update both value and expiry (keeping embedded)
 * 4. Update with no changes (should return same entry)
 * 5. Update to a value that's too large to be embedded
 * 6. Update expiry of a non-embedded entry
 * 7. Update from non-embedded back to embedded value
 * 8. Update entry to less then 3/4 allocation size
 * 9. Update entry to more than 3/4 allocation size
 * 8. Update entry to exactly 3/4 allocation size
 */
TEST_F(EntryTest, entryUpdate) {
    // Create embedded entry
    sds value1 = sdsnew(SHORT_VALUE);
    sds field = sdsnew(SHORT_FIELD);
    sds value_copy1 = sdsdup(value1);
    long long expiry1 = 100;
    entry *e1 = entryCreate(field, value1, expiry1);
    verify_entry_properties(e1, field, value_copy1, expiry1, true, false);

    // Update only value (keeping embedded)
    sds value2 = sdsnew("bar2");
    sds value_copy2 = sdsdup(value2);
    long long expiry2 = expiry1;
    entry *e2 = entryUpdate(e1, value2, expiry2);
    verify_entry_properties(e2, field, value_copy2, expiry2, true, false);

    // Update only expiry (keeping embedded)
    long long expiry3 = 200;
    entry *e3 = entryUpdate(e2, nullptr, expiry3);
    verify_entry_properties(e3, field, value_copy2, expiry3, true, false);

    // Update both value and expiry (keeping embedded)
    sds value4 = sdsnew("bar4");
    long long expiry4 = 300;
    sds value_copy4 = sdsdup(value4);
    entry *e4 = entryUpdate(e3, value4, expiry4);
    verify_entry_properties(e4, field, value_copy4, expiry4, true, false);

    // Update with no changes (should return same entry)
    entry *e5 = entryUpdate(e4, nullptr, expiry4);
    verify_entry_properties(e5, field, value_copy4, expiry4, true, false);
    ASSERT_EQ(e5, e4);

    // Update to a value that's too large to be embedded
    sds value6 = sdsnew(LONG_VALUE);
    sds value_copy6 = sdsdup(value6);
    long long expiry6 = expiry4;
    entry *e6 = entryUpdate(e5, value6, expiry6);
    verify_entry_properties(e6, field, value_copy6, expiry6, true, true);

    // Update expiry of a non-embedded entry
    long long expiry7 = 400;
    entry *e7 = entryUpdate(e6, nullptr, expiry7);
    verify_entry_properties(e7, field, value_copy6, expiry7, true, true);

    // Update from non-embedded back to embedded value
    sds value8 = sdsnew("bar8");
    sds value_copy8 = sdsdup(value8);
    long long expiry8 = expiry7;
    entry *e8 = entryUpdate(e7, value8, expiry8);
    verify_entry_properties(e8, field, value_copy8, expiry8, true, false);

    // Update value with identical value (keeping embedded)
    sds value9 = sdsnew("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    sds value_copy9 = sdsdup(value9);
    long long expiry9 = expiry8;
    entry *e9 = entryUpdate(e8, value9, expiry9);
    verify_entry_properties(e9, field, value_copy9, expiry9, true, false);

    // Update the value so that memory usage is less than 3/4 of the current allocation size
    // Ensuring required_embedded_size < current_embedded_allocation_size * 3 / 4, which creates a new entry
    size_t current_embedded_allocation_size = entryMemUsage(e9);
    sds value10 = sdsnew("xxxxxxxxxxxxxxxxxxxxx");
    sds value_copy10 = sdsdup(value10);
    long long expiry10 = expiry9;
    entry *e10 = entryUpdate(e9, value10, expiry10);
    verify_entry_properties(e10, field, value_copy10, expiry10, true, false);
    ASSERT_LT(entryMemUsage(e10), current_embedded_allocation_size * 3 / 4);
    ASSERT_NE(e10, e9);

    // Update the value so that memory usage is at least 3/4 of the current memory usage
    // Ensuring required_embedded_size > current_embedded_allocation_size * 3 / 4 without creating a new entry
    current_embedded_allocation_size = entryMemUsage(e10);
    sds value11 = sdsnew("yyyyyyyyyyyyy");
    sds value_copy11 = sdsdup(value11);
    long long expiry11 = expiry10;
    entry *e11 = entryUpdate(e10, value11, expiry11);
    verify_entry_properties(e11, field, value_copy11, expiry11, true, false);
    ASSERT_GE(entryMemUsage(e11), current_embedded_allocation_size * 3 / 4);
    ASSERT_LE(entryMemUsage(e11), current_embedded_allocation_size);
    ASSERT_LE(entryMemUsage(e11), (size_t)EMBED_VALUE_MAX_ALLOC_SIZE);
    ASSERT_EQ(e10, e11);

    // Update the value so that memory usage is exactly equal to the current allocation size
    // Ensuring required_embedded_size == current_embedded_allocation_size without creating a new entry
    current_embedded_allocation_size = entryMemUsage(e11);
    sds value12 = sdsnew("zzzzzzzzzzzzz");
    sds value_copy12 = sdsdup(value12);
    long long expiry12 = expiry11;
    entry *e12 = entryUpdate(e11, value12, expiry12);
    verify_entry_properties(e11, field, value_copy12, expiry12, true, false);
    ASSERT_EQ(entryMemUsage(e12), current_embedded_allocation_size);
    ASSERT_LE(entryMemUsage(e12), (size_t)EMBED_VALUE_MAX_ALLOC_SIZE);
    ASSERT_EQ(e12, e11);

    entryFree(e12);
    sdsfree(field);
    sdsfree(value_copy1);
    sdsfree(value_copy2);
    sdsfree(value_copy4);
    sdsfree(value_copy6);
    sdsfree(value_copy8);
    sdsfree(value_copy9);
    sdsfree(value_copy10);
    sdsfree(value_copy11);
    sdsfree(value_copy12);
}

/**
 * Test setting expiry on an entry:
 * 1. No expiry
 * 2. Set expiry on entry without expiry
 * 3. Update expiry on entry with expiry
 * 4. Test with non-embedded entry
 * 5. Set expiry on non-embedded entry
 */
TEST_F(EntryTest, entryHasexpiry_entrySetExpiry) {
    // No expiry
    sds field1 = sdsnew(SHORT_FIELD);
    sds value1 = sdsnew(SHORT_VALUE);
    entry *e1 = entryCreate(field1, value1, EXPIRY_NONE);
    ASSERT_FALSE(entryHasExpiry(e1));
    ASSERT_EQ(entryGetExpiry(e1), EXPIRY_NONE);

    // Set expiry on entry without expiry
    long long expiry2 = 100;
    entry *e2 = entrySetExpiry(e1, expiry2);
    ASSERT_TRUE(entryHasExpiry(e2));
    ASSERT_EQ(entryGetExpiry(e2), expiry2);

    // Update expiry on entry with expiry
    long long expiry3 = 200;
    entry *e3 = entrySetExpiry(e2, expiry3);
    ASSERT_TRUE(entryHasExpiry(e3));
    ASSERT_EQ(entryGetExpiry(e3), expiry3);
    ASSERT_EQ(e2, e3); // Should be the same pointer when just updating expiry

    // Test with non-embedded entry
    sds field4 = sdsnew(LONG_FIELD);
    sds value4 = sdsnew(LONG_VALUE);
    entry *e4 = entryCreate(field4, value4, EXPIRY_NONE);
    ASSERT_FALSE(entryHasExpiry(e4));
    ASSERT_FALSE(entryHasEmbeddedValue(e4));

    // Set expiry on entry without expiry
    long long expiry5 = 100;
    entry *e5 = entrySetExpiry(e4, expiry5);
    ASSERT_TRUE(entryHasExpiry(e5));
    ASSERT_EQ(entryGetExpiry(e5), expiry5);

    // Update expiry on entry with expiry
    long long expiry6 = 200;
    entry *e6 = entrySetExpiry(e5, expiry6);
    ASSERT_TRUE(entryHasExpiry(e6));
    ASSERT_EQ(entryGetExpiry(e6), expiry6);
    ASSERT_EQ(e5, e6); // Should be the same pointer when just updating expiry

    entryFree(e3);
    entryFree(e6);
    sdsfree(field1);
    sdsfree(field4);
}

/**
 * Test entryIsExpired:
 * 1. No expiry
 * 2. Future expiry
 * 3. Current time expiry
 * 4. Past expiry
 * 5. Test with loading mode
 * 6. Test with import mode and import source client
 * 7. Test with import mode and import source client and import expiry
 * 8. Test with import mode and import source client and import expiry and import expiry is in the past
 */
TEST_F(EntryTest, entryIsExpired) {
    // Setup server state
    enterExecutionUnit(1, ustime());
    long long current_time = commandTimeSnapshot();

    // No expiry
    sds field1 = sdsnew(SHORT_FIELD);
    sds value1 = sdsnew(SHORT_VALUE);
    entry *e1 = entryCreate(field1, value1, EXPIRY_NONE);
    ASSERT_EQ(entryGetExpiry(e1), EXPIRY_NONE);
    ASSERT_FALSE(entryIsExpired(e1));

    // Future expiry
    sds field2 = sdsnew(SHORT_FIELD);
    sds value2 = sdsnew(SHORT_VALUE);
    long long future_time = current_time + 10000; // 10 seconds in future
    entry *e2 = entryCreate(field2, value2, future_time);
    ASSERT_EQ(entryGetExpiry(e2), future_time);
    ASSERT_FALSE(entryIsExpired(e2));

    // Current time expiry
    sds field3 = sdsnew(SHORT_FIELD);
    sds value3 = sdsnew(SHORT_VALUE);
    entry *e3 = entryCreate(field3, value3, current_time);
    ASSERT_EQ(entryGetExpiry(e3), current_time);
    ASSERT_FALSE(entryIsExpired(e3));

    // Test with past expiry
    sds field4 = sdsnew(SHORT_FIELD);
    sds value4 = sdsnew(SHORT_VALUE);
    long long past_time = current_time - 10000; // 10 seconds ago
    entry *e4 = entryCreate(field4, value4, past_time);
    ASSERT_EQ(entryGetExpiry(e4), past_time);
    ASSERT_TRUE(entryIsExpired(e4));

    entryFree(e1);
    entryFree(e2);
    entryFree(e3);
    entryFree(e4);
    sdsfree(field1);
    sdsfree(field2);
    sdsfree(field3);
    sdsfree(field4);
    exitExecutionUnit();
}

/**
 * Test entryMemUsage:
 * 1. Embedded entry tests:
 *    - Initial creation without expiry
 *    - Adding expiry (should increase memory usage)
 *    - Updating expiry (should not change memory usage)
 *    - Updating value while keeping it embedded:
 *      * To smaller value (should not decrease memory usage)
 *      * To bigger value (should not increase memory usage)
 *
 * 2. Non-embedded entry tests:
 *    - Initial creation without expiry
 *    - Adding expiry (should increase memory usage)
 *    - Updating expiry (should not change memory usage)
 *    - Updating value:
 *      * To smaller value (should decrease memory usage)
 *      * To bigger value (should increase memory usage)
 */
TEST_F(EntryTest, entryMemUsage_entrySetExpiry_entryUpdate) {
    // Tests with embedded entry
    // Embedded entry without expiry
    sds field1 = sdsnew(SHORT_FIELD);
    sds value1 = sdsnew(SHORT_VALUE);
    sds value_copy1 = sdsdup(value1);
    long long expiry1 = EXPIRY_NONE;
    entry *e1 = entryCreate(field1, value1, expiry1);
    size_t e1_entryMemUsage = entryMemUsage(e1);
    verify_entry_properties(e1, field1, value_copy1, expiry1, false, false);
    ASSERT_GT(e1_entryMemUsage, 0u);

    // Add expiry to embedded entry without expiry
    // This should increase memory usage by sizeof(long long) + 2 bytes
    // (long long for the expiry value, 2 bytes for SDS header adjustment)
    long long expiry2 = 100;
    entry *e2 = entrySetExpiry(e1, expiry2);
    size_t e2_entryMemUsage = entryMemUsage(e2);
    verify_entry_properties(e2, field1, value_copy1, expiry2, true, false);
    ASSERT_EQ(zmalloc_usable_size((char *)e2 - sizeof(long long) - 3), e2_entryMemUsage);

    // Update expiry on an entry that already has one
    // This should NOT change memory usage as we're just updating the expiry value (long long)
    long long expiry3 = 10000;
    entry *e3 = entrySetExpiry(e2, expiry3);
    size_t e3_entryMemUsage = entryMemUsage(e3);
    verify_entry_properties(e3, field1, value_copy1, expiry3, true, false);
    ASSERT_EQ(e3_entryMemUsage, e2_entryMemUsage);

    // Update to smaller value (keeping embedded)
    // Memory usage should decrease by the difference in value size (2 bytes)
    sds value4 = sdsnew("x");
    sds value_copy4 = sdsdup(value4);
    entry *e4 = entryUpdate(e3, value4, entryGetExpiry(e3));
    size_t e4_entryMemUsage = entryMemUsage(e4);
    verify_entry_properties(e4, field1, value_copy4, expiry3, true, false);
    ASSERT_EQ(zmalloc_usable_size((char *)e4 - sizeof(long long) - 3), e4_entryMemUsage);

    // Update to bigger value (keeping embedded)
    // Memory usage should increase by the difference in value size (1 byte)
    sds value5 = sdsnew("xx");
    sds value_copy5 = sdsdup(value5);
    entry *e5 = entryUpdate(e4, value5, entryGetExpiry(e4));
    size_t e5_entryMemUsage = entryMemUsage(e5);
    verify_entry_properties(e5, field1, value_copy5, expiry3, true, false);
    ASSERT_EQ(zmalloc_usable_size((char *)e5 - sizeof(long long) - 3), e5_entryMemUsage);

    // Tests with non-embedded entry
    // Non-embedded entry without expiry
    sds field6 = sdsnew(LONG_FIELD);
    field6 = sdscat(field6, LONG_FIELD); // Double the length to ensure non-embedded entry
    sds value6 = sdsnew(LONG_VALUE);
    sds value_copy6 = sdsdup(value6);
    long long expiry6 = EXPIRY_NONE;
    entry *e6 = entryCreate(field6, value6, EXPIRY_NONE);
    size_t e6_entryMemUsage = entryMemUsage(e6);
    verify_entry_properties(e6, field6, value_copy6, expiry6, false, true);
    ASSERT_GT(e6_entryMemUsage, 0u);

    // Add expiry to non-embedded entry without expiry
    // For non-embedded entries this increases memory by exactly sizeof(long long)
    long long expiry7 = 100;
    entry *e7 = entrySetExpiry(e6, expiry7);
    size_t e7_entryMemUsage = entryMemUsage(e7);
    verify_entry_properties(e7, field6, value_copy6, expiry7, true, true);
    size_t expected_e7_entry_mem = zmalloc_usable_size((char *)e7 - sizeof(long long) - sizeof(sds) - 3) + sdsAllocSize(value6);
    ASSERT_EQ(expected_e7_entry_mem, e7_entryMemUsage);

    // Update expiry on a non-embedded entry that already has one
    // This should not change memory usage as we're just updating the expiry value
    long long expiry8 = 10000;
    entry *e8 = entrySetExpiry(e7, expiry8);
    size_t e8_entryMemUsage = entryMemUsage(e8);
    verify_entry_properties(e8, field6, value_copy6, expiry8, true, true);
    ASSERT_EQ(e8_entryMemUsage, e7_entryMemUsage);

    // Update to smaller value (keeping non-embedded)
    // Memory usage should increase by at least the difference between LONG_VALUE and "x" (143)
    sds value9 = sdsnew("x");
    sds value_copy9 = sdsdup(value9);
    entry *e9 = entryUpdate(e8, value9, entryGetExpiry(e8));
    size_t e9_entryMemUsage = entryMemUsage(e9);
    verify_entry_properties(e9, field6, value_copy9, expiry8, true, true);
    size_t expected_e9_entry_mem = zmalloc_usable_size((char *)e9 - sizeof(long long) - sizeof(sds) - 3) + sdsAllocSize(value9);
    ASSERT_EQ(expected_e9_entry_mem, e9_entryMemUsage);

    // Update to bigger value (keeping non-embedded)
    // Memory usage increases by the difference in value size (1 byte)
    sds value10 = sdsnew("xx");
    sds value_copy10 = sdsdup(value10);
    entry *e10 = entryUpdate(e9, value10, entryGetExpiry(e9));
    size_t e10_entryMemUsage = entryMemUsage(e10);
    size_t expected_10_entry_mem = zmalloc_usable_size((char *)e10 - sizeof(long long) - sizeof(sds) - 3) + sdsAllocSize(value10);
    ASSERT_EQ(expected_10_entry_mem, e10_entryMemUsage);

    entryFree(e5);
    entryFree(e10);
    sdsfree(field1);
    sdsfree(field6);
    sdsfree(value_copy1);
    sdsfree(value_copy4);
    sdsfree(value_copy5);
    sdsfree(value_copy6);
    sdsfree(value_copy9);
    sdsfree(value_copy10);
}

TEST_F(EntryTest, entryStringRef) {
    sds field1 = sdsnew(SHORT_FIELD);
    sds value1 = sdsnew(SHORT_VALUE);
    sds value_copy1 = sdsdup(value1);
    long long expiry1 = EXPIRY_NONE;
    entry *e1 = entryCreate(field1, value1, expiry1);
    entry *e2 = entryUpdateAsStringRef(e1, value_copy1, sdslen(value_copy1), entryGetExpiry(e1));
    verify_entry_properties(e2, field1, value_copy1, expiry1, false, true);
    ASSERT_TRUE(entryHasStringRef(e2));

    long long expiry2 = 100;
    entry *e3 = entryUpdateAsStringRef(e2, value_copy1, sdslen(value_copy1), expiry2);
    ASSERT_NE(e2, e3);
    verify_entry_properties(e3, field1, value_copy1, expiry2, true, true);
    ASSERT_TRUE(entryHasStringRef(e3));

    long long expiry3 = 200;
    entry *e4 = entryUpdateAsStringRef(e3, value_copy1, sdslen(value_copy1), expiry3);
    ASSERT_EQ(e3, e4);
    verify_entry_properties(e4, field1, value_copy1, expiry3, true, true);
    ASSERT_TRUE(entryHasStringRef(e4));

    sds value2 = sdsnew(SHORT_VALUE);
    sds value_copy2 = sdsdup(value2);
    entry *e5 = entryUpdate(e4, value2, expiry3);
    verify_entry_properties(e5, field1, value_copy2, expiry3, true, false);
    ASSERT_FALSE(entryHasStringRef(e5));

    entry *e6 = entryUpdateAsStringRef(e5, value_copy1, sdslen(value_copy1), expiry2);
    ASSERT_NE(e5, e6);
    verify_entry_properties(e6, field1, value_copy1, expiry2, true, true);
    ASSERT_TRUE(entryHasStringRef(e6));

    sds value3 = sdsnew(LONG_VALUE);
    sds value_copy3 = sdsdup(value3);
    entry *e7 = entryUpdate(e6, value3, expiry1);
    verify_entry_properties(e7, field1, value_copy3, expiry1, false, true);
    ASSERT_FALSE(entryHasStringRef(e7));

    entryFree(e7);
    sdsfree(value_copy1);
    sdsfree(value_copy2);
    sdsfree(value_copy3);
    sdsfree(field1);
}
