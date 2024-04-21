#include <strings.h>
#include <stdio.h>
#include "test_files.h"
#include "test_help.h"

int __failed_tests = 0;
int __test_num = 0;

struct unitTest *getTestByName(const char *name) {
    int numtests = sizeof(unitTestSuite)/sizeof(struct unitTest);
    for (int j = 0; j < numtests; j++) {
        for (int i = 0; unitTestSuite[j].tests[i].name != NULL; i++) {
            if (!strcasecmp(name,unitTestSuite[j].tests[i].name)) {
                return &unitTestSuite[j].tests[i];
            }
        }
    }
    return NULL;
}

int runTestSuite(struct unitTestSuite *test, int argc, char **argv, int flags) {
    int test_num = 0;
    int failed_tests = 0;
    printf("[" KBLUE "START" KRESET "] Test - %s\n", test->filename);

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

    printf("[" KBLUE "END" KRESET "] Test - %s: ", test->filename);
    printf("%d tests, %d passed, %d failed\n", test_num,
        test_num - failed_tests, failed_tests);
    return !failed_tests;
}

int main(int argc, char **argv) {
    int flags = 0;
    for (int j = 2; j < argc; j++) {
        char *arg = argv[j];
        if (!strcasecmp(arg, "--accurate")) flags |= UNIT_TEST_ACCURATE;
        else if (!strcasecmp(arg, "--large-memory")) flags |= UNIT_TEST_LARGE_MEMORY;
    }

    int numtests = sizeof(unitTestSuite)/sizeof(struct unitTest);
    int failed_num = 0;
    for (int j = 0; j < numtests; j++) {
        if (!runTestSuite(&unitTestSuite[j], argc, argv, flags)) {
            failed_num++;
        }
    }
    printf("%d test suites executed, %d passed, %d failed\n", numtests,
            numtests-failed_num, failed_num);

    return failed_num == 0 ? 0 : 1;
}

void _serverAssert(const char *estr, const char *file, int line) {
    printf("Hi");
    UNUSED(estr);
    UNUSED(file);
    UNUSED(line);
}
