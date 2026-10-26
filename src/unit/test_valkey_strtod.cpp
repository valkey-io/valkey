/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cerrno>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
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

static void expect_same_double(double a, double b) {
    if (isnan(a) || isnan(b)) {
        EXPECT_TRUE(isnan(a) && isnan(b));
        return;
    }
    EXPECT_DOUBLE_EQ(a, b);
}

/* Compare valkey_strtod / valkey_strtod_n against libc strtod.
 * string2d is the ZADD path: 9.0 also rejected leading space and NaN. */
static void expect_matches_libc(const char *s, size_t slen) {
    char libc_buf[64];
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
    int string2d_expect;

    ASSERT_LT(slen, sizeof(libc_buf));
    memcpy(libc_buf, s, slen);
    libc_buf[slen] = '\0';

    errno = 0;
    libc_val = strtod(libc_buf, &libc_end);
    libc_ok = parse_consumed(libc_buf, slen, libc_end, errno);

    errno = 0;
    vk_val = valkey_strtod(libc_buf, &vk_end);
    vk_ok = parse_consumed(libc_buf, slen, vk_end, errno);

    errno = 0;
    vkn_val = valkey_strtod_n(s, slen, &vkn_end);
    vkn_ok = parse_consumed(s, slen, vkn_end, errno);

    string2d_ok = string2d(s, slen, &string2d_val);
    string2d_expect = libc_ok && slen > 0 && !isspace((unsigned char)s[0]) && !isnan(libc_val);

    if (vk_ok != libc_ok || vkn_ok != libc_ok) {
        fprintf(stderr, "strtod oracle mismatch input='%.*s' libc=%d vk=%d vkn=%d\n",
                (int)slen, s, libc_ok, vk_ok, vkn_ok);
    }

    EXPECT_EQ(vk_ok, libc_ok);
    EXPECT_EQ(vkn_ok, libc_ok);
    EXPECT_EQ(string2d_ok, string2d_expect);
    if (libc_ok && vk_ok) {
        expect_same_double(vk_val, libc_val);
    }
    if (libc_ok && vkn_ok) {
        expect_same_double(vkn_val, libc_val);
    }
    if (string2d_expect && string2d_ok) {
        expect_same_double(string2d_val, libc_val);
    }
}

/* Hex / whitespace / inf / nan / leading-dot vs libc strtod.
 * ZADD/ZINCRBY -> string2d; closed ranges -> valkey_strtod_n. */
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
        "inf",
        "Inf",
        "INF",
        "+inf",
        "-inf",
        "infinity",
        "Infinity",
        "INFINITY",
        "+Infinity",
        "-infinity",
        "nan",
        "NaN",
        "NAN",
        "+nan",
        "-nan",
        ".5",
        "-.5",
        "+.5",
        ".",
        "5.",
        " 1",
        " 0",
        " 3",
        "\t2",
        "\n4",
        " +1",
        " -2",
        " 0x10",
    };
    size_t n = sizeof(inputs) / sizeof(inputs[0]);
    size_t i;
    unsigned int seed = 42;
    int fuzz_i;

    for (i = 0; i < n; i++) {
        expect_matches_libc(inputs[i], strlen(inputs[i]));
    }

    /* Random bytes without interior NUL, plus a float-ish alphabet corpus. */
    for (fuzz_i = 0; fuzz_i < 1024; fuzz_i++) {
        char buf[16];
        size_t slen = 1 + (size_t)(rand_r(&seed) % 15);
        size_t j;
        static const char alphabet[] = "0123456789.+-eExXaAbCfFnNiI.yY \t\n";

        if (fuzz_i < 512) {
            for (j = 0; j < slen; j++) {
                buf[j] = (char)(1 + (rand_r(&seed) % 255));
            }
        } else {
            for (j = 0; j < slen; j++) {
                buf[j] = alphabet[rand_r(&seed) % (sizeof(alphabet) - 1)];
            }
        }
        expect_matches_libc(buf, slen);
    }
}
