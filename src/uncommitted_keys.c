#include "uncommitted_keys.h"
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

typedef struct uncommittedKeyCleanupCtx {
    hashtable *ht;
    long long acked_offset;
    unsigned long long *scan_count;
} uncommittedKeyCleanupCtx;

/**
 * Below are the data structures used to buffer intermediate dirty keys/DBs for multi-command
 * blocks including MULTI/EXEC and Lua script. As we execute the individual commands in the
 * transaction, we don't know the final replication offset so we store the updated keys and DBs
 * in afterCommandTrackReplOffset(), and process them in postCommandExec() after the entire transaction is
 * propagated to the replication buffer.
 */
typedef struct pendingUncommittedKey {
    robj *key;
    hashtable *uncommitted_keys;
} pendingUncommittedKey;

// Track the list of pending uncommitted keys for an ongoing multi-command block
static list *pending_uncommitted_keys;

// Track the list of pending uncommitted databases for an ongoing multi-command block
static list *pending_uncommitted_dbs;


/*================================= Internal Prototypes ====================== */

static void addUncommittedKey(sds key, long long offset, hashtable *uncommittedKeys);
static void uncommittedKeysCleanupScanCallback(void *privdata, void *entry);
static void pendingUncommittedKeyDestructor(void *entry);
static uint64_t uncommittedKeysHash(const void *key);
static int uncommittedKeysKeyCompare(const void *key1, const void *key2);
static const void *uncommittedKeyEntryGetKey(const void *entry);
static void uncommittedKeyEntryDestructor(void *entry);
static void handleDirtyDatabase(client *c, serverDb *db);

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

unsigned long long getUncommittedKeysCleanupTimeLimit(unsigned long long num_uncommitted_keys) {
    unsigned long long time_limit_ms = 1;
    if (num_uncommitted_keys > 0) {
        time_limit_ms = ceil(server.durability.keys_cleanup_time_limit_ms * MIN(1, (double)(num_uncommitted_keys / 1000000.0)));
    }
    return time_limit_ms;
}

/*================================= Key Tracking ============================= */

/**
 * Mark a key as uncommitted at a particular replication offset.
 */
static void addUncommittedKey(const sds key, const long long offset, hashtable *uncommittedKeys) {
    uncommittedKeyEntry *entry = zmalloc(sizeof(*entry));
    entry->key = sdsdup(key);
    entry->offset = offset;

    void *existing = NULL;
    if (hashtableAddOrFind(uncommittedKeys, entry, &existing)) {
        return;
    }

    uncommittedKeyEntry *existing_entry = existing;
    existing_entry->offset = offset;
    sdsfree(entry->key);
    zfree(entry);
}

/**
 * Callback for hashtableScan for cleaning up uncommitted keys.
 */
static void uncommittedKeysCleanupScanCallback(void *privdata, void *entry) {
    uncommittedKeyCleanupCtx *ctx = privdata;
    uncommittedKeyEntry *uke = entry;
    if (uke->offset <= ctx->acked_offset) {
        hashtableDelete(ctx->ht, uke->key);
    }
    (*ctx->scan_count)++;
}

/**
 * Retrieve the uncommitted replication offset for a given key, purge the given
 * key from uncommitted keys set if the replication offset has been committed.
 */
