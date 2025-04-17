/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <strings.h>
#include <string.h>
#include <stdio.h>
#include "test_files.h"
#include "test_help.h"
#include "../util.h"
#include "../mt19937-64.h"
#include "../hashtable.h"
#include "../zmalloc.h"

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

    /* Check if the test suite has cleaned up all the memory used. */
    if (zmalloc_used_memory() > 0) {
        printf("[" KRED "%s" KRESET "] Memory leak detected of %zu bytes\n", test->filename, zmalloc_used_memory());
        failed_tests++;
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

    char seed_cstr[129];
    getRandomSeedCString(seed_cstr, 129);

    printf("Tests will run with seed=%s\n", seed_cstr);

    unsigned long long genrandseed;
    getRandomBytes((void *)&genrandseed, sizeof(genrandseed));

    uint8_t hashseed[16];
    getRandomBytes(hashseed, sizeof(hashseed));


    int numtests = sizeof(unitTestSuite) / sizeof(struct unitTestSuite);
    int failed_num = 0, suites_executed = 0;
    for (int j = 0; j < numtests; j++) {
        if (file && strcasecmp(file, unitTestSuite[j].filename)) continue;

        /* We need to explicitly set the seed in the several random numbers
         * generator that valkey server uses so that the unit tests reproduce
         * the random values in a deterministic way. */
        setRandomSeedCString(seed_cstr, strlen(seed_cstr));
        init_genrand64(genrandseed);
        srandom((unsigned)genrandseed);
        hashtableSetHashFunctionSeed(hashseed);

        if (!runTestSuite(&unitTestSuite[j], argc, argv, flags)) {
            failed_num++;
        }
        suites_executed++;
    }
    printf("%d test suites executed, %d passed, %d failed\n", suites_executed, suites_executed - failed_num,
           failed_num);

    return failed_num == 0 ? 0 : 1;
}
