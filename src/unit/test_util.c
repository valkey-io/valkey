#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

#include "../config.h"
#include "../util.h"
#include "test_help.h"

#if defined(__linux__)
#include <sys/statfs.h>
#include <linux/magic.h>
#endif

int test_stringmatch(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    TEST_BEGIN();

#define TEST_STRINGMATCH(p, s, expectCase, expectNocase)   \
    do {                                                   \
        TEST_EXPECT(stringmatch(p, s, 0) == expectCase);   \
        TEST_EXPECT(stringmatch(p, s, 1) == expectNocase); \
    } while (0)

    /* Case sensitivity: */
    TEST_STRINGMATCH("a", "a", 1, 1);
    TEST_STRINGMATCH("a", "A", 0, 1);
    TEST_STRINGMATCH("A", "A", 1, 1);
    TEST_STRINGMATCH("A", "a", 0, 1);
    TEST_STRINGMATCH("\\a", "a", 1, 1);
    TEST_STRINGMATCH("\\a", "A", 0, 1);
    TEST_STRINGMATCH("\\A", "A", 1, 1);
    TEST_STRINGMATCH("\\A", "a", 0, 1);
    TEST_STRINGMATCH("[\\a]", "a", 1, 1);
    TEST_STRINGMATCH("[\\a]", "A", 0, 1);
    TEST_STRINGMATCH("[\\A]", "A", 1, 1);
    TEST_STRINGMATCH("[\\A]", "a", 0, 1);

    /* Escaped metacharacters: */
    TEST_STRINGMATCH("\\*", "*", 1, 1);
    TEST_STRINGMATCH("\\?", "?", 1, 1);
    TEST_STRINGMATCH("\\\\", "\\", 1, 1);
    TEST_STRINGMATCH("\\[", "[", 1, 1);
    TEST_STRINGMATCH("\\]", "]", 1, 1);
    TEST_STRINGMATCH("\\^", "^", 1, 1);
    TEST_STRINGMATCH("\\-", "-", 1, 1);
    TEST_STRINGMATCH("[\\*]", "*", 1, 1);
    TEST_STRINGMATCH("[\\?]", "?", 1, 1);
    TEST_STRINGMATCH("[\\\\]", "\\", 1, 1);
    TEST_STRINGMATCH("[\\[]", "[", 1, 1);
    TEST_STRINGMATCH("[\\]]", "]", 1, 1);
    TEST_STRINGMATCH("[\\^]", "^", 1, 1);
    TEST_STRINGMATCH("[\\-]", "-", 1, 1);

    /* Not special outside character classes: */
    TEST_STRINGMATCH("]", "]", 1, 1);
    TEST_STRINGMATCH("^", "^", 1, 1);
    TEST_STRINGMATCH("-", "-", 1, 1);
    /* Not special inside character classes: */
    TEST_STRINGMATCH("[*]", "*", 1, 1);
    TEST_STRINGMATCH("[?]", "?", 1, 1);
    TEST_STRINGMATCH("[[]", "[", 1, 1);
    /* Not special as the first character in a character class: */
    TEST_STRINGMATCH("[-]", "-", 1, 1);

    /* Not special as range end (undocumented): */
    TEST_STRINGMATCH("[+-]]", ",", 1, 1); /* ASCII range + to ] includes , */
    TEST_STRINGMATCH("[+-]]", "*", 0, 0); /*   but not * (below) */
    TEST_STRINGMATCH("[+-]]", "^", 0, 0); /*   or ^ (above) */
    TEST_STRINGMATCH("[+--]", ",", 1, 1); /* ASCII range + to - includes , */
    TEST_STRINGMATCH("[+--]", "*", 0, 0); /*   but not * (below) */
    TEST_STRINGMATCH("[+--]", ".", 0, 0); /*   or . (above) */
    /* And the same, but unclosed: */
    TEST_STRINGMATCH("[+-]", ",", 1, 1);
    TEST_STRINGMATCH("[+-]", "*", 0, 0);
    TEST_STRINGMATCH("[+-]", "^", 0, 0);
    TEST_STRINGMATCH("[+--", ",", 1, 1);
    TEST_STRINGMATCH("[+--", "*", 0, 0);
    TEST_STRINGMATCH("[+--", ".", 0, 0);

    /* Escaped ] alone is literal: */
    TEST_STRINGMATCH("[\\]a]", "]", 1, 1);
    TEST_STRINGMATCH("[\\]a]", "a", 1, 1);

    /* Escapes at range start: */
    TEST_STRINGMATCH("[\\]-_]", "^", 1, 1); /* ASCII range ] to _ includes ^ */
    TEST_STRINGMATCH("[\\]-_]", "-", 0, 0); /*   but not - */

    /* Escapes at range end: */
    TEST_STRINGMATCH("[+-\\\\]", ",", 1, 1); /* ASCII range + to \ includes , */
    TEST_STRINGMATCH("[+-\\\\]", "*", 0, 0); /*   but not * (below) */
    TEST_STRINGMATCH("[+-\\\\]", "]", 0, 0); /*   or ] (above) */
    TEST_STRINGMATCH("[+-\\]]", ",", 1, 1);  /* ASCII range + to ] includes , */
    TEST_STRINGMATCH("[+-\\]]", "*", 0, 0);  /*   but not * (below) */
    TEST_STRINGMATCH("[+-\\]]", "^", 0, 0);  /*   or ^ (above) */
    /* Unclosed is the same: */
    TEST_STRINGMATCH("[+-\\\\", ",", 1, 1);
    TEST_STRINGMATCH("[+-\\\\", "*", 0, 0);
    TEST_STRINGMATCH("[+-\\\\", "]", 0, 0);
    TEST_STRINGMATCH("[+-\\]", ",", 1, 1);
    TEST_STRINGMATCH("[+-\\]", "*", 0, 0);
    TEST_STRINGMATCH("[+-\\]", "^", 0, 0);
    /* An incomplete escape is treated as literal backslash: */
    TEST_STRINGMATCH("[+-\\", ",", 1, 1);
    TEST_STRINGMATCH("[+-\\", "*", 0, 0);
    TEST_STRINGMATCH("[+-\\", "]", 0, 0);
    /* Regression tests: */
    TEST_STRINGMATCH("[+-\\\\]]", "\\]", 1, 1);
    TEST_STRINGMATCH("[+-\\\\]", "]", 0, 0);
    TEST_STRINGMATCH("[+-\\]]", "\\]", 0, 0);

    /* Empty character class matches nothing: */
    TEST_STRINGMATCH("[]", "", 0, 0);
    TEST_STRINGMATCH("[]", "a", 0, 0);
    TEST_STRINGMATCH("[", "", 0, 0); /* Unclosed is the same */
    TEST_STRINGMATCH("[", "a", 0, 0);
    /* Empty negated character class is equivalent to pattern "?": */
    TEST_STRINGMATCH("[^]", "", 0, 0);
    TEST_STRINGMATCH("[^]", "a", 1, 1);
    TEST_STRINGMATCH("[^]", "ab", 0, 0);
    TEST_STRINGMATCH("[^", "", 0, 0); /* Unclosed is the same */
    TEST_STRINGMATCH("[^", "a", 1, 1);
    TEST_STRINGMATCH("[^", "ab", 0, 0);

    /* Unclosed character classes are not an error (undocumented): */
    TEST_STRINGMATCH("[A-", "B", 0, 0);

    // TODO:
    // - CVE tests
    // - regression tests

    TEST_END();
}

