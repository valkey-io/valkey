/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include "cluster_migrateslots.h"
#include "bio.h"
#include "module.h"
#include "functions.h"

typedef enum slotMigrationJobState {
    /* Importing states */
    SLOT_IMPORT_WAIT_ACK,
    SLOT_IMPORT_RECEIVE_SNAPSHOT,
    SLOT_IMPORT_WAITING_FOR_PAUSED,
    SLOT_IMPORT_FAILOVER_REQUESTED,
    SLOT_IMPORT_FAILOVER_GRANTED,
    SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP,

    /* Exporting states */
    SLOT_EXPORT_CONNECTING,
    SLOT_EXPORT_SEND_AUTH,
    SLOT_EXPORT_READ_AUTH_RESPONSE,
    SLOT_EXPORT_SEND_ESTABLISH,
    SLOT_EXPORT_READ_ESTABLISH_RESPONSE,
    SLOT_EXPORT_WAITING_TO_SNAPSHOT,
    SLOT_EXPORT_SNAPSHOTTING,
    SLOT_EXPORT_STREAMING,
    SLOT_EXPORT_WAITING_TO_PAUSE,
    SLOT_EXPORT_FAILOVER_PAUSED,
    SLOT_EXPORT_FAILOVER_GRANTED,

    /* Terminal states */
    SLOT_MIGRATION_JOB_FAILED,
    SLOT_MIGRATION_JOB_CANCELLED,
    SLOT_MIGRATION_JOB_SUCCESS,
} slotMigrationJobState;

typedef enum slotMigrationJobType {
    SLOT_MIGRATION_EXPORT,
    SLOT_MIGRATION_IMPORT,
} slotMigrationJobType;

/* A slotMigrationJob represents an export or import of a slot to/from another
 * node for an ongoing slot migration. A job is created on either end of a
 * migration during the duration of a CLUSTER MIGRATESLOTS operation. */
typedef struct slotMigrationJob {
    slotMigrationJobType type;                /* Type of the migration job
                                               * (either for import or
                                               * export) */
    time_t ctime;                             /* Migration job creation time. */
    time_t last_update;                       /* Migration job last update
                                               * time. */
    time_t last_ack;                          /* Migration job last ack time. */
    char nodename[CLUSTER_NAMELEN];           /* Name of the slot import source
                                               * node, hex string, sha1-size. */
    char name[CLUSTER_NAMELEN];               /* Unique name for the job, hex
                                               * string, sha1-size. */
    client *client;                           /* Client to other node. */
    slotMigrationJobState state;              /* State of the slot migration
                                               * job. */
    sds status_msg;                           /* Human readable status message
                                               * with more details. */
    list *slot_ranges;                        /* List of the slot ranges we want
                                               * to import. */
    sds slot_ranges_str;                      /* Precomputed string of the slot
                                               * ranges, for logging and
                                               * info. */
    mstime_t mf_end;                          /* End time for the manual
                                               * failover, after this we will
                                               * unpause. */
    slotMigrationJobState post_cleanup_state; /* Target state, after pending
                                               * cleanup is done. */
    sds description;                          /* Description, used for
                                               * logging. */

    /* State needed during client establishment */
    connection *conn; /* Connection to slot import source node. */
    sds response_buf;
} slotMigrationJob;

static bool isSlotMigrationJobFinished(slotMigrationJob *job);
static bool isSlotMigrationJobInProgress(slotMigrationJob *job);
static slotMigrationJob *createSlotImportJob(client *c,
                                             clusterNode *source_node,
                                             char *name,
                                             list *slot_ranges);
static int connectSlotExportJob(slotMigrationJob *job);
static const char *slotMigrationJobStateToString(slotMigrationJobState state);
static void updateSlotMigrationJobState(slotMigrationJob *job,
                                        slotMigrationJobState state);
static void sendSyncSlotsMessage(slotMigrationJob *job, const char *subcommand);
static void proceedWithSlotMigration(slotMigrationJob *job);
static slotMigrationJob *createSlotExportJob(clusterNode *target_node,
                                             list *slot_ranges);
static bool isSlotExportPauseTimedOut(slotMigrationJob *job);
static void resetSlotMigrationJob(slotMigrationJob *job);
static void finishSlotMigrationJob(slotMigrationJob *job,
                                   slotMigrationJobState state,
                                   char *message);
static void freeSlotMigrationJob(void *o);
static sds generateSlotMigrationJobDescription(slotMigrationJob *job,
                                               clusterNode *node);
static void slotExportTryUnpause(void);

/* Create an empty list of slot ranges. */
list *createSlotRangeList(void) {
    list *slot_ranges = listCreate();
    listSetFreeMethod(slot_ranges, zfree);
    return slot_ranges;
}

/* Represent the provided slot range list as a string. */
sds representSlotRangeList(list *slot_ranges) {
    sds res = sdsempty();
    listNode *ln;
    listIter li;
    int first = 1;
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        if (first) {
            res = sdscatfmt(res, "%u-%u", range->start_slot, range->end_slot);
            first = 0;
        } else {
            res = sdscatfmt(res, " %u-%u", range->start_slot, range->end_slot);
        }
    }
    return res;
}

/* Return the node that owns the provided slot ranges, or NULL if there is no
 * such node. */
clusterNode *getClusterNodeBySlotRanges(list *slot_ranges, int *cross_node) {
    clusterNode *n = NULL;
    listNode *ln;
    listIter li;
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        for (int i = range->start_slot; i <= range->end_slot; i++) {
            if (server.cluster->slots[i] == NULL) {
                return NULL;
            }
            if (!n) {
                n = server.cluster->slots[i];
            }
            if (n != server.cluster->slots[i]) {
                if (cross_node) *cross_node = 1;
                return NULL;
            }
        }
    }
    return n;
}

/* Return whether or not a given slot is in the list of slot ranges. */
bool isSlotInSlotRanges(int slot, list *slot_ranges) {
    /* Loop to check if the slot in any slot range. */
    listNode *ln;
    listIter li;
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        if (slot >= range->start_slot && slot <= range->end_slot) {
            return true;
        }
    }
    return false;
}

/* Return whether or not the two slot ranges overlap or are distinct. */
bool doSlotRangesOverlap(slotRange *range1, slotRange *range2) {
    return range1->end_slot >= range2->start_slot &&
           range1->start_slot <= range2->end_slot;
}

/* Return whether or not the two lists of slot ranges overlap or are
 * distinct. */
bool doSlotRangeListsOverlap(list *ranges1, list *ranges2) {
    /* Since they aren't guaranteed to be sorted, just use a nested loop. */
    listIter li1, li2;
    listNode *ln1, *ln2;
    listRewind(ranges1, &li1);
    while ((ln1 = listNext(&li1)) != NULL) {
        slotRange *range1 = ln1->value;
        listRewind(ranges2, &li2);
        while ((ln2 = listNext(&li2)) != NULL) {
            slotRange *range2 = ln2->value;
            if (doSlotRangesOverlap(range1, range2)) {
                return true;
            }
        }
    }
    return false;
}

/* Remove all the keys in the hash slots that are in the given slot range list
 * and not owned by myself now. */
void delKeysNotOwnedByMyself(list *slot_ranges) {
    listNode *ln;
    listIter li;
    listRewind(slot_ranges, &li);

    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        for (int i = range->start_slot; i <= range->end_slot; i++) {
            if (server.cluster->slots[i] != server.cluster->myself) {
                delKeysInSlot(i, 1, true, false);
            }
        }
    }
}

void setSlotImportingStateInDb(serverDb *db,
                               list *slot_ranges,
                               int is_importing) {
    if (db == NULL) return;
    listNode *ln;
    listIter li;
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        for (int slot = range->start_slot; slot <= range->end_slot; slot++) {
            kvstoreSetIsImporting(db->keys, slot, is_importing);
            kvstoreSetIsImporting(db->expires, slot, is_importing);
            kvstoreSetIsImporting(db->keys_with_volatile_items, slot,
                                  is_importing);
        }
    }
}

void setSlotImportingStateInAllDbs(list *slot_ranges, int is_importing) {
    for (int i = 0; i < server.dbnum; i++) {
        serverDb *db = server.db[i];
        setSlotImportingStateInDb(db, slot_ranges, is_importing);
    }
}

/* Parse as many slot ranges starting from start_index, returning a list of
 * parsed slot ranges, or NULL if there is an error (in which case err_out will
 * be set). If parsing completes successfully, end_index_out will be set to the
 * index of the next argument needed to be parsed in c->argv.
 *
 * Note that all slots in the slot range should belong to a sole node in the
 * cluster topology. node_out will be set to the node that owns all the
 * slots. */
list *parseSlotRangesOrReply(client *c,
                             int start_index,
                             int *end_index_out,
                             clusterNode **node_out) {
    list *slot_ranges = createSlotRangeList();
    *node_out = NULL;
    *end_index_out = c->argc;
    for (int i = start_index; i < c->argc; i += 2) {
        if (getLongLongFromObject(c->argv[i], NULL) != C_OK) {
            /* If we encounter a non-integer parameter, we assume that this is
             * the next argument. */
            *end_index_out = i;
            break;
        }
        int startslot, endslot;
        /* Get the current slot range. */
        if ((startslot = getSlotOrReply(c, c->argv[i])) == -1) {
            goto cleanup;
        }
        if (i + 1 >= c->argc) {
            addReplyError(c, "No end slot for final slot range");
            goto cleanup;
        }
        if ((endslot = getSlotOrReply(c, c->argv[i + 1])) == -1) {
            goto cleanup;
        }
        if (startslot > endslot) {
            addReplyErrorFormat(c,
                                "Start slot number %d is greater than end slot "
                                "number %d.",
                                startslot, endslot);
            goto cleanup;
        }
        /* Check if the current slot range is ready to do the migration. */
        for (int j = startslot; j <= endslot; j++) {
            if (server.cluster->slots[j] == NULL) {
                addReplyErrorFormat(c, "Slot %d has no node served.", j);
                goto cleanup;
            }
            if (!*node_out) {
                *node_out = server.cluster->slots[j];
            } else if (*node_out != server.cluster->slots[j]) {
                addReplyError(c, "Requested slots span multiple shards");
                goto cleanup;
            }
        }

        slotRange *new_range = zmalloc(sizeof(slotRange));
        new_range->start_slot = startslot;
        new_range->end_slot = endslot;

        /* Check for overlap of slot ranges */
        listNode *ln;
        listIter li;
        listRewind(slot_ranges, &li);
        while ((ln = listNext(&li)) != NULL) {
            slotRange *prev_range = ln->value;
            if (doSlotRangesOverlap(new_range, prev_range)) {
                addReplyErrorFormat(c,
                                    "Slot range %d-%d overlaps with previous "
                                    "range %d-%d.",
                                    startslot,
                                    endslot,
                                    prev_range->start_slot,
                                    prev_range->end_slot);
                zfree(new_range);
                goto cleanup;
            }
        }

        /* Add the current slot range to the range list. */
        listAddNodeTail(slot_ranges, new_range);
    }
    if (slot_ranges->len == 0) {
        addReplyError(c, "No slot ranges specified");
        goto cleanup;
    }
    return slot_ranges;

cleanup:
    listRelease(slot_ranges);
    return NULL;
}

