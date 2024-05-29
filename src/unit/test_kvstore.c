#include "../kvstore.c"
#include "test_help.h"

uint64_t hashTestCallback(const void *key) {
    return dictGenHashFunction((unsigned char *)key, strlen((char *)key));
}

void freeTestCallback(dict *d, void *val) {
    UNUSED(d);
    zfree(val);
}

dictType KvstoreDictTestType = {hashTestCallback, NULL, NULL, NULL, freeTestCallback, NULL, NULL};

char *stringFromInt(int value) {
    char buf[32];
    int len;
    char *s;

    len = snprintf(buf, sizeof(buf), "%d", value);
    s = zmalloc(len + 1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

int test_kvstoreAdd16Keys(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int i;
    dictEntry *de;

    int didx = 0;
    kvstore *kvs1 = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND);
    kvstore *kvs2 = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND | KVSTORE_FREE_EMPTY_DICTS);

    for (i = 0; i < 16; i++) {
        de = kvstoreDictAddRaw(kvs1, didx, stringFromInt(i), NULL);
        TEST_ASSERT(de != NULL);
        de = kvstoreDictAddRaw(kvs2, didx, stringFromInt(i), NULL);
        TEST_ASSERT(de != NULL);
    }
    TEST_ASSERT(kvstoreDictSize(kvs1, didx) == 16);
    TEST_ASSERT(kvstoreSize(kvs1) == 16);
    TEST_ASSERT(kvstoreDictSize(kvs2, didx) == 16);
    TEST_ASSERT(kvstoreSize(kvs2) == 16);

    kvstoreRelease(kvs1);
    kvstoreRelease(kvs2);
    return 0;
}

int test_kvstoreIteratorRemoveAllKeysNoDeleteEmptyDict(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int i;
    void *key;
    dictEntry *de;
    kvstoreIterator *kvs_it;

    int didx = 0;
    int curr_slot = 0;
    kvstore *kvs1 = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND);

    for (i = 0; i < 16; i++) {
        de = kvstoreDictAddRaw(kvs1, didx, stringFromInt(i), NULL);
        TEST_ASSERT(de != NULL);
    }

    kvs_it = kvstoreIteratorInit(kvs1);
    while ((de = kvstoreIteratorNext(kvs_it)) != NULL) {
        curr_slot = kvstoreIteratorGetCurrentDictIndex(kvs_it);
        key = dictGetKey(de);
        TEST_ASSERT(kvstoreDictDelete(kvs1, curr_slot, key) == DICT_OK);
    }
    kvstoreIteratorRelease(kvs_it);

    dict *d = kvstoreGetDict(kvs1, didx);
    TEST_ASSERT(d != NULL);
    TEST_ASSERT(kvstoreDictSize(kvs1, didx) == 0);
    TEST_ASSERT(kvstoreSize(kvs1) == 0);

    kvstoreRelease(kvs1);
    return 0;
}

int test_kvstoreIteratorRemoveAllKeysDeleteEmptyDict(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int i;
    void *key;
    dictEntry *de;
    kvstoreIterator *kvs_it;

    int didx = 0;
    int curr_slot = 0;
    kvstore *kvs2 = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND | KVSTORE_FREE_EMPTY_DICTS);

    for (i = 0; i < 16; i++) {
        de = kvstoreDictAddRaw(kvs2, didx, stringFromInt(i), NULL);
        TEST_ASSERT(de != NULL);
    }

    kvs_it = kvstoreIteratorInit(kvs2);
    while ((de = kvstoreIteratorNext(kvs_it)) != NULL) {
        curr_slot = kvstoreIteratorGetCurrentDictIndex(kvs_it);
        key = dictGetKey(de);
        TEST_ASSERT(kvstoreDictDelete(kvs2, curr_slot, key) == DICT_OK);
    }
    kvstoreIteratorRelease(kvs_it);

    /* Make sure the dict was removed from the rehashing list. */
    while (kvstoreIncrementallyRehash(kvs2, 1000)) {
    }

    dict *d = kvstoreGetDict(kvs2, didx);
    TEST_ASSERT(d == NULL);
    TEST_ASSERT(kvstoreDictSize(kvs2, didx) == 0);
    TEST_ASSERT(kvstoreSize(kvs2) == 0);

    kvstoreRelease(kvs2);
    return 0;
}

int test_kvstoreDictIteratorRemoveAllKeysNoDeleteEmptyDict(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int i;
    void *key;
    dictEntry *de;
    kvstoreDictIterator *kvs_di;

    int didx = 0;
    kvstore *kvs1 = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND);

    for (i = 0; i < 16; i++) {
        de = kvstoreDictAddRaw(kvs1, didx, stringFromInt(i), NULL);
        TEST_ASSERT(de != NULL);
    }

    kvs_di = kvstoreGetDictSafeIterator(kvs1, didx);
    while ((de = kvstoreDictIteratorNext(kvs_di)) != NULL) {
        key = dictGetKey(de);
        TEST_ASSERT(kvstoreDictDelete(kvs1, didx, key) == DICT_OK);
    }
    kvstoreReleaseDictIterator(kvs_di);

    dict *d = kvstoreGetDict(kvs1, didx);
    TEST_ASSERT(d != NULL);
    TEST_ASSERT(kvstoreDictSize(kvs1, didx) == 0);
    TEST_ASSERT(kvstoreSize(kvs1) == 0);

    kvstoreRelease(kvs1);
    return 0;
}

int test_kvstoreDictIteratorRemoveAllKeysDeleteEmptyDict(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int i;
    void *key;
    dictEntry *de;
    kvstoreDictIterator *kvs_di;

    int didx = 0;
    kvstore *kvs2 = kvstoreCreate(&KvstoreDictTestType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND | KVSTORE_FREE_EMPTY_DICTS);

    for (i = 0; i < 16; i++) {
        de = kvstoreDictAddRaw(kvs2, didx, stringFromInt(i), NULL);
        TEST_ASSERT(de != NULL);
    }

    kvs_di = kvstoreGetDictSafeIterator(kvs2, didx);
    while ((de = kvstoreDictIteratorNext(kvs_di)) != NULL) {
        key = dictGetKey(de);
        TEST_ASSERT(kvstoreDictDelete(kvs2, didx, key) == DICT_OK);
    }
    kvstoreReleaseDictIterator(kvs_di);

    dict *d = kvstoreGetDict(kvs2, didx);
    TEST_ASSERT(d == NULL);
    TEST_ASSERT(kvstoreDictSize(kvs2, didx) == 0);
    TEST_ASSERT(kvstoreSize(kvs2) == 0);

    kvstoreRelease(kvs2);
    return 0;
}
