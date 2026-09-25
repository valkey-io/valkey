/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
#include "config.h"
#include "fmacros.h"
#include "server.h"
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

    v = 0.0L / 0.0L;
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

extern "C" void nolocks_localtime(struct tm *tmp, time_t t, long utc_offset);

/* Whether this host's tz database knows the zone TZ is currently set to. An
 * unknown zone makes localtime_r() fall back to UTC silently, so a test that
 * compares two libc-derived values would pass without covering the zone. Every
 * non-UTC zone used by the tests below is away from UTC at 2026-07-15 03:00Z
 * (Dublin is at +01:00 in July), so a wall clock of 03:00 there means UTC. */
static int tzKnownToHost(const char *tz) {
    const time_t jul15 = 1784084400;
    if (strcmp(tz, "UTC") == 0) return 1;
    struct tm probe;
    localtime_r(&jul15, &probe);
    return !(probe.tm_hour == 3 && probe.tm_min == 0);
}

/* utcOffsetFromLocaltime() must return the actual offset of local time east of
 * UTC, with whatever daylight-saving shape tzdata applies. The zones below cover
 * the cases a "standard offset + 3600 * tm_isdst" model gets wrong: Europe/Dublin
 * (tzdata models winter as negative DST: tm_isdst=1 with offset 0) and Lord Howe
 * Island (30-minute DST), alongside ordinary zones in both hemispheres,
 * half-hour and 45-minute offsets, and the extremes.
 *
 * Instants: 2026-01-15 03:00Z, 2026-07-15 03:00Z (opposite DST states per
 * hemisphere), 2026-12-31 23:30Z and 2026-01-01 00:30Z (local and UTC dates
 * straddle a year boundary). */
TEST_F(UtilTest, TestUtcOffsetFromLocaltime) {
    const time_t jan15 = 1768446000, jul15 = 1784084400, dec31 = 1798759800, jan1 = 1767227400;
    struct Case {
        const char *tz;
        long jul_east; /* offset at 2026-07-15 */
        long jan_east; /* offset at 2026-01-15, 2026-12-31 and 2026-01-01 (same season, both hemispheres) */
    };
    const Case cases[] = {
        {"UTC", 0, 0},
        {"America/Los_Angeles", -7 * 3600, -8 * 3600},
        {"America/New_York", -4 * 3600, -5 * 3600},
        {"America/St_Johns", -(2 * 3600 + 1800), -(3 * 3600 + 1800)},
        {"Europe/Stockholm", 2 * 3600, 1 * 3600},
        {"Europe/Dublin", 1 * 3600, 0}, /* negative DST in tzdata: winter is the "DST" period at +00:00 */
        {"Asia/Kolkata", 5 * 3600 + 1800, 5 * 3600 + 1800},
        {"Asia/Kathmandu", 5 * 3600 + 2700, 5 * 3600 + 2700},
        {"Australia/Adelaide", 9 * 3600 + 1800, 10 * 3600 + 1800}, /* southern: DST in January */
        {"Australia/Lord_Howe", 10 * 3600 + 1800, 11 * 3600},      /* 30-minute DST, in January */
        {"Pacific/Auckland", 12 * 3600, 13 * 3600},
        {"Pacific/Chatham", 12 * 3600 + 2700, 13 * 3600 + 2700},
        {"Pacific/Kiritimati", 14 * 3600, 14 * 3600},
        {"Etc/GMT+12", -12 * 3600, -12 * 3600},
    };

    const time_t instants[] = {jan15, jul15, dec31, jan1};

    const char *saved_tz = getenv("TZ");
    sds saved = saved_tz ? sdsnew(saved_tz) : NULL;
    int zones_covered = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const Case *c = &cases[i];
        setenv("TZ", c->tz, 1);
        tzset();
        if (!tzKnownToHost(c->tz)) continue;
        zones_covered++;

        EXPECT_EQ(utcOffsetFromLocaltime(jan15), c->jan_east) << c->tz << " at 2026-01-15";
        EXPECT_EQ(utcOffsetFromLocaltime(jul15), c->jul_east) << c->tz << " at 2026-07-15";
        EXPECT_EQ(utcOffsetFromLocaltime(dec31), c->jan_east) << c->tz << " at 2026-12-31T23:30Z";
        EXPECT_EQ(utcOffsetFromLocaltime(jan1), c->jan_east) << c->tz << " at 2026-01-01T00:30Z";

        /* The lock-free converter fed with that offset must reproduce libc's wall clock. */
        for (size_t j = 0; j < sizeof(instants) / sizeof(instants[0]); j++) {
            time_t t = instants[j];
            struct tm expected, got;
            localtime_r(&t, &expected);
            nolocks_localtime(&got, t, utcOffsetFromLocaltime(t));
            EXPECT_EQ(got.tm_year, expected.tm_year) << c->tz << " at " << t;
            EXPECT_EQ(got.tm_yday, expected.tm_yday) << c->tz << " at " << t;
            EXPECT_EQ(got.tm_hour, expected.tm_hour) << c->tz << " at " << t;
            EXPECT_EQ(got.tm_min, expected.tm_min) << c->tz << " at " << t;
        }
    }

    if (saved)
        setenv("TZ", saved, 1);
    else
        unsetenv("TZ");
    tzset();
    sdsfree(saved);
    /* UTC alone proves nothing about daylight-saving handling. */
    if (zones_covered <= 1) GTEST_SKIP() << "host tz database has none of the zones under test";
}

