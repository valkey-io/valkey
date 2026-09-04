/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fmacros.h"
#include "bgiteration.h"
#include "dict.h"
#include "fifo.h"
#include "kvstore.h"
#include "monotonic.h"
#include "mutexqueue.h"
#include "server.h"


static bool receiveItemsBackFromOneIterator(bgIterator *it);


// Returns true if the cmd is a script command that may replicate.
static bool isScriptCallWriteCmd(struct serverCommand *cmd) {
    return ((cmd->proc == fcallCommand) || (cmd->proc == evalCommand) || (cmd->proc == evalShaCommand));
}

/* The PFCOUNT command (which does NOT have the CMD_WRITE flag) modifies the underlying string and
 * is replicated as a write.  So it needs to be detected and handled specially. */
static bool isWriteCmd(struct serverCommand *cmd) {
    return ((cmd->flags & CMD_WRITE) || (cmd->proc == pfcountCommand) || (cmd->proc == execCommand) || (isScriptCallWriteCmd(cmd)));
}

// Returns true if the command is a deletion based command (DEL or UNLINK)
static bool isDeleteCmd(struct serverCommand *cmd) {
    return ((cmd->proc == delCommand) || (cmd->proc == unlinkCommand));
}

/* Returns true if the command is a flush based command (FLUSHDB or FLUSHALL) */
static bool isFlushCmd(struct serverCommand *cmd) {
    return ((cmd->proc == flushdbCommand) || (cmd->proc == flushallCommand));
}

/* This utility utilizes the main thread and background threads for processing.  The API is split,
 * with some of the functions intended for the main thread and others intended for the background
 * clients.  This sanity check ensures that we maintain thread safety, calling the API as intended. */
static bool hasMainThreadExclusivity(void) {
    /* Modules interact with the main thread using a mutex.  If a module owns the mutex, consider
     *  that equivalent to being on the main thread. */
    bool mightBeInModule = (atomic_load_explicit(&server.module_gil_acquired, memory_order_relaxed) == 0);
    return onServerMainThread() || mightBeInModule;
}


/* Parse a parameters robj, extracting a valid DBID.
 * Returns FALSE if DBID isn't valid. */
static bool getDbIdFromRobj(robj *obj, int *db_id) {
    long long value;
    if (getLongLongFromObject(obj, &value) != C_OK) return false;
    if ((value < 0) || (value >= server.dbnum)) return false;
    *db_id = (int)value;
    return true;
}

/* Parse the parameters of the COPY command, extracting the target DBID.
 * Returns FALSE if the command would not run. */
static bool getTargetDbIdForCopyCommand(int argc, robj **argv, int selected_dbid, int *target_dbid) {
    const int COPY_COMMAND_OPTIONAL_ARG_START_INDEX = 3;

    *target_dbid = selected_dbid;

    for (int i = COPY_COMMAND_OPTIONAL_ARG_START_INDEX; i < argc; i++) {
        if (!strcasecmp((char *)objectGetVal(argv[i]), "replace")) {
            continue;
        } else if (!strcasecmp((char *)objectGetVal(argv[i]), "db") && (i + 1 < argc)) {
            /* Note the parsing here needs to perfectly match what we have in copyCommand.  The
             * following command is considered OK so we can't return here, but must continue to
             * parse till the last db which is the one that's effectively used.
             *    COPY key1 key2 db 1 db 2 db 3    (This will use db 3) */
            if (!getDbIdFromRobj(argv[i + 1], target_dbid)) {
                return false; // parse failure
            }
            i++; // Consume additional argument
        } else {
            return false; // parse failure
        }
    }
    return true;
}

/* Get parameters for the SWAPDB command.
 * The optional permission_client allows for checking of a client's permission for swapdb.
 * Returns true if command would be executed. */
static bool getParamsForSwapdb(int argc, robj **argv, client *permission_client, int *id1_p, int *id2_p) {
    static struct serverCommand *swapdb_cmd = NULL;

    // We don't need to check permissions in the replication phase
    if (permission_client != NULL) {
        if (swapdb_cmd == NULL) {
            swapdb_cmd = lookupCommandByCString("swapdb");
            serverAssert(swapdb_cmd != NULL);
        }

        int idxptr;
        if (ACLCheckAllUserCommandPerm(permission_client->user, swapdb_cmd, argv, argc,
                                       permission_client->db->id, &idxptr) != ACL_OK) return false;
    }

    long long dbid1, dbid2;
    if (argc != 3) return false;
    if (server.cluster_enabled) return false;
    if (getLongLongFromObject(argv[1], &dbid1) != C_OK) return false;
    if (getLongLongFromObject(argv[2], &dbid2) != C_OK) return false;
    if (dbid1 < 0 || dbid1 >= server.dbnum) return false;
    if (dbid2 < 0 || dbid2 >= server.dbnum) return false;
    if (dbid1 == dbid2) return false; // Valid, but doesn't do anything

    *id1_p = (int)dbid1;
    *id2_p = (int)dbid2;
    return true;
}

/* Get parameters for the SELECT command.
 * The optional permission_client allows for checking of a client's permission for select.
 * Returns true if command would be executed. */
static bool getParamsForSelect(int argc, robj **argv, client *permission_client, int *dbid_p) {
    static struct serverCommand *select_cmd = NULL;

    // We don't need to check permissions in the replication phase
    if (permission_client != NULL) {
        if (select_cmd == NULL) {
            select_cmd = lookupCommandByCString("select");
            serverAssert(select_cmd != NULL);
        }

        int idxptr;
        if (ACLCheckAllUserCommandPerm(permission_client->user, select_cmd, argv, argc,
                                       permission_client->db->id, &idxptr) != ACL_OK) return false;
    }

    long long dbid;
    if (argc != 2) return false;
    if (getLongLongFromObject(argv[1], &dbid) != C_OK) return false;
    if (dbid < 0 || dbid >= server.dbnum) return false;

    *dbid_p = (int)dbid;
    return true;
}

static void pauseRehashForKvsHashtable(kvstore *kvs, int didx) {
    hashtable *ht = kvstoreGetHashtable(kvs, didx);
    if (ht != NULL) hashtablePauseRehashing(ht);
}

static void resumeRehashForKvsHashtable(kvstore *kvs, int didx) {
    hashtable *ht = kvstoreGetHashtable(kvs, didx);
    if (ht != NULL) hashtableResumeRehashing(ht);
}


/* DictType for SDS->ptr.  The SDS is referenced, no destructor. */
static dictType sdsrefToPtrDictType = {
    .entryGetKey = dictEntryGetKey,
    .hashFunction = dictSdsHash,
    .keyCompare = dictSdsKeyCompare,
    .entryDestructor = zfree};


/* Wrap decrRefCount() so that it can be used as a callback requiring void. */
static void decrRefCountVoid(void *o) {
    decrRefCount(o);
}


/* Concatenate argc/argv into a command string for debugging. */
static sds createSdsFromClientArgv(int argc, robj **argv) {
    sds cmd = sdsempty();
    for (int i = 0; i < argc; i++) {
        robj *arg = getDecodedObject(argv[i]); // some objects are int encoded
        cmd = sdscatprintf(cmd, "'%s' ", (char *)objectGetVal(arg));
        decrRefCount(arg);
    }
    return cmd;
}


// ###########################################################################


/* bgIteration internal (compile time) configuration values */
enum {
    BGITER_EARLY_ITERATE_DICT_INITIAL_SIZE = 16384,  // Prevent initial rehashing
    BGITER_MAX_CLONE_ITEM_BYTES = 512,               // Max size item to clone
    BGITER_MAX_CLONE_POOL_BYTES = (1 * 1024 * 1024), // Total limit for all cloned items
    BGITER_QUEUE_INCREASE_INCR = 100,                // Step size when increasing queue target
    BGITER_QUEUE_MAX_LENGTH = 10000,                 // Max length for the dynamic queue
    BGITER_CYCLE_DELAY_MS = 2,                       // Delay between calls on bgIteration timer
    BGITER_CYCLE_BUDGET_MS = 1,                      // Normal time limit for timer processing
    BGITER_CYCLE_BUDGET_MAX_MS = 10                  // Maximum time limit when starvation seen
};

// dbEntry metadata
typedef struct {
    uint32_t iterator_epoch; // iterator epoch of last modification
} bgIterationEntryMetadata;
static_assert(sizeof(bgIterationEntryMetadata) == BGITERATION_ENTRY_METADATA_SIZE, "");


// These can be tweaked by unit tests
static int bgiter_max_clone_item_bytes = BGITER_MAX_CLONE_ITEM_BYTES;
static int bgiter_max_clone_pool_bytes = BGITER_MAX_CLONE_POOL_BYTES;

void bgIteration_unitTestDisableCloning(void) {
    bgiter_max_clone_item_bytes = 0;
    bgiter_max_clone_pool_bytes = 0;
}
void bgIteration_unitTestEnableCloning(int item_bytes, int pool_bytes) {
    bgiter_max_clone_item_bytes = item_bytes;
    bgiter_max_clone_pool_bytes = pool_bytes;
}

typedef enum {
    BGITERATION_TYPE_NONE,
    BGITERATION_TYPE_FULLSCAN,
    BGITERATION_TYPE_CLUSTERSLOT
} bgIterationType;


/* Flag indicates that a consistent iteration is required.  This is used to create a point-in-time
 * iteration.  The iteration client will see all keys AS THEY EXISTED at the time when the iterator
 * was created.
 * Note:  The DBID provided with the DICTENTRY events is the original DBID (at the time of iteration
 *        start).  SWAPDB events are NOT provided during a consistent iteration.  */
#define BGITERATOR_FLAG_CONSISTENT (1 << 0)

/* Flag indicating that the replication stream for keys which have already been processed should be
 * forwarded to the iteration client.  Used for non-consistent iteration to track changes
 * to keys already processed.  By tracking changes, this allows an non-consistent iteration client
 * to achieve a consistent view at the END of the iteration.
 * NOTE:  Replication events will be provided ordered and synchronized with any SWAPDB events. */
#define BGITERATOR_FLAG_REPLICATION (1 << 1)


/* Extensions to bgIteratorItemType.  These enumerations are used internally, and are not part of
 *  the published interface.  These allow for extensibility in the internal information-passing
 *  between the Valkey main thread and the iteration client thread. */
typedef enum {
    /* Indicates that the iteration client has completed use of the bgIterator and that the
     * bgIterator should be cleaned up and freed by the Valkey main thread. */
    BGITERATOR_ITEMEXT_ITER_CLOSED = 10
} bgIteratorItemTypeExtended;

// Static bgIterator items for items which carry no data
static const bgIteratorItem STATIC_ITEM_TERMINATED = {.type = (bgIteratorItemType)BGITERATOR_ITEM_TERMINATED};
static const bgIteratorItem STATIC_ITEM_ITER_CLOSED = {.type = (bgIteratorItemType)BGITERATOR_ITEMEXT_ITER_CLOSED};


/* A dictionary with a pointer (itself) as a key (the address pointed to is NOT referenced).
 * Nothing is duplicated, this is a very fast dictionary, but potentially unsafe if the original
 * items are deleted or moved.
 * WARNING:  This needs to maintain safety with things that may move the object.
 *   + In db.c, if the object is reallocated, bgIteration_updateDbEntryPtr() is called.
 *   + In defrag.c, we don't defrag if there are multiple references (and we incr the refcount). */

// Thomas Wang's 64-bit mix
static uint64_t pointerHash(const void *key) {
    uint64_t h = (uint64_t)(uintptr_t)key;
    h = (~h) + (h << 21); // h = (h << 21) - h - 1;
    h = h ^ (h >> 24);
    h = (h + (h << 3)) + (h << 8); // h * 265
    h = h ^ (h >> 14);
    h = (h + (h << 2)) + (h << 4); // h * 21
    h = h ^ (h >> 28);
    h = h + (h << 31);
    return h;
}

static int pointerCompare(const void *key1, const void *key2) {
    return key1 == key2;
}

// This dict grows and shrinks constantly during the iteration.  Avoid constant rehashing.
static int onlyAllowExpansion(size_t moreMem, double usedRatio) {
    UNUSED(moreMem);
    return (usedRatio > 0.5); // Return true only if expanding
}

static dictType dictEntryPtrDictType = {
    .entryGetKey = dictEntryGetKey,
    .hashFunction = pointerHash,
    .keyCompare = pointerCompare,
    .resizeAllowed = onlyAllowExpansion,
    .entryDestructor = zfree};

static hashtableType dbEntryPtrHashtableType = {
    .hashFunction = pointerHash,
    .keyCompare = pointerCompare,
    .resizeAllowed = onlyAllowExpansion};


// A free list for bgIteratorItem's - avoids churning zmalloc calls
typedef struct itemListNode {
    struct itemListNode *next;
} itemListNode;

static const int FREE_ITEM_MAX = 500;
static itemListNode *freeItemStackHead = NULL;
static int freeItemStackCount = 0;

static void itemFreeList_returnItemBackToFreeList(bgIteratorItem *item) {
    itemListNode *freedNode = (itemListNode *)item;
    if (freeItemStackCount < FREE_ITEM_MAX) {
        freedNode->next = freeItemStackHead;
        freeItemStackHead = freedNode;
        freeItemStackCount++;
    } else {
        zfree(freedNode);
    }
}

// Pop a free node from the free list or allocate if none free
static bgIteratorItem *itemFreeList_getElementOrAllocate(void) {
    bgIteratorItem *item;
    if (freeItemStackHead) {
        item = (bgIteratorItem *)freeItemStackHead;
        freeItemStackHead = freeItemStackHead->next;
        freeItemStackCount--;
        if (freeItemStackHead) valkey_prefetch(freeItemStackHead);
    } else {
        serverAssert(freeItemStackCount == 0);
        // Create new listNode and item
        item = zmalloc(sizeof(bgIteratorItem));
    }
    return item;
}

