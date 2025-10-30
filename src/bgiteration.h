#ifndef __BGITERATION_H
#define __BGITERATION_H

#include <stdbool.h>
#include "sds.h"

/* A mechanism for creating iteration clients which iterate over the main dictionary in a
 * background thread.
 *
 * This mechanism passes keys to the iteration client, while blocking the keys from write by the
 * Valkey main thread.  Once an iteration client is done with a key, it is returned to the Valkey
 * main thread and any pending writers are unblocked.
 *
 * A bgIterator must be created on the main Valkey thread, and then passed to another thread which
 * implements the logic of the iteration client.
 *
 * Iteration clients are expected to read through the keyspace until the iteration is complete or
 * terminated.  An iteration client may not perform modifications on a key.
 *
 * Future enhancement:  Certain types of modifications may be passed back to the Valkey main thread.
 *                      Use case: A background compression thread wants to compress a string value.
 */

/* Avoids dependency on server.h */
typedef struct serverObject dbEntry;    // An object with key/value inserted into main dictionary
typedef struct serverObject robj;       // An object with a value used for command parameters
typedef struct client client;

/* The bgIterator is an opaque structure.  */
typedef struct bgIterator bgIterator;


/* Flag indicates that a consistent iteration is required.  This is used to create a point-in-time
 * iteration.  The iteration client will see all keys AS THEY EXISTED at the time when the iterator
 * was created.
 * Note:  The DBID provided with the DICTENTRY events is the original DBID (at the time of iteration
 *        start).  SWAPDB events are NOT provided during a consistent iteration.  */
#define BGITERATOR_FLAG_CONSISTENT (1 << 0)

/* Flag indicating that the replication stream for keys which have already been processed should be
 * forwarded to the iteration client.  Most useful for non-consistent iteration to track changes
 * to keys already processed.  By tracking changes, this allows an non-consistent iteration client
 * to achieve a consistent view at the END of the iteration.
 * NOTE:  Replication events will be provided ordered and synchronized with any SWAPDB events.
 * LIMITATION:  Since SWAPDB events are not provided during CONSISTENT iteration, it is not
 *              permitted to use both CONSISTENT and REPLICATION on a non-clustermode instance.  */
#define BGITERATOR_FLAG_REPLICATION (1 << 1)


/* When running an iterator with replication, a replication-done function (callback) may be
 * provided.  This function will be executed after the last replication item has been fed into the
 * queue for the client.  This function will be run on the Valkey main thread, and allows a client
 * to recognize the point where no additional replication data will be sent for processing.
 *
 * PRIVDATA:    this pointer is for data private to the iteration client.
 *
 * Returns true when an iterator stops accepting any replication item into the queue for the client.
 * If false is returned, replication will continue, and bgiteration will periodically call the callback
 * until true is returned. In this context, returning false indicates that the client is not ready to
 * stop receiving replication, it is requesting that replication be continued.
 */
typedef bool (*bgIteratorReplDoneFunc)(void *privdata);


/* When creating a bgIterator, a cleanup function (callback) may be provided.  This function will be
 * executed once iteration has completed and this will run on the Valkey main thread.
 *
 * TERMINATED:  will be passed as TRUE if the iteration process was terminated early (either by
 *              the main thread calling bgIteratorTerminate() or the iteration client calling
 *              bgIteratorClose()).
 * PRIVDATA:    this pointer is for data private to the iteration client.
 */
typedef void (*bgIteratorCleanupFunc)(bool terminated, void *privdata);


/* Create a background full-scan iterator (bgIterator).
 * This bgIterator will iterate through the entire keyspace (across all DBs).
 *
 * NAME:        a human readable name for the iterator (must be unique)
 * FLAGS:       creation flags indicate iteration options
 * REPLDONE:    if provided, called after the last replication item has been queued (on the Valkey main thread)
 * CLEANUP:     if provided, called at the end of iteration (on the Valkey main thread)
 * PRIVDATA:    passed to cleanup function
 *
 * This method creates and initializes the bgIterator.  It does not perform any thread management.
 * It is expected that the main Valkey thread will call this method, and then start a new thread to
 * to implement the iteration client which will read from the returned bgIterator.
 *
 * There is no need to delete/destroy a bgIterator.  It will automatically be cleaned up after the
 * last item is read.
 */
