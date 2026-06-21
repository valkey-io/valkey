/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "cluster_slot_stats.h"

#define UNASSIGNED_SLOT 0

typedef enum {
    KEY_COUNT,
    CPU_USEC,
    NETWORK_BYTES_IN,
    NETWORK_BYTES_OUT,
    MEMORY_BYTES,
    SLOT_STAT_COUNT,
    INVALID
} slotStatType;

/* -----------------------------------------------------------------------------
 * Per-slot byte accounting (memory-bytes metric).
 *
 * Adding a new type later means adding a case to slotStatsObjectSize() that
 * returns its allocated byte size. All the wiring in db.c stays type-agnostic.
 * -------------------------------------------------------------------------- */

/* Returns 1 when per-slot memory-bytes accounting is active. We gate this on
 * cluster_enabled only (not cluster_slot_stats_enabled), because memory_bytes
 * is a state metric that must remain consistent with the database contents
 * regardless of the more expensive cumulative metrics being toggled. */
static inline int isMemoryBytesAccountingEnabled(int slot) {
    return server.cluster_enabled && slot >= 0 && slot < CLUSTER_SLOTS;
}

/* Returns the allocated memory footprint in bytes that the (`key`, `val`) entry
 * contributes to its slot's memory-bytes counter, or 0 for untracked types.
 * `key` may be NULL when it is already embedded in `val` and thus counted as
 * part of the value allocation. */
static uint64_t slotStatsObjectSize(sds key, robj *val) {
    uint64_t asize = zmalloc_size(val);
    /* Key sds: already inside zmalloc_size(val) when embedded; add it
     * otherwise. (key may be NULL for callers that pass only the value.) */
    if (!val->hasembkey && key != NULL) asize += sdsAllocSize(key);
    switch (val->type) {
    case OBJ_STRING:
        /* EMBSTR/INT keep the value inside the robj (already counted). RAW
         * has a separate sds allocation. */
        if (val->encoding == OBJ_ENCODING_RAW) asize += sdsAllocSize(objectGetVal(val));
        return asize;
    case OBJ_STREAM: {
        /* streamMemUsage() covers the stream struct, its data radix tree, and
         * the listpacks in that tree. Consumer group memory is not yet counted. */
        stream *s = objectGetVal(val);
        return asize + streamMemUsage(s);
    }
    default:
        return 0;
    }
}

/* The `SdsKey` variants take the key as a raw `sds`. They are the right choice
 * when the caller only has the key as an sds (e.g. dbAddRDBLoad), or when the
 * key has already been embedded into `val` (in which case pass NULL: the
 * embedded key is counted by zmalloc_size(val) inside slotStatsObjectSize). */
void clusterSlotStatsAddMemorySdsKey(int slot, sds key, robj *val) {
    if (!isMemoryBytesAccountingEnabled(slot)) return;
    uint64_t sz = slotStatsObjectSize(key, val);
    if (sz == 0) return;
    server.cluster->slot_stats[slot].memory_bytes += sz;
}

void clusterSlotStatsSubMemorySdsKey(int slot, sds key, robj *val) {
    if (!isMemoryBytesAccountingEnabled(slot)) return;
    uint64_t sz = slotStatsObjectSize(key, val);
    if (sz == 0) return;
    saturated_sub(&server.cluster->slot_stats[slot].memory_bytes, sz);
}

/* The `robj *` key variants forward to the SdsKey variants. These are the
 * right choice in db.c primitives, which hold the key as a `robj *` and have
 * dict_index in hand from their kvstore lookup. */
void clusterSlotStatsAddMemory(int slot, robj *key, robj *val) {
    clusterSlotStatsAddMemorySdsKey(slot, key ? objectGetVal(key) : NULL, val);
}

void clusterSlotStatsSubMemory(int slot, robj *key, robj *val) {
    clusterSlotStatsSubMemorySdsKey(slot, key ? objectGetVal(key) : NULL, val);
}

/* Convenience wrappers that derive the slot from the key and forward to the
 * lower-level slot variants. Use these from command-level code that doesn't
 * already have dict_index in hand. */
void clusterSlotStatsAddMemoryForKey(robj *key, robj *val) {
    clusterSlotStatsAddMemory(getSlotForKey(objectGetVal(key)), key, val);
}

