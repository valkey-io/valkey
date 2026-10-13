/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cerrno>
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "util.h"
#include "valkey_strtod.h"
}

class ValkeyStrtodTest : public ::testing::Test {};

TEST_F(ValkeyStrtodTest, TestValkeyStrtod) {
    errno = 0;
    double value = valkey_strtod("231.2341234", NULL);
    ASSERT_DOUBLE_EQ(value, 231.2341234);
    ASSERT_EQ(errno, 0);

    value = valkey_strtod("+inf", NULL);
    ASSERT_TRUE(isinf(value));
    ASSERT_EQ(errno, 0);

    value = valkey_strtod("-inf", NULL);
    ASSERT_TRUE(isinf(value));
    ASSERT_EQ(errno, 0);

    value = valkey_strtod("inf", NULL);
    ASSERT_TRUE(isinf(value));
    ASSERT_EQ(errno, 0);
}

TEST_F(ValkeyStrtodTest, TestValkeyStrtodN) {
    errno = 0;
    double value = valkey_strtod_n("231.2341234", 11, NULL);
    ASSERT_DOUBLE_EQ(value, 231.2341234);
    ASSERT_EQ(errno, 0);

    value = valkey_strtod_n("+inf", 4, NULL);
    ASSERT_TRUE(isinf(value));
    ASSERT_EQ(errno, 0);

    value = valkey_strtod_n("-inf", 4, NULL);
    ASSERT_TRUE(isinf(value));
    ASSERT_EQ(errno, 0);

    value = valkey_strtod_n("inf", 3, NULL);
    ASSERT_TRUE(isinf(value));
    ASSERT_EQ(errno, 0);
}

/* True if [s, s+slen) is a complete number (same consume rule as string2d / zslParseRange). */
static int parse_consumed(const char *s, size_t slen, char *end, int err) {
    return err == 0 && end != NULL && end == s + slen;
}

/* Hex floats 9.0 accepted via libc strtod.
 * ZADD/ZINCRBY -> string2d; closed ranges -> valkey_strtod_n;
 * exclusive ranges strip a leading '(' then call valkey_strtod_n. */
TEST_F(ValkeyStrtodTest, TestHexFloat) {
    const char *inputs[] = {
        "0x10",
        "0X10",
        "0x1p4",
        "+0x10",
        "-0x10",
        "0x0",
        "0x100",
        "0x1.0p4",
        "0x1p-1",
        "0X1P4",
        "0x",
        "0xg",
        "0xp4",
    };
    const char *exclusive[] = {
        "(0x10",
        "(0x0",
        "(0x100",
        "(0x1p4",
        "(0x1.0p4",
        "(0xg",
    };
    size_t n = sizeof(inputs) / sizeof(inputs[0]);
    size_t nexcl = sizeof(exclusive) / sizeof(exclusive[0]);
    size_t i;

    for (i = 0; i < n; i++) {
        const char *s = inputs[i];
        size_t slen = strlen(s);
        char *libc_end = NULL;
        char *vk_end = NULL;
        char *vkn_end = NULL;
        double libc_val;
        double vk_val;
        double vkn_val;
        double string2d_val = 0.0;
        int libc_ok;
        int vk_ok;
        int vkn_ok;
        int string2d_ok;

        errno = 0;
        libc_val = strtod(s, &libc_end);
        libc_ok = parse_consumed(s, slen, libc_end, errno);

        errno = 0;
        vk_val = valkey_strtod(s, &vk_end);
        vk_ok = parse_consumed(s, slen, vk_end, errno);

        errno = 0;
        vkn_val = valkey_strtod_n(s, slen, &vkn_end);
        vkn_ok = parse_consumed(s, slen, vkn_end, errno);

        string2d_ok = string2d(s, slen, &string2d_val);

        EXPECT_EQ(vk_ok, libc_ok);
        EXPECT_EQ(vkn_ok, libc_ok);
        EXPECT_EQ(string2d_ok, libc_ok);
        if (libc_ok && vk_ok) {
            EXPECT_DOUBLE_EQ(vk_val, libc_val);
        }
        if (libc_ok && vkn_ok) {
            EXPECT_DOUBLE_EQ(vkn_val, libc_val);
        }
        if (libc_ok && string2d_ok) {
            EXPECT_DOUBLE_EQ(string2d_val, libc_val);
        }
    }

    for (i = 0; i < nexcl; i++) {
        const char *s = exclusive[i];
        size_t slen = strlen(s);
        const char *inner = s + 1;
        size_t innerlen = slen - 1;
        char *libc_end = NULL;
        char *vkn_end = NULL;
        double libc_val;
        double vkn_val;
        int libc_ok;
        int vkn_ok;

        errno = 0;
        libc_val = strtod(inner, &libc_end);
        libc_ok = parse_consumed(inner, innerlen, libc_end, errno);

        errno = 0;
        vkn_val = valkey_strtod_n(inner, innerlen, &vkn_end);
        /* zslParseRange: eptr[0] != '\0' means the range token is invalid. */
        vkn_ok = (errno == 0 && vkn_end != NULL && vkn_end[0] == '\0' && !isnan(vkn_val));

        EXPECT_EQ(vkn_ok, libc_ok);
        if (libc_ok && vkn_ok) {
            EXPECT_DOUBLE_EQ(vkn_val, libc_val);
        }
    }
}