extern "C" void formatTimezone(char *buf, size_t buflen, long utc_offset);

/* The ISO 8601 log suffix must render the offset's minutes, not just whole hours. */
TEST_F(UtilTest, TestFormatTimezone) {
    struct Case {
        long utc_offset;
        const char *expected;
    };
    const Case cases[] = {
        {0, "+00:00"},
        {3600, "+01:00"},
        {-5 * 3600, "-05:00"},
        {10 * 3600 + 1800, "+10:30"},   /* Lord Howe summer */
        {-(3 * 3600 + 1800), "-03:30"}, /* Newfoundland */
        {5 * 3600 + 2700, "+05:45"},    /* Nepal */
        {12 * 3600 + 2700, "+12:45"},   /* Chatham */
        {14 * 3600, "+14:00"},          /* Kiritimati */
        {-12 * 3600, "-12:00"},         /* Etc/GMT+12 */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char buf[7];
        formatTimezone(buf, sizeof(buf), cases[i].utc_offset);
        EXPECT_STREQ(buf, cases[i].expected) << "offset " << cases[i].utc_offset;
    }
}

/* updateCachedTime(1) must publish the offset the logger reads: for the cached
 * unixtime, the lock-free converter fed with server.utc_offset must agree with
 * libc's localtime_r under the current TZ. */
TEST_F(UtilTest, TestUpdateCachedTimeRefreshesUtcOffset) {
    const char *zones[] = {"UTC", "America/St_Johns", "Europe/Dublin", "Australia/Lord_Howe"};
    const char *saved_tz = getenv("TZ");
    sds saved = saved_tz ? sdsnew(saved_tz) : NULL;
    int zones_covered = 0;

    for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
        const char *tz = zones[i];
        setenv("TZ", tz, 1);
        tzset();
        if (!tzKnownToHost(tz)) continue;
        zones_covered++;
        updateCachedTime(1);
        time_t now = server.unixtime;
        long cached = server.utc_offset; /* plain read: the test build maps _Atomic(T) to T */

        struct tm expected, got;
        localtime_r(&now, &expected);
        nolocks_localtime(&got, now, cached);
        EXPECT_EQ(cached, utcOffsetFromLocaltime(now)) << tz;
        EXPECT_EQ(got.tm_yday, expected.tm_yday) << tz;
        EXPECT_EQ(got.tm_hour, expected.tm_hour) << tz;
        EXPECT_EQ(got.tm_min, expected.tm_min) << tz;
    }

    if (saved)
        setenv("TZ", saved, 1);
    else
        unsetenv("TZ");
    tzset();
    updateCachedTime(1);
    sdsfree(saved);
    if (zones_covered <= 1) GTEST_SKIP() << "host tz database has none of the zones under test";
}
