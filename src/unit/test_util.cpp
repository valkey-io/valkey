/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <climits>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
#include "config.h"
#include "fmacros.h"
#include "util.h"

extern bool valgrind;
}

#if defined(__linux__)
#include <linux/magic.h>
#include <sys/statfs.h>
#endif

class UtilTest : public ::testing::Test {};

TEST_F(UtilTest, TestString2ll) {
    char buf[32];
    long long v;

    /* May not start with +. */
    valkey_strlcpy(buf, "+1", sizeof(buf));
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 0);

    /* Leading space. */
    valkey_strlcpy(buf, " 1", sizeof(buf));
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 0);

    /* Trailing space. */
    valkey_strlcpy(buf, "1 ", sizeof(buf));
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 0);

    /* May not start with 0. */
    valkey_strlcpy(buf, "01", sizeof(buf));
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 0);

    valkey_strlcpy(buf, "-1", sizeof(buf));
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, -1);

    valkey_strlcpy(buf, "0", sizeof(buf));
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, 0);

    valkey_strlcpy(buf, "1", sizeof(buf));
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, 1);

    valkey_strlcpy(buf, "99", sizeof(buf));
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, 99);

    valkey_strlcpy(buf, "-99", sizeof(buf));
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, -99);

    valkey_strlcpy(buf, "-9223372036854775808", sizeof(buf));
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, LLONG_MIN);

    valkey_strlcpy(buf, "-9223372036854775809", sizeof(buf)); /* overflow */
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 0);

    valkey_strlcpy(buf, "9223372036854775807", sizeof(buf));
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, LLONG_MAX);

    valkey_strlcpy(buf, "9223372036854775808", sizeof(buf)); /* overflow */
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 0);

    valkey_strlcpy(buf, "18446744073709551615", sizeof(buf)); /* overflow */
    ASSERT_EQ(string2ll(buf, strlen(buf), &v), 0);
}

