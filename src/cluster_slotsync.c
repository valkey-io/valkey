#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"

/* -----------------------------------------------------------------------------
 * Cluster functions related to slot range and slot range list.
 * -------------------------------------------------------------------------- */

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

/* Create an empty slot range list. */
list *createSlotRangeList(void) {
    list *slot_ranges = listCreate();
    listSetFreeMethod(slot_ranges, freeSlotRangeValue);
    listSetDupMethod(slot_ranges, dupSlotRangeValue);
    return slot_ranges;
}

/* Represent the given slot range list with the given separator. */
sds representSlotRangeList(list *slot_ranges, const char sep) {
    sds res = sdsempty();
    listNode *ln;
    listIter li;
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        res = sdscatprintf(res, "%d%c%d ", range->start_slot, sep, range->end_slot);
    }
    return res;
}

/* Represent the given slot range list and use a hyphen as separator. */
sds reprSlotRangeListWithHyphen(list *slot_ranges) {
    return representSlotRangeList(slot_ranges, '-');
}

/* Represent the given slot range list and use a blank as separator. */
sds reprSlotRangeListWithBlank(list *slot_ranges) {
    return representSlotRangeList(slot_ranges, ' ');
}

/* Returns 1 if the two given slot range lists are the same, 0 otherwise. */
int isSlotRangeListSame(list *lx, list *ly) {
    if (listLength(lx) != listLength(ly)) {
        return 0;
    }

    int len = listLength(lx);
    if (len == 0) {
        return 1;
    }

    listNode *lnx, *lny;
    listIter lix, liy;
    listRewind(lx, &lix);
    listRewind(ly, &liy);
    while (len--) {
        lnx = listNext(&lix);
        lny = listNext(&liy);
        slotRange *range_x = (slotRange*)lnx->value;
        slotRange *range_y = (slotRange*)lny->value;
        if (range_x->start_slot != range_y->start_slot ||
            range_x->end_slot != range_y->end_slot) {
            return 0;
        }
    }
    return 1;
}

/* Returns 1 if the given slot is in the specified slot ranges, 0 otherwise. */
int isSlotInSlotRangeList(int slot, list *slot_ranges) {
    if (slot < 0) {
        return 0;
    }

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

/* -----------------------------------------------------------------------------
 * Cluster functions related to slot sync link.
 * -------------------------------------------------------------------------- */

/* Free the clusterSlotSyncLink.sync_conn, close the connection fd.
 *
 * Note: If we have created the client, the connection fd is owned by the client
 *       and it will be closed in unlinkClient(). We should make sure the client
 *       is NULL before we close the fd here to avoid double close. */
void freeSlotSyncConn(clusterSlotSyncLink *link) {
    if (!link->client && link->sync_conn) {
        connClose(link->sync_conn);
        link->sync_conn = NULL;
    }
}

void resetSlotSyncLink(clusterSlotSyncLink *link, int reconn) {
    /* If we have created the client, the connection fd is owned by the client,
     * we should not close the fd here. */
    if (link->client == NULL) {
        freeSlotSyncConn(link);
    }

    if (link->transfer_tmpfile_fd > 0) {
        close(link->transfer_tmpfile_fd);
        link->transfer_tmpfile_fd = -1;
    }

    if (link->transfer_tmpfile_name) {
        zfree(link->transfer_tmpfile_name);
        link->transfer_tmpfile_name = NULL;
    }

    if (reconn) {
        /* Set the state to TOCONNECT, so the cron will retry start next time. */
        link->sync_state = CLUSTER_SLOTSYNC_STATE_TOCONNECT;
        link->client = NULL;
        link->transfer_total_size = -1;
        link->transfer_read_size = 0;
        link->transfer_last_fsync_off = 0;
        link->transfer_lastio = server.unixtime;
        link->slot_mf_ready = 0;
        link->slot_mf_end = 0;
        link->slot_mf_lag = 0;
    } else {
        listRelease(link->slot_ranges);
    }
}

void resetSlotSyncLinkForReconnect(clusterSlotSyncLink *link) {
    resetSlotSyncLink(link, 1);
}

void resetSlotSyncLinkForFree(clusterSlotSyncLink *link) {
    resetSlotSyncLink(link, 0);
}

void onSlotSyncClientClose(void *link) {
    resetSlotSyncLinkForReconnect((clusterSlotSyncLink *)link);
}

void freeSlotSyncLink(void *o) {
    clusterSlotSyncLink *link = o;
    resetSlotSyncLinkForFree(link);
    zfree(o);
}

void initClusterSlotSyncLinkList(void) {
    server.cluster->slotsync_links = listCreate();
    listSetFreeMethod(server.cluster->slotsync_links, freeSlotSyncLink);
}

void clearClusterSlotSyncLinkList(void) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterSlotSyncLink *link = ln->value;
        if (link->client) {
            link->client->slotsync_link = NULL;
            freeClient(link->client);
        }
    }
    listEmpty(server.cluster->slotsync_links);
}

sds slotSyncStateToStr(slotSyncState state) {
    const char *desc = NULL;
    switch (state) {
        case CLUSTER_SLOTSYNC_STATE_NONE: desc = "none"; break;
        case CLUSTER_SLOTSYNC_STATE_TOCONNECT: desc = "to_connect"; break;
        case CLUSTER_SLOTSYNC_STATE_CONNECTING: desc = "connecting"; break;
        case CLUSTER_SLOTSYNC_STATE_SEND_AUTH: desc = "send_auth"; break;
        case CLUSTER_SLOTSYNC_STATE_RECV_AUTH: desc = "recv_auth"; break;
        case CLUSTER_SLOTSYNC_STATE_SEND_CAPA: desc = "send_capa"; break;
        case CLUSTER_SLOTSYNC_STATE_RECV_CAPA: desc = "recv_capa"; break;
        case CLUSTER_SLOTSYNC_STATE_WAIT_SCHED: desc = "wait_sched"; break;
        case CLUSTER_SLOTSYNC_STATE_SEND_SYNC: desc = "send_sync"; break;
        case CLUSTER_SLOTSYNC_STATE_RECV_RDB: desc = "recv_rdb"; break;
        case CLUSTER_SLOTSYNC_STATE_CONNECTED: desc = "connected"; break;
        case CLUSTER_SLOTSYNC_STATE_FAILED: desc = "failed"; break;
        default: desc = "unknow";
    }
    return sdsnew(desc);
}

sds representSlotSyncLink(clusterSlotSyncLink *link) {
    sds sync_state = slotSyncStateToStr(link->sync_state);
    sds slot_ranges = reprSlotRangeListWithHyphen(link->slot_ranges);
    sds desc = sdscatprintf(sdsempty(), "id:%.40s node:%.40s state:%s lag:%lld slot:%s",
                            link->linkname, link->nodename, sync_state, link->slot_mf_lag, slot_ranges);
    sdsfree(slot_ranges);
    sdsfree(sync_state);
    return desc;
}

void addReplySlotSyncLinksDescription(client *c) {
    addReplyArrayLen(c, listLength(server.cluster->slotsync_links));

    listNode *ln;
    listIter li;
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterSlotSyncLink *link = ln->value;
        addReplyBulkSds(c, representSlotSyncLink(link));
    }
}

void clusterKillSlotSyncLink(client *c, char *linkid) {
    /* TODO: supply in next commit */
    UNUSED(linkid);
    addReplyError(c, "The kill action is not supported");
}
