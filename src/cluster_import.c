/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include "cluster_import.h"
#include "bio.h"
#include "module.h"
#include "functions.h"

static int isSlotMigrationLinkInProgress(slotMigrationLink *link);
static slotMigrationLink *createSlotImportLink(clusterNode *source_node, list *slot_ranges, int one_shot);
static void connectSlotImportLink(slotMigrationLink *link);
static const char *slotMigrationLinkStateToString(slotMigrationLinkState state);
static void updateSlotMigrationLinkState(slotMigrationLink *link, slotMigrationLinkState state);
static void sendSyncSlotsMessageOnClient(client *c, const char *subcommand);
static void sendSyncSlotsMessageOnLink(slotMigrationLink *link, const char *subcommand);
static void proceedWithSlotMigration(slotMigrationLink *link);
static slotMigrationLink *createSlotExportLink(client *c, char *nodename, char *linkname, list *slot_ranges);
static int isSlotExportPauseTimedOut(slotMigrationLink *link);
static void resetSlotMigrationLink(slotMigrationLink *link);
static void finishSlotMigrationLink(slotMigrationLink *link, slotMigrationLinkState state, char *message);
static void updateSlotMigrationLinkStatusMessage(slotMigrationLink *link, char *message);


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
                delKeysInSlot(i, /*lazy=*/1, /*propagate_del=*/1, /*send_del_event=*/0);
            }
        }
    }
}

int isAnySlotInManualImportingState(void) {
    for (int i = 0; i < CLUSTER_SLOTS; i++) {
        if (server.cluster->importing_slots_from[i] != NULL) {
            return 1;
        }
    }
    return 0;
}

int isAnySlotInManualMigratingState(void) {
    for (int i = 0; i < CLUSTER_SLOTS; i++) {
        if (server.cluster->migrating_slots_to[i] != NULL) {
            return 1;
        }
    }
    return 0;
}

/* Parse as many slot ranges starting from start_index, returning a list of parsed slot ranges, or
 * NULL if there is an error. end_index_out can be used to proceed with argument parsing from the
 * last parsed slot range.*/
list *parseSlotRanges(client *c, int start_index, clusterNode **node_out, sds *err_out) {
    list *slot_ranges = createSlotRangeList();
    int startslot, endslot;
    *node_out = NULL;
    for (int i = start_index; i < c->argc; i += 2) {
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
                *err_out = sdsnew("The slot ranges can not cross nodes, "
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

/* Sent by an operator to the target (which must be a primary), the target will
 * attempt to import and failover the provided slot range from the current owner. */
void clusterCommandImport(client *c, int one_shot) {
    if (!nodeIsPrimary(server.cluster->myself)) {
        addReplyError(c, "Import can only be used on primary nodes.");
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

    if (strcasecmp(c->argv[2]->ptr, "slotsrange")) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    clusterNode *source_node = NULL;

    sds err = NULL;
    list *slot_ranges = parseSlotRanges(c, 3, &source_node, &err);
    if (err != NULL) {
        addReplyErrorSds(c, err);
        return;
    }
    if (source_node == server.cluster->myself) {
        addReplyErrorFormat(c, "Slots are already served by myself.");
        listRelease(slot_ranges);
        return;
    }
    listIter li;
    listNode *ln;
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li))) {
        slotRange *range = (slotRange *)ln->value;
        for (int j = range->start_slot; j <= range->end_slot; j++) {
            if (clusterIsSlotImporting(j)) {
                addReplyErrorFormat(c, "I am already importing slot %d.", j);
                listRelease(slot_ranges);
                return;
            }
        }
    }

    slotMigrationLink *link = createSlotImportLink(source_node, slot_ranges, one_shot);
    listAddNodeHead(server.cluster->slot_migration_links, link);
    proceedWithSlotMigration(link);
    addReply(c, shared.ok);
}

slotMigrationLink *clusterLookupImportLink(sds linkname) {
    listNode *ln;
    listIter li;
    if (sdslen(linkname) != CLUSTER_NAMELEN) return NULL;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationLink *link = ln->value;
        if (link->type != SLOT_MIGRATION_IMPORT) continue;
        if (!memcmp(linkname, link->linkname, CLUSTER_NAMELEN)) return link;
    }
    return NULL;
}

