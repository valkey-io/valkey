/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

/* ExpirySet
 * =========
 *
 * ExpirySet is a general-purpose container providing O(1) insertion, removal,
 * and O(E) expiration by timestamp where E is the number of expired items.
 * It maintains:
 *  - A hash table mapping each key directly to its list node for constant-time lookup and removal.
 *  - A doubly‐linked list of ExpirySetItem structs, always keep in ascending order
 *        of expiry so that all expired entries can be purged by scanning
 *        from the head.
 *
 * Common use cases:
 *  - Cluster failure reporting: track per-node failure votes with automatic
 *    eviction once they age out.
 *  - TTL-based caches: insert items with individual timeouts and expire
 *    stale entries efficiently.
 *  - Session or lease management: manage session expiry or resource leases
 *    with minimal overhead.
 *  - Any scenario where you need to keep a set of keys, support
 *    fast add/remove, and periodically purge expired entries.
 *
 *
 * Convention
 * -----------
 *
 * Functions and types are prefixed by "expirySet", macros by "EXPIRY_SET".
 * Internal names don't use the prefix. Internal functions are 'static'.
 */

#include "expiry_set.h"
#include "server.h"
#include "zmalloc.h"

/* Remove a single expiry set from the ExpirySet.
 *
 * This unlinks the item from the expiry_list and deletes its dict mapping,
 * then frees the object. Internal use only. */
static inline void removeItem(ExpirySet *es, ExpirySetItem *es_item) {
    listNode *ln = dictFetchValue(es->dict, es_item->key);
    listDelNode(es->expiry_list, ln);
    dictDelete(es->dict, es_item->key);
    zfree(es_item);
}

/* Allocate and initialize a new ExpirySet. */
ExpirySet *expirySetCreate(dictType *dt) {
    ExpirySet *es = zmalloc(sizeof(*es));
    if (!es) return NULL;

    es->dict = dictCreate(dt);
    if (!es->dict) {
        zfree(es);
        return NULL;
    }

    es->expiry_list = listCreate();
    if (!es->expiry_list) {
        dictRelease(es->dict);
        zfree(es);
        return NULL;
    }

    return es;
}

/* Destroy an ExpirySet and free all resources. */
void expirySetFree(ExpirySet *es) {
    /* Remove all entries */
    listNode *ln;
    while ((ln = listFirst(es->expiry_list))) {
        ExpirySetItem *es_item = ln->value;
        removeItem(es, es_item);
    }
    dictRelease(es->dict);
    listRelease(es->expiry_list);
    zfree(es);
}

/* Add a new key with its expiry timestamp, or refresh an existing one.
 * Return 1 if it is new key, 0 otherwise */
int expirySetAdd(ExpirySet *es, void *key, mstime_t expiry) {
    listNode *ln = dictFetchValue(es->dict, key);
    ExpirySetItem *es_item;
    int is_new = 0;
    if (ln) {
        /* refresh expiry */
        es_item = ln->value;
        listDelNode(es->expiry_list, ln);
        es_item->expiry = expiry;
    } else {
        /* new key */
        es_item = zmalloc(sizeof(*es_item));
        es_item->key = key;
        es_item->expiry = expiry;
        is_new = 1;
    }

    /* Find the insertion point by scanning backwards from the tail */
    listNode *prev_node = listLast(es->expiry_list);
    while (prev_node) {
        ExpirySetItem *prev_item = listNodeValue(prev_node);
        if (prev_item->expiry <= expiry) break;
        prev_node = prev_node->prev;
    }

    listNode *new_ln;
    if (prev_node) {
        /* Insert after prev_node so everything before has expiry ≤ new expiry */
        listInsertNode(es->expiry_list, prev_node, es_item, 1);
        new_ln = prev_node->next;
    } else {
        /* No existing es_item ≤ expiry. This becomes the new head */
        listAddNodeHead(es->expiry_list, es_item);
        new_ln = listFirst(es->expiry_list);
    }

    /* add/update dict mapping to point at our new node */
    dictReplace(es->dict, key, new_ln);

    return is_new;
}

/* Remove a key and its key from the ExpirySet.
 * Return 1 if a key is deleted, 0 if no matching key is found */
int expirySetRemove(ExpirySet *es, void *key) {
    listNode *ln = dictFetchValue(es->dict, key);
    if (!ln) return 0;
    removeItem(es, ln->value);
    return 1;
}

/* Expire all entries whose timestamp ≤ now.
 * Return the number of removed entries */
int expirySetExpire(ExpirySet *es) {
    int removed = 0;
    listNode *ln;
    mstime_t now = mstime();
    /* expire from head while expiry ≤ now */
    while ((ln = listFirst(es->expiry_list))) {
        ExpirySetItem *es_item = ln->value;
        if (es_item->expiry > now) break;
        removeItem(es, es_item);
        removed++;
    }
    return removed;
}

/* Expire old entries and return the count of remaining live entries. */
int expirySetCount(ExpirySet *es) {
    expirySetExpire(es);
    return es->expiry_list->len;
}

/* Check whether the key exists in the set.
 * If the expiry ≤ current time, it is removed and treated as missing.
 * Return 1 if key exists, 0 otherwise */
int expirySetExists(ExpirySet *es, void *key) {
    listNode *ln = dictFetchValue(es->dict, key);
    if (!ln) return 0;
    ExpirySetItem *es_item = ln->value;
    if (es_item->expiry <= mstime()) {
        expirySetRemove(es, key);
        return 0;
    }
    return 1;
}

/* Retrieve the expiry timestamp for the key.
 * If the expiry ≤ current time, it is removed and treated as missing.
 * Returns 1 if key exists, 0 otherwise */
int expirySetGetExpiry(ExpirySet *es, void *key, mstime_t *out_expiry) {
    listNode *ln = dictFetchValue(es->dict, key);
    if (!ln) return 0;
    ExpirySetItem *es_item = ln->value;
    if (es_item->expiry <= mstime()) {
        expirySetRemove(es, key);
        return 0;
    }
    if (out_expiry) *out_expiry = es_item->expiry;
    return 1;
}
