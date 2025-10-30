#include "config.h"
#include "bgiteration.h"
#include "dict.h"
#include "fifo.h"
#include "kvstore.h"
#include "monotonic.h"
#include "mutexqueue.h"
#include "server.h"

int getFlushCommandFlags(client *c, int *flags);    // in db.c
uint64_t dictObjHash(const void *key);                                      // in server.c
int dictObjKeyCompare(const void *key1, const void *key2);  // in server.c
size_t objectComputeSize(robj *key, robj *o, size_t sample_size, int dbid); // in object.c
robj *createStringObjectWithKeyAndExpire(const char *ptr, size_t len, const sds key, long long expire); // in object.c


// Non-public hashtable/kvstore functions...
bool hashtableInternalFindBucketIdx(hashtable *ht, void *key, int *table_idx, size_t *bucket_idx);
void hashtableInternalIteratorGetBucketIdx(hashtableIterator *iterator, int *table_idx, size_t *bucket_idx);
bool hashtableInternalIteratorIsBucketIdxComplete(hashtableIterator *iterator);
hashtableIterator *kvstoreInternalIteratorGetCurrentHashtableIterator(kvstoreIterator *kvs_it);


static bool receiveItemsBackFromOneIterator(bgIterator *it);    // in bgiteration.c - forward declaration

//################  TEMP COMPILE HACKS   ###########################
// Issue found.  server.db has changed from an array of db to an array of pointers to db (change all refs to server.db)
// Issue: iterators (kvstore/hashtable) are not safe across event loop invocations.  Hashtable (kvstore?) needs to track and maintain safe iterators.


// Don't think there's any current need for this...
static bool ignoreKeyForSave(sds key) {
    UNUSED(key);
    return false;
}

void amzUnblockClientsOnKey(void *info, robj *key); // temp for mock
int amzBlockClientOnKeys(void *info, client *c, robj *keys[], int nKeys); // temp for mock

//------- END OF COMPILE HACKS -------------------


// Returns true if the cmd is a script command that may replicate.
static bool isScriptCallWriteCmd(struct serverCommand *cmd) {
    return ((cmd->proc == fcallCommand)
            || (cmd->proc == evalCommand)
            || (cmd->proc == evalShaCommand));
}

// The PFCOUNT command (which does NOT have the CMD_WRITE flag) modifies the underlying string and
//  is replicated as a write.  So it needs to be detected and handled specially.
static bool isWriteCmd(struct serverCommand *cmd) {
    return ((cmd->flags & CMD_WRITE)
            || (cmd->proc == pfcountCommand) 
            || (cmd->proc == execCommand)
            || (isScriptCallWriteCmd(cmd)));
}

// Returns true if the command is a deletion based command (DEL or UNLINK)
static bool isDeleteCmd(struct serverCommand *cmd) {
    return ((cmd->proc == delCommand) || (cmd->proc == unlinkCommand));
}


static bool onValkeyMainThread(void) {
    return (pthread_equal(server.main_thread_id, pthread_self()) != 0);
}

/* Parse a parameters robj, extracting a valid DBID.
 * Returns FALSE if DBID isn't valid.
 */
static bool getDbIdFromRobj(robj *obj, int *db_id) {
    long long value;
    if (getLongLongFromObject(obj, &value) != C_OK) return false;
    if ((value < 0) || (value >= server.dbnum)) return false;
    *db_id = (int)value;
    return true;
}

/* Parse the parameters of the COPY command, extracting the target DBID.
 * Returns FALSE if the 
 */
static bool getTargetDbIdForCopyCommand(int argc, robj **argv, int selected_dbid, int *target_dbid) {
    const int COPY_COMMAND_OPTIONAL_ARG_START_INDEX = 3;

    *target_dbid = selected_dbid;

    for (int i = COPY_COMMAND_OPTIONAL_ARG_START_INDEX;  i < argc;  i++) {
        if (!strcasecmp((char *)objectGetVal(argv[i]), "replace")) {
            continue;
        } else if (!strcasecmp((char *)objectGetVal(argv[i]), "db") && (i + 1 < argc)) {
            /* Note the parsing here needs to perfectly match what we have in Valkey OSS for COPY.
             * The following command is considered OK by Valkey 8.1 so we can't return here, but
             * must continue to parse till the last db which is the one that's effectively used.
             *    COPY key1 key2 db 1 db 2 db 3    // (This will use db 3)
             */
            if (!getDbIdFromRobj(argv[i + 1], target_dbid)) {
                return false;   // parse failure
            }
            i++; // Consume additional argument
        } else {
            return false;   // parse failure
        }
    }
    return true;
}

/* Get parameters for the SWAPDB command.
 * The optional permission_client allows for checking of a client's permission for swapdb.
 * Returns true if command would be executed.
 */
bool getParamsForSwapdb(int argc, robj **argv, client *permission_client, int *id1_p, int *id2_p) {
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
    if (dbid1 == dbid2) return false;   // Valid, but doesn't do anything

    *id1_p = (int)dbid1;
    *id2_p = (int)dbid2;
    return true;
}

/* Get parameters for the SELECT command.
 * The optional permission_client allows for checking of a client's permission for select.
 * Returns true if command would be executed.
 */
bool getParamsForSelect(int argc, robj **argv, client *permission_client, int *dbid_p) {
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


/* DictType for SDS->ptr.  The SDS is referenced, no destructor. */
static dictType sdsrefToPtrDictType = {
    dictSdsHash,        /* hash function */
    NULL,               /* key dup */
    dictSdsKeyCompare,  /* key compare */
    NULL,               /* key destructor */
    NULL,               /* val destructor */
    NULL                /* allow to expand */
};


/* Wrap decrRefCount() so that it can be used as a callback requiring void. */
static void decrRefCountVoid(void *o) {
    decrRefCount(o);
}


/* Concatenate argc/argv into a command string for debugging. */
static sds createSdsFromClientArgv(int argc, robj **argv) {
    sds cmd = sdsempty();
    for (int i = 0;  i < argc;  i++) {
        robj *arg = getDecodedObject(argv[i]);  // some objects are int encoded
        cmd = sdscatprintf(cmd, "'%s' ", (char *)objectGetVal(arg));
        decrRefCount(arg);
    }
    return cmd;
}


//###########################################################################


/* bgIteration internal (compile time) configuration values */
static const int BGITER_EARLY_ITERATE_DICT_INITIAL_SIZE = 16384;    // Prevent initial rehashing
static const int BGITER_MAX_CLONE_ITEM_BYTES = 512;                 // Max size item to clone
static const int BGITER_MAX_CLONE_POOL_BYTES = (1 * 1024 * 1024);   // Total limit for all cloned items
static const int BGITER_QUEUE_INCREASE_INCR = 100;                  // Step size when increasing queue target
static const int BGITER_CYCLE_DELAY_MS = 2;                         // Delay between calls on bgIteration timer
static const int BGITER_CYCLE_BUDGET_MS = 1;                        // Normal time limit for timer processing
static const int BGITER_CYCLE_BUDGET_MAX_MS = 10;                   // Maximum time limit when starvation seen


typedef enum {
    BGITERATION_TYPE_NONE,
    BGITERATION_TYPE_FULLSCAN,
    BGITERATION_TYPE_CLUSTERSLOT
} bgIterationType;

/* Extensions to bgIteratorItemType.  These enumerations are used internally, and are not part of
 *  the published interface.  These allow for extensibility in the internal information-passing
 *  between the Valkey main thread and the iteration client thread. */
typedef enum {
    /* Indicates that the iteration client has completed use of the bgIterator and that the
     * bgIterator should be cleaned up and freed by the Valkey main thread. */
    BGITERATOR_ITEMEXT_ITER_CLOSED = 10
} bgIteratorItemTypeExtended;

/* Item for bgIteratorItemTypeExtended.BGITERATOR_ITEMEXT_ITER_CLOSED.  Used to pass a bgIterator
 * back to the Valkey main thread for cleanup/release. */
typedef struct {
    bgIteratorItemTypeExtended type;
    bgIterator *iter;
} bgIteratorItemExtClose;

/* Used for dictEntryPtrDictType. This dict grows and shrinks constantly during the iteration.
 * There is no point to rehash it all the time. */
static int neverShrink(size_t moreMem, double usedRatio) {
    UNUSED(moreMem);
    return (usedRatio > 0.5); // Return true only if expanding
}

// A dictionary with a pointer (itself) as a key (the address pointed to is NOT referenced).
//  Nothing is duplicated, this is a very fast dictionary, but potentially unsafe if the original
//  items are deleted or moved.
// WARNING:  Can't have active defrag running!  It might reallocate memory blocks, swapping their
//           pointer values!  A check must be made in active defrag to ensure that no iteration is
//           active.

// Thomas Wang's 64-bit mix
static uint64_t pointerHash(const void *key) {
    uint64_t h = (uint64_t)key;
    h = (~h) + (h << 21);           // h = (h << 21) - h - 1;
    h = h ^ (h >> 24);
    h = (h + (h << 3)) + (h << 8);  // h * 265
    h = h ^ (h >> 14);
    h = (h + (h << 2)) + (h << 4);  // h * 21
    h = h ^ (h >> 28);
    h = h + (h << 31);
    return h;
}

static int pointerCompare(const void *key1, const void *key2) {
    return key1 == key2;
}

static dictType dictEntryPtrDictType = {
    pointerHash,                /* hash function */
    NULL,                       /* key dup */
    pointerCompare,             /* key compare */
    NULL,                       /* key destructor */
    NULL,                       /* val destructor */
    neverShrink,                /* allow only to expand */
};

// A TEMP set of robj's (of type sds).  This is only for temporary sets as the robj's are not
//  ref-counted at insertion/deletion.  Used for robj->NULL.
static dictType tempKeysetDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    dictObjKeyCompare,          /* key compare */
    NULL,                       /* key destructor */
    NULL                        /* val destructor */
};

typedef struct genericIterator genericIterator;
typedef void   (*iteratorReleaseFunc)       (genericIterator *genIt);
typedef fifo * (*iteratorGetEntriesFunc)    (genericIterator *genIt, int *orig_dbid, int *cur_dbid);
typedef void   (*iteratorSwapDbFunc)        (genericIterator *genIt, int db1, int db2);
typedef void   (*iteratorFlushDbFunc)       (genericIterator *genIt, int cur_dbid);
typedef bool   (*iteratorHasPassedItemFunc) (genericIterator *genIt, const sds key, int cur_dbid);
typedef int    (*iteratorOriginalDbFunc)    (genericIterator *genIt, int cur_dbid);
typedef bool   (*iteratorIsKeyInScopeFunc)  (genericIterator *genIt, const sds key);

// Function pointers supporting polymorphic iterator implementation
struct genericIterator {
    iteratorReleaseFunc         release;
    iteratorGetEntriesFunc      getEntries;
    iteratorSwapDbFunc          swapDb;
    iteratorFlushDbFunc         flushDb;
    iteratorHasPassedItemFunc   hasPassedItem;
    iteratorOriginalDbFunc      originalDb;
    iteratorIsKeyInScopeFunc    isKeyInScope;
};

typedef struct itemListNode {
    struct itemListNode *next;
} itemListNode;

static itemListNode *freeItemStackHead = NULL;