static void itemFreeList_release(void) {
    while (freeItemStackHead) {
        itemListNode *node = freeItemStackHead;
        freeItemStackHead = node->next;
        freeItemStackCount--;
        zfree(node);
    }
    serverAssert(freeItemStackCount == 0);
}


/* A TEMPORARY set of robj's (of type sds).  This is only for temporary sets as the robj's are not
 * ref-counted at insertion/deletion. */
static hashtableType tempKeysetHashtableType = {
    .hashFunction = dictObjHash,
    .keyCompare = dictObjKeyCompare};


typedef struct genericIterator genericIterator;
typedef void (*iteratorReleaseFunc)(genericIterator *genIt);
typedef fifo *(*iteratorGetEntriesFunc)(genericIterator *genIt, int *orig_dbid, int *cur_dbid);
typedef void (*iteratorSwapDbFunc)(genericIterator *genIt, int db1, int db2);
typedef void (*iteratorFlushDbFunc)(genericIterator *genIt, int cur_dbid);
typedef bool (*iteratorHasPassedItemFunc)(genericIterator *genIt, const_sds key, int cur_dbid);
typedef int (*iteratorOriginalDbFunc)(genericIterator *genIt, int cur_dbid);
typedef bool (*iteratorIsKeyInScopeFunc)(genericIterator *genIt, const_sds key);

// Function pointers supporting polymorphic iterator implementation
struct genericIterator {
    iteratorReleaseFunc release;
    iteratorGetEntriesFunc getEntries;
    iteratorSwapDbFunc swapDb;
    iteratorFlushDbFunc flushDb;
    iteratorHasPassedItemFunc hasPassedItem;
    iteratorOriginalDbFunc originalDb;
    iteratorIsKeyInScopeFunc isKeyInScope;
};


/* This struct is used across threads.  Unless otherwise noted, the fields are initialized at
 *  iterator creation (within the main thread) and are read-only by the client thread. */
struct bgIterator {
    sds name;                        // Iterator name
    bgIteratorReplDoneFunc repldone; // Optional repldone function to be run on the main thread
    bgIteratorCleanupFunc cleanup;   // Optional cleanup function to be run on main thread
    void *privdata;                  // Client's private data to be passed to cleanup function

    int iteration_flags;                 // Consistent and/or Replication
    int iteration_type;                  // Full scan or cluster slot
    uint32_t consistent_modification_id; // iterator epoch at time of iterator creation

    genericIterator *keyset_iter; // Low-level iterator (polymorphic)

    /* A set of dbEntry, compared by pointer.  Used to track items which have already been iterated
     * over by out-of-order expedited processing.  Ensures a bgIterator does not try to reprocess
     * items.  Used only by main thread. */
    hashtable *early_iterate_entries;

    mutexQueue *items_for_iterator; // Created/Destroyed in main thread, used in both (threadsafe)

    mutexQueue *return_to_main_thread; // Queue of items to be returned to the Valkey main thread (threadsafe)

    unsigned int item_count_target; // Used only by main thread

    bgIteratorItem *current_item; // Used in client thread, validated in main after iterator complete

    bool client_is_active; // Set to true when client performs 1st read

    /* Set to true in main thread when last item from iteration has been queued to the client.  No
     * additional items will be enqueued to the client after this has been set. */
    bool completed;

    /* Set to true in main thread when iteration is to be killed.
     * Set to true in iteration client when it decides to end early. */
    volatile bool terminated;

    bool cur_cmd_may_replicate; // Used only in main thread during command processing

    // Variables maintaining runtime statistics
    unsigned long dbentries_queued;         // Updated by main thread
    unsigned long dbentries_processed;      // Updated by client thread
    unsigned long replication_queued;       // Updated by main thread
    unsigned long replication_processed;    // Updated by client thread
    unsigned long swapdb_queued;            // Updated by main thread
    unsigned long swapdb_processed;         // Updated by client thread
    unsigned long flushdb_queued;           // Updated by main thread
    unsigned long flushdb_processed;        // Updated by client thread
    unsigned long dbentry_clones_queued;    // Updated by main thread
    unsigned long dbentry_clones_processed; // Updated by client thread
    monotime monotonic_start_time;          // Time iteration started

    /* FLUSHDB and SWAPDB are special in that they affect all keys.  When expediting a key, it's
     * preferable to put it at the front of the queue.  However, if there is a FLUSHDB or SWAPDB in
     * the queue, we must maintain strict ordering.
     * This value is equivalent to (flushdb_queued-flushdb_processed)+(swapdb_queued-swapdb_processed) */
    int barrier_items;

    /* The item start time is set in the iteration client.  It is marked volatile as it can be read
     * from the main thread by bgIteratorGetStatus.  If 0, this indicates that the iteration client
     * is waiting for an item to process. */
    volatile monotime monotonic_item_start_time;
};


// These static values are only accessed from the main Valkey thread.

static list *allIterators;   // list of bgIterator
static dict *nameToIterator; // bgIterator->name -> bgIterator

// Global, across all iterators, dict contains a dbEntry pointer -> ref count
static dict *inUseEntries; // dbEntry -> ref count

/* Key values in the current command which don't exist in the DB yet.  Needed for determination of
 * replication for NON-consistent iterations. */
static list *curCmdMissingKeys; // list of robj

/* A counter of the total amount of memory used for buffered replication data.  This amount is
 * excluded when computing the need for evictions. */
static ssize_t bufferedReplicationBytes;

// Memory pool to track current allocated memory of cloned items (in bytes)
static ssize_t bgiteration_current_clone_memory_pool_size;

/* Snapshot of the last queue size to seed the next queue.  We assume all bgIterators consume items
 * at roughly the same rate. */
static int last_item_count_target;

// Eventloop ID of the timerproc (or AE_DELETED_EVENT_ID)
static long long bgIterator_timeproc_id;

// Incremented on each new iteration, this is updated in dbEntry metadata whenever an entry is modified.
static uint32_t bgIteration_epoch = 1;

/* If true, the iterators' cur_cmd_may_replicate flag was determined in the last call to
 * blockClientIfRequired.  Otherwise, we skipped over computing this flag (maybe because it was a
 * READ command).
 * If this is true, AND we are in the context of executing a command inside of call(), then we
 * should respect the setting of cur_cmd_may_replicate. */
static bool iteratorReplicationFlagsWereUpdated;

/* When a key is deleted (expire/evict):
 *   1. bgIteration_keyDelete() is called
 *   2. the key is physically deleted
 *   3. replication is generated
 * At the time of replication, we need the (deleted) dbEntry pointer to be able to check
 * early_iterated_entries.  This variable stores the pointer from the last call of keyDelete() */
static dbEntry *dbEntryPtrOfLastKeyDelete;

/* BgIteration debug captures BgIteration activity to a large sds buffer.  When an iterator is
 * completed, the entire buffer is written to a file in the current working directory.  Note that
 * memory must be available for the ENTIRE debug in memory.  This isn't captured incrementally to
 * a file as the file I/O is more likely to affect timing.
 *
 * Future implementation: the current design is most useful for a single iterator.  When items are
 * queued to an iterator, the iterator name is not recorded (to save space).
 *
 * Developer note: using a CONST value here allows the compiler to completely remove all of the
 * debugging code at compile time.  There is no run-time performance overhead when set to FALSE.
 * This is essentially like an IFDEF, however, it's better as it forces the compiler to validate
 * syntax. */
static const bool BGITERATION_DEBUG = false; // DO NOT SUBMIT WITH THIS SYMBOL SET TO TRUE!
static sds debugBuffer;


/* =============================================================================================
 *                        Full Scan Iterator
 * =============================================================================================
 * The full scan iterator performs the actual iteration over the Valkey keyset.  The iterator is
 * only used from within the Valkey main thread.  Iteration proceeds one DB at a time, based on
 * the DB ordering at the time of iterator creation.  Each time the iterator returns items, all
 * of the dictionary entries from a single hash bucket are returned. */

struct fullScanIterator {
    genericIterator callbacks; // (must be first item)

    /* Array of mapping from original DB ID (at the time of iteration start) to that DB's current
     * index.  So, if the DB which was DB-0 is now at index 6, orig_to_cur_db[0]==6. */
    int *orig_to_cur_db;

    /* The reverse of the above array.  This maps a current DB index to its original index (at the
     * time of iteration start). */
    int *cur_to_orig_db;

    /* This is the DB we are currently iterating over.  This is relative to the ORIGINAL DB
     * ordering, at the time of iterator creation.  Iteration proceeds from 0..N based on the
     * original ordering. */
    int iter_db;

    // Iterator for the DB orig_to_cur_db[iter_db]
    kvstore *kvs;     // keep track of kvs associated with iter_dbi
    int kvs_didx;     // hashtable index within the kvstore
    size_t ht_cursor; // cursor for scanning hashtable
};

static void fullScanIteratorRelease(genericIterator *genIt) {
    struct fullScanIterator *it = (struct fullScanIterator *)genIt;
    if (it->kvs) resumeRehashForKvsHashtable(it->kvs, it->kvs_didx);
    zfree(it->orig_to_cur_db);
    zfree(it->cur_to_orig_db);
    zfree(it);
}

/* Scan callback used by fullScanIteratorGetEntries2 to collect entries into a fifo. */
static void fullScanIteratorScanCallback(void *privdata, void *entry) {
    fifo *dbEntryFifo = (fifo *)privdata;
    dbEntry *de = (dbEntry *)entry;
    fifoPush(dbEntryFifo, de);
}

static fifo *fullScanIteratorGetEntries(genericIterator *genIt, int *orig_dbid, int *cur_dbid) {
    struct fullScanIterator *it = (struct fullScanIterator *)genIt;
    if (it->iter_db >= server.dbnum) return NULL; // Finished scanning

    fifo *dbEntryFifo = fifoCreate();
    while (fifoLength(dbEntryFifo) == 0) {
        while (it->kvs == NULL) {
            if (++it->iter_db >= server.dbnum) {
                fifoRelease(dbEntryFifo);
                return NULL; // Iteration complete
            }
            serverDb *db = server.db[it->orig_to_cur_db[it->iter_db]];
            if (db != NULL) {
                it->kvs = db->keys;
                it->kvs_didx = kvstoreGetFirstNonEmptyHashtableIndex(it->kvs);
                it->ht_cursor = 0;
                if (it->kvs_didx == KVSTORE_INDEX_NOT_FOUND) it->kvs = NULL;
                if (it->kvs != NULL) pauseRehashForKvsHashtable(it->kvs, it->kvs_didx);
            }
        }

        hashtable *ht = kvstoreGetHashtable(it->kvs, it->kvs_didx);
        if (ht) {
            it->ht_cursor = hashtableScan(ht, it->ht_cursor, fullScanIteratorScanCallback, dbEntryFifo);
        } else {
            it->ht_cursor = 0;
        }

        if (it->ht_cursor == 0) {
            /* Done with this hashtable, move to next. */
            resumeRehashForKvsHashtable(it->kvs, it->kvs_didx);
            it->kvs_didx = kvstoreGetNextNonEmptyHashtableIndex(it->kvs, it->kvs_didx);
            if (it->kvs_didx == KVSTORE_INDEX_NOT_FOUND) it->kvs = NULL;
            if (it->kvs != NULL) pauseRehashForKvsHashtable(it->kvs, it->kvs_didx);
        }
    }
    *orig_dbid = it->iter_db;
    *cur_dbid = it->orig_to_cur_db[*orig_dbid];
    return dbEntryFifo;
}

static void fullScanIteratorSwapDb(genericIterator *genIt, int db1, int db2) {
    struct fullScanIterator *it = (struct fullScanIterator *)genIt;
    int temp = it->cur_to_orig_db[db1];
    it->cur_to_orig_db[db1] = it->cur_to_orig_db[db2];
    it->cur_to_orig_db[db2] = temp;

    it->orig_to_cur_db[it->cur_to_orig_db[db1]] = db1;
    it->orig_to_cur_db[it->cur_to_orig_db[db2]] = db2;
}

static void fullScanIteratorFlushDb(genericIterator *genIt, int cur_dbid) {
    struct fullScanIterator *it = (struct fullScanIterator *)genIt;
    int orig_db = (cur_dbid == -1) ? it->iter_db : it->cur_to_orig_db[cur_dbid];
    if (orig_db == it->iter_db) {
        // We are currently iterating on the DB that's being flushed.
        if (it->kvs) {
            // If it->kvs is set, we're actively scanning and have paused rehash
            resumeRehashForKvsHashtable(it->kvs, it->kvs_didx);
            it->kvs = NULL;
        }
        // Iteration will continue with the next DB.
    }
}

static bool fullScanIteratorHasPassedItem(genericIterator *genIt, const_sds key, int cur_dbid) {
    struct fullScanIterator *it = (struct fullScanIterator *)genIt;
    int orig_dbid = it->cur_to_orig_db[cur_dbid];

    if (orig_dbid < it->iter_db) return true;  // Entire DB has already been processed
    if (orig_dbid > it->iter_db) return false; // Haven't started this DB yet
    // Now, orig_dbid == it->iter_db

    if (it->kvs == NULL) return true; // just finished this DB

    /* We're in the middle of processing a DB.  In cluster-mode, the DB is divided into 1 hashtable
     * per slot.  In cluster-mode-disabled, we treat all keys as in slot 0. */
    int keySlot = server.cluster_enabled ? getKVStoreIndexForKey((sds)key) : 0;
    if (keySlot < it->kvs_didx) return true;
    if (keySlot > it->kvs_didx) return false;

    // At this point, we're down to a specific hashtable.

    hashtable *ht = kvstoreGetHashtable(it->kvs, keySlot);
    if (hashtableScanHasPassedKey(ht, key, it->ht_cursor)) return true;

    return false;
}

