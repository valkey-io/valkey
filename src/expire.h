#ifndef EXPIRE_H
#define EXPIRE_H

#include <stdbool.h>
#include "util.h"

/* Special Expiry values */
#define EXPIRY_NONE -1

/* Flags for expireIfNeeded */
#define EXPIRE_FORCE_DELETE_EXPIRED 1
#define EXPIRE_AVOID_DELETE_EXPIRED 2

#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1

/* Command flags for items expiration update conditions */
#define EXPIRE_NX (1 << 0)
#define EXPIRE_XX (1 << 1)
#define EXPIRE_GT (1 << 2)
#define EXPIRE_LT (1 << 3)

/* Return values for expireIfNeeded */
typedef enum {
    KEY_VALID = 0, /* Could be volatile and not yet expired, non-volatile, or even nonexistent key. */
    KEY_EXPIRED,   /* Logically expired but not yet deleted. */
    KEY_DELETED    /* The key was deleted now. */
} keyStatus;

/* Return value for getExpirationPolicy */
typedef enum {
    POLICY_IGNORE_EXPIRE, /* Ignore expiration time of items and treat them as valid. */
    POLICY_KEEP_EXPIRED,  /* Ignore items which are expired but do not actively delete them. */
    POLICY_DELETE_EXPIRED /* Delete expired keys on access. */
} expirationPolicy;

/* Types of active expiry jobs. Used to track and orchestrate
 * separate expiry mechanisms within the same database.
 *
 * KEYS:   Expiry of top-level keys via db->expires.
 * FIELDS: Expiry of hash fields stored in volatile sets (e.g., per-field TTLs).
 *
 * ACTIVE_EXPIRY_TYPE_COUNT: Number of expiry types, used for sizing arrays and iteration. */
enum activeExpiryType {
    KEYS,
    FIELDS,
    ACTIVE_EXPIRY_TYPE_COUNT
};

/* Forward declarations */
typedef struct client client;
typedef struct serverObject robj;
typedef struct serverDb serverDb;

/* return the relevant expiration policy based on the current server state and the provided flags.
 * FLAGS can indicate either:
 * EXPIRE_AVOID_DELETE_EXPIRED - which indicate the command is explicitly executed with the NO_EXPIRE flag.
 * EXPIRE_FORCE_DELETE_EXPIRED - which indicate to delete expired keys even in case of a replica (for the writable replicas case) */
expirationPolicy getExpirationPolicyWithFlags(int flags);
int parseExtendedExpireArgumentsOrReply(client *c, int *flags, int max_args);
int convertExpireArgumentToUnixTime(client *c, robj *arg, long long basetime, int unit, long long *unixtime);

/* Handling of expired keys and hash fields */
void activeExpireCycle(int type);
void expireReplicaKeys(void);
void rememberReplicaKeyWithExpire(serverDb *db, robj *key);
void flushReplicaKeysWithExpireList(void);
size_t getReplicaKeyWithExpireCount(void);
bool timestampIsExpired(mstime_t when);

#endif
