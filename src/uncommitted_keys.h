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

/**
 * Initialize sync replication related fields for a database.
 */
void syncReplicationInitDatabase(struct serverDb *db);

/**
 * Handle a dirty key for a given client.
 * @param c The calling client. NULL if the key becomes dirty outside a client command (i.e. expiry/eviction)
 * @param key The key object
 * @param db The database
 */
void handleUncommittedKeyForClient(const struct client *c, struct serverObject *key, struct serverDb *db);

/**
 * Retrieve the uncommitted replication offset for a given key, purge the given
 * key from uncommitted keys set if the replication offset has been committed.
 * @return the ACK offset of the key if key is uncommitted, returns -1 otherwise.
 */
long long syncReplicationPurgeAndGetUncommittedKeyOffset(sds key, struct serverDb *db);

/**
 * Clears all uncommitted DBs and keys that are properly acknowledged by
 * sufficient number of replicas.
 */
void clearUncommittedKeysAcknowledged(void);

/**
 * Clear all uncommitted keys for each database.
 */
void clearAllUncommittedKeys(void);

/**
 * Get the number of uncommitted keys across all databases.
 */
unsigned long long getNumberOfUncommittedKeys(void);

/**
 * Calculate cleanup time limit based on number of uncommitted keys.
 */
unsigned long long getUncommittedKeysCleanupTimeLimit(unsigned long long num_uncommitted_keys);

/*================================= Database Modification Tracking =========== */

/**
 * Handle database-level modification commands (FLUSHDB, FLUSHALL, SWAPDB).
 */
void handleDatabaseModification(struct client *c);

/**
 * Process pending uncommitted data (keys, databases, function store)
 * after a transaction completes.
 */
void processPendingUncommittedData(long long blocking_repl_offset);

/**
 * Initialize the pending uncommitted data structures.
 */
void uncommittedKeysInitPending(void);

/**
 * Clean up the pending uncommitted data structures.
 */
void uncommittedKeysCleanupPending(void);

/*================================= Uncommitted Data Access Checks =========== */

/**
 * Checks if we should reject a command that is accessing uncommitted data.
 */
bool shouldRejectCommandWithUncommittedData(struct client *c);

/**
 * Determine if a client is trying to access uncommitted keys.
 */
int isAccessingUncommittedData(struct client *c);

/**
 * Determine if there are uncommitted keys in the server.
 */
int hasUncommittedKeys(void);

/**
 * Determines if a single command is trying to access an uncommitted key.
 */
int isSingleCommandAccessingUncommittedKeys(const struct serverDb *db, struct serverCommand *cmd, struct serverObject **argv, int argc);

/*================================= Command parameter helpers ================ */

/* Renamed from amzSwapdbGetParams */
bool swapdbGetParams(struct serverObject **argv, int argc, int *id1_p, int *id2_p);
/* Renamed from amzSelectGetParams */
bool selectGetParams(struct serverObject **argv, int argc, struct client *permission_client, int *dbid_p);
/* Renamed from amzGetDbIdFromRobj */
bool getDbIdFromRobj(struct serverObject *obj, int *db_id);
/* Renamed from amzGetTargetDbIdForCopyCommand */
bool getTargetDbIdForCopyCommand(int argc, struct serverObject **argv, int selected_dbid, int *target_dbid);
bool commandModifiesFirstKeyOnly(struct serverCommand *cmd);

#endif /* UNCOMMITTED_KEYS_H */