static int fullScanIteratorOriginalDb(genericIterator *genIt, int cur_dbid) {
    struct fullScanIterator *it = (struct fullScanIterator *)genIt;
    return it->cur_to_orig_db[cur_dbid];
}

static bool fullScanIteratorIsKeyInScope(genericIterator *genIt, const_sds key) {
    UNUSED(genIt);
    UNUSED(key);
    return true; // All keys are in scope
}

static genericIterator *fullScanIteratorCreate(void) {
    struct fullScanIterator *it = zmalloc(sizeof(struct fullScanIterator));
    it->orig_to_cur_db = zmalloc(sizeof(int) * server.dbnum);
    it->cur_to_orig_db = zmalloc(sizeof(int) * server.dbnum);
    for (int i = 0; i < server.dbnum; i++) {
        it->orig_to_cur_db[i] = i;
        it->cur_to_orig_db[i] = i;
    }
    it->iter_db = -1;
    it->kvs = NULL;

    it->callbacks.release = fullScanIteratorRelease;
    it->callbacks.getEntries = fullScanIteratorGetEntries;
    it->callbacks.swapDb = fullScanIteratorSwapDb;
    it->callbacks.flushDb = fullScanIteratorFlushDb;
    it->callbacks.hasPassedItem = fullScanIteratorHasPassedItem;
    it->callbacks.originalDb = fullScanIteratorOriginalDb;
    it->callbacks.isKeyInScope = fullScanIteratorIsKeyInScope;

    return (genericIterator *)it;
}


/* =============================================================================================
 *                        Cluster Slot Iterator
 * =============================================================================================
 * The cluster slot iterator performs iteration over one cluster slot of the Valkey keyset.  The
 * iterator is only used from within the Valkey main thread. */
struct clusterSlotIterator {
    genericIterator callbacks; // (must be first item)
};

static void clusterSlotIteratorRelease(genericIterator *genIt) {
    UNUSED(genIt);
    serverAssert(false); // Not yet implemented
}

static fifo *clusterSlotIteratorGetEntries(genericIterator *genIt, int *orig_dbid, int *cur_dbid) {
    UNUSED(genIt);
    UNUSED(orig_dbid);
    UNUSED(cur_dbid);
    serverAssert(false); // Not yet implemented
}

static void clusterSlotIteratorSwapDb(genericIterator *genIt, int db1, int db2) {
    UNUSED(genIt);
    UNUSED(db1);
    UNUSED(db2);
    serverAssert(false); // swap not valid in cluster mode
}

static void clusterSlotIteratorFlushDb(genericIterator *genIt, int cur_dbid) {
    UNUSED(genIt);
    UNUSED(cur_dbid);
    serverAssert(false); // Not yet implemented
}

static bool clusterSlotIteratorHasPassedItem(genericIterator *genIt, const_sds key, int cur_dbid) {
    UNUSED(genIt);
    UNUSED(key);
    UNUSED(cur_dbid);
    serverAssert(false); // Not yet implemented
}

static int clusterSlotIteratorOriginalDb(genericIterator *genIt, int cur_dbid) {
    UNUSED(genIt);
    UNUSED(cur_dbid);
    return cur_dbid; // swap not supported in cluster mode
}

/* When checking if a command is in scope for this iterator, all of its keys should be either in
 * scope or not. In cluster mode enabled a command cannot reference keys from different slots, so
 * this assumption will always be true. */
static bool clusterSlotIteratorIsKeyInScope(genericIterator *genIt, const_sds key) {
    UNUSED(genIt);
    UNUSED(key);
    serverAssert(false); // Not yet implemented
}

static genericIterator *clusterSlotIteratorCreate(const int *slots, size_t slots_count) {
    struct clusterSlotIterator *it = zmalloc(sizeof(struct clusterSlotIterator));
    it->callbacks.release = clusterSlotIteratorRelease;
    it->callbacks.getEntries = clusterSlotIteratorGetEntries;
    it->callbacks.swapDb = clusterSlotIteratorSwapDb;
    it->callbacks.flushDb = clusterSlotIteratorFlushDb;
    it->callbacks.hasPassedItem = clusterSlotIteratorHasPassedItem;
    it->callbacks.originalDb = clusterSlotIteratorOriginalDb;
    it->callbacks.isKeyInScope = clusterSlotIteratorIsKeyInScope;

    UNUSED(slots);
    UNUSED(slots_count);
    serverAssert(false); // Not yet implemented

    return (genericIterator *)it;
}


/* =============================================================================================
 *                        General iteration support (across all iterators)
 * ============================================================================================= */

/* While an item is potentially in use by a background thread, we can't have rehashing by the main
 * thread.  Returns true if rehashing was paused. */
static bool pauseRehashing(dbEntry *de) {
    switch (de->encoding) {
    case OBJ_ENCODING_HASHTABLE: { // SET or HASH
        hashtable *ht = objectGetVal(de);
        hashtablePauseRehashing(ht);
        return true;
    }
    case OBJ_ENCODING_BTREE: { // SORTED SET
        zset *zs = objectGetVal(de);
        hashtablePauseRehashing(zs->ht);
        return true;
    }
    default:
        return false;
    }
}

static void resumeRehashing(dbEntry *de) {
    switch (de->encoding) {
    case OBJ_ENCODING_HASHTABLE: { // SET or HASH
        hashtable *ht = objectGetVal(de);
        hashtableResumeRehashing(ht);
        break;
    }
    case OBJ_ENCODING_BTREE: { // SORTED SET
        zset *zs = objectGetVal(de);
        hashtableResumeRehashing(zs->ht);
        break;
    }
    default:
        break;
    }
}

// Maintain a list of entries which are currently in-use.  These items should not be modified.
static void incrementEntryInuse(dbEntry *de) {
    dictEntry *existingEntry;
    dictEntry *newEntry = dictAddRaw(inUseEntries, de, &existingEntry);
    if (newEntry) {
        incrRefCount(de);
        dictSetSignedIntegerVal(newEntry, 1);
    } else {
        dictSetSignedIntegerVal(existingEntry, dictGetSignedIntegerVal(existingEntry) + 1);
    }
}


static void decrementEntryInuse(dbEntry *de) {
    dictEntry *entry = dictFind(inUseEntries, de);
    if (dictGetSignedIntegerVal(entry) == 1) {
        dictDelete(inUseEntries, de);
        decrRefCount(de);
    } else {
        serverAssert(dictGetSignedIntegerVal(entry) > 1);
        dictSetSignedIntegerVal(entry, dictGetSignedIntegerVal(entry) - 1);
    }
}

static bool isEntryInuseBySingleIterator(dbEntry *de) {
    dictEntry *entry = dictFind(inUseEntries, de);
    return dictGetSignedIntegerVal(entry) == 1;
}

static bool isEntryInuseByAnyIterator(dbEntry *de) {
    return (dictFind(inUseEntries, de) != NULL);
}


static ssize_t computeStringDbEntrySize(dbEntry *de) {
    sds key = objectGetKey(de);
    size_t valueSize = stringObjectLen(de);

    return sdslen(key) + valueSize; // ignore the rest of the overhead, it's minor & transient
}


static dbEntry *tryCloneDbEntry(dbEntry *de) {
    if (bgiteration_current_clone_memory_pool_size + bgiter_max_clone_item_bytes >
        bgiter_max_clone_pool_bytes) return NULL;

    /* Future optimization: Incorporate small ziplists, sorted sets, etc.
     * OBJ_ENCODING_INT is omitted only because there isn't a good API for cloning it yet. */
    if (de->type == OBJ_STRING && de->encoding != OBJ_ENCODING_INT) {
        ssize_t itemSize = computeStringDbEntrySize(de);

        if (itemSize <= bgiter_max_clone_item_bytes) {
            bgiteration_current_clone_memory_pool_size += itemSize;
            dbEntry *clone = createStringObjectWithKeyAndExpire((char *)objectGetVal(de),
                                                                sdslen(objectGetVal(de)),
                                                                objectGetKey(de),
                                                                objectGetExpire(de));
            ((bgIterationEntryMetadata *)objectGetMetadata(clone))->iterator_epoch =
                ((bgIterationEntryMetadata *)objectGetMetadata(de))->iterator_epoch;
            return clone;
        }
    }

    return NULL;
}

static void freeClonedDictEntry(dbEntry *clonedEntry) {
    serverAssert(clonedEntry->type == OBJ_STRING);

    bgiteration_current_clone_memory_pool_size -= computeStringDbEntrySize(clonedEntry);

    decrRefCount(clonedEntry);
}


static bgIteratorItem *makeDbEntryItem(dbEntry *de, int dbid, bool isCloned) {
    if (!isCloned) incrementEntryInuse(de);

    bgIteratorItem *item = itemFreeList_getElementOrAllocate();
    item->type = BGITERATOR_ITEM_DBENTRY;
    item->dbid = dbid;
    item->u.dbe.de = de;
    item->u.dbe.is_cloned = isCloned;
    item->u.dbe.is_rehashing_paused = pauseRehashing(de);

    return item;
}

static robj **cloneRobjArray(int argc, robj **argv) {
    robj **newarray = zmalloc(sizeof(robj *) * argc);
    for (int i = 0; i < argc; i++) {
        newarray[i] = argv[i];
        incrRefCount(argv[i]);
    }
    return newarray;
}


static void freeRobjArray(int argc, robj **argv) {
    for (int i = 0; i < argc; i++) {
        decrRefCount(argv[i]);
    }
    zfree(argv);
}


// Called by iterator thread to release an item.
static void returnCurrentItemToMainThread(bgIterator *it) {
    bgIteratorItem *item = it->current_item;
    if (item == NULL) return;

    switch (item->type) {
    case BGITERATOR_ITEM_DBENTRY:
        it->dbentries_processed++;
        if (item->u.dbe.is_cloned) it->dbentry_clones_processed++;
        mutexQueueAdd(it->return_to_main_thread, item);
        break;
    case BGITERATOR_ITEM_REPLICATION:
        it->replication_processed++;
        mutexQueueAdd(it->return_to_main_thread, item);
        break;
    case BGITERATOR_ITEM_SWAPDB:
        it->swapdb_processed++;
        mutexQueueAdd(it->return_to_main_thread, item);
        break;
    case BGITERATOR_ITEM_FLUSHDB:
        it->flushdb_processed++;
        mutexQueueAdd(it->return_to_main_thread, item);
        break;

    case BGITERATOR_ITEM_COMPLETE:
    case BGITERATOR_ITEM_TERMINATED:
        // These are static and just used to wake the iterator - they should never be returned.
        serverAssert(false);
        break;

    default:
        serverAssert(false);
    }

    it->current_item = NULL;
}


/* =============================================================================================
 *                        Background Iterator (private)
 * ============================================================================================= */

static void bgIteratorRelease(bgIterator *it) {
    serverAssert(hasMainThreadExclusivity());
    serverAssert(it->current_item == NULL);
    serverAssert(mutexQueueLength(it->items_for_iterator) == 0);
    serverAssert(mutexQueueLength(it->return_to_main_thread) == 0);

    dictDelete(nameToIterator, it->name);
    listDelNode(allIterators, listSearchKey(allIterators, it));

    mutexQueueRelease(it->items_for_iterator);
    it->items_for_iterator = NULL;

    mutexQueueRelease(it->return_to_main_thread);
    it->return_to_main_thread = NULL;

    it->keyset_iter->release(it->keyset_iter);
    it->keyset_iter = NULL;

    hashtableRelease(it->early_iterate_entries);
    it->early_iterate_entries = NULL;

    sdsfree(it->name);
    zfree(it);
}


static bool shouldFeedIteratorMore(bgIterator *it) {
    return (!it->completed &&
            !it->terminated &&
            mutexQueueLength(it->items_for_iterator) < it->item_count_target);
}


// Debugging routine
static sds createEntryString(int dbid, dbEntry *de) {
    sds key = objectGetKey(de);

    sds entrySds = sdsempty();
    entrySds = sdscatprintf(entrySds, "(%d)'%s'", dbid, key);
    if (de->type == OBJ_STRING) {
        robj *o = getDecodedObject(de); // might be encoded as int
        const unsigned valuePrintLen = 20;
        entrySds = sdscatprintf(entrySds, " : '%.*s'", valuePrintLen, (char *)objectGetVal(o));
        if (sdslen((sds)objectGetVal(o)) > valuePrintLen) entrySds = sdscat(entrySds, "...");
        decrRefCount(o);
    } else {
        entrySds = sdscatprintf(entrySds, " : type(%d)", de->type);
    }
    return entrySds;
}