static void itemFreeList_returnItemBackToFreeList(bgIteratorItem* item) {
    itemListNode *freedNode = (itemListNode*)item;
    freedNode->next = freeItemStackHead;
    freeItemStackHead = freedNode;
}

static bgIteratorItem *itemFreeList_getElementOrAllocate(void) {
 
    bgIteratorItem *item;
    // Pop a free node from the free list or allocate if none free
    if (freeItemStackHead) {
        item = (bgIteratorItem*)freeItemStackHead;
        freeItemStackHead = freeItemStackHead->next;
        if (freeItemStackHead) {
            valkey_prefetch(freeItemStackHead);
        }
    }
    else {
        // Create new listNode and item
        item = zmalloc(sizeof(bgIteratorItem));
    }
    return item;
}
 
static void itemFreeList_release(void) { 
    while(freeItemStackHead) { 
        itemListNode *node = freeItemStackHead;
        freeItemStackHead = node->next;
        zfree((bgIteratorItem*)node);
    }
}

// This struct is used across threads.  Unless otherwise noted, the fields are initialized at
//  iterator creation (within the main thread) and are read-only by the client thread.
struct bgIterator {
    sds name;                           // Iterator name
    bgIteratorReplDoneFunc repldone;    // Optional repldone function to be run on the main thread
    bgIteratorCleanupFunc cleanup;      // Optional cleanup function to be run on main thread
    void *privdata;                     // Client's private data to be passed to cleanup function

    int iteration_flags;                // Consistent and/or Replication
    int iteration_type;                 // Full scan or cluster slot
    uint32_t consistent_modification_id;// iterator epoch at time of iterator creation

    genericIterator *keyset_iter;   // Low-level iterator (polymorphic)

    dict *early_iterate_entries;    // Used to keep track of what items have already been iterated
                                    // over by out-of-order expedited process, ensuring a bgIterator 
                                    // does not try to reprocess items.
                                    // Used only by main thread.
                                    // dictEntry -> NULL

    mutexQueue *items_for_iterator; // Created/Destroyed in main thread, used in both (threadsafe)

    mutexQueue *return_to_valkey;   // Queue of items to be returned to the Valkey main thread (threadsafe)
    
    unsigned int item_count_target; // Used only by main thread

                                    // current_item is normally only used in the iteration client.
                                    //  It's marked volatile here only to support snooping from the
                                    //  main thread when handling a FLUSHDB command.  This prevents
                                    //  the compiler from generating code which might read the
                                    //  pointer multiple times (when it's coded to read only once).
                                    // Also - this syntax is for a volatile POINTER to a
                                    //  non-volatile item.  "volatile" at the beginning of the
                                    //  declaration, would indicate a (non-volatile) pointer to a
                                    //  volatile item.
    bgIteratorItem *volatile current_item;

    bool client_is_active;          // Set to true when client performs 1st read
    bool completed;                 // Set to true in main thread when last item from iteration has
                                    //  been queued to the client.  No additional items will be
                                    //  enqueued to the client after this has been set.

    volatile bool terminated;       // Set to true in main thread when iteration is to be killed
                                    // Set to true in iteration client when it decides to end early

    bool cur_cmd_may_replicate;     // Used only in main thread during command processing

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

                                    // The item start time is set in the iteration client.  It is
                                    //  marked volatile as it can be read from the main thread by
                                    //  bgIteratorGetStatus.  If 0, this indicates that the
                                    //  iteration client is waiting for an item to process.
    volatile monotime monotonic_item_start_time;     // Time current item processing started
};


// These static values are only accessed from the main Valkey thread.

static list *allIterators;      // list of bgIterator
static dict *nameToIterator;    // bgIterator->name -> bgIterator

// Global, across all iterators, dict contains a dbEntry pointer -> ref count
static dict *inUseEntries;      // dbEntry -> ref count

// Key values in the current command which don't exist in the DB yet.  Needed for determination of
//  replication for NON-consistent iterations.
static list *curCmdMissingKeys; // list of robj

// A counter of the total amount of memory used for buffered replication data.
//  This amount is excluded when computing the need for evictions.
static ssize_t bufferedReplicationBytes;

// Memory pool to track current allocated memory of cloned items (in bytes)
static ssize_t bgiteration_current_clone_memory_pool_size;

// Snapshot of the last queue size to seed the next queue
// We assume all bgIterators consume items at the same rate
static int last_item_count_target;

// Eventloop ID of the timerproc (or AE_DELETED_EVENT_ID)
static long long bgIterator_timeproc_id;

/* PUBLIC API - this is read whenever a dbEntry is modified in Valkey. */
uint32_t bgIteration_epoch;


// BgIteration debug captures BgIteration activity to a large sds buffer.  When an iterator is
//  completed, the entire buffer is written to a file in the current working directory.  Note that
//  memory must be available for the ENTIRE debug in memory.  This isn't captured incrementally to
//  a file as the file I/O is more likely to affect timing.
// Future implementation: the current design is most useful for a single iterator.  When items are
//  queued to an iterator, the iterator name is not recorded (to save space).
// Developer note: using a CONST value here allows the compiler to completely remove all of the
//  debugging code at compile time.  There is no run-time performance overhead when set to FALSE.
//  This is essentially like an IFDEF, however, it's better as it forces the compiler to validate
//  syntax.
static const bool BGITERATION_DEBUG = false;   // DO NOT SUBMIT WITH THIS SYMBOL SET TO TRUE!
static sds debugBuffer;



//=============================================================================================
//                        Full Scan Iterator
//=============================================================================================
/* The full scan iterator performs the actual iteration over the Valkey keyset.  The iterator is
 * only used from within the Valkey main thread.  Iteration proceeds one DB at a time, based on
 * the DB ordering at the time of iterator creation.  Each time the iterator returns items, all
 * of the dictionary entries from a single hash bucket are returned.
 */

struct fullScanIterator {
    genericIterator callbacks;  // (must be first item)

    // Array of mapping from original DB ID (at the time of iteration start) to that DB's
    //  current index.  So, if the DB which was DB-0 is now at index 6, orig_to_cur_db[0]==6.
    int *orig_to_cur_db;

    // The reverse of the above array.  This maps a current DB index to its original index
    //  (at the time of iteration start).
    int *cur_to_orig_db;

    // This is the DB we are currently iterating over.  This is relative to the ORIGINAL
    //  DB ordering, at the time of iterator creation.  Iteration proceeds from 0..N based on
    //  the original ordering.
    int iter_db;

    // Iterator for the DB orig_to_cur_db[iter_db]
    kvstore *kvs;   // keep track of kvs associated with iter_dbi
    kvstoreIterator *iter_dbi;
};

static void fullScanIteratorRelease(genericIterator *genIt) {
    struct fullScanIterator *it = (struct fullScanIterator *)genIt;
    if (it->iter_dbi) kvstoreIteratorRelease(it->iter_dbi);
    zfree(it->orig_to_cur_db);
    zfree(it->cur_to_orig_db);
    zfree(it);
}

