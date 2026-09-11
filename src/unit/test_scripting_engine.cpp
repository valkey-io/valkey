/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "sds.h"
#include "zmalloc.h"

/* Wrapper function declaration for accessing static scripting_engine.c
 * internals. */
sds *testOnlyWrapText(const char *text, size_t max_len, size_t *count);
}

static void freeLines(sds *lines, size_t count) {
    for (size_t i = 0; i < count; i++) sdsfree(lines[i]);
    zfree(lines);
}

TEST(WrapText, WrapsAtLastSpace) {
    size_t count = 0;
    sds *lines = testOnlyWrapText("hello world foo", 8, &count);
    ASSERT_EQ(count, 3u);
    EXPECT_STREQ(lines[0], "hello");
    EXPECT_STREQ(lines[1], "world");
    EXPECT_STREQ(lines[2], "foo");
    freeLines(lines, count);
}

TEST(WrapText, ShortTextSingleLine) {
    size_t count = 0;
    sds *lines = testOnlyWrapText("short", 49, &count);
    ASSERT_EQ(count, 1u);
    EXPECT_STREQ(lines[0], "short");
    freeLines(lines, count);
}

TEST(WrapText, EmptyTextReturnsNoLines) {
    size_t count = 99;
    sds *lines = testOnlyWrapText("", 49, &count);
    ASSERT_EQ(count, 0u);
    ASSERT_EQ(lines, nullptr);
}

TEST(WrapText, ExactWidthSingleLine) {
    size_t count = 0;
    sds *lines = testOnlyWrapText("aaaaaaaa", 8, &count);
    ASSERT_EQ(count, 1u);
    EXPECT_STREQ(lines[0], "aaaaaaaa");
    freeLines(lines, count);
}

/* A single word longer than the wrap width used to
 * trigger NULL pointer arithmetic and silently drop the remaining
 * text (returning only one line). */
TEST(WrapText, HardBreaksLongWordWithoutSpaces) {
    size_t count = 0;
    sds text = sdsempty();
    for (int i = 0; i < 60; i++) text = sdscat(text, "a");
    sds *lines = testOnlyWrapText(text, 49, &count);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(sdslen(lines[0]), 49u);
    EXPECT_EQ(sdslen(lines[1]), 11u);
    freeLines(lines, count);
    sdsfree(text);
}

/* Hard-break ending exactly at a separator must not emit an empty
 * line; the separator run is consumed like the space-wrap path. */
TEST(WrapText, HardBreakConsumesFollowingSeparator) {
    size_t count = 0;
    sds *lines = testOnlyWrapText("abc defgh", 3, &count);
    ASSERT_EQ(count, 3u);
    EXPECT_STREQ(lines[0], "abc");
    EXPECT_STREQ(lines[1], "def");
    EXPECT_STREQ(lines[2], "gh");
    freeLines(lines, count);
}