static void feedIterator(bgIterator *it, monotime end_time_us) {
    unsigned int initial_queue_len = mutexQueueLength(it->items_for_iterator);

    /* The queue size dynamically adjusts using an AIMD approach.  If we have left over stuff from
     * the prior call to feedIterator, reduce by half the remaining size.  If the queue ran dry
     * and we have time left (at the end of this function), additively increase the queue length. */
    if (initial_queue_len > 2 && it->item_count_target >= initial_queue_len) {
        it->item_count_target -= initial_queue_len / 2;
    }

    // Now do some feeding
    bool have_time = (getMonotonicUs() < end_time_us);
    int timeCheckCounter = 0;
    while (shouldFeedIteratorMore(it) && have_time) {
        int orig_dbid, cur_dbid;
        fifo *dbEntryFifo = it->keyset_iter->getEntries(it->keyset_iter, &orig_dbid, &cur_dbid);

        if (dbEntryFifo == NULL) {
            // Iteration of items is complete for this iterator
            serverAssert(it->dbentries_queued >= it->dbentries_processed);
            serverAssert(it->replication_queued >= it->replication_processed);
            serverAssert(it->swapdb_queued >= it->swapdb_processed);
            serverAssert(it->flushdb_queued >= it->flushdb_processed);
            serverAssert(it->dbentry_clones_queued >= it->dbentry_clones_processed);

            // Snapshot queue size to seed next iterator when terminated
            last_item_count_target = it->item_count_target;

            if (it->iteration_flags & BGITERATOR_FLAG_REPLICATION) {
                if (!it->client_is_active || (it->dbentries_queued > it->dbentries_processed)) {
                    /* Even though we have sent all of the dbEntries, we continue sending
                     * replication until the iterator has consumed all of the dbEntries.
                     * client_is_active prevents race conditions in the case of an empty DB. */
                    break;
                }
                if (it->repldone) {
                    bool clientWantsMoreReplication = (!it->repldone(it->privdata));
                    if (clientWantsMoreReplication) break;
                }
            }
            bgIteratorItem *completionItem = itemFreeList_getElementOrAllocate();
            *completionItem = (bgIteratorItem){.type = BGITERATOR_ITEM_COMPLETE};
            if (it->iteration_flags & BGITERATOR_FLAG_REPLICATION) {
                rdbSaveInfo rsi;
                completionItem->dbid = (rdbPopulateSaveInfo(&rsi)) ? rsi.repl_stream_db : 0;
                completionItem->u.master_repl_offset = server.primary_repl_offset;
                if (BGITERATION_DEBUG) {
                    debugBuffer = sdscat(debugBuffer, "REPLDONE FN\n");
                }
            }

            if (BGITERATION_DEBUG) {
                debugBuffer = sdscat(debugBuffer, "SENDING COMPLETE\n");
            }

            mutexQueueAdd(it->items_for_iterator, completionItem);
            it->completed = true;
            break;
        }

        int dbid = (it->iteration_flags & BGITERATOR_FLAG_CONSISTENT) ? orig_dbid : cur_dbid;

        fifo *itemsToAdd = fifoCreate();
        while (fifoLength(dbEntryFifo) > 0) {
            dbEntry *de;
            fifoPop(dbEntryFifo, (void **)&de);

            // Remove new/modified items during consistent iteration.
            if (it->iteration_flags & BGITERATOR_FLAG_CONSISTENT &&
                ((bgIterationEntryMetadata *)objectGetMetadata(de))->iterator_epoch > it->consistent_modification_id) {
                continue;
            }

            // Remove any items which have been processed early
            if (hashtableDelete(it->early_iterate_entries, de)) {
                if (BGITERATION_DEBUG) {
                    sds entryString = createEntryString(dbid, de);
                    debugBuffer = sdscatprintf(debugBuffer, "SKIPPING ITEM(early iterate): %s\n", entryString);
                    sdsfree(entryString);
                }
                continue;
            }

            // For items which are left, convert them from dbEntry to iteratorItem
            if (BGITERATION_DEBUG) {
                sds entryString = createEntryString(dbid, de);
                debugBuffer = sdscatprintf(debugBuffer, "ITEM: %s\n", entryString);
                sdsfree(entryString);
            }

            bgIteratorItem *item = makeDbEntryItem(de, dbid, false);
            fifoPush(itemsToAdd, item);
        }
        fifoRelease(dbEntryFifo);

        if (fifoLength(itemsToAdd) > 0) {
            it->dbentries_queued += fifoLength(itemsToAdd);
            mutexQueueAddMultiple(it->items_for_iterator, itemsToAdd);
        }
        fifoRelease(itemsToAdd);

        // This is a predictably fast loop.  We don't need to check the time on every pass.
        if (++timeCheckCounter % 32 == 0) {
            have_time = (getMonotonicUs() < end_time_us);
        }
    }

    // Smart logic to dynamically adjust the size of the queue
    if (initial_queue_len == 0 && have_time && it->item_count_target < BGITER_QUEUE_MAX_LENGTH) {
        it->item_count_target += BGITER_QUEUE_INCREASE_INCR;
    }
}


static bool addEarlyIterationKey(bgIterator *it, dbEntry *earlyEntry, int cur_dbid) {
    bool wasAdded = hashtableAdd(it->early_iterate_entries, earlyEntry);
    serverAssert(wasAdded);

    int dbid = (it->iteration_flags & BGITERATOR_FLAG_CONSISTENT)
                   ? it->keyset_iter->originalDb(it->keyset_iter, cur_dbid)
                   : cur_dbid;

    dbEntry *cloneEntry = tryCloneDbEntry(earlyEntry);
    bool isClonedEntry = (cloneEntry != NULL);
    bgIteratorItem *item = makeDbEntryItem(isClonedEntry ? cloneEntry : earlyEntry, dbid, isClonedEntry);

    it->dbentries_queued++;
    if (isClonedEntry) it->dbentry_clones_queued++;

    if (it->barrier_items == 0) {
        // If there are no barrier items, we can add the key right to the front of the queue.
        if (BGITERATION_DEBUG) {
            sds entryString = createEntryString(dbid, item->u.dbe.de);
            debugBuffer = sdscatprintf(debugBuffer, "EARLY_1: %s\n", entryString);
            sdsfree(entryString);
        }
        mutexQueuePushPriority(it->items_for_iterator, item);
    } else {
        // With barrier items, the key must be added to the end, and processed in order.
        if (BGITERATION_DEBUG) {
            sds entryString = createEntryString(dbid, item->u.dbe.de);
            debugBuffer = sdscatprintf(debugBuffer, "EARLY: %s\n", entryString);
            sdsfree(entryString);
        }
        mutexQueueAdd(it->items_for_iterator, item);
    }
    return !isClonedEntry; // Block if the entry will be used by the background thread
}


static bool iteratorHasPassedKey(bgIterator *it, int dbid, const_sds key, dbEntry *de) {
    if (it->completed || it->terminated) return true;

    if (it->keyset_iter->hasPassedItem(it->keyset_iter, key, dbid)) return true;

    if (de && hashtableFind(it->early_iterate_entries, de, NULL)) return true;

    return false;
}


// This expedites a single key and doesn't attempt to avoid expediting through optimization.
static bool expediteSingleKeyWithoutOptimization(bgIterator *it,
                                                 int dbid,
                                                 robj *oKey,
                                                 hashtable *waitingOnKeys) {
    bool mustBlock = false;

    sds key = objectGetVal(oKey);
    dbEntry *de = (server.db[dbid]) ? dbFind(server.db[dbid], key) : NULL;
    if (de != NULL) {
        if (!iteratorHasPassedKey(it, dbid, key, de)) {
            if (addEarlyIterationKey(it, de, dbid)) {
                mustBlock = true;
                hashtableAdd(waitingOnKeys, oKey);
            }
        } else {
            if (isEntryInuseByAnyIterator(de)) {
                mustBlock = true;
                hashtableAdd(waitingOnKeys, oKey);
            }
        }
    }

    return mustBlock;
}


// MOVE/COPY are unfortunate special commands.  They work on 2 DBs at once.
const int MOVE_COMMAND_DBID_ARG_INDEX = 2;
static bool expediteKeysForMove(bgIterator *it,
                                int dbid,
                                int argc,
                                robj **argv,
                                hashtable *waitingOnKeys) {
    if (argc <= MOVE_COMMAND_DBID_ARG_INDEX) return false;

    int destDbid;
    if (!getDbIdFromRobj(argv[MOVE_COMMAND_DBID_ARG_INDEX], &destDbid)) return false;

    bool mustBlock = false;
    robj *key = argv[1];

    /* Not looking for special cases to optimize here.  Just try to expedite both src and dest
     * keys.  Note that the dest key might exist (and need iteration) but could be expired and
     * could be overwritten by MOVE.  In this case, a DEL would replicate due to the expiry.  So
     * even if the target is expired, we need to replicate it before executing the command. */
    if (expediteSingleKeyWithoutOptimization(it, dbid, key, waitingOnKeys)) mustBlock = true;
    if (expediteSingleKeyWithoutOptimization(it, destDbid, key, waitingOnKeys)) mustBlock = true;

    it->cur_cmd_may_replicate = true;
    return mustBlock;
}


// MOVE/COPY are unfortunate special commands.  They work on 2 DBs at once.
static bool expediteKeysForCopy(bgIterator *it,
                                int dbid,
                                int argc,
                                robj **argv,
                                hashtable *waitingOnKeys) {
    int destDbid;
    if (!getTargetDbIdForCopyCommand(argc, argv, dbid, &destDbid)) return false;

    bool mustBlock = false;
    robj *srcKey = argv[1];
    robj *destKey = argv[2];

    /* Not trying to optimize COPY.  Just expedite source and destination (if it exists).  We
     * don't really care if the value is overwritten or not (so no need to parse REPLACE option). */
    if (expediteSingleKeyWithoutOptimization(it, dbid, srcKey, waitingOnKeys)) mustBlock = true;
    if (expediteSingleKeyWithoutOptimization(it, destDbid, destKey, waitingOnKeys)) mustBlock = true;

    it->cur_cmd_may_replicate = true;
    return mustBlock;
}


/* There are several cases where a client must be blocked on write operations.  (Clients never need
 * to be blocked for read operations.)
 *
 * Note:  The CMD_WRITE_FIRSTKEY_ONLY flag allows us to identify commands where the first key is for
 *        write and the rest are for read.  This allows us to make the following optimizations:
 *   - For keys which are read only, there's no need to block if the key is in-use by an iterator
 *   - Without replication, there's no need to immediately queue read keys on a consistent iteration
 *
 * Iterator:  CONSISTENT = NO,  REPLICATION = NO
 *   - Block if any write-key is in use by an iterator
 *
 * Iterator:  CONSISTENT = NO,  REPLICATION = YES
 *   - Block if any write-key is in use by an iterator
 *   - If ANY key has already been iterated (but some keys have not), then
 *       - Block and immediately queue any key (read or write) that has not
 *         already been iterated
 *         Example:  SDIFFSTORE KEY_A KEY_B KEY_C
 *           In this case, KEY_A is written, KEY_B and KEY_C are read.  If KEY_A has already been
 *           iterated over, the replication stream will contain this command.  The receiver of this
 *           replication will need KEY_B and KEY_C in order to process the replication stream.  So
 *           these need to be iterated and the client blocked.
 *
 * Iterator:  CONSISTENT = YES, REPLICATION = NO
 *   - Block if any write-key is in use by an iterator
 *   - Block and immediately queue any WRITE-key that has not already been iterated
 *
 * Iterator:  CONSISTENT = YES, REPLICATION = YES
 *   (Combination only valid in cluster mode - no SWAPDB possible)
 *   - Block if any write-key is in use by an iterator
 *   - Block and immediately queue any key (read or write) that has not already been iterated */