bgIterator * bgIteratorCreateFullScanIter(
        const char *name,
        int flags,
        bgIteratorReplDoneFunc repldone,
        bgIteratorCleanupFunc cleanup,
        void *privdata);


/* Create a background slots iterator (bgIterator).
 * This bgIterator will iterate through the keys belonging to a set of cluster slots.
 *
 * NAME:        a human readable name for the iterator (must be unique)
 * FLAGS:       creation flags indicate iteration options
 * SLOTS:       array of cluster slots to iterate over
 * SLOTS_COUNT: size of the array of slots
 * REPLDONE:    if provided, called after the last replication item has been queued (on the Valkey main thread)
 * CLEANUP:     if provided, called at the end of iteration (on the Valkey main thread)
 * PRIVDATA:    passed to cleanup function
 *
 * This method creates and initializes the bgIterator.  It does not perform any thread management.
 * It is expected that the main Valkey thread will call this method, and then start a new thread to
 * to implement the iteration client which will read from the returned bgIterator.
 *
 * The caller of this function has the ownership of the `slots` array's memory. This function will
 * just copy its data and leave the array untouched.
 *
 * There is no need to delete/destroy a bgIterator.  It will automatically be cleaned up after the
 * last item is read.
 */
bgIterator * bgIteratorCreateSlotsIter(
        const char *name,
        int flags,
        const int *slots,
        int slots_count,
        bgIteratorReplDoneFunc repldone,
        bgIteratorCleanupFunc cleanup,
        void *privdata);


/* Find an existing bgIterator by name.
 * Returns NULL if the iterator does not exist (or has completed).
 */
bgIterator * bgIteratorFind(const char *name);


/* Get the name of an existing iterator.  */
const char * bgIteratorName(bgIterator *iter);


/* Struct to retrieve status information for an active iteration client.  */
typedef struct {
    unsigned long dbentries_queued;         // Cumulative BGITERATOR_ITEM_DBENTRY queued
    unsigned long dbentries_processed;      // Cumulative BGITERATOR_ITEM_DBENTRY processed
    unsigned long replication_queued;       // Cumulative BGITERATOR_ITEM_REPLICATION queued
    unsigned long replication_processed;    // Cumulative BGITERATOR_ITEM_REPLICATION processed
    unsigned long swapdb_queued;            // Cumulative BGITERATOR_ITEM_SWAPDB queued
    unsigned long swapdb_processed;         // Cumulative BGITERATOR_ITEM_SWAPDB processed
    unsigned long flushdb_queued;           // Cumulative BGITERATOR_ITEM_FLUSHDB queued
    unsigned long flushdb_processed;        // Cumulative BGITERATOR_ITEM_FLUSHDB processed
    unsigned long dbentry_clones_queued;    // A subset of dbentries_queued for cloned entries
    unsigned long dbentry_clones_processed; // A subset of dbentries_processed for cloned entries
    unsigned long queue_length;             // Current length of queue to iteration client
    unsigned long queue_length_target;      // Dynamic target length for queue to iteration client
    unsigned long runtime_ms;               // Time, in milliseconds, that iterator has been running
    unsigned long current_item_ms;          // Time, in milliseconds, spent processing current item
} bgIteratorStatus;


/* Get the status of a background iteration.
 *
 * The caller-provided bgIteratorStatus will be populated.
 */
void bgIteratorGetStatus(bgIterator *iter, bgIteratorStatus *status);


