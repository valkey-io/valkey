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

/* stringmatchlen() has two "[...]" class parsers that must stay byte-for-byte
 * equivalent: an inline one used for every pattern, and a cached one
 * (parseCharClass(), reached only when the pattern also contains a '*') used
 * to avoid re-parsing the class on every backtrack position. Both are
 * static, so they can only be exercised here through the public API, by
 * comparing the same class matched via each path.
 *
 * The trick to force one call down each path while keeping the matched set
 * identical: append a fixed-length literal suffix after the class, and use a
 * string exactly as long as "<class><suffix>" requires. A leading '*' can
 * then only ever bind zero characters (there's no slack for it to consume
 * without leaving the mandatory suffix unmatched), so "*<class><suffix>"
 * (cache-eligible: '*' followed later by '[') and "<class><suffix>" (no '*',
 * always the inline parser) must return identical results for every string
 * that's the right length. */
#define CLASS_TEST_MAX_PATTERN 32
static void assertClassCacheAgreesWithInline(const char *label, const char *classPattern, int classPatternLen, int nocase) {
    char withStar[CLASS_TEST_MAX_PATTERN];
    char withoutStar[CLASS_TEST_MAX_PATTERN];
    int withStarLen = 0;
    int withoutStarLen = 0;

    withStar[withStarLen++] = '*';
    memcpy(withStar + withStarLen, classPattern, classPatternLen);
    withStarLen += classPatternLen;
    withStar[withStarLen++] = 'X';

    memcpy(withoutStar, classPattern, classPatternLen);
    withoutStarLen = classPatternLen;
    withoutStar[withoutStarLen++] = 'X';

    for (int b = 0; b < 256; b++) {
        char string[2] = {static_cast<char>(b), 'X'};
        int cached = stringmatchlen(withStar, withStarLen, string, 2, nocase);
        int inlineResult = stringmatchlen(withoutStar, withoutStarLen, string, 2, nocase);
        ASSERT_EQ(cached, inlineResult) << "case=" << label << " nocase=" << nocase << " byte=" << b;
    }
}

TEST_F(UtilTest, TestStringMatchClassCacheAgreesWithInlineFullByteRange) {
    /* Plain ranges, including ones spanning the signed/unsigned char
     * boundary (0x80) that previously diverged between the two parsers. */
    assertClassCacheAgreesWithInline("a-z", "[a-z]", (int)sizeof("[a-z]") - 1, 0);
    assertClassCacheAgreesWithInline("a-z nocase", "[a-z]", (int)sizeof("[a-z]") - 1, 1);
    assertClassCacheAgreesWithInline("a-\\xff", "[a-\xff]", (int)sizeof("[a-\xff]") - 1, 0);
    assertClassCacheAgreesWithInline("a-\\xff nocase", "[a-\xff]", (int)sizeof("[a-\xff]") - 1, 1);
    assertClassCacheAgreesWithInline("\\x80-\\xff", "[\x80-\xff]", (int)sizeof("[\x80-\xff]") - 1, 0);
    assertClassCacheAgreesWithInline("\\x80-\\xff nocase", "[\x80-\xff]", (int)sizeof("[\x80-\xff]") - 1, 1);
    assertClassCacheAgreesWithInline("reversed \\xff-\\x80", "[\xff-\x80]", (int)sizeof("[\xff-\x80]") - 1, 0);

    /* Negation. */
    assertClassCacheAgreesWithInline("^a-c", "[^a-c]", (int)sizeof("[^a-c]") - 1, 0);
    assertClassCacheAgreesWithInline("^a-c nocase", "[^a-c]", (int)sizeof("[^a-c]") - 1, 1);
    assertClassCacheAgreesWithInline("^\\x80-\\xff", "[^\x80-\xff]", (int)sizeof("[^\x80-\xff]") - 1, 0);

    /* Literal high-byte members, and escapes (case-sensitive even under
     * nocase, matching the inline parser's existing behavior). */
    assertClassCacheAgreesWithInline("literal high bytes", "[\x80\x90\xff]", (int)sizeof("[\x80\x90\xff]") - 1, 0);
    assertClassCacheAgreesWithInline("literal high bytes nocase", "[\x80\x90\xff]",
                                     (int)sizeof("[\x80\x90\xff]") - 1, 1);
    assertClassCacheAgreesWithInline("escapes", "[\\a\\b\\c]", (int)sizeof("[\\a\\b\\c]") - 1, 0);
    assertClassCacheAgreesWithInline("escapes nocase", "[\\a\\b\\c]", (int)sizeof("[\\a\\b\\c]") - 1, 1);
}

TEST_F(UtilTest, TestStringMatchClassCacheAgreesWithInlineUnterminated) {
    /* An unterminated class consumes the rest of the pattern as members
     * (see the inline parser's patternLen==0 fallback), so it has no
     * trailing anchor to pin the string length the way the helper above
     * does. Test it directly: both "*[abc" (cache-eligible) and "[abc"
     * (always inline) must treat every byte identically. */
    static const char withStar[] = "*[abc";
    static const char withoutStar[] = "[abc";
    for (int b = 0; b < 256; b++) {
        char string[1] = {static_cast<char>(b)};
        int cached = stringmatchlen(withStar, (int)sizeof(withStar) - 1, string, 1, 0);
        int inlineResult = stringmatchlen(withoutStar, (int)sizeof(withoutStar) - 1, string, 1, 0);
        ASSERT_EQ(cached, inlineResult) << "byte=" << b;
    }
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