static bool expediteKeysForWrite(bgIterator *it,
                                 int dbid,
                                 struct serverCommand *cmd,
                                 int argc,
                                 robj **argv,
                                 keyReference *keyrefs,
                                 int numKeys,
                                 hashtable *waitingOnKeys) {
    serverAssert(numKeys > 0);

    bool mustBlock = false;

    /* All keys of the command should either be in scope or not since in cluster mode enabled they
     * should all be in the same slot. So we just check the first key. */
    robj *oKey = argv[keyrefs[0].pos];
    sds key = objectGetVal(oKey);
    /* If it's not in the iteration scope for the current iterator, then we don't need to do
     * anything with this command. */
    if (!it->keyset_iter->isKeyInScope(it->keyset_iter, key)) return false;

    if ((cmd->flags & CMD_WRITE_FIRSTKEY_ONLY) &&
        !(it->iteration_flags & BGITERATOR_FLAG_REPLICATION)) {
        /* If this write command only modifies the 1st key, we don't need to expedite others
         * unless replication enabled. */
        numKeys = 1;
    }

    if (cmd->proc == moveCommand) {
        // Special case for MOVE
        return expediteKeysForMove(it, dbid, argc, argv, waitingOnKeys);
    }

    if (cmd->proc == copyCommand) {
        // Similar special case for COPY
        return expediteKeysForCopy(it, dbid, argc, argv, waitingOnKeys);
    }

    if (it->iteration_flags & BGITERATOR_FLAG_CONSISTENT) {
        // CONSISTENT = YES, REPLICATION = YES / NO
        for (int i = 0; i < numKeys; i++) {
            robj *oKey = argv[keyrefs[i].pos];
            sds key = objectGetVal(oKey);
            dbEntry *de = (server.db[dbid]) ? dbFind(server.db[dbid], key) : NULL;
            if (de == NULL) continue; // New key, no need to expedite
            if (!iteratorHasPassedKey(it, dbid, key, de) &&
                ((bgIterationEntryMetadata *)objectGetMetadata(de))->iterator_epoch <= it->consistent_modification_id) {
                if (addEarlyIterationKey(it, de, dbid)) {
                    mustBlock = true;
                    hashtableAdd(waitingOnKeys, oKey);
                }
            } else {
                if (isEntryInuseByAnyIterator(de)) {
                    mustBlock = true;
                    hashtableAdd(waitingOnKeys, oKey);
                }
            }
        }
        it->cur_cmd_may_replicate = true; // Will replicate only if replication enabled
    } else {
        /* Identification of missing keys is only needed for non-consistent iteration.  This only
         * needs to be collected once (on the 1st non-consistent iteration). */
        bool collectMissing = (listLength(curCmdMissingKeys) == 0);

        if (it->iteration_flags & BGITERATOR_FLAG_REPLICATION) {
            // CONSISTENT = NO,  REPLICATION = YES
            bool someIterated = false;
            /* dict containing the keys that have not been iterated yet.
             * Using a dict dedupes the keys in case the command contains duplicated keys. */
            dict *notIteratedKeys = dictCreate(&dictEntryPtrDictType); // dict of dbEntry* -> robj*

            for (int i = 0; i < numKeys; i++) {
                robj *oKey = argv[keyrefs[i].pos];
                sds key = objectGetVal(oKey);
                dbEntry *de = (server.db[dbid]) ? dbFind(server.db[dbid], key) : NULL;
                if (de == NULL) {
                    if (collectMissing) {
                        incrRefCount(oKey);
                        listAddNodeHead(curCmdMissingKeys, oKey);
                    }
                    continue;
                }
                if (iteratorHasPassedKey(it, dbid, key, de)) {
                    someIterated = true;
                } else {
                    dictAdd(notIteratedKeys, de, oKey);
                }
                if (isEntryInuseByAnyIterator(de)) {
                    mustBlock = true;
                    hashtableAdd(waitingOnKeys, oKey);
                }
            }

            /* Since missing keys are considered as already iterated, if there are any missing keys
             * we must consider that some keys have been iterated, and make sure all other keys
             * will be expedited if needed. */
            if (listLength(curCmdMissingKeys) > 0) someIterated = true;

            /* This command may be executing as part of a larger transaction.  If some parts of the
             * transaction have already been identified to replicate, we must wait on all keys and
             * replicate here as well.  (Take care not to set cur_cmd_may_replicate to false.) */
            if (someIterated) {
                if (server.in_exec) {
                    /* We are now executing the commands in a multi-exec block.
                     *
                     * Regarding MULTI/EXEC:  Remember that this code is executed twice for commands
                     * within a MULTI/EXEC block.  First, we parse all the commands when deciding
                     * if the EXEC should be blocked.  Then, as each command is executed, it's
                     * re-parsed so that we can maintain the early iterated list as the commands
                     * execute.  In this second pass, as each command is executed, we can't change
                     * the replication decision which was made earlier (when the EXEC was processed).
                     * We don't want to get tricked (by a key being removed and recreated) into
                     * starting to replicate in the middle of a MULTI/EXEC block. */
                } else {
                    it->cur_cmd_may_replicate = true;
                }
            }
            if (it->cur_cmd_may_replicate) {
                dictEntry *de;
                dictIterator *di = dictGetIterator(notIteratedKeys);
                while ((de = dictNext(di)) != NULL) {
                    dbEntry *notIteratedEntry = dictGetKey(de);
                    robj *oKey = dictGetVal(de);

                    if (addEarlyIterationKey(it, notIteratedEntry, dbid)) {
                        mustBlock = true;
                        hashtableAdd(waitingOnKeys, oKey);
                    }
                }
                dictReleaseIterator(di);
            }
            dictRelease(notIteratedKeys);
        } else {
            // CONSISTENT = NO,  REPLICATION = NO
            for (int i = 0; i < numKeys; i++) {
                robj *oKey = argv[keyrefs[i].pos];
                sds key = objectGetVal(oKey);
                dbEntry *de = (server.db[dbid]) ? dbFind(server.db[dbid], key) : NULL;
                if (de == NULL) {
                    if (collectMissing) {
                        incrRefCount(oKey);
                        listAddNodeHead(curCmdMissingKeys, oKey);
                    }
                    continue;
                }
                if (isEntryInuseByAnyIterator(de)) {
                    mustBlock = true;
                    hashtableAdd(waitingOnKeys, oKey);
                }
            }
        }
    }

    return mustBlock;
}


/* Called when an iterator is terminated.  Pulls everything out of the queue
 * and returns the items to the main thread (before they hit the iterator). */
static void returnAllItemsToMainThread(bgIterator *it) {
    serverAssert(hasMainThreadExclusivity());

    fifo *poppedFifo = mutexQueuePopAll(it->items_for_iterator, false);
    if (poppedFifo == NULL) return; // Nothing to return

    // Release non-dictentry items first...
    fifo *itemsToReturn = fifoCreate();
    while (fifoLength(poppedFifo) > 0) {
        bgIteratorItem *item;
        fifoPop(poppedFifo, (void **)&item);
        switch (item->type) {
        // back out the "queued" statistic
        case BGITERATOR_ITEM_DBENTRY:
            it->dbentries_queued--;
            if (item->u.dbe.is_cloned) it->dbentry_clones_queued--;
            break;
        case BGITERATOR_ITEM_REPLICATION:
            it->replication_queued--;
            break;
        case BGITERATOR_ITEM_SWAPDB:
            it->swapdb_queued--;
            it->barrier_items--;
            break;
        case BGITERATOR_ITEM_FLUSHDB:
            it->flushdb_queued--;
            it->barrier_items--;
            break;

        case BGITERATOR_ITEM_COMPLETE:
            /* This can only happen if the completion item has been enqueued and
             * the iterator is terminated before reaching the completion item. */
            itemFreeList_returnItemBackToFreeList(item);
            continue; // Skip pushing this onto itemsToReturn

        case BGITERATOR_ITEM_TERMINATED:
            /* This can only happen if there is a race when terminating between
             *  the iteration client and main thread. */
            serverAssert(item == &STATIC_ITEM_TERMINATED);
            continue; // Skip pushing this onto itemsToReturn

        default:
            serverAssert(false);
        }

        fifoPush(itemsToReturn, item);
    }
    fifoRelease(poppedFifo);

    // Now release items all at once...
    if (fifoLength(itemsToReturn) > 0) {
        mutexQueueAddMultiple(it->return_to_main_thread, itemsToReturn);
    }
    fifoRelease(itemsToReturn);
}


/* =============================================================================================
 *                        Foreground support functions (private)
 * ============================================================================================= */

static size_t replicationItemSize(bgIteratorItem *item) {
    serverAssert(item->type == BGITERATOR_ITEM_REPLICATION);
    size_t itemSize = sizeof(bgIteratorItem);
    for (int i = 0; i < item->u.repl.argc; i++) {
        itemSize += objectComputeSize(NULL, item->u.repl.argv[i], 0, 0);
    }
    return itemSize;
}

static void processReturnOfItemToMainThread(bgIterator *it, bgIteratorItem *item) {
    serverAssert(hasMainThreadExclusivity());
    switch ((int)item->type) {
    case BGITERATOR_ITEM_REPLICATION:
        bufferedReplicationBytes -= item->u.repl.replication_size;
        freeRobjArray(item->u.repl.argc, item->u.repl.argv);
        break;

    case BGITERATOR_ITEM_DBENTRY:
        if (item->u.dbe.is_cloned) {
            freeClonedDictEntry(item->u.dbe.de);
        } else {
            if (isEntryInuseBySingleIterator(item->u.dbe.de)) {
                /* This blocking mechanism assumes a single DB so if the same key appears in
                 * multiple DBs, commands might get unblocked only to get blocked again.  (This
                 * would happen only rarely, and with minimal impact.) */
                robj *key = createStringObjectFromSds(objectGetKey(item->u.dbe.de));
                unblockClientsInUseOnKey(key);
                decrRefCount(key);
            }
            // resumeRehashing must be called before decrementEntryInuse, since decrementEntryInuse can free
            if (item->u.dbe.is_rehashing_paused) resumeRehashing(item->u.dbe.de);
            decrementEntryInuse(item->u.dbe.de);
        }
        break;

    case BGITERATOR_ITEM_SWAPDB:
    case BGITERATOR_ITEM_FLUSHDB:
        it->barrier_items--;
        break;

    case BGITERATOR_ITEMEXT_ITER_CLOSED: {
        if (it->terminated) {
            /* Abnormal termination
             * Normally the item is TERMINATED, but might be COMPLETE in race */
            serverAssert(it->current_item->type == BGITERATOR_ITEM_TERMINATED ||
                         it->current_item->type == BGITERATOR_ITEM_COMPLETE);
            // Release any items stranded on the iterator after early termination
            returnAllItemsToMainThread(it);
            receiveItemsBackFromOneIterator(it);
        } else {
            // Normal completion
            serverAssert(it->current_item->type == BGITERATOR_ITEM_COMPLETE);
        }
        if (it->current_item != &STATIC_ITEM_TERMINATED) itemFreeList_returnItemBackToFreeList(it->current_item);
        it->current_item = NULL;

        serverAssert(mutexQueueLength(it->items_for_iterator) == 0);
        serverAssert(it->barrier_items == 0);
        serverAssert(it->dbentries_queued == it->dbentries_processed);
        serverAssert(it->replication_queued == it->replication_processed);
        serverAssert(it->swapdb_queued == it->swapdb_processed);
        serverAssert(it->flushdb_queued == it->flushdb_processed);
        serverAssert(it->dbentry_clones_queued == it->dbentry_clones_processed);

        listEmpty(curCmdMissingKeys); // Just in case any remain

        bool terminated = it->terminated;
        void *privdata = it->privdata;
        bgIteratorCleanupFunc cleanup = it->cleanup;
        bgIteratorRelease(it); // Fully release the iterator before calling cleanup

        if (BGITERATION_DEBUG) {
            if (cleanup) debugBuffer = sdscatprintf(debugBuffer, "CLEANUP FN (%s)\n",
                                                    (terminated) ? "terminated" : "success");

            sds filename = sdscatprintf(sdsempty(), "bgiteration_debug.%d", getpid());
            FILE *f = fopen(filename, "w");
            sdsfree(filename);

            fputs(debugBuffer, f);

            fclose(f);
            sdsfree(debugBuffer);
            debugBuffer = sdsempty();
        }

        if (cleanup) cleanup(terminated, privdata);
        item = NULL; // Prevent return of static item to free list
    } break;

    default:
        serverAssert(false); // Not expecting any other type of item!
    }

    if (item) itemFreeList_returnItemBackToFreeList(item);
}

static void prepareAndProcessReturnedItems(bgIterator *it, int n, bgIteratorItem **items) {
    for (int i = 0; i < n; i++) valkey_prefetch(items[i]);
    for (int i = 0; i < n; i++) {
        if (items[i]->type != BGITERATOR_ITEM_DBENTRY) continue;
        valkey_prefetch(items[i]->u.dbe.de);
    }
    for (int i = 0; i < n; i++) {
        if (items[i]->type != BGITERATOR_ITEM_DBENTRY) continue;
        valkey_prefetch(objectGetKey(items[i]->u.dbe.de));
    }
    for (int i = 0; i < n; i++) processReturnOfItemToMainThread(it, items[i]);
}

#define PREFETCH_BATCH_SIZE 16

// Returns true if we process at least one item from a given iterator's return_to_main_thread queue.
static bool receiveItemsBackFromOneIterator(bgIterator *it) {
    bgIteratorItem *batchPool[PREFETCH_BATCH_SIZE];
    int n = 0;
    fifo *poppedFifo = mutexQueuePopAll(it->return_to_main_thread, false);
    if (poppedFifo != NULL) {
        while (fifoLength(poppedFifo) > 0) {
            fifoPop(poppedFifo, (void **)&batchPool[n++]);
            if (n == PREFETCH_BATCH_SIZE) {
                prepareAndProcessReturnedItems(it, n, batchPool);
                n = 0;
            }
        }
        if (n > 0) {
            prepareAndProcessReturnedItems(it, n, batchPool);
        }
        fifoRelease(poppedFifo);
        return true;
    }
    return false;
}

/* Process each iterator's return_to_main_thread queue
 * If `blocking` is true, continue reading until at least one queue was not empty. */
static void receiveItemsBackFromIterators(bool blocking) {
    serverAssert(hasMainThreadExclusivity());
    listIter li;
    listNode *node;
    bool processedItems = false;
    do {
        listRewind(allIterators, &li);
        while ((node = listNext(&li)) != NULL) {
            bgIterator *it = listNodeValue(node);
            processedItems |= receiveItemsBackFromOneIterator(it);
        }
        if (blocking && !processedItems) usleep(100); // Short sleep before retry
    } while (blocking && !processedItems);
}


static long long bgIteration_feedIterators_task(struct aeEventLoop *eventLoop,
                                                long long id,
                                                void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);
    serverAssert(hasMainThreadExclusivity());

    static monotime lastFeedEndTime; // STATIC: Persists For checking starvation
    monotime startTime = getMonotonicUs();

    if (!bgIteration_iterationActive()) {
        // No more iterators exist.  Self-check, and terminate the "feed" task.
        serverAssert(dictSize(nameToIterator) == 0);
        serverAssert(dictSize(inUseEntries) == 0);
        serverAssert(bufferedReplicationBytes == 0);

        // Shrink dict back to zero (doesn't normally shrink)
        dictRelease(inUseEntries);
        inUseEntries = dictCreate(&dictEntryPtrDictType);

        itemFreeList_release();

        bgIterator_timeproc_id = AE_DELETED_EVENT_ID;
        lastFeedEndTime = 0;
        return AE_NOMORE;
    }

    long dutyTimeUs = BGITER_CYCLE_BUDGET_MS * 1000;
    if (lastFeedEndTime > 0) {
        /* If the timer was delayed, compute the proportional time we should have had, and increase
         *  the duty cycle to compensate (up to a limit). */
        long starvationUs = (startTime - lastFeedEndTime) - BGITER_CYCLE_DELAY_MS * 1000;
        if (starvationUs > 0) {
            long starvationCompensationUs = starvationUs * BGITER_CYCLE_BUDGET_MS /
                                            (BGITER_CYCLE_BUDGET_MS + BGITER_CYCLE_DELAY_MS);
            dutyTimeUs += starvationCompensationUs;
            dutyTimeUs = MIN(dutyTimeUs, BGITER_CYCLE_BUDGET_MAX_MS * 1000);
        }
    }
    monotime endTime = startTime + dutyTimeUs;

    // Run this part regardless of time limit...
    receiveItemsBackFromIterators(false);

    // Feeding iterators (below) respects endTime.  The stuff above always runs to completion.

    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL && getMonotonicUs() < endTime) {
        bgIterator *it = listNodeValue(node);
        if (it->completed || it->terminated) continue;
        feedIterator(it, endTime);
    }

    lastFeedEndTime = getMonotonicUs();
    return BGITER_CYCLE_DELAY_MS;
}


