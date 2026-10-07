/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The Token Bucket Algorithm is a traffic control method where tokens are added to a bucket at a fixed rate (up to a
 * maximum capacity), and tokens can be requested from the bucket as needed (if tokens are available).
 *
 * Terminology:
 * Token: A permission unit required to perform some metered work; the caller will only perform the work if tokens are available.
 * Bucket: A logical storage that holds tokens until they are used.
 *
 * Working:
 * 1. Tokens are added to the bucket at a constant rate and stored up to the maximum capacity.
 * 2. When a caller needs to perform work, an attempt is made to get one or more tokens.
 * 3. If enough tokens are available, the required number of tokens is removed from the bucket, and the caller may proceed with the intended work.
 * 4. If tokens are unavailable, the caller must wait until sufficient tokens are available.
 */

#ifndef THROTTLE_TOKEN_BUCKET_H
#define THROTTLE_TOKEN_BUCKET_H

#include <stdbool.h>

typedef struct tokenBucket tokenBucket;

/* Create a token bucket that starts full.
 * max_burst_time_secs controls how many seconds of idle accumulation are
 * allowed before the bucket is considered full. A larger value permits
 * bigger bursts after idle periods. */
tokenBucket *tokenBucket_create(double tokens_per_sec, double max_burst_time_secs);

/* Free a token bucket and its resources. */
void tokenBucket_free(tokenBucket *bucket);

/* Return the current refill rate in tokens per second. */
double tokenBucket_getRate(tokenBucket *bucket);

/* Update the refill rate. Tokens are clamped to the new bucket capacity. */
void tokenBucket_setRate(tokenBucket *bucket, double new_rate);

/* Attempt to consume tokens. Returns true if tokens were deducted.
 * force_consume=false: only deducts if enough tokens are available.
 * force_consume=true: always deducts (may drive count negative). */
bool tokenBucket_tryConsume(tokenBucket *bucket, double tokens, bool force_consume);

/* Estimate milliseconds until the requested tokens become available.
 * Returns 0 if already available, or -1 if rate is 0 (never reached). */
double tokenBucket_msUntilAvailable(tokenBucket *bucket, double tokens);

#endif
