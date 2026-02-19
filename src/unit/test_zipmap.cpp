/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "zipmap.h"
}

class ZipmapTest : public ::testing::Test {
};

TEST_F(ZipmapTest, zipmapIterateWithLargeKey) {
    char zm[] = "\x04"
                "\x04"
                "name"
                "\x03\x00"
                "foo"
                "\x07"
                "surname"
                "\x03\x00"
                "foo"
                "\x05"
                "noval"
                "\x00\x00"
                "\xfe\x00\x02\x00\x00"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                "\x04\x00"
                "long"
                "\xff";
    ASSERT_TRUE(zipmapValidateIntegrity((unsigned char *)zm, sizeof zm - 1, 1));

    unsigned char *p = zipmapRewind((unsigned char *)zm);
    unsigned char *key, *value;
    unsigned int klen, vlen;
    char buf[512];
    memset(buf, 'a', 512);
    char *expected_key[] = {(char *)"name", (char *)"surname", (char *)"noval", buf};
    char *expected_value[] = {(char *)"foo", (char *)"foo", nullptr, (char *)"long"};
    unsigned int expected_klen[] = {4, 7, 5, 512};
    unsigned int expected_vlen[] = {3, 3, 0, 4};
    int iter = 0;

    while ((p = zipmapNext(p, &key, &klen, &value, &vlen)) != nullptr) {
        char *tmp = expected_key[iter];
        ASSERT_EQ(klen, expected_klen[iter]);
        ASSERT_EQ(strncmp(tmp, (const char *)key, klen), 0);
        tmp = expected_value[iter];
        ASSERT_EQ(vlen, expected_vlen[iter]);
        if (tmp == nullptr) {
            ASSERT_EQ(vlen, 0u);
        } else {
            ASSERT_EQ(strncmp(tmp, (const char *)value, vlen), 0);
        }
        iter++;
    }
}

TEST_F(ZipmapTest, zipmapIterateThroughElements) {
    char zm[] = "\x06"
                "\x04"
                "name"
                "\x03\x00"
                "foo"
                "\x07"
                "surname"
                "\x03\x00"
                "foo"
                "\x03"
                "age"
                "\x03\x00"
                "foo"
                "\x05"
                "hello"
                "\x06\x00"
                "world!"
                "\x03"
                "foo"
                "\x05\x00"
                "12345"
                "\x05"
                "noval"
                "\x00\x00"
                "\xff";
    ASSERT_TRUE(zipmapValidateIntegrity((unsigned char *)zm, sizeof zm - 1, 1));

    unsigned char *i = zipmapRewind((unsigned char *)zm);
    unsigned char *key, *value;
    unsigned int klen, vlen;
    char *expected_key[] = {(char *)"name", (char *)"surname", (char *)"age", (char *)"hello", (char *)"foo", (char *)"noval"};
    char *expected_value[] = {(char *)"foo", (char *)"foo", (char *)"foo", (char *)"world!", (char *)"12345", (char *)""};
    unsigned int expected_klen[] = {4, 7, 3, 5, 3, 5};
    unsigned int expected_vlen[] = {3, 3, 3, 6, 5, 0};
    int iter = 0;

    while ((i = zipmapNext(i, &key, &klen, &value, &vlen)) != nullptr) {
        char *tmp = expected_key[iter];
        ASSERT_EQ(klen, expected_klen[iter]);
        ASSERT_EQ(strncmp(tmp, (const char *)key, klen), 0);
        tmp = expected_value[iter];
        ASSERT_EQ(vlen, expected_vlen[iter]);
        ASSERT_EQ(strncmp(tmp, (const char *)value, vlen), 0);
        iter++;
    }
}
