#ifndef UNCOMMITTED_KEYS_H
#define UNCOMMITTED_KEYS_H

#include <stdbool.h>
#include "sds.h"

struct client;
struct serverObject;
struct serverDb;
struct serverCommand;

/* Note: robj is typedef'd in server.h as `typedef struct serverObject robj;`
 * We use struct serverObject * in declarations here to avoid duplicate typedefs. */

/*================================= Uncommitted Key Tracking ================= */

// Initialize reply-blocking-related fields for a database.
void replyBlockingInitDatabase(struct serverDb *db);

/* Record a key modified by any writer, fed from signalModifiedKey(). The key is
 * marked dirty with a placeholder offset; stampPendingUncommittedData() applies
 * the real offset at the end of the execution unit. */
void trackModifiedKey(struct serverDb *db, struct serverObject *key);

/* True when keys or databases dirtied in the current execution unit wait for
 * stampPendingUncommittedData(). Used by the post-execution-unit pending-work
 * gate so the stamp is never skipped. */
bool hasPendingUncommittedData(void);

/* Retrieve the uncommitted replication offset for a given key.
 * Returns -1 if the key is not tracked or has already been committed
 * (offset <= previous_acked_offset). Does NOT purge — cleanup is handled
 * by drainCommittedKeys().
 * @param key The key to look up
 * @param db The database
 * @param previous_acked_offset The last durably committed offset (passed by caller) */
long long getUncommittedKeyOffset(sds key, struct serverDb *db, long long previous_acked_offset);

/* Remove keys from uncommitted_keys when their offset has been durably committed.
 * Called from beforeSleep / notifyReplyBlockingProgress. */
void drainCommittedKeys(long long committed_offset);

// Clear all uncommitted keys for each database.
void clearAllUncommittedKeys(void);

// Get the number of uncommitted keys across all databases.
unsigned long long getNumberOfUncommittedKeys(void);

/*================================= Database Modification Tracking =========== */

// Handle database-level modification commands (FLUSHDB, FLUSHALL, SWAPDB).
void handleDatabaseModification(struct client *c);

/* Apply the final replication offset to the keys and databases dirtied in the
 * execution unit that just completed. Called after propagation, so the offset
 * is final. */
void stampPendingUncommittedData(long long blocking_repl_offset);

// Initialize the pending uncommitted data structures.
void uncommittedKeysInitPending(void);

// Clean up the pending uncommitted data structures.
void uncommittedKeysCleanupPending(void);

// Determine if there are uncommitted keys in the server.
int hasUncommittedKeys(void);

#endif /* UNCOMMITTED_KEYS_H */