void clusterSlotStatsSubMemoryForKey(robj *key, robj *val) {
    clusterSlotStatsSubMemory(getSlotForKey(objectGetVal(key)), key, val);
}

uint64_t clusterSlotStatsGetObjectSize(robj *key, robj *val) {
    return slotStatsObjectSize(key ? objectGetVal(key) : NULL, val);
}

/* -----------------------------------------------------------------------------
 * Per-key in-place mutation snapshot.
 *
 * The set lives on the client (client.slot_memory_bytes). Two entries are stored
 * inline; a third distinct key migrates the inline entries into an overflow
 * list and everything is then kept there. Cluster rejects cross-slot commands,
 * so all entries share a single slot, recorded once.
 * -------------------------------------------------------------------------- */

#define SLOT_MEMORY_BYTES_INLINE 2

/* listRelease/listEmpty free callback for overflow entries. */
static void slotSnapFreeEntry(void *p) {
    slotMemoryBytesSnap *e = p;
    decrRefCount(e->key);
    zfree(e);
}

/* Find the entry tracking `key`, or NULL. Searches the overflow list when
 * spilled, otherwise the inline array. */
static slotMemoryBytesSnap *slotSnapFind(slotMemoryBytesSnapshot *s, robj *key) {
    sds skey = objectGetVal(key);
    if (s->overflow != NULL) {
        listIter li;
        listNode *ln;
        listRewind(s->overflow, &li);
        while ((ln = listNext(&li)) != NULL) {
            slotMemoryBytesSnap *e = listNodeValue(ln);
            if (sdscmp(objectGetVal(e->key), skey) == 0) return e;
        }
        return NULL;
    }
    for (int i = 0; i < s->inlined_count; i++) {
        if (sdscmp(objectGetVal(s->inlined[i].key), skey) == 0) return &s->inlined[i];
    }
    return NULL;
}

/* Remove the entry tracking `key`, if any. Resets the shared slot to -1 once
 * the set becomes empty so the cheap `slot < 0` guards short-circuit. */
static void slotSnapRemove(slotMemoryBytesSnapshot *s, robj *key) {
    sds skey = objectGetVal(key);
    if (s->overflow != NULL) {
        listIter li;
        listNode *ln;
        listRewind(s->overflow, &li);
        while ((ln = listNext(&li)) != NULL) {
            slotMemoryBytesSnap *e = listNodeValue(ln);
            if (sdscmp(objectGetVal(e->key), skey) == 0) {
                listDelNode(s->overflow, ln); /* free method decrefs the key */
                break;
            }
        }
        if (listLength(s->overflow) == 0) s->slot = -1;
        return;
    }
    for (int i = 0; i < s->inlined_count; i++) {
        if (sdscmp(objectGetVal(s->inlined[i].key), skey) == 0) {
            decrRefCount(s->inlined[i].key);
            s->inlined[i] = s->inlined[s->inlined_count - 1]; /* swap with last */
            s->inlined_count--;
            break;
        }
    }
    if (s->inlined_count == 0) s->slot = -1;
}

/* Record (or refresh) the "before" size for `key` at `slot`. Called from
 * lookupKeyWrite. Creates a new entry if the key isn't tracked yet. */
void clusterSlotStatsSnapshotKey(client *c, int slot, robj *key, robj *val) {
    if (c == NULL || !isMemoryBytesAccountingEnabled(slot)) return;
    slotMemoryBytesSnapshot *s = &c->slot_memory_bytes;
    uint64_t before = val ? slotStatsObjectSize(objectGetVal(key), val) : 0;

    slotMemoryBytesSnap *e = slotSnapFind(s, key);
    if (e != NULL) { /* refresh existing */
        e->before = before;
        s->slot = slot;
        return;
    }

    s->slot = slot;
    if (s->overflow == NULL && s->inlined_count < SLOT_MEMORY_BYTES_INLINE) {
        e = &s->inlined[s->inlined_count++];
        e->key = key;
        incrRefCount(key);
        e->before = before;
        return;
    }

    /* Spill: on first overflow migrate the inline entries into the list
     * (ownership of their key refs transfers to the heap entries), then append
     * the new key. */
    if (s->overflow == NULL) {
        s->overflow = listCreate();
        listSetFreeMethod(s->overflow, slotSnapFreeEntry);
        for (int i = 0; i < s->inlined_count; i++) {
            slotMemoryBytesSnap *moved = zmalloc(sizeof(*moved));
            *moved = s->inlined[i];
            listAddNodeTail(s->overflow, moved);
        }
        s->inlined_count = 0;
    }
    slotMemoryBytesSnap *ne = zmalloc(sizeof(*ne));
    ne->key = key;
    incrRefCount(key);
    ne->before = before;
    listAddNodeTail(s->overflow, ne);
}

