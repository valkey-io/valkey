#include <time.h>
#include <stdint.h>

#include "test_help.h"

#include "../config.h"
#include "../zmalloc.h"

extern long long serverPopcount(void *s, long count);

static long long bitcount(void *s, long count) {
    long long bits = 0;
    uint8_t *p = (uint8_t *)s;
    for (int x = 0; x < count; x += 1) {
        uint8_t val = *(x + p);
        while (val) {
            bits += val & 1;
            val >>= 1;
        }
    }
    return bits;
}

static int test_case(const char *msg, int size) {
    size_t bufsize = size > 0 ? size : 1;
    uint8_t buf[bufsize];
    int fuzzing = 1000;
    for (int y = 0; y < fuzzing; y += 1) {
        for (int z = 0; z < size; z += 1) {
            buf[z] = random() % 256;
        }

        long long expect = bitcount(buf, size);
        long long ret = serverPopcount(buf, size);
        TEST_ASSERT_MESSAGE(msg, expect == ret);
    }

    return 0;
}

int test_popcount(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

#define TEST_CASE(MSG, SIZE)                    \
    if (test_case("Test failed: " MSG, SIZE)) { \
        return 1;                               \
    }

    /* The AVX2 version divides the array into the following 3 parts."
     *        Part A         Part B       Part C
     * +-----------------+--------------+---------+
     * | 8 * 32bytes * X |  32bytes * Y | Z bytes |
     * +-----------------+--------------+---------+
     */
    /* So we test the following cases */
    TEST_CASE("Popcount(Part A)", 8 * 32 * 2);
    TEST_CASE("Popcount(Part B)", 32 * 2);
    TEST_CASE("Popcount(Part C)", 2);
    TEST_CASE("Popcount(Part A + Part B)", 8 * 32 * 7 + 32 * 2);
    TEST_CASE("Popcount(Part A + Part C)", 8 * 32 * 11 + 7);
    TEST_CASE("Popcount(Part A + Part B + Part C)", 8 * 32 * 3 + 3 * 32 + 5);
    TEST_CASE("Popcount(Corner case)", 0);

    /* Test cases for AVX512 */
    TEST_CASE("Popcount(64-byte aligned)", 64 * 2);
    TEST_CASE("Popcount(64-byte + small remainder)", 64 * 2 + 2);
    TEST_CASE("Popcount(64-byte + small remainder)", 64 * 2 + 5);
#undef TEST_CASE

    return 0;
}
