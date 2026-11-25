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
    SLOT_STAT_COUNT,
    INVALID
} slotStatType;

/* -----------------------------------------------------------------------------
 * CLUSTER SLOT-STATS command
 * -------------------------------------------------------------------------- */

/* Struct used to temporarily hold slot statistics for sorting. */
typedef struct {
    int slot;
    uint64_t stat;
} slotStatForSort;

static int markSlotsAssignedToMyShard(unsigned char *assigned_slots, int start_slot, int end_slot) {
    clusterNode *primary = clusterNodeGetPrimary(getMyClusterNode());
    int assigned_slots_count = 0;
    for (int slot = start_slot; slot <= end_slot; slot++) {
        if (clusterNodeCoversSlot(primary, slot)) {
            assigned_slots[slot]++;
            assigned_slots_count++;
        }
    }
    return assigned_slots_count;
}

/* Use the sole database with keys directly or precompute counts when several
 * databases have keys. When neither is set, every slot has zero keys. */
typedef struct {
    serverDb *single_db;
    unsigned int *counts;
    int first_slot;
} slotKeyCounts;

/* Returns true when db holds keys. kvstoreSize() leaves out keys in importing
 * slots, which countKeysInSlotForDb() counts, so they are added here. */
static bool dbHasSlotKeys(serverDb *db) {
    return kvstoreSize(db->keys) + kvstoreImportingSize(db->keys) > 0;
}

/* A contiguous range of slots, start..end inclusive. */
typedef struct {
    int start;
    int end;
} slotStatsRange;

/* Fills key_counts with the key counts of the slots in startslot..endslot that
 * are set in assigned_slots, summed over all databases. Unassigned slots are
 * left at 0. Databases are the outer loop so that each database's array of
 * per-slot hashtables is read sequentially, which is cache and prefetch
 * friendly. Iterating slots in the outer loop would read one entry from every
 * database's array per slot, a cache miss per database once several databases
 * hold keys. The assigned slots are grouped into contiguous ranges first, so
 * the inner loop has no ownership check. Databases without keys, including keys
 * in importing slots, are skipped. */
static void collectKeyCounts(slotKeyCounts *key_counts, const unsigned char *assigned_slots, int startslot, int endslot) {
    key_counts->first_slot = startslot;
    key_counts->counts = zcalloc(sizeof(unsigned int) * (endslot - startslot + 1));

    /* Ranges are separated by at least one unassigned slot, so there are at
     * most half as many ranges as slots, rounded up. */
    slotStatsRange *ranges = zmalloc(sizeof(slotStatsRange) * ((endslot - startslot) / 2 + 1));
    int num_ranges = 0;
    for (int slot = startslot; slot <= endslot; slot++) {
        if (!assigned_slots[slot]) continue;
        if (num_ranges > 0 && ranges[num_ranges - 1].end == slot - 1) {
            ranges[num_ranges - 1].end = slot;
        } else {
            ranges[num_ranges].start = slot;
            ranges[num_ranges].end = slot;
            num_ranges++;
        }
    }

    serverDb *db;
    forEachCreatedDb(db) {
        if (!dbHasSlotKeys(db)) continue;
        for (int r = 0; r < num_ranges; r++) {
            const int range_start = ranges[r].start;
            const int range_end = ranges[r].end;
            unsigned int *counts = key_counts->counts + (range_start - startslot);
            for (int slot = range_start; slot <= range_end; slot++) {
                *counts++ += countKeysInSlotForDb(slot, db);
            }
        }
    }
    zfree(ranges);
}

/* Identify the sole database with keys, if any. With zero or one, counting
 * slots directly avoids building an array or walking empty databases again. */
static bool findSingleSlotKeyDb(slotKeyCounts *key_counts) {
    serverDb *db;
    forEachCreatedDb(db) {
        if (!dbHasSlotKeys(db)) continue;
        if (key_counts->single_db != NULL) return false;
        key_counts->single_db = db;
    }
    return true;
}

static unsigned int getSlotKeyCount(const slotKeyCounts *key_counts, int slot) {
    if (key_counts->counts != NULL) return key_counts->counts[slot - key_counts->first_slot];
    return key_counts->single_db != NULL ? countKeysInSlotForDb(slot, key_counts->single_db) : 0;
}

static uint64_t getSlotStat(int slot, slotStatType stat_type, const slotKeyCounts *key_counts) {
    uint64_t slot_stat = 0;
    switch (stat_type) {
    case KEY_COUNT: slot_stat = getSlotKeyCount(key_counts, slot); break;
    case CPU_USEC: slot_stat = server.cluster->slot_stats[slot].cpu_usec; break;
    case NETWORK_BYTES_IN: slot_stat = server.cluster->slot_stats[slot].network_bytes_in; break;
    case NETWORK_BYTES_OUT: slot_stat = server.cluster->slot_stats[slot].network_bytes_out; break;
    case SLOT_STAT_COUNT:
    case INVALID: serverPanic("Invalid slot stat type %d was found.", stat_type);
    }
    return slot_stat;
}

/* Compare by stat in ascending order. If stat is the same, compare by slot in ascending order.
 * Note: subtraction of uint64_t values cast to int truncates when the difference exceeds
 * INT_MAX, potentially inverting the sort order. Use three-way comparison instead. */
