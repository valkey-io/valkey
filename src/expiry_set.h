#ifndef __EXPIRY_SET_H__
#define __EXPIRY_SET_H__

#include "dict.h"
#include "adlist.h"
#include "server.h"

/* ExpirySet is a general-purpose container providing O(1) insertion, removal,
 * and expiration of pointer keys by timestamp. It maintains:
 *  - A hash table mapping keys to entries (for O(1) lookup/remove).
 *  - A doubly-linked list of entries ordered by expiry time (for O(1)
 *    append and expire-from-head).
 *
 * Common use cases:
 *  - Cluster failure reporting: track per-node failure votes with automatic
 *    eviction once they age out.
 *  - TTL-based caches: insert items with individual timeouts and expire
 *    stale entries efficiently.
 *  - Session or lease management: manage session expiry or resource leases
 *    with minimal overhead.
 *  - Any scenario where you need to keep a set of pointers, support
 *    fast add/remove, and periodically purge expired entries.
 */

/* Internal entry struct */
typedef struct ExpirySetEntry {
    void *key;       /* Key */
    mstime_t expiry; /* Expiry time */
    listNode *ln;    /* Back pointer into expiry_list */
} ExpirySetEntry;

/* The ExpirySet container */
typedef struct ExpirySet {
    dict *dict;        /* Hash of key -> ExpirySetEntry* */
    list *expiry_list; /* Entries in ascending expiry order */
} ExpirySet;

ExpirySet *expirySetCreate(dictType *dt);
void expirySetRemoveEntry(ExpirySet *es, ExpirySetEntry *e);
void expirySetFree(ExpirySet *es);
int expirySetAdd(ExpirySet *es, void *key, mstime_t expiry);
int expirySetRemove(ExpirySet *es, void *key);
int expirySetExpire(ExpirySet *es);
int expirySetCount(ExpirySet *es);

#endif /* EXPIRY_SET_H */
