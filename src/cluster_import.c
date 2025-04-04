/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"
#include "bio.h"
#include "module.h"
#include "functions.h"

int isSlotImportLinkInProgress(slotImportLink *link);
slotImportLink *createSlotImportLink(list *slot_ranges);
void connectSlotImportLink(slotImportLink *link, clusterNode *source_node);
const char *slotImportStateToString(slotImportLinkState state);
void updateSlotImportLinkState(slotImportLink *link, slotImportLinkState state);
void sendSyncSlotsMessageToSource(client *c, const char *subcommand);
void proceedWithSlotImport(slotImportLink *link);
void cancelSlotImportLink(slotImportLink *link, slotImportLinkState reason);
slotExportLink *createSlotExportLink(client *c, char *nodename, list *slot_ranges);
void updateSlotExportLinkState(slotExportLink *link, slotExportLinkState state);
void sendSyncSlotsMessageToTarget(client *c, const char *subcommand);
void checkSlotExportPauseTimeout(slotExportLink *link);

void freeSlotRangeValue(void *o) {
    zfree(o);
}

void *dupSlotRangeValue(void *o) {
    slotRange *in = o;
    slotRange *out = zmalloc(sizeof(slotRange));
    out->start_slot = in->start_slot;
    out->end_slot = in->end_slot;
    return out;
}

list *createSlotRangeList(void) {
    list *slot_ranges = listCreate();
    listSetFreeMethod(slot_ranges, freeSlotRangeValue);
    listSetDupMethod(slot_ranges, dupSlotRangeValue);
    return slot_ranges;
}

sds representSlotRangeList(list *slot_ranges, const char separator) {
    sds res = sdsempty();
    listNode *ln;
    listIter li;
    int first = 1;
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        if (first) {
            res = sdscatprintf(res, "%d%c%d", range->start_slot, separator, range->end_slot);
            first = 0;
        } else {
            res = sdscatprintf(res, " %d%c%d", range->start_slot, separator, range->end_slot);
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
        if ((endslot = getSlotOrError(c->argv[i + 1], err_out)) == -1) {
            listRelease(slot_ranges);
            return NULL;
        }
        if (startslot > endslot) {
            *err_out = sdscatfmt(sdsempty(), "Start slot number %d is greater than end slot number %d.", startslot, endslot);
            listRelease(slot_ranges);
            return NULL;
        }
        /* Check if the current slot range is ready to do the slot sync. */
        for (int j = startslot; j <= endslot; j++) {
            if (server.cluster->slots[j] == NULL) {
                *err_out = sdscatfmt(sdsempty(), "Slot %d has no node served.", j);
                listRelease(slot_ranges);
                return NULL;
            }
            if (!*node_out) {
                *node_out = server.cluster->slots[j];
            } else if (*node_out != server.cluster->slots[j]) {
                *err_out = sdscatfmt(sdsempty(), "The slot ranges can not cross nodes, please check slots and try again");
                listRelease(slot_ranges);
                return NULL;
            }
        }
        /* Add the current slot range to the range list. */
        slotRange *new_range = zmalloc(sizeof(slotRange));
        new_range->start_slot = startslot;
        new_range->end_slot = endslot;
        listAddNodeTail(slot_ranges, new_range);
    }
    return slot_ranges;
}

/* -------------------------------------------- TARGET -----------------------------------------
 *
 * During a slot import, the target drives the main state machine and eventually performs the
 * slot takeover. Slot import is initiated when an operator sends a CLUSTER IMPORT request, after
 * which the target node will track the import in a slotImportLink.
 *
 * For transient errors like connections being dropped, the target will restart the import workflow
 * from the beginning. For other errors, the import will be marked as failed and require operator
 * intervention to retry.
 *
 * An operator can view the status of imports with CLUSTER IMPORT-INFO, and cancel imports with
 * CLUSTER IMPORT-CANCEL.
 */

int isSlotImportingViaReplication(int slot) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_import_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotImportLink *link = ln->value;
        if (!isSlotImportLinkInProgress(link)) {
            continue;
        }
        if (isSlotInSlotRanges(slot, link->slot_ranges)) {
            return 1;
        }
    }
    return 0;
}

/* Sent by an operator to the target (which must be a primary), the target will
 * attempt to import and failover the provided slot range from the current owner. */
void clusterCommandImport(client *c) {
    if (!nodeIsPrimary(server.cluster->myself)) {
        addReplyError(c, "Myself should be a primary.");
        return;
    }

    if (isAnySlotInManualImportingState()) {
        addReplyError(c, "Some slots are being manually imported. Please get all slots to a stable state before attempting import.");
        return;
    }
    if (isAnySlotInManualMigratingState()) {
        addReplyError(c, "Some slots are being manually migrated. Please get all slots to a stable state before attempting import.");
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
        sdsfree(err);
        return;
    }
    if (source_node == server.cluster->myself) {
        addReplyErrorFormat(c, "Slots are already served by myself.");
        return;
    }
    listIter li;
    listNode *ln;
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li))) {
        slotRange *range = (slotRange *)ln->value;
        for (int j = range->start_slot; j <= range->end_slot; j++) {
            if (isSlotImportingViaReplication(j)) {
                addReplyErrorFormat(c, "I am already replicating slot %d.", j);
                listRelease(slot_ranges);
                return;
            }
        }
    }

    slotImportLink *link = createSlotImportLink(slot_ranges);
    connectSlotImportLink(link, source_node);
    listAddNodeTail(server.cluster->slot_import_links, link);
    addReply(c, shared.ok);
}

