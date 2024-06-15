/*
 * Copyright Valkey contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include <strings.h>
#include <stdio.h>
#include <regex.h>
#include "test_files.h"
#include "test_help.h"

/* We override the default assertion mechanism, so that it prints out info and then dies. */
void _serverAssert(const char *estr, const char *file, int line) {
    TEST_PRINT_REPORT("[" KRED "serverAssert - %s:%d" KRESET "] - %s", file, line, estr);
    exit(1);
}

/* Run the tests defined by the test suite. */
int runTestSuite(struct unitTestSuite *test, int argc, char **argv, int count, char *pattern, int flags) {
    int test_num = 0;
    int failed_tests = 0;
    int test_result = 0;
    int run_num = 0;
    unsigned long long duration;
    unsigned long long start_time;

    regex_t regex;
    if (pattern != NULL) regcomp(&regex, pattern, 0);
    TEST_PRINT_REPORT("[" KBLUE "START" KRESET "] - %s", test->filename);

    for (int id = 0; test->tests[id].proc != NULL; id++) {
        if (pattern != NULL && regexec(&regex, test->tests[id].name, 0, NULL, 0)) {
            TEST_PRINT_REPORT("[" KBLUE "skip" KRESET "] - %s:%s", test->filename, test->tests[id].name);
            continue;
        }

        test_num++;
        elapsedMonoStart(&start_time);
        for (run_num = 0; run_num < count; run_num++) {
            test_result = (test->tests[id].proc(argc, argv, flags) != 0);
            if (test_result) break;
        }
        duration = elapsedMonoNs(start_time);
        if (!test_result) {
            TEST_PRINT_REPORT("[" KGRN "ok" KRESET "] - %s:%s\t%d\t%.4lf ns/op", test->filename, test->tests[id].name,
                              count, ((double)duration / count));
        } else {
            TEST_PRINT_REPORT("[" KRED "fail" KRESET "] - %s:%s", test->filename, test->tests[id].name);
            failed_tests++;
        }
    }

    TEST_PRINT_REPORT("[" KBLUE "END" KRESET "] - %s: ", test->filename);
    TEST_PRINT_REPORT("%d tests, %d passed, %d failed", test_num, test_num - failed_tests, failed_tests);
    return !failed_tests;
}

int main(int argc, char **argv) {
    int flags = 0;
    char *pattern = NULL;
    char *file = NULL;
    int count = 1;
    regex_t regex;

    verbosity = LL_NOTICE;
    for (int j = 1; j < argc; j++) {
        char *arg = argv[j];
        if (!strcasecmp(arg, "--accurate"))
            flags |= UNIT_TEST_ACCURATE;
        else if (!strcasecmp(arg, "--large-memory"))
            flags |= UNIT_TEST_LARGE_MEMORY;
        else if (!strcasecmp(arg, "--count") && (j + 1 < argc)) {
            count = atoi(argv[j + 1]);
            j++;
        } else if (!strcasecmp(arg, "--single") && (j + 1 < argc)) {
            flags |= UNIT_TEST_SINGLE;
            file = argv[j + 1];
            j++;
        } else if (!strcasecmp(arg, "--only") && (j + 1 < argc)) {
            pattern = argv[j + 1];
            if (regcomp(&regex, pattern, 0)) {
                TEST_PRINT_REPORT("pattern compile error %s", pattern);
                return 1;
            }
            j++;
        } else if (!strcasecmp(arg, "--loglevel") && (j + 1 < argc)) {
            if (!strcasecmp(argv[j + 1], "debug"))
                verbosity = LL_DEBUG;
            else if (!strcasecmp(argv[j + 1], "verbose"))
                verbosity = LL_VERBOSE;
            else if (!strcasecmp(argv[j + 1], "notice"))
                verbosity = LL_NOTICE;
            else if (!strcasecmp(argv[j + 1], "warning"))
                verbosity = LL_WARNING;
            else if (!strcasecmp(argv[j + 1], "nothing"))
                verbosity = LL_NOTHING;
            else {
                TEST_PRINT_REPORT("loglevel error %s, help: debug, verbose, notice, warning, nothing", argv[j + 1]);
                return 1;
            }
            j++;
        }
    }

    int numtests = sizeof(unitTestSuite) / sizeof(struct unitTestSuite);
    int failed_num = 0, suites_executed = 0;
    for (int j = 0; j < numtests; j++) {
        if (file && strcasecmp(file, unitTestSuite[j].filename)) continue;
        if (!runTestSuite(&unitTestSuite[j], argc, argv, count, pattern, flags)) {
            failed_num++;
        }
        suites_executed++;
    }
    TEST_PRINT_REPORT("%d test suites executed, %d passed, %d failed", suites_executed, suites_executed - failed_num,
                      failed_num);

    return failed_num == 0 ? 0 : 1;
}
