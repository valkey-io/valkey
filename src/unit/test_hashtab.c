#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include "test_help.h"

#include "../hashtab.h"

/* From util.c: getRandomBytes to seed hash function. */
void getRandomBytes(unsigned char *p, size_t len);

/* An element holding a string key and a string value in one allocation. */
typedef struct {
    unsigned int keysize; /* Sizes, including null-terminator */
    unsigned int valsize;
    char data[]; /* key and value */
} keyval;

static keyval *create_keyval(const char *key, const char *val) {
    size_t keysize = strlen(key) + 1;
    size_t valsize = strlen(val) + 1;
    keyval *e = malloc(sizeof(keyval) + keysize + valsize);
    e->keysize = keysize;
    e->valsize = valsize;
    memcpy(e->data, key, keysize);
    memcpy(e->data + keysize, val, valsize);
    return e;
}

static const void *getkey(const void *element) {
    const keyval *e = element;
    return e->data;
}

static const void *getval(const void *element) {
    const keyval *e = element;
    return e->data + e->keysize;
}

static uint64_t hashfunc(const void *key) {
    return hashtabGenHashFunction(key, strlen(key));
}

static int keycmp(hashtab *ht, const void *key1, const void *key2) {
    (void)ht;
    return strcmp(key1, key2);
}

static void freekeyval(hashtab *ht, void *keyval) {
    (void)ht;
    free(keyval);
}

/* Hashtab type used for some of the tests. */
static hashtabType keyval_type = {
    .elementGetKey = getkey,
    .hashFunction = hashfunc,
    .keyCompare = keycmp,
    .elementDestructor = freekeyval,
};

/* Prototypes for debugging */
void hashtabDump(hashtab *t);
void hashtabHistogram(hashtab *t);
int hashtabLongestProbingChain(hashtab *t);
size_t nextCursor(size_t v, size_t mask);

int test_cursor(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    TEST_ASSERT(nextCursor(0x0000, 0xffff) == 0x8000);
    TEST_ASSERT(nextCursor(0x8000, 0xffff) == 0x4000);
    TEST_ASSERT(nextCursor(0x4001, 0xffff) == 0xc001);
    TEST_ASSERT(nextCursor(0xffff, 0xffff) == 0x0000);
    return 0;
}

int test_set_hash_function_seed(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    uint8_t hashseed[16];
    getRandomBytes(hashseed, sizeof(hashseed));
    hashtabSetHashFunctionSeed(hashseed);
    return 0;
}

static void add_find_delete_test_helper(int flags) {
    int count = (flags & UNIT_TEST_ACCURATE) ? 1000000 : 200;
    hashtab *t = hashtabCreate(&keyval_type);
    int j;

    /* Add */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e = create_keyval(key, val);
        assert(hashtabAdd(t, e));
    }

    if (count < 1000) {
        printf("Bucket fill: ");
        hashtabHistogram(t);
    }

    /* Find */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e;
        assert(hashtabFind(t, key, (void **)&e));
        assert(!strcmp(val, getval(e)));
    }

    /* Delete */
    for (j = 0; j < count; j++) {
        char key[32];
        snprintf(key, sizeof(key), "%d", j);
        if (j % 3 == 0) {
            /* Test hashtabPop */
            char val[32];
            snprintf(val, sizeof(val), "%d", count - j + 42);
            keyval *e;
            assert(hashtabPop(t, key, (void **)&e));
            assert(!strcmp(val, getval(e)));
            free(e);
        } else {
            assert(hashtabDelete(t, key));
        }
    }

    /* Release memory */
    hashtabRelease(t);
}    

int test_add_find_delete(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    add_find_delete_test_helper(flags);
    return 0;
}

int test_add_find_delete_avoid_resize(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    hashtabSetResizePolicy(HASHTAB_RESIZE_AVOID);
    add_find_delete_test_helper(flags);
    hashtabSetResizePolicy(HASHTAB_RESIZE_ALLOW);
    return 0;
}

int test_two_phase_insert(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int count = (flags & UNIT_TEST_ACCURATE) ? 1000000 : 200;
    hashtab *t = hashtabCreate(&keyval_type);
    int j;

    /* hashtabFindPositionForInsert + hashtabInsertAtPosition */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        void *position = hashtabFindPositionForInsert(t, key, NULL);
        assert(position != NULL);
        keyval *e = create_keyval(key, val);
        printf("hashtabInsertAtPosition(%p, %d, %p)\n", t, j, position);
        hashtabInsertAtPosition(t, e, position);
    }

    if (count < 1000) {
        printf("Bucket fill: ");
        hashtabHistogram(t);
    }

    /* Find */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e;
        assert(hashtabFind(t, key, (void **)&e));
        assert(!strcmp(val, getval(e)));
    }

    hashtabRelease(t);
    return 0;
}

typedef struct {
    long count;
    uint8_t element_seen[];
} scandata;

void scanfn(void *privdata, void *element) {
    scandata *data = (scandata *)privdata;
    unsigned long j = (unsigned long)element;
    data->element_seen[j]++;
    data->count++;
}

