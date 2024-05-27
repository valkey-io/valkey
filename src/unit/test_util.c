#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

#include "../config.h"
#include "../util.h"
#include "test_help.h"

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
    UNUSED(flags);

#if defined(__linux__)
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