void clusterCommandImportCommit(client *c) {
    if (strcasecmp(c->argv[2]->ptr, "link")) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    slotMigrationLink *link = clusterLookupImportLink(c->argv[3]->ptr);
    if (!link || !isSlotMigrationLinkInProgress(link)) {
        addReplyErrorFormat(c, "Link name not found");
        return;
    }
    if (link->one_shot) {
        addReplyErrorFormat(c, "Link is executing in one-shot mode");
        return;
    }
    if (link->state != SLOT_IMPORT_RECEIVE_STREAM) {
        addReplyErrorFormat(c, "Link is not ready to be committed");
        return;
    }

    /* Start the failover procedure */
    sendSyncSlotsMessageOnLink(link, "PAUSE");
    updateSlotMigrationLinkState(link, SLOT_IMPORT_FAILOVER_WAITING_FOR_PAUSED);

    /* If there was a status from a previous attempt, clear it as this is a fresh attempt. */
    if (link->status_msg) {
        sdsfree(link->status_msg);
        link->status_msg = NULL;
    }
    addReply(c, shared.ok);
}

/* Cancels one or all ongoing imports. */
void clusterCommandImportCancel(client *c) {
    listNode *ln;
    listIter li;

    if (!strcasecmp(c->argv[2]->ptr, "all")) {
        if (!clusterIsAnySlotImporting()) {
            addReplyError(c, "No imports ongoing");
            return;
        }

        listRewind(server.cluster->slot_migration_links, &li);
        while ((ln = listNext(&li)) != NULL) {
            slotMigrationLink *link = ln->value;
            if (!isSlotMigrationLinkInProgress(link)) {
                continue;
            }
            if (link->client) {
                sendSyncSlotsMessageOnLink(link, "CANCEL");
                link->client->flag.close_after_reply = 1;
            }
            finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_CANCELLED, NULL);
        }
        serverLog(LL_NOTICE, "Canceled all in progress slot imports due to operator's request.");
        addReply(c, shared.ok);
        return;
    } else if (!strcasecmp(c->argv[2]->ptr, "link") && c->argc > 3) {
        slotMigrationLink *link = clusterLookupImportLink(c->argv[3]->ptr);
        if (!link || !isSlotMigrationLinkInProgress(link)) {
            addReplyErrorFormat(c, "Link name not found.");
            return;
        }
        if (link->client) {
            sendSyncSlotsMessageOnLink(link, "CANCEL");
            link->client->flag.close_after_reply = 1;
        }
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_CANCELLED, NULL);
        serverLog(LL_NOTICE,
                  "Slot import link %.40s to node %.40s (owner of slots [%s]) "
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

/* Sent by the source to the target after dumping the snapshot in
 * AOF format. */
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
    if (link->one_shot) {
        sendSyncSlotsMessageOnLink(link, "PAUSE");
        updateSlotMigrationLinkState(link, SLOT_IMPORT_FAILOVER_WAITING_FOR_PAUSED);
    } else {
        sendSyncSlotsMessageOnLink(link, "STREAM");
        updateSlotMigrationLinkState(link, SLOT_IMPORT_RECEIVE_STREAM);
    }
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
    if (link->state != SLOT_IMPORT_FAILOVER_WAITING_FOR_PAUSED) {
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
    if (link->one_shot) {
        serverLog(LL_WARNING,
                  "Slot import link %.40s had failover denied from node %.40s "
                  "(owner of slots [%s]). Failing import request.",
                  link->linkname,
                  link->nodename,
                  link->slot_ranges_str);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Failover denied");
        return;
    }
    serverLog(LL_WARNING,
              "Slot import link %.40s had failover denied from node %.40s "
              "(owner of slots [%s]). Going back to streaming incremental updates.",
              link->linkname,
              link->nodename,
              link->slot_ranges_str);
    updateSlotMigrationLinkStatusMessage(link, "Previous failover attempt denied from source");
    updateSlotMigrationLinkState(link, SLOT_IMPORT_RECEIVE_STREAM);
}

/* Sent by the source to the target to notify of some unrecoverable error. */
void clusterCommandSyncSlotsFail(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_IMPORT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS FAIL from client %llu, "
                  "but the client is not a slot import source. Closing link.",
                  (unsigned long long)c->id);
        freeClientAsync(c);
        return;
    }
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Source node terminated import");
}

slotMigrationLink *createSlotImportLink(clusterNode *source_node, list *slot_ranges, int one_shot) {
    slotMigrationLink *link = zcalloc(sizeof(slotMigrationLink));
    link->ctime = server.unixtime;
    link->last_update = link->ctime;
    link->last_ack = link->ctime;
    link->type = SLOT_MIGRATION_IMPORT;
    link->slot_ranges = slot_ranges;
    link->slot_ranges_str = representSlotRangeList(slot_ranges);
    link->one_shot = one_shot;
    link->state = SLOT_IMPORT_CONNECTING;
    getRandomHexChars(link->linkname, sizeof(link->linkname));
    memcpy(link->nodename, source_node->name, CLUSTER_NAMELEN);
    serverLog(LL_NOTICE,
              "New slot import link created: link name %.40s, "
              "source node %.40s, slot ranges %s.",
              link->linkname,
              link->nodename,
              link->slot_ranges_str);
    return link;
}

