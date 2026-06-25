/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for featureSearchSIMD - tests actual implementations
 */

#include "generated_wrappers.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "config.h"
#include "zmalloc.h"
}

#define TEST_ASSERT(x) ASSERT_TRUE(x)
#define TEST_ASSERT_MESSAGE(msg, x) ASSERT_TRUE(x) << msg

/* Constants from fbtree_ordered_index.c */
#define FEATURE_SIZE 4
#define FEATURE_ROW_SIZE 64
#define FEATURE_BIAS 0x80

#define BIASED(x) ((char)((unsigned char)(x) ^ FEATURE_BIAS))

#define TEST_SETUP()                               \
    char features[FEATURE_SIZE][FEATURE_ROW_SIZE]; \
    initFeatures(features)

static void initFeatures(char features[FEATURE_SIZE][FEATURE_ROW_SIZE]) {
    for (int j = 0; j < FEATURE_SIZE; j++)
        for (int i = 0; i < FEATURE_ROW_SIZE; i++)
            features[j][i] = BIASED(0);
}

/* Declare test wrappers - these call actual code in fbtree_ordered_index.c */
extern "C" {
void featureSearchSIMD_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                    int num_keys,
                                    const unsigned char target[FEATURE_SIZE],
                                    int *out_left,
                                    int *out_right);

#if HAVE_X86_SIMD
void featureSearchSIMD_avx2_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                         int num_keys,
                                         const unsigned char target[FEATURE_SIZE],
                                         int *out_left,
                                         int *out_right);

void featureSearchSIMD_sse2_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                         int num_keys,
                                         const unsigned char target[FEATURE_SIZE],
                                         int *out_left,
                                         int *out_right);
#endif

void featureSearchSIMD_scalar_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                           int num_keys,
                                           const unsigned char target[FEATURE_SIZE],
                                           int *out_left,
                                           int *out_right);
}

typedef void (*FeatureSearchFn)(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                int num_keys,
                                const unsigned char target[FEATURE_SIZE],
                                int *out_left,
                                int *out_right);

/* Test all available implementations produce identical results against scalar (truth) */
static void testAllImpls(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                         int num_keys,
                         const unsigned char target[FEATURE_SIZE]) {
    int scalar_left, scalar_right;
    featureSearchSIMD_scalar_test_wrapper(features, num_keys, target, &scalar_left, &scalar_right);

    int def_left, def_right;
    featureSearchSIMD_test_wrapper(features, num_keys, target, &def_left, &def_right);
    TEST_ASSERT_MESSAGE("default mismatch", def_left == scalar_left && def_right == scalar_right);

#if HAVE_X86_SIMD
    int sse_left, sse_right;
    featureSearchSIMD_sse2_test_wrapper(features, num_keys, target, &sse_left, &sse_right);
    TEST_ASSERT_MESSAGE("sse2 mismatch", sse_left == scalar_left && sse_right == scalar_right);

    if (__builtin_cpu_supports("avx2")) {
        int avx_left, avx_right;
        featureSearchSIMD_avx2_test_wrapper(features, num_keys, target, &avx_left, &avx_right);
        TEST_ASSERT_MESSAGE("avx2 mismatch", avx_left == scalar_left && avx_right == scalar_right);
    }
#endif
}

/* ==========================================================================
 * Expected values test - demonstrates the semantics of featureSearchSIMD.
 * Range [left, right) = candidate children that might contain target.
 * ========================================================================== */

