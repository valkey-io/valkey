#include <string.h>
#include "volatile_set.h"
#include "zmalloc.h"
#include "config.h"
#include "endianconv.h"
#include "serverassert.h"

#define EXPIRY_HASH_SIZE 16
volatile_set *createVolatileSet(volatileEntryType *type) {
    volatile_set *set = zmalloc(sizeof(volatile_set));
    set->etypr = type;
    set->expiry_buckets = raxNew();
    return set;
}

void freeVolatileSet(volatile_set *b) {
    raxFree(b->expiry_buckets);
    zfree(b);
}

int volatileSetAddEntry(volatile_set *set, void *entry, long long expiry) {
    unsigned char buf[EXPIRY_HASH_SIZE];
    expiry = htonu64(expiry);
    memcpy(buf, &expiry, sizeof(expiry));
    memcpy(buf + 8, &entry, sizeof(entry));
    if (sizeof(entry) == 4) memset(buf + 12, 0, 4); /* Zero padding for 32bit target. */
    return raxTryInsert(set->expiry_buckets, buf, sizeof(buf), NULL, NULL);
}

int volatileSetRemoveEntry(volatile_set *set, void *entry, long long expiry) {
    unsigned char buf[EXPIRY_HASH_SIZE];
    expiry = htonu64(expiry);
    memcpy(buf, &expiry, sizeof(expiry));
    memcpy(buf + 8, &entry, sizeof(entry));
    if (sizeof(entry) == 4) memset(buf + 12, 0, 4); /* Zero padding for 32bit target. */
    return raxRemove(set->expiry_buckets, buf, sizeof(buf), NULL);
}

int volatileSetUpdateEntry(volatile_set *set, void *old_entry, void *new_entry, long long old_expiry, long long new_expiry) {
    if (old_entry == new_entry && old_expiry == new_expiry) return 1;

    if (old_entry && old_expiry != -1) {
        assert(volatileSetRemoveEntry(set, old_entry, old_expiry));
    }
    if (new_entry && new_expiry != -1) {
        assert(volatileSetAddEntry(set, new_entry, new_expiry));
    }
    return 1;
}

int volatileSetExpireEntry(volatile_set *set, void *entry) {
    volatileSetRemoveEntry(set, entry, set->etypr->getExpiry(entry));
    if (set->etypr->expire) {
        set->etypr->expire(entry);
        return 1;
    }
    return 0;
}

size_t volatileSetNumEntries(volatile_set *set) {
    assert(set && set->expiry_buckets);
    return set->expiry_buckets->numele;
}

void volatileSetStart(volatile_set *set, volatileSetIterator *it) {
    raxStart(&it->bucket, set->expiry_buckets);
}

int volatileSetNext(volatileSetIterator *it, void **entryptr) {
    if (raxNext(&it->bucket)) {
        assert(it->bucket.key_len == EXPIRY_HASH_SIZE);
        memcpy(entryptr, it->bucket.key + sizeof(long long), sizeof(*entryptr));
        return 1;
    }
    return 0;
}
void volatileSetReset(volatileSetIterator *it) {
    raxStop(&it->bucket);
}
