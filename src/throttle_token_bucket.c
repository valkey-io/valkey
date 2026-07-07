/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "throttle_token_bucket.h"
#include "server.h"
#include "monotonic.h"

struct tokenBucket {
    double tokens_per_sec;
    double max_burst_time_secs;
    double token_count;
    monotime last_time_check;
};

#define BUCKET_EPSILON 0.0001

static double getBucketSize(tokenBucket *bucket) {
    return (bucket->tokens_per_sec < BUCKET_EPSILON) ? 0.0
        : 2.0 + bucket->tokens_per_sec * bucket->max_burst_time_secs;
}

static void trimTokenBucket(tokenBucket *bucket) {
    double bucket_size = getBucketSize(bucket);
    if (bucket->token_count > bucket_size) bucket->token_count = bucket_size;
    if (bucket->token_count < -bucket_size) bucket->token_count = -bucket_size;
}

static void tokenBucket_replenish(tokenBucket *bucket) {
    monotime now = getMonotonicUs();
    uint64_t delta_us = now - bucket->last_time_check;
    double tokens_to_add = delta_us * bucket->tokens_per_sec / 1000000.0;
    if (tokens_to_add > 0) {
        bucket->token_count += tokens_to_add;
        trimTokenBucket(bucket);
    }
    bucket->last_time_check = now;
}

tokenBucket *tokenBucket_create(double tokens_per_sec, double max_burst_time_secs) {
    serverAssert(tokens_per_sec >= 0);
    serverAssert(max_burst_time_secs >= 0);
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
    serverAssert(new_rate >= 0);
    bucket->tokens_per_sec = new_rate;
    trimTokenBucket(bucket);
}

bool tokenBucket_tryConsume(tokenBucket *bucket, double tokens, bool force_consume) {
    tokenBucket_replenish(bucket);
    if (!force_consume && bucket->token_count < tokens) return false;
    bucket->token_count -= tokens;
    return true;
}

double tokenBucket_msUntilAvailable(tokenBucket *bucket, double target_tokens) {
    if (bucket->token_count >= target_tokens) return 0.0;
    if (bucket->tokens_per_sec <= 0) return -1.0; /* halted — never available */
    double needed = target_tokens - bucket->token_count;
    return needed / bucket->tokens_per_sec * 1000.0;
}