/* Synthesizes a view of ongoing and recently completed imports for an operator. */
void clusterCommandImportInfo(client *c) {
    addReplyArrayLen(c, listLength(server.cluster->slot_import_links));

    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_import_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotImportLink *link = ln->value;

        addReplyMapLen(c, 6);
        addReplyBulkCString(c, "create_time_ms");
        addReplyLongLong(c, link->ctime);
        addReplyBulkCString(c, "last_update_time_ms");
        addReplyLongLong(c, link->last_update);
        addReplyBulkCString(c, "source_node");
        addReplyBulkCBuffer(c, link->nodename, CLUSTER_NAMELEN);
        addReplyBulkCString(c, "state");
        addReplyBulkCString(c, slotImportStateToString(link->state));
        addReplyBulkCString(c, "message");
        addReplyBulkCString(c, link->status_msg ? link->status_msg : "");
        addReplyBulkCString(c, "slot-ranges");
        addReplyBulkCString(c, link->slot_ranges_str);
    }
}

/* Cancels all ongoing imports. */
void clusterCommandImportCancel(client *c) {
    listNode *ln;
    listIter li;

    if (!clusterIsAnySlotImportingViaRepl()) {
        addReplyError(c, "No imports ongoing");
        return;
    }

    listRewind(server.cluster->slot_import_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotImportLink *link = ln->value;
        if (!isSlotImportLinkInProgress(link)) {
            continue;
        }
        updateSlotImportLinkState(link, SLOT_IMPORT_CANCEL);
    }

    serverLog(LL_NOTICE, "Canceled all in progres slot imports due to operator's request.");

    addReply(c, shared.ok);
}

/* Sent by the source to the target after dumping the snapshot in
 * AOF format. */
void clusterCommandSyncSlotsSnapshotEof(client *c) {
    if (c->flag.primary) {
        /* During slot migration, our primary will send us this as it is
         * proxying the snapshot directly to us. We can safely ignore it. */
        serverLog(LL_NOTICE, "Received CLUSTER SYNCSLOTS SNAPSHOT-EOF from my primary. Ignoring.");
        return;
    }
    if (!c->flag.slot_import_source || !c->slot_import_link) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS SNAPSHOT-EOF from client %lu, but the client is not a slot import source. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    slotImportLink *link = (slotImportLink *)c->slot_import_link;
    if (link->state != SLOT_IMPORT_RECEIVE_SNAPSHOT) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS SNAPSHOT-EOF from client %lu, but not currently loading an AOF snapshot. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    serverLog(LL_NOTICE, "Successfully received slot snapshot for %s from %.40s. Beginning incremental stream...", link->slot_ranges_str, link->nodename);
    sendSyncSlotsMessageToSource(c, "PAUSE");
    updateSlotImportLinkState(link, SLOT_IMPORT_FAILOVER_WAITING_FOR_PAUSED);
}

/* Sent by the source to the target as a marker of when the pause
 * began (therefore, target is caught up once read). */
void clusterCommandSyncSlotsPaused(client *c) {
    if (c->flag.primary) {
        /* During slot migration, our primary will send us this as it is
         * proxying the snapshot directly to us. We can safely ignore it. */
        serverLog(LL_NOTICE, "Received CLUSTER SYNCSLOTS PAUSED from my primary. Ignoring.");
        return;
    }
    if (!c->flag.slot_import_source || !c->slot_import_link) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS PAUSED from client %lu, but the client is not a slot import source. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    slotImportLink *link = (slotImportLink *)c->slot_import_link;
    if (link->state != SLOT_IMPORT_FAILOVER_WAITING_FOR_PAUSED) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS PAUSED from client %lu, but client is not currently in paused state locally. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    sendSyncSlotsMessageToSource(c, "REQUEST-FAILOVER");
    updateSlotImportLinkState(link, SLOT_IMPORT_FAILOVER_REQUESTED);
}

/* Sent by the source to the target to grant final authorization for
 * failover. */
void clusterCommandSyncSlotsFailoverGranted(client *c) {
    if (c->flag.primary) {
        /* During slot migration, our primary will send us this as it is
         * proxying the snapshot directly to us. We can safely ignore it. */
        serverLog(LL_NOTICE, "Received CLUSTER SYNCSLOTS FAILOVER-GRANTED from my primary. Ignoring.");
        return;
    }
    if (!c->flag.slot_import_source || !c->slot_import_link) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS FAILOVER-GRANTED from client %lu, but the client is not a slot import source. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    slotImportLink *link = (slotImportLink *)c->slot_import_link;
    if (link->state != SLOT_IMPORT_FAILOVER_REQUESTED) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS FAILOVER-GRANTED from client %lu, but we never sent a failover request. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    updateSlotImportLinkState(link, SLOT_IMPORT_FAILOVER_GRANTED);
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
}

/* Sent by the source to the target to deny final authorization for
 * failover. */
void clusterCommandSyncSlotsFailoverDenied(client *c) {
    if (!c->flag.slot_import_source || !c->slot_import_link) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS FAILOVER-DENIED from client %lu, but the client is not a slot import source. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    slotImportLink *link = (slotImportLink *)c->slot_import_link;
    if (link->state != SLOT_IMPORT_FAILOVER_REQUESTED) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS FAILOVER-DENIED from client %lu, but we never sent a failover request. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    sds slot_ranges = representSlotRangeList(link->slot_ranges, '-');
    serverLog(LL_WARNING, "Failover was denied from source node %s for slots (%s). Failing import request", link->nodename, slot_ranges);
    link->status_msg = sdscatfmt(sdsempty(), "Failover denied from source owner");
    updateSlotImportLinkState(link, SLOT_IMPORT_FAIL);
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
}

void sendSyncSlotsMessageToSource(client *c, const char *subcommand) {
    serverAssert(c && c->flag.slot_import_source && c->slot_import_link);
    ClientFlags old_flags = c->flag;
    c->flag.pushing = 1;
    addReplyArrayLen(c, 3);
    addReplyBulkCString(c, "CLUSTER");
    addReplyBulkCString(c, "SYNCSLOTS");
    addReplyBulkCString(c, subcommand);
    if (!old_flags.pushing) c->flag.pushing = 0;
}

slotImportLink *createSlotImportLink(list *slot_ranges) {
    slotImportLink *link = zcalloc(sizeof(slotImportLink));
    link->ctime = mstime();
    link->slot_ranges = slot_ranges;
    link->slot_ranges_str = representSlotRangeList(slot_ranges, '-');
    return link;
}

