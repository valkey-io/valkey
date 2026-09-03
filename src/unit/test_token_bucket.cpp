/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for throttle_token_bucket.h (token bucket algorithm).
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "throttle_token_bucket.h"
static monotime fakeGetMonotonicUs(void);
static monotime (*origGetMonotonicUs)(void);
}

static monotime fakeMonotimeUs;

static monotime fakeGetMonotonicUs(void) {
    return fakeMonotimeUs;
}

class TokenBucketTest : public ::testing::Test {
  protected:
    tokenBucket *bucket;

    static void SetUpTestSuite() {
        origGetMonotonicUs = getMonotonicUs;
        getMonotonicUs = fakeGetMonotonicUs;
    }

    static void TearDownTestSuite() {
        getMonotonicUs = origGetMonotonicUs;
    }

    void SetUp() override {
        fakeMonotimeUs = 1000000;                /* start at 1 second */
        bucket = tokenBucket_create(100.0, 0.1); /* 100 tokens/sec, 0.1s burst */
    }

    void TearDown() override {
        tokenBucket_free(bucket);
    }
};

TEST_F(TokenBucketTest, BucketCreation) {
    /* Bucket starts full and can consume up to bucket capacity */
    EXPECT_DOUBLE_EQ(tokenBucket_getRate(bucket), 100.0);
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 10.0), 0.0);
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 1.0, false));
}

TEST_F(TokenBucketTest, HaltedBucket) {
    /* Set the rate to zero, this will also empty the bucket. */
    tokenBucket_setRate(bucket, 0.0);
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 1.0, false));
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), -1.0);

    fakeMonotimeUs += 1000000; /* advance 1 second */
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 1.0, false));
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 1.0, true));            /* Force consume should work */
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), -1.0); /* Never available */

    /* Now set the rate back to a positive value */
    tokenBucket_setRate(bucket, 100.0);
    fakeMonotimeUs += 1000000; /* advance 1 second, now the token bucket is refilled */
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 1.0, false));
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 11.0), 0.0); /* We should still have 11 tokens available */
}

TEST_F(TokenBucketTest, MsUntilAvailableReplenishes) {
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 12.0, false));          /* drain (size = 100*0.1+2 = 12) */
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 10.0); /* empty right now */

    fakeMonotimeUs += 1000000; /* advance 1s, bucket refills to full */

    /* Bucket is full again */
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 12.0), 0.0);

    /* Partial refill: drain again, advance only 5ms */
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 12.0, false));
    fakeMonotimeUs += 5000;                                           /* 0.5 token accrued */
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 5.0); /* need 5ms more */
}

TEST_F(TokenBucketTest, ConsumeTokens_normal) {
    /* Drain all tokens (bucket size = rate * burst_time + 2 = 100*0.1+2 = 12) */
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 12.0, false));
    /* Now empty */
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 10.0);
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 1.0, false));

    fakeMonotimeUs += 10000; // Advance 10ms -> 1 token replenished
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 1.1, false));
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 1.0, false));
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 10.0); /* Now empty */

    fakeMonotimeUs += 1000000;                                 /* advance 1 second */
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 12.1, false)); /* Cannot consume tokens over bucket capacity */
    for (int i = 0; i < 12; ++i) {
        EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 0.0);
        EXPECT_TRUE(tokenBucket_tryConsume(bucket, 1.0, false));
    }
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 10.0); /* Now empty */
}

TEST_F(TokenBucketTest, ConsumeTokens_force) {
    /* Drain all tokens (bucket size = rate * burst_time + 2 = 100*0.1+2 = 12) */
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 12.0, true));
    /* Now empty */
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 10.0);
    /* Force consume should work even when empty */
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 1.0, true));
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 20.0); /* Now we need to wait for 2 tokens to be available */
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 1.0, false));

    /* Force consume should work even when the token count is negative */
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 1.0, true));
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 30.0); /* Now we need to wait for 3 tokens to be available */
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 1.0, false));

    fakeMonotimeUs += 30000;
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 1.1, false));
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 1.0, false));

    /* Force consume can drop tokens below zero, but not below the minimum capacity (- bucket size)*/
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 100.0, true));
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 130.0);

    fakeMonotimeUs += 130000;
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 1.1, false)); /* replenish -12+13=1; 1 < 1.1 */
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 1.0, false));

    fakeMonotimeUs += 1000000;                              /* advance 1 second, refill bucket */
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 1.0, true)); /* Force consume 1, 11 should be left*/
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 12.0), 10.0);
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 11.0), 0.0);
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 11.0, true));
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 1.0, false));
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 10.0);
}

TEST_F(TokenBucketTest, SetRateChangesRate) {
    tokenBucket_setRate(bucket, 200.0);
    EXPECT_DOUBLE_EQ(tokenBucket_getRate(bucket), 200.0);

    fakeMonotimeUs += 1000000;                                /* replenish caps at the new 22 */
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 22.0, false)); /* larger capacity is reachable */
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 1.0, false)); /* now empty */

    fakeMonotimeUs += 1000000;
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 1.0, false)); /* replenish to 22, 21 left */
    tokenBucket_setRate(bucket, 10.0);
    EXPECT_DOUBLE_EQ(tokenBucket_getRate(bucket), 10.0);
    /* only 3 remain after trim */
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 21.0, false));
    EXPECT_TRUE(tokenBucket_tryConsume(bucket, 3.0, false));
    EXPECT_FALSE(tokenBucket_tryConsume(bucket, 1.0, false)); /* now empty */
    EXPECT_DOUBLE_EQ(tokenBucket_msUntilAvailable(bucket, 1.0), 100.0);
}

TEST_F(TokenBucketTest, SetRateSettlesElapsedAtOldRate) {
    /* setRate must credit elapsed time at the OLD rate before switching. */
    tokenBucket *b = tokenBucket_create(1.0, 10.0);
    EXPECT_TRUE(tokenBucket_tryConsume(b, 12.0, false)); /* drain to empty */
    EXPECT_FALSE(tokenBucket_tryConsume(b, 0.5, false));

    fakeMonotimeUs += 5000000; /* 5 tokens should accrue */

    tokenBucket_setRate(b, 1000.0); /* the 5 elapsed seconds belong to the OLD rate */

    EXPECT_FALSE(tokenBucket_tryConsume(b, 6.0, false)); /* only 5 available, not thousands */
    EXPECT_TRUE(tokenBucket_tryConsume(b, 5.0, false));

    /* Confirm the NEW rate now governs accrual: at 1000/s, 10ms yields ~10 tokens */
    fakeMonotimeUs += 10000;
    EXPECT_TRUE(tokenBucket_tryConsume(b, 10.0, false));
    EXPECT_FALSE(tokenBucket_tryConsume(b, 0.1, false)); /* now empty */

    tokenBucket_free(b);
}