void slotImportConnectHandler(connection *conn) {
    slotMigrationLink *link = (slotMigrationLink *)connGetPrivateData(conn);
    proceedWithSlotMigration(link);
}

void connectSlotImportLink(slotMigrationLink *link) {
    sds status_msg;
    clusterNode *n = clusterLookupNode(link->nodename, CLUSTER_NAMELEN);
    int port = getNodeDefaultClientPort(n);
    serverLog(LL_NOTICE,
              "Connecting slot import link %.40s to %.40s "
              "(owner of slots [%s]) at (ip: %s, port %d)",
              link->linkname,
              link->nodename,
              link->slot_ranges_str,
              n->ip,
              port);

    link->conn = connCreate(connTypeOfReplication());
    if (connConnect(link->conn, n->ip, port, server.bind_source_addr,
        /*multipath=*/ 0, slotImportConnectHandler) == C_ERR) {
        serverLog(LL_WARNING,
                  "Failed to connect slot import link %.40s to %.40s: %s",
                  link->linkname,
                  link->nodename,
                  connGetLastError(link->conn));
        status_msg = sdscatfmt(sdsempty(), "Unable to connect to source node: %s",
                               connGetLastError(link->conn));
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, status_msg);
        sdsfree(status_msg);
        return;
    }

    /* Store the slot migration link in connection private data, until we have a
     * client to store it in. */
    connSetPrivateData(link->conn, link);
}

void clusterHandleSlotImportLinkClientClose(slotMigrationLink *link) {
    link->client = NULL;
    if (!isSlotMigrationLinkInProgress(link)) return;
    if (link->state == SLOT_IMPORT_CONNECTING) return;
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
    serverLog(LL_WARNING,
              "Slot import link %.40s lost connection to node %.40s "
              "(owner of slots [%s]). Reconnecting...",
              link->linkname,
              link->nodename,
              link->slot_ranges_str);

    /* Perform the reconnect. Leaving partially imported slots would result in
     * corruption, so we flush those now. */
    resetSlotMigrationLink(link);
    delKeysNotOwnedByMyself(link->slot_ranges);
    updateSlotMigrationLinkState(link, SLOT_IMPORT_CONNECTING);
}

void clusterHandleSlotMigrationLinkClientOOM(void *o) {
    slotMigrationLink *link = (slotMigrationLink *)o;
    if (link->type != SLOT_MIGRATION_IMPORT) return;
    if (isSlotMigrationLinkInProgress(link)) {
        serverLog(LL_WARNING,
                  "Slot import link %.40s to node %.40s (owner of slots [%s]) "
                  "failed due due to OOM",
                  link->linkname,
                  link->nodename,
                  link->slot_ranges_str);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                                "Ran out of memory (OOM) during slot import");
    }
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

/* Determine if the slot migration link is connected, and return 1 if so. Handles state transition
 * in the case of success/failure. Returns if the connection is done. */
int proceedWithSlotImportLinkConnecting(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_IMPORT && link->conn);
    switch (connGetState(link->conn)) {
    case CONN_STATE_CONNECTED:
        serverLog(LL_NOTICE,
                  "Slot import link %.40s connected to node %.40s (owner of slots [%s]).",
                  link->linkname,
                  link->nodename,
                  link->slot_ranges_str);
        updateSlotMigrationLinkState(link, SLOT_IMPORT_AUTHENTICATING);
        return 1;
    case CONN_STATE_CONNECTING: return 0;
    default:
        serverLog(LL_NOTICE,
                  "Failed to connect to slot owner %.40s for import of slots (%s): %s",
                  link->nodename,
                  link->slot_ranges_str,
                  connGetLastError(link->conn));
        sds status_msg = sdscatfmt(sdsempty(), "Unable to connect to source node: %s",
                                   connGetLastError(link->conn));
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, status_msg);
        sdsfree(status_msg);
        return 1;
    }
}