static fifo * fullScanIteratorGetEntries(genericIterator *genIt, int *orig_dbid, int *cur_dbid) {
    struct fullScanIterator *it = (struct fullScanIterator *)genIt;
    if (it->iter_db >= server.dbnum) return NULL;   // Finished scanning

    fifo *dbEntryFifo = fifoCreate();
    while (fifoLength(dbEntryFifo) == 0) {
        while (it->iter_dbi == NULL) {
            if (++it->iter_db >= server.dbnum) {
                fifoRelease(dbEntryFifo);
                return NULL;    // Iteration complete
            }
            serverDb *db = server.db[it->orig_to_cur_db[it->iter_db]];
            if (db != NULL) {
                it->kvs = db->keys;
                it->iter_dbi = kvstoreIteratorInit(it->kvs, HASHTABLE_ITER_SAFE);
            }
        }

        hashtableIterator *ht_it = NULL;
        do {
            dbEntry *de;
            if (!kvstoreIteratorNext(it->iter_dbi, (void **)&de)) {
                kvstoreIteratorRelease(it->iter_dbi);
                it->kvs = NULL, it->iter_dbi = NULL;
                break;
            }

            ht_it = kvstoreInternalIteratorGetCurrentHashtableIterator(it->iter_dbi);
            if (ignoreKeyForSave(objectGetKey(de))) continue; // slot migration: keys being purged
            fifoPush(dbEntryFifo, de);
        } while (!hashtableInternalIteratorIsBucketIdxComplete(ht_it));
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
    int orig_db = it->cur_to_orig_db[cur_dbid];
    if (orig_db == it->iter_db) {
        // We are currently iterating on the DB that's being flushed.
        kvstoreIteratorRelease(it->iter_dbi);
        it->kvs = NULL, it->iter_dbi = NULL;
        // Iteration will continue with the next DB.
    }
}

static bool fullScanIteratorHasPassedItem(genericIterator *genIt, const sds key, int cur_dbid) {
    struct fullScanIterator *it = (struct fullScanIterator *) genIt;
    int orig_dbid = it->cur_to_orig_db[cur_dbid];

    if (orig_dbid < it->iter_db) return true;   // Entire DB has already been processed
    if (orig_dbid > it->iter_db) return false;  // Haven't started this DB yet
    // Now, orig_dbid == it->iter_db

    if (it->iter_dbi == NULL) return true;       // just finished this DB

    // We're in the middle of processing a DB.  In cluster-mode, the DB is divided into 1 hashtable
    //  per slot.  In cluster-mode-disabled, we treat all keys as in slot 0.
    int keySlot = server.cluster_enabled ? getKeySlot(key) : 0;
    if (keySlot < kvstoreIteratorGetCurrentHashtableIndex(it->iter_dbi)) return true;
    if (keySlot > kvstoreIteratorGetCurrentHashtableIndex(it->iter_dbi)) return false;

    // At this point, we're down to a specific hashtable.

    hashtable *iter_current_ht = kvstoreGetHashtable(it->kvs, keySlot);
    int table; // 0 or 1 (supporting rehashing)
    size_t index; // bucket number within the hashtable
    // If key doesn't exist, we consider it passed - we MIGHT have iterated over it had it existed.
    if (!hashtableInternalFindBucketIdx(iter_current_ht, key, &table, &index)) return true;

    hashtableIterator *htIter = kvstoreInternalIteratorGetCurrentHashtableIterator(it->iter_dbi);
    int iter_table;
    size_t iter_index;
    hashtableInternalIteratorGetBucketIdx(htIter, &iter_table, &iter_index);
    if (table < iter_table) return true;       // iteration in table 1, but item is in table 0
    if (table > iter_table) return false;      // iteration in table 0, but item is in table 1
    // if index <= iterator index, it has been passed. bgIterator
    // processes buckets atomically. hashtableIterator points to the
    // last returned position. It means bucket at iter_index has
    // already been processed.
    if (index <= iter_index) return true;
    if (ignoreKeyForSave(key)) return true;    // if slot being purged, pretend we have passed it
    return false;
}

static int fullScanIteratorOriginalDb(genericIterator *genIt, int cur_dbid) {
    struct fullScanIterator *it = (struct fullScanIterator *)genIt;
    return it->cur_to_orig_db[cur_dbid];
}

static bool fullScanIteratorIsKeyInScope(genericIterator *genIt, const sds key) {
    UNUSED(genIt);
    UNUSED(key);
    return true;    // All keys are in scope
}

static genericIterator * fullScanIteratorCreate(void) {
    struct fullScanIterator *it = zmalloc(sizeof(struct fullScanIterator));
    it->orig_to_cur_db = zmalloc(sizeof(int) * server.dbnum);
    it->cur_to_orig_db = zmalloc(sizeof(int) * server.dbnum);
    for (int i = 0;  i < server.dbnum;  i++) {
        it->orig_to_cur_db[i] = i;
        it->cur_to_orig_db[i] = i;
    }
    it->iter_db = -1;
    it->kvs = NULL;
    it->iter_dbi = NULL;

    it->callbacks.release = fullScanIteratorRelease;
    it->callbacks.getEntries = fullScanIteratorGetEntries;
    it->callbacks.swapDb = fullScanIteratorSwapDb;
    it->callbacks.flushDb = fullScanIteratorFlushDb;
    it->callbacks.hasPassedItem = fullScanIteratorHasPassedItem;
    it->callbacks.originalDb = fullScanIteratorOriginalDb;
    it->callbacks.isKeyInScope = fullScanIteratorIsKeyInScope;

    return (genericIterator *)it;
}



//=============================================================================================
//                        Cluster Slot Iterator
//=============================================================================================
/* The cluster slot iterator performs iteration over one cluster slot of the Valkey keyset.  The
 * iterator is only used from within the Valkey main thread.
 */
struct clusterSlotIterator {
    genericIterator callbacks;  // (must be first item)
};

static void clusterSlotIteratorRelease(genericIterator *genIt) {
    UNUSED(genIt);
    serverAssert(false);    // Not yet implemented
}

static fifo * clusterSlotIteratorGetEntries(genericIterator *genIt, int *orig_dbid, int *cur_dbid) {
    UNUSED(genIt);
    UNUSED(orig_dbid);
    UNUSED(cur_dbid);
    serverAssert(false);    // Not yet implemented
}

static void clusterSlotIteratorSwapDb(genericIterator *genIt, int db1, int db2) {
    UNUSED(genIt);
    UNUSED(db1);
    UNUSED(db2);
    serverAssert(false);    // swap not valid in cluster mode
}

static void clusterSlotIteratorFlushDb(genericIterator *genIt, int cur_dbid) {
    UNUSED(genIt);
    UNUSED(cur_dbid);
    serverAssert(false);    // Not yet implemented
}

static bool clusterSlotIteratorHasPassedItem(genericIterator *genIt, const sds key, int cur_dbid) {
    UNUSED(genIt);
    UNUSED(key);
    UNUSED(cur_dbid);
    serverAssert(false);    // Not yet implemented
}

static int clusterSlotIteratorOriginalDb(genericIterator *genIt, int cur_dbid) {
    UNUSED(genIt);
    UNUSED(cur_dbid);
    return cur_dbid;        // swap not supported in cluster mode
}

/* When checking if a command is in scope for this iterator, all of its keys should be either in
 * scope or not. In cluster mode enabled a command cannot reference keys from different slots, so
 * this assumption will always be true. */
static bool clusterSlotIteratorIsKeyInScope(genericIterator *genIt, const sds key) {
    UNUSED(genIt);
    UNUSED(key);
    serverAssert(false);    // Not yet implemented
}

static genericIterator * clusterSlotIteratorCreate(const int *slots, size_t slots_count) {
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
    serverAssert(false);    // Not yet implemented

    return (genericIterator *)it;
}



//=============================================================================================
//                        General iteration support (across all iterators)
//=============================================================================================

// While an item is potentially in use by a background thread, we can't have
//  rehashing by the main thread.  Returns true if rehashing was paused.
static bool pauseRehashing(dbEntry *de) {
    switch (de->encoding) {
        case OBJ_ENCODING_HASHTABLE: {  // SET or HASH
            hashtable *ht = objectGetVal(de);
            hashtablePauseRehashing(ht);
            return true;
        }
        case OBJ_ENCODING_SKIPLIST: {   // SORTED SET
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
        case OBJ_ENCODING_HASHTABLE: {  // SET or HASH
            hashtable *ht = objectGetVal(de);
            hashtableResumeRehashing(ht);
            break;
        }
        case OBJ_ENCODING_SKIPLIST: {   // SORTED SET
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

    return sdslen(key) + valueSize;     // ignore the rest of the overhead, it's minor & transient
}


static dbEntry *tryCloneDbEntry(dbEntry *de) {
    if (bgiteration_current_clone_memory_pool_size + BGITER_MAX_CLONE_ITEM_BYTES 
            > BGITER_MAX_CLONE_POOL_BYTES) {
        return NULL;
    }

    // Future optimization: Incorporate small ziplists, sorted sets, etc.
    // OBJ_ENCODING_INT is omitted only because there isn't a good API for cloning it yet.
    if (de->type == OBJ_STRING && de->encoding != OBJ_ENCODING_INT) {
        ssize_t itemSize = computeStringDbEntrySize(de);

        if (itemSize <= BGITER_MAX_CLONE_ITEM_BYTES) {
            bgiteration_current_clone_memory_pool_size += itemSize;
            dbEntry *clone = createStringObjectWithKeyAndExpire((char *)objectGetVal(de), sdslen(objectGetVal(de)), objectGetKey(de), objectGetExpire(de));
            ((bgIterationEntryMetadata *)objectGetMetadata(clone))->iterator_epoch
                    = ((bgIterationEntryMetadata *)objectGetMetadata(de))->iterator_epoch;
            return clone;
        }
    }

    return NULL;
}


static void freeClonedDictEntry(dbEntry *clonedEntry) {
    serverAssert(clonedEntry->type == OBJ_STRING);

    // Add back to memory pool
    bgiteration_current_clone_memory_pool_size -= computeStringDbEntrySize(clonedEntry);

    decrRefCount(clonedEntry);
}

static bgIteratorItem * makeDbEntryItem(dbEntry *de, int dbid, bool isCloned) {
    if (!isCloned) incrementEntryInuse(de);

    bgIteratorItem *item = itemFreeList_getElementOrAllocate();
    item->type = BGITERATOR_ITEM_DBENTRY;
    item->dbid = dbid;
    item->u.dbe.de = de;
    item->u.dbe.is_cloned = isCloned;
    item->u.dbe.is_rehashing_paused = pauseRehashing(de);

    return item;
}

static robj ** cloneRobjArray(int argc, robj **argv) {
    robj **newarray = zmalloc(sizeof(robj*) * argc);
    for (int i = 0;  i < argc;  i++) {
        newarray[i] = argv[i];
        incrRefCount(argv[i]);
    }
    return newarray;
}


static void freeRobjArray(int argc, robj **argv) {
    for (int i = 0;  i < argc;  i++) {
        decrRefCount(argv[i]);
    }
    zfree(argv);
}


// Called by iterator thread to release an item.
static void returnCurrentItemToValkey(bgIterator *it) {
    bgIteratorItem *item = it->current_item;
    if (item == NULL) return;

    switch (item->type) {
        case BGITERATOR_ITEM_DBENTRY:
            it->dbentries_processed++;
            if (item->u.dbe.is_cloned) it->dbentry_clones_processed++;
            mutexQueueAdd(it->return_to_valkey, item);
            break;
        case BGITERATOR_ITEM_REPLICATION:
            it->replication_processed++;
            mutexQueueAdd(it->return_to_valkey, item);
            break;
        case BGITERATOR_ITEM_SWAPDB:
            it->swapdb_processed++;
            mutexQueueAdd(it->return_to_valkey, item);
            break;
        case BGITERATOR_ITEM_FLUSHDB:
            it->flushdb_processed++;
            mutexQueueAdd(it->return_to_valkey, item);
            break;

        case BGITERATOR_ITEM_COMPLETE:
        case BGITERATOR_ITEM_TERMINATED:
            // These are static and just used to wake the iterator - they should never be returned.
            serverAssert(false);
            break;

        default:
            serverAssert(false);
    }

    // Do this AFTER placing into return_to_valkey.  This is volatile and snooped when there is a
    //  flushall event.  Don't want an item to be missed.
    it->current_item = NULL;
}



//=============================================================================================
//                        Background Iterator (private)
//=============================================================================================

static void bgIteratorRelease(bgIterator *it) {
    serverAssert(onValkeyMainThread());
    serverAssert(it->current_item == NULL);
    serverAssert(mutexQueueLength(it->items_for_iterator) == 0);
    serverAssert(mutexQueueLength(it->return_to_valkey) == 0);

    dictDelete(nameToIterator, it->name);
    listDelNode(allIterators, listSearchKey(allIterators, it));

    mutexQueueRelease(it->items_for_iterator);
    it->items_for_iterator = NULL;

    mutexQueueRelease(it->return_to_valkey);
    it->return_to_valkey = NULL;

    it->keyset_iter->release(it->keyset_iter);
    it->keyset_iter = NULL;

    dictRelease(it->early_iterate_entries);
    it->early_iterate_entries = NULL;

    sdsfree(it->name);
    zfree(it);
}


static bool shouldFeedIteratorMore(bgIterator *it) {
    return (!it->completed
         && !it->terminated
         && mutexQueueLength(it->items_for_iterator) < it->item_count_target);
}


// Debugging routine
static sds createEntryString(int dbid, dbEntry *de) {
    sds key = objectGetKey(de);

    sds entrySds = sdsempty();
    entrySds = sdscatprintf(entrySds, "(%d)'%s'", dbid, key);
    if (de->type == OBJ_STRING) {
        robj *o = getDecodedObject(de);    // might be encoded as int
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
    // Smart logic to dynamically adjust the size of the queue
    unsigned int initial_queue_len = mutexQueueLength(it->items_for_iterator);

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
            serverAssert(it->dbentries_queued      >= it->dbentries_processed);
            serverAssert(it->replication_queued    >= it->replication_processed);
            serverAssert(it->swapdb_queued         >= it->swapdb_processed);
            serverAssert(it->flushdb_queued        >= it->flushdb_processed);
            serverAssert(it->dbentry_clones_queued >= it->dbentry_clones_processed);

            // Snapshot queue size to seed next iterator when terminated
            last_item_count_target = it->item_count_target;

            if (it->iteration_flags & BGITERATOR_FLAG_REPLICATION) {
                if (!it->client_is_active || (it->dbentries_queued > it->dbentries_processed)) {
                    // We are done feeding dict entries to the iterator, but before ending the
                    //  replication processing make sure that the iterator has become active (has
                    //  started reading) and make sure that all of the dict entries have been processed
                    //  by the client.
                    break;
                }
                if (it->repldone) {
                    bool clientWantsMoreReplication = (!it->repldone(it->privdata));
                    if (clientWantsMoreReplication) break;
                }
            }
            bgIteratorItem *completionItem = itemFreeList_getElementOrAllocate();
            *completionItem = (bgIteratorItem){ .type = BGITERATOR_ITEM_COMPLETE };
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
            if (it->iteration_flags & BGITERATOR_FLAG_CONSISTENT
                    && ((bgIterationEntryMetadata *)objectGetMetadata(de))->iterator_epoch > it->consistent_modification_id) {
                continue;
            }

            // Remove any items which have been processed early
            if (dictFind(it->early_iterate_entries, de) != NULL) {
                dictDelete(it->early_iterate_entries, de);
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
    if (initial_queue_len == 0 && have_time) {
        it->item_count_target += BGITER_QUEUE_INCREASE_INCR;
    }
}


static bool addEarlyIterationKey(bgIterator *it, dbEntry *earlyEntry, int cur_dbid) {
    int rc = dictAdd(it->early_iterate_entries, earlyEntry, NULL);
    serverAssert(rc == DICT_OK);
  
    int dbid = (it->iteration_flags & BGITERATOR_FLAG_CONSISTENT)
            ? it->keyset_iter->originalDb(it->keyset_iter, cur_dbid)
            : cur_dbid;

    dbEntry *cloneEntry = tryCloneDbEntry(earlyEntry);
    bool isClonedEntry = (cloneEntry != NULL);
    bgIteratorItem *item = makeDbEntryItem(isClonedEntry ? cloneEntry : earlyEntry, dbid, isClonedEntry);

    it->dbentries_queued++;
    if (isClonedEntry) it->dbentry_clones_queued++;

    if (it->iteration_flags & BGITERATOR_FLAG_CONSISTENT) {
        // On consistent iteration, SWAPDB events are not provided.  So there is no requirement to
        //  keep items in order or synchronized with SWAPDB.
        if (BGITERATION_DEBUG) {
            sds entryString = createEntryString(dbid, item->u.dbe.de);
            debugBuffer = sdscatprintf(debugBuffer, "EARLY_1: %s\n", entryString);
            sdsfree(entryString);
        }
        mutexQueuePushPriority(it->items_for_iterator, item);
    } else {
        if (BGITERATION_DEBUG) {
            sds entryString = createEntryString(dbid, item->u.dbe.de);
            debugBuffer = sdscatprintf(debugBuffer, "EARLY: %s\n", entryString);
            sdsfree(entryString);
        }
        mutexQueueAdd(it->items_for_iterator, item);
    }
    return !isClonedEntry; // Block if the entry will be used by the background thread
}


// This expedites a single key and doesn't attempt to avoid expediting through optimization.
static bool expediteSingleKeyWithoutOptimization(
        bgIterator *it,
        int dbid,
        robj *oKey,
        dict *waitingOnKeys) {

    bool mustBlock = false;

    bool iterComplete = it->completed || it->terminated;

    sds key = objectGetVal(oKey);
    dbEntry *de = dbFind(server.db[dbid], key);
    if (de != NULL) {
        if (!(iterComplete || it->keyset_iter->hasPassedItem(it->keyset_iter, key, dbid))
                && (dictFind(it->early_iterate_entries, de) == NULL)) {
            if (addEarlyIterationKey(it, de, dbid)) {
                mustBlock = true;
                dictAdd(waitingOnKeys, oKey, NULL); 
            }
        } else {
            if (isEntryInuseByAnyIterator(de)) {
                mustBlock = true;
                dictAdd(waitingOnKeys, oKey, NULL);
            }
        }
    }

    return mustBlock;
}


// MOVE/COPY are unfortunate special commands.  They work on 2 DBs at once.
const int MOVE_COMMAND_DBID_ARG_INDEX = 2;
static bool expediteKeysForMove(
        bgIterator *it,
        int dbid,
        int argc,
        robj **argv,
        dict *waitingOnKeys) {
    if (argc <= MOVE_COMMAND_DBID_ARG_INDEX) return false;

    int destDbid;
    if (!getDbIdFromRobj(argv[MOVE_COMMAND_DBID_ARG_INDEX], &destDbid)) return false;

    bool mustBlock = false;
    robj *key = argv[1];

    // Not looking for special cases to optimize here.  Just try to expedite both src and dest
    //  keys.  Note that the dest key might exist (and need iteration) but could be expired and
    //  could be overwritten by MOVE.  In this case, a DEL would replicate due to the expiry.  So
    //  even if the target is expired, we need to replicate it before executing the command.
    if (expediteSingleKeyWithoutOptimization(it, dbid, key, waitingOnKeys)) mustBlock = true;
    if (expediteSingleKeyWithoutOptimization(it, destDbid, key, waitingOnKeys)) mustBlock = true;

    it->cur_cmd_may_replicate = true;
    return mustBlock;
}


// MOVE/COPY are unfortunate special commands.  They work on 2 DBs at once.
static bool expediteKeysForCopy(
        bgIterator *it,
        int dbid,
        int argc,
        robj **argv,
        dict *waitingOnKeys) {

    int destDbid;
    if (!getTargetDbIdForCopyCommand(argc, argv, dbid, &destDbid)) return false;

    bool mustBlock = false;
    robj *srcKey = argv[1];
    robj *destKey = argv[2];

    // Not trying to optimize COPY.  Just expedite source and destination (if it exists).  We
    //  don't really care if the value is overwritten or not (so no need to parse REPLACE option).
    if (expediteSingleKeyWithoutOptimization(it, dbid, srcKey, waitingOnKeys)) mustBlock = true;
    if (expediteSingleKeyWithoutOptimization(it, destDbid, destKey, waitingOnKeys)) mustBlock = true;

    it->cur_cmd_may_replicate = true;
    return mustBlock;
}


/* There are several cases where a client must be blocked on write operations.  (Clients never need
 * to be blocked for read operations.)
 *
 * Note:  An Amazon extension to the Valkey command structure allows us to identify commands where
 *        the first key is for write and the rest are for read.  This allows us to make the
 *        following optimizations:
 *   - for keys which are read only, there's no need to block if the key is in-use by an iterator
 *   - without replication, there's no need to immediately queue read keys on a consistent iteration
 *
 * Iterator:  CONSISTENT = NO,  REPLICATION = NO
 *   - Block if any write-key is in use by an the iterator
 *
 * Iterator:  CONSISTENT = NO,  REPLICATION = YES
 *   - Block if any write-key is in use by an the iterator
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
 *   - Block if any write-key is in use by an the iterator
 *   - Block and immediately queue any WRITE-key that has not already been iterated
 *
 * Iterator:  CONSISTENT = YES, REPLICATION = YES
 *   (Combination only valid in cluster mode - no SWAPDB possible)
 *   - Block if any write-key is in use by an the iterator
 *   - Block and immediately queue any key (read or write) that has not already been iterated
 */
static bool expediteKeysForWrite(
        bgIterator *it,
        int dbid,
        struct serverCommand *cmd,
        int argc,
        robj **argv,
        keyReference *keyrefs,
        int numKeys,
        dict *waitingOnKeys) {
    serverAssert(numKeys > 0);

    bool mustBlock = false;

    // All keys of the command should either be in scope or not since in cluster mode enabled they
    // should all be in the same slot. So we just check the first key.
    robj *oKey = argv[keyrefs[0].pos];
    sds key = objectGetVal(oKey);
    // If it's not in the iteration scope for the current iterator, then we don't need to do
    // anything with this command.
    if (!it->keyset_iter->isKeyInScope(it->keyset_iter, key)) return false;

    // Note: performance optimization for commands which only modify the first key.  If this flag
    //  is not available, we can safely remove this `if` statement.
    if ((cmd->flags & CMD_WRITE_FIRSTKEY_ONLY)
            && !(it->iteration_flags & BGITERATOR_FLAG_REPLICATION)) {
        // If this write command only modifies the 1st key, we don't need to expedite others
        //  unless replication enabled.
        numKeys = 1;
    }

    if (cmd->proc == moveCommand) {
        // Unfortunate special case for MOVE
        return expediteKeysForMove(it, dbid, argc, argv, waitingOnKeys);
    }

    if (cmd->proc == copyCommand) {
        // Similar special case for COPY
        return expediteKeysForCopy(it, dbid, argc, argv, waitingOnKeys);
    }

    bool iterComplete = it->completed || it->terminated;

    if (it->iteration_flags & BGITERATOR_FLAG_CONSISTENT) {
        // CONSISTENT = YES, REPLICATION = YES / NO
        for (int i = 0;  i < numKeys;  i++) {
            robj *oKey = argv[keyrefs[i].pos];
            sds key = objectGetVal(oKey);
            dbEntry *de = dbFind(server.db[dbid], key);
            if (de == NULL) continue;  // New key, no need to expedite
            if (!(iterComplete || it->keyset_iter->hasPassedItem(it->keyset_iter, key, dbid))
                    && dictFind(it->early_iterate_entries, de) == NULL
                    && ((bgIterationEntryMetadata *)objectGetMetadata(de))->iterator_epoch <= it->consistent_modification_id) {
                if (addEarlyIterationKey(it, de, dbid)) {
                    mustBlock = true;
                    dictAdd(waitingOnKeys, oKey, NULL); 
                }
            } else {
                if (isEntryInuseByAnyIterator(de)) {
                    mustBlock = true;
                    dictAdd(waitingOnKeys, oKey, NULL);
                }
            }
        }
        it->cur_cmd_may_replicate = true;   // Will replicate only if replication enabled
    } else {
        // Identification of missing keys is only needed for non-consistent iteration.  This only
        //  needs to be collected once (on the 1st non-consistent iteration)
        bool collectMissing = (listLength(curCmdMissingKeys) == 0);

        if (it->iteration_flags & BGITERATOR_FLAG_REPLICATION) {
            // CONSISTENT = NO,  REPLICATION = YES
            bool someIterated = false;
            // dict containing the keys that have not been iterated yet.
            //  Using a dict dedupes the keys in case the command contains duplicated keys.
            dict *notIteratedKeys = dictCreate(&dictEntryPtrDictType);    // dict of dbEntry* -> robj*

            for (int i = 0; i < numKeys; i++) {
                robj *oKey = argv[keyrefs[i].pos];
                sds key = objectGetVal(oKey);
                dbEntry *de = dbFind(server.db[dbid], key);
                if (de == NULL) {
                    if (collectMissing) {
                        incrRefCount(oKey);
                        listAddNodeHead(curCmdMissingKeys, oKey);
                    }
                    continue;
                }
                if (iterComplete
                        || it->keyset_iter->hasPassedItem(it->keyset_iter, key, dbid)
                        || (dictFind(it->early_iterate_entries, de) != NULL)) {
                    someIterated = true;
                } else {
                    dictAdd(notIteratedKeys, de, oKey);
                }
                if (isEntryInuseByAnyIterator(de)) {
                    mustBlock = true;
                    dictAdd(waitingOnKeys, oKey, NULL);
                }
            }

            // Since missing keys are considered as already iterated, if there are any missing keys
            //  we must consider that some keys have been iterated, and make sure all other keys
            //  will be expedited if needed.
            if (listLength(curCmdMissingKeys) > 0) someIterated = true;

            // This command may be executing as part of a larger transaction.  If some parts of the
            //  transaction have already been identified to replicate, we must wait on all keys and
            //  replicate here as well.  (Take care not to set cur_cmd_may_replicate to false.)
            if (someIterated) {
                if (server.in_exec) {
                    // We are now executing the commands in a multi-exec block.
                    //
                    // Regarding MULTI/EXEC:  Remember that this code is executed twice for commands
                    //  within a MULTI/EXEC block.  First, we parse all the commands when deciding
                    //  if the EXEC should be blocked.  Then, as each command is executed, it's
                    //  re-parsed so that we can maintain the early iterated list as the commands
                    //  execute.  In this second pass, as each command is executed, we can't change
                    //  the replication decision which was made earlier (when the EXEC was processed).
                    // We don't want to get tricked (by a key being removed and recreated) into
                    //  into starting to replicate in the middle of a MULTI/EXEC block.
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
                        dictAdd(waitingOnKeys, oKey, NULL); 
                    }
                }
                dictReleaseIterator(di);
            }
            dictRelease(notIteratedKeys);
        } else {
            // CONSISTENT = NO,  REPLICATION = NO
            for (int i = 0;  i < numKeys;  i++) {
                robj *oKey = argv[keyrefs[i].pos];
                sds key = objectGetVal(oKey);
                dbEntry *de = dbFind(server.db[dbid], key);
                if (de == NULL) {
                    if (collectMissing) {
                        incrRefCount(oKey);
                        listAddNodeHead(curCmdMissingKeys, oKey);
                    }
                    continue;
                }
                if (isEntryInuseByAnyIterator(de)) {
                    mustBlock = true;
                    dictAdd(waitingOnKeys, oKey, NULL);
                }
            }
        }
    }

    return mustBlock;
}


// Called when an iterator is terminated.  Pulls everything out of the queue
//  and returns the items to Valkey (before they hit the iterator).
static void returnAllItemsToValkey(bgIterator *it) {
    serverAssert(onValkeyMainThread());

    fifo *poppedFifo = mutexQueuePopAll(it->items_for_iterator, false);
    if (poppedFifo == NULL) return;   // Nothing to return

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
                break;
            case BGITERATOR_ITEM_FLUSHDB:
                it->flushdb_queued--;
                break;

            case BGITERATOR_ITEM_COMPLETE:
                // This can only happen if the completion item has been enqueued and
                //  the iterator is terminated before reaching the completion item.
                itemFreeList_returnItemBackToFreeList(item);
                continue;   // Skip pushing this onto itemsToReturn

            case BGITERATOR_ITEM_TERMINATED:
                // This can only happen if there is a race when terminating between
                //  the iteration client and main thread.
                itemFreeList_returnItemBackToFreeList(item);
                continue;   // Skip pushing this onto itemsToReturn

            default:
                serverAssert(false);
        }

        fifoPush(itemsToReturn, item);
    }
    fifoRelease(poppedFifo);

    // Now release items all at once...
    if (fifoLength(itemsToReturn) > 0) {
        mutexQueueAddMultiple(it->return_to_valkey, itemsToReturn);
    }
    fifoRelease(itemsToReturn);
}



//=============================================================================================
//                        Foreground support functions (private)
//=============================================================================================

static size_t replicationItemSize(bgIteratorItem *item) {
    serverAssert(item->type == BGITERATOR_ITEM_REPLICATION);
    size_t itemSize = sizeof(bgIteratorItem);
    for (int i = 0;  i < item->u.repl.argc;  i++) {
        itemSize += objectComputeSize(NULL, item->u.repl.argv[i], 0, 0);
    }
    return itemSize;
}

static void processReturnOfItemToValkey(bgIteratorItem *item, bgIterator *iter) {
    serverAssert(onValkeyMainThread());
    switch ((int)item->type) {
        case BGITERATOR_ITEM_REPLICATION:
            bufferedReplicationBytes -= replicationItemSize(item);
            freeRobjArray(item->u.repl.argc, item->u.repl.argv);
            break;

        case BGITERATOR_ITEM_DBENTRY:
            {
                if (item->u.dbe.is_cloned) {
                    freeClonedDictEntry(item->u.dbe.de);
                } else {
                    if (isEntryInuseBySingleIterator(item->u.dbe.de)) {
                        // This blocking mechanism isn't the best.  Written for slot-migration,
                        //  it assumes a single DB so if the same key appears in multiple DBs,
                        //  commands might get unblocked only to get blocked again.  (This would
                        //  happen only rarely, and with minimal impact.)
                        robj key;
                        initStaticStringObject(key, objectGetKey(item->u.dbe.de));
                        amzUnblockClientsOnKey(NULL, &key);
                    }
                    // resumeRehashing must be called before decrementEntryInuse, since decrementEntryInuse can free
                    if (item->u.dbe.is_rehashing_paused) resumeRehashing(item->u.dbe.de);
                    decrementEntryInuse(item->u.dbe.de);
                }
            }
            break;

        case BGITERATOR_ITEM_SWAPDB:
        case BGITERATOR_ITEM_FLUSHDB:
            break;

        case BGITERATOR_ITEMEXT_ITER_CLOSED:
            {
                bgIterator *it = ((bgIteratorItemExtClose*)item)->iter;
                serverAssert(it == iter);
                if (it->terminated) {
                    // Abnormal termination
                    //  Normally the item is TERMINATED, but might be COMPLETE in race
                    serverAssert(it->current_item->type == BGITERATOR_ITEM_TERMINATED
                            || it->current_item->type == BGITERATOR_ITEM_COMPLETE);
                    // Release any items stranded on the iterator after early termination
                    returnAllItemsToValkey(it);
                    receiveItemsBackFromOneIterator(it);
                } else {
                    // Normal completion
                    serverAssert(it->current_item->type == BGITERATOR_ITEM_COMPLETE);
                }
                serverAssert(mutexQueueLength(it->items_for_iterator) == 0);
                serverAssert(it->dbentries_queued      == it->dbentries_processed);
                serverAssert(it->replication_queued    == it->replication_processed);
                serverAssert(it->swapdb_queued         == it->swapdb_processed);
                serverAssert(it->flushdb_queued        == it->flushdb_processed);
                serverAssert(it->dbentry_clones_queued >= it->dbentry_clones_processed);

                listEmpty(curCmdMissingKeys);   // Just in case any remain

                itemFreeList_returnItemBackToFreeList(it->current_item);
                it->current_item = NULL;

                bool terminated = it->terminated;
                void *privdata = it->privdata;
                bgIteratorCleanupFunc cleanup = it->cleanup;
                bgIteratorRelease(it);  // Fully release the iterator before calling cleanup

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
            }
            break;

        default:
            serverAssert(false);    // Not expecting any other type of item!
    }

    // We don't allocate extension items from the pool so we manually free them
    if((int)item->type == BGITERATOR_ITEMEXT_ITER_CLOSED) {
        zfree(item);
    } else {
        itemFreeList_returnItemBackToFreeList(item);
    }
}

static void prepareAndProcessReturnedItems(int n, bgIteratorItem **items, bgIterator *iter) {
    int i = 0;
    for (i = 0; i < n; i++) valkey_prefetch(items[i]);
    for (i = 0; i < n; i++) {
        if (items[i]->type != BGITERATOR_ITEM_DBENTRY) continue;
        // Prefetch can have a significant perf hit on NULL
        // but we never expect items[i]->u.dbe.de to be NULL
        valkey_prefetch(items[i]->u.dbe.de);
    }
    for (i = 0; i < n; i++) {
        if (items[i]->type != BGITERATOR_ITEM_DBENTRY) continue;
        // Same as above, assume key is never NULL
        valkey_prefetch(objectGetKey(items[i]->u.dbe.de));
    }
    for (i = 0; i < n; i++) processReturnOfItemToValkey(items[i], iter);
}

#define PREFETCH_BATCH_SIZE 16

static bool receiveItemsBackFromOneIterator(bgIterator *it) {
    bgIteratorItem* batchPool[PREFETCH_BATCH_SIZE];
    int n = 0;
    // Returns true if we process at least one item from
    // a given iterator's return_to_valkey queue, false otherwise.
    fifo *poppedFifo = mutexQueuePopAll(it->return_to_valkey, false);
    if (poppedFifo != NULL) {
        while (fifoLength(poppedFifo) > 0) {
            fifoPop(poppedFifo, (void **)&batchPool[n++]);
            if (n == PREFETCH_BATCH_SIZE) {
                prepareAndProcessReturnedItems(n, batchPool, it);
                n = 0;
            }
        }
        if (n > 0) {
            prepareAndProcessReturnedItems(n, batchPool, it);
        }
        fifoRelease(poppedFifo);
        return true;
    }
    return false;
}

static void receiveItemsBackFromIterators(bool blocking) {
    // Process each iterator's return_to_valkey queue
    // If `blocking` is true, continue reading until
    // at least one queue was not empty.
    serverAssert(onValkeyMainThread());
    listIter li;
    listNode *node;
    bool processedItems = false;
    do {
        listRewind(allIterators, &li);
        while ((node = listNext(&li)) != NULL) {
            bgIterator *it = listNodeValue(node);
            processedItems |= receiveItemsBackFromOneIterator(it);
        }
        if (blocking) usleep(100);    // Sleep for 1ms and re-try processing iterators
    } while (blocking && !processedItems);
}


static long long bgIteration_feedIterators_task(
        struct aeEventLoop *eventLoop,
        long long id,
        void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);
    serverAssert(onValkeyMainThread());

    static monotime lastFeedEndTime;    // STATIC: Persists For checking starvation
    monotime startTime = getMonotonicUs();

    if (!bgIteration_iterationActive()) {
        // No more iterators exist.  Self-check, and terminate the "feed" task.
        serverAssert(dictSize(nameToIterator) == 0);
        serverAssert(dictSize(inUseEntries) == 0);
        serverAssert(bufferedReplicationBytes == 0);

        dictShrink(inUseEntries, 0);
        itemFreeList_release();

        bgIterator_timeproc_id = AE_DELETED_EVENT_ID;
        lastFeedEndTime = 0;
        return AE_NOMORE;
    }

    long dutyTimeUs = BGITER_CYCLE_BUDGET_MS * 1000;
    if (lastFeedEndTime > 0) {
        // If the timer was delayed, compute the proportional time we should have had, and increase
        //  the duty cycle to compensate (up to a limit).
        long starvationUs = (startTime - lastFeedEndTime) - BGITER_CYCLE_DELAY_MS * 1000;
        if (starvationUs > 0) {
            long starvationCompensationUs = starvationUs * BGITER_CYCLE_BUDGET_MS
                / (BGITER_CYCLE_BUDGET_MS + BGITER_CYCLE_DELAY_MS);
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
    // For unit testing, force the item_count_target to 1 in each call.  This ensures that we only
    //  feed a minimal amount to the iterators rather than a non-deterministic amount.
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
    // For any given command, the command may or may not need to be replicated based on the status
    //  and flags of each iterator.  Furthermore, if a command does need to be replicated, this
    //  replication must occur for an entire atomic unit; we can't replicate only part of a script
    //  or multi/exec.
    // This function is the only place where the replication flag is cleared.

    if (c->flag.multi || c->flag.script) {
        // REGARDING MULTI/EXEC
        // --------------------
        // When processing a MULTI/EXEC, blockClientIfRequired is called first for the MULTI.  Then,
        //  all of the commands are queued up in server.c:processCommand().  It's only when EXEC is
        //  encountered, that server.c:call() is fired to begin execution.
        // AFTER the EXEC is processed by call(), then each of the commands in the MULTI/EXEC block
        //  will be processed through call().
        // If write commands are present, MULTI & EXEC will be passed to the replication stream
        //  before/after the transaction commands.  Note that MULTI & EXEC are not actually
        //  "executed" at the time when their replication is passed to the replication stream.
        //
        // Example:  MULTI; SET A B; EXEC
        //  1. blockClientIfRequired() called for MULTI.  MULTI flag IS NOT set.  (Won't block.)
        //  2. blockClientIfRequired() called for EXEC.  MULTI flag IS set.  (Might block.)
        //  3. blockClientIfRequired() called for SET.  MULTI flag IS set.  (Won't block.)
        //  4. handleCommandReplication() is called for MULTI.
        //  5. handleCommandReplication() is called for SET.
        //  6. handleCommandReplication() is called for EXEC.
        //
        // SO - if the MULTI flag is set, we DON'T clear the flag.  It should only be cleared at the
        //  start of the transaction, when MULTI is received - and the flag isn't set yet.

        // REGARDING SCRIPTS
        // -----------------
        // When processing a script, blockClientIfRequired is called first for the EVAL/EVALSHA/FCALL.
        //  Then, all of the commands are processed using a special script client.  The script
        //  client has the CLIENT_SCRIPT flag set.  For scripts, the replication flag is set when
        //  processing the EVAL/EVALSHA/FCALL and should not be cleared when executing individual
        //  commands in the script.

        // If it's the EXEC command, we fall through and clear the flag below.  But for all other
        //  commands within the transaction, we don't clear the flag.
        if (c->cmd->proc != execCommand) return;
    }

    // For most commands, the replication flag is cleared and we determine if replication is needed
    //  based on the keys being used and their state in each iterator. If a modified key hasn't been
    //  processed yet, there's no need to expedite the key or send the replication.  The key will be
    //  sent later, when reached by the iterator.
    // However, for scripts, it is not possible to perform this optimization.  There is no way to
    //  know if an undeclared key might be modified.  Since the entire script needs to be replicated
    //  (or not replicated) atomically, we can't take the chance that an undeclared key might be
    //  hit which requires replication.
    bool isScript = isScriptCallWriteCmd(c->cmd);

    getKeysResult result;
    initGetKeysResult(&result);
    getKeysFromCommand(c->cmd, c->argv, c->argc, &result);

    // [sm-bgiterator] TODO: ELMO-108525, This assumes all keys are in the same slot, should consider cross-slot script case.
    sds check_key = (result.numkeys > 0) ? objectGetVal(c->argv[result.keys[0].pos]) : NULL;

    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        if (it->completed || it->terminated) {
            it->cur_cmd_may_replicate = false;
        } else {
            // Set initial state of the replication flag for this transaction
            // For full scan iterators, write commands within scripts must always be replicated.
            // For cluster slot iterators, replication of script write commands depends on whether
            // the key is in scope of the current iterator.
            it->cur_cmd_may_replicate = isScript && it->keyset_iter->isKeyInScope(it->keyset_iter, check_key);
        }
    }
    getKeysFreeResult(&result);
}


static void handleSwapdb(int db1, int db2) {
    serverAssert(onValkeyMainThread());
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
            mutexQueueAdd(it->items_for_iterator, item);
        }
    }
}


static void removePtrFromEarlyIterate(dbEntry *de) {
    // If the item is being released, let's get the pointer out of our early_iterate_entries.
    //  Note that this is not strictly necessary, but it frees some memory and keeps the
    //  dictionary small.
    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        dictDelete(it->early_iterate_entries, de);   // just try delete (might not be here)
    }
}


