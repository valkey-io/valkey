#ifndef EXPIRY_SET_H
#define EXPIRY_SET_H

/* ExpirySet implementation.
 *
 * This is an efficient container for managing keys with individual expiry times.
 * For details about the implementation and documentation of functions,
 * see comments in expiry_set.c.
 *
 * Terminology:
 *
 *   ExpirySet
 *     - dict
 *          A hash table mapping key -> listNode* (O(1) lookups, inserts, and removals).
 *     - expiry_list
 *          A doubly‐linked list of ExpirySetItem structs, always keep in ascending order
 *          of expiry so that all expired entries can be purged in O(E) by scanning
 *          from the head. (Where E is the number of expired items)
 *
 *   ExpirySetItem
 *     - key:    The pointer used to identify the entry.
 *     - expiry: The absolute timestamp (in ms) when the entry should expire.
 */

#include "dict.h"
#include "adlist.h"
#include "server.h"

/* Internal struct for expiry_list in ExpirySet */
typedef struct ExpirySetItem {
    void *key;       /* Key */
    mstime_t expiry; /* Expiry time */
} ExpirySetItem;

/* ExpirySet container */
typedef struct ExpirySet {
    dict *dict;        /* Hash of key -> listNode* */
    list *expiry_list; /* ExpirySetItems in ascending expiry order */
} ExpirySet;

ExpirySet *expirySetCreate(dictType *dt);
void expirySetFree(ExpirySet *es);
int expirySetAdd(ExpirySet *es, void *key, mstime_t expiry);
int expirySetRemove(ExpirySet *es, void *key);
int expirySetExpire(ExpirySet *es);
int expirySetCount(ExpirySet *es);
int expirySetExists(ExpirySet *es, void *key);
int expirySetGetExpiry(ExpirySet *es, void *key, mstime_t *out_expiry);

#endif /* EXPIRY_SET_H */
