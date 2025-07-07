/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include "cluster_migrate.h"
#include "bio.h"
#include "module.h"
#include "functions.h"

static int isSlotMigrationLinkInProgress(slotMigrationLink *link);
static slotMigrationLink *createSlotImportLink(client *c, char *nodename, char *linkname, list *slot_ranges);
static void connectSlotExportLink(slotMigrationLink *link);
static const char *slotMigrationLinkStateToString(slotMigrationLinkState state);
static void updateSlotMigrationLinkState(slotMigrationLink *link, slotMigrationLinkState state);
static void sendSyncSlotsMessageOnClient(client *c, const char *subcommand);
static void sendSyncSlotsMessageOnLink(slotMigrationLink *link, const char *subcommand);
static void proceedWithSlotMigration(slotMigrationLink *link);
static slotMigrationLink *createSlotExportLink(clusterNode *target_node, list *slot_ranges);
static int isSlotExportPauseTimedOut(slotMigrationLink *link);
static void resetSlotMigrationLink(slotMigrationLink *link);
static void finishSlotMigrationLink(slotMigrationLink *link, slotMigrationLinkState state, char *message);
static void updateSlotMigrationLinkStatusMessage(slotMigrationLink *link, char *message);
static void freeSlotMigrationLink(void *o);


list *createSlotRangeList(void) {
    list *slot_ranges = listCreate();
    listSetFreeMethod(slot_ranges, zfree);
    return slot_ranges;
}

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
                *cross_node = 1;
                return NULL;
            }
        }
    }
    return n;
}

int isSlotInSlotRanges(int slot, list *slot_ranges) {
    /* Loop to check if the slot in any slot range. */
    listNode *ln;
    listIter li;
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        if (slot >= range->start_slot && slot <= range->end_slot) {
            return 1;
        }
    }
    return 0;
}

int doSlotRangesOverlap(slotRange *range1, slotRange *range2) {
    return range1->end_slot >= range2->start_slot && range1->start_slot <= range2->end_slot;
}

int doSlotRangeListsOverlap(list *ranges1, list *ranges2) {
    /* Since they aren't guaranteed to be sorted, just use a nested loop. */
    listIter li1, li2;
    listNode *ln1, *ln2;
    listRewind(ranges1, &li1);
    listRewind(ranges2, &li2);
    while ((ln1 = listNext(&li1)) != NULL) {
        while ((ln2 = listNext(&li2)) != NULL) {
            slotRange *range1 = ln1->value;
            slotRange *range2 = ln2->value;
            if (doSlotRangesOverlap(range1, range2)) {
                return 1;
            }
        }
    }
    return 0;
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
                delKeysInSlot(i, 1, 1, 0);
            }
        }
    }
}

/* Returns if any slot has been put in IMPORTING state via SETSLOT command. */
int isAnySlotInManualImportingState(void) {
    for (int i = 0; i < CLUSTER_SLOTS; i++) {
        if (server.cluster->importing_slots_from[i] != NULL) {
            return 1;
        }
    }
    return 0;
}

/* Returns if any slot has been put in MIGRATING state via SETSLOT command. */
int isAnySlotInManualMigratingState(void) {
    for (int i = 0; i < CLUSTER_SLOTS; i++) {
        if (server.cluster->migrating_slots_to[i] != NULL) {
            return 1;
        }
    }
    return 0;
}

/* Parse as many slot ranges starting from start_index, returning a list of parsed slot ranges, or
 * NULL if there is an error (in which case err_out will be set). If parsing completes successfully,
 * end_index_out will be set to the index of the next argument needed to be parsed in c->argv.
 *
 * Note that all slots in the slot range should belong to a sole node in the cluster topology.
 * node_out will be set to the node that owns all the slots. */
list *parseSlotRanges(client *c, int start_index, int *end_index_out, clusterNode **node_out, sds *err_out) {
    list *slot_ranges = createSlotRangeList();
    int startslot, endslot;
    *node_out = NULL;
    *end_index_out = c->argc;
    for (int i = start_index; i < c->argc; i += 2) {
        if (getLongLongFromObject(c->argv[i], NULL) != C_OK) {
            /* If we encounter a non-integer parameter, we assume that this is the next argument. */
            *end_index_out = i;
            break;
        }
        /* Get the current slot range. */
        if ((startslot = getSlotOrError(c->argv[i], err_out)) == -1) {
            listRelease(slot_ranges);
            return NULL;
        }
        if (i + 1 >= c->argc) {
            *err_out = sdsnew("No end slot for final slot range");
            listRelease(slot_ranges);
            return NULL;
        }
        if ((endslot = getSlotOrError(c->argv[i + 1], err_out)) == -1) {
            listRelease(slot_ranges);
            return NULL;
        }
        if (startslot > endslot) {
            *err_out = sdscatprintf(sdsempty(),
                                    "Start slot number %d is greater than end slot number %d.",
                                    startslot, endslot);
            listRelease(slot_ranges);
            return NULL;
        }
        /* Check if the current slot range is ready to do the slot sync. */
        for (int j = startslot; j <= endslot; j++) {
            if (server.cluster->slots[j] == NULL) {
                *err_out = sdscatprintf(sdsempty(), "Slot %d has no node served.", j);
                listRelease(slot_ranges);
                return NULL;
            }
            if (!*node_out) {
                *node_out = server.cluster->slots[j];
            } else if (*node_out != server.cluster->slots[j]) {
                *err_out = sdsnew("The slot ranges are not all owned by the same node, "
                                  "please check slots and try again.");
                listRelease(slot_ranges);
                return NULL;
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
                *err_out = sdscatprintf(sdsempty(),
                                        "Slot range %d-%d overlaps with previous range %d-%d.",
                                        startslot,
                                        endslot,
                                        prev_range->start_slot,
                                        prev_range->end_slot);
                listRelease(slot_ranges);
                zfree(new_range);
                return NULL;
            }
        }

        /* Add the current slot range to the range list. */
        listAddNodeTail(slot_ranges, new_range);
    }
    if (slot_ranges->len == 0) {
        *err_out = sdsnew("No slot ranges specified");
        listRelease(slot_ranges);
        return NULL;
    }
    return slot_ranges;
}

/* -------------------------------------------- TARGET -----------------------------------------
 *
 * During a slot import, the target drives the main state machine and eventually performs the
 * slot takeover. Slot import is initiated when an operator sends a CLUSTER IMPORT request, after
 * which the target node will track the import in a slotMigrationLink.
 *
 * For transient errors like connections being dropped, the target will restart the import workflow
 * from the beginning. For other errors, the import will be marked as failed and require operator
 * intervention to retry.
 *
 * An operator can view the status of imports with CLUSTER MIGRATIONS, and cancel imports with
 * CLUSTER IMPORT-CANCEL.
 */

int clusterIsSlotImporting(int slot) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationLink *link = ln->value;
        if (link->type != SLOT_MIGRATION_IMPORT) continue;
        if (!isSlotMigrationLinkInProgress(link)) continue;
        if (isSlotInSlotRanges(slot, link->slot_ranges)) return 1;
    }
    return 0;
}

/* Sent by the source to the target to initiate the AOF formatted snapshot.
 * Note that if there is an error in the request, we send a fail message in
 * order to prevent infinite retry in the case of incompatibility.
 *
 * CLUSTER SYNCSLOTS ESTABLISH is the only CLUSTER SYNCSLOTS subcommand that
 * will return a reply. Errors are written from the perspective of the end user
 * to help with debugging migrations. */