// Not static, but not API.  Intended for unit tests where the event loop may not be active.
void bgIteration_feedIterators(void) {
    /* For unit testing, force the item_count_target to 1 in each call.  This ensures that we only
     * feed a minimal amount to the iterators rather than a non-deterministic amount. */
    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        it->item_count_target = 1;
    }

    // Invoke the feeding task (normally invoked by timer).
    bgIteration_feedIterators_task(NULL, 0, NULL);
}


static void resetReplicationFlagForIterators(client *c) {
    /* For any given command, the command may or may not need to be replicated based on the status
     * and flags of each iterator.  Furthermore, if a command does need to be replicated, this
     * replication must occur for an entire atomic unit; we can't replicate only part of a script
     * or multi/exec.
     * This function is the only place where the replication flag is cleared. */

    if (c->flag.multi || c->flag.script) {
        /* REGARDING MULTI/EXEC
         * --------------------
         * When processing a MULTI/EXEC, blockClientIfRequired is called first for the MULTI.  Then,
         * all of the commands are queued up in server.c:processCommand().  It's only when EXEC is
         * encountered, that server.c:call() is fired to begin execution.
         *
         * AFTER the EXEC is processed by call(), then each of the commands in the MULTI/EXEC block
         * will be processed through call().
         *
         * If write commands are present, MULTI & EXEC will be passed to the replication stream
         * before/after the transaction commands.  Note that MULTI & EXEC are not actually
         * "executed" at the time when their replication is passed to the replication stream.
         *
         * Example:  MULTI; SET A B; EXEC
         *  1. blockClientIfRequired() called for MULTI.  MULTI flag IS NOT set.  (Won't block.)
         *  2. blockClientIfRequired() called for EXEC.  MULTI flag IS set.  (Might block.)
         *  3. blockClientIfRequired() called for SET.  MULTI flag IS set.  (Won't block.)
         *  4. handleCommandReplication() is called for MULTI.
         *  5. handleCommandReplication() is called for SET.
         *  6. handleCommandReplication() is called for EXEC.
         *
         * SO - if the MULTI flag is set, we DON'T clear the flag.  It should only be cleared at the
         * start of the transaction, when MULTI is received - and the flag isn't set yet. */

        /* REGARDING SCRIPTS
         * -----------------
         * When processing a script, blockClientIfRequired is called first for the EVAL/EVALSHA/FCALL.
         * Then, all of the commands are processed using a special script client.  The script
         * client has the CLIENT_SCRIPT flag set.  For scripts, the replication flag is set when
         * processing the EVAL/EVALSHA/FCALL and should not be cleared when executing individual
         * commands in the script. */

        /* If it's the EXEC command, we fall through and clear the flag below.  But for all other
         *  commands within the transaction, we don't clear the flag. */
        if (c->cmd->proc != execCommand) return;
    }

    /* For most commands, the replication flag is cleared and we determine if replication is needed
     * based on the keys being used and their state in each iterator. If a modified key hasn't been
     * processed yet, there's no need to expedite the key or send the replication.  The key will be
     * sent later, when reached by the iterator.
     *
     * However, for scripts, it is not possible to perform this optimization.  There is no way to
     * know if an undeclared key might be modified.  Since the entire script needs to be replicated
     * (or not replicated) atomically, we can't take the chance that an undeclared key might be
     * hit which requires replication. */
    bool isScript = isScriptCallWriteCmd(c->cmd);

    sds firstScriptKey = NULL;
    if (isScript) {
        /* If it's a script, we will normally replicate.  But if the keys are out of scope for the
         * iteration, we shouldn't.  The use-case for this is with slot iteration, when the script
         * is acting on keys from a different slot.  Here, we just check the first declared key, and
         * if it's out of scope for the iteration, we won't replicate it.  This might cause issues
         * for cross-slot scripts (anti-pattern), but the alternative is replicating all scripts,
         * regardless of slot. */
        getKeysResult result;
        initGetKeysResult(&result);
        getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
        if (result.numkeys > 0) firstScriptKey = objectGetVal(c->argv[result.keys[0].pos]);
        getKeysFreeResult(&result);
    }

    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        if (it->completed || it->terminated) {
            it->cur_cmd_may_replicate = false;
        } else {
            /* For normal commands, the flag is initialized to false (not to replicate).  For these
             * commands, we decide later based on the actual commands.
             *
             * However, for scripts, we don't know what commands will be executed.  So IF it's a
             * script, and the keys are in scope (on the right slot) we initialize the replication
             * flag to true. */
            it->cur_cmd_may_replicate = isScript && firstScriptKey &&
                                        it->keyset_iter->isKeyInScope(it->keyset_iter, firstScriptKey);
        }
    }
}


static void handleSwapdb(int db1, int db2) {
    serverAssert(hasMainThreadExclusivity());
    serverAssert(bgIteration_iterationActive());
    serverAssert(!server.cluster_enabled);

    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        if (it->completed || it->terminated) continue;

        // Let the iterator internal mechanism know
        it->keyset_iter->swapDb(it->keyset_iter, db1, db2);

        // Let the background client know
        if (!(it->iteration_flags & BGITERATOR_FLAG_CONSISTENT)) {
            if (BGITERATION_DEBUG) {
                debugBuffer = sdscatprintf(debugBuffer, "SWAP: %d %d\n", db1, db2);
            }

            bgIteratorItem *item = itemFreeList_getElementOrAllocate();
            item->type = BGITERATOR_ITEM_SWAPDB;
            item->dbid = db1;
            item->u.dbid2 = db2;
            it->swapdb_queued++;
            it->barrier_items++;
            mutexQueueAdd(it->items_for_iterator, item);
        }
    }
}


static bool isDbSignificant(int dbid) {
    unsigned long long totalKeys = 0;
    for (int i = 0; i < server.dbnum; i++) {
        totalKeys += (server.db[i]) ? dbSize(server.db[i]) : 0;
    }
    return (server.db[dbid]) ? (dbSize(server.db[dbid]) > totalKeys / 2) : false;
}


static void handleFlushdb(int dbid) {
    // Invoked BEFORE the actual flush.  -1 indicates FLUSHALL.
    bool should_abort_iterators = (dbid == -1 || isDbSignificant(dbid));

    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);

        // Let the low-level iterator know the DB is being flushed
        it->keyset_iter->flushDb(it->keyset_iter, dbid);

        if (should_abort_iterators || it->iteration_flags & BGITERATOR_FLAG_CONSISTENT) {
            if (!it->terminated) bgIteratorTerminate(it);
        } else {
            /* In this (limited) case, we're only flushing a single DB that contains < half the
             * keys.  We don't want to kill a full-sync replication.  We will just continue with
             * iteration, knowing that a replication client will also receive the FLUSHDB on the
             * replication stream.  There's no need to worry about the items themselves.  Since
             * we've incremented the refcount, the items still in queue won't be physically deleted. */

            // Send a flushdb event to notify the client
            if (BGITERATION_DEBUG) {
                debugBuffer = sdscatprintf(debugBuffer, "FLUSH: %d\n", dbid);
            }
            bgIteratorItem *item = itemFreeList_getElementOrAllocate();
            item->type = BGITERATOR_ITEM_FLUSHDB;
            item->dbid = dbid;
            it->flushdb_queued++;
            it->barrier_items++;
            mutexQueueAdd(it->items_for_iterator, item);
        }
    }
    receiveItemsBackFromIterators(false); // Receive items back before flushing the items
}


static bool expediteKeysForWriteOnAllIterators(int dbid,
                                               struct serverCommand *cmd,
                                               int argc,
                                               robj **argv,
                                               keyReference *keyrefs,
                                               int numKeys,
                                               hashtable *waitingOnKeys) {
    bool mustBlock = false;

    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        if (expediteKeysForWrite(it, dbid, cmd, argc, argv, keyrefs, numKeys, waitingOnKeys))
            mustBlock = true;
    }

    return mustBlock;
}


static bool anIteratorWillReplicateForThisCommand(void) {
    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        if (it->cur_cmd_may_replicate) return true;
    }
    return false;
}


static bool expediteKeysForMultiExec(client *c, hashtable *waitingOnKeys) {
    serverAssert(c->cmd->proc == execCommand);

    /* For MULTI/EXEC, Valkey buffers all of the commands until hitting the EXEC.
     * At this point, the client holds all of the commands to be executed.  This function searches
     * for all of the keys used by any of the buffered write commands.  In addition, if SWAPDB or
     * SELECT is used, this tracks the DBIDs through various swap/select operations. */

    /* There's a special concern for a NON-consistent iteration with replication.  If the keys are
     * all "future" keys (which haven't been processed by the iterator yet), then we don't expedite
     * the keys or replicate.  However, if some keys have already been processed, we need to
     * expedite the remaining keys and replicate everything.
     *
     * When processing a single command, this is all handled.  But in this function, for MULTI/EXEC,
     * we process 1 command at a time.  There's an issue if the first command modifies a "future"
     * key, we don't know (without reading ahead) if a later command will modify a prior key.  This
     * would require the future key to be expedited.
     *
     * This COULD be addressed by collecting all of the keys into a single structure and then
     * analyzing them all at once.  However, this won't share code well with the single commands.
     * Also, building this structure is a little complex/time-consuming as we need to track both
     * key AND dictID.  One way to do this might be with a dict of dicts, where the first dict maps
     * a dictID to a dict of keys.
     *
     * ALTERNATIVELY (and it's the simpler approach that's taken here) we can just check if the
     * MULTI will be replicated.  If so, we re-process the MULTI, just in case there were commands
     * prior to deciding that replication was required that might have missed expediting.  If so,
     * these will be caught on the 2nd time around.
     *
     * Checking replication status before/after ensures that there can only be a single recursive
     * call. */
    bool initiallyAnIteratorWillReplicate = anIteratorWillReplicateForThisCommand();

    bool mustBlock = false;
    int *cur_to_orig_db = NULL;

    int curDb = c->db->id;
    for (int cmdNum = 0; cmdNum < c->mstate->count; cmdNum++) {
        struct serverCommand *cmd = c->mstate->commands[cmdNum].cmd;
        robj **argv = c->mstate->commands[cmdNum].argv;
        int argc = c->mstate->commands[cmdNum].argc;

        if (cmd->proc == swapdbCommand) {
            int id1, id2;
            if (getParamsForSwapdb(argc, argv, c, &id1, &id2)) {
                if (cur_to_orig_db == NULL) {
                    cur_to_orig_db = zmalloc(sizeof(int) * server.dbnum);
                    for (int i = 0; i < server.dbnum; i++) cur_to_orig_db[i] = i;
                }
                int temp = cur_to_orig_db[id1];
                cur_to_orig_db[id1] = cur_to_orig_db[id2];
                cur_to_orig_db[id2] = temp;
            }
            continue;
        }

        if (cmd->proc == selectCommand) {
            int id;
            if (getParamsForSelect(argc, argv, c, &id)) {
                curDb = id;
            }
            continue;
        }

        if (!isWriteCmd(cmd)) continue;

        getKeysResult result;
        initGetKeysResult(&result);
        int numkeys = getKeysFromCommand(cmd, argv, argc, &result);
        keyReference *keyrefs = result.keys;
        if (numkeys == 0) {
            getKeysFreeResult(&result);
            continue; // Write command with no keys - like FLUSHDB
        }

        if (expediteKeysForWriteOnAllIterators(
                cur_to_orig_db ? cur_to_orig_db[curDb] : curDb,
                cmd, argc, argv, keyrefs, numkeys, waitingOnKeys)) {
            mustBlock = true;
        }
        getKeysFreeResult(&result);
    }

    zfree(cur_to_orig_db);

    if (!initiallyAnIteratorWillReplicate && anIteratorWillReplicateForThisCommand()) {
        /* We've decided to replicate.  Re-process the MULTI/EXEC just once more to make sure that
         * we didn't miss any keys at the beginning.  This can't continue to recurse because
         * `initiallyAnIteratorWillReplicate` will be TRUE in the recursive call.  Note that the
         * recursive call may add additional entries to `waitingOnKeys`. */
        if (expediteKeysForMultiExec(c, waitingOnKeys)) mustBlock = true;
    }

    return mustBlock;
}


