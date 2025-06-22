#ifndef EXPIRE_H
#define EXPIRE_H

#include <time.h>
#include "monotonic.h"

#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1

/* Command flags for items expiration update conditions */
#define EXPIRE_NX (1 << 0)
#define EXPIRE_XX (1 << 1)
#define EXPIRE_GT (1 << 2)
#define EXPIRE_LT (1 << 3)

/* Return values for expireIfNeeded */
typedef enum {
    KEY_VALID = 0, /* Could be volatile and not yet expired, non-volatile, or even non-existing key. */
    KEY_EXPIRED,   /* Logically expired but not yet deleted. */
    KEY_DELETED    /* The key was deleted now. */
} keyStatus;

/* Return value for getExpirationPolicy */
typedef enum { 
    POLICY_IGNORE_EXPIRE, /* Ignore expiration time of items and treat them as valid. */
    POLICY_KEEP_EXPIRED,  /* Ignore items which are expired but do not actively delete them. */
    POLICY_DELETE_EXPIRED /* Delete expired keys on access. */
} expirationPolicy;

/* Forward declarations */
typedef struct client client;
typedef struct serverObject robj;

int timestampIsExpired(mstime_t when);
expirationPolicy getExpirationPolicyWithFlags(int flags);
int parseExtendedExpireArgumentsOrReply(client *c, int *flags, int max_args);
int convertExpireArgumentToUnixTime(client *c, robj *arg, long long basetime, int unit, long long *unixtime);

#endif