/* -------------------------------- TARGET -------------------------------------
 *
 * During a slot migration, the target is informed of a migration by the source
 * through the CLUSTER SYNCSLOTS ESTABLISH command. From here, the target tracks
 * the migration via its own slotMigrationJob.
 *
 * If we encounter an error, the import will be marked as failed and the
 * connection to the source will be closed. From here, the operator will need to
 * restart the migration from the source side.
 *
 * State Machine:
 *
 *              ┌────────────────────┐
 *              │SLOT_IMPORT_WAIT_ACK┼──────┐
 *              └──────────┬─────────┘      │
 *                      ACK│                │
 *          ┌──────────────▼─────────────┐  │
 *          │SLOT_IMPORT_RECEIVE_SNAPSHOT┼──┤
 *          └──────────────┬─────────────┘  │
 *             SNAPSHOT-EOF│                │
 *         ┌───────────────▼──────────────┐ │
 *         │SLOT_IMPORT_WAITING_FOR_PAUSED┼─┤
 *         └───────────────┬──────────────┘ │
 *                   PAUSED│                │
 *         ┌───────────────▼──────────────┐ │ Error Conditions:
 *         │SLOT_IMPORT_FAILOVER_REQUESTED┼─┤  1. OOM
 *         └───────────────┬──────────────┘ │  2. Slot Ownership Change
 *         FAILOVER-GRANTED│                │  3. Demotion to replica
 *          ┌──────────────▼─────────────┐  │  4. FLUSHDB
 *          │SLOT_IMPORT_FAILOVER_GRANTED┼──┤  5. Connection Lost
 *          └──────────────┬─────────────┘  │  6. No ACK from source (timeout)
 *       Takeover Performed│                │
 *          ┌──────────────▼───────────┐    │
 *          │SLOT_MIGRATION_JOB_SUCCESS┼──-─┤
 *          └──────────────────────────┘    │
 *                                          │
 *    ┌─────────────────────────────────────▼─┐
 *    │SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP│
 *    └────────────────────┬──────────────────┘
 * Unowned Slots Cleaned Up│
 *           ┌─────────────▼───────────┐
 *           │SLOT_MIGRATION_JOB_FAILED│
 *           └─────────────────────────┘
 *
 */

bool clusterIsSlotImporting(int slot) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (job->type != SLOT_MIGRATION_IMPORT) continue;
        if (isSlotMigrationJobFinished(job)) continue;
        if (isSlotInSlotRanges(slot, job->slot_ranges)) return true;
    }
    return false;
}

/* Sent by the source to the target to initiate the AOF formatted snapshot.
 * Note that if there is an error in the request, we send a fail message in
 * order to prevent infinite retry in the case of incompatibility.
 *
 * CLUSTER SYNCSLOTS ESTABLISH is the only CLUSTER SYNCSLOTS subcommand that
 * will return a reply. Errors are written from the perspective of the end user
 * to help with debugging migrations. */
void clusterCommandSyncSlotsEstablish(client *c) {
    char *name = NULL;
    clusterNode *source_node = NULL;
    clusterNode *owning_node = NULL;
    list *slot_ranges = NULL;

    if (!nodeIsPrimary(server.cluster->myself)) {
        addReplyError(c, "Target node is not a primary");
        return;
    }

    if (isAnySlotInManualImportingState() ||
        isAnySlotInManualMigratingState()) {
        addReplyError(c, "A slot on the target node is being manually imported "
                         "or migrated");
        return;
    }

    if (c->slot_migration_job) {
        addReplyError(c, "Slot migration client is already a slot migration "
                         "job");
        return;
    }

    /* Order agnostic. We skip unknown key/value pairs forwards
     * compatibility. */
    int i = 3;
    while (i < c->argc) {
        if (!strcasecmp(c->argv[i]->ptr, "source")) {
            if (source_node || i + 1 >= c->argc ||
                sdslen(c->argv[i + 1]->ptr) != CLUSTER_NAMELEN) {
                addReplyErrorObject(c, shared.syntaxerr);
                goto cleanup;
            }
            source_node = clusterLookupNode(c->argv[4]->ptr, CLUSTER_NAMELEN);
            if (!source_node) {
                addReplyError(c, "Target node does not know the source node");
                goto cleanup;
            }
            if (source_node == server.cluster->myself) {
                addReplyError(c, "Source node is target node itself");
                goto cleanup;
            }
            i += 2;
            continue;
        }
        if (!strcasecmp(c->argv[i]->ptr, "name")) {
            if (name || i + 1 >= c->argc ||
                sdslen(c->argv[i + 1]->ptr) != CLUSTER_NAMELEN) {
                addReplyErrorObject(c, shared.syntaxerr);
                goto cleanup;
            }
            name = c->argv[i + 1]->ptr;
            i += 2;
            continue;
        }
        if (!strcasecmp(c->argv[i]->ptr, "slotsrange")) {
            if (slot_ranges) {
                addReplyErrorObject(c, shared.syntaxerr);
                goto cleanup;
            }
            /* parseSlotRanges will set i for the next iteration */
            slot_ranges = parseSlotRangesOrReply(c, i + 1, &i, &owning_node);
            if (slot_ranges == NULL) {
                goto cleanup;
            }

            listIter li;
            listNode *ln;
            listRewind(slot_ranges, &li);
            while ((ln = listNext(&li)) != NULL) {
                slotRange *range = ln->value;
                for (int i = range->start_slot; i <= range->end_slot; i++) {
                    if (clusterIsSlotImporting(i)) {
                        addReplyError(c, "Slot is already being imported on "
                                         "the target by a different migration");
                        goto cleanup;
                    }
                }
            }

            continue;
        }
        addReplyErrorObject(c, shared.syntaxerr);
        goto cleanup;
    }
    if (!source_node || !name || !slot_ranges) {
        addReplyErrorObject(c, shared.syntaxerr);
        goto cleanup;
    }
    if (source_node != owning_node) {
        addReplyError(c, "Target node does not agree about current slot "
                         "ownership");
        goto cleanup;
    }

    slotMigrationJob *job = createSlotImportJob(c, source_node, name,
                                                slot_ranges);
    listAddNodeHead(server.cluster->slot_migration_jobs, job);

    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);

    addReply(c, shared.ok);
    job->client->flag.reply_off = 1;
    return;

cleanup:
    if (slot_ranges) {
        listRelease(slot_ranges);
    }
    return;
}

/* Sent by the source to the target after dumping the snapshot in AOF format. */
void clusterCommandSyncSlotsSnapshotEof(client *c) {
    if (!c->slot_migration_job) {
        addReplyError(c, "CLUSTER SYNCSLOTS SNAPSHOT-EOF should only be used "
                         "by slot migration clients");
        return;
    }
    serverAssert(isSlotMigrationJobInProgress(c->slot_migration_job));
    if (c->slot_migration_job->state != SLOT_IMPORT_RECEIVE_SNAPSHOT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS SNAPSHOT-EOF from slot migration "
                  "%s, but not currently loading an AOF snapshot. Failing "
                  "migration.",
                  c->slot_migration_job->description);
        finishSlotMigrationJob(c->slot_migration_job, SLOT_MIGRATION_JOB_FAILED,
                               "Unexpected state machine transition");
        return;
    }
    serverLog(LL_NOTICE,
              "Slot migration %s successfully received slot snapshot. "
              "Beginning incremental stream...",
              c->slot_migration_job->description);
    sendSyncSlotsMessage(c->slot_migration_job, "REQUEST-PAUSE");
    updateSlotMigrationJobState(c->slot_migration_job,
                                SLOT_IMPORT_WAITING_FOR_PAUSED);
}

/* Sent by the source to the target as a marker of when the pause
 * began (therefore, target is caught up once read). */
void clusterCommandSyncSlotsPaused(client *c) {
    if (!c->slot_migration_job) {
        addReplyError(c, "CLUSTER SYNCSLOTS PAUSED should only be used by slot "
                         "migration clients");
        return;
    }
    serverAssert(isSlotMigrationJobInProgress(c->slot_migration_job));
    if (c->slot_migration_job->state != SLOT_IMPORT_WAITING_FOR_PAUSED) {
        serverLog(LL_WARNING,
                  "Received unexpected CLUSTER SYNCSLOTS PAUSED from slot "
                  "migration %s, . Failing migration.",
                  c->slot_migration_job->description);
        finishSlotMigrationJob(c->slot_migration_job, SLOT_MIGRATION_JOB_FAILED,
                               "Unexpected state machine transition");
        return;
    }
    sendSyncSlotsMessage(c->slot_migration_job, "REQUEST-FAILOVER");
    updateSlotMigrationJobState(c->slot_migration_job,
                                SLOT_IMPORT_FAILOVER_REQUESTED);
}

/* Sent by the source to the target to grant final authorization for
 * failover. */
