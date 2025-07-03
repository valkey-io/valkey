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
    if (es == NULL || es_entry == NULL) return;
    listDelNode(es->expiry_list, es_entry->ln);
    dictDelete(es->dict, es_entry->key);
    zfree(es_entry);
}

/* Allocate and initialize a new ExpirySet. */
ExpirySet *expirySetCreate(dictType *dt) {
    assert(dt != NULL);
    ExpirySet *es = zmalloc(sizeof(*es));
    if (!es) return NULL;
    es->dict = dictCreate(dt);
    es->expiry_list = listCreate();
    return es;
}

/* Destroy an ExpirySet and free all resources. */
void expirySetFree(ExpirySet *es) {
    if (!es) return;
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

/* Add a new key with its expiry timestamp, or refresh an existing one. */
int expirySetAdd(ExpirySet *es, void *key, mstime_t expiry) {
    assert(es && key);
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
    /* append at tail (most recently used/updated) */
    listAddNodeTail(es->expiry_list, es_entry);
    es_entry->ln = es->expiry_list->tail;
    return is_new;
}

/* Remove a key (and its entry) from the ExpirySet, if present. */
int expirySetRemove(ExpirySet *es, void *key) {
    assert(es && key);
    ExpirySetEntry *es_entry = dictFetchValue(es->dict, key);
    if (!es_entry) return 0;
    expirySetRemoveEntry(es, es_entry);
    return 1;
}

/* Expire all entries whose timestamp ≤ now. */
int expirySetExpire(ExpirySet *es) {
    assert(es);
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
    assert(es);
    expirySetExpire(es);
    return es->expiry_list->len;
}
