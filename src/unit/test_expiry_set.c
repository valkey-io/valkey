#include <stdint.h>
#include "../expiry_set.h"
#include "test_help.h"

static uint64_t ptrHash(const void *key) {
    /* use the pointer value itself as hash */
    return (uint64_t)(uintptr_t)key;
}
static int ptrKeyCompare(const void *a, const void *b) {
    return a == b;
}

static dictType testDictType = {
    ptrHash,       /* hash function */
    NULL,          /* key dup       */
    ptrKeyCompare, /* key compare   */
    NULL,          /* key destructor*/
    NULL,          /* val dup       */
    NULL           /* val destructor*/
};

/* Two distinct static variables. we’ll use these as keys. */
static int key1, key2;

/* Test adding a new key returns 1 and increases the count. */
int test_expiry_set_add(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    /* First insert of key1 should return “new” (1). */
    TEST_ASSERT(expirySetAdd(es, &key1, now + 5000) == 1);
    /* And now exactly one entry lives. */
    TEST_ASSERT(expirySetCount(es) == 1);

    expirySetFree(es);
    return 0;
}

/* Test refreshing an existing key returns 0 and does not increase count. */
int test_expiry_set_refresh_count(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    /* Insert once first. */
    TEST_ASSERT(expirySetAdd(es, &key1, now + 5000) == 1);
    /* Refresh the same key: should return 0. */
    TEST_ASSERT(expirySetAdd(es, &key1, now + 10000) == 0);
    /* Count must still be 1. */
    TEST_ASSERT(expirySetCount(es) == 1);

    expirySetFree(es);
    return 0;
}

/* Test removal of an existing key. */
int test_expiry_set_remove_existing(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    /* Add two keys. */
    expirySetAdd(es, &key1, now + 5000);
    expirySetAdd(es, &key2, now + 5000);
    TEST_ASSERT(expirySetCount(es) == 2);

    /* Removing key1 should succeed and reduce the count. */
    TEST_ASSERT(expirySetRemove(es, &key1) == 1);
    TEST_ASSERT(expirySetCount(es) == 1);

    expirySetFree(es);
    return 0;
}

/* Test expirySetExists */
int test_expiry_set_exists(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es);
    mstime_t now = mstime();

    TEST_ASSERT(!expirySetExists(es, &key1));
    expirySetAdd(es, &key1, now + 5000);
    TEST_ASSERT(expirySetExists(es, &key1));
    TEST_ASSERT(!expirySetExists(es, &key2));

    expirySetFree(es);
    return 0;
}

/* Test expirySetExists expires stale entries on lookup. */
int test_expiry_set_exists_auto_expire(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    /* Insert a key that is already expired. */
    TEST_ASSERT(expirySetAdd(es, &key1, now - 1) == 1);
    /* Exists should must remove it immediately. */
    TEST_ASSERT(expirySetExists(es, &key1) == 0);
    TEST_ASSERT(expirySetCount(es) == 0);

    expirySetFree(es);
    return 0;
}

/* Test expirySetGetExpiry return correct expiry */
int test_expiry_set_getexpiry(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    mstime_t now = mstime(), exp = now + 7777, out_exp;

    TEST_ASSERT(!expirySetGetExpiry(es, &key1, &out_exp));
    expirySetAdd(es, &key1, exp);
    TEST_ASSERT(expirySetGetExpiry(es, &key1, &out_exp));
    TEST_ASSERT(exp == out_exp);

    expirySetFree(es);
    return 0;
}

/* Test expirySetGetExpiry returns the correct timestamp for live keys,
 * and removes expired one on lookup. */