void clusterCommandSyncSlotsFailoverGranted(client *c) {
    if (!c->slot_migration_job) {
        addReplyError(c, "CLUSTER SYNCSLOTS FAILOVER-GRANTED should only be "
                         "used by slot migration clients");
        return;
    }
    serverAssert(isSlotMigrationJobInProgress(c->slot_migration_job));
    if (c->slot_migration_job->state != SLOT_IMPORT_FAILOVER_REQUESTED) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS FAILOVER-GRANTED from slot "
                  "migration %s, but we never sent a failover request. Failing "
                  "migration.",
                  c->slot_migration_job->description);
        finishSlotMigrationJob(c->slot_migration_job, SLOT_MIGRATION_JOB_FAILED,
                               "Unexpected state machine transition");
        return;
    }
    updateSlotMigrationJobState(c->slot_migration_job,
                                SLOT_IMPORT_FAILOVER_GRANTED);
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
}

slotMigrationJob *createSlotImportJob(client *c,
                                      clusterNode *source_node,
                                      char *name,
                                      list *slot_ranges) {
    slotMigrationJob *job = zcalloc(sizeof(slotMigrationJob));
    memcpy(job->name, name, CLUSTER_NAMELEN);
    memcpy(job->nodename, source_node->name, CLUSTER_NAMELEN);
    job->ctime = server.unixtime;
    job->last_update = job->ctime;
    job->last_ack = job->ctime;
    job->type = SLOT_MIGRATION_IMPORT;
    job->slot_ranges = slot_ranges;
    job->slot_ranges_str = representSlotRangeList(slot_ranges);
    job->state = SLOT_IMPORT_WAIT_ACK;
    job->client = c;
    job->client->slot_migration_job = job;
    job->description = generateSlotMigrationJobDescription(job, source_node);

    /* We treat slot imports like primaries. Primaries are expected to have a
     * dedicated query buffer and allocated replication data. */
    initClientReplicationData(job->client);
    job->client->querybuf = sdsempty();

    /* Mark all the slots as importing in the kvstore */
    setSlotImportingStateInAllDbs(slot_ranges, 1);

    serverLog(LL_NOTICE, "New slot import job created: %s.", job->description);
    return job;
}

/* This function implements the final part of manual slot failovers,
 * where the target takes over all the slot migration job's hash slots, and
 * propagates the new configuration. */
void performSlotImportJobFailover(slotMigrationJob *job) {
    serverAssert(job->type == SLOT_MIGRATION_IMPORT);
    /* 1) Force bump the epoch to facilitate propagation. */
    clusterBumpConfigEpochWithoutConsensus();

    /* 2) Claim all the slots in the slot migration job to myself. */
    listNode *ln;
    listIter li;
    listRewind(job->slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        for (int i = range->start_slot; i <= range->end_slot; i++) {
            clusterDelSlot(i);
            clusterAddSlot(server.cluster->myself, i);
        }
    }

    /* 3) Update state and save config. */
    clearCachedClusterSlotsResponse();
    clusterUpdateState();
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_FSYNC_CONFIG);

    /* 4) Pong all the other nodes so that they can update the state accordingly
     *    and detect that we have taken over the slots. */
    clusterDoBeforeSleep(CLUSTER_TODO_BROADCAST_ALL);

    /* 5) Mark all slots as stable in the kvstore (for SCAN/KEYS/RANDOMKEY) */
    setSlotImportingStateInAllDbs(job->slot_ranges, 0);
}

bool clusterIsAnySlotImporting(void) {
    if (!server.cluster_enabled || !server.cluster) return false;
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (job->type != SLOT_MIGRATION_IMPORT) continue;
        if (!isSlotMigrationJobFinished(job)) return true;
    }
    return false;
}

void clusterMarkImportingSlotsInDb(serverDb *db) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (job->type != SLOT_MIGRATION_IMPORT) continue;
        if (isSlotMigrationJobFinished(job)) continue;
        setSlotImportingStateInDb(db, job->slot_ranges, 1);
    }
}

/* Called within topology updates to update any slot imports immediately
 * when the ownership changes. Will fail import if any of our imports are no
 * longer valid. */
void clusterUpdateSlotImportsOnOwnershipChange(void) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li))) {
        slotMigrationJob *job = (slotMigrationJob *)ln->value;
        if (job->type != SLOT_MIGRATION_IMPORT) continue;
        if (!isSlotMigrationJobInProgress(job)) continue;
        if (!nodeIsPrimary(server.cluster->myself)) {
            finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                   "I was demoted to a replica");
            continue;
        }
        clusterNode *n = getClusterNodeBySlotRanges(job->slot_ranges, NULL);
        if (n && !memcmp(n->name, job->nodename, CLUSTER_NAMELEN)) continue;
        if (n == server.cluster->myself) {
            finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                   "Slots were unexpectedly assigned to myself "
                                   "during import");
        }
        finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                               "Slots are no longer owned by source node");
    }
}

/* Callback triggered whenever flush happens and there is an active slot
 * migration. We handle this by failing the ongoing migration. */
void clusterHandleFlushDuringSlotMigration(void) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li))) {
        slotMigrationJob *job = (slotMigrationJob *)ln->value;
        if (!isSlotMigrationJobInProgress(job)) continue;
        /* Note that if we are exporting, we don't send a FAIL message, so the
         * target should reconnect and complete the migration shortly after,
         * since we now have no data.
         *
         * If we are importing, we fail the migration and expect the operator to
         * retry. */
        finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                               "Data was flushed");
    }
}

/* ---------------------------------- SOURCE -----------------------------------
 *
 * During a slot import, the source node tracks ongoing export operations as
 * slotMigrationJobs. A slotMigrationJob is initially created when the operator
 * issues CLUSTER MIGRATESLOTS. After this, we ensure that all data in the
 * requested slots are sent to the target node.
 *
 * If at any time we detect an error, the connection will be closed, causing the
 * slot migration to fail.
 *
 * State Machine:
 *
 *                ┌──────────────────────┐
 *                │SLOT_EXPORT_CONNECTING├─────────┐
 *                └───────────┬──────────┘         │
 *                   Connected│                    │
 *                ┌───────────▼─────────┐          │
 *                │SLOT_EXPORT_SEND_AUTH┼──────────┤
 *                └───────────┬─────────┘          │
 *        AUTH command written│                    │
 *              ┌─────────────▼────────────────┐   │
 *              │SLOT_EXPORT_READ_AUTH_RESPONSE┼───┤
 *              └─────────────┬────────────────┘   │
 *               Authenticated│                    │
 *              ┌─────────────▼────────────┐       │
 *              │SLOT_EXPORT_SEND_ESTABLISH┼───────┤
 *              └─────────────┬────────────┘       │
 *   ESTABLISH command written│                    │
 *         ┌──────────────────▼────────────────┐   │
 *         │SLOT_EXPORT_READ_ESTABLISH_RESPONSE┼───┤
 *         └──────────────────┬────────────────┘   │
 *    Full response read (+OK)│                    │
 *           ┌────────────────▼──────────────┐     │ Error Conditions:
 *           │SLOT_EXPORT_WAITING_TO_SNAPSHOT┼─────┤  1. User sends CANCELSLOTMIGRATIONS
 *           └────────────────┬──────────────┘     │  2. Slot ownership change
 *      No other child process│                    │  3. Demotion to replica
 *               ┌────────────▼───────────┐        │  4. FLUSHDB
 *               │SLOT_EXPORT_SNAPSHOTTING┼────────┤  5. Connection Lost
 *               └────────────┬───────────┘        │  6. AUTH failed
 *               Snapshot done│                    │  7. ERR from ESTABLISH command
 *                ┌───────────▼─────────┐          │  8. Unpaused before failover completed
 *                │SLOT_EXPORT_STREAMING┼──────────┤  9. Snapshot failed (e.g. Child OOM)
 *                └───────────┬─────────┘          │  10. No ack from target (timeout)
 *               REQUEST-PAUSE│                    │  11. Client output buffer overrun
 *             ┌──────────────▼─────────────┐      │
 *             │SLOT_EXPORT_WAITING_TO_PAUSE┼──────┤
 *             └──────────────┬─────────────┘      │
 *              Buffer drained│                    │
 *             ┌──────────────▼────────────┐       │
 *             │SLOT_EXPORT_FAILOVER_PAUSED┼───────┤
 *             └──────────────┬────────────┘       │
 *    Failover request granted│                    │
 *            ┌───────────────▼────────────┐       │
 *            │SLOT_EXPORT_FAILOVER_GRANTED┼───────┤
 *            └───────────────┬────────────┘       │
 *       New topology received│                    │
 *             ┌──────────────▼───────────┐        │
 *             │SLOT_MIGRATION_JOB_SUCCESS│        │
 *             └──────────────────────────┘        │
 *                                                 │
 *             ┌─────────────────────────┐         │
 *             │SLOT_MIGRATION_JOB_FAILED│◄────────┤
 *             └─────────────────────────┘         │
 *                                                 │
 *            ┌────────────────────────────┐       │
 *            │SLOT_MIGRATION_JOB_CANCELLED│◄──────┘
 *            └────────────────────────────┘
 *
 */

/* Returns 1 if the given slot is being exported, 0 otherwise. */
bool clusterIsSlotExporting(int slot) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (job->type != SLOT_MIGRATION_EXPORT) continue;
        if (isSlotMigrationJobFinished(job)) continue;
        if (isSlotInSlotRanges(slot, job->slot_ranges)) return true;
    }
    return false;
}

/* Returns the name of the slot export target for the given slot, or NULL if the
 * slot is not being exported. */
bool clusterSlotFailoverGranted(int slot) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (job->type != SLOT_MIGRATION_EXPORT) continue;
        if (!isSlotMigrationJobInProgress(job)) continue;
        if (!isSlotInSlotRanges(slot, job->slot_ranges)) continue;
        return job->state == SLOT_EXPORT_FAILOVER_GRANTED;
    }
    return false;
}

/* Sent by an operator to the current owner of one or more slot ranges. The
 * source will attempt to migrate the slot ranges to the specified target
 * node. */
