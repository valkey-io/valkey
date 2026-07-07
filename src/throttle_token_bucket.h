/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef THROTTLE_TOKEN_BUCKET_H
#define THROTTLE_TOKEN_BUCKET_H

#include <stdbool.h>

typedef struct tokenBucket tokenBucket;

tokenBucket *tokenBucket_create(double tokens_per_sec, double max_burst_time_secs);
void tokenBucket_free(tokenBucket *bucket);

double tokenBucket_getRate(tokenBucket *bucket);
void tokenBucket_setRate(tokenBucket *bucket, double new_rate);

bool tokenBucket_tryConsume(tokenBucket *bucket, double tokens, bool force_consume);
double tokenBucket_msUntilAvailable(tokenBucket *bucket, double tokens);

#endif