void slotImportConnectHandler(connection *conn) {
    slotImportLink *link = (slotImportLink *)connGetPrivateData(conn);
    proceedWithSlotImport(link);
}

void connectSlotImportLink(slotImportLink *link, clusterNode *source_node) {
    memcpy(link->nodename, source_node->name, CLUSTER_NAMELEN);

    serverLog(LL_NOTICE, "Connecting to slot owner %.40s (%s) %s:%d for import of slots %s.",
              source_node->name, source_node->human_nodename, source_node->ip,
              getNodeDefaultClientPort(source_node), link->slot_ranges_str);

    link->conn = connCreate(connTypeOfCluster());
    if (connConnect(link->conn, source_node->ip, getNodeDefaultClientPort(source_node),
                    server.bind_source_addr, slotImportConnectHandler) == C_ERR) {
        serverLog(LL_WARNING, "Unable to connect to %.40s (%s) for import of slots (%s): %s", source_node->name,
                  source_node->human_nodename, link->slot_ranges_str, connGetLastError(link->conn));
        link->status_msg = sdscatfmt(sdsempty(), "Unable to connect to source node: %s", connGetLastError(link->conn));
        cancelSlotImportLink(link, SLOT_IMPORT_FAILED);
        return;
    }

    connSetPrivateData(link->conn, link);
    updateSlotImportLinkState(link, SLOT_IMPORT_CONNECTING);
}

void resetSlotImportLink(slotImportLink *link) {
    /* Only one of client or conn should be set. */
    serverAssert(!link->client || !link->conn);
    if (link->client) {
        /* Since we are trying to manually close the connection, clear slot
         * import data so that we don't trigger
         * handleSlotImportLinkClientClose*/
        link->client->flag.slot_import_source = 0;
        link->client->slot_import_link = NULL;
        freeClientAsync(link->client);
        link->client = NULL;
    } else if (link->conn) {
        connClose(link->conn);
        link->conn = NULL;
    }
}

void handleSlotImportLinkClientClose(void *import) {
    slotImportLink *link = (slotImportLink *)import;
    link->client = NULL;
    if (link->state == SLOT_IMPORT_RECONNECT) {
        /* Already reconnecting */
        return;
    }
    serverLog(LL_WARNING, "Slot import connection to node %.40s (owner of slots [%s]) lost. Reconnecting...", link->nodename, link->slot_ranges_str);
    updateSlotImportLinkState(link, SLOT_IMPORT_RECONNECT);
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
}

void handleSlotImportLinkClientOOM(void *import) {
    slotImportLink *link = (slotImportLink *)import;
    if (link->state != SLOT_IMPORT_FAILED) {
        serverLog(LL_WARNING, "Failing slot import of slots (%s) from node %.40s due to OOM", link->slot_ranges_str, link->nodename);
        link->status_msg = sdscatfmt(sdsempty(), "OOM during slot import");
        cancelSlotImportLink(link, SLOT_IMPORT_FAILED);
    }
}

void freeSlotImportLink(void *o) {
    slotImportLink *link = o;
    resetSlotImportLink(link);
    zfree(link->slot_ranges);
    sdsfree(link->slot_ranges_str);
    if (link->status_msg) {
        sdsfree(link->status_msg);
    }
    zfree(o);
}

void initClusterSlotImportLinkList(void) {
    server.cluster->slot_import_links = listCreate();
    listSetFreeMethod(server.cluster->slot_import_links, freeSlotImportLink);
}

int isSlotImportLinkInProgress(slotImportLink *link) {
    return link->state != SLOT_IMPORT_FAILED && link->state != SLOT_IMPORT_FAILOVER_COMPLETE && link->state != SLOT_IMPORT_CANCELED;
}

const char *slotImportStateToString(slotImportLinkState state) {
    switch (state) {
    case SLOT_IMPORT_RECONNECT: return "reconnecting";
    case SLOT_IMPORT_CONNECTING: return "connecting";
    case SLOT_IMPORT_AUTHENTICATING: return "authenticating";
    case SLOT_IMPORT_START_SNAPSHOT:
    case SLOT_IMPORT_RECEIVE_SNAPSHOT: return "snapshotting";
    case SLOT_IMPORT_RECEIVE_STREAM: return "replicating";
    case SLOT_IMPORT_FAILOVER_WAITING_FOR_PAUSED: return "failover-syncing";
    case SLOT_IMPORT_FAILOVER_REQUESTED: return "failover-requested";
    case SLOT_IMPORT_FAILOVER_GRANTED: return "failover-granted";
    case SLOT_IMPORT_FAILOVER_COMPLETE: return "failover-complete";
    case SLOT_IMPORT_FAIL:
    case SLOT_IMPORT_FAILED: return "failed";
    case SLOT_IMPORT_CANCEL:
    case SLOT_IMPORT_CANCELED: return "canceled";
    default: return "unknown";
    }
}

void updateSlotImportLinkState(slotImportLink *link, slotImportLinkState state) {
    // Debug only
    serverLog(LL_NOTICE, "SLOT IMPORT: %s -> %s", slotImportStateToString(link->state), slotImportStateToString(state));
    link->last_update = mstime();
    link->state = state;
}