static int findDbForEntry(dbEntry *de) {
    for (int i = 0;  i < server.dbnum;  i++) {
        if (dbFind(server.db[i], objectGetKey(de)) == de) return i;
    }
    serverAssert(false);    // the entry MUST be in one of the DBs
}


static void terminateIteratorForFlush(bgIterator *it, int dbid) {
    if (!it->terminated) bgIteratorTerminate(it);

    // Snoop on the iterator.  There might be 1 item still being processed.  If that item is in the
    //  DB being flushed, the item is removed from the dict and held for deferred deletion.  This
    //  allows the iterator to complete processing on the current item without the item being
    //  deleted unexpectedly.
    // Since this is running in parallel with a background thread, the results are volatile.  This
    //  is OK as when the iterator completes processing the item, it still won't have been accepted
    //  back to Valkey yet, meaning the item will still be in inUseEntries.
    bgIteratorItem *item = it->current_item;
    if (item && item->type == BGITERATOR_ITEM_DBENTRY) {
        dbEntry *de = item->u.dbe.de;
        int deDb = findDbForEntry(de);
        if (dbid == -1 || dbid == deDb) {
            removePtrFromEarlyIterate(de);
        }
    }
}


static void preserveIteratorItemsForFlush(bgIterator *it, int dbid) {
    serverAssert(onValkeyMainThread());
    serverAssert(!(it->iteration_flags & BGITERATOR_FLAG_CONSISTENT));
    serverAssert(dbid >= 0);
    // Since this is not a consistent iteration, it's OK if the early_iterate_entries contains
    //  pointers to items being deleted.  The item is not actually accessed from the pointer.  And
    //  if the pointer gets reused for a new item, there's no guarantee that we would iterate it
    //  anyway.  If replication is enabled, both new items and early_iterate_entries are treated the
    //  same (replication is processed).  So this is safe in all cases.
    // Given this, we will just worry about preserving items in the iterator's processing queue.
    //  Because of commands like SWAPDB and MOVE, there's no attempt to remove unnecessary items
    //  from the queue.  This is also safer to future Valkey extensions.

    // Temporarily yank all items from the iterator's queue
    fifo *poppedFifo = mutexQueuePopAll(it->items_for_iterator, false);
    if (poppedFifo != NULL) {
        fifo *readdFifo = fifoCreate();
        while(fifoLength(poppedFifo) > 0) {
            bgIteratorItem *item;
            fifoPop(poppedFifo, (void **)&item);
            if (item->type == BGITERATOR_ITEM_DBENTRY) {
                dbEntry *de = item->u.dbe.de;
                if (dbFind(server.db[dbid], objectGetKey(de)) == de) {
                  // Found the entry in the DB about to be flushed
                  removePtrFromEarlyIterate(de);
                }
            }
            fifoPush(readdFifo, item);
        }
        fifoRelease(poppedFifo);

        // Now give the list back to the iterator
        mutexQueueAddMultiple(it->items_for_iterator, readdFifo);
        fifoRelease(readdFifo);
    }

    // And snoop on the active item.  Even if the background task finishes with this item as we look
    //  at it, the item can't have been returned to Valkey yet.
    bgIteratorItem *item = it->current_item;
    if (item && item->type == BGITERATOR_ITEM_DBENTRY) {
        dbEntry *de = item->u.dbe.de;
        if (dbFind(server.db[dbid], objectGetKey(de)) == de) {
          // Found the entry in the DB about to be flushed
          removePtrFromEarlyIterate(de);
        }
    }
}


