#include "server.h"
#include "zmalloc.h"
#include "script.h"
#include <assert.h>
#include <math.h>

/*================================= Internal Data Structures ================= */
typedef struct uncommittedKeyEntry {
    sds key;
    long long offset;
} uncommittedKeyEntry;

/* Pending key reference used during multi-command blocks (MULTI/EXEC, Lua).
 * We mark keys dirty immediately but don't yet know the final replication
 * offset, so we keep a reference to update the offset after the transaction
 * completes. */
typedef struct pendingUncommittedKey {
    robj *key;
    hashtable *uncommitted_keys;
} pendingUncommittedKey;

/* Pending keys buffered during MULTI/EXEC or Lua scripts.  These are keys
 * that have already been marked dirty in uncommitted_keys (with LLONG_MAX
 * as a placeholder offset) but whose real offset is not yet known. */
static list *pending_uncommitted_keys;

// Pending databases dirtied during a multi-command block.
static list *pending_uncommitted_dbs;

/* Keys modified by background/implicit writes (expiry, eviction) — i.e.
 * signalModifiedKey() with a NULL client, which have no argv to derive keys
 * from. Marked dirty here; drainBackgroundModifiedKeys() sets the real offset. */
static list *background_modified_keys;

/* Tracks whether all databases were dirtied during the current command
 * within a multi-command block (MULTI/EXEC or Lua script). Module-local
 * state — only accessed within uncommitted_keys.c. */
static bool all_dbs_dirty_in_current_cmd;


/*================================= Internal Prototypes ====================== */

static bool addUncommittedKey(sds key, long long offset, hashtable *uncommittedKeys);
static void pendingUncommittedKeyDestructor(void *entry);
static uint64_t uncommittedKeysHash(const void *key);
static int uncommittedKeysKeyCompare(const void *key1, const void *key2);
static const void *uncommittedKeyEntryGetKey(const void *entry);
static void uncommittedKeyEntryDestructor(void *entry);
static void handleDirtyDatabase(client *c, serverDb *db);
static bool swapdbGetParams(robj **argv, int argc, int *id1_p, int *id2_p);
static bool getDbIdFromRobj(robj *obj, int *db_id);

/*================================= Hashtable Type =========================== */

static hashtableType uncommittedKeysHashtableType = {
    .entryGetKey = uncommittedKeyEntryGetKey,
    .hashFunction = uncommittedKeysHash,
    .keyCompare = uncommittedKeysKeyCompare,
    .entryDestructor = uncommittedKeyEntryDestructor,
};

/*================================= Utility Functions ======================== */

static void pendingUncommittedKeyDestructor(void *entry) {
    if (entry == NULL) return;
    pendingUncommittedKey *uk = entry;
    if (uk->key != NULL) decrRefCount(uk->key);
    zfree(uk);
}

static uint64_t uncommittedKeysHash(const void *key) {
    const sds keystr = (const sds)key;
    return hashtableGenHashFunction(keystr, sdslen(keystr));
}

static int uncommittedKeysKeyCompare(const void *key1, const void *key2) {
    const sds s1 = (const sds)key1;
    const sds s2 = (const sds)key2;
    return sdslen(s1) == sdslen(s2) && memcmp(s1, s2, sdslen(s1)) == 0;
}

static const void *uncommittedKeyEntryGetKey(const void *entry) {
    return ((const uncommittedKeyEntry *)entry)->key;
}

static void uncommittedKeyEntryDestructor(void *entry) {
    if (entry == NULL) return;
    uncommittedKeyEntry *uke = entry;
    sdsfree(uke->key);
    zfree(uke);
}

unsigned long long getNumberOfUncommittedKeys(void) {
    unsigned long long num_uncommitted_keys = 0;
    for (int i = 0; i < server.dbnum; i++) {
        if (server.db[i] != NULL) {
            num_uncommitted_keys += hashtableSize(server.db[i]->uncommitted_keys);
        }
    }
    return num_uncommitted_keys;
}

/*================================= Key Tracking ============================= */

/* Mark a key as uncommitted at a particular replication offset.
 * If the key already exists in the hashtable, update its offset. */
static bool addUncommittedKey(const sds key, const long long offset, hashtable *uncommittedKeys) {
    uncommittedKeyEntry *entry = zmalloc(sizeof(*entry));
    entry->key = sdsdup(key);
    entry->offset = offset;

    void *existing = NULL;
    if (hashtableAddOrFind(uncommittedKeys, entry, &existing)) {
        return true; /* newly added — caller should enqueue pending record */
    }

    // Key already tracked — update to the latest offset
    uncommittedKeyEntry *existing_entry = existing;
    bool was_placeholder = (existing_entry->offset == LLONG_MAX);
    existing_entry->offset = offset;
    sdsfree(entry->key);
    zfree(entry);
    // Only need a new pending record if the existing entry wasn't already a placeholder
    return !was_placeholder;
}