int performSlotImportLinkReconnect(slotImportLink *link) {
    /* Leaving partially imported slots would result in corruption. */
    resetSlotImportLink(link);
    delKeysNotOwnedByMyself(link->slot_ranges);

    int cross_node = 0;

    /* Validate that the slot replication is still owned by one node */
    clusterNode *n = getClusterNodeBySlotRanges(link->slot_ranges, &cross_node);
    if (!n) {
        if (cross_node) {
            serverLog(LL_WARNING, "Slots (%s) being imported from node %.40s are no longer owned "
                                  "by a single node. Stopping slot import.",
                      link->slot_ranges_str, link->nodename);
            link->status_msg = sdscatfmt(sdsempty(), "Slots are no longer owned by a single node");
        } else {
            serverLog(LL_WARNING, "Slots (%s) being imported from node %.40s contains slots no "
                                  "longer owned by any node. Stopping slot import.",
                      link->slot_ranges_str, link->nodename);
            link->status_msg = sdscatfmt(sdsempty(), "Slots are no longer owned by any node in the cluster");
        }
        cancelSlotImportLink(link, SLOT_IMPORT_FAILED);
        return C_ERR;
    }

    /* The target node should not be myself. */
    if (n == server.cluster->myself) {
        serverLog(LL_WARNING, "Slots (%s) being imported from node %.40s already owned by myself. "
                              "Stopping slot import.",
                  link->slot_ranges_str, link->nodename);
        link->status_msg = sdscatfmt(sdsempty(), "Slots were unexpectedly assigned to myself during import");
        cancelSlotImportLink(link, SLOT_IMPORT_FAILED);
        return C_ERR;
    }

    /* Do the reconnect for this link. */
    connectSlotImportLink(link, n);

    return C_OK;
}

void cancelSlotImportLink(slotImportLink *link, slotImportLinkState reason) {
    delKeysNotOwnedByMyself(link->slot_ranges);
    resetSlotImportLink(link);
    updateSlotImportLinkState(link, reason);
}

/* This function implements the final part of manual slot failovers,
 * where the replica grabs all the slotsync link's hash slots, and
 * propagates the new configuration.
 *
 * Note that it's up to the caller to be sure that the node got a new
 * configuration epoch already. */
void performSlotImportLinkFailover(slotImportLink *link) {
    /* 1) Force bump the epoch to facilitate propagation. */
    clusterBumpConfigEpochWithoutConsensus();

    /* 2) Claim all the slots in the slotsync links to myself. */
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_import_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotImportLink *link = ln->value;

        listNode *ln2;
        listIter li2;
        listRewind(link->slot_ranges, &li2);
        while ((ln2 = listNext(&li2)) != NULL) {
            slotRange *range = ln2->value;
            for (int i = range->start_slot; i <= range->end_slot; i++) {
                clusterDelSlot(i);
                clusterAddSlot(server.cluster->myself, i);
            }
        }
    }

    /* 3) Update state and save config. */
    clusterUpdateState();
    clusterSaveConfigOrDie(1);

    /* 4) Pong all the other nodes so that they can update the state accordingly
     *    and detect that we switched to master role. */
    clusterBroadcastPong(CLUSTER_BROADCAST_ALL);

    /* 5) Reflect the failover in the slot import link state */
    serverLog(LL_NOTICE, "Successfully took over slots %s from source node %.40s!", link->slot_ranges_str, link->nodename);
    resetSlotImportLink(link);
    updateSlotImportLinkState(link, SLOT_IMPORT_FAILOVER_COMPLETE);
}

/* Determine if the link is connected, and return 1 if so. Handles state transition in the case of
 * success/failure.*/
int proceedWithSlotImportLinkConnecting(slotImportLink *link) {
    serverAssert(link->conn);
    switch (connGetState(link->conn)) {
    case CONN_STATE_CONNECTED:
        serverLog(LL_NOTICE, "Successfully connected to slot owner %.40s for import of slots (%s).", link->nodename, link->slot_ranges_str);
        updateSlotImportLinkState(link, SLOT_IMPORT_AUTHENTICATING);
        return 1;
    case CONN_STATE_CONNECTING:
        return 0;
    default:
        serverLog(LL_NOTICE, "Failed to connect to slot owner %.40s for import of slots (%s): %s", link->nodename, link->slot_ranges_str, connGetLastError(link->conn));
        link->status_msg = sdscatfmt(sdsempty(), "Unable to connect to source node: %s", connGetLastError(link->conn));
        cancelSlotImportLink(link, SLOT_IMPORT_FAILED);
        return 1;
    }
}

void performSlotImportLinkAuthentication(slotImportLink *link) {
    if (!server.primary_auth) {
        updateSlotImportLinkState(link, SLOT_IMPORT_START_SNAPSHOT);
        return;
    }
    char *err = replicationSendAuth(link->conn);
    if (err) {
        serverLog(LL_NOTICE, "Failed to send AUTH command to node %.40s for import of slots (%s): %s", link->nodename, link->slot_ranges_str, err);
        link->status_msg = sdscatfmt(sdsempty(), "Failed to send AUTH to source node: %s", err);
        sdsfree(err);
        cancelSlotImportLink(link, SLOT_IMPORT_FAILED);
        return;
    }
    err = receiveSynchronousResponse(link->conn);
    if (err == NULL) {
        serverLog(LL_WARNING, "Received no response to AUTH command from node %.40s for import of slots (%s)", link->nodename, link->slot_ranges_str);
        link->status_msg = sdscatfmt(sdsempty(), "AUTH command received no response");
        cancelSlotImportLink(link, SLOT_IMPORT_FAILED);
        return;
    }
    if (err[0] == '-') {
        serverLog(LL_WARNING, "Failed to AUTH to node %.40s for import of slots (%s): %s", link->nodename, link->slot_ranges_str, err);
        link->status_msg = sdscatfmt(sdsempty(), "Failed to AUTH to source node: %s", err);
        cancelSlotImportLink(link, SLOT_IMPORT_FAILED);
    } else {
        serverLog(LL_NOTICE, "Successfully authenticated to node %.40s for import of slots (%s)", link->nodename, link->slot_ranges_str);
        updateSlotImportLinkState(link, SLOT_IMPORT_START_SNAPSHOT);
    }
    sdsfree(err);
}

void initSlotImportLinkClient(slotImportLink *link) {
    link->client = createClient(link->conn);
    link->conn = NULL;
    link->client->flag.authenticated = 1;
    link->client->flag.reply_off = 1;
    link->client->slot_import_link = link;
    link->client->flag.slot_import_source = 1;
    link->client->flag.import_source = 1;

    /* Use dedicated querybuf and replication data to proxy
     * replication stream to replicas directly. */
    initClientReplicationData(link->client);
    link->client->querybuf = sdsempty();
}