void clusterCommandMigrateSlots(client *c) {
    if (!nodeIsPrimary(server.cluster->myself)) {
        addReplyError(c, "Slot migration can only be used on primary nodes.");
        return;
    }

    if (isAnySlotInManualImportingState()) {
        addReplyError(c, "Slots are being manually imported");
        return;
    }
    if (isAnySlotInManualMigratingState()) {
        addReplyError(c, "Slots are being manually migrated");
        return;
    }

    int curr_index = 2;
    list *new_slot_migrations = listCreate();
    listSetFreeMethod(new_slot_migrations, freeSlotMigrationJob);
    list *slot_ranges = NULL;

    while (curr_index < c->argc) {
        if (strcasecmp(c->argv[curr_index]->ptr, "slotsrange")) {
            addReplyErrorObject(c, shared.syntaxerr);
            goto cleanup;
        }
        curr_index++;

        clusterNode *source_node = NULL;
        slot_ranges = parseSlotRangesOrReply(c, curr_index, &curr_index,
                                             &source_node);
        if (slot_ranges == NULL) {
            goto cleanup;
        }
        if (source_node != server.cluster->myself) {
            addReplyError(c, "Slots are not served by this node.");
            goto cleanup;
        }

        listIter li;
        listNode *ln;
        listRewind(slot_ranges, &li);
        while ((ln = listNext(&li))) {
            slotRange *range = (slotRange *)ln->value;
            for (int j = range->start_slot; j <= range->end_slot; j++) {
                if (clusterIsSlotExporting(j)) {
                    addReplyErrorFormat(c,
                                        "I am already migrating slot %d.", j);
                    goto cleanup;
                }
            }
        }

        listRewind(new_slot_migrations, &li);
        while ((ln = listNext(&li))) {
            slotMigrationJob *job = (slotMigrationJob *)ln->value;
            if (doSlotRangeListsOverlap(job->slot_ranges, slot_ranges)) {
                addReplyError(c, "Slot ranges in migrations overlap");
                goto cleanup;
            }
        }

        if (curr_index + 1 >= c->argc ||
            strcasecmp(c->argv[curr_index]->ptr, "node")) {
            addReplyErrorObject(c, shared.syntaxerr);
            goto cleanup;
        }
        curr_index++;
        if (sdslen(c->argv[curr_index]->ptr) != CLUSTER_NAMELEN) {
            addReplyErrorFormat(c, "Invalid node name: %s",
                                (sds)c->argv[curr_index]->ptr);
            goto cleanup;
        }
        clusterNode *target_node = clusterLookupNode(c->argv[curr_index]->ptr,
                                                     CLUSTER_NAMELEN);
        if (!target_node) {
            addReplyErrorFormat(c, "Unknown node name: %s",
                                (sds)c->argv[curr_index]->ptr);
            goto cleanup;
        }
        if (target_node == server.cluster->myself) {
            addReplyError(c, "Target node can not be this node.");
            goto cleanup;
        }
        curr_index++;

        slotMigrationJob *job = createSlotExportJob(target_node, slot_ranges);
        listAddNodeHead(new_slot_migrations, job);
        slot_ranges = NULL;
    }

    /* If we reach here, we have successfully parsed all arguments */
    listIter li;
    listRewind(new_slot_migrations, &li);
    listNode *ln;
    sds client_info = catClientInfoShortString(sdsempty(), c,
                                               server.hide_user_data_from_log);
    while ((ln = listNext(&li))) {
        slotMigrationJob *job = ln->value;
        listAddNodeHead(server.cluster->slot_migration_jobs, job);
        serverLog(LL_NOTICE,
                  "Slot migration initiated through CLUSTER MIGRATESLOTS "
                  "command: %s (user request from '%s')",
                  job->description, client_info);
        proceedWithSlotMigration(job);
    }
    listSetFreeMethod(new_slot_migrations, NULL);
    listRelease(new_slot_migrations);
    sdsfree(client_info);
    addReply(c, shared.ok);
    return;

cleanup:
    if (slot_ranges) listRelease(slot_ranges);
    listRelease(new_slot_migrations);
}

slotMigrationJob *clusterLookupMigrationJob(sds name) {
    listNode *ln;
    listIter li;
    if (sdslen(name) != CLUSTER_NAMELEN) return NULL;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (!memcmp(name, job->name, CLUSTER_NAMELEN)) return job;
    }
    return NULL;
}

/* Cancels all ongoing migrations. */
void clusterCommandCancelSlotMigrations(client *c) {
    listNode *ln;
    listIter li;

    if (!clusterIsAnySlotExporting()) {
        addReplyError(c, "No migrations ongoing");
        return;
    }

    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (!isSlotMigrationJobInProgress(job) ||
            job->type == SLOT_MIGRATION_IMPORT) {
            continue;
        }
        finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_CANCELLED, NULL);
    }
    sds client_info = catClientInfoShortString(sdsempty(), c,
                                               server.hide_user_data_from_log);
    serverLog(LL_NOTICE,
              "Canceled all in progress slot migrations (user request from "
              "'%s')",
              client_info);
    sdsfree(client_info);
    addReply(c, shared.ok);
    return;
}

/* Handler for connect callbacks during slot migration job connection. */
void slotExportConnectHandler(connection *conn) {
    slotMigrationJob *job = (slotMigrationJob *)connGetPrivateData(conn);
    proceedWithSlotMigration(job);
}

/* Connect the given job to the target node. The created connection will have
 * the job as private data. */
int connectSlotExportJob(slotMigrationJob *job) {
    clusterNode *n = clusterLookupNode(job->nodename, CLUSTER_NAMELEN);
    int port = getNodeDefaultReplicationPort(n);
    serverLog(LL_NOTICE, "Connecting slot migration %s (ip: %s, port %d)",
              job->description,
              n->ip,
              port);

    job->conn = connCreate(connTypeOfReplication());
    if (connConnect(job->conn, n->ip, port, server.bind_source_addr,
                    0, slotExportConnectHandler) == C_ERR) {
        return C_ERR;
    }

    /* Store the slot migration job in connection private data, until we have a
     * client to store it in. */
    connSetPrivateData(job->conn, job);
    return C_OK;
}

/* Proceed with a previously started connection. If no problems have occurred,
 * C_OK is returned and completed is set. C_ERR will be returned if an issue was
 * encountered. */
int proceedWithSlotExportJobConnecting(slotMigrationJob *job, bool *completed) {
    serverAssert(job->type == SLOT_MIGRATION_EXPORT && job->conn);
    *completed = false;

    switch (connGetState(job->conn)) {
    case CONN_STATE_CONNECTED:
        *completed = true;
        return C_OK;
    case CONN_STATE_CONNECTING: return C_OK;
    default:
        return C_ERR;
    }
}

/* Read a response to the AUTH command, moving to the next stage of the migration
 * if the response is a success. If there is an error, fail the migration with the
 * error message. */
void slotMigrationJobReadAuthResponse(connection *conn) {
    slotMigrationJob *job = (slotMigrationJob *)connGetPrivateData(conn);

    sds err = receiveSynchronousResponse(job->conn);
    if (err == NULL) {
        finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED, "Target node did not respond to AUTH command");
        return;
    }
    if (err[0] == '-') {
        sds status_msg = sdscatfmt(sdsempty(), "Failed to AUTH to target node: %s", err);
        finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED, status_msg);
        sdsfree(err);
        sdsfree(status_msg);
        return;
    }

    sdsfree(err);
    serverLog(LL_NOTICE, "Successfully authenticated slot migration %s", job->description);
    updateSlotMigrationJobState(job, SLOT_EXPORT_SEND_ESTABLISH);
    proceedWithSlotMigration(job);
}

/* Perform the authentication steps needed to authenticate a slot migration
 * job's connection. */
void slotMigrationJobSendAuth(slotMigrationJob *job) {
    serverAssert(job->type == SLOT_MIGRATION_EXPORT);
    serverAssert(server.primary_auth);

    sds err = replicationSendAuth(job->conn);
    if (err) {
        sds status_msg = sdscatfmt(sdsempty(), "Failed to send AUTH command to target node: %s", err);
        finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED, status_msg);
        sdsfree(err);
        sdsfree(status_msg);
        return;
    }

    connSetReadHandler(job->conn, slotMigrationJobReadAuthResponse);
    updateSlotMigrationJobState(job, SLOT_EXPORT_READ_AUTH_RESPONSE);
}

/* Initialize the client for the slot migration job, which should already be
 * connected, authenticated, and established. */
void initSlotExportJobClient(slotMigrationJob *job) {
    serverAssert(job->type == SLOT_MIGRATION_EXPORT);
    job->client = createClient(job->conn);
    job->conn = NULL;
    job->client->flag.authenticated = 1;
    job->client->slot_migration_job = job;
    initClientReplicationData(job->client);
}

/* Generate and store the SYNCSLOTS ESTABLISH command to send to the target for
 * the job. */
sds generateSyncSlotsEstablishCommand(slotMigrationJob *job) {
    serverAssert(job->type == SLOT_MIGRATION_EXPORT);
    sds result = sdscatprintf(sdsempty(),
                              "*%ld\r\n$7\r\nCLUSTER\r\n$9\r\nSYNCSLOTS\r\n"
                              "$9\r\nESTABLISH\r\n$6\r\nSOURCE\r\n$40\r\n"
                              "%.40s\r\n$4\r\nNAME\r\n$40\r\n%.40s\r\n"
                              "$10\r\nSLOTSRANGE\r\n",
                              8 + listLength(job->slot_ranges) * 2,
                              server.cluster->myself->name,
                              job->name);
    listIter li;
    listNode *ln;
    listRewind(job->slot_ranges, &li);
    while ((ln = listNext(&li))) {
        slotRange *range = (slotRange *)ln->value;
        sdscatfmt(result, "$%i\r\n%i\r\n$%i\r\n%i\r\n",
                  digits10(range->start_slot), range->start_slot,
                  digits10(range->end_slot), range->end_slot);
    }
    return result;
}

/* There are two potential triggers for streaming (whichever happens first):
 *   1. SYNCSLOTS REQUEST-PAUSE command
 *   2. BGSAVE child process completes
 */