/* Retrieve the uncommitted replication offset for a given key.
 * Returns -1 if the key is not tracked or has already been committed
 * (offset <= previous_acked_offset). Does NOT purge — cleanup is handled
 * by drainCommittedKeys(). */
long long getUncommittedKeyOffset(const sds key, serverDb *db, long long previous_acked_offset) {
    serverAssert(iAmPrimary());
    uncommittedKeyEntry *entry = NULL;
    if (!hashtableFind(db->uncommitted_keys, key, (void **)&entry)) {
        return -1;
    }

    long long key_offset = entry->offset;

    if (key_offset <= previous_acked_offset) {
        return -1;
    }

    return key_offset;
}

/* Handle a dirty key for a given client.
 *
 * Keys are marked dirty immediately in db->uncommitted_keys.  For single
 * commands outside a transaction the real replication offset is known.
 *
 * Inside a MULTI/EXEC or Lua script we use LLONG_MAX as a placeholder
 * offset (so reads are blocked immediately) and buffer a reference in
 * pending_uncommitted_keys.  processPendingUncommittedData() will later
 * update the offset once the transaction completes.
 *
 * Cleanup happens in drainCommittedKeys() which iterates the hashtable
 * and removes entries whose offset has been committed. */
void handleUncommittedKeyForClient(const client *c, robj *key, serverDb *db) {
    sds keystr = objectGetVal(key);

    if (scriptIsRunning() || ((c != NULL) && c->flag.multi)) {
        if (all_dbs_dirty_in_current_cmd) return;

        // Mark dirty immediately with placeholder offset
        bool needs_pending = addUncommittedKey(keystr, LLONG_MAX, db->uncommitted_keys);

        // Buffer a reference so we can update offset later (only if not already pending)
        if (needs_pending) {
            if (pending_uncommitted_keys == NULL) {
                pending_uncommitted_keys = listCreate();
                listSetFreeMethod(pending_uncommitted_keys, pendingUncommittedKeyDestructor);
            }
            pendingUncommittedKey *dirty_key = zmalloc(sizeof(pendingUncommittedKey));
            incrRefCount(key);
            dirty_key->key = key;
            dirty_key->uncommitted_keys = db->uncommitted_keys;
            listAddNodeTail(pending_uncommitted_keys, dirty_key);
        }
    } else {
        // Single command: mark dirty with real offset
        addUncommittedKey(keystr, server.primary_repl_offset, db->uncommitted_keys);
    }
}

/* Record a key modified by a background write (expiry/eviction), fed from
 * signalModifiedKey with a NULL client. Marked dirty with an LLONG_MAX
 * placeholder; drainBackgroundModifiedKeys() sets the real offset after
 * propagation (the deletion isn't propagated yet at signalModifiedKey time). */
void trackBackgroundModifiedKey(serverDb *db, robj *key) {
    if (all_dbs_dirty_in_current_cmd) return;

    if (addUncommittedKey(objectGetVal(key), LLONG_MAX, db->uncommitted_keys)) {
        if (background_modified_keys == NULL) {
            background_modified_keys = listCreate();
            listSetFreeMethod(background_modified_keys, pendingUncommittedKeyDestructor);
        }
        pendingUncommittedKey *entry = zmalloc(sizeof(*entry));
        incrRefCount(key);
        entry->key = key;
        entry->uncommitted_keys = db->uncommitted_keys;
        listAddNodeTail(background_modified_keys, entry);
    }
}

/* Apply the final replication offset to keys dirtied by background writes in
 * the execution unit that just completed, then clear the set. Called from
 * postExecutionUnitOperations() after propagation, so the offset is final. */
void drainBackgroundModifiedKeys(long long offset) {
    if (background_modified_keys == NULL || listLength(background_modified_keys) == 0) return;

    listIter li;
    listNode *ln;
    listRewind(background_modified_keys, &li);
    while ((ln = listNext(&li)) != NULL) {
        const pendingUncommittedKey *uk = listNodeValue(ln);
        uncommittedKeyEntry *entry = NULL;
        if (hashtableFind(uk->uncommitted_keys, objectGetVal(uk->key), (void **)&entry)) {
            if (entry->offset == LLONG_MAX || entry->offset < offset) {
                entry->offset = offset;
            }
        }
        listDelNode(background_modified_keys, ln);
    }
}

