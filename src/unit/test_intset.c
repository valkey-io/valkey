#include <sys/time.h>
#include <time.h>

#include "../intset.c"
#include "test_help.h"
#if defined(__GNUC__) && __GNUC__ >= 7
/* Several functions in this file get inlined in such a way that fortify warns there might
 * be an out of bounds memory access depending on the intset encoding, but they aren't actually
 * reachable because we check the encoding. There are other strategies to fix this, but they
 * all require other hacks to prevent the inlining. So for now, just omit the check. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec) * 1000000) + tv.tv_usec;
}

static intset *createSet(int bits, int size) {
    uint64_t mask = (1 << bits) - 1;
    uint64_t value;
    intset *is = intsetNew();

    for (int i = 0; i < size; i++) {
        if (bits > 32) {
            value = (rand() * rand()) & mask;
        } else {
            value = rand() & mask;
        }
        is = intsetAdd(is, value, NULL);
    }
    return is;
}

static int checkConsistency(intset *is) {
    for (uint32_t i = 0; i < (intrev32ifbe(is->length) - 1); i++) {
        uint32_t encoding = intrev32ifbe(is->encoding);

        if (encoding == INTSET_ENC_INT16) {
            int16_t *i16 = (int16_t *)is->contents;
            TEST_ASSERT(i16[i] < i16[i + 1]);
        } else if (encoding == INTSET_ENC_INT32) {
            int32_t *i32 = (int32_t *)is->contents;
            TEST_ASSERT(i32[i] < i32[i + 1]);
        } else {
            int64_t *i64 = (int64_t *)is->contents;
            TEST_ASSERT(i64[i] < i64[i + 1]);
        }
    }
    return 1;
}

int test_intsetValueEncodings(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    TEST_ASSERT(_intsetValueEncoding(-32768) == INTSET_ENC_INT16);
    TEST_ASSERT(_intsetValueEncoding(+32767) == INTSET_ENC_INT16);
    TEST_ASSERT(_intsetValueEncoding(-32769) == INTSET_ENC_INT32);
    TEST_ASSERT(_intsetValueEncoding(+32768) == INTSET_ENC_INT32);
    TEST_ASSERT(_intsetValueEncoding(-2147483648) == INTSET_ENC_INT32);
    TEST_ASSERT(_intsetValueEncoding(+2147483647) == INTSET_ENC_INT32);
    TEST_ASSERT(_intsetValueEncoding(-2147483649) == INTSET_ENC_INT64);
    TEST_ASSERT(_intsetValueEncoding(+2147483648) == INTSET_ENC_INT64);
    TEST_ASSERT(_intsetValueEncoding(-9223372036854775808ull) == INTSET_ENC_INT64);
    TEST_ASSERT(_intsetValueEncoding(+9223372036854775807ull) == INTSET_ENC_INT64);

    return 0;
}

int test_intsetBasicAdding(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    intset *is = intsetNew();
    uint8_t success;
    is = intsetAdd(is, 5, &success);
    TEST_ASSERT(success);
    is = intsetAdd(is, 6, &success);
    TEST_ASSERT(success);
    is = intsetAdd(is, 4, &success);
    TEST_ASSERT(success);
    is = intsetAdd(is, 4, &success);
    TEST_ASSERT(!success);
    TEST_ASSERT(6 == intsetMax(is));
    TEST_ASSERT(4 == intsetMin(is));
    zfree(is);

    return 0;
}

int test_intsetLargeNumberRandomAdd(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    uint32_t inserts = 0;
    uint8_t success;
    intset *is = intsetNew();
    for (int i = 0; i < 1024; i++) {
        is = intsetAdd(is, rand() % 0x800, &success);
        if (success) inserts++;
    }
    TEST_ASSERT(intrev32ifbe(is->length) == inserts);
    TEST_ASSERT(checkConsistency(is) == 1);
    zfree(is);
    return 0;
}

int test_intsetUpgradeFromint16Toint32(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    intset *is = intsetNew();
    is = intsetAdd(is, 32, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
    is = intsetAdd(is, 65535, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
    TEST_ASSERT(intsetFind(is, 32));
    TEST_ASSERT(intsetFind(is, 65535));
    TEST_ASSERT(checkConsistency(is) == 1);
    zfree(is);

    is = intsetNew();
    is = intsetAdd(is, 32, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
    is = intsetAdd(is, -65535, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
    TEST_ASSERT(intsetFind(is, 32));
    TEST_ASSERT(intsetFind(is, -65535));
    TEST_ASSERT(checkConsistency(is) == 1);
    zfree(is);

    return 0;
}

int test_intsetUpgradeFromint16Toint64(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    intset *is = intsetNew();
    is = intsetAdd(is, 32, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
    is = intsetAdd(is, 4294967295, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
    TEST_ASSERT(intsetFind(is, 32));
    TEST_ASSERT(intsetFind(is, 4294967295));
    TEST_ASSERT(checkConsistency(is) == 1);
    zfree(is);

    is = intsetNew();
    is = intsetAdd(is, 32, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
    is = intsetAdd(is, -4294967295, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
    TEST_ASSERT(intsetFind(is, 32));
    TEST_ASSERT(intsetFind(is, -4294967295));
    TEST_ASSERT(checkConsistency(is) == 1);
    zfree(is);

    return 0;
}

int test_intsetUpgradeFromint32Toint64(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    intset *is = intsetNew();
    is = intsetAdd(is, 65535, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
    is = intsetAdd(is, 4294967295, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
    TEST_ASSERT(intsetFind(is, 65535));
    TEST_ASSERT(intsetFind(is, 4294967295));
    TEST_ASSERT(checkConsistency(is) == 1);
    zfree(is);

    is = intsetNew();
    is = intsetAdd(is, 65535, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
    is = intsetAdd(is, -4294967295, NULL);
    TEST_ASSERT(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
    TEST_ASSERT(intsetFind(is, 65535));
    TEST_ASSERT(intsetFind(is, -4294967295));
    TEST_ASSERT(checkConsistency(is) == 1);
    zfree(is);

    return 0;
}

int test_intsetStressLookups(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    long num = 100000, size = 10000;
    int i, bits = 20;
    long long start;
    intset *is = createSet(bits, size);
    TEST_ASSERT(checkConsistency(is) == 1);

    start = usec();
    for (i = 0; i < num; i++) intsetSearch(is, rand() % ((1 << bits) - 1), NULL);
    TEST_PRINT_INFO("%ld lookups, %ld element set, %lldusec\n", num, size, usec() - start);
    zfree(is);

    return 0;
}

int test_intsetStressAddDelete(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int i, v1, v2;
    intset *is = intsetNew();
    for (i = 0; i < 0xffff; i++) {
        v1 = rand() % 0xfff;
        is = intsetAdd(is, v1, NULL);
        TEST_ASSERT(intsetFind(is, v1));

        v2 = rand() % 0xfff;
        is = intsetRemove(is, v2, NULL);
        TEST_ASSERT(!intsetFind(is, v2));
    }
    TEST_ASSERT(checkConsistency(is) == 1);
    zfree(is);

    return 0;
}

#if defined(__GNUC__) && __GNUC__ >= 12
#pragma GCC diagnostic pop
#endif