TEST(FeatureSearchTest, expected_values) {
    TEST_SETUP();
    features[0][0] = BIASED(0x20);
    features[0][1] = BIASED(0x40);
    features[0][2] = BIASED(0x40);
    features[0][3] = BIASED(0x40);
    features[0][4] = BIASED(0x60);
    features[0][5] = BIASED(0x80);
    int left, right;

    /* Case 1: Target before all - empty range (no candidates) */
    unsigned char t1[FEATURE_SIZE] = {0x10, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 6, t1, &left, &right);
    TEST_ASSERT_MESSAGE("before all: left", left == 0);
    TEST_ASSERT_MESSAGE("before all: right", right == 0);
    testAllImpls(features, 6, t1);

    /* Case 2: Target after all - empty range (no candidates) */
    unsigned char t2[FEATURE_SIZE] = {0x90, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 6, t2, &left, &right);
    TEST_ASSERT_MESSAGE("after all: left", left == 6);
    TEST_ASSERT_MESSAGE("after all: right", right == 6);
    testAllImpls(features, 6, t2);

    /* Case 3: Exact match on unique value - 1 candidate */
    unsigned char t3[FEATURE_SIZE] = {0x20, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 6, t3, &left, &right);
    TEST_ASSERT_MESSAGE("unique match: left", left == 0);
    TEST_ASSERT_MESSAGE("unique match: right", right == 1);
    testAllImpls(features, 6, t3);

    /* Case 4: Match on duplicates - few candidates (3 matches) */
    unsigned char t4[FEATURE_SIZE] = {0x40, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 6, t4, &left, &right);
    TEST_ASSERT_MESSAGE("duplicates: left", left == 1);
    TEST_ASSERT_MESSAGE("duplicates: right", right == 4);
    testAllImpls(features, 6, t4);

    /* Case 5: All same features - all candidates (nothing narrowed) */
    for (int i = 0; i < 5; i++) features[0][i] = BIASED(0x50);
    unsigned char t5[FEATURE_SIZE] = {0x50, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 5, t5, &left, &right);
    TEST_ASSERT_MESSAGE("all same: left", left == 0);
    TEST_ASSERT_MESSAGE("all same: right", right == 5);
    testAllImpls(features, 5, t5);

    /* Case 6: Between values - empty range (target 0x50 between 0x40 and 0x60) */
    features[0][0] = BIASED(0x20);
    features[0][1] = BIASED(0x40);
    features[0][2] = BIASED(0x60);
    features[0][3] = BIASED(0x80);
    unsigned char t6[FEATURE_SIZE] = {0x50, 0, 0, 0};
    featureSearchSIMD_scalar_test_wrapper(features, 4, t6, &left, &right);
    TEST_ASSERT_MESSAGE("between: left", left == 2);
    TEST_ASSERT_MESSAGE("between: right", right == 2);
    testAllImpls(features, 4, t6);
}

/* ==========================================================================
 * Edge cases - empty, single element, two elements
 * ========================================================================== */

TEST(FeatureSearchTest, empty) {
    TEST_SETUP();
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 0, target);
}

TEST(FeatureSearchTest, single_match) {
    TEST_SETUP();
    features[0][0] = BIASED(0x50);
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 1, target);
}

TEST(FeatureSearchTest, single_less) {
    TEST_SETUP();
    features[0][0] = BIASED(0x50);
    unsigned char target[FEATURE_SIZE] = {0x40, 0, 0, 0};
    testAllImpls(features, 1, target);
}

TEST(FeatureSearchTest, single_greater) {
    TEST_SETUP();
    features[0][0] = BIASED(0x50);
    unsigned char target[FEATURE_SIZE] = {0x60, 0, 0, 0};
    testAllImpls(features, 1, target);
}

TEST(FeatureSearchTest, two_elements) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x70);
    unsigned char targets[][FEATURE_SIZE] = {
        {0x20, 0, 0, 0}, {0x30, 0, 0, 0}, {0x50, 0, 0, 0}, {0x70, 0, 0, 0}, {0x80, 0, 0, 0}};
    for (int i = 0; i < 5; i++) {
        testAllImpls(features, 2, targets[i]);
    }
}

/* ==========================================================================
 * Basic scenarios - small arrays, before/after all, not found
 * ========================================================================== */

TEST(FeatureSearchTest, three_elements) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x50);
    features[0][2] = BIASED(0x70);
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 3, target);
}

TEST(FeatureSearchTest, before_all) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x50);
    features[0][2] = BIASED(0x70);
    unsigned char target[FEATURE_SIZE] = {0x20, 0, 0, 0};
    testAllImpls(features, 3, target);
}

TEST(FeatureSearchTest, after_all) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x50);
    features[0][2] = BIASED(0x70);
    unsigned char target[FEATURE_SIZE] = {0x80, 0, 0, 0};
    testAllImpls(features, 3, target);
}

TEST(FeatureSearchTest, not_found) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x50);
    features[0][2] = BIASED(0x70);
    unsigned char target[FEATURE_SIZE] = {0x45, 0, 0, 0}; /* Between 0x30 and 0x50 */
    testAllImpls(features, 3, target);
}

