

/* ------------------------------- Benchmark ---------------------------------*/
#include "../dict.c"
#include "test_help.h"


#define UNUSED(V) ((void)V)
#define TEST(name) printf("test — %s\n", name);

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char *)key, strlen((char *)key));
}

int compareCallback(dict *d, const void *key1, const void *key2) {
    int l1, l2;
    UNUSED(d);

    l1 = strlen((char *)key1);
    l2 = strlen((char *)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(dict *d, void *val) {
    UNUSED(d);

    zfree(val);
}

char *stringFromLongLong(long long value) {
    char buf[32];
    int len;
    char *s;

    len = snprintf(buf, sizeof(buf), "%lld", value);
    s = zmalloc(len + 1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

dictType BenchmarkDictType = {hashCallback, NULL, compareCallback, freeCallback, NULL, NULL};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg)                                                                                             \
    do {                                                                                                               \
        elapsed = timeInMilliseconds() - start;                                                                        \
        printf(msg ": %ld items in %lld ms\n", count, elapsed);                                                        \
    } while (0)

/* ./valkey-server test dict [<count> | --accurate] */
int test_dict(int argc, char **argv, int flags) {
    long j;
    long long start, elapsed;
    int retval;
    dict *dict = dictCreate(&BenchmarkDictType);
    long count = 0;
    unsigned long new_dict_size, current_dict_used, remain_keys;
    int accurate = (flags & UNIT_TEST_ACCURATE);

    if (argc == 4) {
        if (accurate) {
            count = 5000000;
        } else {
            count = strtol(argv[3], NULL, 10);
        }
    } else {
        count = 5000;
    }

    monotonicInit(); // To prevent SIGSEV when calling dictRehashMicroseconds()

    TEST("Add 16 keys and verify dict resize is ok") {
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
        for (j = 0; j < 16; j++) {
            retval = dictAdd(dict, stringFromLongLong(j), (void *)j);
            TEST_ASSERT(retval == DICT_OK);
        }
        while (dictIsRehashing(dict)) dictRehashMicroseconds(dict, 1000);
        TEST_ASSERT(dictSize(dict) == 16);
        TEST_ASSERT(dictBuckets(dict) == 16);
    }

    TEST("Use DICT_RESIZE_AVOID to disable the dict resize and pad to (dict_force_resize_ratio * 16)") {
        /* Use DICT_RESIZE_AVOID to disable the dict resize, and pad
         * the number of keys to (dict_force_resize_ratio * 16), so we can satisfy
         * dict_force_resize_ratio in next test. */
        dictSetResizeEnabled(DICT_RESIZE_AVOID);
        for (j = 16; j < (long)dict_force_resize_ratio * 16; j++) {
            retval = dictAdd(dict, stringFromLongLong(j), (void *)j);
            TEST_ASSERT(retval == DICT_OK);
        }
        current_dict_used = dict_force_resize_ratio * 16;
        TEST_ASSERT(dictSize(dict) == current_dict_used);
        TEST_ASSERT(dictBuckets(dict) == 16);
    }

    TEST("Add one more key, trigger the dict resize") {
        retval = dictAdd(dict, stringFromLongLong(current_dict_used), (void *)(current_dict_used));
        TEST_ASSERT(retval == DICT_OK);
        current_dict_used++;
        new_dict_size = 1UL << _dictNextExp(current_dict_used);
        TEST_ASSERT(dictSize(dict) == current_dict_used);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[0]) == 16);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[1]) == new_dict_size);

        /* Wait for rehashing. */
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
        while (dictIsRehashing(dict)) dictRehashMicroseconds(dict, 1000);
        TEST_ASSERT(dictSize(dict) == current_dict_used);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[0]) == new_dict_size);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[1]) == 0);
    }

    TEST("Delete keys until we can trigger shrink in next test") {
        /* Delete keys until we can satisfy (1 / HASHTABLE_MIN_FILL) in the next test. */
        for (j = new_dict_size / HASHTABLE_MIN_FILL + 1; j < (long)current_dict_used; j++) {
            char *key = stringFromLongLong(j);
            retval = dictDelete(dict, key);
            zfree(key);
            TEST_ASSERT(retval == DICT_OK);
        }
        current_dict_used = new_dict_size / HASHTABLE_MIN_FILL + 1;
        TEST_ASSERT(dictSize(dict) == current_dict_used);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[0]) == new_dict_size);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[1]) == 0);
    }

    TEST("Delete one more key, trigger the dict resize") {
        current_dict_used--;
        char *key = stringFromLongLong(current_dict_used);
        retval = dictDelete(dict, key);
        zfree(key);
        unsigned long oldDictSize = new_dict_size;
        new_dict_size = 1UL << _dictNextExp(current_dict_used);
        TEST_ASSERT(retval == DICT_OK);
        TEST_ASSERT(dictSize(dict) == current_dict_used);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[0]) == oldDictSize);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[1]) == new_dict_size);

        /* Wait for rehashing. */
        while (dictIsRehashing(dict)) dictRehashMicroseconds(dict, 1000);
        TEST_ASSERT(dictSize(dict) == current_dict_used);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[0]) == new_dict_size);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[1]) == 0);
    }

    TEST("Empty the dictionary and add 128 keys") {
        dictEmpty(dict, NULL);
        for (j = 0; j < 128; j++) {
            retval = dictAdd(dict, stringFromLongLong(j), (void *)j);
            TEST_ASSERT(retval == DICT_OK);
        }
        while (dictIsRehashing(dict)) dictRehashMicroseconds(dict, 1000);
        TEST_ASSERT(dictSize(dict) == 128);
        TEST_ASSERT(dictBuckets(dict) == 128);
    }

    TEST("Use DICT_RESIZE_AVOID to disable the dict resize and reduce to 3") {
        /* Use DICT_RESIZE_AVOID to disable the dict reset, and reduce
         * the number of keys until we can trigger shrinking in next test. */
        dictSetResizeEnabled(DICT_RESIZE_AVOID);
        remain_keys = DICTHT_SIZE(dict->ht_size_exp[0]) / (HASHTABLE_MIN_FILL * dict_force_resize_ratio) + 1;
        for (j = remain_keys; j < 128; j++) {
            char *key = stringFromLongLong(j);
            retval = dictDelete(dict, key);
            zfree(key);
            TEST_ASSERT(retval == DICT_OK);
        }
        current_dict_used = remain_keys;
        TEST_ASSERT(dictSize(dict) == remain_keys);
        TEST_ASSERT(dictBuckets(dict) == 128);
    }

    TEST("Delete one more key, trigger the dict resize") {
        current_dict_used--;
        char *key = stringFromLongLong(current_dict_used);
        retval = dictDelete(dict, key);
        zfree(key);
        new_dict_size = 1UL << _dictNextExp(current_dict_used);
        TEST_ASSERT(retval == DICT_OK);
        TEST_ASSERT(dictSize(dict) == current_dict_used);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[0]) == 128);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[1]) == new_dict_size);

        /* Wait for rehashing. */
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
        while (dictIsRehashing(dict)) dictRehashMicroseconds(dict, 1000);
        TEST_ASSERT(dictSize(dict) == current_dict_used);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[0]) == new_dict_size);
        TEST_ASSERT(DICTHT_SIZE(dict->ht_size_exp[1]) == 0);
    }

    TEST("Restore to original state") {
        dictEmpty(dict, NULL);
        dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        retval = dictAdd(dict, stringFromLongLong(j), (void *)j);
        TEST_ASSERT(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    TEST_ASSERT((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMicroseconds(dict, 100 * 1000);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict, key);
        TEST_ASSERT(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict, key);
        TEST_ASSERT(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        dictEntry *de = dictFind(dict, key);
        TEST_ASSERT(de != NULL);
        zfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        dictEntry *de = dictGetRandomKey(dict);
        TEST_ASSERT(de != NULL);
    }
    end_benchmark("Accessing random keys");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict, key);
        TEST_ASSERT(de == NULL);
        zfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        retval = dictDelete(dict, key);
        TEST_ASSERT(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict, key, (void *)j);
        TEST_ASSERT(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
    dictRelease(dict);
    return 0;
}