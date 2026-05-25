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

/* data-bytes metric (per-type logical byte accounting).
 *
 * Tracks `sdslen(key) + <value logical size>` per (slot, type). The pair
 * is type-agnostic: the dispatcher in cluster_slot_stats.c decides how to
 * size each object kind. Currently OBJ_STRING is the only kind wired up.
 *
 * Lifecycle pattern at db.c primitives:
 *   - On new key      -> Add(slot, key, val)
 *   - On overwrite    -> Sub(slot, key, old); Add(slot, key, new)
 *   - On delete       -> Sub(slot, key, val) (before freeing)
 *   - On in-place     -> Sub(slot, key, val); ... mutate ... ; Add(slot, key, val)
 */
void clusterSlotStatsAddMemory(int slot, sds key, robj *val);
void clusterSlotStatsSubMemory(int slot, sds key, robj *val);
/* Convenience wrappers for command-level callers that have the key as a
 * `robj *` and would otherwise have to compute the slot themselves. The
 * lower-level slot/sds variants above stay available for db.c primitives,
 * which already have dict_index in hand from their kvstore lookup. */
void clusterSlotStatsAddMemoryForKey(robj *key, robj *val);
void clusterSlotStatsSubMemoryForKey(robj *key, robj *val);
/* Returns the logical size (key_bytes + value_bytes) for a given (key, val)
 * pair, or 0 for untracked types. Used by lookupKeyWrite to snapshot the
 * "before" size for in-place mutation tracking. */
uint64_t clusterSlotStatsGetObjectSize(sds key, robj *val);
/* Bulk reset, used by FLUSHDB / FLUSHALL / emptyData. Does not touch the
 * cumulative cpu_usec / network_bytes_* counters. */
void clusterSlotStatsResetDataBytesAll(void);

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

/* Subtract `b` from `*a`, clamping to zero. Used for data_bytes counters
 * which can be zeroed by clusterSlotStatReset during slot migration before
 * all keys are physically removed. */
static inline void saturated_sub(uint64_t *a, uint64_t b) {
    *a = (*a >= b) ? *a - b : 0;
}