static bool isDbSignificant(int dbid) {
    unsigned long long totalKeys = 0;
    for (int i = 0;  i < server.dbnum;  i++) {
        totalKeys += dbSize(server.db[i]);
    }
    return dbSize(server.db[dbid]) > totalKeys / 2;
}


static void handleFlushdb(int dbid) {
    // Invoked BEFORE the actual flush.  -1 indicates FLUSHALL.
    bool should_abort_iterators = server.cluster_enabled || dbid == -1 || isDbSignificant(dbid);

    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);

        if (should_abort_iterators || it->iteration_flags & BGITERATOR_FLAG_CONSISTENT) {
            terminateIteratorForFlush(it, dbid);
        } else {
            // In this (limited) case, we're only flushing a single DB that contains < half the
            //  keys.  We don't want to kill a full-sync replication.  We will just continue with
            //  iteration, knowing that a replication client will also receive the FLUSHDB on the
            //  replication stream.
            // It would be nice to do this with consistent snapshot also, but given that this is a
            //  very rare condition, development is not justified to save off the DB for deferred
            //  delete.  This would add a lot of complexity as well as memory implications.
            preserveIteratorItemsForFlush(it, dbid);
            it->keyset_iter->flushDb(it->keyset_iter, dbid);

            // Send a flushdb event to notify the client
            if (BGITERATION_DEBUG) {
                debugBuffer = sdscatprintf(debugBuffer, "FLUSH: %d\n", dbid);
            }

            bgIteratorItem *item = itemFreeList_getElementOrAllocate();
            item->type = BGITERATOR_ITEM_FLUSHDB;
            item->dbid = dbid;
            it->flushdb_queued++;
            mutexQueueAdd(it->items_for_iterator, item);
        }
    }
    receiveItemsBackFromIterators(false);   // Receive items back before flushing the items
}


