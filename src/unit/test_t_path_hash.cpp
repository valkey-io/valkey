/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "server.h"
}

TEST(PathHashTest, CountPrefixesAcrossCompressedEdgesAndBinaryPaths) {
    robj *o = createPathHashObject();
    pathHashObject *tree = (pathHashObject *)objectGetVal(o);
    unsigned char paths[][4] = {{0}, {'a'}, {'a', 'b'}, {'a', 'b', 'c', 'd'}, {'a', 'b', 'x'}, {0, 255}};
    size_t lengths[] = {0, 1, 2, 4, 3, 2};
    EXPECT_EQ(pathHashTypeCount(o, NULL, 0), 0u);
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        raxInsert(tree->index, paths[i], lengths[i], createHashObject(), NULL);
    }
    EXPECT_EQ(pathHashTypeCount(o, NULL, 0), 6u);
    EXPECT_EQ(pathHashTypeCount(o, (const unsigned char *)"a", 1), 4u);
    EXPECT_EQ(pathHashTypeCount(o, (const unsigned char *)"abc", 3), 1u);
    EXPECT_EQ(pathHashTypeCount(o, (const unsigned char *)"abcd", 4), 1u);
    EXPECT_EQ(pathHashTypeCount(o, (const unsigned char *)"abcde", 5), 0u);
    EXPECT_EQ(pathHashTypeCount(o, paths[5], 1), 1u);
    EXPECT_EQ(pathHashTypeCount(o, paths[5], 2), 1u);
    EXPECT_EQ(pathHashTypeCount(o, (const unsigned char *)"z", 1), 0u);
    void *removed = NULL;
    EXPECT_EQ(raxRemove(tree->index, paths[2], lengths[2], &removed), 1);
    decrRefCount((robj *)removed);
    EXPECT_EQ(pathHashTypeCount(o, (const unsigned char *)"ab", 2), 2u);
    decrRefCount(o);
}