/*================================= Database Modification ==================== */

static void handleDirtyDatabase(client *c, serverDb *db) {
    if ((c->flag.multi) || scriptIsRunning()) {
        if (all_dbs_dirty_in_current_cmd) return;
        if (db != NULL) {
            listAddNodeTail(pending_uncommitted_dbs, db);
        } else {
            all_dbs_dirty_in_current_cmd = true;
            listEmpty(pending_uncommitted_keys);
            if (background_modified_keys != NULL) listEmpty(background_modified_keys);
            listEmpty(pending_uncommitted_dbs);
            /* FLUSHALL inside a transaction: any keys previously dirtied
             * in this transaction are now gone.  Clear the per-DB
             * uncommitted_keys hashtables so stale LLONG_MAX-offset
             * entries don't block future reads after the EXEC commits. */
            for (int i = 0; i < server.dbnum; i++) {
                if (server.db[i] != NULL) {
                    hashtableEmpty(server.db[i]->uncommitted_keys, NULL);
                }
            }
        }
    } else {
        if (db != NULL) {
            db->dirty_repl_offset = server.primary_repl_offset;
        } else {
            for (int i = 0; i < server.dbnum; i++) {
                if (server.db[i] != NULL) {
                    server.db[i]->dirty_repl_offset = server.primary_repl_offset;
                }
            }
        }
    }
}

void handleDatabaseModification(client *c) {
    if (c->cmd->proc == swapdbCommand && server.cluster_enabled == 0) {
        int id1, id2;
        if (swapdbGetParams(c->argv, c->argc, &id1, &id2)) {
            handleDirtyDatabase(c, server.db[id1]);
            handleDirtyDatabase(c, server.db[id2]);
        }
    } else if (c->cmd->proc == flushdbCommand) {
        handleDirtyDatabase(c, c->db);
    } else if (c->cmd->proc == flushallCommand) {
        handleDirtyDatabase(c, NULL);
    }
}

/*================================= Command Parameter Helpers ================ */

static bool swapdbGetParams(robj **argv, int argc, int *id1_p, int *id2_p) {
    long long dbid1, dbid2;
    if (argc != 3) return false;
    if (server.cluster_enabled) return false;
    if (getLongLongFromObject(argv[1], &dbid1) != C_OK) return false;
    if (getLongLongFromObject(argv[2], &dbid2) != C_OK) return false;
    if (dbid1 < 0 || dbid1 >= server.dbnum) return false;
    if (dbid2 < 0 || dbid2 >= server.dbnum) return false;
    if (dbid1 == dbid2) return false;

    *id1_p = (int)dbid1;
    *id2_p = (int)dbid2;
    return true;
}

static bool getDbIdFromRobj(robj *obj, int *db_id) {
    if ((getIntFromObject(obj, db_id) != C_OK) || (*db_id < 0) || (*db_id >= server.dbnum)) {
        return false;
    }
    return true;
}

bool getTargetDbIdForCopyCommand(int argc, robj **argv, int selected_dbid, int *target_dbid) {
    const int copy_command_optional_arg_start_index = 3;

    *target_dbid = selected_dbid;

    for (int j = copy_command_optional_arg_start_index; j < argc; j++) {
        if (!strcasecmp(objectGetVal(argv[j]), "replace")) {
            continue;
        } else if (!strcasecmp(objectGetVal(argv[j]), "db") && (argc > j + 1)) {
            if (!getDbIdFromRobj(argv[j + 1], target_dbid)) {
                return false;
            }
            j++;
        } else {
            return false;
        }
    }
    return true;
}

/*================================= Drain / Cleanup ========================== */

/* Remove committed entries from the per-DB uncommitted_keys hashtables.
 *
 * Iterates each database's uncommitted_keys hashtable with a safe iterator
 * and deletes entries whose offset has been durably committed.
 *
 * With appendfsync=always the uncommitted set stays small (bounded by keys
 * written between fsyncs), so the full-scan cost is smaller than the fsync. */
void drainCommittedKeys(long long committed_offset) {
    for (int i = 0; i < server.dbnum; i++) {
        serverDb *db = server.db[i];
        if (db == NULL) continue;

        if (hashtableSize(db->uncommitted_keys) > 0) {
            hashtableIterator iter;
            hashtableInitIterator(&iter, db->uncommitted_keys, HASHTABLE_ITER_SAFE);
            void *entry;
            while (hashtableNext(&iter, &entry)) {
                uncommittedKeyEntry *uke = entry;
                if (uke->offset <= committed_offset) {
                    hashtableDelete(db->uncommitted_keys, uke->key);
                }
            }
            hashtableCleanupIterator(&iter);
        }

        if (db->dirty_repl_offset <= committed_offset) {
            db->dirty_repl_offset = -1;
        }
    }
}