void performSlotImportLinkSnapshot(slotImportLink *link) {
    listIter li;
    listNode *ln;
    client *c = link->client;
    serverAssert(c);

    ClientFlags old_flags = c->flag;
    c->flag.pushing = 1;
    addReplyArrayLen(c, 6 + listLength(link->slot_ranges) * 2);
    addReplyBulkCString(c, "CLUSTER");
    addReplyBulkCString(c, "SYNCSLOTS");
    addReplyBulkCString(c, "SNAPSHOT");
    addReplyBulkCString(c, "TARGET");
    addReplyBulkCBuffer(c, server.cluster->myself->name, CLUSTER_NAMELEN);
    addReplyBulkCString(c, "SLOTSRANGE");
    listRewind(link->slot_ranges, &li);
    while ((ln = listNext(&li))) {
        slotRange *range = (slotRange *)ln->value;
        addReplyBulkLongLong(c, range->start_slot);
        addReplyBulkLongLong(c, range->end_slot);
    }
    if (!old_flags.pushing) c->flag.pushing = 0;

    updateSlotImportLinkState(link, SLOT_IMPORT_RECEIVE_SNAPSHOT);
}


void proceedWithSlotImport(slotImportLink *link) {
    /* Continue within the state machine until we have no more work. */
    while (1) {
        switch (link->state) {
        case SLOT_IMPORT_RECONNECT:
            performSlotImportLinkReconnect(link);
            return;
        case SLOT_IMPORT_CONNECTING:
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
        case SLOT_IMPORT_FAIL:
            cancelSlotImportLink(link, SLOT_IMPORT_FAILED);
            return;
        case SLOT_IMPORT_CANCEL:
            cancelSlotImportLink(link, SLOT_IMPORT_CANCELED);
            return;
        case SLOT_IMPORT_RECEIVE_SNAPSHOT:
        case SLOT_IMPORT_RECEIVE_STREAM:
        case SLOT_IMPORT_FAILOVER_WAITING_FOR_PAUSED:
        case SLOT_IMPORT_FAILOVER_REQUESTED:
        case SLOT_IMPORT_FAILED:
        case SLOT_IMPORT_FAILOVER_COMPLETE:
        case SLOT_IMPORT_CANCELED:
            /* Nothing to do right now in cron */
            return;
        }
    }
}

void proceedWithAllSlotImports(void) {
    slotImportLink *link;
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_import_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        link = ln->value;
        if (!isSlotImportLinkInProgress(link)) {
            /* Cleanup old records */
            if (link->last_update < mstime() - CLUSTER_SLOT_IMPORT_LOG_TTL) {
                listDelNode(server.cluster->slot_import_links, ln);
            }
            continue;
        }
        if (link->client) {
            if (server.unixtime - link->client->last_interaction > server.repl_timeout) {
                serverLog(LL_WARNING, "Timing out slot import from node %.40s for slots (%s) after not receiving interaction for too long", link->nodename, link->slot_ranges_str);
                link->status_msg = sdscatfmt(sdsempty(), "Timed out after too long with no interaction");
                cancelSlotImportLink(link, SLOT_IMPORT_FAILED);
                continue;
            }
            /* Periodically send ACK as a liveness check */
            run_with_period(1000) sendSyncSlotsMessageToSource(link->client, "ACK");
        }
        proceedWithSlotImport(link);
    }
}

int clusterIsAnySlotImportingViaRepl(void) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_import_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotImportLink *link = ln->value;
        if (isSlotImportLinkInProgress(link)) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------- SOURCE ------------------------------------------
 *
 * During a slot import, the source node tracks all the target nodes in slotExportLinks.
 * A slotExportLink is initially created when the target connects and sends a SYNCSLOTS command
 * to us. After this, we ensure that all data in the requested slots are sent to the target node.
 *
 * If at any time we detect an error, the source side will simply terminate the slot export. The
 * slot import is responsible for determining how to proceed from there. The one exception is
 * during failover, where we will explicitly send a FAILOVER-DENIED message if we do not want
 * to proceed with the failover, but leave the export alive.
 */

/* Sent by the target to the source to initiate the AOF formatted
 * snapshot. */
void clusterCommandSyncSlotsSnapshot(client *c) {
    if (!nodeIsPrimary(server.cluster->myself)) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS SNAPSHOT from client %lu, but I am not a primary. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    if (strcasecmp(c->argv[3]->ptr, "target") || sdslen(c->argv[4]->ptr) != CLUSTER_NAMELEN) {
        serverLog(LL_WARNING, "Malformatted or missing node ID in CLUSTER SYNCSLOTS SNAPSHOT from client %lu. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    clusterNode *target_node = clusterLookupNode(c->argv[4]->ptr, CLUSTER_NAMELEN);
    if (!target_node) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS SNAPSHOT from client %lu with an unknown node ID. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    if (strcasecmp(c->argv[5]->ptr, "slotsrange")) {
        serverLog(LL_WARNING, "Missing SLOTSRANGE in CLUSTER SYNCSLOTS SNAPSHOT from node %.40s. Closing link.", target_node->name);
    }
    sds err = NULL;
    clusterNode *source_node;
    list *slot_ranges = parseSlotRanges(c, 6, &source_node, &err);
    if (err) {
        serverLog(LL_WARNING, "Failed to parse slot range provided by CLUSTER SYNCSLOTS SNAPSHOT from node %.40s: %s. Closing link.", target_node->name, err);
        freeClientAsync(c);
        return;
    }
    if (source_node != server.cluster->myself) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS SNAPSHOT from node %.40s, but I am not the owner of the slots. Closing link.", target_node->name);
        freeClientAsync(c);
        return;
    }

    slotExportLink *link = createSlotExportLink(c, target_node->name, slot_ranges);
    listAddNodeTail(server.cluster->slot_export_links, link);
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
    /* No reply, we will proceed with AOF formatted dump once BGSAVE is able to succeed */
}

/* Sent by the target to the source to pause writes to the slot for
 * slot failover. */
void clusterCommandSyncSlotsPause(client *c) {
    if (!c->flag.slot_export_target || !c->slot_export_link) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS PAUSE from client %lu, but the client is not a slot export target. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    slotExportLink *link = (slotExportLink *)c->slot_export_link;
    /* Note that the background save may not have been completed by this point, so we may still be
     * in snapshotting. In this case, we can safely fast-forward to PAUSED.*/
    if (link->state != SLOT_EXPORT_STREAMING && link->state != SLOT_EXPORT_SNAPSHOTTING) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS PAUSE on export to node %.40s, but the client was not streaming incremental updates. Closing link.", link->nodename);
        freeClientAsync(c);
        return;
    }
    if (isAnySlotInManualImportingState() || isAnySlotInManualMigratingState()) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS PAUSE on export to node %.40s, but some slots are being manually migrated. Please get all slots to a stable state and retrigger the import.", link->nodename);
        /* Don't pause, but send the PAUSED message to let the target attempt
         * failover request (and get denied since we aren't paused).*/
    } else {
        serverLog(LL_NOTICE, "Pausing writes to allow slot of slots (%s) export to node %.40s.", link->slot_ranges_str, link->nodename);
        link->mf_end = mstime() + CLUSTER_MF_TIMEOUT * CLUSTER_MF_PAUSE_MULT;
        pauseActions(PAUSE_DURING_SLOT_MIGRATION, link->mf_end, PAUSE_ACTIONS_CLIENT_WRITE_SET);
        updateSlotExportLinkState(link, SLOT_EXPORT_FAILOVER_PAUSED);
    }
    sendSyncSlotsMessageToTarget(c, "PAUSED");

    /* When the slot export is not ready, it will skip adding the client to the
     * pending write queue (creating a backlog of pending commands). If any
     * data is pending there, we need to manually put it in the write queue to
     * flush it. */
    putClientInPendingWriteQueue(c);
}