void clusterCommandSyncSlotsEstablish(client *c) {
    char *link_name = NULL;
    clusterNode *source_node = NULL;
    clusterNode *owning_node = NULL;
    sds err = NULL;
    list *slot_ranges = NULL;

    if (!nodeIsPrimary(server.cluster->myself)) {
        addReplyError(c, "Target node is not a primary");
        return;
    }

    if (isAnySlotInManualImportingState() || isAnySlotInManualMigratingState()) {
        addReplyError(c, "A slot on the target node is being manually imported or migrated");
        return;
    }

    if (c->slot_migration_link) {
        addReplyError(c, "Slot migration client is already a slot migration link");
        return;
    }

    /* Order agnostic. We skip unknown key/value pairs forwards compatibility. */
    int i = 3;
    while (i < c->argc) {
        if (!strcasecmp(c->argv[i]->ptr, "source")) {
            if (source_node || i + 1 >= c->argc || sdslen(c->argv[i + 1]->ptr) != CLUSTER_NAMELEN) {
                addReplyErrorObject(c, shared.syntaxerr);
                goto cleanup;
            }
            source_node = clusterLookupNode(c->argv[4]->ptr, CLUSTER_NAMELEN);
            if (!source_node) {
                addReplyError(c, "Target node does not know the source node");
                goto cleanup;
            }
            i += 2;
            continue;
        }
        if (!strcasecmp(c->argv[i]->ptr, "linkname")) {
            if (link_name || i + 1 >= c->argc || sdslen(c->argv[i + 1]->ptr) != CLUSTER_NAMELEN) {
                addReplyErrorObject(c, shared.syntaxerr);
                goto cleanup;
            }
            link_name = c->argv[i + 1]->ptr;
            i += 2;
            continue;
        }
        if (!strcasecmp(c->argv[i]->ptr, "slotsrange")) {
            if (slot_ranges) {
                addReplyErrorObject(c, shared.syntaxerr);
                goto cleanup;
            }
            /* parseSlotRanges will set i for the next iteration */
            slot_ranges = parseSlotRanges(c, i + 1, &i, &owning_node, &err);
            if (err) {
                addReplyErrorSds(c, sdscatfmt(sdsempty(), "Failed to parse slot ranges on target node: %S", err));
                sdsfree(err);
                goto cleanup;
            }

            listIter li;
            listNode *ln;
            listRewind(slot_ranges, &li);
            while ((ln = listNext(&li)) != NULL) {
                slotRange *range = ln->value;
                for (int i = range->start_slot; i <= range->end_slot; i++) {
                    if (clusterIsSlotImporting(i)) {
                        addReplyError(c, "Slot is already being imported on the target by a different migration");
                        goto cleanup;
                    }
                }
            }

            continue;
        }
        addReplyErrorObject(c, shared.syntaxerr);
        goto cleanup;
    }
    if (!source_node || !link_name || !slot_ranges) {
        addReplyErrorObject(c, shared.syntaxerr);
        goto cleanup;
    }
    if (source_node != owning_node) {
        addReplyError(c, "Target node does not agree about current slot ownership");
        goto cleanup;
    }

    slotMigrationLink *link = createSlotImportLink(c, source_node->name, link_name, slot_ranges);
    listAddNodeHead(server.cluster->slot_migration_links, link);

    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);

    /* Push the reply, which will allow the migration to continue */
    ClientFlags old_flags = link->client->flag;
    link->client->flag.pushing = 1;
    addReply(c, shared.ok);
    link->client->flag = old_flags;
    return;

cleanup:
    if (slot_ranges) {
        listRelease(slot_ranges);
    }
    return;
}

/* Sent by the source to the target after dumping the snapshot in AOF format. */
void clusterCommandSyncSlotsSnapshotEof(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_IMPORT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS SNAPSHOT-EOF from client %llu, "
                  "but the client is not a slot import source. Closing link.",
                  (unsigned long long)c->id);
        freeClientAsync(c);
        return;
    }
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    if (link->state != SLOT_IMPORT_RECEIVE_SNAPSHOT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS SNAPSHOT-EOF from link %.40s, "
                  "but not currently loading an AOF snapshot. Closing link.",
                  link->linkname);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                                "Unexpected state machine transition");
        return;
    }
    serverLog(LL_NOTICE,
              "Slot import link %.40s successfully received slot snapshot from %.40s "
              "(owner of slots [%s]). Beginning incremental stream...",
              link->linkname,
              link->nodename,
              link->slot_ranges_str);
    sendSyncSlotsMessageOnLink(link, "PAUSE");
    updateSlotMigrationLinkState(link, SLOT_IMPORT_WAITING_FOR_PAUSED);
}

/* Sent by the source to the target as a marker of when the pause
 * began (therefore, target is caught up once read). */
void clusterCommandSyncSlotsPaused(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_IMPORT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS PAUSED from client %llu, "
                  "but the client is not a slot import source. Closing link.",
                  (unsigned long long)c->id);
        freeClientAsync(c);
        return;
    }
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    if (link->state != SLOT_IMPORT_WAITING_FOR_PAUSED) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS PAUSED from link %.40s, "
                  "but client is not currently in paused state locally. Closing link.",
                  link->linkname);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                                "Unexpected state machine transition");
        return;
    }
    sendSyncSlotsMessageOnLink(link, "REQUEST-FAILOVER");
    updateSlotMigrationLinkState(link, SLOT_IMPORT_FAILOVER_REQUESTED);
}

/* Sent by the source to the target to grant final authorization for
 * failover. */
void clusterCommandSyncSlotsFailoverGranted(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_IMPORT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS FAILOVER-GRANTED from client %llu, "
                  "but the client is not a slot import source. Closing link.",
                  (unsigned long long)c->id);
        freeClientAsync(c);
        return;
    }
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    if (link->state != SLOT_IMPORT_FAILOVER_REQUESTED) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS FAILOVER-GRANTED from link %.40s, "
                  "but we never sent a failover request. Closing link.",
                  link->linkname);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                                "Unexpected state machine transition");
        return;
    }
    updateSlotMigrationLinkState(link, SLOT_IMPORT_FAILOVER_GRANTED);
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
}

/* Sent by the source to the target to deny final authorization for
 * failover. */
void clusterCommandSyncSlotsFailoverDenied(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_IMPORT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS FAILOVER-DENIED from client %llu, "
                  "but the client is not a slot import source. Closing link.",
                  (unsigned long long)c->id);
        freeClientAsync(c);
        return;
    }
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    if (link->state != SLOT_IMPORT_FAILOVER_REQUESTED) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS FAILOVER-DENIED from link %.40s, "
                  "but we never sent a failover request. Closing link.",
                  link->linkname);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                                "Unexpected state machine transition");
        return;
    }
    serverLog(LL_WARNING,
              "Slot import link %.40s had failover denied from node %.40s "
              "(owner of slots [%s]). Failing import request.",
              link->linkname,
              link->nodename,
              link->slot_ranges_str);
    finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Failover denied");
    return;
}

slotMigrationLink *createSlotImportLink(client *c, char *nodename, char *linkname, list *slot_ranges) {
    slotMigrationLink *link = zcalloc(sizeof(slotMigrationLink));
    memcpy(link->linkname, linkname, CLUSTER_NAMELEN);
    memcpy(link->nodename, nodename, CLUSTER_NAMELEN);
    link->ctime = server.unixtime;
    link->last_update = link->ctime;
    link->last_ack = link->ctime;
    link->type = SLOT_MIGRATION_IMPORT;
    link->slot_ranges = slot_ranges;
    link->slot_ranges_str = representSlotRangeList(slot_ranges);
    link->state = SLOT_IMPORT_RECEIVE_SNAPSHOT;
    link->client = c;
    link->client->slot_migration_link = link;
    link->client->flag.reply_off = 1;

    /* We treat slot imports like primaries. Primaries are expected to have a
     * dedicated query buffer and allocated replication data. */
    initClientReplicationData(link->client);
    link->client->querybuf = sdsempty();

    serverLog(LL_NOTICE,
              "New slot import link created: link name %.40s, "
              "source node %.40s, slot ranges %s.",
              link->linkname,
              link->nodename,
              link->slot_ranges_str);
    return link;
}

/* Validate that the slot migration is still owned by the node it originally
 * was importing from. If the source node has since changed, or no longer has
 * sole ownership by the original source node, we fail the migration. Otherwise,
 * return the source node for this migration. */
clusterNode *validateClusterNodeForImportIsUnchanged(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_IMPORT);
    int cross_node;
    clusterNode *n = getClusterNodeBySlotRanges(link->slot_ranges, &cross_node);
    if (n && !memcmp(n->name, link->nodename, CLUSTER_NAMELEN)) {
        return n;
    }
    if (n == server.cluster->myself) {
        serverLog(LL_WARNING,
                  "Slot import link %.40s to node %.40s (previous owner of slots [%s]) "
                  "already owned by myself. Failing slot import...",
                  link->linkname,
                  link->nodename,
                  link->slot_ranges_str);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                                "Slots were unexpectedly assigned to myself during import");
        return NULL;
    }
    serverLog(LL_WARNING,
              "Slot import link %.40s to node %.40s (previous owner of slots [%s]) "
              "contains at least one slot no longer owned by the source node. "
              "Failing slot import...",
              link->linkname,
              link->nodename,
              link->slot_ranges_str);
    finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                            "Slots are no longer owned by source node");
    return NULL;
}