static bgIterator *bgIteratorCreate(const char *name,
                                    bgIteratorConsistency consistency,
                                    bgIteratorReplDoneFunc repldone,
                                    bgIteratorCleanupFunc cleanup,
                                    void *privdata,
                                    bgIterationType iter_type,
                                    genericIterator *keyset_iter) {
    serverAssert(server.forkless_infrastructure_enabled);
    serverAssert(hasMainThreadExclusivity());
    serverAssert(server.cluster_enabled || iter_type == BGITERATION_TYPE_FULLSCAN);

    int flags;
    switch (consistency) {
    case BGITERATOR_CONSISTENCY_NONE: flags = 0; break;
    case BGITERATOR_CONSISTENCY_START: flags = BGITERATOR_FLAG_CONSISTENT; break;
    case BGITERATOR_CONSISTENCY_EVENTUAL: flags = BGITERATOR_FLAG_REPLICATION; break;
    default: serverAssert(false);
    }
    // Consistent, with replication - doesn't make sense.
    serverAssert(!((flags & BGITERATOR_FLAG_CONSISTENT) && (flags & BGITERATOR_FLAG_REPLICATION)));

    bgIterator *it = zmalloc(sizeof(bgIterator));
    it->name = sdsnew(name);
    it->repldone = repldone;
    it->cleanup = cleanup;
    it->privdata = privdata;
    it->items_for_iterator = mutexQueueCreate();
    it->return_to_main_thread = mutexQueueCreate();

    // Floor queue size to bgiteration_queue_increase_incr or use last queue size value
    if (last_item_count_target < BGITER_QUEUE_INCREASE_INCR) {
        last_item_count_target = BGITER_QUEUE_INCREASE_INCR;
    }
    it->item_count_target = last_item_count_target;
    it->iteration_flags = flags;
    it->iteration_type = iter_type;
    it->consistent_modification_id = bgIteration_epoch++;
    it->keyset_iter = keyset_iter;
    it->early_iterate_entries = hashtableCreate(&dbEntryPtrHashtableType);
    hashtableExpand(it->early_iterate_entries, BGITER_EARLY_ITERATE_DICT_INITIAL_SIZE);
    it->current_item = NULL;
    it->client_is_active = false;
    it->completed = false;
    it->terminated = false;
    it->cur_cmd_may_replicate = false;

    it->dbentries_queued = 0;
    it->dbentries_processed = 0;
    it->replication_queued = 0;
    it->replication_processed = 0;
    it->swapdb_queued = 0;
    it->swapdb_processed = 0;
    it->flushdb_queued = 0;
    it->flushdb_processed = 0;
    it->dbentry_clones_queued = 0;
    it->dbentry_clones_processed = 0;

    it->barrier_items = 0;

    elapsedStart(&it->monotonic_start_time);
    it->monotonic_item_start_time = 0;


    if (bgIterator_timeproc_id <= 0) {
        // If iteration is not currently active, start the feeding task.  (Runs in main thread.)
        bgIterator_timeproc_id = aeCreateTimeEvent(server.el, 0, bgIteration_feedIterators_task, NULL, NULL);
        serverAssert(bgIterator_timeproc_id != AE_ERR);
    }

    if (dictAdd(nameToIterator, it->name, it) != DICT_OK) {
        // Can't have 2 iterators with the same name!
        serverAssert(false);
    }

    listAddNodeTail(allIterators, it);

    dictExpand(inUseEntries, listLength(allIterators) * it->item_count_target);

    return it;
}


/* =============================================================================================
 *                        PUBLIC INTERFACE:  Iterator creation and use
 * ============================================================================================= */

// PUBLIC API
bgIterator *bgIteratorCreateFullScanIter(const char *name,
                                         bgIteratorConsistency consistency,
                                         bgIteratorReplDoneFunc repldone,
                                         bgIteratorCleanupFunc cleanup,
                                         void *privdata) {
    return bgIteratorCreate(name, consistency, repldone, cleanup, privdata,
                            BGITERATION_TYPE_FULLSCAN, fullScanIteratorCreate());
}

// PUBLIC API
bgIterator *bgIteratorCreateSlotsIter(const char *name,
                                      bgIteratorConsistency consistency,
                                      const int *slots,
                                      int slots_count,
                                      bgIteratorReplDoneFunc repldone,
                                      bgIteratorCleanupFunc cleanup,
                                      void *privdata) {
    return bgIteratorCreate(name, consistency, repldone, cleanup, privdata,
                            BGITERATION_TYPE_CLUSTERSLOT, clusterSlotIteratorCreate(slots, slots_count));
}

// PUBLIC API
bgIterator *bgIteratorFind(const char *name) {
    serverAssert(hasMainThreadExclusivity());

    sds sdsname = sdsnew(name);
    bgIterator *it = dictFetchValue(nameToIterator, sdsname);
    sdsfree(sdsname);

    return it;
}


// PUBLIC API
const char *bgIteratorName(bgIterator *it) {
    return it->name;
}


// PUBLIC API
void bgIteratorGetStatus(bgIterator *it, bgIteratorStatus *status) {
    status->dbentries_queued = it->dbentries_queued;
    status->dbentries_processed = it->dbentries_processed;
    status->replication_queued = it->replication_queued;
    status->replication_processed = it->replication_processed;
    status->swapdb_queued = it->swapdb_queued;
    status->swapdb_processed = it->swapdb_processed;
    status->flushdb_queued = it->flushdb_queued;
    status->flushdb_processed = it->flushdb_processed;
    status->dbentry_clones_queued = it->dbentry_clones_queued;
    status->dbentry_clones_processed = it->dbentry_clones_processed;

    status->queue_length = mutexQueueLength(it->items_for_iterator);
    status->queue_length_target = it->item_count_target;

    status->runtime_ms = elapsedMs(it->monotonic_start_time);

    monotime nonvolatile_item_start_time = it->monotonic_item_start_time;
    status->current_item_ms = (nonvolatile_item_start_time == 0)
                                  ? 0
                                  : elapsedMs(nonvolatile_item_start_time);
}


// PUBLIC API
void bgIteratorTerminate(bgIterator *it) {
    serverAssert(hasMainThreadExclusivity());

    // Remove any items in the queue, but doesn't affect the 1 item that's being processed.
    returnAllItemsToMainThread(it);

    // We have to add an item, just in case the READER is waiting on the mutex.
    if (BGITERATION_DEBUG) {
        debugBuffer = sdscat(debugBuffer, "SENDING TERMINATE\n");
    }

    mutexQueueAdd(it->items_for_iterator, (void *)&STATIC_ITEM_TERMINATED);

    it->terminated = true;
}


// PUBLIC API
bool bgIteratorIsTerminating(bgIterator *it) {
    return it->terminated;
}


// PUBLIC API
bgIteratorItem *bgIteratorRead(bgIterator *it) {
    serverAssert(it->current_item == NULL ||
                 (it->current_item->type != BGITERATOR_ITEM_COMPLETE &&
                  it->current_item->type != BGITERATOR_ITEM_TERMINATED));

    // First, clean up the previous item read
    if (it->current_item != NULL) {
        returnCurrentItemToMainThread(it);

        /* To support unit tests.  Normal clients call bgIteratorRead from an alternate thread.
         * Without this, a unit test could get stuck waiting on the completion event because
         * feed won't get invoked.  For production, feed is called regularly from the main thread.
         * Note - this is checking that the exact same thread is used and shouldn't count modules. */
        if (onServerMainThread()) bgIteration_feedIterators_task(NULL, 0, NULL);
    } else {
        it->client_is_active = true;
    }

    it->monotonic_item_start_time = 0; // idle until blocking pop returns
    it->current_item = mutexQueuePop(it->items_for_iterator, true);
    it->monotonic_item_start_time = getMonotonicUs();

    return it->current_item;
}


// PUBLIC API
void bgIteratorClose(bgIterator *it) {
    if (it->current_item != NULL) {
        if (it->current_item->type == BGITERATOR_ITEM_COMPLETE ||
            it->current_item->type == BGITERATOR_ITEM_TERMINATED) {
            // Normal confirmation of background completion
        } else {
            // Client is initiating the termination
            it->terminated = true;
            returnCurrentItemToMainThread(it);

            it->current_item = (bgIteratorItem *)&STATIC_ITEM_TERMINATED;
        }
    } else {
        // terminated before first item read
        it->terminated = true;
        it->current_item = (bgIteratorItem *)&STATIC_ITEM_TERMINATED;
    }

    mutexQueueAdd(it->return_to_main_thread, (void *)&STATIC_ITEM_ITER_CLOSED);
}


/* =============================================================================================
 *                        PUBLIC INTERFACE:  Valkey main-thread support hooks
 * ============================================================================================= */

// PUBLIC API
void bgIteration_init(void) {
    serverAssert(hasMainThreadExclusivity());

    /* This should be called once and only once from the Valkey main thread.  However to support
     * unit tests, this is not validated, and multiple invocations are ignored.  */
    if (nameToIterator) return; // If already initialized, ignore (unit tests)

    nameToIterator = dictCreate(&sdsrefToPtrDictType);
    serverAssert(nameToIterator != NULL);

    allIterators = listCreate();
    serverAssert(allIterators != NULL);

    inUseEntries = dictCreate(&dictEntryPtrDictType);
    serverAssert(inUseEntries != NULL);

    curCmdMissingKeys = listCreate();
    serverAssert(curCmdMissingKeys != NULL);
    listSetFreeMethod(curCmdMissingKeys, decrRefCountVoid);

    bufferedReplicationBytes = 0;

    if (BGITERATION_DEBUG) {
        debugBuffer = sdsMakeRoomFor(sdsempty(), SDS_MAX_PREALLOC);
    }
}


// PUBLIC API
bool bgIteration_iterationActive(void) {
    return (allIterators != NULL && listLength(allIterators) > 0);
}


// PUBLIC API
void bgIteration_beforeSleep(void) {
    if (!bgIteration_iterationActive()) return;
    receiveItemsBackFromIterators(false);
}


// PUBLIC API
void bgIteration_keyDelete(int dbid, const_sds key) {
    if (!bgIteration_iterationActive()) return;
    serverAssert(hasMainThreadExclusivity());

    if (BGITERATION_DEBUG) {
        debugBuffer = sdscatprintf(debugBuffer, "KEYDEL: (%d)%s\n", dbid, key);
    }

    dbEntry *de = dbFind(server.db[dbid], (sds)key);
    serverAssert(de != NULL); // This API should be called BEFORE removal from main dict

    dbEntryPtrOfLastKeyDelete = de; // save for check at replication time

    // For consistent iterators, we need to make sure the item gets written before delete
    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        if (it->completed || it->terminated || !it->keyset_iter->isKeyInScope(it->keyset_iter, key)) continue;

        if (it->iteration_flags & BGITERATOR_FLAG_CONSISTENT &&
            ((bgIterationEntryMetadata *)objectGetMetadata(de))->iterator_epoch <= it->consistent_modification_id) {
            if (!iteratorHasPassedKey(it, dbid, key, de)) {
                addEarlyIterationKey(it, de, dbid); // (may also add to inUseEntries)
            }
        }
    }

    /* We might be within the context of a command execution.  This happens if the key is found to
     * be expired when attempting to execute the command.  In this case, we should treat the key as
     * missing.  If the key exists after the command executes, we can treat it like a new key. */
    if (server.in_call) {
        robj *oKey = createObject(OBJ_STRING, sdsdup(key));
        listAddNodeHead(curCmdMissingKeys, oKey);
    }
}


// PUBLIC API
void bgIteration_flushall(void) {
    handleFlushdb(-1);
}


// PUBLIC API
bool bgIteration_blockClientIfRequired(client *c) {
    serverAssert(hasMainThreadExclusivity());
    iteratorReplicationFlagsWereUpdated = false;
    if (!bgIteration_iterationActive()) return false;
    if (!isWriteCmd(c->cmd)) return false;
    if (isFlushCmd(c->cmd) && (getFlushCommandFlags(c, false, NULL) == C_ERR)) return false;

    if (BGITERATION_DEBUG) {
        sds sdsArgv = createSdsFromClientArgv(c->argc, c->argv);
        debugBuffer = sdscatprintf(debugBuffer, "BLCK?: (%d)%s\n", c->db->id, sdsArgv);
        sdsfree(sdsArgv);
    }

    /* Before executing a command or atomic transaction, the replication flag is cleared for each
     * iterator.  If it's determined that the command should replicate, the flag will be set
     * as the command and keys are examined for expedite. */
    resetReplicationFlagForIterators(c);
    iteratorReplicationFlagsWereUpdated = true;

    if (isFlushCmd(c->cmd)) {
        // Handle flush commands prior to execution
        handleFlushdb((c->cmd->proc == flushdbCommand) ? c->db->id : -1);
    }

    bool mustBlock = false;
    hashtable *waitOnKeys = hashtableCreate(&tempKeysetHashtableType); // set of robj(sds)
    listEmpty(curCmdMissingKeys);

    if (c->cmd->proc == execCommand) {
        mustBlock = expediteKeysForMultiExec(c, waitOnKeys);
    } else {
        getKeysResult result;
        initGetKeysResult(&result);
        int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
        keyReference *keyrefs = result.keys;
        if (numkeys > 0) {
            mustBlock = expediteKeysForWriteOnAllIterators(
                c->db->id, c->cmd, c->argc, c->argv, keyrefs, numkeys, waitOnKeys);
            // We shouldn't need to block on a command within a multi (that's not a script)
            serverAssert(!(mustBlock && c->flag.multi && !c->flag.script));

            if (mustBlock && (c->flag.script)) {
                /* For scripts, we will block for keys declared in EVAL/EVALSHA/FCALL.
                 *  However, scripts are NOT required to declare keys.  Even if it declares keys,
                 *  it's not declaring the DB for the key.  After a SELECT or SWAPDB, we might be on
                 *  a key we haven't blocked for.  In this case, there is no option but to execute a
                 *  synchronous block and wait for the iterator(s) to be done with the key(s).
                 *  (Yuck.)  */
                static const mstime_t SYNC_BLOCKING_LOG_INTERVAL = 60000;
                static mstime_t last_log = 0; // STATIC: persists to prevent log spamming
                static int blocked_count = 0; // STATIC: persistent count since last log
                blocked_count++;
                if (server.mstime - last_log > SYNC_BLOCKING_LOG_INTERVAL) {
                    serverLog(LL_WARNING,
                              "Forkless operation synchronously blocked %d times for scripts with undeclared keys",
                              blocked_count);
                    last_log = server.mstime;
                    blocked_count = 0;
                }

                while (mustBlock) {
                    receiveItemsBackFromIterators(true); // Blocking
                    hashtableEmpty(waitOnKeys, NULL);
                    mustBlock = expediteKeysForWriteOnAllIterators(
                        c->db->id, c->cmd, c->argc, c->argv, keyrefs, numkeys, waitOnKeys);
                }
            }
        } else {
            // WRITE commands with no keys should always be replicated.  SWAPDB, FLUSH, FUNCTION, etc.
            listIter li;
            listNode *node;
            listRewind(allIterators, &li);
            while ((node = listNext(&li)) != NULL) {
                bgIterator *it = listNodeValue(node);
                it->cur_cmd_may_replicate = true;
            }
        }
        getKeysFreeResult(&result);
    }

    if (mustBlock) {
        serverAssert(hashtableSize(waitOnKeys) > 0);
        robj **waitKeysArgv = zmalloc(sizeof(robj *) * hashtableSize(waitOnKeys));

        robj *key;
        hashtableIterator hi;
        hashtableInitIterator(&hi, waitOnKeys, 0);
        unsigned long argvCount = 0;
        while (hashtableNext(&hi, (void **)&key)) {
            waitKeysArgv[argvCount++] = key;
        }
        hashtableCleanupIterator(&hi);
        serverAssert(argvCount == hashtableSize(waitOnKeys));

        blockClientInUseOnKeys(c, argvCount, waitKeysArgv);

        zfree(waitKeysArgv);
    }

    hashtableRelease(waitOnKeys);

    if (BGITERATION_DEBUG) {
        if (mustBlock) debugBuffer = sdscat(debugBuffer, " (blocked)\n");
    }

    return mustBlock;
}