/* Sent by the target to the source to request final authorization for
 * failover. Authorization could be denied if the source has unpaused itself by
 * now. */
void clusterCommandSyncSlotsRequestFailover(client *c) {
    if (!c->flag.slot_export_target || !c->slot_export_link) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS REQUEST-FAILOVER from client %lu, but the client is not a slot export target. Closing link.", c->id);
        freeClientAsync(c);
        return;
    }
    slotExportLink *link = (slotExportLink *)c->slot_export_link;

    /* Do one last check, since we could have unpaused in the background. */
    checkSlotExportPauseTimeout(link);

    if (link->state != SLOT_EXPORT_FAILOVER_PAUSED) {
        serverLog(LL_WARNING, "Received CLUSTER SYNCSLOTS REQUEST-FAILOVER on export to node %.40s, but we are not paused. Denying failover.", link->nodename);
        sendSyncSlotsMessageToTarget(c, "FAILOVER-DENIED");
        return;
    }

    /* Renew our pause to help ensure we don't unpause before the gossip is
     * propagated. If the existing pause is longer than this, it will be honored */
    mstime_t prop_deadline = mstime() + CLUSTER_OPERATION_TIMEOUT;
    if (link->mf_end < prop_deadline) {
        link->mf_end = prop_deadline;
        pauseActions(PAUSE_DURING_SLOT_MIGRATION, prop_deadline, PAUSE_ACTIONS_CLIENT_WRITE_SET);
    }

    sendSyncSlotsMessageToTarget(c, "FAILOVER-GRANTED");
    updateSlotExportLinkState(link, SLOT_EXPORT_FAILOVER_GRANTED);
}

void sendSyncSlotsMessageToTarget(client *c, const char *subcommand) {
    serverAssert(c && c->flag.slot_export_target && c->slot_export_link);
    addReplyArrayLen(c, 3);
    addReplyBulkCString(c, "CLUSTER");
    addReplyBulkCString(c, "SYNCSLOTS");
    addReplyBulkCString(c, subcommand);
}

slotExportLink *createSlotExportLink(client *c, char *nodename, list *slot_ranges) {
    slotExportLink *link = zcalloc(sizeof(slotExportLink));

    link->last_ack = server.unixtime;
    memcpy(link->nodename, nodename, CLUSTER_NAMELEN);
    link->client = c;
    link->state = SLOT_EXPORT_WAITING_TO_SNAPSHOT;
    link->slot_ranges = slot_ranges;
    link->slot_ranges_str = representSlotRangeList(slot_ranges, '-');

    link->client->flag.slot_export_target = 1;
    link->client->slot_export_link = link;
    initClientReplicationData(link->client);

    serverLog(LL_NOTICE, "Received connection from node %.40s for export of slots (%s).", link->nodename, link->slot_ranges_str);
    return link;
}

int isSlotExportLinkInProgress(slotExportLink *link) {
    return link->state != SLOT_EXPORT_FAILED && link->state != SLOT_EXPORT_FAILOVER_COMPLETE;
}

int shouldRewriteDictionary(int didx, void *privdata) {
    return isSlotInSlotRanges(didx, (list *)privdata);
}

int childSnapshotForSyncSlot(int req, rio *rdb, void *privdata) {
    UNUSED(req);
    int retval = rewriteAppendOnlyFileRio(rdb, shouldRewriteDictionary, privdata);
    rioWrite(rdb, "*3\r\n", 4);
    rioWriteBulkString(rdb, "CLUSTER", 7);
    rioWriteBulkString(rdb, "SYNCSLOTS", 9);
    rioWriteBulkString(rdb, "SNAPSHOT-EOF", 12);
    return retval;
}

// Debug only
const char * slotExportStateToString(slotExportLinkState state) {
    switch (state) {
        case SLOT_EXPORT_WAITING_TO_SNAPSHOT: return "waiting-to-snapshot";
        case SLOT_EXPORT_SNAPSHOTTING: return "snapshotting";
        case SLOT_EXPORT_STREAMING: return "streaming";
        case SLOT_EXPORT_FAILOVER_PAUSED: return "failover-paused";
        case SLOT_EXPORT_FAILOVER_GRANTED: return "failover-granted";
        case SLOT_EXPORT_FAILOVER_COMPLETE: return "failover-complete";
        case SLOT_EXPORT_FAILED: return "failed";
    }
    return "";
}