/* This function implements the final part of manual slot failovers,
 * where the replica grabs all the slot migration link's hash slots, and
 * propagates the new configuration.
 *
 * Note that it's up to the caller to be sure that the node got a new
 * configuration epoch already. */
void performSlotImportLinkFailover(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_IMPORT);
    /* 1) Force bump the epoch to facilitate propagation. */
    clusterBumpConfigEpochWithoutConsensus();

    /* 2) Claim all the slots in the slot migration link to myself. */
    listNode *ln;
    listIter li;
    listRewind(link->slot_ranges, &li);
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
    clusterSaveConfigOrDie(1);

    /* 4) Pong all the other nodes so that they can update the state accordingly
     *    and detect that we switched to master role. */
    clusterBroadcastPong(CLUSTER_BROADCAST_ALL);

    /* 5) Reflect the failover in the slot migration link state */
    serverLog(LL_NOTICE,
              "Slot import link %.40s completed import successfully. "
              "This node is now the owner of slots (%s)",
              link->linkname,
              link->slot_ranges_str);
    finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_SUCCESS, NULL);
}

int clusterIsAnySlotImporting(void) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationLink *link = ln->value;
        if (link->type != SLOT_MIGRATION_IMPORT) continue;
        if (isSlotMigrationLinkInProgress(link)) return 1;
    }
    return 0;
}

/* Called within topology updates to update any slot imports immediately
 * when the ownership changes. Will fail import if any of our imports are no
 * longer valid. */
void clusterUpdateSlotImportsOnOwnershipChange(void) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li))) {
        slotMigrationLink *link = (slotMigrationLink *)ln->value;
        if (link->type != SLOT_MIGRATION_IMPORT) continue;
        if (!isSlotMigrationLinkInProgress(link)) continue;
        if (!nodeIsPrimary(server.cluster->myself)) {
            finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "I was demoted to a replica");
            continue;
        }
        validateClusterNodeForImportIsUnchanged(link);
    }
}

void clusterHandleFlushDuringSlotMigration(void) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li))) {
        slotMigrationLink *link = (slotMigrationLink *)ln->value;
        if (!isSlotMigrationLinkInProgress(link)) continue;
        /* Note that if we are exporting, we don't send a FAIL message, so the target should
         * reconnect and complete the migration shortly after, since we now have no data.
         *
         * If we are importing, we fail the migration and expect the operator to retry. */
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Data was flushed");
    }
}

/* ------------------------------------------- SOURCE ------------------------------------------
 *
 * During a slot import, the source node tracks ongoing export operations as slotMigrationLinks.
 * A slotMigrationLink is initially created when the target connects and sends a SYNCSLOTS command
 * to us. After this, we ensure that all data in the requested slots are sent to the target node.
 *
 * If at any time we detect an error, the source side can send a CLUSTER SYNCSLOTS FAIL message
 * to fail the slot migration. If the error is retriable, the connection to the slot import can
 * simply be dropped, and the target can choose to retry from there.
 */

int clusterIsSlotExporting(int slot) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationLink *link = ln->value;
        if (link->type != SLOT_MIGRATION_EXPORT) continue;
        if (!isSlotMigrationLinkInProgress(link)) continue;
        if (isSlotInSlotRanges(slot, link->slot_ranges)) return 1;
    }
    return 0;
}

/* Sent by an operator to the current owner of one or more slot ranges. The
 * source will attempt to migrate the slot ranges to the specified target node. */
void clusterCommandMigrate(client *c) {
    if (!nodeIsPrimary(server.cluster->myself)) {
        addReplyError(c, "Slot migration can only be used on primary nodes.");
        return;
    }

    if (isAnySlotInManualImportingState()) {
        addReplyError(c, "Some slots are being manually imported. "
                         "Please get all slots to a stable state before attempting import.");
        return;
    }
    if (isAnySlotInManualMigratingState()) {
        addReplyError(c, "Some slots are being manually migrated. "
                         "Please get all slots to a stable state before attempting import.");
        return;
    }

    int curr_index = 2;
    sds err = NULL;
    list *new_slot_migrations = listCreate();
    listSetFreeMethod(new_slot_migrations, freeSlotMigrationLink);

    while (curr_index < c->argc) {
        if (strcasecmp(c->argv[curr_index]->ptr, "slotsrange")) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
        curr_index++;

        clusterNode *source_node = NULL;
        list *slot_ranges = parseSlotRanges(c, curr_index, &curr_index, &source_node, &err);
        if (err != NULL) {
            addReplyErrorSds(c, err);
            listRelease(new_slot_migrations);
            return;
        }
        if (source_node != server.cluster->myself) {
            addReplyErrorFormat(c, "Slots are not served by myself.");
            listRelease(slot_ranges);
            listRelease(new_slot_migrations);
            return;
        }

        listIter li;
        listNode *ln;
        listRewind(slot_ranges, &li);
        while ((ln = listNext(&li))) {
            slotRange *range = (slotRange *)ln->value;
            for (int j = range->start_slot; j <= range->end_slot; j++) {
                if (clusterIsSlotExporting(j)) {
                    addReplyErrorFormat(c, "I am already migrating slot %d.", j);
                    listRelease(slot_ranges);
                    listRelease(new_slot_migrations);
                    return;
                }
            }
        }

        if (curr_index + 1 >= c->argc || strcasecmp(c->argv[curr_index]->ptr, "node")) {
            addReplyErrorObject(c, shared.syntaxerr);
            listRelease(slot_ranges);
            listRelease(new_slot_migrations);
            return;
        }
        curr_index++;
        if (sdslen(c->argv[curr_index]->ptr) != CLUSTER_NAMELEN) {
            addReplyErrorFormat(c, "Invalid node name");
            listRelease(slot_ranges);
            listRelease(new_slot_migrations);
            return;
        }
        clusterNode *target_node = clusterLookupNode(c->argv[curr_index]->ptr, CLUSTER_NAMELEN);
        if (!target_node) {
            addReplyErrorFormat(c, "Unknown node name");
            listRelease(slot_ranges);
            listRelease(new_slot_migrations);
            return;
        }
        curr_index++;

        slotMigrationLink *link = createSlotExportLink(target_node, slot_ranges);
        listAddNodeHead(new_slot_migrations, link);
    }

    /* If we reach here, we have successfully parsed all arguments */
    listIter li;
    listRewind(new_slot_migrations, &li);
    listNode *ln;
    while ((ln = listNext(&li))) {
        slotMigrationLink *link = ln->value;
        listAddNodeHead(server.cluster->slot_migration_links, link);
        serverLog(LL_NOTICE,
                  "New slot export link created: link name %.40s, target node %.40s, slot range %s",
                  link->linkname,
                  link->nodename,
                  link->slot_ranges_str);
        proceedWithSlotMigration(link);
    }
    listSetFreeMethod(new_slot_migrations, NULL);
    listRelease(new_slot_migrations);
    addReply(c, shared.ok);
}

slotMigrationLink *clusterLookupExportLink(sds linkname) {
    listNode *ln;
    listIter li;
    if (sdslen(linkname) != CLUSTER_NAMELEN) return NULL;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationLink *link = ln->value;
        if (link->type != SLOT_MIGRATION_EXPORT) continue;
        if (!memcmp(linkname, link->linkname, CLUSTER_NAMELEN)) return link;
    }
    return NULL;
}