/* Refresh the "before" size for `key` to the new value's size, but only if the
 * key is already snapshotted. Called from db-hooks (dbAdd / dbSetValue) after
 * they account the change themselves, so a later signalModifiedKey computes a
 * zero delta instead of double counting. Never creates an entry, so bulk write
 * paths (e.g. MSET) that never snapshotted don't allocate. */
void clusterSlotStatsRefreshKey(client *c, robj *key, robj *val) {
    if (c == NULL || !server.cluster_enabled) return;
    slotMemoryBytesSnapshot *s = &c->slot_memory_bytes;
    if (s->slot < 0) return;
    slotMemoryBytesSnap *e = slotSnapFind(s, key);
    if (e != NULL) e->before = val ? slotStatsObjectSize(objectGetVal(key), val) : 0;
}

/* Drop any pending snapshot for `key`. Called from the delete path, where the
 * db-hook has already subtracted the key's size explicitly. */
void clusterSlotStatsForgetKey(client *c, robj *key) {
    if (c == NULL || !server.cluster_enabled) return;
    slotMemoryBytesSnapshot *s = &c->slot_memory_bytes;
    if (s->slot < 0) return;
    slotSnapRemove(s, key);
}

/* Apply the in-place delta for `key` and drop its snapshot. Called from
 * signalModifiedKey. */
void clusterSlotStatsCommitKey(client *c, serverDb *db, robj *key) {
    if (c == NULL || !server.cluster_enabled) return;
    slotMemoryBytesSnapshot *s = &c->slot_memory_bytes;
    if (s->slot < 0) return;
    slotMemoryBytesSnap *e = slotSnapFind(s, key);
    if (e == NULL) return;

    int slot = s->slot;
    uint64_t before = e->before;
    /* Re-read the value to get the post-mutation size. Use read-only flags so
     * this lookup does not itself take a new write snapshot. */
    robj *val = lookupKeyReadWithFlags(db, key, LOOKUP_NOTOUCH | LOOKUP_NOSTATS | LOOKUP_NOEXPIRE | LOOKUP_NONOTIFY);
    if (val != NULL) {
        uint64_t after = slotStatsObjectSize(objectGetVal(key), val);
        if (after != before) {
            if (before > 0) saturated_sub(&server.cluster->slot_stats[slot].memory_bytes, before);
            if (after > 0) server.cluster->slot_stats[slot].memory_bytes += after;
        }
    }
    slotSnapRemove(s, key);
}

/* Initialize the snapshot on a freshly allocated client. createClient() uses
 * zmalloc (not zeroed), so every field must be set explicitly. This must not
 * read `overflow` -- unlike clusterSlotStatsClearSnapshot() -- because it is
 * uninitialized at this point. */
void clusterSlotStatsInitSnapshot(client *c) {
    slotMemoryBytesSnapshot *s = &c->slot_memory_bytes;
    s->inlined_count = 0;
    s->slot = -1;
    s->overflow = NULL;
}

/* Clear all pending snapshots. Called at each command boundary (afterCommand)
 * and from resetClient, so a snapshot can never leak across commands (which
 * also makes a per-entry db-id unnecessary: c->db is fixed within a command). */
void clusterSlotStatsClearSnapshot(client *c) {
    slotMemoryBytesSnapshot *s = &c->slot_memory_bytes;
    if (s->overflow != NULL) listEmpty(s->overflow); /* keep header for reuse */
    for (int i = 0; i < s->inlined_count; i++) decrRefCount(s->inlined[i].key);
    s->inlined_count = 0;
    s->slot = -1;
}

