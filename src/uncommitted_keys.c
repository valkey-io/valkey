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

/**
 * An entry in the offset tracker queue.  When a key is dirtied we add it to
 * this FIFO queue.  When the durability system reports a new committed offset
 * we drain the head of the queue and remove the keys whose offset has been
 * committed from the per-DB uncommitted_keys hashtable.
 */
typedef struct offsetTrackerEntry {
    sds key;            /* owned copy of the key string */
    long long offset;   /* replication offset this key must reach */
    hashtable *ht;      /* pointer to db->uncommitted_keys that owns the key */
} offsetTrackerEntry;

/**
 * Pending key reference used during multi-command blocks (MULTI/EXEC, Lua).
 * We mark keys dirty immediately but don't yet know the final replication
 * offset, so we keep a reference to update the offset and enqueue for
 * cleanup tracking after the transaction completes.
 */
typedef struct pendingUncommittedKey {
    robj *key;
    hashtable *uncommitted_keys;
} pendingUncommittedKey;

/* Offset tracker queue — FIFO list of offsetTrackerEntry.
 * Entries are appended when keys are dirtied (or when a transaction completes
 * and we learn the real offset for keys dirtied during the transaction).
 * Entries are drained from the head when the committed offset advances. */
static list *offset_tracker_queue;

/* Pending keys buffered during MULTI/EXEC or Lua scripts.  These are keys
 * that have already been marked dirty in uncommitted_keys (with LLONG_MAX
 * as a placeholder offset) but whose real offset is not yet known. */
static list *pending_uncommitted_keys;

/* Pending databases dirtied during a multi-command block. */
static list *pending_uncommitted_dbs;


/*================================= Internal Prototypes ====================== */

static void addUncommittedKey(sds key, long long offset, hashtable *uncommittedKeys);
static void enqueueOffsetTracker(sds key, long long offset, hashtable *ht);
static void pendingUncommittedKeyDestructor(void *entry);
static void offsetTrackerEntryDestructor(void *entry);
static uint64_t uncommittedKeysHash(const void *key);
static int uncommittedKeysKeyCompare(const void *key1, const void *key2);
static const void *uncommittedKeyEntryGetKey(const void *entry);
static void uncommittedKeyEntryDestructor(void *entry);
static void handleDirtyDatabase(client *c, serverDb *db);
static bool swapdbGetParams(robj **argv, int argc, int *id1_p, int *id2_p);
static bool selectGetParams(robj **argv, int argc, client *permission_client, int *dbid_p);
static bool getDbIdFromRobj(robj *obj, int *db_id);
static int isSingleCommandAccessingUncommittedKeys(const serverDb *db, struct serverCommand *cmd, robj **argv, int argc);
static int isAccessingUncommittedData(client *c);

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

static void offsetTrackerEntryDestructor(void *entry) {
    if (entry == NULL) return;
    offsetTrackerEntry *ote = entry;
    sdsfree(ote->key);
    zfree(ote);
}

static uint64_t uncommittedKeysHash(const void *key) {
    const sds keystr = (const sds)key;
    return hashtableGenHashFunction(keystr, sdslen(keystr));
}

