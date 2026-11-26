/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Volatile set metadata stays synchronized through expiry transitions and defragmentation. */

#include "generated_wrappers.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "expire.h"
#include "listpack.h"
#include "server.h"
#include "smember.h"
}

#define NOW 1000000000000LL

/* Allocate before freeing so relocation always changes the pointer. */
static void *alwaysMoveDefragAlloc(void *ptr) {
    size_t size = zmalloc_usable_size(ptr);
    void *newptr = zmalloc(size);
    memcpy(newptr, ptr, size);
    zfree(ptr);
    return newptr;
}

static void addMembers(robj *o, const char **members, size_t count) {
    for (size_t i = 0; i < count; i++) {
        sds s = sdsnew(members[i]);
        EXPECT_EQ(setTypeAdd(o, s), 1);
        sdsfree(s);
    }
}

static robj *makeListpackSet(const char **members, size_t count) {
    robj *o = createSetListpackObject();
    addMembers(o, members, count);
    return o;
}

/* Convert while empty: a hashtable-encoded set never converts back. */
static robj *makeHashtableSet(const char **members, size_t count) {
    robj *o = createSetListpackObject();
    setTypeConvert(o, OBJ_ENCODING_HASHTABLE);
    addMembers(o, members, count);
    return o;
}

static expiryModificationResult setExpiry(robj *o, const char *member, mstime_t expiry) {
    sds s = sdsnew(member);
    expiryModificationResult res = setTypeSetExpiry(o, s, expiry, 0);
    sdsfree(s);
    return res;
}

static int getExpiry(robj *o, const char *member, mstime_t *expiry) {
    sds s = sdsnew(member);
    int res = setTypeGetExpiry(o, s, expiry);
    sdsfree(s);
    return res;
}

static smember **memberRef(robj *o, const char *member) {
    sds s = sdsnew(member);
    void **ref = hashtableFindRef((hashtable *)objectGetVal(o), s);
    sdsfree(s);
    return (smember **)ref;
}

static hashtableType *hashtableTypeOf(robj *o) {
    return hashtableGetType((hashtable *)objectGetVal(o));
}

static bool hasVset(robj *o) {
    return vsetIsValid((vset *)hashtableMetadata((hashtable *)objectGetVal(o)));
}

class SetVolatileTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        memset(&server, 0, sizeof(valkeyServer));
        server.set_max_listpack_entries = 128;
        server.set_max_listpack_value = 64;
    }

    void SetUp() override {
        server.cmd_time_snapshot = NOW;
    }
};