/* Fuzz stringmatchlen() trying to crash it with bad input. */
int test_stringmatchlen_fuzz(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char str[32];
    char pat[32];
    int cycles = 100000;
    int total_matches = 0;
    while (cycles--) {
        int strlen = rand() % sizeof(str);
        int patlen = rand() % sizeof(pat);
        for (int j = 0; j < strlen; j++) str[j] = rand() % 128;
        for (int j = 0; j < patlen; j++) pat[j] = rand() % 128;
        total_matches += stringmatchlen(pat, patlen, str, strlen, 0);
    }
    TEST_ASSERT(total_matches <= 100000);
    return 0;
}

int test_string2ll(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char buf[32];
    long long v;

    /* May not start with +. */
    valkey_strlcpy(buf, "+1", sizeof(buf));
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 0);

    /* Leading space. */
    valkey_strlcpy(buf, " 1", sizeof(buf));
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 0);

    /* Trailing space. */
    valkey_strlcpy(buf, "1 ", sizeof(buf));
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 0);

    /* May not start with 0. */
    valkey_strlcpy(buf, "01", sizeof(buf));
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 0);

    valkey_strlcpy(buf, "-1", sizeof(buf));
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == -1);

    valkey_strlcpy(buf, "0", sizeof(buf));
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == 0);

    valkey_strlcpy(buf, "1", sizeof(buf));
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == 1);

    valkey_strlcpy(buf, "99", sizeof(buf));
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == 99);

    valkey_strlcpy(buf, "-99", sizeof(buf));
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == -99);

    valkey_strlcpy(buf, "-9223372036854775808", sizeof(buf));
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == LLONG_MIN);

    valkey_strlcpy(buf, "-9223372036854775809", sizeof(buf)); /* overflow */
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 0);

    valkey_strlcpy(buf, "9223372036854775807", sizeof(buf));
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == LLONG_MAX);

    valkey_strlcpy(buf, "9223372036854775808", sizeof(buf)); /* overflow */
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 0);

    valkey_strlcpy(buf, "18446744073709551615", sizeof(buf)); /* overflow */
    TEST_ASSERT(string2ll(buf, strlen(buf), &v) == 0);

    return 0;
}