void performSlotImportLinkAuthentication(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_IMPORT);
    sds status_msg;
    if (!server.primary_auth) {
        updateSlotMigrationLinkState(link, SLOT_IMPORT_START_SNAPSHOT);
        return;
    }
    char *err = replicationSendAuth(link->conn);
    if (err) {
        serverLog(LL_NOTICE,
                  "Failed to send AUTH command to node %.40s for import of slots (%s): %s",
                  link->nodename,
                  link->slot_ranges_str,
                  err);
        status_msg = sdscatfmt(sdsempty(), "Failed to send AUTH to source node: %s", err);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, status_msg);
        sdsfree(err);
        sdsfree(status_msg);
        return;
    }
    err = receiveSynchronousResponse(link->conn);
    if (err == NULL) {
        serverLog(LL_WARNING,
                  "Received no response to AUTH command from node %.40s for import of slots (%s)",
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
        status_msg = sdscatfmt(sdsempty(), "Failed to AUTH to source node: %s", err);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, status_msg);
        sdsfree(status_msg);
    } else {
        serverLog(LL_NOTICE,
                  "Successfully authenticated to node %.40s for import of slots (%s)",
                  link->nodename,
                  link->slot_ranges_str);
        updateSlotMigrationLinkState(link, SLOT_IMPORT_START_SNAPSHOT);
    }
    sdsfree(err);
}

void initSlotImportLinkClient(slotMigrationLink *link) {
    serverAssert(link->type == SLOT_MIGRATION_IMPORT);
    link->client = createClient(link->conn);
    link->conn = NULL;
    link->client->flag.authenticated = 1;
    link->client->flag.reply_off = 1;
    link->client->slot_migration_link = link;

    /* Use dedicated querybuf and replication data to proxy
     * replication stream to replicas directly. */
    initClientReplicationData(link->client);
    link->client->querybuf = sdsempty();
}

void performSlotImportLinkSnapshot(slotMigrationLink *link) {
    listIter li;
    listNode *ln;
    client *c = link->client;
    serverAssert(link->type == SLOT_MIGRATION_IMPORT && c);

    ClientFlags old_flags = c->flag;
    c->flag.pushing = 1;
    addReplyArrayLen(c, 8 + listLength(link->slot_ranges) * 2);
    addReplyBulkCString(c, "CLUSTER");
    addReplyBulkCString(c, "SYNCSLOTS");
    addReplyBulkCString(c, "SNAPSHOT");
    addReplyBulkCString(c, "TARGET");
    addReplyBulkCBuffer(c, server.cluster->myself->name, CLUSTER_NAMELEN);
    addReplyBulkCString(c, "LINKNAME");
    addReplyBulkCBuffer(c, link->linkname, CLUSTER_NAMELEN);
    addReplyBulkCString(c, "SLOTSRANGE");
    listRewind(link->slot_ranges, &li);
    while ((ln = listNext(&li))) {
        slotRange *range = (slotRange *)ln->value;
        addReplyBulkLongLong(c, range->start_slot);
        addReplyBulkLongLong(c, range->end_slot);
    }
    if (!old_flags.pushing) c->flag.pushing = 0;

    updateSlotMigrationLinkState(link, SLOT_IMPORT_RECEIVE_SNAPSHOT);
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

void sendFailAndCloseAfterReply(client *c) {
    sendSyncSlotsMessageOnClient(c, "FAIL");
    c->flag.close_after_reply = 1;
}

/* Sent by the target to the source to initiate the AOF formatted snapshot.
 * Note that if there is an error in the request, we send a fail message in
 * order to prevent infinite retry in the case of incompatibility. */
void clusterCommandSyncSlotsSnapshot(client *c) {
    clusterNode *target_node = NULL;
    char *link_name = NULL;
    clusterNode *source_node = NULL;
    sds err = NULL;
    list *slot_ranges = NULL;
    int error = 0;

    if (!nodeIsPrimary(server.cluster->myself)) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS SNAPSHOT from client %llu, "
                  "but I am not a primary. Failing link.",
                  (unsigned long long)c->id);
        sendFailAndCloseAfterReply(c);
        return;
    }

    if (isAnySlotInManualImportingState() || isAnySlotInManualMigratingState()) {
        serverLog(LL_WARNING,
                  "Failing slot import request due to manual slot migration in progress.");
        sendFailAndCloseAfterReply(c);
        return;
    }

    if (c->slot_migration_link) {
        serverLog(
            LL_WARNING,
            "Received CLUSTER SYNCSLOTS SNAPSHOT from client %llu which is already a slot link. "
            "Failing link.",
            (unsigned long long)c->id);
        sendFailAndCloseAfterReply(c);
        return;
    }

    /* Order agnostic, except SLOTSRANGE should come last (to simplify parsing).
     * We skip unknown key/value pairs forwards compatibility. */
    for (int i = 3; i < c->argc; i += 2) {
        if (!strcasecmp(c->argv[i]->ptr, "target")) {
            if (target_node || i + 1 >= c->argc || sdslen(c->argv[i + 1]->ptr) != CLUSTER_NAMELEN) {
                serverLog(LL_WARNING,
                          "Malformatted or missing node ID in CLUSTER SYNCSLOTS SNAPSHOT from "
                          "client %llu. Failing link.",
                          (unsigned long long)c->id);
                error = 1;
                break;
            }
            target_node = clusterLookupNode(c->argv[4]->ptr, CLUSTER_NAMELEN);
            if (!target_node) {
                serverLog(
                    LL_WARNING,
                    "Received CLUSTER SYNCSLOTS SNAPSHOT from client %llu with an unknown node ID. "
                    "Failing link.",
                    (unsigned long long)c->id);
                error = 1;
                break;
            }
        }
        if (!strcasecmp(c->argv[i]->ptr, "linkname")) {
            if (link_name || i + 1 >= c->argc || sdslen(c->argv[i + 1]->ptr) != CLUSTER_NAMELEN) {
                serverLog(
                    LL_WARNING,
                    "Malformatted or missing link name in CLUSTER SYNCSLOTS SNAPSHOT from client "
                    "%llu. Failing link.",
                    (unsigned long long)c->id);
                error = 1;
                break;
            }
            link_name = c->argv[i + 1]->ptr;
        }
        if (!strcasecmp(c->argv[i]->ptr, "slotsrange")) {
            slot_ranges = parseSlotRanges(c, i + 1, &source_node, &err);
            if (err) {
                serverLog(
                    LL_WARNING,
                    "Failed to parse slot range provided by CLUSTER SYNCSLOTS SNAPSHOT from client "
                    "%llu: %s. Failing link.",
                    (unsigned long long)c->id, err);
                sdsfree(err);
                error = 1;
                break;
            }
            if (source_node != server.cluster->myself) {
                serverLog(LL_WARNING,
                          "Received CLUSTER SYNCSLOTS SNAPSHOT from client %llu, "
                          "but I am not the owner of the requested slots. Failing link.",
                          (unsigned long long)c->id);
                error = 1;
                break;
            }
            /* The last argument should be SLOTSRANGE */
            break;
        }
    }
    if (!error && (!target_node || !link_name || !slot_ranges)) {
        serverLog(LL_WARNING,
                  "Missing >= 1 required argument in CLUSTER SYNCSLOTS SNAPSHOT from client %llu. "
                  "Failing link.",
                  (unsigned long long)c->id);
        error = 1;
    }
    if (error) {
        if (slot_ranges) {
            listRelease(slot_ranges);
        }
        sendFailAndCloseAfterReply(c);
        return;
    }

    slotMigrationLink *link = createSlotExportLink(c, target_node->name, link_name, slot_ranges);
    listAddNodeHead(server.cluster->slot_migration_links, link);

    /* No reply, we will proceed with AOF formatted dump once BGSAVE is able to
     * succeed */
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
}