static int slotStatForSortAscCmp(const void *a, const void *b) {
    slotStatForSort entry_a = *((slotStatForSort *)a);
    slotStatForSort entry_b = *((slotStatForSort *)b);
    if (entry_a.stat == entry_b.stat) {
        return entry_a.slot - entry_b.slot;
    }
    return (entry_a.stat > entry_b.stat) - (entry_a.stat < entry_b.stat);
}

/* Compare by stat in descending order. If stat is the same, compare by slot in ascending order. */
static int slotStatForSortDescCmp(const void *a, const void *b) {
    slotStatForSort entry_a = *((slotStatForSort *)a);
    slotStatForSort entry_b = *((slotStatForSort *)b);
    if (entry_b.stat == entry_a.stat) {
        return entry_a.slot - entry_b.slot;
    }
    return (entry_b.stat > entry_a.stat) - (entry_b.stat < entry_a.stat);
}

static void collectAndSortSlotStats(slotStatForSort slot_stats[],
                                    const unsigned char *assigned_slots,
                                    slotStatType order_by,
                                    int desc,
                                    const slotKeyCounts *key_counts) {
    int i = 0;

    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (assigned_slots[slot]) {
            slot_stats[i].slot = slot;
            slot_stats[i].stat = getSlotStat(slot, order_by, key_counts);
            i++;
        }
    }
    qsort(slot_stats, i, sizeof(slotStatForSort), (desc) ? slotStatForSortDescCmp : slotStatForSortAscCmp);
}

static void addReplySlotStat(client *c, int slot, unsigned int key_count) {
    addReplyArrayLen(c, 2); /* Array of size 2, where 0th index represents (int) slot,
                             * and 1st index represents (map) usage statistics. */
    addReplyLongLong(c, slot);
    addReplyMapLen(c, (server.cluster_slot_stats_enabled) ? SLOT_STAT_COUNT
                                                          : 1); /* Nested map representing slot usage statistics. */
    addReplyBulkCString(c, "key-count");
    addReplyLongLong(c, key_count);

    /* Any additional metrics aside from key-count come with a performance trade-off,
     * and are aggregated and returned based on its server config. */
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
    slotKeyCounts key_counts = {0};
    if (len > 0 && !findSingleSlotKeyDb(&key_counts)) {
        collectKeyCounts(&key_counts, assigned_slots, startslot, endslot);
    }
    addReplyArrayLen(c, len); /* Top level RESP reply format is defined as an array, due to ordering invariance. */

    for (int slot = startslot; slot <= endslot; slot++) {
        if (assigned_slots[slot]) addReplySlotStat(c, slot, getSlotKeyCount(&key_counts, slot));
    }
    zfree(key_counts.counts);
}

/* After sorting a limited reply by another metric, reuse stat to hold the
 * key counts of the returned slots. Check which databases hold keys once. */
static void countKeysForSortedReply(slotStatForSort slot_stats[], int len) {
    for (int i = 0; i < len; i++) slot_stats[i].stat = 0;
    serverDb *db;
    forEachCreatedDb(db) {
        if (!dbHasSlotKeys(db)) continue;
        for (int i = 0; i < len; i++) {
            slot_stats[i].stat += countKeysInSlotForDb(slot_stats[i].slot, db);
        }
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
    unsigned char assigned_slots[CLUSTER_SLOTS] = {UNASSIGNED_SLOT};
    int assigned_count = markSlotsAssignedToMyShard(assigned_slots, 0, CLUSTER_SLOTS - 1);
    int len = min(limit, assigned_count);
    if (len == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    /* When most slots will be returned, a sequential scan of owned slots is
     * more cache friendly than looking them up in rank order. Bound the extra
     * lookups to the final quarter of the assigned slots. */
    slotKeyCounts key_counts = {0};
    bool count_reply_only = false;
    if (!findSingleSlotKeyDb(&key_counts)) {
        if (order_by == KEY_COUNT || len >= assigned_count - assigned_count / 4) {
            collectKeyCounts(&key_counts, assigned_slots, 0, CLUSTER_SLOTS - 1);
        } else {
            count_reply_only = true;
        }
    }
    collectAndSortSlotStats(slot_stats, assigned_slots, order_by, desc, &key_counts);
    if (count_reply_only) countKeysForSortedReply(slot_stats, len);

    addReplyArrayLen(c, len); /* Top level RESP reply format is defined as an array, due to ordering invariance. */
    for (int i = 0; i < len; i++) {
        int slot = slot_stats[i].slot;
        unsigned int key_count = (order_by == KEY_COUNT || count_reply_only) ? slot_stats[i].stat
                                                                             : getSlotKeyCount(&key_counts, slot);
        addReplySlotStat(c, slot, key_count);
    }
    zfree(key_counts.counts);
}

/* Resets applicable slot statistics. */
void clusterSlotStatReset(int slot) {
    /* key-count is exempt, as it is computed from the keyspace when queried. */
    memset(&server.cluster->slot_stats[slot], 0, sizeof(slotStat));
}

void clusterSlotStatResetAll(void) {
    memset(server.cluster->slot_stats, 0, sizeof(server.cluster->slot_stats));
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

int testOnlySlotStatForSortAscCmp(const void *a, const void *b) {
    return slotStatForSortAscCmp(a, b);
}

int testOnlySlotStatForSortDescCmp(const void *a, const void *b) {
    return slotStatForSortDescCmp(a, b);
}
