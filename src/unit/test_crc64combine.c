#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "test_help.h"
#include "../zmalloc.h"
#include "../crc64.h"
#include "../crcspeed.h"
#include "../crccombine.h"

static void genBenchmarkRandomData(char *data, int count);
static int bench_crc64(unsigned char *data, uint64_t size, long long passes, uint64_t check, char *name, int csv);
static void bench_combine(char *label, uint64_t size, uint64_t expect, int csv);

long long _ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

static int bench_crc64(unsigned char *data, uint64_t size, long long passes, uint64_t check, char *name, int csv) {
    uint64_t min = size, hash = 0;
    long long original_start = _ustime(), original_end;
    for (long long i = passes; i > 0; i--) {
        hash = crc64(0, data, size);
    }
    original_end = _ustime();
    min = (original_end - original_start) * 1000 / passes;
    /* approximate nanoseconds without nstime */
    if (csv) {
        printf("%s,%" PRIu64 ",%" PRIu64 ",%d\n", name, size, (1000 * size) / min, hash == check);
    } else {
        TEST_PRINT_INFO("test size=%" PRIu64 " algorithm=%s %" PRIu64 " M/sec matches=%d", size, name,
                        (1000 * size) / min, hash == check);
    }
    return hash != check;
}

const uint64_t BENCH_RPOLY = UINT64_C(0x95ac9329ac4bc9b5);

static void bench_combine(char *label, uint64_t size, uint64_t expect, int csv) {
    uint64_t min = size, start = expect, thash = expect ^ (expect >> 17);
    long long original_start = _ustime(), original_end;
    for (int i = 0; i < 1000; i++) {
        crc64_combine(thash, start, size, BENCH_RPOLY, 64);
    }
    original_end = _ustime();
    /* ran 1000 times, want ns per, counted us per 1000 ... */
    min = original_end - original_start;
    if (csv) {
        printf("%s,%" PRIu64 ",%" PRIu64 "\n", label, size, min);
    } else {
        printf("%s size=%" PRIu64 " in %" PRIu64 " nsec\n", label, size, min);
    }
}

static void genBenchmarkRandomData(char *data, int count) {
    static uint32_t state = 1234;
    int i = 0;

    while (count--) {
        state = (state * 1103515245 + 12345);
        data[i++] = '0' + ((state >> 16) & 63);
    }
}

/* This is a special unit test useful for benchmarking crc64combine performance. The
 * benchmarking is only done when the tests are invoked with a single test target,
 * like 'valkey-unit-tests --single test_crc64combine.c --crc 16384'. */
int test_crc64combine(int argc, char **argv, int flags) {
    if (!(flags & UNIT_TEST_SINGLE)) {
        return 0;
    }

    uint64_t crc64_test_size = 0;
    int i, lastarg, csv = 0, loop = 0, combine = 0;
again:
    for (i = 3; i < argc; i++) {
        lastarg = (i == (argc - 1));
        if (!strcmp(argv[i], "--help")) {
            goto usage;
        } else if (!strcmp(argv[i], "--csv")) {
            csv = 1;
        } else if (!strcmp(argv[i], "-l")) {
            loop = 1;
        } else if (!strcmp(argv[i], "--crc")) {
            if (lastarg) goto invalid;
            crc64_test_size = atoll(argv[++i]);
        } else if (!strcmp(argv[i], "--combine")) {
            combine = 1;
        } else {
        invalid:
            printf("Invalid option \"%s\" or option argument missing\n\n", argv[i]);
        usage:
            printf("Usage: --single test_crc64combine.c [OPTIONS]\n\n"
                   " --csv              Output in CSV format\n"
                   " -l                 Loop. Run the tests forever\n"
                   " --crc <bytes>      Benchmark crc64 faster options, using a buffer this big, and quit when done.\n"
                   " --combine          Benchmark crc64 combine value ranges and timings.\n");
            return 1;
        }
    }

    int init_this_loop = 1;
    long long init_start, init_end;

    do {
        unsigned char *data = NULL;
        uint64_t passes = 0;
        if (crc64_test_size) {
            data = zmalloc(crc64_test_size);
            genBenchmarkRandomData((char *)data, crc64_test_size);
            /* We want to hash about 1 gig of data in total, looped, to get a good
             * idea of our performance.
             */
            passes = (UINT64_C(0x100000000) / crc64_test_size);
            passes = passes >= 2 ? passes : 2;
            passes = passes <= 1000 ? passes : 1000;
        }

        crc64_init();
        /* warm up the cache */
        set_crc64_cutoffs(crc64_test_size + 1, crc64_test_size + 1);
        uint64_t expect = crc64(0, data, crc64_test_size);

        if (!combine && crc64_test_size) {
            if (csv && init_this_loop) printf("algorithm,buffer,performance,crc64_matches\n");

            /* get the single-character version for single-byte Redis behavior */
            set_crc64_cutoffs(0, crc64_test_size + 1);
            if (bench_crc64(data, crc64_test_size, passes, expect, "crc_1byte", csv)) return 1;

            set_crc64_cutoffs(crc64_test_size + 1, crc64_test_size + 1);
            /* run with 8-byte "single" path, crcfaster */
            if (bench_crc64(data, crc64_test_size, passes, expect, "crcspeed", csv)) return 1;

            /* run with dual 8-byte paths */
            set_crc64_cutoffs(1, crc64_test_size + 1);
            if (bench_crc64(data, crc64_test_size, passes, expect, "crcdual", csv)) return 1;

            /* run with tri 8-byte paths */
            set_crc64_cutoffs(1, 1);
            if (bench_crc64(data, crc64_test_size, passes, expect, "crctri", csv)) return 1;

            /* Be free memory region, be free. */
            zfree(data);
            data = NULL;
        }

        uint64_t INIT_SIZE = UINT64_C(0xffffffffffffffff);
        if (combine) {
            if (init_this_loop) {
                init_start = _ustime();
                crc64_combine(UINT64_C(0xdeadbeefdeadbeef), UINT64_C(0xfeebdaedfeebdaed), INIT_SIZE, BENCH_RPOLY, 64);
                init_end = _ustime();

                init_end -= init_start;
                init_end *= 1000;
                if (csv) {
                    printf("operation,size,nanoseconds\n");
                    printf("init_64,%" PRIu64 ",%" PRIu64 "\n", INIT_SIZE, (uint64_t)init_end);
                } else {
                    TEST_PRINT_INFO("init_64 size=%" PRIu64 " in %" PRIu64 " nsec", INIT_SIZE, (uint64_t)init_end);
                }
                /* use the hash itself as the size (unpredictable) */
                bench_combine("hash_as_size_combine", crc64_test_size, expect, csv);

                /* let's do something big (predictable, so fast) */
                bench_combine("largest_combine", INIT_SIZE, expect, csv);
            }
            bench_combine("combine", crc64_test_size, expect, csv);
        }
        init_this_loop = 0;
        /* step down by ~1.641 for a range of test sizes */
        crc64_test_size -= (crc64_test_size >> 2) + (crc64_test_size >> 3) + (crc64_test_size >> 6);
    } while (crc64_test_size > 3);
    if (loop) goto again;
    return 0;
}