void updateSlotExportLinkState(slotExportLink *link, slotExportLinkState state) {
    // Debug only
    serverLog(LL_NOTICE, "SLOT EXPORT: %s -> %s", slotExportStateToString(link->state), slotExportStateToString(state));
    link->state = state;
}

void slotExportLinkBeginSnapshot(slotExportLink *link) {
    connection **conns = zmalloc(sizeof(connection *));
    *conns = link->client->conn;
    serverLog(LL_NOTICE, "Beginning snapshot for slot export of slots (%s) to target %.40s.", link->slot_ranges_str, link->nodename);
    if (saveSnapshotToConnectionSockets(conns, 1, 1, 0, childSnapshotForSyncSlot, link->slot_ranges) != C_OK) {
        serverLog(LL_WARNING, "Failed to start slot export of slots (%s) to target %.40s", link->slot_ranges_str, link->nodename);
        updateSlotExportLinkState(link, SLOT_EXPORT_FAILED);
        return;
    }
    updateSlotExportLinkState(link, SLOT_EXPORT_SNAPSHOTTING);
}

int clusterIsSlotExportLinkSnapshotting(void *export) {
    slotExportLink *link = (slotExportLink *)export;
    return link->state == SLOT_EXPORT_SNAPSHOTTING;
}

void clusterHandleSlotExportBackgroundSaveDone(int bgsaveerr) {
    listIter li;
    listNode *ln;
    listRewind(server.cluster->slot_export_links, &li);
    while ((ln = listNext(&li))) {
        slotExportLink *link = (slotExportLink *)ln->value;
        if (link->state != SLOT_EXPORT_SNAPSHOTTING) {
            continue;
        }
        if (bgsaveerr == C_OK) {
            serverLog(LL_NOTICE, "Finished snapshotting slots (%s) to target %.40s, beginning incremental stream...", link->slot_ranges_str, link->nodename);
            updateSlotExportLinkState(link, SLOT_EXPORT_STREAMING);
        } else {
            serverLog(LL_WARNING, "Failed to snapshot slots (%s) to target %.40s", link->slot_ranges_str, link->nodename);
            updateSlotExportLinkState(link, SLOT_EXPORT_FAILED);
        }
        return;
    }
}

void freeSlotExportLink(void *o) {
    slotExportLink *link = o;
    if (link->client) {
        /* Reset slot export data to not trigger clusterHandleSlotExportLinkClientClose. */
        link->client->flag.slot_export_target = 0;
        link->client->slot_export_link = NULL;
        freeClientAsync(link->client);
    }
    link->client = NULL;
    zfree(link->slot_ranges);
    sdsfree(link->slot_ranges_str);
    zfree(o);
}

void initClusterSlotExportLinkList(void) {
    server.cluster->slot_export_links = listCreate();
    listSetFreeMethod(server.cluster->slot_export_links, freeSlotExportLink);
}

int clusterIsSlotExportReadyForReplData(client *c) {
    if (!c->flag.slot_export_target || !c->slot_export_link) {
        return 1;
    }
    slotExportLink *link = (slotExportLink *)c->slot_export_link;
    return link->state != SLOT_EXPORT_SNAPSHOTTING && link->state != SLOT_EXPORT_WAITING_TO_SNAPSHOT;
}

void clusterFeedSlotExportLinks(int dbid, robj **argv, int argc) {
    UNUSED(dbid);
    int i, error_code;
    int slot = -1;
    listIter li;
    listNode *ln;

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
        for (int i = 0; i < argc; i++)
            new_argv[i] = getDecodedObject(argv[i]);
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

    listRewind(server.cluster->slot_export_links, &li);
    while ((ln = listNext(&li))) {
        slotExportLink *export = (slotExportLink *)ln->value;
        if (!export->client) continue;
        if (!isSlotExportLinkInProgress(export)) continue;
        if (!isSlotInSlotRanges(slot, export->slot_ranges)) continue;

        addReplyArrayLen(export->client, argc);
        for (i = 0; i < argc; i++) {
            addReplyBulk(export->client, argv[i]);
        }
    }

    if (new_argv) {
        for (int i = 0; i < argc; i++)
            decrRefCount(new_argv[i]);
        zfree(new_argv);
    }
}

void clusterHandleSlotExportLinkClientClose(void *export) {
    slotExportLink *link = (slotExportLink *)export;
    link->client = NULL;
    serverLog(LL_NOTICE, "Slot export connection to node %.40s (for slots [%s]) lost.", link->nodename, link->slot_ranges_str);
    if (link->state != SLOT_EXPORT_FAILOVER_GRANTED) {
        /* Keep the slot export around so that we remain paused until we find out about the
         * takeover. */
        updateSlotExportLinkState(link, SLOT_EXPORT_FAILED);
    }
    clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_SLOT_MIGRATION);
}

void handleSlotExportPauseTimeout(slotExportLink *link) {
    serverLog(LL_WARNING, "Pause timed out during requested slot failover from %.40s for slot ranges (%s).", link->nodename, link->slot_ranges_str);
    updatePausedActions();
    link->mf_end = 0;
}

void checkSlotExportPauseTimeout(slotExportLink *link) {
    if (link->mf_end < mstime() || !getPausedActionsWithPurpose(PAUSE_DURING_SLOT_MIGRATION)) {
        handleSlotExportPauseTimeout(link);
        updateSlotExportLinkState(link, SLOT_EXPORT_STREAMING);
    }
}

