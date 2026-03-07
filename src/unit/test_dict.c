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
