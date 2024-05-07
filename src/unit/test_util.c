
#include <assert.h>
#include <sys/mman.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "../testhelp.h"
#include "../util.h"

#define UNUSED(x) ((void)(x))

static void test_string2ll(void) {
    char buf[32];
    long long v;

    /* May not start with +. */
    valkey_strlcpy(buf,"+1",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 0);

    /* Leading space. */
    valkey_strlcpy(buf," 1",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 0);

    /* Trailing space. */
    valkey_strlcpy(buf,"1 ",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 0);

    /* May not start with 0. */
    valkey_strlcpy(buf,"01",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 0);

    valkey_strlcpy(buf,"-1",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == -1);

    valkey_strlcpy(buf,"0",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == 0);

    valkey_strlcpy(buf,"1",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == 1);

    valkey_strlcpy(buf,"99",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == 99);

    valkey_strlcpy(buf,"-99",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == -99);

    valkey_strlcpy(buf,"-9223372036854775808",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == LLONG_MIN);

    valkey_strlcpy(buf,"-9223372036854775809",sizeof(buf)); /* overflow */
    assert(string2ll(buf,strlen(buf),&v) == 0);

    valkey_strlcpy(buf,"9223372036854775807",sizeof(buf));
    assert(string2ll(buf,strlen(buf),&v) == 1);
    assert(v == LLONG_MAX);

    valkey_strlcpy(buf,"9223372036854775808",sizeof(buf)); /* overflow */
    assert(string2ll(buf,strlen(buf),&v) == 0);
}

static void test_string2l(void) {
    char buf[32];
    long v;

    /* May not start with +. */
    valkey_strlcpy(buf,"+1",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 0);

    /* May not start with 0. */
    valkey_strlcpy(buf,"01",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 0);

    valkey_strlcpy(buf,"-1",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == -1);

    valkey_strlcpy(buf,"0",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == 0);

    valkey_strlcpy(buf,"1",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == 1);

    valkey_strlcpy(buf,"99",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == 99);

    valkey_strlcpy(buf,"-99",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == -99);

#if LONG_MAX != LLONG_MAX
    valkey_strlcpy(buf,"-2147483648",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == LONG_MIN);

    valkey_strlcpy(buf,"-2147483649",sizeof(buf)); /* overflow */
    assert(string2l(buf,strlen(buf),&v) == 0);

    valkey_strlcpy(buf,"2147483647",sizeof(buf));
    assert(string2l(buf,strlen(buf),&v) == 1);
    assert(v == LONG_MAX);

    valkey_strlcpy(buf,"2147483648",sizeof(buf)); /* overflow */
    assert(string2l(buf,strlen(buf),&v) == 0);
#endif
}

static void test_ll2string(void) {
    char buf[32];
    long long v;
    int sz;

    v = 0;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 1);
    assert(!strcmp(buf, "0"));

    v = -1;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 2);
    assert(!strcmp(buf, "-1"));

    v = 99;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 2);
    assert(!strcmp(buf, "99"));

    v = -99;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 3);
    assert(!strcmp(buf, "-99"));

    v = -2147483648;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 11);
    assert(!strcmp(buf, "-2147483648"));

    v = LLONG_MIN;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 20);
    assert(!strcmp(buf, "-9223372036854775808"));

    v = LLONG_MAX;
    sz = ll2string(buf, sizeof buf, v);
    assert(sz == 19);
    assert(!strcmp(buf, "9223372036854775807"));
}

static void test_ld2string(void) {
    char buf[32];
    long double v;
    int sz;

    v = 0.0 / 0.0;
    sz = ld2string(buf, sizeof(buf), v, LD_STR_AUTO);
    assert(sz == 3);
    assert(!strcmp(buf, "nan"));
}

static void test_fixedpoint_d2string(void) {
    char buf[32];
    double v;
    int sz;
    v = 0.0;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 6);
    assert(!strcmp(buf, "0.0000"));
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    assert(sz == 3);
    assert(!strcmp(buf, "0.0"));
    /* set junk in buffer */
    memset(buf,'A',32);
    v = 0.0001;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 6);
    assert(buf[sz] == '\0');
    assert(!strcmp(buf, "0.0001"));
    /* set junk in buffer */
    memset(buf,'A',32);
    v = 6.0642951598391699e-05;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 6);
    assert(buf[sz] == '\0');
    assert(!strcmp(buf, "0.0001"));
    v = 0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 6);
    assert(!strcmp(buf, "0.0100"));
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    assert(sz == 3);
    assert(!strcmp(buf, "0.0"));
    v = -0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 7);
    assert(!strcmp(buf, "-0.0100"));
     v = -0.1;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    assert(sz == 4);
    assert(!strcmp(buf, "-0.1"));
    v = 0.1;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 1);
    assert(sz == 3);
    assert(!strcmp(buf, "0.1"));
    v = 0.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 17);
    assert(sz == 19);
    assert(!strcmp(buf, "0.01000000000000000"));
    v = 10.01;
    sz = fixedpoint_d2string(buf, sizeof buf, v, 4);
    assert(sz == 7);
    assert(!strcmp(buf, "10.0100"));
    /* negative tests */
    sz = fixedpoint_d2string(buf, sizeof buf, v, 18);
    assert(sz == 0);
    sz = fixedpoint_d2string(buf, sizeof buf, v, 0);
    assert(sz == 0);
    sz = fixedpoint_d2string(buf, 1, v, 1);
    assert(sz == 0);
}

#if defined(__linux__)
/* Since fadvise and mincore is only supported in specific platforms like
 * Linux, we only verify the fadvise mechanism works in Linux */
static int cache_exist(int fd) {
    unsigned char flag;
    void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    assert(m);
    assert(mincore(m, 4096, &flag) == 0);
    munmap(m, 4096);
    /* the least significant bit of the byte will be set if the corresponding
     * page is currently resident in memory */
    return flag&1;
}

static void test_reclaimFilePageCache(void) {
    char *tmpfile = "/tmp/redis-reclaim-cache-test";
    int fd = open(tmpfile, O_RDWR|O_CREAT, 0644);
    assert(fd >= 0);

    /* test write file */
    char buf[4] = "foo";
    assert(write(fd, buf, sizeof(buf)) > 0);
    assert(cache_exist(fd));
    assert(valkey_fsync(fd) == 0);
    assert(reclaimFilePageCache(fd, 0, 0) == 0);
    assert(!cache_exist(fd));

    /* test read file */
    assert(pread(fd, buf, sizeof(buf), 0) > 0);
    assert(cache_exist(fd));
    assert(reclaimFilePageCache(fd, 0, 0) == 0);
    assert(!cache_exist(fd));

    unlink(tmpfile);
    printf("reclaimFilePageCache test is ok\n");
}
#endif

int utilTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    test_string2ll();
    test_string2l();
    test_ll2string();
    test_ld2string();
    test_fixedpoint_d2string();
#if defined(__linux__)
    if (!(flags & TEST_VALGRIND)) {
        test_reclaimFilePageCache();
    }
#endif
    printf("Done testing util\n");
    return 0;
}