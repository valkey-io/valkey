/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include "test_help.h"

/* Daily also enables the legacy tests compiled inside quicklist.c. */
#ifdef SERVER_TEST
#define TEST_ACCURATE UNIT_TEST_ACCURATE
#define TEST_LARGE_MEMORY UNIT_TEST_LARGE_MEMORY
#endif

#include "../quicklist.c"

int test_quicklistMergeSizeOverflowRejected(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    if (sizeof(size_t) < 8) return 0;

    /* Fake packed nodes exercise the size check without allocating 4 GiB. */
    quicklistNode *a = quicklistCreateNode();
    quicklistNode *b = quicklistCreateNode();
    a->sz = (size_t)4294967200U;
    b->sz = 200;
    int allowed = _quicklistNodeAllowMerge(a, b, -2);
    zfree(a);
    zfree(b);
    TEST_ASSERT(!allowed);
    return 0;
}
