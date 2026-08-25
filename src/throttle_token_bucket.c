/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "throttle_token_bucket.h"
#include "monotonic.h"
#include "zmalloc.h"

struct tokenBucket {
    double tokens_per_sec;      // Rate at which tokens are added to the bucket (tokens per second)
    double max_burst_time_secs; // Maximum time for which tokens can accumulate in the bucket
    double token_count;         // Current number of tokens in the bucket (can be negative if force-consumed)
    monotime last_time_check;   // Last time the bucket was replenished (in microseconds)
};

#define BUCKET_EPSILON 0.0001

/* Bucket capacity scales with rate: higher rates allow larger bursts.
 * The +2 guarantees the bucket can always hold at least 2 tokens, preventing
 * permanent starvation at very low rates where rate * burst_time < 1.
 * Returns 0 when rate is effectively zero. */
static double getBucketSize(tokenBucket *bucket) {
    return (bucket->tokens_per_sec < BUCKET_EPSILON) ? 0.0
                                                     : 2.0 + bucket->tokens_per_sec * bucket->max_burst_time_secs;
}

/* Clamp token count to valid range [-bucket_size, bucket_size]. */
static void trimTokenBucket(tokenBucket *bucket) {
    double bucket_size = getBucketSize(bucket);
    if (bucket->token_count > bucket_size) bucket->token_count = bucket_size;
    if (bucket->token_count < -bucket_size) bucket->token_count = -bucket_size;
}

static void replenishTokenBucket(tokenBucket *bucket) {
    monotime now = getMonotonicUs();
    uint64_t delta_us = now - bucket->last_time_check;
    double tokens_to_add = delta_us * bucket->tokens_per_sec / 1000000.0;
    bucket->token_count += tokens_to_add;
    trimTokenBucket(bucket);
    bucket->last_time_check = now;
}

tokenBucket *tokenBucket_create(double tokens_per_sec, double max_burst_time_secs) {
    tokenBucket *bucket = zmalloc(sizeof(tokenBucket));
    bucket->tokens_per_sec = tokens_per_sec;
    bucket->max_burst_time_secs = max_burst_time_secs;
    bucket->token_count = getBucketSize(bucket);
    bucket->last_time_check = getMonotonicUs();
    return bucket;
}

void tokenBucket_free(tokenBucket *bucket) {
    zfree(bucket);
}

double tokenBucket_getRate(tokenBucket *bucket) {
    return bucket->tokens_per_sec;
}

void tokenBucket_setRate(tokenBucket *bucket, double new_rate) {
    replenishTokenBucket(bucket);
    bucket->tokens_per_sec = new_rate;
    trimTokenBucket(bucket);
}

bool tokenBucket_tryConsume(tokenBucket *bucket, double tokens, bool force_consume) {
    replenishTokenBucket(bucket);
    if (!force_consume && bucket->token_count < tokens) return false;
    bucket->token_count -= tokens;
    trimTokenBucket(bucket); /* bound debt at -bucket_size so recovery time stays bounded */
    return true;
}

double tokenBucket_msUntilAvailable(tokenBucket *bucket, double target_tokens) {
    replenishTokenBucket(bucket);
    if (bucket->token_count >= target_tokens) return 0.0;
    /* Rates below BUCKET_EPSILON give zero capacity, so tokens never accumulate. */
    if (bucket->tokens_per_sec < BUCKET_EPSILON) return -1.0; /* halted -- never available */
    double needed = target_tokens - bucket->token_count;
    return needed * 1000.0 / bucket->tokens_per_sec;
}
