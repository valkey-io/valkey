#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"

/* General use-cases. */
void clusterSlotStatReset(int slot);
void clusterSlotStatResetAll(void);
int clusterSlotStatsEnabled(int slot);

/* cpu-usec metric. */
void clusterSlotStatsAddCpuDuration(client *c, ustime_t duration);

/* network-bytes-in metric. */
void clusterSlotStatsAddNetworkBytesInForUserClient(client *c);

/* network-bytes-out metric. */
void clusterSlotStatsAddNetworkBytesOutForSlot(int slot, unsigned long long net_bytes_out);
void clusterSlotStatsAddNetworkBytesOutForUserClient(client *c);
void clusterSlotStatsIncrNetworkBytesOutForReplication(long long len);
void clusterSlotStatsDecrNetworkBytesOutForReplication(long long len);
void clusterSlotStatsAddNetworkBytesOutForShardedPubSubInternalPropagation(client *c, int slot);

/* memory-bytes metric (per-slot allocated byte accounting).
 *
 * Tracks the allocated footprint (`zmalloc_size(val)` plus the type's external
 * allocations) per (db, slot), in the metadata of the slot's hashtable in
 * db->keys. The accounting is type-agnostic: the dispatcher in
 * cluster_slot_stats.c decides how to size each object kind. Currently
 * OBJ_STRING and OBJ_STREAM are wired up.
 *
 * Lifecycle pattern at db.c primitives:
 *   - On new key      -> Add(db, slot, key, val)
 *   - On overwrite    -> Sub(db, slot, key, old); Add(db, slot, key, new)
 *   - On delete       -> Sub(db, slot, key, val) (before freeing)
 *   - On in-place     -> snapshot at lookupKeyWrite, delta at signalModifiedKey
 */
void clusterSlotStatsAddMemory(serverDb *db, int slot, robj *key, robj *val);
void clusterSlotStatsSubMemory(serverDb *db, int slot, robj *key, robj *val);
/* `sds` key variants, for callers that hold the key as an sds, or that pass
 * NULL because the key is already embedded in `val` (counted by zmalloc_size). */
void clusterSlotStatsAddMemorySdsKey(serverDb *db, int slot, sds key, robj *val);
void clusterSlotStatsSubMemorySdsKey(serverDb *db, int slot, sds key, robj *val);
/* Returns the allocated size for a given (key, val) pair, or 0 for untracked
 * types or when cluster mode is off. */
uint64_t clusterSlotStatsGetObjectSize(robj *key, robj *val);
/* Applies the change from `before` to the current size of `val`, for values
 * reallocated outside dbSetValue (e.g. setExpire). */
void clusterSlotStatsUpdateMemory(serverDb *db, int slot, robj *key, uint64_t before, robj *val);

/* Per-key in-place mutation snapshot (one set for the whole server).
 *
 * Write-lookups snapshot each modified key's "before" size via
 * clusterSlotStatsSnapshotKey(); db-hooks that overwrite a value in place
 * refresh it via clusterSlotStatsRefreshKey() so the eventual signalModifiedKey
 * sees a zero delta; deletes drop the snapshot via clusterSlotStatsForgetKey();
 * and signalModifiedKey applies the delta via clusterSlotStatsCommitKey().
 * Entries of a command are dropped when that command ends, and all entries in
 * afterSleep and in the AOF load loop (clusterSlotStatsClearSnapshot). All are
 * no-ops when cluster mode is off. */
void clusterSlotStatsSnapshotKey(serverDb *db, int slot, robj *key, robj *val);
void clusterSlotStatsRefreshKey(serverDb *db, robj *key, robj *val);
void clusterSlotStatsForgetKey(serverDb *db, robj *key);
void clusterSlotStatsCommitKey(serverDb *db, robj *key);
/* Applies any pending in-place change of `key` (current value `val`) before a
 * db-hook subtracts the current size. Used by dbSetValue and the delete path. */
void clusterSlotStatsSettleKey(serverDb *db, robj *key, robj *val);
void clusterSlotStatsInitSnapshot(void);
void clusterSlotStatsClearSnapshot(int level);
