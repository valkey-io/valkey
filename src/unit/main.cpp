/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
#include "sha256.h"
#include "util.h"
}


bool accurate = false;
bool large_memory = false;
bool valgrind = false;
char *seed = nullptr;
int test_argc = 0;
char **test_argv = nullptr;

bool hasFlag(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

char *getFlagValue(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return nullptr;
}

int main(int argc, char **argv) {
    accurate = hasFlag(argc, argv, "--accurate");
    large_memory = hasFlag(argc, argv, "--large-memory");
    valgrind = hasFlag(argc, argv, "--valgrind");
    seed = getFlagValue(argc, argv, "--seed");
    if (seed) {
        unsigned int seed_value = (unsigned int)atoi(seed);
        srandom(seed_value);
        srand(seed_value);

        // Convert the seed to a 128-character hexadecimal string
        // by hashing it with SHA256 twice (to get 64 bytes = 128 hex chars)
        char seed_hex[129];
        SHA256_CTX ctx;
        unsigned char hash[SHA256_BLOCK_SIZE];

        // First hash
        sha256_init(&ctx);
        sha256_update(&ctx, (const unsigned char *)seed, strlen(seed));
        sha256_final(&ctx, hash);

        // Convert first hash to hex (32 bytes = 64 hex chars)
        for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
            snprintf(seed_hex + (i * 2), 3, "%02X", hash[i]);
        }

        // Second hash to get another 32 bytes
        sha256_init(&ctx);
        sha256_update(&ctx, hash, SHA256_BLOCK_SIZE);
        sha256_final(&ctx, hash);

        // Convert second hash to hex (32 bytes = 64 hex chars)
        for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
            snprintf(seed_hex + 64 + (i * 2), 3, "%02X", hash[i]);
        }
        seed_hex[128] = '\0';

        // Now we have a 128-character hex string
        setRandomSeedCString(seed_hex, strlen(seed_hex));
    }

    // The following line must be executed to initialize GoogleTest before running the tests.
    ::testing::InitGoogleMock(&argc, argv);

    // The tests are designed to run one per process, and they do not clean up
    // all the global state they change. Refuse a run that would execute every
    // test in one process. The filter is read from GoogleTest, so it also
    // covers the GTEST_FILTER environment variable.
    // gtest-parallel lists the tests and then runs each one with its own filter.
    // Only the positive part, before the first '-', selects tests. If it is
    // empty, as in "-Suite.Test", GoogleTest treats it as "*". Keep a copy of
    // the flag: with the Abseil flag backend GTEST_FLAG_GET returns a temporary.
    std::string filter_flag = GTEST_FLAG_GET(filter);
    const char *filter = filter_flag.c_str();
    const char *dash = strchr(filter, '-');
    size_t positive_len = dash ? (size_t)(dash - filter) : strlen(filter);
    bool runs_everything = positive_len == 0 || (positive_len == 1 && filter[0] == '*');
    if (runs_everything && !GTEST_FLAG_GET(list_tests) &&
        GTEST_FLAG_GET(internal_run_death_test).empty()) {
        fprintf(stderr, "Unit tests are designed to run one per process. Use 'make test-unit' to run them all, "
                        "or choose tests with --gtest_filter=Suite.Test (for example to debug one test in gdb).\n");
        return 2;
    }

    // Set death test style to threadsafe when running under Valgrind
    if (valgrind) {
        GTEST_FLAG_SET(death_test_style, "threadsafe");
    }

    test_argc = argc;
    test_argv = argv;
    int result = RUN_ALL_TESTS();
    if (result == 0) {
        printf("\033[32mAll UNIT TESTS PASSED!\033[0m\n");
    }
    return result;
}