/* Cancels one or all ongoing imports. */
void clusterCommandCancelMigration(client *c) {
    listNode *ln;
    listIter li;

    if (!strcasecmp(c->argv[2]->ptr, "all")) {
        if (!clusterIsAnySlotExporting()) {
            addReplyError(c, "No migrations ongoing");
            return;
        }

        listRewind(server.cluster->slot_migration_links, &li);
        while ((ln = listNext(&li)) != NULL) {
            slotMigrationLink *link = ln->value;
            if (!isSlotMigrationLinkInProgress(link) || link->type == SLOT_MIGRATION_IMPORT) {
                continue;
            }
            finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_CANCELLED, NULL);
        }
        serverLog(LL_NOTICE, "Canceled all in progress slot migrations due to operator's request.");
        addReply(c, shared.ok);
        return;
    } else if (!strcasecmp(c->argv[2]->ptr, "link") && c->argc > 3) {
        slotMigrationLink *link = clusterLookupExportLink(c->argv[3]->ptr);
        if (!link || !isSlotMigrationLinkInProgress(link)) {
            addReplyErrorFormat(c, "No outgoing migration with link name found.");
            return;
        }
        if (link->type == SLOT_MIGRATION_IMPORT) {
            addReplyErrorFormat(c, "Migrations must be cancelled on the node that currently owns the slots.");
            return;
        }
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_CANCELLED, NULL);
        serverLog(LL_NOTICE,
                  "Slot export link %.40s to node %.40s (destination of slots [%s]) "
                  "cancelled due to operator's request.",
                  link->linkname,
                  link->nodename,
                  link->slot_ranges_str);
        addReply(c, shared.ok);
        return;
    } else {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
}


void slotExportConnectHandler(connection *conn) {
    slotMigrationLink *link = (slotMigrationLink *)connGetPrivateData(conn);
    proceedWithSlotMigration(link);
}

void connectSlotExportLink(slotMigrationLink *link) {
    sds status_msg;
    clusterNode *n = clusterLookupNode(link->nodename, CLUSTER_NAMELEN);
    int port = getNodeDefaultClientPort(n);
    serverLog(LL_NOTICE,
              "Connecting slot export link %.40s to %.40s "
              "(destination of slots [%s]) at (ip: %s, port %d)",
              link->linkname,
              link->nodename,
              link->slot_ranges_str,
              n->ip,
              port);

    link->conn = connCreate(connTypeOfReplication());
    if (connConnect(link->conn, n->ip, port, server.bind_source_addr,
                    /*multipath=*/0, slotExportConnectHandler) == C_ERR) {
        serverLog(LL_WARNING,
                  "Failed to connect slot export link %.40s to %.40s: %s",
                  link->linkname,
                  link->nodename,
                  connGetLastError(link->conn));
        status_msg = sdscatfmt(sdsempty(), "Unable to connect to target node: %s",
                               connGetLastError(link->conn));
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, status_msg);
        sdsfree(status_msg);
        return;
    }

    /* Store the slot migration link in connection private data, until we have a
     * client to store it in. */
    connSetPrivateData(link->conn, link);
}

/* Determine if the slot migration link is connected, and return 1 if so. Handles state transition
 * in the case of success/failure. Returns if the connection is done. */
int proceedWithSlotExportLinkConnecting(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_EXPORT && link->conn);
    switch (connGetState(link->conn)) {
    case CONN_STATE_CONNECTED:
        serverLog(LL_NOTICE,
                  "Slot export link %.40s connected to node %.40s (destination of slots [%s]).",
                  link->linkname,
                  link->nodename,
                  link->slot_ranges_str);
        updateSlotMigrationLinkState(link, SLOT_EXPORT_AUTHENTICATING);
        return 1;
    case CONN_STATE_CONNECTING: return 0;
    default:
        serverLog(LL_NOTICE,
                  "Failed to connect to target node %.40s for export of slots (%s): %s",
                  link->nodename,
                  link->slot_ranges_str,
                  connGetLastError(link->conn));
        sds status_msg = sdscatfmt(sdsempty(), "Unable to connect to target node: %s",
                                   connGetLastError(link->conn));
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, status_msg);
        sdsfree(status_msg);
        return 1;
    }
}

void performSlotExportLinkAuthentication(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_EXPORT);
    sds status_msg;
    if (!server.primary_auth) {
        updateSlotMigrationLinkState(link, SLOT_EXPORT_ESTABLISH_LINK);
        return;
    }
    char *err = replicationSendAuth(link->conn);
    if (err) {
        serverLog(LL_NOTICE,
                  "Failed to send AUTH command to node %.40s for export of slots (%s): %s",
                  link->nodename,
                  link->slot_ranges_str,
                  err);
        status_msg = sdscatfmt(sdsempty(), "Failed to send AUTH to target node: %s", err);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, status_msg);
        sdsfree(err);
        sdsfree(status_msg);
        return;
    }
    err = receiveSynchronousResponse(link->conn);
    if (err == NULL) {
        serverLog(LL_WARNING,
                  "Received no response to AUTH command from node %.40s for export of slots (%s)",
                  link->nodename,
                  link->slot_ranges_str);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                                "AUTH command received no response");
        return;
    }
    if (err[0] == '-') {
        serverLog(LL_WARNING,
                  "Failed to AUTH to node %.40s for import of slots (%s): %s",
                  link->nodename,
                  link->slot_ranges_str, err);
        status_msg = sdscatfmt(sdsempty(), "Failed to AUTH to target node: %s", err);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, status_msg);
        sdsfree(status_msg);
    } else {
        serverLog(LL_NOTICE,
                  "Successfully authenticated to node %.40s for export of slots (%s)",
                  link->nodename,
                  link->slot_ranges_str);
        updateSlotMigrationLinkState(link, SLOT_EXPORT_ESTABLISH_LINK);
    }
    sdsfree(err);
}

void initSlotExportLinkClient(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_EXPORT);
    link->client = createClient(link->conn);
    link->conn = NULL;
    link->client->flag.authenticated = 1;
    link->client->slot_migration_link = link;
    initClientReplicationData(link->client);
}

sds generateEstablishLinkCommand(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_EXPORT);
    sds result = sdscatprintf(sdsempty(),
                              "*%ld\r\n$7\r\nCLUSTER\r\n$9\r\nSYNCSLOTS\r\n"
                              "$9\r\nESTABLISH\r\n$6\r\nSOURCE\r\n$40\r\n"
                              "%.40s\r\n$8\r\nLINKNAME\r\n$40\r\n%.40s\r\n"
                              "$10\r\nSLOTSRANGE\r\n",
                              8 + listLength(link->slot_ranges) * 2,
                              server.cluster->myself->name,
                              link->linkname);
    listIter li;
    listNode *ln;
    listRewind(link->slot_ranges, &li);
    while ((ln = listNext(&li))) {
        slotRange *range = (slotRange *)ln->value;
        sdscatfmt(result, "$%i\r\n%i\r\n$%i\r\n%i\r\n",
                  digits10(range->start_slot), range->start_slot,
                  digits10(range->end_slot), range->end_slot);
    }
    return result;
}

/* There are two potential triggers for streaming (whichever happens first):
 *   1. SYNCSLOTS PAUSE command
 *   2. BGSAVE child process dies
 */
void slotExportBeginStreaming(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_EXPORT);
    updateSlotMigrationLinkState(link, SLOT_EXPORT_STREAMING);

    /* When the slot export is not ready, it will skip adding the client to the
     * pending write queue (creating a backlog of pending commands). If any
     * data is pending there, we need to manually put it in the write queue to
     * flush it. */
    putClientInPendingWriteQueue(link->client);

    serverLog(LL_NOTICE,
              "Slot export link %.40s to node %.40s (for slots [%s]) snapshot finished, "
              "starting streaming.",
              link->linkname,
              link->nodename,
              link->slot_ranges_str);
}

void slotExportTryDoPause(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_EXPORT);

    if (server.debug_slot_migration_prevent_pause ||
        /* TODO - what number? */
        link->client->reply_bytes > 0) {
        return;
    }
    serverLog(LL_NOTICE,
              "Pausing writes to allow slot export link %.40s to synchronize slots (%s) to node %.40s.",
              link->linkname,
              link->slot_ranges_str,
              link->nodename);
    link->mf_end = mstime() + server.cluster_mf_timeout * CLUSTER_MF_PAUSE_MULT;
    pauseActions(PAUSE_DURING_SLOT_MIGRATION, link->mf_end, PAUSE_ACTIONS_CLIENT_WRITE_SET);
    updateSlotMigrationLinkState(link, SLOT_EXPORT_FAILOVER_PAUSED);
    sendSyncSlotsMessageOnLink(link, "PAUSED");
}

/* Sent by the target to the source to pause writes to the slot for slot
 * failover. */