int checkSlotExportOwnershipTransferComplete(slotExportLink *link) {
    int cross_node = 0;
    clusterNode *curr_owner = getClusterNodeBySlotRanges(link->slot_ranges, &cross_node);
    if (curr_owner != server.cluster->myself && !cross_node) {
        /* All slots are now claimed elsewhere, we can unpause ourselves */
        serverLog(LL_NOTICE, "Successfully transferred slot ownership of slots %s (new "
                             "owner: %.40s)",
                  link->slot_ranges_str, curr_owner->name);
        return 1;
    }

    if (!getPausedActionsWithPurpose(PAUSE_DURING_SLOT_MIGRATION)) {
        /* Note that we think this won't happen very commonly. The main source of
         * latency that may trigger an unpause will occur due to the time it takes
         * to process the incremental changes. The final REQUEST-FAILOVER handshake
         * will validate that the source node is still paused after this initial
         * handshake, and renew the pause for an additional amount of time. From this
         * point, we expect the takeover of the slot and gossip to be relatively quick
         * in steady state.
         *
         * Regardless, we log a warning and proceed with cleaning up the export link. */
        serverLog(LL_WARNING, "Write loss risk! During slot export, new owner did not "
                              "broadcast ownership before we unpaused ourselves. Any "
                              "writes we have recorded since unpausing will now be lost!");
        return 1;
    }
    return 0;
}

/* Proceed with a single slot export, until there is nothing left to do at this time. Returns 1 if
 * the slot migration is no longer in progress, or 0 if it is still active. */
void proceedWithSlotExport(slotExportLink *link) {
    switch (link->state) {
    case SLOT_EXPORT_WAITING_TO_SNAPSHOT:
        if (hasActiveChildProcess()) {
            return;
        }
        slotExportLinkBeginSnapshot(link);
        return;
    case SLOT_EXPORT_SNAPSHOTTING:
    case SLOT_EXPORT_STREAMING:
        return;
    case SLOT_EXPORT_FAILOVER_PAUSED:
        checkSlotExportPauseTimeout(link);
        return;
    case SLOT_EXPORT_FAILOVER_GRANTED:
        if (checkSlotExportOwnershipTransferComplete(link)) {
            link->mf_end = 0;
            updateSlotExportLinkState(link, SLOT_EXPORT_FAILOVER_COMPLETE);
            return;
        }
        return;
    case SLOT_EXPORT_FAILOVER_COMPLETE:
    case SLOT_EXPORT_FAILED:
        return;
    }
}

void proceedWithAllSlotExports(void) {
    slotExportLink *link;
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_export_links, &li);
    int paused = 0;
    while ((ln = listNext(&li)) != NULL) {
        link = ln->value;
        if (!isSlotExportLinkInProgress(link)) {
            listDelNode(server.cluster->slot_export_links, ln);
            continue;
        }
        if (server.unixtime - link->last_ack > server.repl_timeout) {
            serverLog(LL_WARNING, "Timing out slot export to node %.40s for slots (%s) after not receiving ack for too long", link->nodename, link->slot_ranges_str);
            listDelNode(server.cluster->slot_export_links, ln);
            continue;
        }
        if (link->client && link->state != SLOT_EXPORT_SNAPSHOTTING) {
            /* Send acks only when the child process isn't writing to it. */
            run_with_period(1000) sendSyncSlotsMessageToTarget(link->client, "ACK");
        }
        proceedWithSlotExport(link);
        if (!isSlotExportLinkInProgress(link)) {
            listDelNode(server.cluster->slot_export_links, ln);
            continue;
        }
        if (link->mf_end) {
            paused++;
        }
    }
    if (!paused && getPausedActionsWithPurpose(PAUSE_DURING_SLOT_MIGRATION)) {
        unpauseActions(PAUSE_DURING_SLOT_MIGRATION);
    }
}

int clusterIsAnySlotExportingViaRepl(void) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slot_export_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotExportLink *link = ln->value;
        if (isSlotExportLinkInProgress(link)) {
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------- TARGET & SOURCE -------------------------------------- */

/* Entrypoint from clusterCron */
void clusterSlotMigrationCron(void) {
    proceedWithAllSlotImports();
    proceedWithAllSlotExports();
}

/* Sent by either the target or the source as a liveness check. */
void clusterCommandSyncSlotsAck(client *c) {
    if (c->flag.slot_export_target && c->slot_export_link) {
        slotExportLink *link = (slotExportLink *)c->slot_export_link;
        link->last_ack = mstime();
    } else if (c->flag.slot_import_source && c->slot_import_link) {
        slotImportLink *link = (slotImportLink *)c->slot_import_link;
        link->last_ack = mstime();
    }
}

/* Sent by either the target or the source as a control message for progressing
 * with slot import. */
void clusterCommandSyncSlots(client *c) {
    if (!strcasecmp(c->argv[2]->ptr, "snapshot") && c->argc > 6 && c->argc % 2 == 0) {
        /* CLUSTER SYNCSLOTS SNAPSHOT TARGET <node-id> SLOTSRANGE <start slot> <end slot> [<start slot> <end slot> ...] */
        clusterCommandSyncSlotsSnapshot(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "snapshot-eof") && c->argc == 3) {
        /* CLUSTER SYNCSLOTS SNAPSHOT-EOF */
        clusterCommandSyncSlotsSnapshotEof(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "pause") && c->argc == 3) {
        /* CLUSTER SYNCSLOTS PAUSE */
        clusterCommandSyncSlotsPause(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "paused") && c->argc == 3) {
        /* CLUSTER SYNCSLOTS PAUSED */
        clusterCommandSyncSlotsPaused(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "request-failover") && c->argc == 3) {
        /* CLUSTER SYNCSLOTS REQUEST-FAILOVER */
        clusterCommandSyncSlotsRequestFailover(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "failover-granted") && c->argc == 3) {
        /* CLUSTER SYNCSLOTS FAILOVER-GRANTED */
        clusterCommandSyncSlotsFailoverGranted(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "failover-denied") && c->argc == 3) {
        /* CLUSTER SYNCSLOTS FAILOVER-DENIED */
        clusterCommandSyncSlotsFailoverDenied(c);
    } else if (!strcasecmp(c->argv[2]->ptr, "ack") && c->argc == 3) {
        /* CLUSTER SYNCSLOTS ACK */
        clusterCommandSyncSlotsAck(c);
    } else {
        addReplyErrorObject(c, shared.syntaxerr);
    }
}
