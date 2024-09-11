#include "../hashtab.h"
#include "test_help.h"
#include "../mt19937-64.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <math.h>


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

/* Callback for testing hashtabEmpty(). */
static long empty_callback_call_counter;
void emptyCallback(hashtab *t) {
    UNUSED(t);
    empty_callback_call_counter++;
}

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

    /* Delete half of them */
    for (j = 0; j < count / 2; j++) {
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

    /* Empty, i.e. delete remaining elements, with progress callback. */
    empty_callback_call_counter = 0;
    hashtabEmpty(t, emptyCallback);
    assert(empty_callback_call_counter > 0);

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

int test_instant_rehashing(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    long count = 200;

    /* A set of longs, i.e. pointer-sized values. */
    hashtabType type = {.instant_rehashing = 1};
    hashtab *t = hashtabCreate(&type);
    long j;

    /* Populate and check that rehashing is never ongoing. */
    for (j = 0; j < count; j++) {
        assert(hashtabAdd(t, (void *)j));
        assert(!hashtabIsRehashing(t));
    }

    /* Delete and check that rehashing is never ongoing. */
    for (j = 0; j < count; j++) {
        assert(hashtabDelete(t, (void *)j));
        assert(!hashtabIsRehashing(t));
    }

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

int test_two_phase_insert_and_pop(int argc, char **argv, int flags) {
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
        hashtabInsertAtPosition(t, e, position);
    }

    if (count < 1000) {
        printf("Bucket fill: ");
        hashtabHistogram(t);
    }

    /* Check that all elements were inserted. */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e;
        assert(hashtabFind(t, key, (void **)&e));
        assert(!strcmp(val, getval(e)));
    }

    /* Test two-phase pop. */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e;
        void *position;
        size_t size_before_find = hashtabSize(t);
        assert(hashtabTwoPhasePopFind(t, key, (void **)&e, &position));
        assert(!strcmp(val, getval(e)));
        assert(hashtabSize(t) == size_before_find);
        hashtabTwoPhasePopDelete(t, position);
        assert(hashtabSize(t) == size_before_find - 1);
    }
    assert(hashtabSize(t) == 0);

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

    long num_elements = (flags & UNIT_TEST_LARGE_MEMORY) ? 1000000 : 200000;
    int num_rounds = (flags & UNIT_TEST_ACCURATE) ? 20 : 5;

    /* A set of longs, i.e. pointer-sized values. */
    hashtabType type = {0};
    long j;

    for (int round = 0; round < num_rounds; round++) {
        /* First round count = num_elements, then some more. */
        long count = num_elements * (1 + 2 * (double)round / num_rounds);

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
            printf("Element %lu returned, max == %lu. Num returned: %u\n", j, count * 2 - 1, num_returned);
            printf("Safe %d, table %d, index %lu, pos in bucket %d, rehashing? %d\n", iter.safe, iter.table, iter.index,
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
    printf("Safe iterator returned %lu of the %lu elements inserted while iterating.\n", num_optional_returned, count);

    hashtabRelease(t);
    return 0;
}

int test_random_element(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    long count = (flags & UNIT_TEST_LARGE_MEMORY) ? 7000 : 400;
    long num_rounds = (flags & UNIT_TEST_ACCURATE) ? 1000000 : 10000;

    unsigned long long seed;
    getRandomBytes((void *)&seed, sizeof(seed));
    init_genrand64(seed);
    srandom((unsigned)seed);

    /* A set of longs, i.e. pointer-sized values. */
    hashtabType type = {0};
    hashtab *t = hashtabCreate(&type);

    /* Populate */
    for (long j = 0; j < count; j++) {
        assert(hashtabAdd(t, (void *)j));
    }

    /* Pick elements, and count how many times each element is picked. */
    unsigned times_picked[count];
    memset(times_picked, 0, sizeof(times_picked));
    for (long i = 0; i < num_rounds; i++) {
        long element;
        assert(hashtabFairRandomElement(t, (void **)&element));
        assert(element >= 0 && element < count);
        times_picked[element]++;
    }
    hashtabRelease(t);

    /* Fairness measurement
     * --------------------
     *
     * Selecting a single random element: For any element in the hash table, let
     * X=1 if the we selected the element (success) and X=0 otherwise. With m
     * elements, our element is sepected with probability p = 1/m, the expected
     * value is E(X) = 1/m, E(X^2) = 1/m and the variance:
     *
     *     Var(X) = E(X^2) - (E(X))^2 = 1/m - 1/(m^2) = (1/m) * (1 - 1/m).
     *
     * Repeating the selection of a random element: Let's repeat the experiment
     * n times and let Y be the number of times our element was selected. This
     * is a binomial distribution.
     *
     *     Y = X_1 + X_2 + ... + X_n
     *     E(Y) = n/m
     *
     * The variance of a sum of independent random variables is the sum of the
     * variances, so Y has variance np(1−p).
     *
     *     Var(Y) = npq = np(1 - p) = (n/m) * (1 - 1/m) = n * (m - 1) / (m * m)
     */
    double m = (double)count, n = (double)num_rounds;
    double expected = n / m;                 /* E(Y) */
    double variance = n * (m - 1) / (m * m); /* Var(Y) */
    double std_dev = sqrt(variance);

    /* With large n, the distribution approaches a normal distribution and we
     * can use p68 = within 1 std dev, p95 = within 2 std dev, p99.7 = within 3
     * std dev. */
    long p68 = 0, p95 = 0, p99 = 0, p4dev = 0, p5dev = 0;
    for (long j = 0; j < count; j++) {
        double dev = expected - times_picked[j];
        p68 += (dev >= -std_dev && dev <= std_dev);
        p95 += (dev >= -std_dev * 2 && dev <= std_dev * 2);
        p99 += (dev >= -std_dev * 3 && dev <= std_dev * 3);
        p4dev += (dev >= -std_dev * 4 && dev <= std_dev * 4);
        p5dev += (dev >= -std_dev * 5 && dev <= std_dev * 5);
    }
    printf("Random element fairness test\n");
    printf("  Pick one of %ld elements, %ld times.\n", count, num_rounds);
    printf("  Expecting each element to be picked %.2lf times, std dev %.3lf.\n", expected, std_dev);
    printf("  Within 1 std dev (p68) = %.2lf%%\n", 100 * p68 / m);
    printf("  Within 2 std dev (p95) = %.2lf%%\n", 100 * p95 / m);
    printf("  Within 3 std dev (p99) = %.2lf%%\n", 100 * p99 / m);
    printf("  Within 4 std dev       = %.2lf%%\n", 100 * p4dev / m);
    printf("  Within 5 std dev       = %.2lf%%\n", 100 * p5dev / m);

    /* Conclusion? The number of trials (n) relative to the probabilities (p and
     * 1 − p) must be sufficiently large (n * p ≥ 5 and n * (1 − p) ≥ 5) to
     * approximate a binomial distribution with a normal distribution. */
    if (n / m >= 5 && n * (1 - 1 / m) >= 5) {
        TEST_ASSERT_MESSAGE("Too unfair randomness", 100 * p99 / m >= 60.0);
    } else {
        printf("To uncertain numbers to draw any conclusions about fairness.\n");
    }
    return 0;
}
