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
    bucketSizeFunc *bucket_size_func;
};

static double calcBucketSize(double tokens_per_sec, double max_burst_time_secs) {
    return tokens_per_sec * max_burst_time_secs;
}

static double trimTokenBucket(tokenBucket *bucket) {
    double unused_tokens = 0;
    double bucket_size = bucket->bucket_size_func(bucket->tokens_per_sec, bucket->max_burst_time_secs);
    if (bucket->token_count > bucket_size) {
        unused_tokens = bucket->token_count - bucket_size;
        bucket->token_count = bucket_size;
    }
    return unused_tokens;
}

tokenBucket *tokenBucket_create(double tokens_per_sec, double max_burst_time_secs, bucketSizeFunc *bucket_size_func) {
    serverAssert(tokens_per_sec >= 0);
    serverAssert(max_burst_time_secs >= 0);
    tokenBucket *bucket = zmalloc(sizeof(tokenBucket));
    bucket->tokens_per_sec = tokens_per_sec;
    bucket->max_burst_time_secs = max_burst_time_secs;
    bucket->bucket_size_func = bucket_size_func == NULL ? calcBucketSize : bucket_size_func;
    bucket->token_count = bucket->bucket_size_func(bucket->tokens_per_sec, bucket->max_burst_time_secs);
    bucket->last_time_check = getMonotonicUs();
    return bucket;
}

void tokenBucket_free(tokenBucket *bucket) {
    zfree(bucket);
}

double tokenBucket_getTokenCount(tokenBucket *bucket) {
    return bucket->token_count;
}

double tokenBucket_getTokensPerSec(tokenBucket *bucket) {
    return bucket->tokens_per_sec;
}

double tokenBucket_getBucketSize(tokenBucket *bucket) {
    return bucket->bucket_size_func(bucket->tokens_per_sec, bucket->max_burst_time_secs);
}

double tokenBucket_getMaxBurstTime(tokenBucket *bucket) {
    return bucket->max_burst_time_secs;
}

void tokenBucket_setTokensPerSec(tokenBucket *bucket, double tokens_per_sec) {
    serverAssert(tokens_per_sec >= 0);
    bucket->tokens_per_sec = tokens_per_sec;
    trimTokenBucket(bucket);
}

void tokenBucket_setMaxBurstSec(tokenBucket *bucket, double max_burst_time_secs) {
    serverAssert(max_burst_time_secs >= 0);
    bucket->max_burst_time_secs = max_burst_time_secs;
    trimTokenBucket(bucket);
}

void tokenBucket_capDebt(tokenBucket *bucket, double max_debt) {
    serverAssert(max_debt >= 0);
    if (bucket->token_count < -max_debt) {
        bucket->token_count = -max_debt;
    }
}

double tokenBucket_add(tokenBucket *bucket, double tokens) {
    bucket->token_count += tokens;
    return trimTokenBucket(bucket);
}

double tokenBucket_replenish(tokenBucket *bucket) {
    monotime now = getMonotonicUs();
    uint64_t delta_us = now - bucket->last_time_check;
    bucket->last_time_check = now;
    return tokenBucket_add(bucket, delta_us * bucket->tokens_per_sec / 1000000.0);
}

void tokenBucket_consume(tokenBucket *bucket, double tokens) {
    bucket->token_count -= tokens;
}

bool tokenBucket_canConsume(tokenBucket *bucket, double tokens) {
    return bucket->token_count >= tokens;
}

double tokenBucket_msUntilAvailable(tokenBucket *bucket, double target_tokens) {
    if (bucket->token_count >= target_tokens) return 0.0;
    if (bucket->tokens_per_sec <= 0) return -1.0; /* halted — never available */
    double needed = target_tokens - bucket->token_count;
    return needed / bucket->tokens_per_sec * 1000.0;
}

void tokenBucket_halt(tokenBucket *bucket) {
    bucket->token_count = 0;
    bucket->tokens_per_sec = 0;
}