void clusterCommandSyncSlotsPause(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_EXPORT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS PAUSE from client %llu, "
                  "but the client is not a slot migration target. Closing link.",
                  (unsigned long long)c->id);
        freeClientAsync(c);
        return;
    }
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    /* Child process may not have closed yet, so SNAPSHOTTING is okay here */
    if (link->state != SLOT_EXPORT_STREAMING && link->state != SLOT_EXPORT_SNAPSHOTTING) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS PAUSE for migration to node %.40s, "
                  "but the client was not streaming incremental updates. Closing link.",
                  link->nodename);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                                "Unexpected state machine transition");
        return;
    }
    if (link->state != SLOT_EXPORT_STREAMING) {
        slotExportBeginStreaming(link);
    }

    updateSlotMigrationLinkState(link, SLOT_EXPORT_WAITING_TO_PAUSE);
    slotExportTryDoPause(link);
}

/* Sent by the target to the source to request final authorization for
 * failover. Authorization could be denied if the source has unpaused itself by
 * now. */
void clusterCommandSyncSlotsRequestFailover(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_EXPORT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS REQUEST-FAILOVER from client %llu, "
                  "but the client is not a slot migration target. Closing link.",
                  (unsigned long long)c->id);
        freeClientAsync(c);
        return;
    }
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }

    /* Do one last check, since we could have unpaused in the background. */
    if (isSlotExportPauseTimedOut(link) || link->state != SLOT_EXPORT_FAILOVER_PAUSED) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS REQUEST-FAILOVER on export to link %.40s (for slots %s), "
                  "but we are not paused. Denying failover.",
                  link->linkname,
                  link->slot_ranges_str);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Unpaused before failover completed");
        return;
    }

    /* Renew our pause to help ensure we don't unpause before the gossip is
     * propagated. If the existing pause is longer than this, it will be honored */
    mstime_t prop_deadline = mstime() + CLUSTER_OPERATION_TIMEOUT;
    if (link->mf_end < prop_deadline) {
        link->mf_end = prop_deadline;
        pauseActions(PAUSE_DURING_SLOT_MIGRATION, prop_deadline, PAUSE_ACTIONS_CLIENT_WRITE_SET);
    }

    sendSyncSlotsMessageOnLink(link, "FAILOVER-GRANTED");
    updateSlotMigrationLinkState(link, SLOT_EXPORT_FAILOVER_GRANTED);
}


int shouldRewriteHashtableIndex(int didx, void *privdata) {
    return isSlotInSlotRanges(didx, (list *)privdata);
}

int childSnapshotForSyncSlot(int req, rio *rdb, void *privdata) {
    UNUSED(req);
    int retval = rewriteAppendOnlyFileRio(rdb, 1, shouldRewriteHashtableIndex, privdata);
    rioWrite(rdb, "*3\r\n", 4);
    rioWriteBulkString(rdb, "CLUSTER", 7);
    rioWriteBulkString(rdb, "SYNCSLOTS", 9);
    rioWriteBulkString(rdb, "SNAPSHOT-EOF", 12);
    return retval;
}

void slotExportLinkBeginSnapshot(slotMigrationLink *link) {
    connection **conns = zmalloc(sizeof(connection *));
    *conns = link->client->conn;
    serverLog(LL_NOTICE,
              "Beginning snapshot of slot export link %.40s for slots (%s) to target %.40s.",
              link->linkname,
              link->slot_ranges_str,
              link->nodename);
    if (saveSnapshotToConnectionSockets(conns, 1, 1, 0, childSnapshotForSyncSlot, 1,
                                        link->slot_ranges) != C_OK) {
        serverLog(LL_WARNING,
                  "Slot export link %.40s failed to start slot export for slots (%s)",
                  link->linkname,
                  link->slot_ranges_str);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Failed to start snapshot");
        return;
    }
    updateSlotMigrationLinkState(link, SLOT_EXPORT_SNAPSHOTTING);
    if (server.debug_pause_after_fork) debugPauseProcess();
}

void clusterHandleSlotExportBackgroundSaveDone(int bgsaveerr) {
    listIter li;
    listNode *ln;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li))) {
        slotMigrationLink *link = (slotMigrationLink *)ln->value;
        if (link->type != SLOT_MIGRATION_EXPORT) continue;
        if (link->state != SLOT_EXPORT_SNAPSHOTTING) {
            continue;
        }
        if (bgsaveerr == C_OK) {
            serverLog(LL_NOTICE,
                      "Finished snapshotting slots (%s) to target %.40s, beginning incremental stream...",
                      link->slot_ranges_str,
                      link->nodename);
            slotExportBeginStreaming(link);
        } else {
            serverLog(LL_WARNING,
                      "Failed to snapshot slots (%s) to target %.40s",
                      link->slot_ranges_str,
                      link->nodename);
            finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Failed to perform snapshot");
        }
    }
}

int clusterSlotMigrationShouldInstallWriteHandler(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_EXPORT) {
        return 1;
    }
    return link->state != SLOT_EXPORT_SNAPSHOTTING;
}

void failAllSlotExports(char *message) {
    listIter li;
    listNode *ln;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li))) {
        slotMigrationLink *link = (slotMigrationLink *)ln->value;
        if (link->type != SLOT_MIGRATION_EXPORT) continue;
        if (!isSlotMigrationLinkInProgress(link)) continue;
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, message);
    }
}

void clusterFeedSlotExportLinks(int dbid, robj **argv, int argc, int slot) {
    UNUSED(dbid);

    if (slot == -1) {
        /* We can safely ignore any commands with no keys. This includes
         * MULTI/EXEC. This isn't a problem since the entire slot migration is
         * atomically visible and therefore transactions are redundant. */
        return;
    }

    listIter li;
    listNode *ln;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li))) {
        slotMigrationLink *link = (slotMigrationLink *)ln->value;
        if (link->type != SLOT_MIGRATION_EXPORT) continue;
        if (!link->client) continue;
        if (!isSlotMigrationLinkInProgress(link) || link->state < SLOT_EXPORT_SNAPSHOTTING) continue;
        if (!isSlotInSlotRanges(slot, link->slot_ranges)) continue;

        addReplyArrayLen(link->client, argc);
        for (int i = 0; i < argc; i++) {
            addReplyBulk(link->client, argv[i]);
        }
    }
}

int isSlotExportPauseTimedOut(slotMigrationLink *link) {
    return link->mf_end < mstime() || !getPausedActionsWithPurpose(PAUSE_DURING_SLOT_MIGRATION);
}

void updateSlotExportIfOwnershipChanged(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_EXPORT);
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    int cross_node = 0;
    clusterNode *n = getClusterNodeBySlotRanges(link->slot_ranges, &cross_node);
    if (n) {
        if (n == server.cluster->myself) {
            return;
        } else if (!memcmp(n->name, link->nodename, CLUSTER_NAMELEN)) {
            /* All slots are now claimed by the target of this slot migration link */
            serverLog(LL_NOTICE,
                      "Slot export link %.40s completed export successfully. "
                      "Slots (%s) are now owned by %.40s",
                      link->linkname,
                      link->slot_ranges_str,
                      link->nodename);
            finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_SUCCESS, "");
            return;
        }
    }

    serverLog(LL_WARNING,
              "Slot export link %.40s (for slots [%s]) to node %.40s are no longer all owned by "
              "myself. Failing slot export...",
              link->linkname,
              link->slot_ranges_str,
              link->nodename);

    finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                            "Slots are no longer owned by myself");
    return;
}

/* Called within topology updates to update any slot exports immediately
 * when the ownership changes. Will unpause if all paused slot migration links
 * are now done. */
void clusterUpdateSlotExportsOnOwnershipChange(void) {
    listNode *ln;
    listIter li;
    int paused = 0;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li))) {
        slotMigrationLink *link = (slotMigrationLink *)ln->value;
        if (link->type != SLOT_MIGRATION_EXPORT) continue;
        updateSlotExportIfOwnershipChanged(link);
        if (link->mf_end) {
            paused++;
        }
    }
    if (!paused && getPausedActionsWithPurpose(PAUSE_DURING_SLOT_MIGRATION)) {
        unpauseActions(PAUSE_DURING_SLOT_MIGRATION);
    }
}

int clusterIsAnySlotExporting(void) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationLink *link = ln->value;
        if (link->type != SLOT_MIGRATION_EXPORT) continue;
        if (isSlotMigrationLinkInProgress(link)) {
            return 1;
        }
    }
    return 0;
}

