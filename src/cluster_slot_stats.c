/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include "server.h"
#include "cluster.h"

#define UNASSIGNED_SLOT 0

typedef enum {
    KEY_COUNT,
    INVALID,
} slotStatTypes;

/* -----------------------------------------------------------------------------
 * CLUSTER SLOT-STATS command
 * -------------------------------------------------------------------------- */

typedef struct {
    int slot;
    uint64_t stat;
} slotStatEntry;

static int doesSlotBelongToMyShard(int slot) {
    clusterNode *myself = getMyClusterNode();
    clusterNode *master = clusterNodeGetMaster(myself);

    return clusterNodeCoversSlot(master, slot);
}

static void markSlotsAssignedToMyShard(unsigned char *assigned_slots, int start_slot, int end_slot, int *len) {
    for (int slot = start_slot; slot <= end_slot; slot++) {
        if (doesSlotBelongToMyShard(slot)) {
            assigned_slots[slot]++;
            (*len)++;
        }
    }
}

static uint64_t getSlotStat(int slot, int stat_type) {
    serverAssert(stat_type != INVALID);
    uint64_t slot_stat = 0;
    if (stat_type == KEY_COUNT) {
        slot_stat = countKeysInSlot(slot);
    }
    return slot_stat;
}

static int slotStatEntryAscCmp(const void *a, const void *b) {
    slotStatEntry entry_a = *((slotStatEntry *) a);
    slotStatEntry entry_b = *((slotStatEntry *) b);
    return entry_a.stat - entry_b.stat;
}

static int slotStatEntryDescCmp(const void *a, const void *b) {
    slotStatEntry entry_a = *((slotStatEntry *) a);
    slotStatEntry entry_b = *((slotStatEntry *) b);
    return entry_b.stat - entry_a.stat;
}

static void collectAndSortSlotStats(slotStatEntry slot_stats[], int order_by, int desc) {
    int i = 0;
    
    for (int slot = 0; slot < CLUSTER_SLOTS; slot++) {
        if (doesSlotBelongToMyShard(slot)) {
            slot_stats[i].slot = slot;
            slot_stats[i].stat = getSlotStat(slot, order_by);
            i++;
        }
    }
    qsort(slot_stats, i, sizeof(slotStatEntry), (desc) ? slotStatEntryDescCmp : slotStatEntryAscCmp);
}

static void addReplySlotStat(client *c, int slot) {
    addReplyLongLong(c, slot);
    addReplyMapLen(c, 1);
    addReplyBulkCString(c, "key-count");
    addReplyLongLong(c, countKeysInSlot(slot));
}

static void addReplySlotStats(client *c, unsigned char *assigned_slots, int startslot, int endslot, int len) {
    addReplyMapLen(c, len);

    for (int slot = startslot; slot <= endslot; slot++) {
        if (assigned_slots[slot]) addReplySlotStat(c, slot);
    }
}

static void addReplySortedSlotStats(client *c, slotStatEntry slot_stats[], long limit) {
    int num_slots_assigned = getMyShardSlotCount();
    int len = min(limit, num_slots_assigned);
    addReplyMapLen(c, len);

    for (int i = 0; i < len; i++) {
        addReplySlotStat(c, slot_stats[i].slot);
    }
}

static void sortAndAddReplySlotStats(client *c, int order_by, long limit, int desc) {
    slotStatEntry slot_stats[CLUSTER_SLOTS];
    collectAndSortSlotStats(slot_stats, order_by, desc);
    addReplySortedSlotStats(c, slot_stats, limit);
}

void clusterSlotStatsCommand(client *c) {
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }

    /* Parse additional arguments. */
    if (c->argc == 5 && !strcasecmp(c->argv[2]->ptr,"slotsrange")) {
        /* CLUSTER SLOT-STATS SLOTSRANGE start-slot end-slot */
        int startslot, endslot;
        if ((startslot = getSlotOrReply(c,c->argv[3])) == C_ERR ||
            (endslot = getSlotOrReply(c,c->argv[4])) == C_ERR) {
            return;
        }
        if (startslot > endslot) {
            addReplyErrorFormat(c,"Start slot number %d is greater than end slot number %d", startslot, endslot);
            return;
        }
        /* Initialize slot assignment array. */
        unsigned char assigned_slots[CLUSTER_SLOTS]= {UNASSIGNED_SLOT};
        int len = 0;
        markSlotsAssignedToMyShard(assigned_slots, startslot, endslot, &len);
        addReplySlotStats(c, assigned_slots, startslot, endslot, len);

    } else if (c->argc >= 4 && !strcasecmp(c->argv[2]->ptr,"orderby")) {
        /* CLUSTER SLOT-STATS ORDERBY metric [LIMIT limit] [ASC | DESC] */
        int desc = 1, order_by = INVALID;
        if (!strcasecmp(c->argv[3]->ptr, "key-count")) {
            order_by = KEY_COUNT;
        } else {
            addReplyError(c, "Unrecognized sort metric for ORDER BY. The supported metrics are: key-count.");
            return;
        }
        int i = 4; /* Next argument index, following ORDERBY */
        int limit_counter = 0, asc_desc_counter = 0;
        long limit;
        while(i < c->argc) {
            int moreargs = c->argc > i+1;
            if (!strcasecmp(c->argv[i]->ptr,"limit") && moreargs) {
                if (getRangeLongFromObjectOrReply(
                    c, c->argv[i+1], 1, CLUSTER_SLOTS, &limit,
                    "Limit has to lie in between 1 and 16384 (maximum number of slots).") != C_OK)
                    return;
                i++;
                limit_counter++;
            } else if (!strcasecmp(c->argv[i]->ptr,"asc")) {
                desc = 0;
                asc_desc_counter++;
            } else if (!strcasecmp(c->argv[i]->ptr,"desc")) {
                desc = 1;
                asc_desc_counter++;
            } else {
                addReplyErrorObject(c,shared.syntaxerr);
                return;
            }
            if (limit_counter > 1 || asc_desc_counter > 1) {
                addReplyError(c, "Multiple filters of the same type are disallowed.");
                return;
            }
            i++;
        }
        sortAndAddReplySlotStats(c, order_by, limit, desc);

    } else {
        addReplySubcommandSyntaxError(c);
    }
}