void slotExportBeginStreaming(slotMigrationJob *job) {
    serverAssert(job->type == SLOT_MIGRATION_EXPORT);
    updateSlotMigrationJobState(job, SLOT_EXPORT_STREAMING);

    /* When the slot export is not ready, it will skip adding the client to the
     * pending write queue (creating a backlog of pending commands). If any
     * data is pending there, we need to manually put it in the write queue to
     * flush it. */
    putClientInPendingWriteQueue(job->client);

    serverLog(LL_NOTICE,
              "Slot migration %s snapshot finished, starting streaming.",
              job->description);
}

/* Attempt to pause the provided slot export job. If we can pause, the state
 * will be updated to SLOT_EXPORT_FAILOVER_PAUSED and the PAUSED subcommand is
 * sent to the target. Otherwise, there is no effect. */
int slotExportTryDoPause(slotMigrationJob *job) {
    serverAssert(job->type == SLOT_MIGRATION_EXPORT);

    if (server.debug_slot_migration_prevent_pause ||
        (server.slot_migration_max_failover_repl_bytes >= 0 &&
         getClientOutputBufferMemoryUsage(job->client) >
             (size_t)server.slot_migration_max_failover_repl_bytes)) {
        return C_ERR;
    }
    serverLog(LL_NOTICE,
              "Pausing writes to allow slot migration %s to finalize failover.",
              job->description);
    job->mf_end = mstime() + server.cluster_mf_timeout * CLUSTER_MF_PAUSE_MULT;
    pauseActions(PAUSE_DURING_SLOT_MIGRATION, job->mf_end,
                 PAUSE_ACTIONS_CLIENT_WRITE_SET);
    sendSyncSlotsMessage(job, "PAUSED");
    return C_OK;
}

/* Sent by the target to the source to pause writes to the slot for slot
 * failover. */
void clusterCommandSyncSlotsRequestPause(client *c) {
    if (!c->slot_migration_job) {
        addReplyError(c, "CLUSTER SYNCSLOTS REQUEST-PAUSE should only be used "
                         "by slot migration clients");
        return;
    }
    serverAssert(isSlotMigrationJobInProgress(c->slot_migration_job));
    /* Child process may not have closed yet, so SNAPSHOTTING is okay here */
    if (c->slot_migration_job->state != SLOT_EXPORT_STREAMING &&
        c->slot_migration_job->state != SLOT_EXPORT_SNAPSHOTTING) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS REQUEST-PAUSE for slot migration "
                  "%s, but the client was not streaming incremental updates. "
                  "Failing migration.",
                  c->slot_migration_job->description);
        finishSlotMigrationJob(c->slot_migration_job, SLOT_MIGRATION_JOB_FAILED,
                               "Unexpected state machine transition");
        return;
    }
    if (c->slot_migration_job->state != SLOT_EXPORT_STREAMING) {
        slotExportBeginStreaming(c->slot_migration_job);
    }

    if (slotExportTryDoPause(c->slot_migration_job) == C_ERR) {
        updateSlotMigrationJobState(c->slot_migration_job,
                                    SLOT_EXPORT_WAITING_TO_PAUSE);
        return;
    }
    updateSlotMigrationJobState(c->slot_migration_job,
                                SLOT_EXPORT_FAILOVER_PAUSED);
}

/* Sent by the target to the source to request final authorization for
 * failover. Authorization could be denied if the source has unpaused itself by
 * now. */
void clusterCommandSyncSlotsRequestFailover(client *c) {
    if (!c->slot_migration_job) {
        addReplyError(c,
                      "CLUSTER SYNCSLOTS REQUEST-FAILOVER should only be used "
                      "by slot migration clients");
        return;
    }
    serverAssert(isSlotMigrationJobInProgress(c->slot_migration_job));
    if (c->slot_migration_job->state != SLOT_EXPORT_FAILOVER_PAUSED) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS REQUEST-FAILOVER for slot "
                  "migration %s, but the client was not in the paused state. "
                  "Failing migration.",
                  c->slot_migration_job->description);
        finishSlotMigrationJob(c->slot_migration_job, SLOT_MIGRATION_JOB_FAILED,
                               "Unexpected state machine transition");
        return;
    }

    /* Do one last check, since we could have unpaused in the background. */
    if (isSlotExportPauseTimedOut(c->slot_migration_job)) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS REQUEST-FAILOVER on slot "
                  "migration %s, but we are not paused. Denying failover.",
                  c->slot_migration_job->description);
        finishSlotMigrationJob(c->slot_migration_job, SLOT_MIGRATION_JOB_FAILED,
                               "Unpaused before failover completed");
        return;
    }

    /* Renew our pause to help ensure we don't unpause before the gossip is
     * propagated. If the existing pause is longer than this, it will be
     * honored */
    mstime_t prop_deadline = mstime() + CLUSTER_OPERATION_TIMEOUT;
    if (c->slot_migration_job->mf_end < prop_deadline) {
        c->slot_migration_job->mf_end = prop_deadline;
        pauseActions(PAUSE_DURING_SLOT_MIGRATION, prop_deadline,
                     PAUSE_ACTIONS_CLIENT_WRITE_SET);
    }

    sendSyncSlotsMessage(c->slot_migration_job, "FAILOVER-GRANTED");
    updateSlotMigrationJobState(c->slot_migration_job,
                                SLOT_EXPORT_FAILOVER_GRANTED);
}

/* Predicate function supplied to rewriteAppendOnlyFileRio to filter to only
 * slots in this migration. */
bool shouldRewriteHashtableIndex(int didx, hashtable *ht, void *privdata) {
    UNUSED(ht);
    return isSlotInSlotRanges(didx, (list *)privdata);
}

/* Contains the logic run on the child process during the snapshot phase. */
int childSnapshotForSyncSlot(int req, rio *rdb, void *privdata) {
    UNUSED(req);
    list *slot_ranges = privdata;
    size_t key_count = 0;
    for (int db_num = 0; db_num < server.dbnum; db_num++) {
        listIter li;
        listNode *ln;
        listRewind(slot_ranges, &li);
        while ((ln = listNext(&li))) {
            slotRange *r = (slotRange *)ln->value;
            for (int slot = r->start_slot; slot <= r->end_slot; slot++) {
                if (rewriteSlotToAppendOnlyFileRio(
                        rdb, db_num, slot, &key_count) == C_ERR) return C_ERR;
            }
        }
    }
    rioWrite(rdb, "*3\r\n", 4);
    rioWriteBulkString(rdb, "CLUSTER", 7);
    rioWriteBulkString(rdb, "SYNCSLOTS", 9);
    rioWriteBulkString(rdb, "SNAPSHOT-EOF", 12);
    return C_OK;
}

/* Begin the snapshot for the provided job in a child process. */
int slotExportJobBeginSnapshot(slotMigrationJob *job) {
    connection **conns = zmalloc(sizeof(connection *));
    *conns = job->client->conn;
    rdbSnapshotOptions opts = {
        .connsnum = 1,
        .conns = conns,
        .use_pipe = 1,
        .req = REPLICA_REQ_NONE,
        .skip_checksum = 1,
        .snapshot_func = childSnapshotForSyncSlot,
        .privdata = job->slot_ranges};
    if (saveSnapshotToConnectionSockets(opts) != C_OK) {
        return C_ERR;
    }
    if (server.debug_pause_after_fork) debugPauseProcess();
    return C_OK;
}

/* Callback triggered after snapshot is finished. We either begin sending the
 * incremental contents or fail the associated migration. */
void clusterHandleSlotExportBackgroundSaveDone(int bgsaveerr) {
    if (!server.cluster_enabled) return;
    listIter li;
    listNode *ln;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li))) {
        slotMigrationJob *job = (slotMigrationJob *)ln->value;
        if (job->type != SLOT_MIGRATION_EXPORT) continue;
        if (job->state != SLOT_EXPORT_SNAPSHOTTING) {
            continue;
        }
        if (bgsaveerr == C_OK) {
            slotExportBeginStreaming(job);
        } else {
            serverLog(LL_WARNING,
                      "Child process failed to snapshot slot migration %s",
                      job->description);
            finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                   "Failed to perform snapshot");
        }
    }
}

/* Returns 1 if the attached slot migration job for the client is ready to flush
 * the data from the output buffer. Returns 0 otherwise. While this returns 0,
 * replicated data for this slot migration will accumulate in the client output
 * buffer. */
bool clusterSlotMigrationShouldInstallWriteHandler(client *c) {
    slotMigrationJob *job = c->slot_migration_job;
    if (!job || job->type != SLOT_MIGRATION_EXPORT) {
        return true;
    }
    return job->state != SLOT_EXPORT_SNAPSHOTTING;
}

/* Feed the slot export jobs with the given argv and argc from the replication
 * log. Slot is pre-populated using the original command that was run. */
void clusterFeedSlotExportJobs(int dbid, robj **argv, int argc, int slot) {
    if (slot == -1) {
        /* We can safely ignore any commands with no keys. This includes
         * MULTI/EXEC. This isn't a problem since the entire slot migration is
         * atomically visible and therefore transactions are redundant. */
        return;
    }

    listIter li;
    listNode *ln;
    listRewind(server.cluster->slot_migration_jobs, &li);

    /* Select cmd will be generated only if at least one client needs it. */
    robj *selectcmd = NULL;
    while ((ln = listNext(&li))) {
        slotMigrationJob *job = (slotMigrationJob *)ln->value;
        if (job->type != SLOT_MIGRATION_EXPORT) continue;
        if (!job->client) continue;
        if (!isSlotMigrationJobInProgress(job) ||
            job->state < SLOT_EXPORT_SNAPSHOTTING) continue;
        if (!isSlotInSlotRanges(slot, job->slot_ranges)) continue;

        if (dbid != job->client->db->id) {
            serverAssert(selectDb(job->client, dbid) == C_OK);
            if (!selectcmd) {
                selectcmd = generateSelectCommand(dbid);
            }
            addReply(job->client, selectcmd);
        }

        addReplyArrayLen(job->client, argc);
        for (int i = 0; i < argc; i++) {
            addReplyBulk(job->client, argv[i]);
        }
    }
    if (selectcmd) {
        decrRefCount(selectcmd);
    }
}