slotMigrationLink *
createSlotExportLink(clusterNode *target_node, list *slot_ranges) {
    slotMigrationLink *link = zcalloc(sizeof(slotMigrationLink));

    link->ctime = server.unixtime;
    link->last_update = link->ctime;
    link->last_ack = link->ctime;
    link->type = SLOT_MIGRATION_EXPORT;
    link->state = SLOT_EXPORT_CONNECTING;
    link->slot_ranges = slot_ranges;
    link->slot_ranges_str = representSlotRangeList(slot_ranges);
    getRandomHexChars(link->linkname, sizeof(link->linkname));
    memcpy(link->nodename, target_node->name, CLUSTER_NAMELEN);
    return link;
}

void slotMigrationLinkReadEstablishResponse(connection *conn) {
    slotMigrationLink *link = (slotMigrationLink *)connGetPrivateData(conn);
    if (!link->read_buf) {
        link->read_buf = sdsempty();
        link->read_buf = sdsMakeRoomForNonGreedy(link->read_buf, PROTO_IOBUF_LEN);
    }

    int result;
    result = connRead(link->conn, ((char *)link->read_buf) + sdslen(link->read_buf), sdsavail(link->read_buf));
    if (result > 0) {
        sdsIncrLen(link->read_buf, result);
    }
    if (link->conn->state != CONN_STATE_CONNECTED) {
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Connection failed while reading establish link response");
        return;
    }
    if (sdslen(link->read_buf) >= sdsalloc(link->read_buf)) {
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Response to establish link is larger than buffer limit");
        return;
    }
    if (sdslen(link->read_buf) < 2 ||
        link->read_buf[sdslen(link->read_buf) - 2] != '\r' ||
        link->read_buf[sdslen(link->read_buf) - 1] != '\n') {
        connSetReadHandler(link->conn, slotMigrationLinkReadEstablishResponse);
        return;
    }
    if (link->read_buf[0] == '-') {
        sds err_msg = sdscatfmt(sdsempty(), "Received error during handshake to target: %S", link->read_buf);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, err_msg);
        sdsfree(err_msg);
        return;
    }

    updateSlotMigrationLinkState(link, SLOT_EXPORT_WAITING_TO_SNAPSHOT);
    initSlotExportLinkClient(link);
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
    sdsfree(link->read_buf);
    link->read_buf = NULL;
}

void slotMigrationLinkWriteEstablishCommand(connection *conn) {
    slotMigrationLink *link = (slotMigrationLink *)connGetPrivateData(conn);
    serverAssert(link->write_buf);
    size_t write_len = sdslen(link->write_buf);
    int result = connWrite(link->conn, link->write_buf, write_len);
    if (result < (ssize_t)write_len) {
        if (connGetState(link->conn) != CONN_STATE_CONNECTED) {
            finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Connection failed while sending establish link command");
            return;
        }
        sdsrange(link->write_buf, result, -1);
        connSetWriteHandler(link->conn, slotMigrationLinkWriteEstablishCommand);
        return;
    }
    updateSlotMigrationLinkState(link, SLOT_EXPORT_READ_ESTABLISH_LINK_RESPONSE);
    connSetWriteHandler(link->conn, NULL);
    sdsfree(link->write_buf);
    link->write_buf = NULL;

    slotMigrationLinkReadEstablishResponse(link->conn);
}

/* -------------------------------------- TARGET & SOURCE -------------------------------------- */

void updateSlotMigrationLinkStatusMessage(slotMigrationLink *link, char *message) {
    if (link->status_msg) {
        sdsfree(link->status_msg);
    }
    link->status_msg = sdsnew(message);
}

/* proceedWithSlotMigration contains the main logic for driving the slot migration state machine.
 *
 *                                   │ CLUSTER
 *                                   │ IMPORT
 *                                   ▼
 * ┌─────────────────────┐  ┌──────────────────────┐
 * │SLOT_IMPORT_RECONNECT├─►│SLOT_IMPORT_CONNECTING│
 * └─────────────────────┘  └────────┬─────────────┘
 *           ▲                       │ Connected
 *           │                       ▼
 *           .          ┌──────────────────────────┐
 * Disconnect|          │SLOT_IMPORT_AUTHENTICATING│
 *           .          └────────────┬─────────────┘
 *                                   │ AUTH success
 *                                   ▼
 *                      ┌──────────────────────────┐                    ┌───────────────────────────────┐
 *                      │SLOT_IMPORT_START_SNAPSHOT├─────SNAPSHOT──────►│SLOT_EXPORT_WAITING_TO_SNAPSHOT│
 *                      └────────────┬─────────────┘                    └──────────┬────────────────────┘
 *                                   │                                             │  Child process
 *                                   ▼                                             ▼  available
 *                    ┌────────────────────────────┐                    ┌────────────────────────┐
 *                    │                            │◄──<AOF snapshot>───┤                        │
 *            ┌───────┼SLOT_IMPORT_RECEIVE_SNAPSHOT│                    │SLOT_EXPORT_SNAPSHOTTING│
 *            │       │                            │◄───SNAPSHOT-EOF────┤                        │
 *            │       └──────────────┬─────────────┘                    └──────────┬─────────────┘
 *            │  On           On EOF ▼ (two phase)                       On STREAM ▼ (or child done)
 *            │  EOF    ┌──────────────────────────┐                    ┌─────────────────────┐
 *            │ (one    │                          ├──────STREAM───────►│                     │
 *            │  shot)  │SLOT_IMPORT_RECEIVE_STREAM│                    │SLOT_EXPORT_STREAMING│◄───────────┐
 *      ┌─────┼────────►│                          │◄───<cmd stream>────│                     │       If   │
 *      │     │         └────────────┬─────────────┘                    └──────────┬──────────┘       not  │
 *      │     ▼                  On  ▼ IMPORT-COMMIT                     On PAUSE  ▼                paused │
 *      │  ┌───────────────────────────────────────┐                    ┌───────────────────────────┐      │
 *      │  │                                       ├───────PAUSE───────►│                           │      │
 *      │  │SLOT_IMPORT_WAITING_FOR_PAUSED         │                    │SLOT_EXPORT_FAILOVER_PAUSED├──────┘
 *      │  │                                       │◄─────PAUSED────────┤                           │
 *      │  └─────────────────────────┬─────────────┘                    └──────────┬────────────────┘
 *      │                            ▼ On PAUSED                                   ▼  If still paused
 *      │(two phase)┌──────────────────────────────┐                    ┌────────────────────────────┐
 *      └───────────┤                              ├─REQUEST-FAILOVER──►│                            │
 *       If denied  │SLOT_IMPORT_FAILOVER_REQUESTED│                    │SLOT_EXPORT_FAILOVER_GRANTED│
 *      ┌───────────┤                              │◄─FAILOVER-GRANTED/─│                            │
 *      │(one shot) └────────────────┬─────────────┘  FAILOVER-DENIED   └──────────┬─────────────────┘
 *      │                            ▼ If granted                                  ▼  On slots transferred
 *      │             ┌────────────────────────────┐  Cluster bus       ┌───────────────────────────┐
 *      │             │SLOT_IMPORT_FAILOVER_GRANTED├──broadcast────────►│SLOT_MIGRATION_LINK_SUCCESS│
 *      │             └──────────────┬─────────────┘                    └───────────────────────────┘
 *      │                            ▼
 *      │              ┌───────────────────────────┐
 *      │              │SLOT_MIGRATION_LINK_SUCCESS│
 *      │              └───────────────────────────┘
 *      │
 *      │               ┌──────────────────────────┐
 *      └──────────────►│SLOT_MIGRATION_LINK_FAILED│
 *                      └──────────────────────────┘
 */