/* The aggregate header exists exactly while at least one member has a TTL. */
TEST_F(SetVolatileTest, volatileCountHeaderLifecycle) {
    const char *members[] = {"a", "b", "c"};
    robj *o = makeListpackSet(members, sizeof(members) / sizeof(members[0]));
    ASSERT_EQ(setTypeVolatileCount(o), 0);
    ASSERT_FALSE(lpIsMetadata(lpStart((unsigned char *)objectGetVal(o))));

    ASSERT_EQ(setExpiry(o, "a", NOW + 5000), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(setTypeVolatileCount(o), 1);
    ASSERT_EQ(setTypeSize(o), 3u);

    ASSERT_EQ(setExpiry(o, "c", NOW + 9000), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(setTypeVolatileCount(o), 2);

    ASSERT_EQ(setExpiry(o, "a", NOW + 7000), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(setTypeVolatileCount(o), 2);

    ASSERT_EQ(setExpiry(o, "a", EXPIRY_NONE), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(setTypeVolatileCount(o), 1);

    ASSERT_EQ(setExpiry(o, "c", EXPIRY_NONE), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(setTypeVolatileCount(o), 0);
    ASSERT_FALSE(lpIsMetadata(lpStart((unsigned char *)objectGetVal(o))));
    ASSERT_EQ(setTypeSize(o), 3u);

    decrRefCount(o);
}

/* An expired member is invisible outside an ignore-TTL bracket and still counted until reclaimed. */
TEST_F(SetVolatileTest, ignoreTTLBracketRevealsExpiredMembers) {
    const char *members[] = {"a", "b"};
    robj *o = makeListpackSet(members, sizeof(members) / sizeof(members[0]));
    ASSERT_EQ(setExpiry(o, "a", NOW + 1000), EXPIRATION_MODIFICATION_SUCCESSFUL);
    server.cmd_time_snapshot = NOW + 2000;

    mstime_t expiry;
    ASSERT_EQ(getExpiry(o, "a", &expiry), C_ERR);
    ASSERT_EQ(setTypeSize(o), 2u);
    ASSERT_EQ(setTypeVolatileCount(o), 1);

    setTypeIgnoreTTL(o, true);
    int found = getExpiry(o, "a", &expiry);
    setTypeIgnoreTTL(o, false);
    ASSERT_EQ(found, C_OK);
    ASSERT_EQ(expiry, NOW + 1000);
    ASSERT_EQ(getExpiry(o, "a", &expiry), C_ERR);

    decrRefCount(o);
}

/* As for hashes, closing a bracket clears the shared listpack flag whatever the encoding, so a
 * bracket opened on a listpack that was converted in between cannot leak it. Opening one on a
 * hashtable leaves the flag alone. */
TEST_F(SetVolatileTest, closingIgnoreTTLBracketClearsListpackFlagForAnyEncoding) {
    const char *members[] = {"a", "b"};
    robj *ht = makeHashtableSet(members, sizeof(members) / sizeof(members[0]));
    robj *is = createIntsetObject();

    listpackObjectIgnoreTTL(true);
    setTypeIgnoreTTL(ht, false);
    ASSERT_FALSE(listpackObjectIgnoreTTL(false));

    listpackObjectIgnoreTTL(true);
    setTypeIgnoreTTL(is, false);
    ASSERT_FALSE(listpackObjectIgnoreTTL(false));

    setTypeIgnoreTTL(ht, true);
    ASSERT_FALSE(listpackObjectIgnoreTTL(false));

    decrRefCount(ht);
    decrRefCount(is);
}

/* The hashtable type carries the vset, so it swaps on the first and last member TTL only. */
TEST_F(SetVolatileTest, firstAndLastExpirySwapTheHashtableType) {
    const char *members[] = {"a", "b", "c"};
    robj *o = makeHashtableSet(members, sizeof(members) / sizeof(members[0]));

    ASSERT_EQ(hashtableTypeOf(o), &setHashtableType);
    ASSERT_FALSE(hasVset(o));

    ASSERT_EQ(setExpiry(o, "a", NOW + 1000), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(hashtableTypeOf(o), &setWithVolatileMembersHashtableType);
    ASSERT_TRUE(hasVset(o));

    ASSERT_EQ(setExpiry(o, "b", NOW + 2000), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(hashtableTypeOf(o), &setWithVolatileMembersHashtableType);

    ASSERT_EQ(setExpiry(o, "a", EXPIRY_NONE), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(hashtableTypeOf(o), &setWithVolatileMembersHashtableType);
    ASSERT_TRUE(hasVset(o));

    ASSERT_EQ(setExpiry(o, "b", EXPIRY_NONE), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(hashtableTypeOf(o), &setHashtableType);
    ASSERT_FALSE(hasVset(o));

    ASSERT_EQ(setExpiry(o, "c", NOW + 3000), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(hashtableTypeOf(o), &setWithVolatileMembersHashtableType);
    ASSERT_TRUE(hasVset(o));

    decrRefCount(o);
}

TEST_F(SetVolatileTest, removingLastVolatileMemberSwapsTypeBack) {
    const char *members[] = {"a", "b"};
    robj *o = makeHashtableSet(members, sizeof(members) / sizeof(members[0]));
    ASSERT_EQ(setExpiry(o, "a", NOW + 1000), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(hashtableTypeOf(o), &setWithVolatileMembersHashtableType);

    sds s = sdsnew("a");
    ASSERT_EQ(setTypeRemove(o, s), 1);
    sdsfree(s);
    ASSERT_EQ(hashtableTypeOf(o), &setHashtableType);
    ASSERT_FALSE(hasVset(o));
    ASSERT_EQ(setTypeSize(o), 1u);

    decrRefCount(o);
}

/* Active expiration must leave the set outside the ignore-TTL bracket. */
TEST_F(SetVolatileTest, reclaimingTheLastVolatileMemberSwapsTypeBack) {
    const char *members[] = {"a", "b", "c"};
    robj *o = makeHashtableSet(members, sizeof(members) / sizeof(members[0]));
    ASSERT_EQ(setExpiry(o, "a", NOW + 1000), EXPIRATION_MODIFICATION_SUCCESSFUL);
    ASSERT_EQ(setExpiry(o, "b", NOW + 5000), EXPIRATION_MODIFICATION_SUCCESSFUL);

    ASSERT_EQ(setTypeDeleteExpiredMembers(o, NOW + 2000, 1, NULL), 1u);
    ASSERT_EQ(hashtableTypeOf(o), &setWithVolatileMembersHashtableType);
    ASSERT_TRUE(hasVset(o));

    ASSERT_EQ(setTypeDeleteExpiredMembers(o, NOW + 6000, 1, NULL), 1u);
    ASSERT_EQ(hashtableTypeOf(o), &setHashtableType);
    ASSERT_FALSE(hasVset(o));
    ASSERT_EQ(setTypeSize(o), 1u);

    decrRefCount(o);
}

/* Defrag must repoint both hashtable buckets and the vset. */
TEST_F(SetVolatileTest, scanDefragVolatileMovesMembersAndVset) {
    const size_t count = 200;
    char names[count][16];
    const char *members[count];
    smember *members_before[count];
    const size_t volatile_count = count / 2;

    for (size_t i = 0; i < count; i++) {
        snprintf(names[i], sizeof(names[i]), "member:%zu", i);
        members[i] = names[i];
    }
    robj *o = makeHashtableSet(members, count);

    /* Staggered expiries give vsetScanDefrag bucket allocations to relocate. */
    for (size_t i = 0; i < count; i += 2) {
        ASSERT_EQ(setExpiry(o, members[i], NOW + 1000 + (mstime_t)i), EXPIRATION_MODIFICATION_SUCCESSFUL);
    }
    ASSERT_EQ(setTypeVolatileCount(o), (long long)volatile_count);

    for (size_t i = 0; i < count; i++) members_before[i] = *memberRef(o, members[i]);
    void *bucket_before = ((vset *)hashtableMetadata((hashtable *)objectGetVal(o)))->bucket;

    size_t cursor = 0;
    do {
        cursor = setTypeScanDefrag(o, cursor, alwaysMoveDefragAlloc);
    } while (cursor != 0);

    ASSERT_EQ(setTypeSize(o), count);
    ASSERT_EQ(setTypeVolatileCount(o), (long long)volatile_count);
    for (size_t i = 0; i < count; i++) {
        ASSERT_NE(*memberRef(o, members[i]), members_before[i]) << members[i];
        mstime_t expiry;
        ASSERT_EQ(getExpiry(o, members[i], &expiry), C_OK) << members[i];
        ASSERT_EQ(expiry, (i % 2 == 0) ? NOW + 1000 + (mstime_t)i : EXPIRY_NONE) << members[i];
    }
    ASSERT_NE(((vset *)hashtableMetadata((hashtable *)objectGetVal(o)))->bucket, bucket_before);

    ASSERT_EQ(setTypeDeleteExpiredMembers(o, NOW + 100000, volatile_count, NULL), volatile_count);
    ASSERT_FALSE(setTypeHasVolatileMembers(o));
    ASSERT_EQ(setTypeSize(o), count - volatile_count);

    decrRefCount(o);
}

/* The members carrying a TTL, counted without the maintained count: by walking
 * the index and by reading every member. */
static void expectVolatileCount(robj *o, const char *step) {
    long long indexed = 0, with_ttl = 0;
    if (hasVset(o)) {
        vsetIterator it;
        setTypeInitVolatileIterator(o, &it);
        void *entry;
        while (vsetNext(&it, &entry)) indexed++;
        vsetResetIterator(&it);
    }
    hashtableIterator it;
    hashtableInitIterator(&it, (hashtable *)objectGetVal(o), HASHTABLE_ITER_SKIP_VALIDATION);
    void *entry;
    while (hashtableNext(&it, &entry)) with_ttl += smemberHasExpiry((smember *)entry);
    hashtableCleanupIterator(&it);
    EXPECT_EQ(setTypeVolatileCount(o), indexed) << step;
    EXPECT_EQ(setTypeVolatileCount(o), with_ttl) << step;
}

/* The count kept beside the index follows every way a member gains, changes or
 * loses a TTL: expiry changes, removal, reclamation, conversion, duplication
 * and defrag. */
TEST_F(SetVolatileTest, volatileCountFollowsTheIndex) {
    srand(4242);
    const size_t count = 600;
    char names[count][16];
    const char *members[count];
    for (size_t i = 0; i < count; i++) {
        snprintf(names[i], sizeof(names[i]), "m%zu", i);
        members[i] = names[i];
    }
    /* A listpack that converts with volatile members on board. */
    robj *o = makeListpackSet(members, 100);
    for (size_t i = 0; i < 100; i += 3) {
        ASSERT_EQ(setExpiry(o, members[i], NOW + 1000 + (mstime_t)i), EXPIRATION_MODIFICATION_SUCCESSFUL);
    }
    addMembers(o, members + 100, count - 100);
    ASSERT_EQ(objectGetEncoding(o), OBJ_ENCODING_HASHTABLE);
    expectVolatileCount(o, "conversion");

    mstime_t now = NOW;
    for (int round = 0; round < 3000 && !HasFailure(); round++) {
        sds s = sdsnew(members[rand() % count]);
        switch (rand() % 6) {
        case 0:
        case 1: setTypeSetExpiry(o, s, now + 1 + rand() % 50000, 0); break;
        case 2: setTypeSetExpiry(o, s, EXPIRY_NONE, 0); break;
        case 3: setTypeRemove(o, s); break;
        case 4: setTypeAdd(o, s); break;
        default:
            now += rand() % 2000;
            setTypeDeleteExpiredMembers(o, now, 1 + rand() % 50, NULL);
            break;
        }
        sdsfree(s);
        expectVolatileCount(o, "operation");
    }

    robj *dup = setTypeDup(o);
    expectVolatileCount(dup, "dup");
    decrRefCount(dup);

    size_t cursor = 0;
    do {
        cursor = setTypeScanDefrag(o, cursor, alwaysMoveDefragAlloc);
    } while (cursor != 0);
    expectVolatileCount(o, "defrag");

    decrRefCount(o);
}
