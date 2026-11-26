/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdint>
#include <cstdio>
#include <pthread.h>

extern "C" {
#include "crc64.h"
#include "fmacros.h"

extern uint64_t _crc64(uint_fast64_t crc, const void *in_data, const uint64_t len);
}

class Crc64Test : public ::testing::Test {
  protected:
    void SetUp() override {
        crc64_init();
    }
};

TEST_F(Crc64Test, TestCrc64) {
    unsigned char numbers[] = "123456789";
    ASSERT_EQ((uint64_t)_crc64(0, numbers, 9), 16845390139448941002ull) << "[calcula]: CRC64 '123456789'";
    ASSERT_EQ((uint64_t)crc64(0, numbers, 9), 16845390139448941002ull) << "[calcula]: CRC64 '123456789'";

    unsigned char li[] = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed "
                         "do eiusmod tempor incididunt ut labore et dolore magna "
                         "aliqua. Ut enim ad minim veniam, quis nostrud exercitation "
                         "ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis "
                         "aute irure dolor in reprehenderit in voluptate velit esse "
                         "cillum dolore eu fugiat nulla pariatur. Excepteur sint "
                         "occaecat cupidatat non proident, sunt in culpa qui officia "
                         "deserunt mollit anim id est laborum.";
    ASSERT_EQ((uint64_t)_crc64(0, li, sizeof(li)), 14373597793578550195ull) << "[calcula]: CRC64 TEXT'";
    ASSERT_EQ((uint64_t)crc64(0, li, sizeof(li)), 14373597793578550195ull) << "[calcula]: CRC64 TEXT";
}

#define CRC64_THREADS 4
#define CRC64_THREAD_BUF_LEN (64 * 1024)

typedef struct {
    const unsigned char *buf;
    uint64_t crc;
} crc64ThreadArg;

static void *crc64Thread(void *arg) {
    crc64ThreadArg *a = (crc64ThreadArg *)arg;
    a->crc = crc64(0, a->buf, CRC64_THREAD_BUF_LEN);
    return NULL;
}

/* The forkless save thread computes CRC64 concurrently with the main thread.
 * The buffer is large enough to take the split path that uses
 * crc64_combine(), whose cache crc64_init() must fill before threads start. */
TEST_F(Crc64Test, TestCrc64Concurrent) {
    static unsigned char buf[CRC64_THREAD_BUF_LEN];
    for (int i = 0; i < CRC64_THREAD_BUF_LEN; i++) buf[i] = (unsigned char)(i * 31 + 7);
    uint64_t expected = _crc64(0, buf, CRC64_THREAD_BUF_LEN);

    pthread_t threads[CRC64_THREADS];
    crc64ThreadArg args[CRC64_THREADS];
    for (int i = 0; i < CRC64_THREADS; i++) {
        args[i].buf = buf;
        args[i].crc = 0;
        ASSERT_EQ(pthread_create(&threads[i], NULL, crc64Thread, &args[i]), 0);
    }
    for (int i = 0; i < CRC64_THREADS; i++) {
        ASSERT_EQ(pthread_join(threads[i], NULL), 0);
        ASSERT_EQ(args[i].crc, expected) << "thread " << i;
    }
}
