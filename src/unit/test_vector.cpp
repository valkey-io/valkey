/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdint>

extern "C" {
#include "fmacros.h"
#include "vector.h"
}

typedef struct {
    uint8_t uint8;
    uint64_t uint64;
} test_struct;

class VectorTest : public ::testing::Test {};

TEST_F(VectorTest, TestVector) {
    /* The test cases cover the following scenarios:
     * 1) Whether the array pre-allocates memory during initialization;
     * 2) Element sizes smaller than, equal to, and larger than sizeof(void*);
     * 3) Usage of each API.
     */
    vector uint8_vector;
    vector uint64_vector;
    vector struct_vector;
    vectorInit(&uint8_vector, 0, sizeof(uint8_t));
    vectorInit(&uint64_vector, 10, sizeof(uint64_t));
    vectorInit(&struct_vector, 128, sizeof(test_struct));

    for (uint64_t i = 0; i < 128; i++) {
        uint8_t *uint8_item = (uint8_t *)vectorPush(&uint8_vector);
        *uint8_item = i;

        uint64_t *uint64_item = (uint64_t *)vectorPush(&uint64_vector);
        *uint64_item = i * 1000;

        test_struct *struct_item = (test_struct *)vectorPush(&struct_vector);
        struct_item->uint8 = i;
        struct_item->uint64 = i * 1000;
    }

    /* uint8_vector length */
    ASSERT_EQ(vectorLen(&uint8_vector), 128u);
    /* uint64_vector length */
    ASSERT_EQ(vectorLen(&uint64_vector), 128u);
    /* struct_vector length */
    ASSERT_EQ(vectorLen(&struct_vector), 128u);

    for (uint32_t i = 0; i < vectorLen(&uint8_vector); i++) {
        uint8_t *uint8_item = (uint8_t *)(vectorGet(&uint8_vector, i));
        /* uint8_item value */
        ASSERT_EQ(*uint8_item, i);

        uint64_t *uint64_item = (uint64_t *)(vectorGet(&uint64_vector, i));
        /* uint64_item value */
        ASSERT_EQ(*uint64_item, i * 1000);

        test_struct *struct_item = (test_struct *)(vectorGet(&struct_vector, i));
        /* struct_item uint8 value */
        ASSERT_EQ(struct_item->uint8, i);
        /* struct_item uint64 value */
        ASSERT_EQ(struct_item->uint64, i * 1000);
    }

    vectorCleanup(&uint8_vector);
    vectorCleanup(&uint64_vector);
    vectorCleanup(&struct_vector);
}
