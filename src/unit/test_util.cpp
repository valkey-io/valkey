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

    /* Check if /tmp is memory-backed (e.g., tmpfs) */
    if (statfs("/tmp", &stats) == 0) {
        if (stats.f_type != TMPFS_MAGIC) { // Not tmpfs, use /tmp
            GTEST_SKIP() << "Skipping test because /tmp is not tmpfs";
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