int test_scan(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    long num_elements = (flags & UNIT_TEST_LARGE_MEMORY) ? 10000000 : 2000000;
    int num_rounds = (flags & UNIT_TEST_ACCURATE) ? 10 : 1;

    /* A set of longs, i.e. pointer-sized values. */
    hashtabType type = {0};
    long j;

    for (int round = 0; round < num_rounds; round++) {

        long count = num_elements * (1 + 2 * (double)round / num_rounds);
        //long count = num_elements * (round + 100) / 100;

        /* Seed, to make sure each round is different. */
        test_set_hash_function_seed(argc, argv, flags);

        /* Populate */
        hashtab *t = hashtabCreate(&type);
        for (j = 0; j < count; j++) {
            assert(hashtabAdd(t, (void *)j));
        }

        /* Scan */
        scandata *data = calloc(1, sizeof(scandata) + count);
        unsigned max_elements_per_cycle = 0;
        unsigned num_cycles = 0;
        long scanned_count = 0;
        size_t cursor = 0;
        do {
            data->count = 0;
            cursor = hashtabScan(t, cursor, scanfn, data, 0);
            if (data->count > max_elements_per_cycle) {
                max_elements_per_cycle = data->count;
            }
            scanned_count += data->count;
            data->count = 0;
            num_cycles++;
        } while (cursor != 0);

        /* Verify every element was returned at least once, but no more than
         * twice. Elements can be returned twice due to probing chains wrapping
         * around scan cursor zero. */
        TEST_ASSERT(scanned_count >= count);
        TEST_ASSERT(scanned_count < count * 2);
        for (j = 0; j < count; j++) {
            assert(data->element_seen[j] >= 1);
            assert(data->element_seen[j] <= 2);
        }

        /* Verify some stuff, but just print it for now. */
        printf("Scanned: %lu; ", count);
        printf("duplicates emitted: %lu; ", scanned_count - count);
        printf("max emitted per call: %d; ", max_elements_per_cycle);
        printf("avg emitted per call: %.2lf\n", (double)count / num_cycles);

        /* Cleanup */
        hashtabRelease(t);
        free(data);
    }
    return 0;
}

int test_iterator(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    long count = 2000000;

    /* A set of longs, i.e. pointer-sized values. */
    hashtabType type = {0};
    hashtab *t = hashtabCreate(&type);
    long j;

    /* Populate */
    for (j = 0; j < count; j++) {
        assert(hashtabAdd(t, (void *)j));
    }

    /* Iterate */
    uint8_t element_returned[count];
    memset(element_returned, 0, sizeof element_returned);
    unsigned num_returned = 0;
    hashtabIterator iter;
    hashtabInitIterator(&iter, t);
    while (hashtabNext(&iter, (void **)&j)) {
        num_returned++;
        element_returned[j]++;
    }
    hashtabResetIterator(&iter);

    /* Check that all elements were returned exactly once. */
    TEST_ASSERT(num_returned == count);
    for (j = 0; j < count; j++) {
        if (element_returned[j] != 1) {
            printf("Element %ld returned %d times\n", j, element_returned[j]);
            return 0;
        }
    }

    hashtabRelease(t);
    return 0;
}

int test_safe_iterator(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    long count = 1000;

    /* A set of longs, i.e. pointer-sized values. */
    hashtabType type = {0};
    hashtab *t = hashtabCreate(&type);
    long j;

    /* Populate */
    for (j = 0; j < count; j++) {
        assert(hashtabAdd(t, (void *)j));
    }

    /* Iterate */
    uint8_t element_returned[count * 2];
    memset(element_returned, 0, sizeof element_returned);
    unsigned num_returned = 0;
    hashtabIterator iter;
    hashtabInitSafeIterator(&iter, t);
    while (hashtabNext(&iter, (void **)&j)) {
        num_returned++;
        if (j < 0 || j >= count * 2) {
            printf("Element %lu returned, max == %lu. Num returned: %u\n",
                   j, count * 2 - 1, num_returned);
            printf("Safe %d, table %d, index %lu, pos in bucket %d, rehashing? %d\n",
                   iter.safe, iter.table, iter.index,
                   iter.posInBucket, !hashtabIsRehashing(t));
            hashtabHistogram(t);
            exit(1);
        }
        assert(j >= 0 && j < count * 2);
        element_returned[j]++;
        if (j % 4 == 0) {
            assert(hashtabDelete(t, (void *)j));
        }
        /* Add elements x if count <= x < count * 2) */
        if (j < count) {
            assert(hashtabAdd(t, (void *)(j + count)));
        }
    }
    hashtabResetIterator(&iter);

    /* Check that all elements present during the whole iteration were returned
     * exactly once. (Some are deleted after being returned.) */
    TEST_ASSERT(num_returned >= count);
    for (j = 0; j < count; j++) {
        if (element_returned[j] != 1) {
            printf("Element %ld returned %d times\n", j, element_returned[j]);
            return 0;
        }
    }
    /* Check that elements inserted during the iteration were returned at most
     * once. */
    unsigned long num_optional_returned;
    for (j = count; j < count * 2; j++) {
        assert(element_returned[j] <= 1);
        num_optional_returned += element_returned[j];
    }
    printf("Safe iterator returned %lu of the %lu elements inserted while iterating.\n",
           num_optional_returned, count);

    hashtabRelease(t);
    return 0;
}

int test_probing_chain_length(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned long count = 1000000;

    /* A set of longs, i.e. pointer-sized integer values. */
    hashtabType type = {0};
    hashtab *t = hashtabCreate(&type);
    unsigned long j;
    for (j = 0; j < count; j++) {
        assert(hashtabAdd(t, (void *)j));
    }
    /* If it's rehashing, add a few more until rehashing is complete. */
    while (hashtabIsRehashing(t)) {
        j++;
        assert(hashtabAdd(t, (void *)j));
    }
    TEST_ASSERT(j < count * 2);
    int max_chainlen_not_rehashing = hashtabLongestProbingChain(t);
    TEST_ASSERT(max_chainlen_not_rehashing < 100);

    /* Add more until rehashing starts again. */
    while (!hashtabIsRehashing(t)) {
        j++;
        assert(hashtabAdd(t, (void *)j));
    }
    TEST_ASSERT(j < count * 2);
    int max_chainlen_rehashing = hashtabLongestProbingChain(t);
    TEST_ASSERT(max_chainlen_rehashing < 100);

    hashtabRelease(t);
    return 0;
}