/* Release the snapshot, including the overflow list header. Called on client
 * free. */
void clusterSlotStatsFreeSnapshot(client *c) {
    clusterSlotStatsClearSnapshot(c);
    slotMemoryBytesSnapshot *s = &c->slot_memory_bytes;
    if (s->overflow != NULL) {
        listRelease(s->overflow);
        s->overflow = NULL;
    }
}

void clusterSlotStatsResetMemoryBytesAll(void) {
    if (!server.cluster_enabled) return;
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        server.cluster->slot_stats[slot].memory_bytes = 0;
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER SLOT-STATS command
 * -------------------------------------------------------------------------- */

/* Struct used to temporarily hold slot statistics for sorting. */
typedef struct {
    int slot;
    uint64_t stat;
} slotStatForSort;

static int doesSlotBelongToMyShard(int slot) {
    clusterNode *myself = getMyClusterNode();
    clusterNode *primary = clusterNodeGetPrimary(myself);

    return clusterNodeCoversSlot(primary, slot);
}

static int markSlotsAssignedToMyShard(unsigned char *assigned_slots, int start_slot, int end_slot) {
    int assigned_slots_count = 0;
    for (int slot = start_slot; slot <= end_slot; slot++) {
        if (doesSlotBelongToMyShard(slot)) {
            assigned_slots[slot]++;
            assigned_slots_count++;
        }
    }
    return assigned_slots_count;
}

static uint64_t getSlotStat(int slot, slotStatType stat_type) {
    uint64_t slot_stat = 0;
    switch (stat_type) {
    case KEY_COUNT: slot_stat = countKeysInSlot(slot); break;
    case CPU_USEC: slot_stat = server.cluster->slot_stats[slot].cpu_usec; break;
    case NETWORK_BYTES_IN: slot_stat = server.cluster->slot_stats[slot].network_bytes_in; break;
    case NETWORK_BYTES_OUT: slot_stat = server.cluster->slot_stats[slot].network_bytes_out; break;
    case MEMORY_BYTES: slot_stat = server.cluster->slot_stats[slot].memory_bytes; break;
    case SLOT_STAT_COUNT:
    case INVALID: serverPanic("Invalid slot stat type %d was found.", stat_type);
    }
    return slot_stat;
}

/* Compare by stat in ascending order. If stat is the same, compare by slot in ascending order. */
static int slotStatForSortAscCmp(const void *a, const void *b) {
    slotStatForSort entry_a = *((slotStatForSort *)a);
    slotStatForSort entry_b = *((slotStatForSort *)b);
    if (entry_a.stat == entry_b.stat) {
        return entry_a.slot - entry_b.slot;
    }
    return entry_a.stat - entry_b.stat;
}

/* Compare by stat in descending order. If stat is the same, compare by slot in ascending order. */
static int slotStatForSortDescCmp(const void *a, const void *b) {
    slotStatForSort entry_a = *((slotStatForSort *)a);
    slotStatForSort entry_b = *((slotStatForSort *)b);
    if (entry_b.stat == entry_a.stat) {
        return entry_a.slot - entry_b.slot;
    }
    return entry_b.stat - entry_a.stat;
}

static void collectAndSortSlotStats(slotStatForSort slot_stats[], slotStatType order_by, int desc) {
    int i = 0;

    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (doesSlotBelongToMyShard(slot)) {
            slot_stats[i].slot = slot;
            slot_stats[i].stat = getSlotStat(slot, order_by);
            i++;
        }
    }
    qsort(slot_stats, i, sizeof(slotStatForSort), (desc) ? slotStatForSortDescCmp : slotStatForSortAscCmp);
}