int test_expiry_set_getexpiry_auto_expire(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    mstime_t future = now + 5000;
    /* Add one live key and one expired key. */
    TEST_ASSERT(expirySetAdd(es, &key1, future) == 1);
    TEST_ASSERT(expirySetAdd(es, &key2, now - 1) == 1);

    /* key2 is expired, so getExpiry must fail and remove it. */
    mstime_t out;
    TEST_ASSERT(expirySetGetExpiry(es, &key2, &out) == 0);
    TEST_ASSERT(expirySetExists(es, &key2) == 0);

    /* key1 is still live, getExpiry must succeed and return the stored timestamp. */
    TEST_ASSERT(expirySetGetExpiry(es, &key1, &out) == 1);
    TEST_ASSERT(out == future);

    /* Only key1 remains. */
    TEST_ASSERT(expirySetCount(es) == 1);

    expirySetFree(es);
    return 0;
}

/* Test removal of a nonexistent key. */
int test_expiry_set_remove_nonexistent(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    /* Removing key1 before it was ever added should fail. */
    TEST_ASSERT(expirySetRemove(es, &key1) == 0);

    /* Now add and remove key1 once, then try removing again. */
    mstime_t now = mstime();
    expirySetAdd(es, &key1, now + 5000);
    TEST_ASSERT(expirySetRemove(es, &key1) == 1);
    /* Second removal should report “not found.” */
    TEST_ASSERT(expirySetRemove(es, &key1) == 0);

    expirySetFree(es);
    return 0;
}

/* Test that entries whose expiry ≤ now are purged. */
int test_expiry_set_expire(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    /* Add one already expired and one future entry. */
    expirySetAdd(es, &key1, now - 1);
    expirySetAdd(es, &key2, now + 5000);

    /* Expire should remove the first only. */
    TEST_ASSERT(expirySetExpire(es) == 1);
    TEST_ASSERT(!expirySetExists(es, &key1));
    /* Only key2 remains. */
    TEST_ASSERT(expirySetCount(es) == 1);
    TEST_ASSERT(expirySetExists(es, &key2));

    expirySetFree(es);
    return 0;
}

/* Test automatically expiring old entries before reporting length. */
int test_expiry_set_count_triggers_expire(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    expirySetAdd(es, &key1, now - 1);
    expirySetAdd(es, &key2, now + 5000);

    /* expirySetCount should clear key1 */
    TEST_ASSERT(expirySetCount(es) == 1);
    TEST_ASSERT(!expirySetExists(es, &key1));
    /* Subsequent expire does nothing. */
    TEST_ASSERT(expirySetExpire(es) == 0);

    expirySetFree(es);
    return 0;
}

/* Out‐of‐order insert of an already expired key still puts it at the head in the list */
int test_expiry_set_out_of_order_insert(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    static int keyA, keyB, keyC, keyD;
    /* Insert two future expiring keys first */
    TEST_ASSERT(expirySetAdd(es, &keyA, now + 3000) == 1);
    TEST_ASSERT(expirySetAdd(es, &keyB, now + 2000) == 1);
    /* Now insert one already expired */
    TEST_ASSERT(expirySetAdd(es, &keyC, now - 1) == 1);
    /* And one future key */
    TEST_ASSERT(expirySetAdd(es, &keyD, now + 4000) == 1);

    /* Expire will remove exactly that one, even though it was inserted late. */
    TEST_ASSERT(expirySetExpire(es) == 1);
    TEST_ASSERT(!expirySetExists(es, &keyC));
    /* The remaining entries (A,B,D) are all > now, so Count returns 3. */
    TEST_ASSERT(expirySetCount(es) == 3);

    expirySetFree(es);
    return 0;
}

/* Refresh an existing entry to a later expiry and verify it skips ahead of others. */
int test_expiry_set_refresh_triggers_reorder(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es);

    mstime_t now = mstime();
    static int keyA, keyB;
    expirySetAdd(es, &keyA, now - 2);
    expirySetAdd(es, &keyB, now - 1);

    /* Refresh A so that its new expiry is after B */
    expirySetAdd(es, &keyA, now + 3000);

    /* B should be evicted */
    TEST_ASSERT(expirySetExpire(es) == 1);
    TEST_ASSERT(expirySetCount(es) == 1);
    TEST_ASSERT(expirySetExists(es, &keyA));

    expirySetFree(es);
    return 0;
}
