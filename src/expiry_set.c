#include "expiry_set.h"
#include "server.h"
#include "zmalloc.h"
#include <assert.h>

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
 *  - Any scenario where you need to keep a set of keys, support
 *    fast add/remove, and periodically purge expired entries.
 */

/* Remove a single entry from the ExpirySet.
 *
 * This unlinks the entry from the expiry_list and deletes its dict mapping,
 * then frees the entry object. */
void expirySetRemoveEntry(ExpirySet *es, ExpirySetEntry *es_entry) {
    listDelNode(es->expiry_list, es_entry->ln);
    dictDelete(es->dict, es_entry->key);
    zfree(es_entry);
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
        ExpirySetEntry *es_entry = ln->value;
        expirySetRemoveEntry(es, es_entry);
    }
    dictRelease(es->dict);
    listRelease(es->expiry_list);
    zfree(es);
}

/* Add a new key with its expiry timestamp, or refresh an existing one.
 * Return 1 if it is new entry, 0 otherwise */
int expirySetAdd(ExpirySet *es, void *key, mstime_t expiry) {
    ExpirySetEntry *es_entry = dictFetchValue(es->dict, key);
    int is_new = 0;
    if (es_entry) {
        /* refresh expiry */
        listDelNode(es->expiry_list, es_entry->ln);
        es_entry->expiry = expiry;
    } else {
        /* new entry */
        es_entry = zmalloc(sizeof(*es_entry));
        es_entry->key = key;
        es_entry->expiry = expiry;
        dictAdd(es->dict, key, es_entry);
        is_new = 1;
    }

    /* Find the insertion point by scanning backwards from the tail */
    listNode *prev_node = listLast(es->expiry_list);
    while (prev_node) {
        ExpirySetEntry *prev_entry = listNodeValue(prev_node);
        if (prev_entry->expiry <= expiry) break;
        prev_node = prev_node->prev;
    }

    if (prev_node) {
        /* Insert after prev_node so everything before has expiry ≤ new expiry */
        listInsertNode(es->expiry_list, prev_node, es_entry, 1);
        es_entry->ln = prev_node->next;
    } else {
        /* No existing entry is ≤ expiry. This becomes the new head */
        listAddNodeHead(es->expiry_list, es_entry);
        es_entry->ln = listFirst(es->expiry_list);
    }

    return is_new;
}

/* Remove a key and its entry from the ExpirySet.
 * Return 1 if an entry is deleted, 0 if no matching entry is found */
int expirySetRemove(ExpirySet *es, void *key) {
    ExpirySetEntry *es_entry = dictFetchValue(es->dict, key);
    if (!es_entry) return 0;
    expirySetRemoveEntry(es, es_entry);
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
        ExpirySetEntry *es_entry = ln->value;
        if (es_entry->expiry > now) break;
        expirySetRemoveEntry(es, es_entry);
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
    ExpirySetEntry *e = dictFetchValue(es->dict, key);
    if (!e) return 0;
    if (e->expiry <= mstime()) {
        expirySetRemove(es, key);
        return 0;
    }
    return 1;
}

/* Retrieve the expiry timestamp for the key.
 * If the expiry ≤ current time, it is removed and treated as missing.
 * Returns 1 if key exists, 0 otherwise */
int expirySetGetExpiry(ExpirySet *es, void *key, mstime_t *out_expiry) {
    ExpirySetEntry *e = dictFetchValue(es->dict, key);
    if (!e) return 0;
    if (e->expiry <= mstime()) {
        expirySetRemove(es, key);
        return 0;
    }
    if (out_expiry) *out_expiry = e->expiry;
    return 1;
}
