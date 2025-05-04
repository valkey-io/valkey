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

    return 0;
}