// PUBLIC API
void bgIteration_handleCommandReplication(int dbid,
                                          struct serverCommand *cmd,
                                          int argc,
                                          robj **argv) {
    if (BGITERATION_DEBUG) {
        // DEBUG - enable this to capture replication not queued because iteration is inactive
        if (0 && !bgIteration_iterationActive() && (isWriteCmd(cmd) || cmd->proc == multiCommand)) {
            sds sdsArgv = createSdsFromClientArgv(argc, argv);
            debugBuffer = sdscatprintf(debugBuffer, "REPL? INACT: (%d)%s\n", dbid, sdsArgv);
            sdsfree(sdsArgv);
        }
    }

    if (!bgIteration_iterationActive()) return;
    serverAssert(onServerMainThread());

    /* Some commands are replicated which are not writes (like publish) these can be ignored.
     *  Be careful with MULTI which is not a write command, but must be replicated. */
    if (!isWriteCmd(cmd) && cmd->proc != multiCommand) return;

    if (BGITERATION_DEBUG) {
        sds sdsArgv = createSdsFromClientArgv(argc, argv);
        debugBuffer = sdscatprintf(debugBuffer, "REPL?: (%d)%s\n", dbid, sdsArgv);
        sdsfree(sdsArgv);
    }

    if (cmd->proc == swapdbCommand) {
        // All iterators and clients must be informed of swapdb
        int id1, id2;
        // command has been processed, but Valkey allows "swapdb 0 0" (which can be ignored)
        if (getParamsForSwapdb(argc, argv, NULL, &id1, &id2))
            handleSwapdb(id1, id2);
    }

    /* In the case that a key is touched in a different DB (COPY/MOVE) the key is recorded as
     *  a "special" key and than handled below. */
    int special_dbid = 0;
    sds special_key = NULL;
    dbEntry *special_dbEntry = NULL;
    if (cmd->proc == moveCommand) {
        /* The MOVE command succeeded.  However MOVE requires special handling as it creates a new
         * key in a different database.  We need to make sure that we don't later try to iterate
         * on the key as it would be a duplicate key at that point.  So, instead, we will mark the
         * newly created key as "early iterated". */
        bool success = getDbIdFromRobj(argv[MOVE_COMMAND_DBID_ARG_INDEX], &special_dbid);
        serverAssert(success); // the command already succeeded, so this should work!

        robj *oKey = argv[1];
        special_key = (sds)objectGetVal(oKey);

        special_dbEntry = dbFind(server.db[special_dbid], special_key);
    }
    if (cmd->proc == copyCommand) {
        // The COPY command succeeded.  However COPY requires special handling (like MOVE).
        bool success = getTargetDbIdForCopyCommand(argc, argv, dbid, &special_dbid);
        serverAssert(success); // the command already succeeded, so this should work!

        // Find the newly created entry.
        robj *oKey = argv[2];
        special_key = (sds)objectGetVal(oKey);

        special_dbEntry = dbFind(server.db[special_dbid], special_key);
    }

    /* Implementation note regarding LUA and MULTI:  LUA scripts and MULTI-EXEC blocks must be
     * treated atomically.  We need to ensure that either ALL of the replication (or none of the
     * replication) for the atomic operation is processed by the iterator(s).  This is handled
     * naturally as we can only "complete" the iteration during the feeding process - and feeding
     * is only performed when handling timer events (after the LUA/MULTI has completed).  */

    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        if (it->completed || it->terminated) continue;

        /* For consistent iteration, we only iterate values based on version.  But for
         * non-consistent iteration, we don't need to explicitly iterate any values newly created
         * during the iteration.  So we mark them as expedited.  We know we have a new key if it
         * was missing before the command, and exists now. */

        if (!(it->iteration_flags & BGITERATOR_FLAG_CONSISTENT)) {
            // Handle the special case of a key moved to a different DB
            if (special_dbEntry != NULL) {
                if (it->cur_cmd_may_replicate &&
                    !it->keyset_iter->hasPassedItem(it->keyset_iter, special_key, special_dbid)) {
                    hashtableAdd(it->early_iterate_entries, special_dbEntry);
                    if (BGITERATION_DEBUG) {
                        sds entryString = createEntryString(special_dbid, special_dbEntry);
                        debugBuffer = sdscatprintf(debugBuffer, "EARLY(special): %s\n", entryString);
                        sdsfree(entryString);
                    }
                }

                /* Note: In the cases where there's a special command, we are copying or moving an
                 *       item to a different DB.  In these limited cases, we can only possibly be
                 *       creating a single key.  And if we've handled it here, we don't need to
                 *       handle it as a "missing key" below.  If we were to try to handle it as a
                 *       standard "missing key", we would get the DBID incorrect. */

            } else if (listLength(curCmdMissingKeys) > 0) {
                listIter missingIt;
                listNode *missingNode;
                listRewind(curCmdMissingKeys, &missingIt);
                while ((missingNode = listNext(&missingIt)) != NULL) {
                    robj *oKey = listNodeValue(missingNode);
                    const_sds key = objectGetVal(oKey);
                    dbEntry *de = dbFind(server.db[dbid], (sds)key);
                    if (de != NULL) {
                        // It exists now!
                        if (it->cur_cmd_may_replicate &&
                            !it->keyset_iter->hasPassedItem(it->keyset_iter, key, dbid)) {
                            /* If the current command is allowed to replicate, and there is a new
                             * key which we haven't yet reached in iteration, it needs to be added
                             * to the set of early iterate entries.  (We know that it's not already
                             * in that set because it's a newly created key!) */
                            bool wasAdded = hashtableAdd(it->early_iterate_entries, de);
                            serverAssert(wasAdded);
                            if (BGITERATION_DEBUG) {
                                sds entryString = createEntryString(dbid, de);
                                debugBuffer = sdscatprintf(debugBuffer, "EARLY(NEW): %s\n", entryString);
                                sdsfree(entryString);
                            }
                        }
                    }
                }
            }
        }

        /* Deletes (and unlinks) are special.
         * Developer context:  For most commands, we call bgIteration_blockClientIfRequired before
         *  the command and then call bgIteration_handleCommandReplication after the command.  While
         *  the "before" logic is determining the need to block, it can also determine (mostly) the
         *  need for replication (on each iterator).  Doing this all in one place saves us from
         *  performing some of the same logic twice.  When we get to this point in the code, we just
         *  use the previously determined information regarding replication.  This works because
         *  Valkey is single-threaded and only processes one command at a time.
         *
         * But deletes (and unlinks) happen multiple ways - and occur outside the normal
         *  before/after logic for commands.  These situations must be handled:
         *    - A normal (client-driven) DEL/UNLINK command will use the standard before/after
         *      logic.  If the key is in use by bgIteration, the command will be blocked.
         *    - An EVICTION generates a DEL/UNLINK which happens outside of the context of a client
         *      issued command.  The replication flags on the iterators are stale and relate to the
         *      prior command executed.
         *    - An EXPIRATION in the context of a client-driven WRITE command occurs when the client
         *      command attempts to access a key and it is found to be expired.  In this case, the
         *      client-command has already gone through the blocking process, so it should be OK to
         *      use it->cmd_may_replicate.
         *    - An EXPIRATION in the context of a client-driven READ command occurs when the client
         *      command attempts to access a key and it is found to be expired.  In this case, the
         *      client-command has NOT gone through the blocking process.  The replication flags on
         *      the iterators are stale and relate to the prior (write) command executed.
         *    - An EXPIRATION outside of a client-driven command occurs due to active expiry.  In
         *      this case, the replication flags on the iterator are stale and relate to the prior
         *      command executed.
         *
         * In the case of EXPIRE/EVICT occurring outside the context of a write command, this is
         *  handled.  If the key is in-use by bgIterator, increment of robj's refcount prevents the
         *  key from deletion. In this case the key will be removed from the main dictionary, but
         *  held by bgIteration until no longer needed.
         *  Even though the entry is not physically deleted yet, it is logically deleted and it is
         *  safe to replicate the DEL/UNLINK.  Since iterators process items FIFO, the replication
         *  for DEL/UNLINK won't actually get processed until other queued replication is processed.
         *
         * In the case of a client driven DEL command, the key will have already been deleted when
         *  we hit this routine.  In the case of EXPIRE/EVICT, they propagate happens before the key
         *  is deleted.  So if the key is missing, we can use the cached replication decision.  But
         *  if the key still exists (indicating EXPIRE/EVICT) we evaluate it specially. */
        bool shouldReplicateDelCommand = false;
        bool isDelCommand = isDeleteCmd(cmd);
        if (isDelCommand) {
            sds key = objectGetVal(argv[1]);
            dbEntry *de = dbFind(server.db[dbid], key);
            serverAssert(de == NULL); // dbEntry should be removed before replication (self-check)
            if (it->keyset_iter->isKeyInScope(it->keyset_iter, key)) {
                bool blockClientIfRequiredWasCalled = (server.in_call > 0);
                if (blockClientIfRequiredWasCalled && iteratorReplicationFlagsWereUpdated) {
                    // Here we know that the DEL is related to the running command
                    shouldReplicateDelCommand = it->cur_cmd_may_replicate;
                } else {
                    // Otherwise, it's something like active expiration or eviction (unrelated)
                    if (iteratorHasPassedKey(it, dbid, key, dbEntryPtrOfLastKeyDelete)) {
                        shouldReplicateDelCommand = true;
                    }
                }
                hashtableDelete(it->early_iterate_entries, dbEntryPtrOfLastKeyDelete); // just try delete (might not be here)
            }
        }

        bool replicate = (it->iteration_flags & BGITERATOR_FLAG_REPLICATION &&
                          ((!isDelCommand && it->cur_cmd_may_replicate) || shouldReplicateDelCommand));

        if (replicate) {
            /* We will replicate the command in these cases:
             * 1) For consistent iteration - it->cur_cmd_may_replicate is always true
             * 2) For non-consistent, if any of the keys have been processed, expediteKeysForWrite
             *    will ensure that ALL of the keys have been expedited - and we should replicate
             * 3) For non-consistent, if NONE of the keys have been processed, no need to replicate */
            if (BGITERATION_DEBUG) {
                debugBuffer = sdscat(debugBuffer, " (queued)\n");
            }

            bgIteratorItem *item = itemFreeList_getElementOrAllocate();
            item->type = BGITERATOR_ITEM_REPLICATION;
            item->dbid = dbid;
            item->u.repl.cmd = cmd;
            item->u.repl.argv = cloneRobjArray(argc, argv);
            item->u.repl.argc = argc;
            item->u.repl.replication_size = replicationItemSize(item);
            bufferedReplicationBytes += item->u.repl.replication_size;
            it->replication_queued++;
            mutexQueueAdd(it->items_for_iterator, item);
        }
    } // allIterators loop
}


// PUBLIC API
size_t bgIteration_memoryInuseForReplication(void) {
    return bufferedReplicationBytes;
}


// PUBLIC API
bool bgIteration_isEntryInuse(dbEntry *de) {
    serverAssert(hasMainThreadExclusivity());
    if (!bgIteration_iterationActive()) return false;
    return isEntryInuseByAnyIterator(de);
}


// PUBLIC API
void bgIteration_dbEntryModified(dbEntry *de) {
    if (bgIteration_iterationActive()) {
        bgIterationEntryMetadata *md = (bgIterationEntryMetadata *)objectGetMetadata(de);
        if (md) md->iterator_epoch = bgIteration_epoch;
    }
}


// PUBLIC API
void bgIteration_keyModified(int dbid, const_sds key) {
    if (bgIteration_iterationActive()) {
        dbEntry *de = dbFind(server.db[dbid], (sds)key);
        if (de) bgIteration_dbEntryModified(de);
    }
}


// PUBLIC API
void bgIteration_updateDbEntryPtr(dbEntry *old, dbEntry *new) {
    if (!bgIteration_iterationActive() || old == new) return;
    serverAssert(hasMainThreadExclusivity());
    serverAssert(!isEntryInuseByAnyIterator(old));

    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        if (hashtableDelete(it->early_iterate_entries, old)) {
            if (BGITERATION_DEBUG) {
                debugBuffer = sdscatprintf(debugBuffer, "EARLY LIST UPDATE %p -> %p\n", (void *)old, (void *)new);
            }
            bool wasAdded = hashtableAdd(it->early_iterate_entries, new);
            serverAssert(wasAdded);
        }
    }
}