static void addReplySlotStat(client *c, int slot) {
    addReplyArrayLen(c, 2); /* Array of size 2, where 0th index represents (int) slot,
                             * and 1st index represents (map) usage statistics. */
    addReplyLongLong(c, slot);
    /* The map always carries key-count and memory-bytes (state metrics), and
     * additionally carries cpu-usec / network-bytes-in / network-bytes-out
     * when the cumulative stats are enabled. */
    int map_len = server.cluster_slot_stats_enabled ? SLOT_STAT_COUNT : 2;
    addReplyMapLen(c, map_len);

    addReplyBulkCString(c, "key-count");
    addReplyLongLong(c, countKeysInSlot(slot));

    /* memory-bytes is the total allocated footprint of all keys in the slot. */
    addReplyBulkCString(c, "memory-bytes");
    addReplyLongLong(c, server.cluster->slot_stats[slot].memory_bytes);

    /* Any additional metrics aside from key-count/memory-bytes come with a
     * performance trade-off, and are aggregated and returned based on its
     * server config. */
    if (server.cluster_slot_stats_enabled) {
        addReplyBulkCString(c, "cpu-usec");
        addReplyLongLong(c, server.cluster->slot_stats[slot].cpu_usec);
        addReplyBulkCString(c, "network-bytes-in");
        addReplyLongLong(c, server.cluster->slot_stats[slot].network_bytes_in);
        addReplyBulkCString(c, "network-bytes-out");
        addReplyLongLong(c, server.cluster->slot_stats[slot].network_bytes_out);
    }
}

/* Adds reply for the SLOTSRANGE variant.
 * Response is ordered in ascending slot number. */
static void addReplySlotsRange(client *c, unsigned char *assigned_slots, int startslot, int endslot, int len) {
    addReplyArrayLen(c, len); /* Top level RESP reply format is defined as an array, due to ordering invariance. */

    for (int slot = startslot; slot <= endslot; slot++) {
        if (assigned_slots[slot]) addReplySlotStat(c, slot);
    }
}

static void addReplySortedSlotStats(client *c, slotStatForSort slot_stats[], long limit) {
    int num_slots_assigned = getMyShardSlotCount();
    int len = min(limit, num_slots_assigned);
    addReplyArrayLen(c, len); /* Top level RESP reply format is defined as an array, due to ordering invariance. */

    for (int i = 0; i < len; i++) {
        addReplySlotStat(c, slot_stats[i].slot);
    }
}

/* Accumulates egress bytes for the slot. */
void clusterSlotStatsAddNetworkBytesOutForSlot(int slot, unsigned long long net_bytes_out) {
    if (!clusterSlotStatsEnabled(slot)) return;

    serverAssert(slot >= 0 && slot < CLUSTER_SLOTS);
    server.cluster->slot_stats[slot].network_bytes_out += net_bytes_out;
}

/* Accumulates egress bytes upon sending RESP responses back to user clients. */
void clusterSlotStatsAddNetworkBytesOutForUserClient(client *c) {
    clusterSlotStatsAddNetworkBytesOutForSlot(c->slot, c->net_output_bytes_curr_cmd);
}

/* Accumulates egress bytes upon sending replication stream. This only applies for primary nodes. */
static void clusterSlotStatsUpdateNetworkBytesOutForReplication(long long len) {
    client *c = server.current_client;
    if (c == NULL || !clusterSlotStatsEnabled(c->slot)) return;

    /* We multiply the bytes len by the number of replicas to account for us broadcasting to multiple replicas at once. */
    len *= (long long)listLength(server.replicas);
    serverAssert(c->slot >= 0 && c->slot < CLUSTER_SLOTS);
    serverAssert(nodeIsPrimary(server.cluster->myself));
    /* We sometimes want to adjust the counter downwards (for example when we want to undo accounting for
     * SELECT commands that don't belong to any slot) so let's make sure we don't underflow the counter. */
    serverAssert(len >= 0 || server.cluster->slot_stats[c->slot].network_bytes_out >= (uint64_t)-len);
    server.cluster->slot_stats[c->slot].network_bytes_out += len;
}

/* Increment network bytes out for replication stream. This method will increment `len` value times the active replica
 * count. */
void clusterSlotStatsIncrNetworkBytesOutForReplication(long long len) {
    clusterSlotStatsUpdateNetworkBytesOutForReplication(len);
}

/* Decrement network bytes out for replication stream.
 * This is used to remove accounting of data which doesn't belong to any particular slots e.g. SELECT command.
 * This will decrement `len` value times the active replica count. */
void clusterSlotStatsDecrNetworkBytesOutForReplication(long long len) {
    clusterSlotStatsUpdateNetworkBytesOutForReplication(-len);
}

