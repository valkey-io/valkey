/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Key memory delta tracking, backing ValkeyModuleEvent_KeyMemoryDelta.
 *
 * The first time a key is touched during an execution unit, its existence and
 * size (as reported by MEMORY USAGE) are recorded. When the outermost unit
 * completes, the key is looked up again and a single event describes the
 * transition, however many times the key was modified in between.
 *
 * A snapshot holds a copy of the key name and never a pointer to the value:
 * values may be reallocated (e.g. when an expire is embedded, or by active
 * defrag) or freed before the unit completes.
 *
 * Most units touch a single key: the first key is recorded in a static
 * snapshot whose key buffer is reused from one unit to the next, so that
 * tracking it allocates nothing. Further keys go to a hashtable. */

#include "key_memory_delta.h"
#include "module.h"

typedef struct keyMemoryDeltaSnapshot {
    serverDb *db;
    size_t old_bytes;
    size_t new_bytes;
    int old_exists;
    int new_exists;
    sds key; /* Points into this same allocation, except for 'first'. */
} keyMemoryDeltaSnapshot;

int key_memory_delta_listeners = 0;
size_t key_memory_delta_pending = 0;
static keyMemoryDeltaSnapshot first;        /* The first tracked key, if 'db' is set. */
static hashtable *pending = NULL;           /* The other tracked keys. */
static keyMemoryDeltaSnapshot *last = NULL; /* Most recently tracked snapshot. */

/* Snapshots are hashed by key name only, so that SWAPDB can move them to the
 * other database in place. */
static uint64_t snapshotHash(const void *key) {
    const keyMemoryDeltaSnapshot *s = key;
    return hashtableGenHashFunction(s->key, sdslen(s->key));
}

static int snapshotKeyCompare(const void *key1, const void *key2) {
    const keyMemoryDeltaSnapshot *s1 = key1, *s2 = key2;
    size_t len = sdslen(s1->key);
    return s1->db == s2->db && len == sdslen(s2->key) && memcmp(s1->key, s2->key, len) == 0;
}

static hashtableType snapshotHashtableType = {
    .hashFunction = snapshotHash,
    .keyCompare = snapshotKeyCompare,
};

static size_t sampleSize(serverDb *db, sds key, robj *val) {
    robj keyobj;
    initStaticStringObject(keyobj, key);
    return objectComputeSize(&keyobj, val, OBJ_COMPUTE_SIZE_DEF_SAMPLES, db->id);
}

static keyMemoryDeltaSnapshot *findSnapshot(serverDb *db, sds key) {
    if (!key_memory_delta_pending) return NULL;
    keyMemoryDeltaSnapshot probe = {.db = db, .key = key};
    void *found;
    if (last && snapshotKeyCompare(last, &probe)) return last;
    if (first.db && snapshotKeyCompare(&first, &probe)) return last = &first;
    if (!pending || !hashtableFind(pending, &probe, &found)) return NULL;
    return last = found;
}

void keyMemoryDeltaTrackSlow(serverDb *db, sds key, robj *val) {
    /* During an asynchronous load, only the dataset being loaded is reported:
     * consumers can't tell changes to the dataset it replaces apart. */
    if (server.async_loading && db == server.db[db->id]) return;
    if (findSnapshot(db, key)) return;

    keyMemoryDeltaSnapshot *s = &first;
    size_t len = sdslen(key);
    if (first.db == NULL) {
        first.key = first.key ? sdscpylen(first.key, key, len) : sdsnewlen(key, len);
    } else {
        char type = sdsReqType(len);
        size_t keysize = sdsReqSize(len, type);
        s = zmalloc(sizeof(*s) + keysize);
        s->key = sdswrite((char *)(s + 1), keysize, type, key, len);
    }
    s->db = db;
    s->old_exists = val != NULL;
    s->old_bytes = val ? sampleSize(db, key, val) : 0;
    if (s != &first) {
        if (pending == NULL) pending = hashtableCreate(&snapshotHashtableType);
        bool added = hashtableAdd(pending, s);
        serverAssert(added);
    }
    key_memory_delta_pending++;
    last = s;
}

/* Reads normally leave a value untouched, but a hashtable may rehash when it
 * is looked up, or shrink when a scan or random sampling resumes its paused
 * auto-shrinking, and iterating a quicklist compresses the nodes it leaves,
 * which writes may have left uncompressed. That changes its memory usage. */