static int uncommittedKeysKeyCompare(const void *key1, const void *key2) {
    const sds s1 = (const sds)key1;
    const sds s2 = (const sds)key2;
    return sdslen(s1) != sdslen(s2) || memcmp(s1, s2, sdslen(s1));
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

/**
 * Mark a key as uncommitted at a particular replication offset.
 * If the key already exists in the hashtable, update its offset.
 */
static void addUncommittedKey(const sds key, const long long offset, hashtable *uncommittedKeys) {
    uncommittedKeyEntry *entry = zmalloc(sizeof(*entry));
    entry->key = sdsdup(key);
    entry->offset = offset;

    void *existing = NULL;
    if (hashtableAddOrFind(uncommittedKeys, entry, &existing)) {
        return; /* newly added */
    }

    /* Key already tracked — update to the latest offset */
    uncommittedKeyEntry *existing_entry = existing;
    existing_entry->offset = offset;
    sdsfree(entry->key);
    zfree(entry);
}

/**
 * Enqueue a key into the offset tracker queue for later cleanup.
 */
static void enqueueOffsetTracker(sds key, long long offset, hashtable *ht) {
    offsetTrackerEntry *ote = zmalloc(sizeof(*ote));
    ote->key = sdsdup(key);
    ote->offset = offset;
    ote->ht = ht;
    listAddNodeTail(offset_tracker_queue, ote);
}

/**
 * Retrieve the uncommitted replication offset for a given key, purge the given
 * key from uncommitted keys set if the replication offset has been committed.
 */
long long durabilityPurgeAndGetUncommittedKeyOffset(const sds key, serverDb *db) {
    serverAssert(iAmPrimary());
    uncommittedKeyEntry *entry = NULL;
    if (!hashtableFind(db->uncommitted_keys, key, (void **)&entry)) {
        return -1;
    }

    long long key_offset = entry->offset;

    if (key_offset <= server.durability.previous_acked_offset) {
        hashtableDelete(db->uncommitted_keys, key);
        return -1;
    }

    return key_offset;
}

/**
 * Handle a dirty key for a given client.
 *
 * Keys are marked dirty immediately in db->uncommitted_keys.  For single
 * commands outside a transaction the real replication offset is known so we
 * also enqueue the key in the offset tracker right away.
 *
 * Inside a MULTI/EXEC or Lua script we use LLONG_MAX as a placeholder
 * offset (so reads are blocked immediately) and buffer a reference in
 * pending_uncommitted_keys.  processPendingUncommittedData() will later
 * update the offset and enqueue for cleanup once the transaction completes.
 */
void handleUncommittedKeyForClient(const client *c, robj *key, serverDb *db) {
    sds keystr = objectGetVal(key);

    if ((c != NULL) && ((c->flag.multi) || scriptIsRunning())) {
        if (server.durability.all_dbs_dirty_in_current_cmd) return;

        /* Mark dirty immediately with placeholder offset */
        addUncommittedKey(keystr, LLONG_MAX, db->uncommitted_keys);

        /* Buffer a reference so we can update offset + enqueue later */
        if (pending_uncommitted_keys == NULL) {
            pending_uncommitted_keys = listCreate();
            listSetFreeMethod(pending_uncommitted_keys, pendingUncommittedKeyDestructor);
        }
        pendingUncommittedKey *dirty_key = zmalloc(sizeof(pendingUncommittedKey));
        incrRefCount(key);
        dirty_key->key = key;
        dirty_key->uncommitted_keys = db->uncommitted_keys;
        listAddNodeTail(pending_uncommitted_keys, dirty_key);
    } else {
        /* Single command: mark dirty with real offset and enqueue immediately */
        addUncommittedKey(keystr, server.primary_repl_offset, db->uncommitted_keys);
        enqueueOffsetTracker(keystr, server.primary_repl_offset, db->uncommitted_keys);
    }
}

/*================================= Database Modification ==================== */

static void handleDirtyDatabase(client *c, serverDb *db) {
    if ((c->flag.multi) || scriptIsRunning()) {
        if (server.durability.all_dbs_dirty_in_current_cmd) return;
        if (db != NULL) {
            listAddNodeTail(pending_uncommitted_dbs, db);
        } else {
            server.durability.all_dbs_dirty_in_current_cmd = true;
            listEmpty(pending_uncommitted_keys);
            listEmpty(pending_uncommitted_dbs);
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

static bool selectGetParams(robj **argv, int argc, client *permission_client, int *dbid_p) {
    UNUSED(permission_client);
    int dbid;
    if (argc != 2) return false;
    if (getIntFromObject(argv[1], &dbid) != C_OK) return false;
    if (dbid < 0 || dbid >= server.dbnum) return false;

    *dbid_p = dbid;
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

/**
 * Drain committed entries from the offset tracker queue, removing keys
 * from their per-DB uncommitted_keys hashtable when their offset has been
 * durably committed.
 *
 * Only removes a key from the hashtable if the key's current offset in the
 * hashtable matches the queued offset (so a re-dirtied key at a higher
 * offset is not prematurely removed).
 */
void drainCommittedKeys(long long committed_offset) {
    while (listLength(offset_tracker_queue) > 0) {
        listNode *head = listFirst(offset_tracker_queue);
        offsetTrackerEntry *ote = listNodeValue(head);

        if (ote->offset > committed_offset) break;

        /* Check if key still exists with this offset — a later write may
         * have updated it to a higher offset. */
        uncommittedKeyEntry *entry = NULL;
        if (hashtableFind(ote->ht, ote->key, (void **)&entry)) {
            if (entry->offset <= committed_offset) {
                hashtableDelete(ote->ht, ote->key);
            }
        }

        listDelNode(offset_tracker_queue, head);
    }

    /* Also clear dirty DB offsets */
    for (int i = 0; i < server.dbnum; i++) {
        serverDb *db = server.db[i];
        if (db != NULL && db->dirty_repl_offset <= committed_offset) {
            db->dirty_repl_offset = -1;
        }
    }
}

/**
 * Initialize sync replication related fields for a database.
 */
void durabilityInitDatabase(serverDb *db) {
    db->uncommitted_keys = hashtableCreate(&uncommittedKeysHashtableType);
    db->dirty_repl_offset = -1;
    db->uncommitted_keys_cursor = 0;
    db->scan_in_progress = 0;
}

/**
 * Clear all uncommitted keys for each database.
 */
void clearAllUncommittedKeys(void) {
    serverLog(LL_NOTICE, "Clearing all uncommitted keys for sync replication");
    for (int i = 0; i < server.dbnum; i++) {
        serverDb *db = server.db[i];
        if (db == NULL) continue;
        hashtableRelease(db->uncommitted_keys);
        durabilityInitDatabase(db);
    }
    /* Also clear the offset tracker queue */
    if (offset_tracker_queue != NULL) {
        listEmpty(offset_tracker_queue);
    }
}

/*================================= Access Validation ======================== */

/**
 * Determines if a single command is trying to access an uncommitted key.
 */
int isSingleCommandAccessingUncommittedKeys(const serverDb *db, struct serverCommand *cmd, robj **argv, int argc) {
    if (hashtableSize(db->uncommitted_keys) == 0) return 0;

    getKeysResult keysResult;
    initGetKeysResult(&keysResult);
    const int numKeys = getKeysFromCommand(cmd, argv, argc, &keysResult);
    const keyReference *keys = keysResult.keys;

    for (int i = 0; i < numKeys; i++) {
        const sds keyStr = objectGetVal(argv[keys[i].pos]);
        if (hashtableFind(db->uncommitted_keys, keyStr, NULL)) {
            getKeysFreeResult(&keysResult);
            return 1;
        }
    }

    getKeysFreeResult(&keysResult);
    return 0;
}

/**
 * Determine if there are uncommitted keys in the server.
 */
int hasUncommittedKeys(void) {
    for (int i = 0; i < server.dbnum; i++) {
        if (server.db[i] && (hashtableSize(server.db[i]->uncommitted_keys) > 0))
            return 1;
    }
    return 0;
}

/**
 * Determine if a client is trying to access uncommitted data.
 */
int isAccessingUncommittedData(client *c) {
    // Informational command handling
    if (IS_KEYSPACE_INFORMATIONAL(c->cmd) && (hasUncommittedKeys() || isDurableFunctionStoreUncommitted())) {
        return 1;
    }

    // Single command handling
    if (isSingleCommandAccessingUncommittedKeys(c->db, c->cmd, c->argv, c->argc) || (isFunctionStoreRWCommand(c) && isDurableFunctionStoreUncommitted())) {
        return 1;
    }

    int ret_val = 0;
    if ((c->flag.multi) && c->cmd->proc == execCommand) {
        serverDb *cur_db = c->db;
        for (int i = 0; i < c->mstate->count; i++) {
            multiCmd mc = c->mstate->commands[i];
            if (mc.cmd->proc == selectCommand) {
                int db_id;
                if (selectGetParams(mc.argv, mc.argc, c, &db_id)) {
                    c->db = server.db[db_id];
                    continue;
                } else {
                    discardTransaction(c);
                    ret_val = 1;
                    break;
                }
            }
            if (isSingleCommandAccessingUncommittedKeys(c->db, mc.cmd, mc.argv, mc.argc) || (isFunctionStoreRWCommand(c) && isDurableFunctionStoreUncommitted())) {
                discardTransaction(c);
                ret_val = 1;
                break;
            }
        }
        c->db = cur_db;
    }
    return ret_val;
}

/**
 * Checks if we should reject a command that is accessing uncommitted data.
 */
bool shouldRejectCommandWithUncommittedData(client *c) {
    if (c->cmd == NULL || ((c->cmd->flags & CMD_ADMIN)) || c->flag.primary) {
        return false;
    }

    if ((!iAmPrimary()) && isAccessingUncommittedData(c)) {
        return true;
    }

    return false;
}

/*================================= Pending Data Processing ================== */

void uncommittedKeysInitPending(void) {
    pending_uncommitted_keys = listCreate();
    listSetFreeMethod(pending_uncommitted_keys, pendingUncommittedKeyDestructor);
    pending_uncommitted_dbs = listCreate();
    offset_tracker_queue = listCreate();
    listSetFreeMethod(offset_tracker_queue, offsetTrackerEntryDestructor);
    server.durability.all_dbs_dirty_in_current_cmd = false;
}

void uncommittedKeysCleanupPending(void) {
    if (pending_uncommitted_keys != NULL) {
        listRelease(pending_uncommitted_keys);
        pending_uncommitted_keys = NULL;
    }
    if (pending_uncommitted_dbs != NULL) {
        listRelease(pending_uncommitted_dbs);
        pending_uncommitted_dbs = NULL;
    }
    if (offset_tracker_queue != NULL) {
        listRelease(offset_tracker_queue);
        offset_tracker_queue = NULL;
    }
}

/**
 * After a transaction completes, update the placeholder offsets on keys
 * that were dirtied during the transaction to the real replication offset,
 * and enqueue them in the offset tracker for cleanup.
 */
void processPendingUncommittedData(long long blocking_repl_offset) {
    if (listLength(pending_uncommitted_keys) > 0) {
        listIter li;
        listNode *key_node;
        listRewind(pending_uncommitted_keys, &li);
        while ((key_node = listNext(&li)) != NULL) {
            const pendingUncommittedKey *uk = listNodeValue(key_node);
            sds keystr = objectGetVal(uk->key);

            /* Update the placeholder offset to the real one */
            uncommittedKeyEntry *entry = NULL;
            if (hashtableFind(uk->uncommitted_keys, keystr, (void **)&entry)) {
                /* Only update if still at placeholder or our offset is newer */
                if (entry->offset == LLONG_MAX || entry->offset < blocking_repl_offset) {
                    entry->offset = blocking_repl_offset;
                }
            }

            /* Enqueue for cleanup tracking */
            enqueueOffsetTracker(keystr, blocking_repl_offset, uk->uncommitted_keys);
            listDelNode(pending_uncommitted_keys, key_node);
        }
    }

    if (server.durability.all_dbs_dirty_in_current_cmd) {
        for (int i = 0; i < server.dbnum; i++) {
            if (server.db[i] != NULL) {
                server.db[i]->dirty_repl_offset = blocking_repl_offset;
            }
        }
        server.durability.all_dbs_dirty_in_current_cmd = false;
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
    serverAssert(server.durability.all_dbs_dirty_in_current_cmd == false);

    updateFuncStoreBlockingOffsetForWrite(blocking_repl_offset);
}