// Initialize sync replication related fields for a database.
void replyBlockingInitDatabase(serverDb *db) {
    db->uncommitted_keys = hashtableCreate(&uncommittedKeysHashtableType);
    db->dirty_repl_offset = -1;
}

// Clear all uncommitted keys for each database.
void clearAllUncommittedKeys(void) {
    serverLog(LL_DEBUG, "Clearing all uncommitted keys");
    /* Clear pending list first — entries hold raw pointers to db->uncommitted_keys
     * hashtables that we're about to free. */
    if (pending_uncommitted_keys != NULL) {
        listEmpty(pending_uncommitted_keys);
    }
    if (background_modified_keys != NULL) {
        listEmpty(background_modified_keys);
    }
    if (pending_uncommitted_dbs != NULL) {
        listEmpty(pending_uncommitted_dbs);
    }
    all_dbs_dirty_in_current_cmd = false;
    for (int i = 0; i < server.dbnum; i++) {
        serverDb *db = server.db[i];
        if (db == NULL) continue;
        hashtableRelease(db->uncommitted_keys);
        replyBlockingInitDatabase(db);
    }
}

/*================================= Access Validation ======================== */

// Determine if there are uncommitted keys in the server.
int hasUncommittedKeys(void) {
    for (int i = 0; i < server.dbnum; i++) {
        if (server.db[i] && (hashtableSize(server.db[i]->uncommitted_keys) > 0))
            return 1;
    }
    return 0;
}

/*================================= Pending Data Processing ================== */

void uncommittedKeysInitPending(void) {
    pending_uncommitted_keys = listCreate();
    listSetFreeMethod(pending_uncommitted_keys, pendingUncommittedKeyDestructor);
    background_modified_keys = listCreate();
    listSetFreeMethod(background_modified_keys, pendingUncommittedKeyDestructor);
    pending_uncommitted_dbs = listCreate();
    all_dbs_dirty_in_current_cmd = false;
}

void uncommittedKeysCleanupPending(void) {
    if (pending_uncommitted_keys != NULL) {
        listRelease(pending_uncommitted_keys);
        pending_uncommitted_keys = NULL;
    }
    if (background_modified_keys != NULL) {
        listRelease(background_modified_keys);
        background_modified_keys = NULL;
    }
    if (pending_uncommitted_dbs != NULL) {
        listRelease(pending_uncommitted_dbs);
        pending_uncommitted_dbs = NULL;
    }
}

/* After a transaction completes, update the placeholder offsets on keys
 * that were dirtied during the transaction to the real replication offset.
 * Cleanup will happen when drainCommittedKeys() iterates the hashtable. */
void processPendingUncommittedData(long long blocking_repl_offset) {
    if (listLength(pending_uncommitted_keys) > 0) {
        listIter li;
        listNode *key_node;
        listRewind(pending_uncommitted_keys, &li);
        while ((key_node = listNext(&li)) != NULL) {
            const pendingUncommittedKey *uk = listNodeValue(key_node);
            sds keystr = objectGetVal(uk->key);

            // Update the placeholder offset to the real one
            uncommittedKeyEntry *entry = NULL;
            if (hashtableFind(uk->uncommitted_keys, keystr, (void **)&entry)) {
                // Only update if still at placeholder or our offset is newer
                if (entry->offset == LLONG_MAX || entry->offset < blocking_repl_offset) {
                    entry->offset = blocking_repl_offset;
                }
            }

            listDelNode(pending_uncommitted_keys, key_node);
        }
    }

    if (all_dbs_dirty_in_current_cmd) {
        for (int i = 0; i < server.dbnum; i++) {
            if (server.db[i] != NULL) {
                server.db[i]->dirty_repl_offset = blocking_repl_offset;
            }
        }
        all_dbs_dirty_in_current_cmd = false;
    } else if (listLength(pending_uncommitted_dbs) > 0) {
        listIter li;
        listNode *db_node;
        listRewind(pending_uncommitted_dbs, &li);
        while ((db_node = listNext(&li)) != NULL) {
            serverDb *db = listNodeValue(db_node);
            db->dirty_repl_offset = blocking_repl_offset;
            listDelNode(pending_uncommitted_dbs, db_node);
        }
    }

    serverAssert(listLength(pending_uncommitted_keys) == 0);
    serverAssert(listLength(pending_uncommitted_dbs) == 0);
    serverAssert(all_dbs_dirty_in_current_cmd == false);
}