long long syncReplicationPurgeAndGetUncommittedKeyOffset(const sds key, serverDb *db) {
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
 */
void handleUncommittedKeyForClient(const client *c, robj *key, serverDb *db) {
    if ((c != NULL) && ((c->flag.multi) || scriptIsRunning())) {
        if (server.durability.all_dbs_dirty_in_current_cmd) return;
        if (pending_uncommitted_keys == NULL) {
            pending_uncommitted_keys = listCreate();
            listSetFreeMethod(pending_uncommitted_keys, pendingUncommittedKeyDestructor);
        }
        pendingUncommittedKey *dirty_key = (pendingUncommittedKey *)zmalloc(sizeof(pendingUncommittedKey));
        incrRefCount(key);
        dirty_key->key = key;
        dirty_key->uncommitted_keys = db->uncommitted_keys;
        listAddNodeTail(pending_uncommitted_keys, dirty_key);
    } else {
        addUncommittedKey(objectGetVal(key), server.primary_repl_offset, db->uncommitted_keys);
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

struct serverCommand *lookupCommandOrOriginalBySds(sds s) {
    struct serverCommand *cmd = lookupCommandBySdsLogic(server.commands, s);
    if (!cmd) cmd = lookupCommandBySdsLogic(server.orig_commands, s);
    return cmd;
}

/* Renamed from amzSwapdbGetParams */
bool swapdbGetParams(robj **argv, int argc, int *id1_p, int *id2_p) {
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

/* Renamed from amzSelectGetParams */
bool selectGetParams(robj **argv, int argc, client *permission_client, int *dbid_p) {
    int dbid;
    if (argc != 2) return false;
    if (getIntFromObject(argv[1], &dbid) != C_OK) return false;
    if (dbid < 0 || dbid >= server.dbnum) return false;

    *dbid_p = dbid;
    return true;
}

bool commandModifiesFirstKeyOnly(struct serverCommand *cmd) {
    return false;
}

/* Renamed from amzGetDbIdFromRobj */
bool getDbIdFromRobj(robj *obj, int *db_id) {
    if ((getIntFromObject(obj, db_id) != C_OK) || (*db_id < 0) || (*db_id >= server.dbnum)) {
        return false;
    }
    return true;
}

/* Renamed from amzGetTargetDbIdForCopyCommand */
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

/*================================= Cleanup ================================== */

/**
 * Clears all uncommitted DBs and keys that are properly acknowledged.
 */
void clearUncommittedKeysAcknowledged(void) {
    if (!isPrimarySyncReplicationEnabled()) {
        return;
    }

    durable_t *durability = &server.durability;
    const int TIME_CHECK_INTERVAL = 100;
    unsigned long long scan_count = 0;

    unsigned long long num_uncommitted_keys = getNumberOfUncommittedKeys();
    if (num_uncommitted_keys == 0) return;

    unsigned long long time_limit_ms = getUncommittedKeysCleanupTimeLimit(num_uncommitted_keys);
    unsigned long long start_time_ms = mstime();
    unsigned long long next_time_check = TIME_CHECK_INTERVAL;
    while (durability->curr_db_scan_idx < server.dbnum) {
        serverDb *db = server.db[durability->curr_db_scan_idx];
        if (db != NULL) {
            if (db->dirty_repl_offset <= server.durability.previous_acked_offset) {
                db->dirty_repl_offset = -1;
            }

            if (hashtableSize(db->uncommitted_keys) > 0) {
                uncommittedKeyCleanupCtx ctx = {
                    .ht = db->uncommitted_keys,
                    .acked_offset = server.durability.previous_acked_offset,
                    .scan_count = &scan_count,
                };

                if (!db->scan_in_progress) {
                    db->uncommitted_keys_cursor = 0;
                    db->scan_in_progress = 1;
                }

                do {
                    db->uncommitted_keys_cursor =
                        hashtableScan(db->uncommitted_keys, db->uncommitted_keys_cursor, uncommittedKeysCleanupScanCallback, &ctx);

                    if (time_limit_ms > 0 && scan_count >= next_time_check) {
                        const unsigned long long cur_time_ms = mstime();
                        if (cur_time_ms - start_time_ms > time_limit_ms) {
                            return;
                        }
                        next_time_check += TIME_CHECK_INTERVAL;
                    }
                } while (db->uncommitted_keys_cursor != 0);
            }

            if (db->scan_in_progress) {
                db->scan_in_progress = 0;
            }
        }
        durability->curr_db_scan_idx++;
    }

    if (durability->curr_db_scan_idx == server.dbnum) {
        durability->curr_db_scan_idx = 0;
    }
}

/**
 * Initialize sync replication related fields for a database.
 */
void syncReplicationInitDatabase(serverDb *db) {
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
        syncReplicationInitDatabase(db);
    }
    server.durability.curr_db_scan_idx = 0;
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
    server.durability.all_dbs_dirty_in_current_cmd = false;
}

void uncommittedKeysCleanupPending(void) {
    if (pending_uncommitted_keys != NULL) {
        listRelease(pending_uncommitted_keys);
        pending_uncommitted_keys = NULL;
    }
}

/**
 * Marks keys, databases, and the function store dirty at the current
 * replication offset if they were updated during a transaction.
 */
void processPendingUncommittedData(long long blocking_repl_offset) {
    if (listLength(pending_uncommitted_keys) > 0) {
        listIter li;
        listNode *key_node;
        listRewind(pending_uncommitted_keys, &li);
        while ((key_node = listNext(&li)) != NULL) {
            const pendingUncommittedKey *uk = listNodeValue(key_node);
            addUncommittedKey(objectGetVal(uk->key), blocking_repl_offset, uk->uncommitted_keys);
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
