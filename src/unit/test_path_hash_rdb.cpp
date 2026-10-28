/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "rdb.h"
#include "server.h"
}

TEST(PathHashRdbTest, EmptyContainerIsSkippedWithoutCreatingAnObject) {
    unsigned char data[] = {0}; /* RDB path count, not an empty path. */
    sds buffer = sdsnewlen(data, sizeof(data));
    rio input;
    rioInitWithBuffer(&input, buffer);
    int error = 0;
    robj *o = rdbLoadObject(RDB_TYPE_PATH_HASH, &input, NULL, 0, &error, RDBFLAGS_NONE, 0);
    EXPECT_EQ(o, (robj *)NULL);
    EXPECT_EQ(error, RDB_LOAD_ERR_EMPTY_KEY);
    EXPECT_EQ(rioTell(&input), 1);
    if (o) decrRefCount(o);
    sdsfree(buffer);
}