bool isSlotExportPauseTimedOut(slotMigrationJob *job) {
    return job->mf_end < mstime() ||
           !getPausedActionsWithPurpose(PAUSE_DURING_SLOT_MIGRATION);
}

/* Revalidate that the migration's slots are still owned by ourselves, or fail
 * the migration otherwise. */
int checkSlotExportOwnership(slotMigrationJob *job, bool *migration_done) {
    serverAssert(job->type == SLOT_MIGRATION_EXPORT);
    *migration_done = false;
    if (!isSlotMigrationJobInProgress(job)) {
        return C_OK;
    }
    clusterNode *n = getClusterNodeBySlotRanges(job->slot_ranges, NULL);
    if (n) {
        if (n == server.cluster->myself) {
            return C_OK;
        } else if (!memcmp(n->name, job->nodename, CLUSTER_NAMELEN)) {
            *migration_done = true;
            serverLog(LL_NOTICE,
                      "Slot migration %s slots are now owned by target node.",
                      job->description);
            return C_OK;
        }
    }

    serverLog(LL_WARNING,
              "Slot migration %s contains slots that are no longer all owned "
              "by myself",
              job->description);
    return C_ERR;
}

/* Called within topology updates to update any slot exports immediately
 * when the ownership changes. Will unpause if all paused slot migration jobs
 * are now done. */
void clusterUpdateSlotExportsOnOwnershipChange(void) {
    listNode *ln;
    listIter li;
    int paused = 0;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li))) {
        slotMigrationJob *job = (slotMigrationJob *)ln->value;
        if (job->type != SLOT_MIGRATION_EXPORT) continue;
        bool migration_done;
        if (checkSlotExportOwnership(job, &migration_done) == C_ERR) {
            finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                   "Slots are no longer owned by myself");
        } else if (migration_done) {
            finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_SUCCESS, NULL);
        }
        if (job->mf_end) {
            paused++;
        }
    }
    if (!paused && getPausedActionsWithPurpose(PAUSE_DURING_SLOT_MIGRATION)) {
        unpauseActions(PAUSE_DURING_SLOT_MIGRATION);
    }
}

bool clusterIsAnySlotExporting(void) {
    if (!server.cluster_enabled || !server.cluster) return false;
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (job->type != SLOT_MIGRATION_EXPORT) continue;
        if (!isSlotMigrationJobFinished(job)) {
            return true;
        }
    }
    return false;
}

void clusterFailAllSlotExportsWithMessage(char *message) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (job->type != SLOT_MIGRATION_EXPORT) continue;
        if (!isSlotMigrationJobInProgress(job)) continue;
        finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED, message);
    }
}

size_t clusterGetTotalSlotExportBufferMemory(void) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    size_t result = 0;
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (job->type != SLOT_MIGRATION_EXPORT || job->client == NULL) continue;
        result += getClientOutputBufferMemoryUsage(job->client);
    }
    return result;
}

/* Create a slot export job with the given target and slot ranges. */
slotMigrationJob *createSlotExportJob(clusterNode *target_node,
                                      list *slot_ranges) {
    slotMigrationJob *job = zcalloc(sizeof(slotMigrationJob));

    job->ctime = server.unixtime;
    job->last_update = job->ctime;
    job->last_ack = job->ctime;
    job->type = SLOT_MIGRATION_EXPORT;
    job->state = SLOT_EXPORT_CONNECTING;
    job->slot_ranges = slot_ranges;
    job->slot_ranges_str = representSlotRangeList(slot_ranges);
    getRandomHexChars(job->name, sizeof(job->name));
    memcpy(job->nodename, target_node->name, CLUSTER_NAMELEN);
    job->description = generateSlotMigrationJobDescription(job, target_node);
    return job;
}

/* Read a response to the SYNCSLOTS ESTABLISH response, accumulating it in a
 * buffer, and moving to the next stage of the migration if the response is a
 * success. If there is an error, fail the migration with the error message. */
void slotMigrationJobReadEstablishResponse(connection *conn) {
    client *c = (client *)connGetPrivateData(conn);
    slotMigrationJob *job = c->slot_migration_job;
    if (c->flag.close_asap || !isSlotMigrationJobInProgress(job)) {
        return;
    }
    if (!job->response_buf) {
        job->response_buf = sdsempty();
        job->response_buf = sdsMakeRoomForNonGreedy(job->response_buf,
                                                    PROTO_IOBUF_LEN);
    }

    int result;
    result = connRead(conn,
                      ((char *)job->response_buf) + sdslen(job->response_buf),
                      sdsavail(job->response_buf));
    if (result > 0) {
        sdsIncrLen(job->response_buf, result);
    }
    if (conn->state != CONN_STATE_CONNECTED) {
        freeClientAsync(c);
        return;
    }
    if (sdslen(job->response_buf) < 2 ||
        job->response_buf[sdslen(job->response_buf) - 2] != '\r' ||
        job->response_buf[sdslen(job->response_buf) - 1] != '\n') {
        if (sdsavail(job->response_buf) == 0) {
            /* We filled up the buffer, and we still have no response. Only
             * choice is to stop the migration. */
            finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                   "Response to establish job is larger than "
                                   "buffer limit");
            return;
        }
        connSetReadHandler(conn, slotMigrationJobReadEstablishResponse);
        return;
    }
    if (job->response_buf[0] == '-') {
        sds err_msg = sdscatfmt(sdsempty(), "Received error during handshake "
                                            "to target: %S",
                                job->response_buf);
        finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED, err_msg);
        sdsfree(err_msg);
        return;
    }

    updateSlotMigrationJobState(job, SLOT_EXPORT_WAITING_TO_SNAPSHOT);
    connSetReadHandler(conn, readQueryFromClient);
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);

    /* We need to send an ACK to take the import out of WAIT_ACK state. This
     * will kickstart the health checks, effectively taking the job online. */
    sendSyncSlotsMessage(job, "ACK");

    sdsfree(job->response_buf);
    job->response_buf = NULL;
}

/* ----------------------------- TARGET & SOURCE ---------------------------- */

/* Updates the associated status message for the job, which will be seen in
 * CLUSTER GETSLOTMIGRATIONS. */
void updateSlotMigrationJobStatusMessage(slotMigrationJob *job, char *message) {
    if (job->status_msg) sdsfree(job->status_msg);
    job->status_msg = sdsnew(message);
}

/* proceedWithSlotMigration contains the main logic for driving the slot
 * migration state machine. */
void proceedWithSlotMigration(slotMigrationJob *job) {
    /* Continue within the state machine until we have no more work. */
    while (1) {
        switch (job->state) {
        /* Importing states */
        case SLOT_IMPORT_WAIT_ACK:
            /* Waiting for ACK */
            return;
        case SLOT_IMPORT_RECEIVE_SNAPSHOT:
            /* Waiting for SNAPSHOT-EOF marker */
            return;
        case SLOT_IMPORT_WAITING_FOR_PAUSED:
            /* Waiting for PAUSED marker */
            return;
        case SLOT_IMPORT_FAILOVER_REQUESTED:
            /* Waiting for FAILOVER-GRANTED response */
            return;
        case SLOT_IMPORT_FAILOVER_GRANTED:
            if (!server.debug_slot_migration_prevent_failover) {
                performSlotImportJobFailover(job);

                serverLog(LL_NOTICE,
                          "Slot migration %s completed successfully. "
                          "This node is now the owner of the slots",
                          job->description);
                finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_SUCCESS, NULL);
            }
            return;
        case SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP:
            delKeysNotOwnedByMyself(job->slot_ranges);
            setSlotImportingStateInAllDbs(job->slot_ranges, 0);
            updateSlotMigrationJobState(job, job->post_cleanup_state);
            return;

        /* Exporting states */
        case SLOT_EXPORT_CONNECTING: {
            int status = C_OK;
            bool completed = false;
            if (!job->conn) {
                status = connectSlotExportJob(job);
            } else {
                status = proceedWithSlotExportJobConnecting(job, &completed);
            }
            if (status == C_ERR) {
                sds status_msg =
                    sdscatfmt(sdsempty(),
                              "Unable to connect to target node: %s",
                              connGetLastError(job->conn));
                finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                       status_msg);
                sdsfree(status_msg);
                return;
            }
            if (!completed) return;
            serverLog(LL_NOTICE, "Slot migration %s connection established.",
                      job->description);
            if (server.primary_auth) {
                updateSlotMigrationJobState(job, SLOT_EXPORT_SEND_AUTH);
            } else {
                updateSlotMigrationJobState(job, SLOT_EXPORT_SEND_ESTABLISH);
            }
            continue;
        }
        case SLOT_EXPORT_SEND_AUTH:
            slotMigrationJobSendAuth(job);
            continue;
        case SLOT_EXPORT_READ_AUTH_RESPONSE:
            /* We are still reading back the response, nothing to do in cron */
            return;
        case SLOT_EXPORT_SEND_ESTABLISH:
            initSlotExportJobClient(job);
            addReplySds(job->client, generateSyncSlotsEstablishCommand(job));
            connSetReadHandler(job->client->conn,
                               slotMigrationJobReadEstablishResponse);
            updateSlotMigrationJobState(job,
                                        SLOT_EXPORT_READ_ESTABLISH_RESPONSE);
            return;
        case SLOT_EXPORT_READ_ESTABLISH_RESPONSE:
            /* We are still reading back the response, nothing to do in cron */
            return;
        case SLOT_EXPORT_WAITING_TO_SNAPSHOT:
            /* Perform the snapshot whenever there is no child process.
             *
             * We also check that there are no pending writes here. Since we are
             * sending ACKs at this time, we could have unfortunate timing where
             * an ACK is added to the output buffer just before snapshotting,
             * adding the client to the pending write queue. This write might be
             * flushed after some other command is enqueued during the snapshot,
             * resulting in premature flush of the output buffer and data
             * consistency issues. To prevent this, we defer snapshot until
             * there are no pending writes. */
            if (hasActiveChildProcess() || job->client->flag.pending_write) {
                run_with_period(5000) {
                    serverLog(LL_NOTICE,
                              "Slot migration %s waiting before snapshotting "
                              "due to %s.",
                              job->description,
                              hasActiveChildProcess()
                                  ? "active child process"
                                  : "pending writes in output buffer");
                }
                return;
            }
            serverLog(LL_NOTICE,
                      "Beginning snapshot of slot migration %s.",
                      job->description);
            if (slotExportJobBeginSnapshot(job) == C_ERR) {
                serverLog(LL_WARNING,
                          "Slot migration %s failed to start slot snapshot",
                          job->description);
                finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                       "Failed to start snapshot");
                return;
            }
            updateSlotMigrationJobState(job, SLOT_EXPORT_SNAPSHOTTING);
            return;
        case SLOT_EXPORT_SNAPSHOTTING:
            /* Waiting for child process to finish */
            return;
        case SLOT_EXPORT_STREAMING:
            /* Waiting for PAUSE command to come in */
            return;
        case SLOT_EXPORT_WAITING_TO_PAUSE:
            if (slotExportTryDoPause(job) == C_OK) {
                updateSlotMigrationJobState(job, SLOT_EXPORT_FAILOVER_PAUSED);
            }
            return;
        case SLOT_EXPORT_FAILOVER_PAUSED:
            if (isSlotExportPauseTimedOut(job)) {
                serverLog(LL_WARNING, "Slot migration %s timed out during slot "
                                      "failover.",
                          job->description);
                job->mf_end = 0;
                updatePausedActions();
                finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                       "Timed out before streaming completed");
            }
            return;
        case SLOT_EXPORT_FAILOVER_GRANTED:
            if (isSlotExportPauseTimedOut(job)) {
                /* Although we make strong efforts to ensure all data is
                 * transferred, we do need to eventually unpause if the target
                 * node does not claim ownership in time. Some scenarios that
                 * could cause this are:
                 *
                 * 1. The target node crashed during finalization
                 * 2. A network partition formed as the finalization was
                 *    occurring
                 * 3. This node is suffering performance issues and hasn't yet
                 *    processed the topology update.
                 *
                 * Depending on when such an issue happens, the topology change
                 * may still propagate to us after we unpause. In this case, we
                 * would lose the slot (as our epoch is lower) and any writes we
                 * have since accepted will be reverted. */
                serverLog(LL_WARNING, "Write loss risk! During slot migration "
                                      "%s, new owner did not broadcast "
                                      "ownership before we unpaused ourselves. "
                                      "Any writes we have recorded since "
                                      "unpausing may be lost!",
                          job->description);

                finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                       "Unpaused before migration completed "
                                       "(migration may have succeeded with "
                                       "lost writes)");
                return;
            }
            bool migration_done;
            if (checkSlotExportOwnership(job, &migration_done) == C_ERR) {
                finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                       "Slots are no longer owned by myself");
                return;
            }
            if (migration_done) {
                finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_SUCCESS, NULL);
            }
            return;

        /* Terminal states */
        case SLOT_MIGRATION_JOB_SUCCESS:
        case SLOT_MIGRATION_JOB_CANCELLED:
        case SLOT_MIGRATION_JOB_FAILED:
            return;
        }
    }
}