/* There are three potential triggers for streaming (whichever happens first):
 *   1. SYNCSLOTS STREAM command
 *   2. SYNCSLOTS PAUSE command
 *   3. BGSAVE child process dies
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

/* Sent by the target to the source to begin streaming content after
 * snapshot. */
void clusterCommandSyncSlotsStream(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_EXPORT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS PAUSE from client %llu, "
                  "but the client is not a slot export target. Closing link.",
                  (unsigned long long)c->id);
        sendFailAndCloseAfterReply(c);
        return;
    }
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    /* The background save may have completed first, so we may already be in
     * the streaming state. */
    if (link->state != SLOT_EXPORT_STREAMING && link->state != SLOT_EXPORT_SNAPSHOTTING) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS STREAM on export to node %.40s, "
                  "but the client was not snapshotting or streaming incremental updates. Closing link.",
                  link->nodename);
        sendFailAndCloseAfterReply(c);
        return;
    }
    if (link->state != SLOT_EXPORT_STREAMING) {
        slotExportBeginStreaming(link);
    }
}

/* Sent by the target to the source to pause writes to the slot for slot
 * failover. */
void clusterCommandSyncSlotsPause(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_EXPORT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS PAUSE from client %llu, "
                  "but the client is not a slot export target. Closing link.",
                  (unsigned long long)c->id);
        sendFailAndCloseAfterReply(c);
        return;
    }
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    /* Child process may not have closed yet, so SNAPSHOTTING is okay here */
    if (link->state != SLOT_EXPORT_STREAMING && link->state != SLOT_EXPORT_SNAPSHOTTING) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS PAUSE on export to node %.40s, "
                  "but the client was not streaming incremental updates. Closing link.",
                  link->nodename);
        sendFailAndCloseAfterReply(link->client);
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED,
                                "Unexpected state machine transition");
        return;
    }
    if (link->state != SLOT_EXPORT_STREAMING) {
        slotExportBeginStreaming(link);
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

/* Sent by the target to the source to request final authorization for
 * failover. Authorization could be denied if the source has unpaused itself by
 * now. */
void clusterCommandSyncSlotsRequestFailover(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_EXPORT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS REQUEST-FAILOVER from client %llu, "
                  "but the client is not a slot export target. Closing link.",
                  (unsigned long long)c->id);
        sendFailAndCloseAfterReply(c);
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
        sendSyncSlotsMessageOnLink(link, "FAILOVER-DENIED");
        return;
    }

    /* Note that if any slot ownership is transferred, we should already have
     * failed in updateSlotExportIfOwnershipChanged. So here we just need to
     * check if we already committed this failover to another in-progress
     * export. */
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li))) {
        slotMigrationLink *other_link = (slotMigrationLink *)ln->value;
        if (other_link == link) continue;
        if (link->type != SLOT_MIGRATION_EXPORT) continue;
        if (other_link->state != SLOT_EXPORT_FAILOVER_GRANTED) continue;
        if (!doSlotRangeListsOverlap(link->slot_ranges, other_link->slot_ranges)) continue;
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS REQUEST-FAILOVER on link %.40s (for slots %s), "
                  "but we have already granted failover of at least one of these slots to link %.40s "
                  "(for slots %s). Denying failover.",
                  link->linkname,
                  link->slot_ranges_str,
                  other_link->linkname,
                  other_link->slot_ranges_str);
        sendSyncSlotsMessageOnLink(link, "FAILOVER-DENIED");
        updateSlotMigrationLinkState(link, SLOT_EXPORT_STREAMING);
        link->mf_end = 0;
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

