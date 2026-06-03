#ifndef THROTTLE_TOKEN_BUCKET_H
#define THROTTLE_TOKEN_BUCKET_H

#include <stdbool.h>

typedef double bucketSizeFunc(double tokens_per_sec, double max_burst_time_secs);
typedef struct tokenBucket tokenBucket;

// APIs
tokenBucket *tokenBucket_create(double tokens_per_sec, double max_burst_time_secs, bucketSizeFunc *bucket_size_func);
void tokenBucket_free(tokenBucket *bucket);

double tokenBucket_getTokenCount(tokenBucket *bucket);
double tokenBucket_getTokensPerSec(tokenBucket *bucket);
double tokenBucket_getBucketSize(tokenBucket *bucket);
double tokenBucket_getMaxBurstTime(tokenBucket *bucket);
void tokenBucket_setTokensPerSec(tokenBucket *bucket, double tokens_per_sec);
void tokenBucket_setMaxBurstSec(tokenBucket *bucket, double max_burst_time_secs);

void tokenBucket_capDebt(tokenBucket *bucket, double max_debt);
double tokenBucket_add(tokenBucket *bucket, double tokens);
double tokenBucket_replenish(tokenBucket *bucket);
bool tokenBucket_canConsume(tokenBucket *bucket, double tokens);
void tokenBucket_consume(tokenBucket *bucket, double tokens);
double tokenBucket_msUntilAvailable(tokenBucket *bucket, double tokens);
void tokenBucket_halt(tokenBucket *bucket);

#endif