/* Reset the client and connection information associated with the job, leaving
 * the migration related metadata. */
void resetSlotMigrationJob(slotMigrationJob *job) {
    /* Only one of client or conn should be set. */
    serverAssert(!job->client || !job->conn);
    if (job->client) {
        freeClientAsync(job->client);
        job->client = NULL;
    } else if (job->conn) {
        connClose(job->conn);
        job->conn = NULL;
    }

    sdsfree(job->response_buf);
    job->response_buf = NULL;

    /* Description is not needed once migration is finished */
    sdsfree(job->description);
    job->description = NULL;
}

void freeSlotMigrationJob(void *o) {
    slotMigrationJob *job = o;
    resetSlotMigrationJob(job);
    listRelease(job->slot_ranges);
    sdsfree(job->slot_ranges_str);
    sdsfree(job->status_msg);
    sdsfree(job->response_buf);
    zfree(o);
}

void initClusterSlotMigrationJobList(void) {
    server.cluster->slot_migration_jobs = listCreate();
    listSetFreeMethod(server.cluster->slot_migration_jobs,
                      freeSlotMigrationJob);
}

/* Convert a slotMigrationJobState enum to a user presentable string. */
const char *slotMigrationJobStateToString(slotMigrationJobState state) {
    switch (state) {
    case SLOT_IMPORT_WAIT_ACK: return "waiting-for-ack";
    case SLOT_IMPORT_RECEIVE_SNAPSHOT: return "receiving-snapshot";
    case SLOT_IMPORT_WAITING_FOR_PAUSED: return "waiting-for-paused";
    case SLOT_IMPORT_FAILOVER_REQUESTED: return "failover-requested";
    case SLOT_IMPORT_FAILOVER_GRANTED: return "failover-granted";
    case SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP: return "cleaning-up";

    case SLOT_EXPORT_CONNECTING: return "connecting";
    case SLOT_EXPORT_SEND_AUTH: return "sending-auth-command";
    case SLOT_EXPORT_READ_AUTH_RESPONSE: return "reading-auth-response";
    case SLOT_EXPORT_SEND_ESTABLISH: return "sending-establish-command";
    case SLOT_EXPORT_READ_ESTABLISH_RESPONSE:
        return "reading-establish-response";
    case SLOT_EXPORT_WAITING_TO_SNAPSHOT: return "waiting-to-snapshot";
    case SLOT_EXPORT_SNAPSHOTTING: return "snapshotting";
    case SLOT_EXPORT_STREAMING: return "replicating";
    case SLOT_EXPORT_WAITING_TO_PAUSE: return "waiting-to-pause";
    case SLOT_EXPORT_FAILOVER_PAUSED: return "failover-paused";
    case SLOT_EXPORT_FAILOVER_GRANTED: return "failover-granted";

    case SLOT_MIGRATION_JOB_SUCCESS: return "success";
    case SLOT_MIGRATION_JOB_CANCELLED: return "cancelled";
    case SLOT_MIGRATION_JOB_FAILED: return "failed";
    }
    return "unknown";
}

sds generateSlotMigrationJobDescription(slotMigrationJob *job,
                                        clusterNode *node) {
    char *other_node_desc =
        job->type == SLOT_MIGRATION_EXPORT ? "target_node" : "source_node";
    if (sdslen(node->human_nodename) > 0) {
        return sdscatprintf(sdsempty(),
                            "{name: %.40s, operation: %s, %s_id: %.40s, "
                            "%s_human_name: %s, slots: %s}",
                            job->name,
                            job->type == SLOT_MIGRATION_EXPORT
                                ? "export"
                                : "import",
                            other_node_desc, node->name, other_node_desc,
                            node->human_nodename, job->slot_ranges_str);
    } else {
        return sdscatprintf(sdsempty(),
                            "{name: %.40s, operation: %s, %s_id: %.40s, "
                            "slots: %s}",
                            job->name,
                            job->type == SLOT_MIGRATION_EXPORT
                                ? "export"
                                : "import",
                            other_node_desc, node->name,
                            job->slot_ranges_str);
    }
}

/* Update the provided job to the given state. */
void updateSlotMigrationJobState(slotMigrationJob *job,
                                 slotMigrationJobState state) {
    serverLog(LL_NOTICE,
              "Slot migration %s state transition: %s -> %s",
              job->description,
              slotMigrationJobStateToString(job->state),
              slotMigrationJobStateToString(state));
    job->last_update = server.unixtime;
    job->state = state;
}

void clusterHandleSlotMigrationErrorResponse(slotMigrationJob *job) {
    if (!job || !isSlotMigrationJobInProgress(job)) {
        return;
    }
    finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                           "Failed to process command during slot migration. "
                           "Check logs for more information");
}

/* Callback triggered when a client with a slot migration client is closed. */
void clusterHandleSlotMigrationClientClose(slotMigrationJob *job) {
    job->client = NULL;
    if (!isSlotMigrationJobInProgress(job)) {
        return;
    }
    serverLog(LL_NOTICE, "Slot migration %s connection lost", job->description);

    /* If we have granted failover, the failover may have happened, but we don't
     * know. We keep the slot export around so that we remain paused until we
     * find out about the takeover (or until the pause times out).
     *
     * Otherwise, we can mark it failed. */
    if (job->state != SLOT_EXPORT_FAILOVER_GRANTED) {
        if (job->type == SLOT_MIGRATION_EXPORT) {
            finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                   "Connection lost to target. Check CLUSTER "
                                   "GETSLOTMIGRATIONS on the target node for "
                                   "more information.");
        } else {
            finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                                   "Connection lost to source. Check CLUSTER "
                                   "GETSLOTMIGRATIONS on the source node for "
                                   "more information.");
        }
    }
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
}

/* Callback triggered when a slot migration client is unable to apply a mutation
 * due to OOM. */
void clusterHandleSlotMigrationClientOOM(slotMigrationJob *job) {
    if (job->type != SLOT_MIGRATION_IMPORT) return;
    if (!isSlotMigrationJobInProgress(job)) {
        return;
    }
    serverLog(LL_WARNING,
              "Slot migration %s failed due to OOM", job->description);
    finishSlotMigrationJob(job, SLOT_MIGRATION_JOB_FAILED,
                           "Ran out of memory (OOM) during slot import");
}

/* Move the given job to the provided terminal state. Any associated connections
 * or clients will be closed, and this function will trigger cleanup if this is
 * an import operation. */