/* Sent by the target to the source to notify of cancellation by user. */
void clusterCommandSyncSlotsCancel(client *c) {
    slotMigrationLink *link = (slotMigrationLink *)c->slot_migration_link;
    if (!link || link->type != SLOT_MIGRATION_EXPORT) {
        serverLog(LL_WARNING,
                  "Received CLUSTER SYNCSLOTS CANCEL from client %llu, "
                  "but the client is not a slot export target. Closing link.",
                  (unsigned long long)c->id);
        freeClientAsync(c);
        return;
    }
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_CANCELLED, NULL);
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
    return link->state != SLOT_EXPORT_SNAPSHOTTING &&
           link->state != SLOT_EXPORT_WAITING_TO_SNAPSHOT;
}

void clusterFeedSlotExportLinks(int dbid, robj **argv, int argc) {
    UNUSED(dbid);
    int i, error_code;
    int slot = -1;
    listIter li;
    listNode *ln;

    if (server.server_del_keys_in_slot && !server.current_client) {
        /* Three scenarios:
         *
         *  1. delKeysInSlot is triggered by user via CLUSTER FLUSHSLOT, in
               which case current_client should be set.
         *  2. delKeysInSlot is triggered by a topology update, in which case:
         *     a. The topology update is related to this slot, and we would have
         *        already cancelled/completed the migration via
         *        clusterUpdateSlot(Imports|Exports)OnOwnershipChange
         *     b. The topology update is unrelated to this slot, so we don't
         *        care about the deleted keys.
         *
         * Because of this, we can simply return if it is scenario 2 */
        return;
    }

    /* This function may be called after the command is executed.
     * At this time, the arg in argv may be rewritten and the encoding
     * may be an INT. In this case, we need to decode it into a string
     * object because in getKeysFromCommand all the args are a string. */
    robj **new_argv = NULL;
    for (int i = 0; i < argc; i++) {
        if (!sdsEncodedObject(argv[i])) {
            new_argv = zmalloc(sizeof(robj *) * (argc));
            break;
        }
    }
    if (new_argv) {
        for (int i = 0; i < argc; i++) new_argv[i] = getDecodedObject(argv[i]);
        argv = new_argv;
    }

    /* Check the slot this command belongs to. Note that it is not a guarantee
     * that the slot of the replicated command is the same as the slot of the
     * executed command, for example in the case of module VM_Replicate APIs.
     * Because of this case, we need to redo the slot lookup completely at this
     * time. */
    struct serverCommand *cmd = lookupCommand(argv, argc);
    getNodeByQuery(server.current_client, cmd, argv, argc, &slot, &error_code);
    if (error_code != CLUSTER_REDIR_NONE || slot == -1) {
        /* A couple cases where this could happen:
         *    - The replicated command is a command without a slot.
         *    - The replicated command is written by VM_Replicate module APIs
         *      and is a cross-slot command, or a slot that is not owned by
         *      this node.
         *
         * In any case, our best solution is to not replicate this to the
         * target node. */
        return;
    }

    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li))) {
        slotMigrationLink *link = (slotMigrationLink *)ln->value;
        if (link->type != SLOT_MIGRATION_EXPORT) continue;
        if (!link->client) continue;
        if (!isSlotMigrationLinkInProgress(link)) continue;
        if (!isSlotInSlotRanges(slot, link->slot_ranges)) continue;

        addReplyArrayLen(link->client, argc);
        for (i = 0; i < argc; i++) {
            addReplyBulk(link->client, argv[i]);
        }
    }

    if (new_argv) {
        for (int i = 0; i < argc; i++) decrRefCount(new_argv[i]);
        zfree(new_argv);
    }
}

