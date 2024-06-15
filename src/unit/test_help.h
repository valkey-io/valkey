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
#include <time.h>

/* The flags are the following:
 * --accurate:     Runs tests with more iterations.
 * --large-memory: Enables tests that consume more than 100mb.
 * --single:       A flag to indicate a specific test file was executed. */
#define UNIT_TEST_ACCURATE (1 << 0)
#define UNIT_TEST_LARGE_MEMORY (1 << 1)
#define UNIT_TEST_SINGLE (1 << 2)

#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define LL_NOTHING 4
#define LL_RAW (1 << 10) /* Modifier to log without timestamp */

#define KRED "\33[31m"
#define KGRN "\33[32m"
#define KBLUE "\33[34m"
#define KRESET "\33[0m"

int verbosity;

#define serverLog(level, ...)                                                                                          \
    do {                                                                                                               \
        if (((level) & 0xff) < verbosity) break;                                                                       \
        printf(__VA_ARGS__);                                                                                           \
    } while (0)

#define TEST_PRINT_ERROR(descr)                                                                                        \
    serverLog(LL_WARNING, "[" KRED "%s - %s:%d" KRESET "] %s\n", __func__, __FILE__, __LINE__, descr)

#define TEST_PRINT_LINE(descr)                                                                                         \
    serverLog(LL_VERBOSE, "[" KBLUE "%s - %s:%d" KRESET "] " descr "\n", __func__, __FILE__, __LINE__);

#define TEST_PRINT_INFO(descr, ...)                                                                                    \
    serverLog(LL_VERBOSE, "[" KBLUE "%s -88 %s:%d" KRESET "] " descr "\n", __func__, __FILE__, __LINE__, __VA_ARGS__);

#define TEST_PRINT_REPORT(descr, ...) serverLog(LL_NOTICE, descr "\n", __VA_ARGS__)

#define TEST_ASSERT_MESSAGE(descr, _c)                                                                                 \
    do {                                                                                                               \
        if (!(_c)) {                                                                                                   \
            TEST_PRINT_ERROR(descr);                                                                                   \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define TEST_ASSERT(_c) TEST_ASSERT_MESSAGE("Failed assertion: " #_c, _c)

#ifndef UNUSED
#define UNUSED(x) (void)(x)
#endif

static inline unsigned long long getMonotonicNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((unsigned long long)ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

static inline void elapsedMonoStart(unsigned long long *start_time) {
    *start_time = getMonotonicNs();
}

static inline unsigned long long elapsedMonoNs(unsigned long long start_time) {
    return getMonotonicNs() - start_time;
}

#endif
