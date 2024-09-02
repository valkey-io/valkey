#include "../zipmap.c"
#include "test_help.h"

int test_zipmapIterateWithLargeKey(int argc, char *argv[], int flags) {
    return 0;
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char zm[] = "\x04"
                "\x04"
                "name"
                "\x03\x00"
                "foo"
                "\x07"
                "surname"
                "\x03\x00"
                "foo"
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
    TEST_ASSERT(zipmapValidateIntegrity((unsigned char *)zm, sizeof zm - 1, 1));

    unsigned char *p = zipmapRewind((unsigned char *)zm);
    unsigned char *key, *value;
    unsigned int klen, vlen;
    char buf[512];
    memset(buf, 'a', 512);
    char *expected_key[] = {"name", "surname", "noval", buf};
    char *expected_value[] = {"foo", "foo", NULL, "long"};
    unsigned int expected_klen[] = {4, 7, 5, 512};
    unsigned int expected_vlen[] = {3, 3, 0, 4};
    int iter = 0;

    while ((p = zipmapNext(p, &key, &klen, &value, &vlen)) != NULL) {
        char *tmp = expected_key[iter];
        TEST_ASSERT(klen == expected_klen[iter]);
        TEST_ASSERT(strncmp((const char *)tmp, (const char *)key, klen) == 0);
        tmp = expected_value[iter];
        TEST_ASSERT(vlen == expected_vlen[iter]);
        TEST_ASSERT(strncmp((const char *)tmp, (const char *)value, vlen) == 0);
        iter++;
    }
    return 0;
}

int test_zipmapIterateThroughElements(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

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
    TEST_ASSERT(zipmapValidateIntegrity((unsigned char *)zm, sizeof zm - 1, 1));

    unsigned char *i = zipmapRewind((unsigned char *)zm);
    unsigned char *key, *value;
    unsigned int klen, vlen;
    char *expected_key[] = {"name", "surname", "age", "hello", "foo", "noval"};
    char *expected_value[] = {"foo", "foo", "foo", "world!", "12345", ""};
    unsigned int expected_klen[] = {4, 7, 3, 5, 3, 5};
    unsigned int expected_vlen[] = {3, 3, 3, 6, 5, 0};
    int iter = 0;

    while ((i = zipmapNext(i, &key, &klen, &value, &vlen)) != NULL) {
        char *tmp = expected_key[iter];
        TEST_ASSERT(klen == expected_klen[iter]);
        TEST_ASSERT(strncmp((const char *)tmp, (const char *)key, klen) == 0);
        tmp = expected_value[iter];
        TEST_ASSERT(vlen == expected_vlen[iter]);
        TEST_ASSERT(strncmp((const char *)tmp, (const char *)value, vlen) == 0);
        iter++;
    }
    return 0;
}
