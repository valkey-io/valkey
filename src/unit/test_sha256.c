/*
 * Comprehensive unit tests for SHA-256 implementation.
 *
 * These tests verify:
 * 1. Basic functionality with known test vectors (e.g., "abc")
 * 2. Handling of large input data (4KB repeated 1000 times)
 * 3. Edge case with repeated single-byte input (1 million 'a' characters)
 *
 * The tests ensure compatibility with standard SHA-256 implementations
 * and will help detect regressions during future code changes.
 */

#include "../sha256.h"
#include "test_help.h"
#include <string.h>

#define BUFSIZE 4096

int test_sha256_abc(int argc, char **argv, int flags) {
    SHA256_CTX ctx;
    BYTE hash[32];
    BYTE expected[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    const char *test_str = "abc";

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    sha256_init(&ctx);
    sha256_update(&ctx, (BYTE *)test_str, 3);
    sha256_final(&ctx, hash);

    TEST_ASSERT(memcmp(hash, expected, 32) == 0);
    return 0;
}

int test_sha256_large(int argc, char **argv, int flags) {
    SHA256_CTX ctx;
    BYTE hash[32], buf[BUFSIZE];
    BYTE expected[32] = {
        0x8e, 0x44, 0xff, 0x94, 0xb6, 0x8f, 0xcb, 0x09,
        0x6a, 0x8d, 0x5c, 0xdb, 0x8f, 0x1c, 0xc7, 0x8a,
        0x9c, 0x47, 0x58, 0x45, 0xf1, 0x1a, 0x8d, 0x67,
        0x6f, 0x39, 0xc9, 0x53, 0x7e, 0xd2, 0x31, 0xe0};
    int i;

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    for (i = 0; i < BUFSIZE; i++)
        buf[i] = i % 256;

    sha256_init(&ctx);
    for (i = 0; i < 1000; i++)
        sha256_update(&ctx, buf, BUFSIZE);
    sha256_final(&ctx, hash);

    TEST_ASSERT(memcmp(hash, expected, 32) == 0);
    return 0;
}

int test_sha256_million_a(int argc, char **argv, int flags) {
    SHA256_CTX ctx;
    BYTE hash[32];
    BYTE expected[32] = {
        0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92,
        0x81, 0xa1, 0xc7, 0xe2, 0x84, 0xd7, 0x3e, 0x67,
        0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97, 0x20, 0x0e,
        0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0};
    int i;
    BYTE a = 'a';

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    sha256_init(&ctx);
    for (i = 0; i < 1000000; i++)
        sha256_update(&ctx, &a, 1);
    sha256_final(&ctx, hash);

    TEST_ASSERT(memcmp(hash, expected, 32) == 0);
    return 0;
}
