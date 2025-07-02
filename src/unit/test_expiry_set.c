#include <stdint.h>
#include "expiry_set.h"
#include "test_help.h"
#include "zmalloc.h"

static uint64_t ptrHash(const void *key) {
    /* use the pointer value itself as hash */
    return (uint64_t)(uintptr_t)key;
}
static int ptrKeyCompare(const void *a, const void *b) {
    return a == b;
}

static dictType testDictType = {
    ptrHash,            /* hash function */
    NULL,               /* key dup       */
    ptrKeyCompare,      /* key compare   */
    NULL,               /* key destructor*/
    NULL,               /* val dup       */
    NULL                /* val destructor*/
};

/* Two distinct static variables. we’ll use these as keys. */
static int key1, key2;

/* Test that adding a key returns 1, refreshing it returns 0, and count is correct. */
int test_expiry_set_add_and_count(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    /* First insert of key1 should return “new” (1). */
    int added = expirySetAdd(es, &key1, now + 5000);
    TEST_ASSERT(added == 1);

    /* A second insert of the same key should be “refresh” (0). */
    int refreshed = expirySetAdd(es, &key1, now + 10000);
    TEST_ASSERT(refreshed == 0);

    /* Only one live entry remains. */
    int count = expirySetCount(es);
    TEST_ASSERT(count == 1);

    expirySetFree(es);
    return 0;
}

/* Test removal of existing and non­existing keys. */
int test_expiry_set_remove(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    expirySetAdd(es, &key1, now + 5000);
    expirySetAdd(es, &key2, now + 5000);
    TEST_ASSERT(expirySetCount(es) == 2);

    /* Remove key1: should succeed once, then fail if removed again. */
    TEST_ASSERT(expirySetRemove(es, &key1) == 1);
    TEST_ASSERT(expirySetRemove(es, &key1) == 0);

    /* Only key2 remains. */
    TEST_ASSERT(expirySetCount(es) == 1);

    expirySetFree(es);
    return 0;
}

/* Test that entries whose expiry ≤ now are purged. */
int test_expiry_set_expire(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    /* Add one already expired and one future entry. */
    expirySetAdd(es, &key1, now - 1);
    expirySetAdd(es, &key2, now + 5000);

    /* Expire should remove the first only. */
    TEST_ASSERT(expirySetExpire(es) == 1);

    /* Only key2 remains. */
    TEST_ASSERT(expirySetCount(es) == 1);

    expirySetFree(es);
    return 0;
}

/* Test that Count automatically expires old entries before reporting length. */
int test_expiry_set_count_triggers_expire(int argc, char **argv, int flags) {
    UNUSED(argc); UNUSED(argv); UNUSED(flags);

    ExpirySet *es = expirySetCreate(&testDictType);
    TEST_ASSERT(es != NULL);

    mstime_t now = mstime();
    expirySetAdd(es, &key1, now - 1);
    expirySetAdd(es, &key2, now + 5000);

    /* expirySetCount should clear key1 */
    TEST_ASSERT(expirySetCount(es) == 1);

    /* Subsequent expire does nothing. */
    TEST_ASSERT(expirySetExpire(es) == 0);

    expirySetFree(es);
    return 0;
}