/* Terminate a background iteration.
 *
 * An iteration is terminated by the Valkey main thread.  It is expected that the iteration client
 * will continue to read, receiving BGITERATOR_ITEM_TERMINATED or BGITERATOR_ITEM_COMPLETE to
 * complete the iteration.  (This is necessary to ensure proper cleanup.)
 * NOTE:  If the iteration client wants to terminate iteration, it may call bgIteratorClose().
 */
void bgIteratorTerminate(bgIterator *iter);


/* Check if an iterator is being terminated.
 *
 * This checks if the iterator is in the process of terminating.  For the Valkey main thread, this
 * can be used to determine if a call has already been made to bgIteratorTerminate.  For an
 * iteration client, it normally learns about terminate by reading the next item, this allows
 * out-of-band detection of termination which can be useful when processing a large key.
 */
bool bgIteratorIsTerminating(bgIterator *iter);


typedef enum {
    /* Indicates that the iteration has completed normally.  No more items to read.
     * If replication is enabled, on completion, the final replication offset is recorded in
     *  'u.master_repl_offset' and 'dbid' is set to the selected replication db.  The iteration
     *  client will have received all *applicable* replication data to this point.  */
    BGITERATOR_ITEM_COMPLETE = 1,

    /* Indicates that the iteration has been terminated before completion.  No more items to read.*/
    BGITERATOR_ITEM_TERMINATED,

    /* A dbEntry for DB=dbid.
     * NOTE:  The dbEntry MAY be expired.  It is up to the client to decide how to handle
     *        expired entries.  */
    BGITERATOR_ITEM_DBENTRY,

    /* A replication command for DB=dbid.  cmd, argv, & argc provided.
     * NOTE:  The command may have been re-written before replication.  */
    BGITERATOR_ITEM_REPLICATION,

    /* A SWAPDB event.  dbid swapped with dbid2.
     * Note that SWAPDB events are not provided during consistent iteration.  */
    BGITERATOR_ITEM_SWAPDB,

    /* A FLUSHDB event.  In most cases, iteration will be terminated, and this event will NOT be
     * sent.  However, in the case of a single minor DB being flushed, non-consistent iteration is
     * permitted to continue.  */
    BGITERATOR_ITEM_FLUSHDB
} bgIteratorItemType;


typedef struct {
    dbEntry *de;
    bool is_cloned;
    bool is_rehashing_paused;
} dbEntryData;

typedef struct {
    struct serverCommand *cmd;
    robj **argv;
    int argc;
} replicationData;

typedef struct {
    bgIteratorItemType type;
    int dbid;       /* orig DB ID for CONSISTENT, queue-time DB ID for !CONSISTENT.  */
    union {
        dbEntryData dbe;                // for BGITERATOR_ITEM_DBENTRY
        replicationData repl;           // for BGITERATOR_ITEM_REPLICATION
        long long master_repl_offset;   // for BGITERATOR_ITEM_COMPLETE
        int dbid2;                      // for BGITERATOR_ITEM_SWAPDB
    } u;
} bgIteratorItem;


/* Read the next bgIteratorItem from the bgIterator.
 *
 * The iteration client is expected to call this function in a loop.  After reading
 * BGITERATOR_ITEM_COMPLETE or BGITERATOR_ITEM_TERMINATED, the iteration client must call
 * bgIteratorClose to finalize the iteration process.
 *
 * This is a blocking call.  If the main Valkey thread has been too busy to send items to the
 * iterator, the iteration client's queue may run dry and this call will block until data is
 * available.
 *
 * NOTE: Reading an item returns previously read items to Valkey.  It is unsafe to reference an item
 * previously read.
 *
 * (All memory management is the responsibility of the bgIterator - not the reader.)
 */
bgIteratorItem * bgIteratorRead(bgIterator *iter);