static bool hashtableMayChangeOnRead(hashtable *ht) {
    return hashtableIsRehashing(ht) || hashtableNeedsShrink(ht);
}

static bool valueMayChangeOnRead(robj *val) {
    switch (objectGetType(val)) {
    case OBJ_SET:
    case OBJ_HASH:
        return objectGetEncoding(val) == OBJ_ENCODING_HASHTABLE && hashtableMayChangeOnRead(objectGetVal(val));
    case OBJ_ZSET:
        return objectGetEncoding(val) == OBJ_ENCODING_BTREE &&
               hashtableMayChangeOnRead(((zset *)objectGetVal(val))->ht);
    case OBJ_LIST:
        return objectGetEncoding(val) == OBJ_ENCODING_QUICKLIST && ((quicklist *)objectGetVal(val))->compress != 0;
    case OBJ_PATH_HASH:
        /* Its size is estimated from nested hashes that reads may resize. */
        return true;
    default: return false;
    }
}

/* Like valueMayChangeOnRead(), for code that resumes the paused rehashing of
 * a value's hashtables (active defrag, background iterations), which doesn't
 * apply to path hashes and lists. Defragmenting a hash with volatile fields
 * also scans the hashtables of its set of volatile fields, which may then
 * shrink. */
static bool valueMayResize(robj *val) {
    if (objectGetType(val) == OBJ_PATH_HASH || objectGetType(val) == OBJ_LIST) return false;
    if (objectGetType(val) == OBJ_HASH && objectGetEncoding(val) == OBJ_ENCODING_HASHTABLE &&
        hashTypeHasVolatileFields(val))
        return true;
    return valueMayChangeOnRead(val);
}

/* Write lookups are tracked, and so are read lookups done by write commands:
 * some of them (e.g. GETEX, HGETEX or XREADGROUP) look keys up for reading and
 * then modify them. */
void keyMemoryDeltaTrackLookupSlow(serverDb *db, robj *key, robj *val, int flags) {
    if (!(flags & LOOKUP_WRITE)) {
        client *c = server.executing_client;
        bool write_command = c && c->cmd && (c->cmd->flags & CMD_WRITE);
        if (!write_command && !valueMayChangeOnRead(val)) return;
    }
    keyMemoryDeltaTrackSlow(db, objectGetVal(key), val);
}

void keyMemoryDeltaTrackResizeSlow(int dbid, robj *val) {
    if (!valueMayResize(val)) return;
    /* A value released by a background iteration may have been deleted since,
     * or moved to another database by SWAPDB. */
    sds key = objectGetKey(val);
    serverDb *db = server.db[dbid];
    if (db == NULL || dbFind(db, key) != val) {
        db = NULL;
        for (int j = 0; j < server.dbnum && db == NULL; j++) {
            if (server.db[j] && dbFind(server.db[j], key) == val) db = server.db[j];
        }
        if (db == NULL) return;
    }
    keyMemoryDeltaTrackSlow(db, key, val);
}

/* Keys loaded from an RDB file are reported right away, from the value at
 * hand. A key replacing a duplicate one (see RDBFLAGS_ALLOW_DUP) was tracked
 * when the duplicate was deleted, and is reported by the next flush instead. */
void keyMemoryDeltaLoadedSlow(serverDb *db, sds key, robj *val) {
    if (findSnapshot(db, key)) return;
    size_t bytes = sampleSize(db, key, val);
    /* See keyMemoryDeltaFlush(). */
    enterExecutionUnit(0, 0);
    server.lazy_expire_disabled++;
    moduleNotifyKeyMemoryDelta(db->id, key, 0, bytes, 0, 1);
    server.lazy_expire_disabled--;
    exitExecutionUnit();
}

/* Deliver one event for every tracked key whose existence or size changed,
 * and forget about all tracked keys. */