/* Upon SPUBLISH, two egress events are triggered.
 * 1) Internal propagation, for clients that are subscribed to the current node.
 * 2) External propagation, for other nodes within the same shard (could either be a primary or replica).
 *    This type is not aggregated, to stay consistent with server.stat_net_output_bytes aggregation.
 * This function covers the internal propagation component. */
void clusterSlotStatsAddNetworkBytesOutForShardedPubSubInternalPropagation(client *c, int slot) {
    if (!clusterSlotStatsEnabled(slot)) return;

    serverAssert(slot >= 0 && slot < CLUSTER_SLOTS);
    server.cluster->slot_stats[slot].network_bytes_out += c->net_output_bytes_curr_cmd;

    /* For sharded pubsub, the client's network bytes metrics must be reset here,
     * as resetClient() is not called until subscription ends. */
    c->net_output_bytes_curr_cmd = 0;
}

/* Adds reply for the ORDERBY variant.
 * Response is ordered based on the sort result. */
static void addReplyOrderBy(client *c, slotStatType order_by, long limit, int desc) {
    slotStatForSort slot_stats[CLUSTER_SLOTS];
    collectAndSortSlotStats(slot_stats, order_by, desc);
    addReplySortedSlotStats(c, slot_stats, limit);
}

/* Resets applicable slot statistics. */
void clusterSlotStatReset(int slot) {
    /* key-count is exempt, as it is queried separately through
     * `countKeysInSlot()`. memory_bytes is intentionally cleared here because
     * this entry point is invoked from clusterAddSlot() / clusterDelSlot()
     * where the keys for the slot are gone (or about to be) and the
     * accounting must follow the data. */
    memset(&server.cluster->slot_stats[slot], 0, sizeof(slotStat));
}

void clusterSlotStatResetAll(void) {
    /* This is invoked from CONFIG RESETSTAT (cumulative reset). memory_bytes
     * is a state metric reflecting current key memory usage, so we preserve
     * it here. FLUSHDB / FLUSHALL paths use clusterSlotStatsResetMemoryBytesAll
     * to clear the state metric in step with the data being removed. */
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        slotStat *s = &server.cluster->slot_stats[slot];
        s->cpu_usec = 0;
        s->network_bytes_in = 0;
        s->network_bytes_out = 0;
    }
}

/* For cpu-usec accumulation, nested commands within EXEC, EVAL, FCALL are skipped.
 * This is due to their unique callstack, where the c->duration for
 * EXEC, EVAL and FCALL already includes all of its nested commands.
 * Meaning, the accumulation of cpu-usec for these nested commands
 * would equate to repeating the same calculation twice.
 */
static int canAddCpuDuration(client *c) {
    return clusterSlotStatsEnabled(c->slot) &&
           (!server.execution_nesting ||         /* Either; */
            (server.execution_nesting &&         /* 1) Command should not be nested, or */
             c->realcmd->flags & CMD_BLOCKING)); /* 2) If command is nested, it must be due to unblocking. */
}

void clusterSlotStatsAddCpuDuration(client *c, ustime_t duration) {
    if (!canAddCpuDuration(c)) return;

    serverAssert(c->slot >= 0 && c->slot < CLUSTER_SLOTS);
    server.cluster->slot_stats[c->slot].cpu_usec += duration;
}

static int canAddNetworkBytesIn(client *c) {
    /* First, cluster mode must be enabled.
     * Second, command should target a specific slot.
     * Third, blocked client is not aggregated, to avoid duplicate aggregation upon unblocking.
     * Fourth, the server is not under a MULTI/EXEC transaction, to avoid duplicate aggregation of
     * EXEC's 14 bytes RESP upon nested call()'s afterCommand(). */
    return clusterSlotStatsEnabled(c->slot) && !(c->flag.blocked) && !server.in_exec;
}

/* Adds network ingress bytes of the current command in execution,
 * calculated earlier within networking.c layer.
 *
 * Note: Below function should only be called once c->slot is parsed.
 * Otherwise, the aggregation will be skipped due to canAddNetworkBytesIn() check failure.
 * */