/* Close the bgIterator, allowing the bgIterator to be deallocated.
 *
 * This must be called by an iteration client to release the bgIterator.
 *
 * It is required that this is called after receiving BGITERATOR_ITEM_COMPLETE or
 * BGITERATOR_ITEM_TERMINATED and signals that the background activity is complete.
 *
 * This may also be called by the iteration client to force terminate an iteration early.  The
 * bgIterator will be marked as terminated.
 */
void bgIteratorClose(bgIterator *iter);


/********************************************************************************************
 * BGITERATION HOOKS REQUIRED TO SUPPORT ITERATION - CALLS INSERTED INTO MAIN VALKEY CODE
 ********************************************************************************************/

typedef struct {
    uint32_t iterator_epoch;    // iterator epoch of last modification
} bgIterationEntryMetadata;


/* Must be called once (and only once) at server startup.  */
void bgIteration_init(void);


/* Returns true if any iterators are currently active. */
bool bgIteration_iterationActive(void);


/* Notify bgIteration that a key is being deleted.  In Valkey, key deletion can occur in a READ
 * command if the key is expired.  Note that this notification is more about status than memory.
 * Since the dbEntry is a reference counted object, the dbEntry can't be physically deleted if
 * bgIteration is still actively using it.
 */
void bgIteration_keyDelete(int dbid, const sds key);


/* Iteration needs to know if a FLUSHALL is being performed.  For normal clients, this comes through
 * the standard "blockClientIfRequired" interface.  This interface is for cases where Valkey
 * performs the FLUSHALL operation independently of clients (e.g. when syncing with master).
 */
void bgIteration_flushall(void);


/* Updating value or expiration of an existing key may lead to reallocation of the dbEntry (robj).
 * BgIteration keeps track of expedited keys (by pointer) to avoid repeated iteration.  BgIteration
 * must be notified when dbEntries are reallocated.  BgIteration will not dereference the pointers;
 * it is safe to have deallocated the old dbEntry before calling this function.
 * 
 * We can't update the dbEntry if the entry is actually in use (bgIteration_isEntryInuse)!
 *
 * To simplify calling code, this function does nothing if old_entry == new_entry.
 */
void bgIteration_updateDbEntryPtr(dbEntry *old_entry, dbEntry *new_entry);


/* Before executing any command, the Valkey main thread must call this function.  If the key(s) are
 * blocked for writes by an iterator, the function returns true and the client is blocked.  A
 * blocked client will be unblocked once the key becomes available for write.
 *
 * This should be called for all commands - even commands which are executed as part of a MULTI/EXEC
 * or LUA script.
 *
 * For MULTI/EXEC - This function is called when hitting the EXEC - after all of the commands
 *                  have been queued.  This may block the EXEC, but will NOT block individual
 *                  commands as they are executed in the MULTI/EXEC block.
 *
 * For LUA script - This function is first called for EVAL/EVALSHA.  It may block the script while
 *                  waiting on declared keys.  However, if the script accesses undeclared keys or
 *                  performs SWAPDB, a synchronous block may be performed (returning false) on
 *                  individual commands within the script.
 *
 * Note: this function should be called for all commands (not just writes).
 */
bool bgIteration_blockClientIfRequired(client *c);


/* After execution of a write command, the Valkey main thread must provide the command to iterators
 * which are interested in the replication feed.  It is required that all commands have been passed
 * through bgIteration_blockClientIfRequired(), however, it is permitted that the command can be
 * re-written for propagation.
 */
void bgIteration_handleCommandReplication(
        int dbid,
        struct serverCommand *cmd,
        int argc,
        robj **argv);


/* The memory that bgIteration uses while temporarily buffering replication data is not included in
 * the maxmemory computation used for eviction.  This function provides insight into the current
 * amount of memory used for buffered replication data.
 */
size_t bgIteration_memoryInuseForReplication(void);


/* Check if a dbEntry is currently in-use/locked by bgIteration. */
bool bgIteration_isEntryInuse(dbEntry *de);


/* Get the current iteration epoch, for tagging metadata on keys. */
uint32_t bgIteration_getEpoch(void);

#endif