TEST_F(UtilTest, TestString2l) {
    char buf[32];
    long v;

    /* May not start with +. */
    valkey_strlcpy(buf, "+1", sizeof(buf));
    ASSERT_EQ(string2l(buf, strlen(buf), &v), 0);

    /* May not start with 0. */
    valkey_strlcpy(buf, "01", sizeof(buf));
    ASSERT_EQ(string2l(buf, strlen(buf), &v), 0);

    valkey_strlcpy(buf, "-1", sizeof(buf));
    ASSERT_EQ(string2l(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, -1);

    valkey_strlcpy(buf, "0", sizeof(buf));
    ASSERT_EQ(string2l(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, 0);

    valkey_strlcpy(buf, "1", sizeof(buf));
    ASSERT_EQ(string2l(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, 1);

    valkey_strlcpy(buf, "99", sizeof(buf));
    ASSERT_EQ(string2l(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, 99);

    valkey_strlcpy(buf, "-99", sizeof(buf));
    ASSERT_EQ(string2l(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, -99);

#if LONG_MAX != LLONG_MAX
    valkey_strlcpy(buf, "-2147483648", sizeof(buf));
    ASSERT_EQ(string2l(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, LONG_MIN);

    valkey_strlcpy(buf, "-2147483649", sizeof(buf)); /* overflow */
    ASSERT_EQ(string2l(buf, strlen(buf), &v), 0);

    valkey_strlcpy(buf, "2147483647", sizeof(buf));
    ASSERT_EQ(string2l(buf, strlen(buf), &v), 1);
    ASSERT_EQ(v, LONG_MAX);

    valkey_strlcpy(buf, "2147483648", sizeof(buf)); /* overflow */
    ASSERT_EQ(string2l(buf, strlen(buf), &v), 0);
#endif
}

TEST_F(UtilTest, TestString2ullBase16AsyncSignalSafe) {
    char buf[32];
    unsigned long long value;

    valkey_strlcpy(buf, "0000010000000000", sizeof(buf));
    ASSERT_EQ(string2ull_base16_async_signal_safe(buf, strlen(buf), &value), 1);
    ASSERT_EQ(value, 1ULL << 40);

    valkey_strlcpy(buf, "ffffffffffffffff", sizeof(buf));
    ASSERT_EQ(string2ull_base16_async_signal_safe(buf, strlen(buf), &value), 1);
    ASSERT_EQ(value, ULLONG_MAX);

    valkey_strlcpy(buf, "10000000000000000", sizeof(buf));
    ASSERT_EQ(string2ull_base16_async_signal_safe(buf, strlen(buf), &value), -1);
}

TEST_F(UtilTest, TestLl2string) {
    char buf[32];
    long long v;
    int sz;

    v = 0;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT_EQ(sz, 1);
    ASSERT_TRUE(!strcmp(buf, "0"));

    v = -1;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT_EQ(sz, 2);
    ASSERT_TRUE(!strcmp(buf, "-1"));

    v = 99;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT_EQ(sz, 2);
    ASSERT_TRUE(!strcmp(buf, "99"));

    v = -99;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT_EQ(sz, 3);
    ASSERT_TRUE(!strcmp(buf, "-99"));

    v = -2147483648;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT_EQ(sz, 11);
    ASSERT_TRUE(!strcmp(buf, "-2147483648"));

    v = LLONG_MIN;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT_EQ(sz, 20);
    ASSERT_TRUE(!strcmp(buf, "-9223372036854775808"));

    v = LLONG_MAX;
    sz = ll2string(buf, sizeof buf, v);
    ASSERT_EQ(sz, 19);
    ASSERT_TRUE(!strcmp(buf, "9223372036854775807"));
}

TEST_F(UtilTest, TestLd2string) {
    char buf[32];
    long double v;
    int sz;

    v = 0.0 / 0.0;
    sz = ld2string(buf, sizeof(buf), v, LD_STR_AUTO);
    ASSERT_EQ(sz, 3);
    ASSERT_TRUE(!strcmp(buf, "nan"));
}

TEST_F(UtilTest, TestFixedpointD2string) {
    char buf[32];
    double v;
    int sz;

    v = 0.0;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    ASSERT_EQ(sz, 6);
    ASSERT_TRUE(!strcmp(buf, "0.0000"));

    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    ASSERT_EQ(sz, 3);
    ASSERT_TRUE(!strcmp(buf, "0.0"));

    /* set junk in buffer */
    memset(buf, 'A', 32);
    v = 0.0001;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    ASSERT_EQ(sz, 6);
    ASSERT_EQ(buf[sz], '\0');
    ASSERT_TRUE(!strcmp(buf, "0.0001"));

    /* set junk in buffer */
    memset(buf, 'A', 32);
    v = 6.0642951598391699e-05;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    ASSERT_EQ(sz, 6);
    ASSERT_EQ(buf[sz], '\0');
    ASSERT_TRUE(!strcmp(buf, "0.0001"));

    v = 0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    ASSERT_EQ(sz, 6);
    ASSERT_TRUE(!strcmp(buf, "0.0100"));

    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    ASSERT_EQ(sz, 3);
    ASSERT_TRUE(!strcmp(buf, "0.0"));

    v = -0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    ASSERT_EQ(sz, 7);
    ASSERT_TRUE(!strcmp(buf, "-0.0100"));

    v = -0.1;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    ASSERT_EQ(sz, 4);
    ASSERT_TRUE(!strcmp(buf, "-0.1"));

    v = 0.1;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    ASSERT_EQ(sz, 3);
    ASSERT_TRUE(!strcmp(buf, "0.1"));

    v = 0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 17);
    ASSERT_EQ(sz, 19);
    ASSERT_TRUE(!strcmp(buf, "0.01000000000000000"));

    v = 10.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    ASSERT_EQ(sz, 7);
    ASSERT_TRUE(!strcmp(buf, "10.0100"));

    /* negative tests */
    sz = fixedpoint_d2string(buf, sizeof buf, v, 18);
    ASSERT_EQ(sz, 0);

    sz = fixedpoint_d2string(buf, sizeof buf, v, 0);
    ASSERT_EQ(sz, 0);

    sz = fixedpoint_d2string(buf, 1, v, 1);
    ASSERT_EQ(sz, 0);
}

TEST_F(UtilTest, TestVersion2num) {
    ASSERT_EQ(version2num("7.2.5"), 0x070205);
    ASSERT_EQ(version2num("255.255.255"), 0xffffff);
    ASSERT_EQ(version2num("7.2.256"), -1);
    ASSERT_EQ(version2num("7.2"), -1);
    ASSERT_EQ(version2num("7.2.1.0"), -1);
    ASSERT_EQ(version2num("1.-2.-3"), -1);
    ASSERT_EQ(version2num("1.2.3-rc4"), -1);
    ASSERT_EQ(version2num(""), -1);
}

#if defined(__linux__)
/* Since fadvise and mincore is only supported in specific platforms like
 * Linux, we only verify the fadvise mechanism works in Linux */
static int cache_exist(int fd) {
    unsigned char flag;
    void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return -1;
    if (mincore(m, 4096, &flag) != 0) {
        munmap(m, 4096);
        return -1;
    }
    munmap(m, 4096);
    /* the least significant bit of the byte will be set if the corresponding
     * page is currently resident in memory */
    return flag & 1;
}
#endif

TEST_F(UtilTest, TestReclaimFilePageCache) {
    /* The test is incompatible with valgrind, skip it. */
    if (valgrind) GTEST_SKIP() << "Skipping test due to incompatibility with valgrind";

#if defined(__linux__)
    struct statfs stats;

    /* fadvise(FADV_DONTNEED) has no effect on memory-backed filesystems */
    if (statfs("/tmp", &stats) == 0) {
        if (stats.f_type == TMPFS_MAGIC) {
            GTEST_SKIP() << "Skipping test because /tmp is tmpfs";
        }
    }

    const char *tmpfile = "/tmp/redis-reclaim-cache-test";
    int fd = open(tmpfile, O_RDWR | O_CREAT, 0644);
    ASSERT_GE(fd, 0);

    /* test write file */
    char buf[4] = "foo";
    ASSERT_GT(write(fd, buf, sizeof(buf)), 0);
    ASSERT_TRUE(cache_exist(fd));
    ASSERT_EQ(valkey_fsync(fd), 0);
    ASSERT_EQ(reclaimFilePageCache(fd, 0, 0), 0);
    ASSERT_TRUE(!cache_exist(fd));

    /* test read file */
    ASSERT_GT(pread(fd, buf, sizeof(buf), 0), 0);
    ASSERT_TRUE(cache_exist(fd));
    ASSERT_EQ(reclaimFilePageCache(fd, 0, 0), 0);
    ASSERT_TRUE(!cache_exist(fd));

    close(fd);
    unlink(tmpfile);
#else
    GTEST_SKIP() << "Test only supported on Linux";
#endif
}

TEST_F(UtilTest, TestStringmatchlenCharClass) {
    /* Basic class membership */
    ASSERT_EQ(1, stringmatchlen("[abc]", 5, "a", 1, 0));
    ASSERT_EQ(1, stringmatchlen("[abc]", 5, "b", 1, 0));
    ASSERT_EQ(0, stringmatchlen("[abc]", 5, "d", 1, 0));
    /* Range */
    ASSERT_EQ(1, stringmatchlen("[a-z]", 5, "m", 1, 0));
    ASSERT_EQ(0, stringmatchlen("[a-z]", 5, "A", 1, 0));
    /* Negation */
    ASSERT_EQ(0, stringmatchlen("[^abc]", 6, "a", 1, 0));
    ASSERT_EQ(1, stringmatchlen("[^abc]", 6, "d", 1, 0));
    /* nocase */
    ASSERT_EQ(1, stringmatchlen("[a-z]", 5, "A", 1, 1));
    ASSERT_EQ(1, stringmatchlen("[\\A]", 4, "a", 1, 1));
    /* Large class: correctness preserved, not just the first 256 */
    char large_pat[40010];
    large_pat[0] = '[';
    /* Place 'z' past byte 256 so a parser that stops at 256 would miss it. */
    memset(large_pat + 1, 'a', 39999);
    large_pat[40000] = 'z';
    large_pat[40001] = ']';
    large_pat[40002] = '\0';
    ASSERT_EQ(1, stringmatchlen(large_pat, 40002, "z", 1, 0));
    ASSERT_EQ(1, stringmatchlen(large_pat, 40002, "a", 1, 0));
    ASSERT_EQ(0, stringmatchlen(large_pat, 40002, "b", 1, 0));
    /* Glob with large class: *[40000×z] against various strings.
     * The bitmask is built once and cached across '*' backtrack positions,
     * so O(class_size + string_len) not O(class_size * string_len). */
    large_pat[0] = '*';
    large_pat[1] = '[';
    memset(large_pat + 2, 'z', 40000);
    large_pat[40002] = ']';
    large_pat[40003] = '\0';
    ASSERT_EQ(1, stringmatchlen(large_pat, 40003, "z", 1, 0));
    ASSERT_EQ(0, stringmatchlen(large_pat, 40003, "abc", 3, 0));
    /* Long non-matching string: without the cache this would be O(N*L). */
    char long_str[256];
    memset(long_str, 'a', sizeof(long_str));
    ASSERT_EQ(0, stringmatchlen(large_pat, 40003, long_str, (int)sizeof(long_str), 0));
    /* Long string ending with z: must match. */
    long_str[255] = 'z';
    ASSERT_EQ(1, stringmatchlen(large_pat, 40003, long_str, (int)sizeof(long_str), 0));
    /* Two large classes: *[40000×a]*[40000×z]
     * The first class matches 'a'; the second fails (string is all 'a').
     * Both classes must be cached independently — the second must not
     * overwrite the first cache entry. */
    char pat2[80010];
    pat2[0] = '*'; pat2[1] = '[';
    memset(pat2 + 2, 'a', 40000);
    pat2[40002] = ']'; pat2[40003] = '*'; pat2[40004] = '[';
    memset(pat2 + 40005, 'z', 40000);
    pat2[80005] = ']'; pat2[80006] = '\0';
    memset(long_str, 'a', sizeof(long_str));  /* reset: prior block left [255]='z' */
    ASSERT_EQ(0, stringmatchlen(pat2, 80006, long_str, (int)sizeof(long_str), 0));
    long_str[255] = 'z';
    ASSERT_EQ(1, stringmatchlen(pat2, 80006, long_str, (int)sizeof(long_str), 0));
}

TEST_F(UtilTest, TestWritePointerWithPadding) {
    unsigned char buf[8];
    static int dummy;
    void *ptr = &dummy;
    size_t ptr_size = sizeof(ptr);

    /* Write the pointer and pad to 8 bytes */
    writePointerWithPadding(buf, ptr);

    /* The first ptr_size bytes must match the raw pointer bytes */
    unsigned char expected[sizeof(ptr)];
    memcpy(expected, &ptr, ptr_size);
    ASSERT_EQ(memcmp(buf, expected, ptr_size), 0);

    /* The remaining bytes (if any) must be zero */
    for (size_t i = ptr_size; i < sizeof(buf); i++) {
        ASSERT_EQ(buf[i], 0u);
    }
}
