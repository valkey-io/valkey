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

/* Prototypes for debugging */
void hashtabDump(hashtab *s);
void hashtabHistogram(hashtab *s);
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

int test_add_and_find(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int count = 200;
    //hashtabSetResizePolicy(HASHTAB_RESIZE_AVOID);

    hashtabType keyval_type = {
        .elementGetKey = getkey,
        .hashFunction = hashfunc,
        .keyCompare = keycmp,
        .elementDestructor = freekeyval,
    };
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

    printf("Bucket fill: ");
    hashtabHistogram(t);
    //hashtabDump(t);

    /* Find */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e;
        assert(hashtabFind(t, key, (void **)&e));
        assert(!strcmp(val, getval(e)));
    }

    /* Release memory */
    hashtabRelease(t);

    return 0;
}

#define scan_test_table_size 2000000
typedef struct {
    unsigned emitted_element_count[scan_test_table_size];
    unsigned total_count;
} scandata;

void scanfn(void *privdata, void *element) {
    scandata *data = (scandata *)privdata;
    unsigned long j = (unsigned long)element;
    data->emitted_element_count[j]++;
    data->total_count++;
}

#define cycle_length_upper_bound 999
int test_scan(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    uint8_t hashseed[16];
    getRandomBytes(hashseed, sizeof(hashseed));
    hashtabSetHashFunctionSeed(hashseed);

    unsigned long count = scan_test_table_size;

    /* A set of longs, i.e. pointer-sized values. */
    hashtabType type = {0};
    hashtab *t = hashtabCreate(&type);
    unsigned long j;

    int max_chainlen_seen = 0;

    /* Populate */
    for (j = 0; j < count; j++) {
        long existing = 0;
        int ret = hashtabAddOrFind(t, (void *)j, (void**)&existing);
        /* int ret = hashtabAdd(t, (void *)j); */
        if (!ret) {
            /* printf("Add failed for %ld, ret = %d\n", j, ret); */
            printf("Add failed for %ld, existing = %ld\n", j, existing);
        }
        /* Sample some iterations and check for the longest probing chain. This
         * isn't for the unit test, but for tuning the fill factor and for
         * debugging. */
        if (j % (count / 13) == 0) {
            int longest_chainlen = hashtabLongestProbingChain(t);
            if (longest_chainlen > max_chainlen_seen) {
                max_chainlen_seen = longest_chainlen;
            }
        }
    }

    printf("Added %lu elements. Longest chain seen: %d.\n",
           count, max_chainlen_seen);
    if (0) {
        /* Too large output for hugh tables. */
        printf("Bucket fill: ");
        hashtabHistogram(t);
    }

    scandata data = {0};
    int elements_per_cycle_count[cycle_length_upper_bound + 1] = {0};
    assert(elements_per_cycle_count[0] == 0);
    int num_cycles;
    size_t cursor = 0;
    do {
        data.total_count = 0;
        cursor = hashtabScan(t, cursor, scanfn, &data, 0);
        assert(data.total_count <= cycle_length_upper_bound);
        elements_per_cycle_count[data.total_count]++;
        num_cycles++;
    } while (cursor != 0);

    /* Verify every element was returned exactly once. This can be expected
       since no elements are added or removed during the scan. */
    for (j = 0; j < count; j++) {
        assert(data.emitted_element_count[j] == 1);
    }

    printf("Emitted elements per cycle:");
    int lines_to_print = 10;
    for (int i = cycle_length_upper_bound; i >= 0; i--) {
        if (elements_per_cycle_count[i] > 0) {
            if (lines_to_print < 10) {
                printf(",");
            }
            printf(" %d", i);
            if (elements_per_cycle_count[i] > 1) {
                printf(" (%d times)", elements_per_cycle_count[i]);
            }
            if (--lines_to_print == 0) {
                break;
            }
        }
    }
    printf(".\n");
    return 0;
}