void keyMemoryDeltaFlush(void) {
    if (!key_memory_delta_pending) return;

    /* Like firePostExecutionUnitJobs(), so that freeing the callbacks'
     * contexts doesn't deliver or propagate recursively. Callbacks must not
     * modify the keyspace, but reading a value may still resize it, which is
     * tracked again and reported by the next batch. The flag also keeps
     * callbacks from deleting expired keys they read, as for the Key event. */
    enterExecutionUnit(0, 0);
    server.lazy_expire_disabled++;
    while (key_memory_delta_pending) {
        keyMemoryDeltaSnapshot single;
        keyMemoryDeltaSnapshot *stack_items[16], **items = stack_items;
        size_t count = 0;
        bool has_single = first.db != NULL;
        if (key_memory_delta_pending > sizeof(stack_items) / sizeof(stack_items[0]))
            items = zmalloc(sizeof(*items) * key_memory_delta_pending);
        if (has_single) {
            single = first; /* Takes the key buffer over. */
            first.db = NULL;
            first.key = NULL;
            items[count++] = &single;
        }
        if (pending && hashtableSize(pending)) {
            hashtableIterator iter;
            void *entry;
            hashtableInitIterator(&iter, pending, 0);
            while (hashtableNext(&iter, &entry)) items[count++] = entry;
            hashtableCleanupIterator(&iter);
            hashtableEmpty(pending, NULL);
        }
        debugServerAssert(count == key_memory_delta_pending);
        key_memory_delta_pending = 0;
        last = NULL;

        if (key_memory_delta_listeners) {
            /* Sample every key before running any callback, so that a value
             * resized by a callback's reads is reported relative to the state
             * sampled here. */
            for (size_t i = 0; i < count; i++) {
                keyMemoryDeltaSnapshot *s = items[i];
                robj *val = dbFind(s->db, s->key);
                s->new_exists = val != NULL;
                s->new_bytes = val ? sampleSize(s->db, s->key, val) : 0;
            }
            for (size_t i = 0; i < count; i++) {
                keyMemoryDeltaSnapshot *s = items[i];
                if (s->old_exists == s->new_exists && s->old_bytes == s->new_bytes) continue;
                moduleNotifyKeyMemoryDelta(s->db->id, s->key, s->old_bytes, s->new_bytes, s->old_exists,
                                           s->new_exists);
            }
        }

        /* items[0] is 'single' if set, which isn't allocated. */
        for (size_t i = has_single; i < count; i++) zfree(items[i]);
        if (items != stack_items) zfree(items);
        if (has_single) {
            /* Keep the key buffer for the next unit, unless it grew large or
             * a callback's read tracked a key again meanwhile. */
            if (first.key == NULL && sdsalloc(single.key) <= 1024)
                first.key = single.key;
            else
                sdsfree(single.key);
        }
    }
    server.lazy_expire_disabled--;
    exitExecutionUnit();
}

/* Forget the tracked keys of a database that is being emptied or discarded as
 * a whole: modules learn about that through other events (e.g. FlushDB), so
 * the keys it contained must not be reported one by one. 'dbnum' is -1 for
 * all the databases of 'dbarray'. */
static bool isDiscarded(keyMemoryDeltaSnapshot *s, serverDb **dbarray, int dbnum) {
    return (dbnum == -1 || s->db->id == dbnum) && dbarray[s->db->id] == s->db;
}

void keyMemoryDeltaDiscard(serverDb **dbarray, int dbnum) {
    if (!key_memory_delta_pending) return;
    if (first.db && isDiscarded(&first, dbarray, dbnum)) {
        first.db = NULL;
        key_memory_delta_pending--;
        if (last == &first) last = NULL;
    }
    if (!pending || !hashtableSize(pending)) return;
    hashtableIterator iter;
    void *entry;
    hashtableInitIterator(&iter, pending, HASHTABLE_ITER_SAFE);
    while (hashtableNext(&iter, &entry)) {
        keyMemoryDeltaSnapshot *s = entry;
        if (!isDiscarded(s, dbarray, dbnum)) continue;
        hashtableDelete(pending, s);
        key_memory_delta_pending--;
        if (s == last) last = NULL;
        zfree(s);
    }
    hashtableCleanupIterator(&iter);
}

/* Called when the contents of two databases are swapped: tracked keys move to
 * the other database, so they are reported once, in the database where they
 * end up, after modules processed the SwapDB event. */
static void swapSnapshotDb(keyMemoryDeltaSnapshot *s, serverDb *db1, serverDb *db2) {
    if (s->db == db1)
        s->db = db2;
    else if (s->db == db2)
        s->db = db1;
}

void keyMemoryDeltaSwapDb(serverDb *db1, serverDb *db2) {
    if (!key_memory_delta_pending) return;
    if (first.db) swapSnapshotDb(&first, db1, db2);
    if (!pending || !hashtableSize(pending)) return;
    hashtableIterator iter;
    void *entry;
    hashtableInitIterator(&iter, pending, 0);
    while (hashtableNext(&iter, &entry)) swapSnapshotDb(entry, db1, db2);
    hashtableCleanupIterator(&iter);
}