/* ==========================================================================
 * Duplicates - multiple matching features
 * ========================================================================== */

TEST(FeatureSearchTest, duplicates) {
    TEST_SETUP();
    features[0][0] = BIASED(0x30);
    features[0][1] = BIASED(0x50);
    features[0][2] = BIASED(0x50);
    features[0][3] = BIASED(0x50);
    features[0][4] = BIASED(0x70);
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 5, target);
}

TEST(FeatureSearchTest, all_same) {
    TEST_SETUP();
    for (int i = 0; i < 10; i++)
        features[0][i] = BIASED(0x50);
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 10, target);
}

/* ==========================================================================
 * Multi-byte features - tests all 4 feature bytes
 * ========================================================================== */

TEST(FeatureSearchTest, multibyte) {
    TEST_SETUP();
    features[0][0] = BIASED(0x50);
    features[1][0] = BIASED(0x10);
    features[0][1] = BIASED(0x50);
    features[1][1] = BIASED(0x30);
    features[0][2] = BIASED(0x50);
    features[1][2] = BIASED(0x50);
    unsigned char target[FEATURE_SIZE] = {0x50, 0x30, 0, 0};
    testAllImpls(features, 3, target);
}

TEST(FeatureSearchTest, multibyte_tiebreak) {
    TEST_SETUP();
    for (int i = 0; i < 5; i++) {
        features[0][i] = BIASED(0x50);
        features[1][i] = BIASED(i * 0x20);
    }
    unsigned char target[FEATURE_SIZE] = {0x50, 0x40, 0, 0};
    testAllImpls(features, 5, target);
}

TEST(FeatureSearchTest, all_bytes_matter) {
    TEST_SETUP();
    for (int i = 0; i < 4; i++) {
        features[0][i] = BIASED(0x50);
        features[1][i] = BIASED(0x50);
        features[2][i] = BIASED(0x50);
        features[3][i] = BIASED(i * 0x30);
    }
    unsigned char target[FEATURE_SIZE] = {0x50, 0x50, 0x50, 0x60};
    testAllImpls(features, 4, target);
}

/* Test that each byte position can be the deciding factor.
 * 4 children with features: all bytes equal except one differs.
 * Child 0: {0x50, 0x50, 0x50, 0x50} - all match
 * Child 1: differs only on byte being tested */
TEST(FeatureSearchTest, deciding_byte) {
    TEST_SETUP();

    /* Test each byte position (1, 2, 3) as the deciding factor.
     * Byte 0 is already well-tested by single-byte tests. */
    for (int deciding_byte = 1; deciding_byte < FEATURE_SIZE; deciding_byte++) {
        initFeatures(features);

        /* Set up 4 children, all with 0x50 in all bytes */
        for (int child = 0; child < 4; child++)
            for (int byte = 0; byte < FEATURE_SIZE; byte++)
                features[byte][child] = BIASED(0x50);

        /* Make children differ only on the deciding byte */
        features[deciding_byte][0] = BIASED(0x20);
        features[deciding_byte][1] = BIASED(0x40);
        features[deciding_byte][2] = BIASED(0x60);
        features[deciding_byte][3] = BIASED(0x80);

        /* Target matches child 1 exactly */
        unsigned char target[FEATURE_SIZE] = {0x50, 0x50, 0x50, 0x50};
        target[deciding_byte] = 0x40;
        testAllImpls(features, 4, target);

        /* Target between child 1 and 2 - should find empty range */
        target[deciding_byte] = 0x50;
        testAllImpls(features, 4, target);

        /* Target less than all on deciding byte */
        target[deciding_byte] = 0x10;
        testAllImpls(features, 4, target);

        /* Target greater than all on deciding byte */
        target[deciding_byte] = 0x90;
        testAllImpls(features, 4, target);
    }
}

/* ==========================================================================
 * Value boundaries - signed/unsigned edge cases
 * ========================================================================== */

TEST(FeatureSearchTest, high_values) {
    TEST_SETUP();
    features[0][0] = BIASED(0x7F);
    features[0][1] = BIASED(0x80);
    features[0][2] = BIASED(0xFF);
    unsigned char target[FEATURE_SIZE] = {0x80, 0, 0, 0};
    testAllImpls(features, 3, target);
}