void proceedWithSlotMigration(slotMigrationLink *link) {
    /* Continue within the state machine until we have no more work. */
    while (1) {
        switch (link->state) {
        /* Importing states */
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
                performSlotImportLinkFailover(link);
            }
            return;
        case SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP:
            delKeysNotOwnedByMyself(link->slot_ranges);
            updateSlotMigrationLinkState(link, link->post_cleanup_state);
            return;

        /* Exporting states */
        case SLOT_EXPORT_CONNECTING:
            if (!link->conn) {
                connectSlotExportLink(link);
            }
            if (proceedWithSlotExportLinkConnecting(link)) {
                continue;
            }
            return;
        case SLOT_EXPORT_AUTHENTICATING:
            performSlotExportLinkAuthentication(link);
            continue;
        case SLOT_EXPORT_ESTABLISH_LINK:
            if (!link->write_buf) {
                link->write_buf = generateEstablishLinkCommand(link);
                /* slotMigrationLinkWriteEstablishCommand will attempt to write
                 * out the command and proceed to reading the response when
                 * ready. */
                slotMigrationLinkWriteEstablishCommand(link->conn);
                return;
            }
            /* We are still writing out the command, nothing to do in cron */
            return;
        case SLOT_EXPORT_READ_ESTABLISH_LINK_RESPONSE:
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
            if (hasActiveChildProcess() || link->client->flag.pending_write) {
                return;
            }
            slotExportLinkBeginSnapshot(link);
            return;
        case SLOT_EXPORT_SNAPSHOTTING:
            /* Waiting for child process to finish */
            return;
        case SLOT_EXPORT_STREAMING:
            /* Waiting for PAUSE command to come in */
            return;
        case SLOT_EXPORT_WAITING_TO_PAUSE:
            slotExportTryDoPause(link);
            return;
        case SLOT_EXPORT_FAILOVER_PAUSED:
            if (isSlotExportPauseTimedOut(link)) {
                serverLog(LL_WARNING,
                          "Slot export link %.40s timed out during requested slot "
                          "failover from %.40s for slot ranges (%s).",
                          link->linkname,
                          link->nodename,
                          link->slot_ranges_str);
                link->mf_end = 0;
                updatePausedActions();
                finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Timed out before streaming completed");
            }
            return;
        case SLOT_EXPORT_FAILOVER_GRANTED:
            if (isSlotExportPauseTimedOut(link)) {
                /* Note that we think this won't happen very commonly. The main source of
                 * latency that may trigger an unpause will occur due to the time it takes
                 * to process the incremental changes. The final REQUEST-FAILOVER handshake
                 * will validate that the source node is still paused after this initial
                 * handshake, and renew the pause for an additional amount of time. From this
                 * point, we expect the takeover of the slot and gossip to be relatively quick
                 * in steady state.
                 *
                 * Regardless, we log a warning and proceed with cleaning up the slot migration
                 * link. */
                serverLog(LL_WARNING, "Write loss risk! During slot export, new owner did not "
                                      "broadcast ownership before we unpaused ourselves. Any "
                                      "writes we have recorded since unpausing will now be lost!");

                finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                                        "Unpaused before migration completed (migration may have "
                                        "succeeded with lost writes)");
                return;
            }
            /* updateSlotExportIfOwnershipChanged will close the slot migration link if it is
             * done. */
            updateSlotExportIfOwnershipChanged(link);
            return;

        /* Terminal states */
        case SLOT_MIGRATION_LINK_SUCCESS:
        case SLOT_MIGRATION_LINK_CANCELLED:
        case SLOT_MIGRATION_LINK_FAILED:
            return;
        }
    }
}

void resetSlotMigrationLink(slotMigrationLink *link) {
    /* Only one of client or conn should be set. */
    serverAssert(!link->client || !link->conn);
    if (link->client) {
        freeClientAsync(link->client);
        link->client = NULL;
    } else if (link->conn) {
        connClose(link->conn);
        link->conn = NULL;
    }

    if (link->write_buf) {
        sdsfree(link->write_buf);
        link->write_buf = NULL;
    }
    if (link->read_buf) {
        sdsfree(link->read_buf);
        link->read_buf = NULL;
    }
}

void freeSlotMigrationLink(void *o) {
    slotMigrationLink *link = o;
    resetSlotMigrationLink(link);
    listRelease(link->slot_ranges);
    sdsfree(link->slot_ranges_str);
    if (link->status_msg) {
        sdsfree(link->status_msg);
    }
    if (link->write_buf) {
        sdsfree(link->write_buf);
    }
    if (link->read_buf) {
        sdsfree(link->read_buf);
    }
    zfree(o);
}

void initClusterSlotMigrationLinkList(void) {
    server.cluster->slot_migration_links = listCreate();
    listSetFreeMethod(server.cluster->slot_migration_links, freeSlotMigrationLink);
}

int shouldCleanupSlotMigrationLink(slotMigrationLink *link) {
    return (link->state == SLOT_MIGRATION_LINK_CANCELLED ||
            link->state == SLOT_MIGRATION_LINK_FAILED ||
            link->state == SLOT_MIGRATION_LINK_SUCCESS);
}

const char *slotMigrationLinkStateToString(slotMigrationLinkState state) {
    switch (state) {
    case SLOT_IMPORT_RECEIVE_SNAPSHOT: return "snapshotting";
    case SLOT_IMPORT_WAITING_FOR_PAUSED: return "waiting-for-paused";
    case SLOT_IMPORT_FAILOVER_REQUESTED: return "failover-requested";
    case SLOT_IMPORT_FAILOVER_GRANTED: return "failover-granted";
    case SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP: return "cleaning-up";

    case SLOT_EXPORT_CONNECTING: return "connecting";
    case SLOT_EXPORT_AUTHENTICATING: return "authenticating";
    case SLOT_EXPORT_ESTABLISH_LINK: return "sending-establish-command";
    case SLOT_EXPORT_READ_ESTABLISH_LINK_RESPONSE: return "reading-establish-response";
    case SLOT_EXPORT_WAITING_TO_SNAPSHOT: return "waiting-to-snapshot";
    case SLOT_EXPORT_SNAPSHOTTING: return "snapshotting";
    case SLOT_EXPORT_STREAMING: return "replicating";
    case SLOT_EXPORT_WAITING_TO_PAUSE: return "waiting-to-pause";
    case SLOT_EXPORT_FAILOVER_PAUSED: return "failover-paused";
    case SLOT_EXPORT_FAILOVER_GRANTED: return "failover-granted";

    case SLOT_MIGRATION_LINK_SUCCESS: return "success";
    case SLOT_MIGRATION_LINK_CANCELLED: return "cancelled";
    case SLOT_MIGRATION_LINK_FAILED: return "failed";
    }
    return "unknown";
}

void updateSlotMigrationLinkState(slotMigrationLink *link, slotMigrationLinkState state) {
    serverLog(LL_DEBUG,
              "Slot %s link %.40s state transition: %s -> %s",
              link->type == SLOT_MIGRATION_IMPORT ? "IMPORT" : "EXPORT",
              link->linkname,
              slotMigrationLinkStateToString(link->state),
              slotMigrationLinkStateToString(state));
    link->last_update = server.unixtime;
    link->state = state;
}

void clusterHandleSlotMigrationLinkClientClose(void *o) {
    slotMigrationLink *link = (slotMigrationLink *)o;
    link->client = NULL;
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    serverLog(LL_NOTICE,
              "Slot migration connection to node %.40s (for slots [%s]) lost.",
              link->nodename,
              link->slot_ranges_str);

    /* If we have granted failover, the failover may have happened, but we don't know. We keep the
     * slot export around so that we remain paused until we find out about the takeover (or until
     * the pause times out).
     *
     * Otherwise, we can mark it failed. */
    if (link->state != SLOT_EXPORT_FAILOVER_GRANTED) {
        if (link->type == SLOT_MIGRATION_EXPORT) {
            finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Connection lost to target. Check CLUSTER MIGRATIONS on the target node for more information.");
        } else {
            finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Connection lost to source. Check CLUSTER MIGRATIONS on the source node for more information.");
        }
    }
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
}

void clusterHandleSlotMigrationLinkClientOOM(void *o) {
    slotMigrationLink *link = (slotMigrationLink *)o;
    if (link->type != SLOT_MIGRATION_IMPORT) return;
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    serverLog(LL_WARNING,
              "Slot import link %.40s to node %.40s (owner of slots [%s]) "
              "failed due due to OOM",
              link->linkname,
              link->nodename,
              link->slot_ranges_str);
    finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                            "Ran out of memory (OOM) during slot import");
}


