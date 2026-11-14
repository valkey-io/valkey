/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>

extern "C" {
#include "expire.h"
#include "fmacros.h"
#include "server.h"
#include "smember.h"
}

#define SHORT_MEMBER "tag:7"
#define LONG_MEMBER_LEN 200
#define LARGE_MEMBER_LEN 300

static char LONG_MEMBER[LONG_MEMBER_LEN + 1];
static char LARGE_MEMBER[LARGE_MEMBER_LEN + 1];

/* One member per sds header type (sdshdr5, sdshdr8, sdshdr16) plus a binary one. */
static const struct {
    const char *str;
    size_t len;
} MEMBERS[] = {{SHORT_MEMBER, 5}, {"a\0b", 3}, {LONG_MEMBER, LONG_MEMBER_LEN}, {LARGE_MEMBER, LARGE_MEMBER_LEN}};

static void *neverMoveDefragAlloc(void *) {
    return nullptr;
}

/* Allocate before freeing so relocation always changes the pointer. */
static void *alwaysMoveDefragAlloc(void *ptr) {
    size_t size = zmalloc_usable_size(ptr);
    void *newptr = zmalloc(size);
    memcpy(newptr, ptr, size);
    zfree(ptr);
    return newptr;
}

class SmemberTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        memset(LONG_MEMBER, 'y', LONG_MEMBER_LEN);
        memset(LARGE_MEMBER, 'x', LARGE_MEMBER_LEN);
    }

    void verifyMember(const smember *m, const char *str, size_t len, mstime_t expiry) {
        ASSERT_EQ(sdslen(m), len);
        ASSERT_EQ(memcmp(m, str, len), 0);
        ASSERT_EQ(smemberGetExpiry(m), expiry);
        ASSERT_EQ(smemberHasExpiry(m), expiry != EXPIRY_NONE);
        if (expiry == EXPIRY_NONE) {
            sds reference = sdsnewlen(str, len);
            ASSERT_EQ(sdsType(m), sdsType(reference));
#ifdef USE_JEMALLOC
            /* Other allocators can return different usable sizes for equal requests. */
            ASSERT_EQ(smemberMemUsage(m), sdsAllocSize(reference));
#endif
            sdsfree(reference);
        }
    }
};

TEST_F(SmemberTest, setExpiryTransitions) {
    for (size_t i = 0; i < sizeof(MEMBERS) / sizeof(MEMBERS[0]); i++) {
        const char *str = MEMBERS[i].str;
        size_t len = MEMBERS[i].len;
        smember *m = smemberCreate(str, len, EXPIRY_NONE);
        verifyMember(m, str, len, EXPIRY_NONE);

        m = smemberSetExpiry(m, 5000);
        verifyMember(m, str, len, 5000);

        m = smemberSetExpiry(m, 6000);
        verifyMember(m, str, len, 6000);

        m = smemberSetExpiry(m, EXPIRY_NONE);
        verifyMember(m, str, len, EXPIRY_NONE);

        smemberFree(m);
    }
}

TEST_F(SmemberTest, isExpired) {
    enterExecutionUnit(1, ustime());
    mstime_t now = commandTimeSnapshot();

    smember *plain = smemberCreate(SHORT_MEMBER, strlen(SHORT_MEMBER), EXPIRY_NONE);
    ASSERT_FALSE(smemberIsExpired(plain));

    smember *future = smemberCreate(SHORT_MEMBER, strlen(SHORT_MEMBER), now + 10000);
    ASSERT_FALSE(smemberIsExpired(future));

    smember *past = smemberCreate(SHORT_MEMBER, strlen(SHORT_MEMBER), now - 10000);
    ASSERT_TRUE(smemberIsExpired(past));

    smemberFree(plain);
    smemberFree(future);
    smemberFree(past);
    exitExecutionUnit();
}

/* Allocator slack can force a larger header than the payload requires. */
TEST_F(SmemberTest, memUsage) {
#ifndef USE_JEMALLOC
    GTEST_SKIP() << "Test requires jemalloc";
#endif
    char member[253];
    memset(member, 'x', sizeof(member) - 1);
    member[sizeof(member) - 1] = '\0';
    ASSERT_EQ(sdsReqType(sizeof(member) - 1), SDS_TYPE_8);
    smember *m = smemberCreate(member, sizeof(member) - 1, 1000);
    ASSERT_EQ(sdsType(m), SDS_TYPE_16);
    ASSERT_EQ(smemberMemUsage(m), zmalloc_usable_size((char *)sdsAllocPtr(m) - sizeof(mstime_t)));
    smemberFree(m);
}

/* smemberDefrag() returns the moved member at the same offset into the new allocation. */
TEST_F(SmemberTest, defrag) {
    for (size_t i = 0; i < sizeof(MEMBERS) / sizeof(MEMBERS[0]); i++) {
        const char *str = MEMBERS[i].str;
        size_t len = MEMBERS[i].len;
        smember *plain = smemberCreate(str, len, EXPIRY_NONE);
        ASSERT_EQ(smemberDefrag(plain, neverMoveDefragAlloc), nullptr);
        smember *moved_plain = smemberDefrag(plain, alwaysMoveDefragAlloc);
        ASSERT_NE(moved_plain, nullptr);
        verifyMember(moved_plain, str, len, EXPIRY_NONE);
        smemberFree(moved_plain);

        smember *prefixed = smemberCreate(str, len, 4242);
        smember *moved_prefixed = smemberDefrag(prefixed, alwaysMoveDefragAlloc);
        ASSERT_NE(moved_prefixed, nullptr);
        verifyMember(moved_prefixed, str, len, 4242);
        smemberFree(moved_prefixed);
    }
}
