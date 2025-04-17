/* A very simple test framework for valkey. See unit/README.me for more information on usage.
 *
 * Example:
 *
 * int test_example(int argc, char *argv[], int flags) {
 *     TEST_ASSERT_MESSAGE("Check if 1 == 1", 1==1);
 *     TEST_ASSERT(5 == 5);
 *     return 0;
 * }
 */

#ifndef __TESTHELP_H
#define __TESTHELP_H

#include <stdlib.h>
#include <stdio.h>

/* The flags are the following:
 * --accurate:     Runs tests with more iterations.
 * --large-memory: Enables tests that consume more than 100mb.
 * --single:       A flag to indicate a specific test file was executed.
 * --valgrind:     Runs tests with valgrind. */
#define UNIT_TEST_ACCURATE (1 << 0)
#define UNIT_TEST_LARGE_MEMORY (1 << 1)
#define UNIT_TEST_SINGLE (1 << 2)
#define UNIT_TEST_VALGRIND (1 << 3)

#define KRED "\33[31m"
#define KGRN "\33[32m"
#define KBLUE "\33[34m"
#define KRESET "\33[0m"

#define TEST_PRINT_ERROR(descr) printf("[" KRED "%s - %s:%d" KRESET "] %s\n", __func__, __FILE__, __LINE__, descr)

#define TEST_PRINT_INFO(descr, ...) \
    printf("[" KBLUE "%s - %s:%d" KRESET "] " descr "\n", __func__, __FILE__, __LINE__, __VA_ARGS__)

#define TEST_ASSERT_MESSAGE(descr, _c) \
    do {                               \
        if (!(_c)) {                   \
            TEST_PRINT_ERROR(descr);   \
            return 1;                  \
        }                              \
    } while (0)

#define TEST_ASSERT(_c) TEST_ASSERT_MESSAGE("Failed assertion: " #_c, _c)

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

#endif