void finishSlotMigrationLink(slotMigrationLink *link, slotMigrationLinkState state, char *message) {
    serverLog(LL_NOTICE,
              "Slot %s link %.40s (for slots [%s]) finished. State: %s, Message: %s",
              link->type == SLOT_MIGRATION_IMPORT ? "import" : "export",
              link->linkname,
              link->slot_ranges_str,
              slotMigrationLinkStateToString(state), message ? message : "");
    updateSlotMigrationLinkStatusMessage(link, message);

    if (link->type == SLOT_MIGRATION_EXPORT) {
        /* If we finish the export, we should not remain paused */
        link->mf_end = 0;
    }
    if (link->type == SLOT_MIGRATION_IMPORT) {
        /* Defer cleanup until beforeSleep. */
        link->post_cleanup_state = state;
        state = SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP;
        clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
    }
    updateSlotMigrationLinkState(link, state);
    resetSlotMigrationLink(link);
}

/* In progress may be different than terminated, if we still need to track some cleanup work. */
int isSlotMigrationLinkInProgress(slotMigrationLink *link) {
    return link->state != SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP &&
           link->state != SLOT_MIGRATION_LINK_SUCCESS &&
           link->state != SLOT_MIGRATION_LINK_CANCELLED &&
           link->state != SLOT_MIGRATION_LINK_FAILED;
}

int isImportSlotMigrationLink(void *o) {
    slotMigrationLink *link = (slotMigrationLink *)o;
    return link->type == SLOT_MIGRATION_IMPORT;
}

/* Synthesizes a view of ongoing and recently completed imports for an operator. */
void clusterCommandMigrations(client *c) {
    listNode *ln;
    listIter li;
    addReplyArrayLen(c, listLength(server.cluster->slot_migration_links));
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationLink *link = ln->value;
        addReplyMapLen(c, 9);
        addReplyBulkCString(c, "link_name");
        addReplyBulkCBuffer(c, link->linkname, CLUSTER_NAMELEN);
        addReplyBulkCString(c, "operation");
        addReplyBulkCString(c, link->type == SLOT_MIGRATION_IMPORT ? "IMPORT" : "EXPORT");
        addReplyBulkCString(c, "slot_ranges");
        addReplyBulkCString(c, link->slot_ranges_str);
        addReplyBulkCString(c, "node");
        addReplyBulkCBuffer(c, link->nodename, CLUSTER_NAMELEN);
        addReplyBulkCString(c, "create_time");
        addReplyLongLong(c, link->ctime);
        addReplyBulkCString(c, "last_update_time");
        addReplyLongLong(c, link->last_update);
        addReplyBulkCString(c, "last_ack_time");
        addReplyLongLong(c, link->last_ack);
        addReplyBulkCString(c, "state");
        addReplyBulkCString(c, slotMigrationLinkStateToString(link->state));
        addReplyBulkCString(c, "message");
        addReplyBulkCString(c, link->status_msg ? link->status_msg : "");
    }
}

void sendSyncSlotsMessageOnClient(client *c, const char *subcommand) {
    addReplyArrayLen(c, 3);
    addReplyBulkCString(c, "CLUSTER");
    addReplyBulkCString(c, "SYNCSLOTS");
    addReplyBulkCString(c, subcommand);
}

void sendSyncSlotsMessageOnLink(slotMigrationLink *link, const char *subcommand) {
    serverAssert(link->client);
    if (link->type == SLOT_MIGRATION_EXPORT) {
        sendSyncSlotsMessageOnClient(link->client, subcommand);
        return;
    }
    ClientFlags old_flags = link->client->flag;
    link->client->flag.pushing = 1;
    sendSyncSlotsMessageOnClient(link->client, subcommand);
    if (!old_flags.pushing) link->client->flag.pushing = 0;
}

void clusterCleanupSlotMigrationLog(void) {
    listNode *ln;
    listIter li;
    listRewindTail(server.cluster->slot_migration_links, &li);
    while (server.cluster->slot_migration_links->len > server.cluster_slot_migration_log_max_len &&
           (ln = listNext(&li)) != NULL) {
        slotMigrationLink *link = ln->value;
        if (shouldCleanupSlotMigrationLink(link)) {
            listDelNode(server.cluster->slot_migration_links, ln);
        }
    }
}

int canSlotMigrationSendAck(slotMigrationLink *link) {
    /* 1. We cannot send ACK from parent process while child is snapshotting
     * 2. We don't send an ACK from the import side until the export has first
     *    sent one. This simplifies parsing of the response to CLUSTER SYNCSLOTS
     *    ESTABLISH. */
    return link->state != SLOT_EXPORT_SNAPSHOTTING &&
           (link->type != SLOT_MIGRATION_IMPORT || link->last_ack != link->ctime);
}

void clusterSlotMigrationCron(void) {
    slotMigrationLink *link;
    listNode *ln;
    listIter li;
    int paused = 0;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        link = ln->value;

        if (link->client) {
            time_t last_interaction = link->client->last_interaction;
            if (link->type == SLOT_MIGRATION_EXPORT) {
                /* For export, we just use the last ack received time */
                last_interaction = link->last_ack;
            }

            if (isSlotMigrationLinkInProgress(link)) {
                /* Only enforce the ACK timeout when not in failover granted
                 * state. Instead, rely on the pause timeout in such cases. */
                if (link->state != SLOT_EXPORT_FAILOVER_GRANTED &&
                    last_interaction &&
                    (server.unixtime - last_interaction > server.repl_timeout)) {
                    serverLog(LL_WARNING,
                              "Timing out slot link %.40s to node %.40s for slots (%s) "
                              "after not receiving ack for too long",
                              link->linkname,
                              link->nodename,
                              link->slot_ranges_str);
                    finishSlotMigrationLink(link,
                                            SLOT_MIGRATION_LINK_FAILED,
                                            "Timed out after too long with no interaction");
                    continue;
                }
                /* Send acks only when the child process isn't writing to it. */
                if (canSlotMigrationSendAck(link)) {
                    run_with_period(1000) sendSyncSlotsMessageOnLink(link, "ACK");
                }
            }
        }
        proceedWithSlotMigration(link);
        if (link->mf_end) {
            paused++;
        }
    }

    clusterCleanupSlotMigrationLog();

    /* If no exports are paused, we can unpause */
    if (!paused && getPausedActionsWithPurpose(PAUSE_DURING_SLOT_MIGRATION)) {
        unpauseActions(PAUSE_DURING_SLOT_MIGRATION);
    }
}

/* Sent by either the target or the source as a liveness check. */
void clusterCommandSyncSlotsAck(client *c) {
    if (!c->slot_migration_link) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS ACK from client %llu, "
                  "but the client is not a slot import source or export target. "
                  "Closing link.",
                  (unsigned long long)c->id);
        freeClientAsync(c);
        return;
    }
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    link->last_ack = server.unixtime;
}

/* Sent by either the target or the source as a control message for progressing
 * with slot import. */
void clusterCommandSyncSlots(client *c) {
    if (c->flag.primary) {
        /* Due to primary proxying slot migration source commands to replicas, SYNCSLOTS should be
         * ignored from our primary. */
        return;
    }
    if (!strcasecmp(c->argv[2]->ptr, "establish")) {
        /* CLUSTER SYNCSLOTS ESTABLISH <args> */
        clusterCommandSyncSlotsEstablish(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "snapshot-eof")) {
        /* CLUSTER SYNCSLOTS SNAPSHOT-EOF */
        clusterCommandSyncSlotsSnapshotEof(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "pause")) {
        /* CLUSTER SYNCSLOTS PAUSE */
        clusterCommandSyncSlotsPause(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "paused")) {
        /* CLUSTER SYNCSLOTS PAUSED */
        clusterCommandSyncSlotsPaused(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "request-failover")) {
        /* CLUSTER SYNCSLOTS REQUEST-FAILOVER */
        clusterCommandSyncSlotsRequestFailover(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "failover-granted")) {
        /* CLUSTER SYNCSLOTS FAILOVER-GRANTED */
        clusterCommandSyncSlotsFailoverGranted(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "failover-denied")) {
        /* CLUSTER SYNCSLOTS FAILOVER-DENIED */
        clusterCommandSyncSlotsFailoverDenied(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "ack")) {
        /* CLUSTER SYNCSLOTS ACK */
        clusterCommandSyncSlotsAck(c);
    } else {
        /* Ignore unknown SYNCSLOTS commands to simplify forwards compatibility */
        serverLog(LL_NOTICE, "Got unknown SYNCSLOTS subcommand, ignoring...");
    }
}
