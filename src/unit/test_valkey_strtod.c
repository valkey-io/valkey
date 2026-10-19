/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "../valkey_strtod.h"
#include "errno.h"
#include "math.h"
#include "test_help.h"

int test_valkey_strtod(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    errno = 0;
    double value = valkey_strtod("231.2341234", NULL);
    TEST_ASSERT(value == 231.2341234);
    TEST_ASSERT(errno == 0);

    value = valkey_strtod("+inf", NULL);
    TEST_ASSERT(isinf(value));
    TEST_ASSERT(errno == 0);

    value = valkey_strtod("-inf", NULL);
    TEST_ASSERT(isinf(value));
    TEST_ASSERT(errno == 0);

    value = valkey_strtod("inf", NULL);
    TEST_ASSERT(isinf(value));
    TEST_ASSERT(errno == 0);

    const char *compatibility_inputs[] = {
        "0x0", "0X10", "-0x1.8p+1", "+0XAp-1", "-0x0",
        "0x1p-1074", "0x1p-1075", "0x1p1024", "0x1p+",
        "0x", "-0x", " 0x2p1", "\t-3.5", "\n+inf", " nan", " "};
    for (size_t i = 0; i < sizeof(compatibility_inputs) / sizeof(compatibility_inputs[0]); i++) {
        char *expected_end, *actual_end;
        errno = 0;
        double expected = strtod(compatibility_inputs[i], &expected_end);
        int expected_errno = errno;
        double actual = valkey_strtod(compatibility_inputs[i], &actual_end);
        TEST_ASSERT(errno == expected_errno);
        TEST_ASSERT(actual_end == expected_end);
        TEST_ASSERT(isnan(expected) ? isnan(actual) : actual == expected);
        if (expected == 0.0) TEST_ASSERT(signbit(actual) == signbit(expected));
    }

    return 0;
}