void clusterHandleSlotExportLinkClientClose(slotMigrationLink *link) {
    link->client = NULL;
    if (!isSlotMigrationLinkInProgress(link)) {
        return;
    }
    serverLog(LL_NOTICE,
              "Slot export connection to node %.40s (for slots [%s]) lost.",
              link->nodename,
              link->slot_ranges_str);

    /* If we have granted failover, the failover may have happened, but we don't know. We keep the
     * slot export around so that we remain paused until we find out about the takeover (or until
     * the pause times out).
     *
     * Otherwise, we can mark it failed. */
    if (link->state != SLOT_EXPORT_FAILOVER_GRANTED) {
        finishSlotMigrationLink(link, SLOT_MIGRATION_LINK_FAILED, "Connection lost to target");
    }
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
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

    /* Instigate import failure manually, in case topology update is slow to reach source. */
    sendFailAndCloseAfterReply(link->client);
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
createSlotExportLink(client *c, char *nodename, char *linkname, list *slot_ranges) {
    slotMigrationLink *link = zcalloc(sizeof(slotMigrationLink));

    memcpy(link->nodename, nodename, CLUSTER_NAMELEN);
    memcpy(link->linkname, linkname, CLUSTER_NAMELEN);
    link->ctime = server.unixtime;
    link->last_update = link->ctime;
    link->last_ack = link->ctime;
    link->client = c;
    link->type = SLOT_MIGRATION_EXPORT;
    link->state = SLOT_EXPORT_WAITING_TO_SNAPSHOT;
    link->slot_ranges = slot_ranges;
    link->slot_ranges_str = representSlotRangeList(slot_ranges);
    link->client->slot_migration_link = link;
    initClientReplicationData(link->client);

    serverLog(LL_NOTICE,
              "New slot export link created: link name %.40s, target node %.40s, slot range %s",
              link->linkname,
              link->nodename,
              link->slot_ranges_str);
    return link;
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
 *      │  │SLOT_IMPORT_FAILOVER_WAITING_FOR_PAUSED│                    │SLOT_EXPORT_FAILOVER_PAUSED├──────┘
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
        case SLOT_IMPORT_CONNECTING:
            if (!link->conn) {
                connectSlotImportLink(link);
            }
            if (proceedWithSlotImportLinkConnecting(link)) {
                continue;
            }
            return;
        case SLOT_IMPORT_AUTHENTICATING:
            performSlotImportLinkAuthentication(link);
            continue;
        case SLOT_IMPORT_START_SNAPSHOT:
            initSlotImportLinkClient(link);
            performSlotImportLinkSnapshot(link);
            return;
        case SLOT_IMPORT_FAILOVER_GRANTED:
            performSlotImportLinkFailover(link);
            return;
        case SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP:
            delKeysNotOwnedByMyself(link->slot_ranges);
            updateSlotMigrationLinkState(link, link->post_cleanup_state);
            return;
        case SLOT_IMPORT_RECEIVE_SNAPSHOT:
        case SLOT_IMPORT_RECEIVE_STREAM:
        case SLOT_IMPORT_FAILOVER_WAITING_FOR_PAUSED:
        case SLOT_IMPORT_FAILOVER_REQUESTED:
            return;

        /* Exporting states */
        case SLOT_EXPORT_WAITING_TO_SNAPSHOT:
            /* Perform the snapshot whenever the opportunity arises. */
            if (hasActiveChildProcess()) {
                return;
            }
            slotExportLinkBeginSnapshot(link);
            return;
        case SLOT_EXPORT_SNAPSHOTTING:
        case SLOT_EXPORT_STREAMING:
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
                updateSlotMigrationLinkState(link, SLOT_EXPORT_STREAMING);
                updateSlotMigrationLinkStatusMessage(link, "Timed out before streaming completed");
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
            /* Nothing to do right now in cron */
            return;
        }
    }
}

