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
 * allocations) summed per slot. The accounting is type-agnostic: the dispatcher
 * in cluster_slot_stats.c decides how to size each object kind. Currently
 * OBJ_STRING and OBJ_STREAM are wired up.
 *
 * Lifecycle pattern at db.c primitives:
 *   - On new key      -> Add(slot, key, val)
 *   - On overwrite    -> Sub(slot, key, old); Add(slot, key, new)
 *   - On delete       -> Sub(slot, key, val) (before freeing)
 *   - On in-place     -> Sub(slot, key, val); ... mutate ... ; Add(slot, key, val)
 */
void clusterSlotStatsAddMemory(int slot, robj *key, robj *val);
void clusterSlotStatsSubMemory(int slot, robj *key, robj *val);
/* `sds` key variants, for callers that hold the key as an sds, or that pass
 * NULL because the key is already embedded in `val` (counted by zmalloc_size). */
void clusterSlotStatsAddMemorySdsKey(int slot, sds key, robj *val);
void clusterSlotStatsSubMemorySdsKey(int slot, sds key, robj *val);
/* Convenience wrappers for command-level callers that have the key as a
 * `robj *` and would otherwise have to compute the slot themselves. The
 * lower-level slot variants above stay available for db.c primitives,
 * which already have dict_index in hand from their kvstore lookup. */
void clusterSlotStatsAddMemoryForKey(robj *key, robj *val);
void clusterSlotStatsSubMemoryForKey(robj *key, robj *val);
/* Returns the allocated size for a given (key, val) pair, or 0 for untracked
 * types. Used by lookupKeyWrite to snapshot the "before" size for in-place
 * mutation tracking. */
uint64_t clusterSlotStatsGetObjectSize(robj *key, robj *val);
/* Bulk reset, used by FLUSHDB / FLUSHALL / emptyData. Does not touch the
 * cumulative cpu_usec / network_bytes_* counters. */
void clusterSlotStatsResetMemoryBytesAll(void);

/* Per-key in-place mutation snapshot.
 *
 * A command's write-lookups snapshot each modified key's "before" size via
 * clusterSlotStatsSnapshotKey(); db-hooks that overwrite a value in place
 * refresh it via clusterSlotStatsRefreshKey() so the eventual signalModifiedKey
 * sees a zero delta; deletes drop the snapshot via clusterSlotStatsForgetKey();
 * and signalModifiedKey applies the delta and drops the snapshot via
 * clusterSlotStatsCommitKey(). The whole set is cleared at each command
 * boundary (clusterSlotStatsClearSnapshot) and released on client free
 * (clusterSlotStatsFreeSnapshot). All are no-ops when cluster mode is off. */
void clusterSlotStatsSnapshotKey(client *c, int slot, robj *key, robj *val);
void clusterSlotStatsRefreshKey(client *c, robj *key, robj *val);
void clusterSlotStatsForgetKey(client *c, robj *key);
void clusterSlotStatsCommitKey(client *c, serverDb *db, robj *key);
void clusterSlotStatsInitSnapshot(client *c);
void clusterSlotStatsClearSnapshot(client *c);
void clusterSlotStatsFreeSnapshot(client *c);

/* Subtract `b` from `*a`, clamping to zero. Used for memory_bytes counters
 * which can be zeroed by clusterSlotStatReset during slot migration before
 * all keys are physically removed. */
static inline void saturated_sub(uint64_t *a, uint64_t b) {
    *a = (*a >= b) ? *a - b : 0;
}