static bool expediteKeysForWriteOnAllIterators(
        int dbid,
        struct serverCommand *cmd,
        int argc,
        robj **argv,
        keyReference *keyrefs,
        int numKeys,
        dict *waitingOnKeys) {
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


static bool expediteKeysForMultiExec(client *c, dict *waitingOnKeys) {
    serverAssert(c->cmd->proc == execCommand);

    /* For MULTI/EXEC, Valkey buffers all of the commands until hitting the EXEC.
     * At this point, the client holds all of the commands to be executed.  This function searches
     * for all of the keys used by any of the buffered write commands.  In addition, if SWAPDB or
     * SELECT is used, this tracks the DBIDs through various swap/select operations.
     */

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
     * call.
     */
    bool initiallyAnIteratorWillReplicate = anIteratorWillReplicateForThisCommand();

    bool mustBlock = false;
    int *cur_to_orig_db = NULL;

    int curDb = c->db->id;
    for (int cmdNum = 0;  cmdNum < c->mstate->count;  cmdNum++) {
        struct serverCommand *cmd = c->mstate->commands[cmdNum].cmd;
        robj **argv = c->mstate->commands[cmdNum].argv;
        int argc = c->mstate->commands[cmdNum].argc;

        if (cmd->proc == swapdbCommand) {
            int id1, id2;
            if (getParamsForSwapdb(argc, argv, c, &id1, &id2)) {
                if (cur_to_orig_db == NULL) {
                    cur_to_orig_db = zmalloc(sizeof(int) * server.dbnum);
                    for (int i = 0;  i < server.dbnum;  i++) cur_to_orig_db[i] = i;
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
        if (numkeys == 0) continue;         // Write command with no keys - like FLUSHDB

        if (expediteKeysForWriteOnAllIterators(
                cur_to_orig_db ? cur_to_orig_db[curDb] : curDb,
                cmd, argc, argv, keyrefs, numkeys, waitingOnKeys)) {
            mustBlock = true;
        }
        getKeysFreeResult(&result);
    }

    zfree(cur_to_orig_db);

    if (!initiallyAnIteratorWillReplicate && anIteratorWillReplicateForThisCommand()) {
        // We've decided to replicate.  Re-process the MULTI/EXEC just once more to make sure that
        //  we didn't miss any keys at the beginning.  This can't continue to recurse because
        //  `initiallyAnIteratorWillReplicate` will be TRUE in the recursive call.  Note that the
        //  recursive call may add additional entries to `waitingOnKeys`.
        if (expediteKeysForMultiExec(c, waitingOnKeys)) mustBlock = true;
    }

    return mustBlock;
}

static bgIterator * bgIteratorCreate(
        const char *name,
        int flags,
        bgIteratorReplDoneFunc repldone,
        bgIteratorCleanupFunc cleanup,
        void *privdata,
        bgIterationType iter_type,
        genericIterator *keyset_iter) {
    serverAssert(onValkeyMainThread());
    serverAssert(server.cluster_enabled || iter_type == BGITERATION_TYPE_FULLSCAN);
    serverAssert(server.cluster_enabled                 // Don't allow CONSISTENT & REPLICATION
            || !(flags & BGITERATOR_FLAG_CONSISTENT)    //  unless cluster mode (avoids
            || !(flags & BGITERATOR_FLAG_REPLICATION)); //  complications with SWAPDB & FLUSHDB)

    bgIterator *it = zmalloc(sizeof(bgIterator));
    it->name = sdsnew(name);
    it->repldone = repldone;
    it->cleanup = cleanup;
    it->privdata = privdata;
    it->items_for_iterator = mutexQueueCreate();
    it->return_to_valkey = mutexQueueCreate();

    // Floor queue size to bgiteration_queue_increase_incr or use last queue size value
    if (last_item_count_target < BGITER_QUEUE_INCREASE_INCR) {
        last_item_count_target = BGITER_QUEUE_INCREASE_INCR;
    }
    it->item_count_target = last_item_count_target;
    it->iteration_flags = flags;
    it->iteration_type = iter_type;
    it->consistent_modification_id = bgIteration_epoch++;
    it->keyset_iter = keyset_iter;
    it->early_iterate_entries = dictCreate(&dictEntryPtrDictType);
    dictExpand(it->early_iterate_entries, BGITER_EARLY_ITERATE_DICT_INITIAL_SIZE);
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

    elapsedStart(&it->monotonic_start_time);
    it->monotonic_item_start_time = 0;


    if (bgIterator_timeproc_id <= 0) {
        // If iteration is not currently active, start the feeding task.  (Runs in main thread.)
        bgIterator_timeproc_id = aeCreateTimeEvent(server.el, 1, bgIteration_feedIterators_task, NULL, NULL);
        serverAssert(bgIterator_timeproc_id != AE_ERR);
    }

    if (dictAdd(nameToIterator, (void*)it->name, it) != DICT_OK) {
        // Can't have 2 iterators with the same name!
        serverAssert(false);
    }

    listAddNodeTail(allIterators, it);

    dictExpand(inUseEntries, listLength(allIterators) * it->item_count_target);

    return it;
}



//=============================================================================================
//                        PUBLIC INTERFACE:  Iterator creation and use
//=============================================================================================

// PUBLIC API
bgIterator * bgIteratorCreateFullScanIter(
        const char *name,
        int flags,
        bgIteratorReplDoneFunc repldone,
        bgIteratorCleanupFunc cleanup,
        void *privdata) {
    return bgIteratorCreate(name, flags, repldone, cleanup, privdata, BGITERATION_TYPE_FULLSCAN,
                            fullScanIteratorCreate());
}

// PUBLIC API
bgIterator * bgIteratorCreateSlotsIter(
        const char *name,
        int flags,
        const int *slots,
        int slots_count,
        bgIteratorReplDoneFunc repldone,
        bgIteratorCleanupFunc cleanup,
        void *privdata) {
    return bgIteratorCreate(name, flags, repldone, cleanup, privdata, BGITERATION_TYPE_CLUSTERSLOT,
                            clusterSlotIteratorCreate(slots, slots_count));
}

// PUBLIC API
bgIterator * bgIteratorFind(const char *name) {
    serverAssert(onValkeyMainThread());

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
    status->dbentries_queued      = it->dbentries_queued;
    status->dbentries_processed   = it->dbentries_processed;
    status->replication_queued    = it->replication_queued;
    status->replication_processed = it->replication_processed;
    status->swapdb_queued         = it->swapdb_queued;
    status->swapdb_processed      = it->swapdb_processed;
    status->flushdb_queued        = it->flushdb_queued;
    status->flushdb_processed     = it->flushdb_processed;
    status->dbentry_clones_queued = it->dbentry_clones_queued;
    status->dbentry_clones_processed = it->dbentry_clones_processed;

    status->queue_length = mutexQueueLength(it->items_for_iterator);
    status->queue_length_target = it->item_count_target;

    status->runtime_ms = elapsedMs(it->monotonic_start_time);

    monotime nonvolatile_item_start_time = it->monotonic_item_start_time;
    status->current_item_ms =
            (nonvolatile_item_start_time == 0) ? 0 : elapsedMs(nonvolatile_item_start_time);
}


// PUBLIC API
void bgIteratorTerminate(bgIterator *it) {
    serverAssert(onValkeyMainThread());

    // Remove any items in the queue, but doesn't affect the 1 item that's being processed.
    returnAllItemsToValkey(it);

    // We have to add an item, just in case the READER is waiting on the mutex.
    if (BGITERATION_DEBUG) {
        debugBuffer = sdscat(debugBuffer, "SENDING TERMINATE\n");
    }

    bgIteratorItem *terminationItem = itemFreeList_getElementOrAllocate();
    *terminationItem = (bgIteratorItem){ .type = BGITERATOR_ITEM_TERMINATED };
    mutexQueueAdd(it->items_for_iterator, terminationItem);

    it->terminated = true;
}


// PUBLIC API
bool bgIteratorIsTerminating(bgIterator *it) {
    return it->terminated;
}


// PUBLIC API
bgIteratorItem * bgIteratorRead(bgIterator *it) {
    serverAssert(it->current_item == NULL
            || (it->current_item->type != BGITERATOR_ITEM_COMPLETE
                && it->current_item->type != BGITERATOR_ITEM_TERMINATED));

    // First, clean up the previous item read
    if (it->current_item != NULL) {
        returnCurrentItemToValkey(it);

        // To support unit tests.  Normal clients call bgIteratorRead from an alternate thread.
        //  Without this, a unit test could get stuck waiting on the completion event because
        //  feed won't get invoked.  For production, this is called regularly from the main thread.
        if (onValkeyMainThread()) bgIteration_feedIterators_task(NULL, 0, NULL);
    } else {
        it->client_is_active = true;
    }

    it->monotonic_item_start_time = 0;  // idle until blocking pop returns
    it->current_item = mutexQueuePop(it->items_for_iterator, true);
    it->monotonic_item_start_time = getMonotonicUs();

    return it->current_item;
}


// PUBLIC API
void bgIteratorClose(bgIterator *it) {
    if (it->current_item != NULL) {
        if (it->current_item->type == BGITERATOR_ITEM_COMPLETE
         || it->current_item->type == BGITERATOR_ITEM_TERMINATED) {
            // Normal confirmation of background completion
        } else {
            // Client is initiating the termination
            it->terminated = true;
            returnCurrentItemToValkey(it);

            it->current_item = itemFreeList_getElementOrAllocate();
            *(it->current_item) = (bgIteratorItem){ .type = BGITERATOR_ITEM_TERMINATED };
        }
    } else {
        // terminated before first item read
        it->terminated = true;
        it->current_item = itemFreeList_getElementOrAllocate();
        *(it->current_item) = (bgIteratorItem){ .type = BGITERATOR_ITEM_TERMINATED };
    }

    // We don't allocate extension items from the free list
    bgIteratorItemExtClose *itemClose = zmalloc(sizeof(bgIteratorItemExtClose));
    itemClose->type = BGITERATOR_ITEMEXT_ITER_CLOSED;
    itemClose->iter = it;
    mutexQueueAdd(it->return_to_valkey, itemClose);
}



//=============================================================================================
//                        PUBLIC INTERFACE:  Valkey main-thread support hooks
//=============================================================================================

// PUBLIC API
void bgIteration_init(void) {
    serverAssert(onValkeyMainThread());

    /* This should be called once and only once from the Valkey main thread.  However to support
     * unit tests, this is not validated, and multiple invocations are ignored.  */
    if (nameToIterator) return;     // If already initialized, ignore (unit tests)

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
void bgIteration_keyDelete(int dbid, const sds key) {
    if (!bgIteration_iterationActive()) return;
    serverAssert(onValkeyMainThread());

    if (BGITERATION_DEBUG) {
        debugBuffer = sdscatprintf(debugBuffer, "KEYDEL: (%d)%s\n", dbid, key);
    }

    dbEntry *de = dbFind(server.db[dbid], key);
    if (de == NULL) return;

    // For consistent iterators, we need to make sure the item gets written before delete
    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        if (it->completed || it->terminated || !it->keyset_iter->isKeyInScope(it->keyset_iter, key)) continue;

        if (it->iteration_flags & BGITERATOR_FLAG_CONSISTENT
                && ((bgIterationEntryMetadata *)objectGetMetadata(de))->iterator_epoch <= it->consistent_modification_id) {
            if (!it->keyset_iter->hasPassedItem(it->keyset_iter, key, dbid)
                    && !(dictFind(it->early_iterate_entries, de) != NULL)) {
                addEarlyIterationKey(it, de, dbid); // (may also add to inUseEntries)
            }
        }
    }

    removePtrFromEarlyIterate(de);

    // We might be within the context of a command execution.  This happens if the key is found to
    //  be expired when attempting to execute the command.  In this case, we should treat the key as
    //  missing.  If the key exists after the command executes, we can treat it like a new key.
    // (If not in command execution, this is ok - it's reset at the beginning of command execution.)
    robj *oKey = createObject(OBJ_STRING, sdsdup(key));
    listAddNodeHead(curCmdMissingKeys, oKey);
}


// PUBLIC API
// Notify bgIteration that a FLUSHALL is being performed outside of the normal client interface.
void bgIteration_flushall(void) {
    handleFlushdb(-1);
}


// PUBLIC API
bool bgIteration_blockClientIfRequired(client *c) {
    serverAssert(onValkeyMainThread());
    if (!bgIteration_iterationActive()) return false;
    if (!isWriteCmd(c->cmd)) return false;

    if (BGITERATION_DEBUG) {
        debugBuffer = sdscatprintf(debugBuffer, "BLCK?: (%d)%s\n", c->db->id,
                createSdsFromClientArgv(c->argc, c->argv));
    }

    // Before executing a command or atomic transaction, the replication flag is cleared for each
    //  iterator.  If it's determined that the command should replicate, the flag will be set
    //  as the command and keys are examined for expedite.
    resetReplicationFlagForIterators(c);

    if (c->cmd->proc == flushdbCommand || c->cmd->proc == flushallCommand) {
        // Handle flush commands prior to execution
        int flags;
        if (getFlushCommandFlags(c, &flags) == C_OK) {
            // The command parsed ok - we WILL flush
            handleFlushdb((c->cmd->proc == flushdbCommand) ? c->db->id : -1);
        }
    }

    bool mustBlock = false;
    dict *waitOnKeys = dictCreate(&tempKeysetDictType);     // dict of robj(sds)->NULL
    listEmpty(curCmdMissingKeys);

    if (c->cmd->proc == execCommand) {
        mustBlock = expediteKeysForMultiExec(c, waitOnKeys);
        // JHB - HACK because no blocking yet
        while (mustBlock) {
            receiveItemsBackFromIterators(true);    // Blocking
            dictEmpty(waitOnKeys, NULL);
            mustBlock = expediteKeysForMultiExec(c, waitOnKeys);
        }
        // JHB - END HACK
    } else {
        getKeysResult result;
        initGetKeysResult(&result);
        int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
        keyReference *keyrefs = result.keys;
        if (numkeys > 0) {
            mustBlock = expediteKeysForWriteOnAllIterators(
                            c->db->id, c->cmd, c->argc, c->argv, keyrefs, numkeys, waitOnKeys);
            serverAssert(!(mustBlock && (c->flag.multi) && !(c->flag.script)));

            //if (mustBlock && (c->flag.script)) {
            if (mustBlock) { // JHB HACK - do this for everything because of missing blocking
                /* For scripts, we will block for keys declared in EVAL/EVALSHA/FCALL.
                 *  However, scripts are NOT required to declare keys.  Even if it declares keys,
                 *  it's not declaring the DB for the key.  After a SELECT or SWAPDB, we might be on
                 *  a key we haven't blocked for.  In this case, there is no option but to execute a
                 *  synchronous block and wait for the iterator(s) to be done with the key(s).
                 *  (Yuck.)  */
                while (mustBlock) {
                    receiveItemsBackFromIterators(true);    // Blocking
                    dictEmpty(waitOnKeys, NULL);
                    mustBlock = expediteKeysForWriteOnAllIterators(
                                    c->db->id, c->cmd, c->argc, c->argv, keyrefs, numkeys, waitOnKeys);
                }
            }
            getKeysFreeResult(&result);
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
    }

    if (mustBlock) {
        serverAssert(dictSize(waitOnKeys) > 0);
        robj **waitKeysArgv = zmalloc(sizeof(robj*) * dictSize(waitOnKeys));

        dictEntry *de;
        dictIterator *di = dictGetIterator(waitOnKeys);
        unsigned long argvCount = 0;
        while((de = dictNext(di)) != NULL) {
            waitKeysArgv[argvCount++] = dictGetKey(de);
        }
        dictReleaseIterator(di);
        serverAssert(argvCount == dictSize(waitOnKeys));

        int rc = amzBlockClientOnKeys(NULL, c, waitKeysArgv, argvCount);
        serverAssert(rc == C_OK);

        zfree(waitKeysArgv);
    }

    dictRelease(waitOnKeys);

    if (BGITERATION_DEBUG) {
        if (mustBlock) debugBuffer = sdscat(debugBuffer, " (blocked)\n");
    }

    return mustBlock;
}


// PUBLIC API
void bgIteration_handleCommandReplication(
        int dbid,
        struct serverCommand *cmd,
        int argc,
        robj **argv) {
    if (BGITERATION_DEBUG) {
        // DEBUG - enable this to capture replication not queued because iteration is inactive
        if (0 && !bgIteration_iterationActive() && (isWriteCmd(cmd) || cmd->proc == multiCommand)) {
            debugBuffer = sdscatprintf(debugBuffer, "REPL? INACT: (%d)%s\n", dbid,
                    createSdsFromClientArgv(argc, argv));
        }
    }

    if (!bgIteration_iterationActive()) return;
    serverAssert(onValkeyMainThread());

    // Some commands are replicated which are not writes (like publish) these can be ignored.
    //  Be careful with MULTI which is not a write command, but must be replicated.
    if (!isWriteCmd(cmd) && cmd->proc != multiCommand) return;

    if (BGITERATION_DEBUG) {
        debugBuffer = sdscatprintf(debugBuffer, "REPL?: (%d)%s\n", dbid,
                createSdsFromClientArgv(argc, argv));
    }

    if (cmd->proc == swapdbCommand) {
        // All iterators and clients must be informed of swapdb
        int id1, id2;
        // command has been processed, but Valkey allows "swapdb 0 0" (which can be ignored)
        if (getParamsForSwapdb(argc, argv, NULL, &id1, &id2))
            handleSwapdb(id1, id2);
    }

    // In the case that a key is touched in a different DB (COPY/MOVE) the key is recorded as
    //  a "special" key and than handled below.
    int special_dbid = 0;
    sds special_key = NULL;
    dbEntry *special_dbEntry = NULL;
    if (cmd->proc == moveCommand) {
        // The MOVE command succeeded.  However MOVE requires special handling as it creates a new
        //  key in a different database.  We need to make sure that we don't later try to iterate
        //  on the key as it would be a duplicate key at that point.  So, instead, we will mark the
        //  newly created key as "early iterated".
        bool success = getDbIdFromRobj(argv[MOVE_COMMAND_DBID_ARG_INDEX], &special_dbid);
        serverAssert(success);  // the command already succeeded, so this should work!

        robj *oKey = argv[1];
        special_key = (sds)objectGetVal(oKey);

        special_dbEntry = dbFind(server.db[special_dbid], special_key);
    }
    if (cmd->proc == copyCommand) {
        // The COPY command succeeded.  However COPY requires special handling (like MOVE).
        bool success = getTargetDbIdForCopyCommand(argc, argv, dbid, &special_dbid);
        serverAssert(success);  // the command already succeeded, so this should work!

        // Find the newly created entry.
        robj *oKey = argv[2];
        special_key = (sds)objectGetVal(oKey);

        special_dbEntry = dbFind(server.db[special_dbid], special_key);
    }

    /* Implementation note regarding LUA and MULTI:  LUA scripts and MULTI-EXEC blocks must be
     *  treated atomically.  We need to ensure that either ALL of the replication (or none of the
     *  replication) for the atomic operation is processed by the iterator(s).  This is handled
     *  naturally as we can only "complete" the iteration during the feeding process - and feeding
     *  is only performed when handling timer events (after the LUA/MULTI has completed).  */

    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        if (it->completed || it->terminated) continue;

        // For consistent iteration, we only iterate values based on version.  But for
        //  non-consistent iteration, we don't need to explicitly iterate any values newly created
        //  during the iteration.  So we mark them as expedited.  We know we have a new key if it
        //  was missing before the command, and exists now.
        if (!(it->iteration_flags & BGITERATOR_FLAG_CONSISTENT)) {
            // Handle the special case of a key moved to a different DB
            if (special_dbEntry != NULL) {
                if (it->cur_cmd_may_replicate
                        && !it->keyset_iter->hasPassedItem(it->keyset_iter, special_key, special_dbid)) {
                    dictAdd(it->early_iterate_entries, special_dbEntry, NULL);
                    if (BGITERATION_DEBUG) {
                        sds entryString = createEntryString(special_dbid, special_dbEntry);
                        debugBuffer = sdscatprintf(debugBuffer, "EARLY(special): %s\n", entryString);
                        sdsfree(entryString);
                    }
                }

                // Note: In the cases where there's a special command, we are copying or moving an
                //       item to a different DB.  In these limited cases, we can only possibly be
                //       creating a single key.  And if we've handled it here, we don't need to
                //       handle it as a "missing key" below.  If we were to try to handle it as a
                //       standard "missing key", we would get the DBID incorrect.
            } else if (listLength(curCmdMissingKeys) > 0) {
                listIter missingIt;
                listNode *missingNode;
                listRewind(curCmdMissingKeys, &missingIt);
                while ((missingNode = listNext(&missingIt)) != NULL) {
                    robj *oKey = listNodeValue(missingNode);
                    const sds key = objectGetVal(oKey);
                    dbEntry *de = dbFind(server.db[dbid], key);
                    if (de != NULL) {
                        // It exists now!
                        if (it->cur_cmd_may_replicate
                                && !it->keyset_iter->hasPassedItem(it->keyset_iter, key, dbid)) {
                            // If the current command is allowed to replicate, and there is a new
                            //  key which we haven't yet reached in iteration, it needs to be added
                            //  to the set of early iterate entries.  (We know that it's not already
                            //  in that set because it's a newly created key!)
                            dictAdd(it->early_iterate_entries, de, NULL);
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
         *  held inside bgIteration until no longer needed.
         *  Even though the entry is not physically deleted yet, it is logically deleted and it is
         *  safe to replicate the DEL/UNLINK.  Since iterators process items FIFO, the replication
         *  for DEL/UNLINK won't actually get processed until other queued replication is processed.
         *
         * In the case of a client driven DEL command, the key will have already been deleted when
         *  we hit this routine.  In the case of EXPIRE/EVICT, they propagate happens before the key
         *  is deleted.  So if the key is missing, we can use the cached replication decision.  But
         *  if the key still exists (indicating EXPIRE/EVICT) we evaluate it specially.
         */
        bool shouldReplicateDelCommand = false;
        bool isDelCommand = isDeleteCmd(cmd);
        if (isDelCommand) {
            sds key = objectGetVal(argv[1]);
            if (it->keyset_iter->isKeyInScope(it->keyset_iter, key)) {
                dbEntry *de = dbFind(server.db[dbid], key);
                if (de) {
                    // NOTE:  It's weird, but helpful, for both EXPIRE and EVICT the propagation happens
                    //        BEFORE the actual delete.  So if the dbEntry still exists, we are doing
                    //        an expire/evict which is not preceded by blockClientIfRequired().
                    if (it->keyset_iter->hasPassedItem(it->keyset_iter, key, dbid)
                            || (dictFind(it->early_iterate_entries, de) != NULL)) {
                        shouldReplicateDelCommand = true;
                    }
                } else {
                    // The dbEntry has already been deleted, this must be part of normal command
                    //  processing.
                    shouldReplicateDelCommand = it->cur_cmd_may_replicate;
                }
            }
        }

        bool replicate = (it->iteration_flags & BGITERATOR_FLAG_REPLICATION &&
                ((!isDelCommand && it->cur_cmd_may_replicate)
                        || shouldReplicateDelCommand));

        if (replicate) {
            /* We will replicate the command in these cases:
             * 1) For consistent iteration - it->cur_cmd_may_replicate is always true
             * 2) For non-consistent, if any of the keys have been processed, expediteKeysForWrite
             *    will ensure that ALL of the keys have been expedited - and we should replicate
             * 3) For non-consistent, if NONE of the keys have been processed, no need to replicate
             */

            if (BGITERATION_DEBUG) {
                debugBuffer = sdscat(debugBuffer, " (queued)\n");
            }

            bgIteratorItem *item = itemFreeList_getElementOrAllocate();
            item->type = BGITERATOR_ITEM_REPLICATION;
            item->dbid = dbid;
            item->u.repl.cmd = cmd;
            item->u.repl.argv = cloneRobjArray(argc, argv);
            item->u.repl.argc = argc;
            bufferedReplicationBytes += replicationItemSize(item);
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
    serverAssert(onValkeyMainThread());
    return isEntryInuseByAnyIterator(de);
}


// PUBLIC API
uint32_t bgIteration_getEpoch(void) {
    return bgIteration_epoch;
}


// PUBLIC API
void bgIteration_updateDbEntryPtr(dbEntry *old, dbEntry *new) {
    if (!bgIteration_iterationActive() || old == new) return;
    serverAssert(onValkeyMainThread());
    serverAssert(!isEntryInuseByAnyIterator(old));

    listIter li;
    listNode *node;
    listRewind(allIterators, &li);
    while ((node = listNext(&li)) != NULL) {
        bgIterator *it = listNodeValue(node);
        if (dictDelete(it->early_iterate_entries, old) == DICT_OK) {
            if (BGITERATION_DEBUG) {
                debugBuffer = sdscatprintf(debugBuffer, "EARLY LIST UPDATE %p -> %p\n", (void *)old, (void *)new);
            }
            dictAdd(it->early_iterate_entries, new, NULL);
        }
    }
}