void clusterSlotStatsAddNetworkBytesInForUserClient(client *c) {
    if (!canAddNetworkBytesIn(c)) return;

    if (c->cmd->proc == execCommand) {
        /* Accumulate its corresponding MULTI RESP; *1\r\n$5\r\nmulti\r\n */
        c->net_input_bytes_curr_cmd += 15;
    }

    server.cluster->slot_stats[c->slot].network_bytes_in += c->net_input_bytes_curr_cmd;
}

void clusterSlotStatsCommand(client *c) {
    if (!server.cluster_enabled) {
        addReplyError(c, "This instance has cluster support disabled");
        return;
    }

    /* Parse additional arguments. */
    if (c->argc == 5 && !strcasecmp(objectGetVal(c->argv[2]), "slotsrange")) {
        /* CLUSTER SLOT-STATS SLOTSRANGE start-slot end-slot */
        int startslot, endslot;
        if ((startslot = getSlotOrReply(c, c->argv[3])) == -1 ||
            (endslot = getSlotOrReply(c, c->argv[4])) == -1) {
            return;
        }
        if (startslot > endslot) {
            addReplyErrorFormat(c, "Start slot number %d is greater than end slot number %d", startslot, endslot);
            return;
        }
        /* Initialize slot assignment array. */
        unsigned char assigned_slots[CLUSTER_SLOTS] = {UNASSIGNED_SLOT};
        int assigned_slots_count = markSlotsAssignedToMyShard(assigned_slots, startslot, endslot);
        addReplySlotsRange(c, assigned_slots, startslot, endslot, assigned_slots_count);

    } else if (c->argc >= 4 && !strcasecmp(objectGetVal(c->argv[2]), "orderby")) {
        /* CLUSTER SLOT-STATS ORDERBY metric [LIMIT limit] [ASC | DESC] */
        int desc = 1;
        slotStatType order_by = INVALID;
        if (!strcasecmp(objectGetVal(c->argv[3]), "key-count")) {
            order_by = KEY_COUNT;
        } else if (!strcasecmp(objectGetVal(c->argv[3]), "cpu-usec") && server.cluster_slot_stats_enabled) {
            order_by = CPU_USEC;
        } else if (!strcasecmp(objectGetVal(c->argv[3]), "network-bytes-in") && server.cluster_slot_stats_enabled) {
            order_by = NETWORK_BYTES_IN;
        } else if (!strcasecmp(objectGetVal(c->argv[3]), "network-bytes-out") && server.cluster_slot_stats_enabled) {
            order_by = NETWORK_BYTES_OUT;
        } else if (!strcasecmp(objectGetVal(c->argv[3]), "memory-bytes")) {
            /* memory-bytes is a state metric, always available when cluster is enabled. */
            order_by = MEMORY_BYTES;
        } else {
            addReplyError(c, "Unrecognized sort metric for ORDERBY.");
            return;
        }
        int i = 4; /* Next argument index, following ORDERBY */
        int limit_counter = 0, asc_desc_counter = 0;
        long limit = CLUSTER_SLOTS;
        while (i < c->argc) {
            int moreargs = c->argc > i + 1;
            if (!strcasecmp(objectGetVal(c->argv[i]), "limit") && moreargs) {
                if (getRangeLongFromObjectOrReply(
                        c, c->argv[i + 1], 1, CLUSTER_SLOTS, &limit,
                        "Limit has to lie in between 1 and 16384 (maximum number of slots).") != C_OK) {
                    return;
                }
                i++;
                limit_counter++;
            } else if (!strcasecmp(objectGetVal(c->argv[i]), "asc")) {
                desc = 0;
                asc_desc_counter++;
            } else if (!strcasecmp(objectGetVal(c->argv[i]), "desc")) {
                desc = 1;
                asc_desc_counter++;
            } else {
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
            if (limit_counter > 1 || asc_desc_counter > 1) {
                addReplyError(c, "Multiple filters of the same type are disallowed.");
                return;
            }
            i++;
        }
        addReplyOrderBy(c, order_by, limit, desc);

    } else {
        addReplySubcommandSyntaxError(c);
    }
}

int clusterSlotStatsEnabled(int slot) {
    return server.cluster_slot_stats_enabled && server.cluster_enabled && slot != -1;
}
