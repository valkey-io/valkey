#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include "test_help.h"

#include "../hashset.h"

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
    return hashsetGenHashFunction(key, strlen(key));
}

static int keycmp(hashset *ht, const void *key1, const void *key2) {
    (void)ht;
    return strcmp(key1, key2);
}

static void freekeyval(hashset *ht, void *keyval) {
    (void)ht;
    free(keyval);
}

hashsetType keyval_type = {
    .elementGetKey = getkey,
    .hashFunction = hashfunc,
    .keyCompare = keycmp,
    .elementDestructor = freekeyval,
};

void hashsetDump(hashset *s);
void hashsetHistogram(hashset *s);
size_t rev(size_t v);

int test_rev(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    size_t x = 0xabcdef8801234567;
    printf("Rev(%lx) ==> %lx\n", x, rev(x));
    return 0;
}

int test_add_and_find(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int count = 200;
    //hashsetSetResizePolicy(HASHSET_RESIZE_AVOID);

    hashset *ht = hashsetCreate(&keyval_type);
    int j;

    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e = create_keyval(key, val);
        assert(hashsetAdd(ht, e));
    }

    printf("Bucket fill: ");
    hashsetHistogram(ht);
    //hashsetDump(ht);

    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e;
        assert(hashsetFind(ht, key, (void **)&e));
        assert(!strcmp(val, getval(e)));
        //printf("Key %s => %s\n", (char *)getkey(e), (char *)getval(e));
    }

    return 0;
}