void finishSlotMigrationJob(slotMigrationJob *job,
                            slotMigrationJobState state,
                            char *message) {
    serverLog(state == SLOT_MIGRATION_JOB_FAILED ? LL_WARNING : LL_NOTICE,
              "Slot migration %s finished. State: %s, Message: %s",
              job->description,
              slotMigrationJobStateToString(state), message ? message : "none");
    updateSlotMigrationJobStatusMessage(job, message);

    if (job->type == SLOT_MIGRATION_EXPORT) {
        /* If we finish the export, we should not remain paused */
        job->mf_end = 0;
        slotExportTryUnpause();
        /* Fast fail the child process, which will be cleaned up fully in
         * checkChildrenDone. */
        if (job->state == SLOT_EXPORT_SNAPSHOTTING) killRDBChild();
    }
    if (job->type == SLOT_MIGRATION_IMPORT &&
        job->state != SLOT_MIGRATION_JOB_SUCCESS) {
        /* Defer cleanup until beforeSleep. */
        job->post_cleanup_state = state;
        state = SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP;
        clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
    }
    updateSlotMigrationJobState(job, state);
    resetSlotMigrationJob(job);
}

/* Finished means we are completely done with all work and this entry is just
 * a log for tracking purposes. */
bool isSlotMigrationJobFinished(slotMigrationJob *job) {
    return job->state == SLOT_MIGRATION_JOB_SUCCESS ||
           job->state == SLOT_MIGRATION_JOB_CANCELLED ||
           job->state == SLOT_MIGRATION_JOB_FAILED;
}

/* In progress means we are still trying to perform the migration. It is
 * possible that we are not trying to perform the migration, but we are not
 * finished yet, e.g. if we are still pending cleanup. */
bool isSlotMigrationJobInProgress(slotMigrationJob *job) {
    return job->state != SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP &&
           !isSlotMigrationJobFinished(job);
}

/* Since slotMigrationJob is stored as void* in the client object, this allows
 * other files to determine if a migration is an import or export without
 * knowing the details of the migration job struct. */
bool isImportSlotMigrationJob(slotMigrationJob *job) {
    return job->type == SLOT_MIGRATION_IMPORT;
}

/* Synthesizes a view of ongoing and recently completed imports for an
 * operator. */
void clusterCommandGetSlotMigrations(client *c) {
    listNode *ln;
    listIter li;
    addReplyArrayLen(c, listLength(server.cluster->slot_migration_jobs));
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        addReplyMapLen(c, 9);
        addReplyBulkCString(c, "name");
        addReplyBulkCBuffer(c, job->name, CLUSTER_NAMELEN);
        addReplyBulkCString(c, "operation");
        addReplyBulkCString(c, job->type == SLOT_MIGRATION_IMPORT
                                   ? "IMPORT"
                                   : "EXPORT");
        addReplyBulkCString(c, "slot_ranges");
        addReplyBulkCString(c, job->slot_ranges_str);
        addReplyBulkCString(c, "node");
        addReplyBulkCBuffer(c, job->nodename, CLUSTER_NAMELEN);
        addReplyBulkCString(c, "create_time");
        addReplyLongLong(c, job->ctime);
        addReplyBulkCString(c, "last_update_time");
        addReplyLongLong(c, job->last_update);
        addReplyBulkCString(c, "last_ack_time");
        addReplyLongLong(c, job->last_ack);
        addReplyBulkCString(c, "state");
        addReplyBulkCString(c, slotMigrationJobStateToString(job->state));
        addReplyBulkCString(c, "message");
        addReplyBulkCString(c, job->status_msg ? job->status_msg : "");
    }
}


/* Helper function to send a SYNCSLOTS subcommand for the provided job. */
void sendSyncSlotsMessage(slotMigrationJob *job, const char *subcommand) {
    serverAssert(job->client);
    ClientFlags old_flags = job->client->flag;
    if (job->type == SLOT_MIGRATION_IMPORT) {
        /* Since the slot migration import has replies off, we use the pushing
         * flag to bypass this. */
        job->client->flag.pushing = 1;
    }
    addReplyArrayLen(job->client, 3);
    addReplyBulkCString(job->client, "CLUSTER");
    addReplyBulkCString(job->client, "SYNCSLOTS");
    addReplyBulkCString(job->client, subcommand);
    if (!old_flags.pushing) job->client->flag.pushing = 0;
}

/* Cleanup any finished slot migrations when we are over the max log length. */
void clusterCleanupSlotMigrationLog(void) {
    listNode *ln;
    listIter li;
    listRewindTail(server.cluster->slot_migration_jobs, &li);
    while (server.cluster->slot_migration_jobs->len >
               server.cluster_slot_migration_log_max_len &&
           (ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (isSlotMigrationJobFinished(job)) {
            listDelNode(server.cluster->slot_migration_jobs, ln);
        }
    }
}

/* Returns true if the slot migration is okay to periodically send ACKs, or
 * false otherwise. */
bool canSlotMigrationJobSendAck(slotMigrationJob *job) {
    /* 1. We cannot send ACK from parent process while child is snapshotting
     * 2. We don't send an ACK from the import side until the export has first
     *    sent one (thus taking us out of SLOT_IMPORT_WAIT_ACK). This simplifies
     *    parsing of the response to CLUSTER SYNCSLOTS ESTABLISH.
     * 3. We can't send ACK if we are still connecting or sending establish
     *    job. */
    return job->state != SLOT_EXPORT_SNAPSHOTTING &&
           job->state != SLOT_IMPORT_WAIT_ACK &&
           job->state != SLOT_EXPORT_CONNECTING &&
           job->state != SLOT_EXPORT_SEND_AUTH &&
           job->state != SLOT_EXPORT_READ_AUTH_RESPONSE &&
           job->state != SLOT_EXPORT_SEND_ESTABLISH &&
           job->state != SLOT_EXPORT_READ_ESTABLISH_RESPONSE;
}

/* Cron related tasks run in clusterCron to drive slot migrations. */
void clusterSlotMigrationCron(void) {
    slotMigrationJob *job;
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        job = ln->value;

        /* Note that after granting failover, we no longer care about the
         * connection timeout, since we will use pause timeout. */
        if (isSlotMigrationJobInProgress(job) &&
            job->state != SLOT_EXPORT_FAILOVER_GRANTED) {
            serverAssert(job->type == SLOT_MIGRATION_EXPORT || job->client);
            /* For imports, last interaction will be set to the last
             * incoming command, as replicated clients don't set
             * last_interaction when a reply is sent. However, for exports,
             * we have to use the last ack time to avoid counting sending
             * data/ACKs as an interaction here. */
            time_t last_interaction = job->type == SLOT_MIGRATION_EXPORT
                                          ? job->last_ack
                                          : job->client->last_interaction;

            if (last_interaction &&
                (server.unixtime - last_interaction > server.repl_timeout)) {
                serverLog(LL_WARNING,
                          "Timing out slot migration %s "
                          "after not receiving ack for too long",
                          job->description);
                finishSlotMigrationJob(
                    job, SLOT_MIGRATION_JOB_FAILED,
                    "Timed out after too long with no interaction");
                continue;
            }
            /* Send acks only when the child process isn't writing to it. */
            if (canSlotMigrationJobSendAck(job)) {
                run_with_period(1000) sendSyncSlotsMessage(job, "ACK");
            }
        }
        proceedWithSlotMigration(job);
    }

    clusterCleanupSlotMigrationLog();
    slotExportTryUnpause();
}

void slotExportTryUnpause(void) {
    if (getPausedActionsWithPurpose(PAUSE_DURING_SLOT_MIGRATION) == 0) return;
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_jobs, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationJob *job = ln->value;
        if (job->mf_end) {
            return;
        }
    }
    unpauseActions(PAUSE_DURING_SLOT_MIGRATION);
}

/* Sent by either the target or the source as a liveness check. */
void clusterCommandSyncSlotsAck(client *c) {
    if (!c->slot_migration_job) {
        addReplyError(c,
                      "CLUSTER SYNCSLOTS ACK should only be used by slot "
                      "migration clients");
        return;
    }
    c->slot_migration_job->last_ack = server.unixtime;
    if (c->slot_migration_job->state == SLOT_IMPORT_WAIT_ACK) {
        updateSlotMigrationJobState(c->slot_migration_job,
                                    SLOT_IMPORT_RECEIVE_SNAPSHOT);
    }
}

/* Sent by either the target or the source as a control message for progressing
 * with slot import. */
void clusterCommandSyncSlots(client *c) {
    if (c->flag.primary) {
        /* Since target primaries proxy slot migration source commands to
         * replicas through chaining replication (direct bytewise copy, not
         * commandwise propagation), SYNCSLOTS should be ignored from our
         * primary. */
        return;
    }
    if (!strcasecmp(c->argv[2]->ptr, "establish")) {
        /* CLUSTER SYNCSLOTS ESTABLISH <args> */
        clusterCommandSyncSlotsEstablish(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "snapshot-eof")) {
        /* CLUSTER SYNCSLOTS SNAPSHOT-EOF */
        clusterCommandSyncSlotsSnapshotEof(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "request-pause")) {
        /* CLUSTER SYNCSLOTS REQUEST-PAUSE */
        clusterCommandSyncSlotsRequestPause(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "paused")) {
        /* CLUSTER SYNCSLOTS PAUSED */
        clusterCommandSyncSlotsPaused(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "request-failover")) {
        /* CLUSTER SYNCSLOTS REQUEST-FAILOVER */
        clusterCommandSyncSlotsRequestFailover(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "failover-granted")) {
        /* CLUSTER SYNCSLOTS FAILOVER-GRANTED */
        clusterCommandSyncSlotsFailoverGranted(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "ack")) {
        /* CLUSTER SYNCSLOTS ACK */
        clusterCommandSyncSlotsAck(c);
    } else {
        if (c->slot_migration_job &&
            isSlotMigrationJobInProgress(c->slot_migration_job)) {
            serverLog(LL_WARNING, "Received unknown SYNCSLOTS subcommand from "
                                  "slot migration %s. Failing the migration.",
                      c->slot_migration_job->description);
            finishSlotMigrationJob(c->slot_migration_job,
                                   SLOT_MIGRATION_JOB_FAILED,
                                   "Unknown SYNCSLOTS subcommand used");
            return;
        }
        addReplyErrorObject(c, shared.syntaxerr);
    }
}