void resetSlotMigrationLink(slotMigrationLink *link) {
    /* Only one of client or conn should be set. */
    serverAssert(!link->client || !link->conn);
    if (link->client) {
        if (!link->client->flag.close_after_reply) {
            freeClientAsync(link->client);
        }
        link->client = NULL;
    } else if (link->conn) {
        connClose(link->conn);
        link->conn = NULL;
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
    zfree(o);
}

void clusterHandleSlotMigrationLinkClientClose(void *o) {
    slotMigrationLink *link = (slotMigrationLink *)o;
    switch (link->type) {
    case SLOT_MIGRATION_IMPORT:
        clusterHandleSlotImportLinkClientClose(link);
        return;
    case SLOT_MIGRATION_EXPORT:
        clusterHandleSlotExportLinkClientClose(link);
        return;
    }
}

void initClusterSlotMigrationLinkList(void) {
    server.cluster->slot_migration_links = listCreate();
    listSetFreeMethod(server.cluster->slot_migration_links, freeSlotMigrationLink);
}

int shouldCleanupSlotMigrationLink(slotMigrationLink *link, unsigned long idx) {
    return (link->state == SLOT_MIGRATION_LINK_CANCELLED ||
            link->state == SLOT_MIGRATION_LINK_FAILED ||
            link->state == SLOT_MIGRATION_LINK_SUCCESS) &&
           ((server.cluster_slot_migration_log_ttl &&
             server.unixtime - link->last_update >= server.cluster_slot_migration_log_ttl) ||
            idx >= server.cluster_slot_migration_log_max_len);
}

const char *slotMigrationLinkStateToString(slotMigrationLinkState state) {
    switch (state) {
    case SLOT_IMPORT_CONNECTING: return "connecting";
    case SLOT_IMPORT_AUTHENTICATING: return "authenticating";
    case SLOT_IMPORT_START_SNAPSHOT:
    case SLOT_IMPORT_RECEIVE_SNAPSHOT: return "snapshotting";
    case SLOT_IMPORT_RECEIVE_STREAM: return "replicating";
    case SLOT_IMPORT_FAILOVER_WAITING_FOR_PAUSED: return "failover-syncing";
    case SLOT_IMPORT_FAILOVER_REQUESTED: return "failover-requested";
    case SLOT_IMPORT_FAILOVER_GRANTED: return "failover-granted";
    case SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP: return "cleaning-up";

    case SLOT_EXPORT_WAITING_TO_SNAPSHOT: return "waiting-to-snapshot";
    case SLOT_EXPORT_SNAPSHOTTING: return "snapshotting";
    case SLOT_EXPORT_STREAMING: return "replicating";
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
        /* Defer cleanup until beforeSleep, since it needs to be done outside an execution unit. */
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
    unsigned long idx = 0;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotMigrationLink *link = ln->value;
        if (shouldCleanupSlotMigrationLink(link, idx)) {
            listDelNode(server.cluster->slot_migration_links, ln);
            continue;
        }
    }
    idx++;
}

void clusterSlotMigrationCron(void) {
    slotMigrationLink *link;
    listNode *ln;
    listIter li;
    unsigned long idx = 0;
    int paused = 0;
    listRewind(server.cluster->slot_migration_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        link = ln->value;

        if (link->client) {
            time_t last_interaction = link->client->last_interaction;
            if (link->type == SLOT_MIGRATION_EXPORT) {
                /* For migrations, we just use the last ack received time */
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
                if (link->state != SLOT_EXPORT_SNAPSHOTTING) {
                    run_with_period(1000) sendSyncSlotsMessageOnLink(link, "ACK");
                }
            }
        }
        proceedWithSlotMigration(link);
        if (link->mf_end) {
            paused++;
        }
        if (shouldCleanupSlotMigrationLink(link, idx)) {
            listDelNode(server.cluster->slot_migration_links, ln);
            continue;
        }
        idx++;
    }

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
    if (!strcasecmp(c->argv[2]->ptr, "snapshot")) {
        /* CLUSTER SYNCSLOTS SNAPSHOT <args> */
        clusterCommandSyncSlotsSnapshot(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "snapshot-eof")) {
        /* CLUSTER SYNCSLOTS SNAPSHOT-EOF */
        clusterCommandSyncSlotsSnapshotEof(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "stream")) {
        /* CLUSTER SYNCSLOTS STREAM */
        clusterCommandSyncSlotsStream(c);
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
    } else if (!strcasecmp(c->argv[2]->ptr, "cancel")) {
        /* CLUSTER SYNCSLOTS CANCEL */
        clusterCommandSyncSlotsCancel(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "fail")) {
        /* CLUSTER SYNCSLOTS FAIL */
        clusterCommandSyncSlotsFail(c);
    } else {
        /* Ignore unknown SYNCSLOTS commands to simplify forwards compatibility */
        serverLog(LL_NOTICE, "Got unknown SYNCSLOTS subcommand, ignoring...");
    }
}
