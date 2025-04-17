#include "../dict.c"
#include "test_help.h"

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char *)key, strlen((char *)key));
}

int compareCallback(const void *key1, const void *key2) {
    int l1, l2;
    l1 = strlen((char *)key1);
    l2 = strlen((char *)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(void *val) {
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
#define end_benchmark(msg)                                      \
    do {                                                        \
        elapsed = timeInMilliseconds() - start;                 \
        printf(msg ": %ld items in %lld ms\n", count, elapsed); \
    } while (0)

static dict *_dict = NULL;
static long j;
static int retval;
static unsigned long new_dict_size, current_dict_used, remain_keys;

int test_dictCreate(int argc, char **argv, int flags) {
    _dict = dictCreate(&BenchmarkDictType);

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    monotonicInit(); /* Required for dict tests, that are relying on monotime during dict rehashing. */

    return 0;
}

int test_dictAdd16Keys(int argc, char **argv, int flags) {
    /* Add 16 keys and verify dict resize is ok */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    for (j = 0; j < 16; j++) {
        retval = dictAdd(_dict, stringFromLongLong(j), (void *)j);
        TEST_ASSERT(retval == DICT_OK);
    }
    while (dictIsRehashing(_dict)) dictRehashMicroseconds(_dict, 1000);
    TEST_ASSERT(dictSize(_dict) == 16);
    TEST_ASSERT(dictBuckets(_dict) == 16);

    return 0;
}

int test_dictDisableResize(int argc, char **argv, int flags) {
    /* Use DICT_RESIZE_AVOID to disable the dict resize and pad to (dict_force_resize_ratio * 16) */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Use DICT_RESIZE_AVOID to disable the dict resize, and pad
     * the number of keys to (dict_force_resize_ratio * 16), so we can satisfy
     * dict_force_resize_ratio in next test. */
    dictSetResizeEnabled(DICT_RESIZE_AVOID);
    for (j = 16; j < (long)dict_force_resize_ratio * 16; j++) {
        retval = dictAdd(_dict, stringFromLongLong(j), (void *)j);
        TEST_ASSERT(retval == DICT_OK);
    }
    current_dict_used = dict_force_resize_ratio * 16;
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(dictBuckets(_dict) == 16);

    return 0;
}

int test_dictAddOneKeyTriggerResize(int argc, char **argv, int flags) {
    /* Add one more key, trigger the dict resize */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    retval = dictAdd(_dict, stringFromLongLong(current_dict_used), (void *)(current_dict_used));
    TEST_ASSERT(retval == DICT_OK);
    current_dict_used++;
    new_dict_size = 1UL << dictNextExp(current_dict_used);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == 16);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == new_dict_size);

    /* Wait for rehashing. */
    dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    while (dictIsRehashing(_dict)) dictRehashMicroseconds(_dict, 1000);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == new_dict_size);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == 0);

    return 0;
}

int test_dictDeleteKeys(int argc, char **argv, int flags) {
    /* Delete keys until we can trigger shrink in next test */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Delete keys until we can satisfy (1 / HASHTABLE_MIN_FILL) in the next test. */
    for (j = new_dict_size / HASHTABLE_MIN_FILL + 1; j < (long)current_dict_used; j++) {
        char *key = stringFromLongLong(j);
        retval = dictDelete(_dict, key);
        zfree(key);
        TEST_ASSERT(retval == DICT_OK);
    }
    current_dict_used = new_dict_size / HASHTABLE_MIN_FILL + 1;
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == new_dict_size);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == 0);

    return 0;
}

int test_dictDeleteOneKeyTriggerResize(int argc, char **argv, int flags) {
    /* Delete one more key, trigger the dict resize */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    current_dict_used--;
    char *key = stringFromLongLong(current_dict_used);
    retval = dictDelete(_dict, key);
    zfree(key);
    unsigned long oldDictSize = new_dict_size;
    new_dict_size = 1UL << dictNextExp(current_dict_used);
    TEST_ASSERT(retval == DICT_OK);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == oldDictSize);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == new_dict_size);

    /* Wait for rehashing. */
    while (dictIsRehashing(_dict)) dictRehashMicroseconds(_dict, 1000);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == new_dict_size);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == 0);

    return 0;
}

int test_dictEmptyDirAdd128Keys(int argc, char **argv, int flags) {
    /* Empty the dictionary and add 128 keys */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    dictEmpty(_dict, NULL);
    for (j = 0; j < 128; j++) {
        retval = dictAdd(_dict, stringFromLongLong(j), (void *)j);
        TEST_ASSERT(retval == DICT_OK);
    }
    while (dictIsRehashing(_dict)) dictRehashMicroseconds(_dict, 1000);
    TEST_ASSERT(dictSize(_dict) == 128);
    TEST_ASSERT(dictBuckets(_dict) == 128);

    return 0;
}

int test_dictDisableResizeReduceTo3(int argc, char **argv, int flags) {
    /* Use DICT_RESIZE_AVOID to disable the dict resize and reduce to 3 */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Use DICT_RESIZE_AVOID to disable the dict reset, and reduce
     * the number of keys until we can trigger shrinking in next test. */
    dictSetResizeEnabled(DICT_RESIZE_AVOID);
    remain_keys = DICTHT_SIZE(_dict->ht_size_exp[0]) / (HASHTABLE_MIN_FILL * dict_force_resize_ratio) + 1;
    for (j = remain_keys; j < 128; j++) {
        char *key = stringFromLongLong(j);
        retval = dictDelete(_dict, key);
        zfree(key);
        TEST_ASSERT(retval == DICT_OK);
    }
    current_dict_used = remain_keys;
    TEST_ASSERT(dictSize(_dict) == remain_keys);
    TEST_ASSERT(dictBuckets(_dict) == 128);

    return 0;
}

int test_dictDeleteOneKeyTriggerResizeAgain(int argc, char **argv, int flags) {
    /* Delete one more key, trigger the dict resize */
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    current_dict_used--;
    char *key = stringFromLongLong(current_dict_used);
    retval = dictDelete(_dict, key);
    zfree(key);
    new_dict_size = 1UL << dictNextExp(current_dict_used);
    TEST_ASSERT(retval == DICT_OK);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == 128);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == new_dict_size);

    /* Wait for rehashing. */
    dictSetResizeEnabled(DICT_RESIZE_ENABLE);
    while (dictIsRehashing(_dict)) dictRehashMicroseconds(_dict, 1000);
    TEST_ASSERT(dictSize(_dict) == current_dict_used);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[0]) == new_dict_size);
    TEST_ASSERT(DICTHT_SIZE(_dict->ht_size_exp[1]) == 0);

    /* This is the last one, restore to original state */
    dictRelease(_dict);

    return 0;
}

int test_dictBenchmark(int argc, char **argv, int flags) {
    long j;
    long long start, elapsed;
    int retval;
    dict *dict = dictCreate(&BenchmarkDictType);
    long count = 0;
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

    monotonicInit(); /* Required for dict tests, that are relying on monotime during dict rehashing. */

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