TEST(FeatureSearchTest, boundary_values) {
    TEST_SETUP();
    features[0][0] = BIASED(0x00);
    features[0][1] = BIASED(0x7F);
    features[0][2] = BIASED(0x80);
    features[0][3] = BIASED(0xFF);
    unsigned char targets[][FEATURE_SIZE] = {
        {0x00, 0, 0, 0}, {0x7F, 0, 0, 0}, {0x80, 0, 0, 0}, {0xFF, 0, 0, 0}};
    for (int i = 0; i < 4; i++) {
        testAllImpls(features, 4, targets[i]);
    }
}

/* ==========================================================================
 * SIMD-specific - chunk boundaries, validity masks
 * ========================================================================== */

TEST(FeatureSearchTest, cross_chunk_boundary) {
    TEST_SETUP();
    int boundaries[] = {16, 32}; /* SSE and AVX boundaries */
    int sizes[] = {20, 40};
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};

    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < sizes[b]; i++)
            features[0][i] = BIASED(i < boundaries[b] ? 0x30 : 0x70);
        testAllImpls(features, sizes[b], target);
    }
}

TEST(FeatureSearchTest, chunk_boundaries) {
    TEST_SETUP();
    int sizes[] = {16, 32, 48};
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < sizes[s]; i++)
            features[0][i] = BIASED(i * 4);
        unsigned char target[FEATURE_SIZE] = {(unsigned char)(sizes[s] / 2 * 4), 0, 0, 0};
        testAllImpls(features, sizes[s], target);
    }
}

TEST(FeatureSearchTest, last_in_chunk) {
    TEST_SETUP();
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    int positions[] = {15, 31, 47, 63};
    int sizes[] = {20, 40, 55, 64};

    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < sizes[t]; i++)
            features[0][i] = BIASED(i == positions[t] ? 0x50 : 0x30);
        testAllImpls(features, sizes[t], target);
    }
}

TEST(FeatureSearchTest, first_in_chunk) {
    TEST_SETUP();
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    int positions[] = {16, 32, 48};
    int sizes[] = {20, 40, 55};

    for (int t = 0; t < 3; t++) {
        for (int i = 0; i < sizes[t]; i++)
            features[0][i] = BIASED(i == positions[t] ? 0x50 : 0x70);
        testAllImpls(features, sizes[t], target);
    }
}

TEST(FeatureSearchTest, no_false_positives) {
    TEST_SETUP();

    /* Set up: valid keys are all 0x30, but invalid positions have 0x50 */
    for (int i = 0; i < 64; i++)
        features[0][i] = BIASED(i < 20 ? 0x30 : 0x50);

    /* Search for 0x50 - should NOT find it since it's beyond num_keys=20 */
    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 20, target);
}

TEST(FeatureSearchTest, duplicates_cross_chunk) {
    TEST_SETUP();

    /* Duplicates from position 14 to 18 (crosses chunk 0/1 boundary) */
    for (int i = 0; i < 30; i++)
        features[0][i] = BIASED(i < 14 ? 0x30 : (i < 19 ? 0x50 : 0x70));

    unsigned char target[FEATURE_SIZE] = {0x50, 0, 0, 0};
    testAllImpls(features, 30, target);
}

/* ==========================================================================
 * Realistic sizes - full row, typical node size
 * ========================================================================== */

TEST(FeatureSearchTest, full_row) {
    TEST_SETUP();
    for (int i = 0; i < 64; i++)
        features[0][i] = BIASED(i * 3);
    unsigned char target[FEATURE_SIZE] = {96, 0, 0, 0};
    testAllImpls(features, 64, target);
}

TEST(FeatureSearchTest, typical_node_size) {
    TEST_SETUP();
    for (int i = 0; i < 61; i++)
        features[0][i] = BIASED(i * 4);
    unsigned char targets[][FEATURE_SIZE] = {
        {0, 0, 0, 0},
        {60, 0, 0, 0},
        {64, 0, 0, 0},
        {124, 0, 0, 0},
        {128, 0, 0, 0},
        {188, 0, 0, 0},
        {192, 0, 0, 0},
        {240, 0, 0, 0},
    };
    for (int i = 0; i < 8; i++) {
        testAllImpls(features, 61, targets[i]);
    }
}
