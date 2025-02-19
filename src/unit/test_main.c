/*
 * Copyright Valkey contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include <strings.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "test_files.h"
#include "test_help.h"
#include "../util.h"
#include "../zmalloc.h"
#include "../mt19937-64.h"
#include "../hashtable.h"

/* We override the default assertion mechanism, so that it prints out info and then dies. */
void _serverAssert(const char *estr, const char *file, int line) {
    printf("[" KRED "serverAssert - %s:%d" KRESET "] - %s\n", file, line, estr);
    exit(1);
}

/* Run the tests defined by the test suite. */
int runTestSuite(struct unitTestSuite *test, int argc, char **argv, int flags) {
    int test_num = 0;
    int failed_tests = 0;
    printf("[" KBLUE "START" KRESET "] - %s\n", test->filename);

    for (int id = 0; test->tests[id].proc != NULL; id++) {
        test_num++;
        int test_result = (test->tests[id].proc(argc, argv, flags) != 0);
        if (!test_result) {
            printf("[" KGRN "ok" KRESET "] - %s:%s\n", test->filename, test->tests[id].name);
        } else {
            printf("[" KRED "fail" KRESET "] - %s:%s\n", test->filename, test->tests[id].name);
            failed_tests++;
        }
    }

    printf("[" KBLUE "END" KRESET "] - %s: ", test->filename);
    printf("%d tests, %d passed, %d failed\n", test_num, test_num - failed_tests, failed_tests);
    return !failed_tests;
}

int main(int argc, char **argv) {
    int flags = 0;
    char *file = NULL;
    char *seed = NULL;
    for (int j = 1; j < argc; j++) {
        char *arg = argv[j];
        if (!strcasecmp(arg, "--accurate"))
            flags |= UNIT_TEST_ACCURATE;
        else if (!strcasecmp(arg, "--large-memory"))
            flags |= UNIT_TEST_LARGE_MEMORY;
        else if (!strcasecmp(arg, "--single") && (j + 1 < argc)) {
            flags |= UNIT_TEST_SINGLE;
            file = argv[j + 1];
        } else if (!strcasecmp(arg, "--valgrind")) {
            flags |= UNIT_TEST_VALGRIND;
        } else if (!strcasecmp(arg, "--seed")) {
            seed = argv[j + 1];
        }
    }

    if (seed) {
        setRandomSeedCString(seed, strlen(seed));
    }

    /* We need to copy the seed chars returned by getRandomSeedCString into a
     * buffer allocated with malloc because we need to free the memory of
     * `seed_str` to avoid assert errors in zmalloc statistics. */
    size_t seed_len = 0;
    char *seed_str = getRandomSeedCString(&seed_len);
    seed = malloc(seed_len + 1);
    strncpy(seed, seed_str, seed_len);
    seed[seed_len] = 0;
    zfree(seed_str);

    printf("Tests will run with seed=%s\n", seed);

    int numtests = sizeof(unitTestSuite) / sizeof(struct unitTestSuite);
    int failed_num = 0, suites_executed = 0;
    for (int j = 0; j < numtests; j++) {
        if (file && strcasecmp(file, unitTestSuite[j].filename)) continue;

        /* If the seed parameter was specified, we need to explicitly set the
         * seed in the several random numbers generator that valkey server uses
         * so that the unit tests reproduce the random values in a
         * deterministic way. */
        setRandomSeedCString(seed, strlen(seed));
        unsigned long long seed;
        getRandomBytes((void *)&seed, sizeof(seed));

        init_genrand64(seed);
        srandom((unsigned)seed);

        uint8_t hashseed[16];
        getRandomBytes(hashseed, sizeof(hashseed));
        hashtableSetHashFunctionSeed(hashseed);

        if (!runTestSuite(&unitTestSuite[j], argc, argv, flags)) {
            failed_num++;
        }
        suites_executed++;
    }
    printf("%d test suites executed, %d passed, %d failed\n", suites_executed, suites_executed - failed_num,
           failed_num);

    free(seed);

    return failed_num == 0 ? 0 : 1;
}