int test_string2l(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char buf[32];
    long v;

    /* May not start with +. */
    valkey_strlcpy(buf, "+1", sizeof(buf));
    TEST_ASSERT(string2l(buf, strlen(buf), &v) == 0);

    /* May not start with 0. */
    valkey_strlcpy(buf, "01", sizeof(buf));
    TEST_ASSERT(string2l(buf, strlen(buf), &v) == 0);

    valkey_strlcpy(buf, "-1", sizeof(buf));
    TEST_ASSERT(string2l(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == -1);

    valkey_strlcpy(buf, "0", sizeof(buf));
    TEST_ASSERT(string2l(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == 0);

    valkey_strlcpy(buf, "1", sizeof(buf));
    TEST_ASSERT(string2l(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == 1);

    valkey_strlcpy(buf, "99", sizeof(buf));
    TEST_ASSERT(string2l(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == 99);

    valkey_strlcpy(buf, "-99", sizeof(buf));
    TEST_ASSERT(string2l(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == -99);

#if LONG_MAX != LLONG_MAX
    valkey_strlcpy(buf, "-2147483648", sizeof(buf));
    TEST_ASSERT(string2l(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == LONG_MIN);

    valkey_strlcpy(buf, "-2147483649", sizeof(buf)); /* overflow */
    TEST_ASSERT(string2l(buf, strlen(buf), &v) == 0);

    valkey_strlcpy(buf, "2147483647", sizeof(buf));
    TEST_ASSERT(string2l(buf, strlen(buf), &v) == 1);
    TEST_ASSERT(v == LONG_MAX);

    valkey_strlcpy(buf, "2147483648", sizeof(buf)); /* overflow */
    TEST_ASSERT(string2l(buf, strlen(buf), &v) == 0);
#endif

    return 0;
}

int test_ll2string(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char buf[32];
    long long v;
    int sz;

    v = 0;
    sz = ll2string(buf, sizeof buf, v);
    TEST_ASSERT(sz == 1);
    TEST_ASSERT(!strcmp(buf, "0"));

    v = -1;
    sz = ll2string(buf, sizeof buf, v);
    TEST_ASSERT(sz == 2);
    TEST_ASSERT(!strcmp(buf, "-1"));

    v = 99;
    sz = ll2string(buf, sizeof buf, v);
    TEST_ASSERT(sz == 2);
    TEST_ASSERT(!strcmp(buf, "99"));

    v = -99;
    sz = ll2string(buf, sizeof buf, v);
    TEST_ASSERT(sz == 3);
    TEST_ASSERT(!strcmp(buf, "-99"));

    v = -2147483648;
    sz = ll2string(buf, sizeof buf, v);
    TEST_ASSERT(sz == 11);
    TEST_ASSERT(!strcmp(buf, "-2147483648"));

    v = LLONG_MIN;
    sz = ll2string(buf, sizeof buf, v);
    TEST_ASSERT(sz == 20);
    TEST_ASSERT(!strcmp(buf, "-9223372036854775808"));

    v = LLONG_MAX;
    sz = ll2string(buf, sizeof buf, v);
    TEST_ASSERT(sz == 19);
    TEST_ASSERT(!strcmp(buf, "9223372036854775807"));

    return 0;
}

int test_ld2string(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char buf[32];
    long double v;
    int sz;

    v = 0.0 / 0.0;
    sz = ld2string(buf, sizeof(buf), v, LD_STR_AUTO);
    TEST_ASSERT(sz == 3);
    TEST_ASSERT(!strcmp(buf, "nan"));

    return 0;
}

int test_fixedpoint_d2string(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char buf[32];
    double v;
    int sz;
    v = 0.0;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    TEST_ASSERT(sz == 6);
    TEST_ASSERT(!strcmp(buf, "0.0000"));
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    TEST_ASSERT(sz == 3);
    TEST_ASSERT(!strcmp(buf, "0.0"));
    /* set junk in buffer */
    memset(buf, 'A', 32);
    v = 0.0001;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    TEST_ASSERT(sz == 6);
    TEST_ASSERT(buf[sz] == '\0');
    TEST_ASSERT(!strcmp(buf, "0.0001"));
    /* set junk in buffer */
    memset(buf, 'A', 32);
    v = 6.0642951598391699e-05;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    TEST_ASSERT(sz == 6);
    TEST_ASSERT(buf[sz] == '\0');
    TEST_ASSERT(!strcmp(buf, "0.0001"));
    v = 0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    TEST_ASSERT(sz == 6);
    TEST_ASSERT(!strcmp(buf, "0.0100"));
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    TEST_ASSERT(sz == 3);
    TEST_ASSERT(!strcmp(buf, "0.0"));
    v = -0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    TEST_ASSERT(sz == 7);
    TEST_ASSERT(!strcmp(buf, "-0.0100"));
    v = -0.1;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    TEST_ASSERT(sz == 4);
    TEST_ASSERT(!strcmp(buf, "-0.1"));
    v = 0.1;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    TEST_ASSERT(sz == 3);
    TEST_ASSERT(!strcmp(buf, "0.1"));
    v = 0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 17);
    TEST_ASSERT(sz == 19);
    TEST_ASSERT(!strcmp(buf, "0.01000000000000000"));
    v = 10.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    TEST_ASSERT(sz == 7);
    TEST_ASSERT(!strcmp(buf, "10.0100"));
    /* negative tests */
    sz = fixedpoint_d2string(buf, sizeof buf, v, 18);
    TEST_ASSERT(sz == 0);
    sz = fixedpoint_d2string(buf, sizeof buf, v, 0);
    TEST_ASSERT(sz == 0);
    sz = fixedpoint_d2string(buf, 1, v, 1);
    TEST_ASSERT(sz == 0);

    return 0;
}

int test_version2num(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    TEST_ASSERT(version2num("7.2.5") == 0x070205);
    TEST_ASSERT(version2num("255.255.255") == 0xffffff);
    TEST_ASSERT(version2num("7.2.256") == -1);
    TEST_ASSERT(version2num("7.2") == -1);
    TEST_ASSERT(version2num("7.2.1.0") == -1);
    TEST_ASSERT(version2num("1.-2.-3") == -1);
    TEST_ASSERT(version2num("1.2.3-rc4") == -1);
    TEST_ASSERT(version2num("") == -1);
    return 0;
}

#if defined(__linux__)
/* Since fadvise and mincore is only supported in specific platforms like
 * Linux, we only verify the fadvise mechanism works in Linux */
static int cache_exist(int fd) {
    unsigned char flag;
    void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    TEST_ASSERT(m);
    TEST_ASSERT(mincore(m, 4096, &flag) == 0);
    munmap(m, 4096);
    /* the least significant bit of the byte will be set if the corresponding
     * page is currently resident in memory */
    return flag & 1;
}
#endif

int test_reclaimFilePageCache(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);

    /* The test is incompatible with valgrind, skip it. */
    if (flags & UNIT_TEST_VALGRIND) return 0;

#if defined(__linux__)
    struct statfs stats;

    /* Check if /tmp is memory-backed (e.g., tmpfs) */
    if (statfs("/tmp", &stats) == 0) {
        if (stats.f_type != TMPFS_MAGIC) { // Not tmpfs, use /tmp
            return 0;
        }
    }

    char *tmpfile = "/tmp/redis-reclaim-cache-test";
    int fd = open(tmpfile, O_RDWR | O_CREAT, 0644);
    TEST_ASSERT(fd >= 0);

    /* test write file */
    char buf[4] = "foo";
    TEST_ASSERT(write(fd, buf, sizeof(buf)) > 0);
    TEST_ASSERT(cache_exist(fd));
    TEST_ASSERT(valkey_fsync(fd) == 0);
    TEST_ASSERT(reclaimFilePageCache(fd, 0, 0) == 0);
    TEST_ASSERT(!cache_exist(fd));

    /* test read file */
    TEST_ASSERT(pread(fd, buf, sizeof(buf), 0) > 0);
    TEST_ASSERT(cache_exist(fd));
    TEST_ASSERT(reclaimFilePageCache(fd, 0, 0) == 0);
    TEST_ASSERT(!cache_exist(fd));

    unlink(tmpfile);
#endif
    return 0;
}
