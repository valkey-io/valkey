#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"
#include "bio.h"
#include "module.h"

/* The following functions are declared here as they will be used in this file
 * but they are defined in other files. */
char *sendCommand(connection *conn, ...);
char *receiveSynchronousResponse(connection *conn);
int useDisklessLoad(void);
void delkeysNotOwnedByMySelf(list *slot_ranges);
void clusterUpdateState(void);
void clusterSaveConfigOrDie(int do_fsync);
void clusterCloseAllSlots(void);
int clusterDelSlot(int slot);
int clusterAddSlot(clusterNode *n, int slot);
int clusterBumpConfigEpochWithoutConsensus(void);
unsigned int delKeysInSlotWithTimeLimit(unsigned int hashslot, ustime_t *limit);

/* The following functions are declared here as they will be used by others
 * before the definition, we will define them in this file later. */
void setSlotSyncImporting(list *slot_ranges, clusterNode *node);
void clearSlotSyncImporting(list *slot_ranges);
void syncWithSlotOwner(connection *conn);
void continueSlotSync(clusterSlotSyncLink *link);
void readSlotSyncBulkPayload(connection *conn);
clusterNode *getClusterNodeBySlotList(list *slot_ranges, int *cross_node);
void notifyClientsCloseSlotSyncLink(void);
void clusterDoBeforeSleep(int flags);

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
sds representSlotRangeList(list *slot_ranges, const char separator) {
    sds res = sdsempty();
    listNode *ln;
    listIter li;
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        res = sdscatprintf(res, "%d%c%d ", range->start_slot, separator, range->end_slot);
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
    if (lx == ly) return 1;
    if (lx == NULL || ly == NULL) return 0;
    if (listLength(lx) != listLength(ly)) return 0;

    int len = listLength(lx);
    if (len == 0) return 1;

    listNode *lnx, *lny;
    listIter lix, liy;
    listRewind(lx, &lix);
    listRewind(ly, &liy);
    while (len--) {
        lnx = listNext(&lix);
        lny = listNext(&liy);
        slotRange *range_x = (slotRange*)lnx->value;
        slotRange *range_y = (slotRange*)lny->value;
        if (range_x->start_slot != range_y->start_slot || range_x->end_slot != range_y->end_slot) {
            return 0;
        }
    }
    return 1;
}

/* Returns 1 if the given slot is in the specified slot ranges, 0 otherwise. */
int isSlotInSlotRangeList(int slot, list *slot_ranges) {
    if (slot < 0) {
        // todo check when slot will < 0
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

/* This function is called by rdbSaveRio() when we need to generate a RDB file
 * which only include the keys in the specified slots for slot sync usage.
 *
 * Returns 1 if the given key is in the specified slot ranges, 0 otherwise. */
int isKeyInSlotRanges(robj *key, list *slot_ranges) {
    // maybe this can optimize.
    /* Get the slot of this key and check if the slot in the specified range. */
    int slot = keyHashSlot((char*)key->ptr, sdslen(key->ptr));
    return isSlotInSlotRangeList(slot, slot_ranges);
}

/* This function is called by replicationFeedReplicas if some of the replicas are
 * in slot sync mode. In such a case, we should only feed it the commands which
 * associated with the specified slots.
 *
 * Returns 1 if all the keys in the command are belongs to the same slot and the
 * slot is in the specified slot ranges, 0 otherwise. */
int isCommandInSlotRanges(int argc, robj **argv, list *slot_ranges) {
    struct serverCommand *cmd = lookupCommand(argv, argc);
    if (!cmd) return 0; /* In case of the argv is from DEBUG REPLICATE. */

    /* This function may be called after the command is executed.
     * At this time, the arg in argv may be rewritten and the encoding
     * may be an INT. In this case, we need to decode it into a string
     * object because in getKeysFromCommand, all the arg is a string. */
    robj **new_argv = NULL;
    for (int i = 0; i < argc; i++) {
        if (!sdsEncodedObject(argv[i])) {
            new_argv = zmalloc(sizeof(robj*) * (argc));
            break;
        }
    }
    if (new_argv) {
        for (int i = 0; i < argc; i++)
            new_argv[i] = getDecodedObject(argv[i]);
    }

    /* Extract all the keys from the command. */
    getKeysResult result;
    initGetKeysResult(&result);
    int numkeys = getKeysFromCommand(cmd, new_argv ? new_argv : argv, argc, &result);

    /* Free the new argv. */
    if (new_argv) {
        for (int i = 0; i < argc; i++)
            decrRefCount(new_argv[i]);
        zfree(new_argv);
    }

    /* If slot_ranges is NULL, that is a debug path. */
    if (slot_ranges == NULL) return 0;

    /* Check if all the keys are in the same slot and get this slot. */
    robj *firstkey = NULL;
    keyReference *keyindex = result.keys;
    int slot = -1;
    for (int j = 0; j < numkeys; j++) {
        robj *thiskey = argv[keyindex[j].pos];
        int thisslot = keyHashSlot((char*)thiskey->ptr, sdslen(thiskey->ptr));

        if (firstkey == NULL) {
            firstkey = thiskey;
            slot = thisslot;
        } else {
            if (slot != thisslot) {
                getKeysFreeResult(&result);
                serverLog(LL_WARNING, "Cross slot '%s' '%s' ", (char*)(argv[0]->ptr), (char*)(argv[j]->ptr));
                return 0;
            }
        }
    }
    getKeysFreeResult(&result);

    /* Check if the slot in the specified range. */
    return isSlotInSlotRangeList(slot, slot_ranges);
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to slot sync link.
 * -------------------------------------------------------------------------- */

clusterSlotSyncLink *createSlotSyncLink(void) {
    clusterSlotSyncLink *link = zmalloc(sizeof(clusterSlotSyncLink));
    return link;
}

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

void initSlotSyncLink(clusterSlotSyncLink *link, clusterNode *node, list *slot_ranges) {
    link->ctime = mstime();
    strncpy(link->nodename, node->name, CLUSTER_NAMELEN);
    getRandomHexChars(link->linkname, sizeof(link->linkname));

    link->client = NULL;
    link->transfer_total_size = -1;
    link->transfer_read_size = 0;
    link->transfer_last_fsync_off = 0;
    link->transfer_lastio = server.unixtime;
    link->transfer_tmpfile_fd = -1;
    link->transfer_tmpfile_name = NULL;
    if (slot_ranges) link->slot_ranges = slot_ranges;
    link->slot_mf_ready = 0;
    link->slot_mf_end = 0;
    link->slot_mf_lag = 0;

    link->sync_state = CLUSTER_SLOTSYNC_STATE_CONNECTING;
    link->sync_conn = connCreate(connTypeOfCluster());
    if (connConnect(link->sync_conn, node->ip, getNodeDefaultClientPort(node), server.bind_source_addr,
                    syncWithSlotOwner) == C_ERR) {
        serverLog(LL_WARNING, "Unable to connect to slot owner %.40s (%s): %s", node->name, node->human_nodename,
                  connGetLastError(link->sync_conn));
        freeSlotSyncConn(link);
        return;
    }

    connSetReadHandler(link->sync_conn, syncWithSlotOwner);
    connSetPrivateData(link->sync_conn, link);
    setSlotSyncImporting(link->slot_ranges, node);
    serverLog(LL_NOTICE, "Init slot sync link %.40s from node %.40s (%s).", link->linkname, node->name,
              node->human_nodename);
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
        link->sync_conn = NULL;
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

int isSlotInClusterSlotSyncLinkList(int slot) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterSlotSyncLink *link = ln->value;
        if (isSlotInSlotRangeList(slot, link->slot_ranges)) {
            return 1;
        }
    }
    return 0;
}

const char *slotSyncStateToString(slotSyncState state) {
    switch (state) {
    case CLUSTER_SLOTSYNC_STATE_NONE: return "none";
    case CLUSTER_SLOTSYNC_STATE_TOCONNECT: return "to_connect";
    case CLUSTER_SLOTSYNC_STATE_CONNECTING: return "connecting";
    case CLUSTER_SLOTSYNC_STATE_SEND_AUTH: return "send_auth";
    case CLUSTER_SLOTSYNC_STATE_RECV_AUTH: return "recv_auth";
    case CLUSTER_SLOTSYNC_STATE_SEND_CAPA: return "send_capa";
    case CLUSTER_SLOTSYNC_STATE_RECV_CAPA: return "recv_capa";
    case CLUSTER_SLOTSYNC_STATE_WAIT_SCHED: return "wait_sched";
    case CLUSTER_SLOTSYNC_STATE_SEND_SYNC: return "send_sync";
    case CLUSTER_SLOTSYNC_STATE_RECV_RDB: return "recv_rdb";
    case CLUSTER_SLOTSYNC_STATE_LOADING_RDB: return "loading_rdb";
    case CLUSTER_SLOTSYNC_STATE_DONE_LOADING: return "done_loading";
    case CLUSTER_SLOTSYNC_STATE_CONNECTED: return "connected";
    case CLUSTER_SLOTSYNC_STATE_FAILED: return "failed";
    default: serverPanic("Unknown slot sync state.");
    }
}

sds representSlotSyncLink(clusterSlotSyncLink *link) {
    // todo use dict?
    sds slot_ranges = reprSlotRangeListWithHyphen(link->slot_ranges);
    sds desc = sdscatprintf(sdsempty(), "id:%.40s node:%.40s state:%s lag:%lld slot:%s",
                            link->linkname, link->nodename, slotSyncStateToString(link->sync_state), link->slot_mf_lag,
                            slot_ranges);
    sdsfree(slot_ranges);
    return desc;
}

/* See representSlotSyncLink for the format. */
void clusterCommandSlotLinkList(client *c) {
    addReplyArrayLen(c, listLength(server.cluster->slotsync_links));

    listNode *ln;
    listIter li;
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterSlotSyncLink *link = ln->value;
        addReplyBulkSds(c, representSlotSyncLink(link));
    }
}

void clusterCommandSlotLinkKill(client *c, const char *linkname) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterSlotSyncLink *link = ln->value;

        if (!strncmp(link->linkname, linkname, CLUSTER_NAMELEN)) {
            /* Remove keys not owned by myself. */
            // todo unlink?
            delkeysNotOwnedByMySelf(link->slot_ranges);

            /* Clear all importing state for this link. */
            clearSlotSyncImporting(link->slot_ranges);

            /* Free this link and the client which bound with it. */
            if (link->client) {
                link->client->slotsync_link = NULL;
                freeClientAsync(link->client);
            }
            listDelNode(server.cluster->slotsync_links, ln);

            /* Update state and save config. */
            clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
            addReply(c, shared.ok);
            return;
        }
    }

    addReplyError(c, "There's no such link.");
}

int clusterGetSlotSyncLinkRank(clusterSlotSyncLink *in) {
    int rank = 0;

    listNode *ln;
    listIter li;
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterSlotSyncLink *link = ln->value;

        /* Skip connected links. */
        if (link->sync_state == CLUSTER_SLOTSYNC_STATE_CONNECTED) {
            continue;
        }

        /* Skip the input link itself. */
        if (link == in) {
            continue;
        }

        /* Current link is in progress, the input link should rank after it. */
        if (link->sync_state > CLUSTER_SLOTSYNC_STATE_WAIT_SCHED &&
            link->sync_state < CLUSTER_SLOTSYNC_STATE_CONNECTED) {
            rank++;
            continue;
        }

        /* Use linkname to sort the remaining links. */
        if (memcmp(link->linkname, in->linkname, CLUSTER_NAMELEN) < 0) {
            rank++;
        }
    }

    return rank;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to slot sync importing.
 * -------------------------------------------------------------------------- */

void setSlotSyncImporting(list *slot_ranges, clusterNode *node) {
    listNode *ln;
    listIter li;
    listRewind(slot_ranges, &li);
    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        for (int i = range->start_slot; i <= range->end_slot; i++){
            server.cluster->importing_slots_from[i] = node;
        }
    }
}

void clearSlotSyncImporting(list *slot_ranges) {
    setSlotSyncImporting(slot_ranges, NULL);
}

sds formatSlotSyncImportingSlots(void) {
    sds ci = sdsempty();
    int start = -1;
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterSlotSyncLink *link = ln->value;
        char *link_node = link->nodename;
        char *importing_node = NULL;
        for (int j = 0; j < CLUSTER_SLOTS; j++) {
            int bit = 0;
            if (server.cluster->importing_slots_from[j]) {
                importing_node = server.cluster->importing_slots_from[j]->name;
                if (strncmp(importing_node, link_node, CLUSTER_NAMELEN) == 0) {
                    bit =1;
                }
            }

            if (bit && start == -1) {
                start = j;
            }

            if (start != -1 && (!bit || j == CLUSTER_SLOTS-1)) {
                if (bit && j == CLUSTER_SLOTS-1) j++;

                if (start == j-1) {
                    ci = sdscatprintf(ci," [%d-<-%.40s]", start, server.cluster->importing_slots_from[start]->name);
                } else {
                    ci = sdscatprintf(ci," [%d-%d<-%.40s]", start, j-1, server.cluster->importing_slots_from[start]->name);
                }
                start = -1;
            }
        }
    }
    return ci;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to slot sync handshake and rdb transfer.
 * -------------------------------------------------------------------------- */

void syncWithSlotOwner(connection *conn) {
    clusterSlotSyncLink *link = connGetPrivateData(conn);
    char *err = NULL;

    /* Simulate IO error. */
    if (testInjectError("crs-io-error-beforce-slot-sync")) {
        serverLog(LL_WARNING, "inject crs-io-error-beforce-slot-sync");
        goto error;
    }

    /* Check for errors in the socket: after a non blocking connect() we
     * may find that the socket is in error state. */
    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING, "Error condition on socket for slot sync: %s", connGetLastError(conn));
        goto error;
    }

    /* CLUSTER_SLOTSYNC_STATE: CONNECTING ==> SEND_AUTH|SEND_CAPA
     *
     * Set the read/write event handler. */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_CONNECTING) {
        serverLog(LL_NOTICE, "Non blocking connect for slot sync fired the event.");
        connSetWriteHandler(conn, NULL);
        connSetReadHandler(conn, syncWithSlotOwner);
        if (server.primary_auth) {
            link->sync_state = CLUSTER_SLOTSYNC_STATE_SEND_AUTH;
        } else {
            link->sync_state = CLUSTER_SLOTSYNC_STATE_SEND_CAPA;
        }
    }

    /* Simulate IO error. */
    if (testInjectError("crs-io-error-send-auth")) {
        serverLog(LL_WARNING, "inject crs-io-error-send-auth");
        goto error;
    }

    /* CLUSTER_SLOTSYNC_STATE: SEND_AUTH ==> RECV_AUTH
     *
     * AUTH with the slot owner if needed. */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_SEND_AUTH) {
        err = sendCommand(conn, "AUTH", server.primary_auth, NULL);
        if (err) goto write_error;
        link->sync_state = CLUSTER_SLOTSYNC_STATE_RECV_AUTH;
        return;
    }

    /* CLUSTER_SLOTSYNC_STATE: RECV_AUTH ==> SEND_CAPA
     *
     * Receive AUTH reply. */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_RECV_AUTH) {
        err = receiveSynchronousResponse(conn);
        if (err == NULL) goto no_response_error;
        if (err[0] == '-') {
            serverLog(LL_WARNING, "Unable to AUTH to slot owner: %s", err);
            sdsfree(err);
            err = NULL;
            connClose(conn);
            link->sync_state = CLUSTER_SLOTSYNC_STATE_FAILED;
            return;
        }
        sdsfree(err);
        err = NULL;
        link->sync_state = CLUSTER_SLOTSYNC_STATE_SEND_CAPA;
    }

    // todo may need to confirm whether the slot owner supports slot migration.
    // like support SYNC xxx xxx
    /* CLUSTER_SLOTSYNC_STATE: SEND_CAPA ==> RECV_CAPA
     *
     * Inform the slot owner of our capabilities. */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_SEND_CAPA) {
        sds portstr = getReplicaPortString();
        err = sendCommand(conn, "REPLCONF", "capa", "eof", "listening-port", portstr, NULL);
        sdsfree(portstr);
        if (err) goto write_error;
        sdsfree(err);
        err = NULL;
        link->sync_state = CLUSTER_SLOTSYNC_STATE_RECV_CAPA;
        return;
    }

    /* CLUSTER_SLOTSYNC_STATE: RECV_CAPA ==> WAIT_SCHED
     *
     * Receive CAPA reply. */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_RECV_CAPA) {
        err = receiveSynchronousResponse(conn);
        if (err == NULL) goto no_response_error;
        /* Ignore the error if any, not all the versions support REPLCONF capa. */
        if (err[0] == '-') {
            serverLog(LL_NOTICE, "(Non critical) Slot owner does not understand REPLCONF capa: %s", err);
        }
        sdsfree(err);
        err = NULL;
        link->sync_state = CLUSTER_SLOTSYNC_STATE_WAIT_SCHED;
    }

    /* CLUSTER_SLOTSYNC_STATE: WAIT_SCHED ==> SEND_SYNC
     *
     * Wait other slotsync links. */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_WAIT_SCHED) {
        if (clusterGetSlotSyncLinkRank(link) == 0) {
            link->sync_state = CLUSTER_SLOTSYNC_STATE_SEND_SYNC;
        } else {
            return;
        }
    }
    continueSlotSync(link);
    return;

no_response_error: /* Handle receiveSynchronousResponse() error when slot owner has no reply. */
    serverLog(LL_WARNING, "Slot owner did not respond to command during slotsync handshake.");
    /* Fall through to regular error handling */

error:
    freeSlotSyncConn(link);

    /* Set the state to TOCONNECT, so the cron will retry start next time. */
    link->sync_state = CLUSTER_SLOTSYNC_STATE_TOCONNECT;
    return;

write_error: /* Handle sendCommand() errors. */
    serverLog(LL_WARNING,"Sending command to target handshake: %s", err);
    sdsfree(err);
    err = NULL;
    goto error;
}

void continueSlotSync(clusterSlotSyncLink *link) {
    char tmpfile[256];
    int tmpfd = -1;
    int maxtries = 5;

    /* CLUSTER_SLOTSYNC_STATE: SEND_SYNC ==> RECV_RDB
     *
     * Send the special SYNC command to the slots owner. */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_SEND_SYNC) {
        sds sync_cmd = sdscatprintf(sdsempty(), "SYNC ");
        sds slot_ranges = reprSlotRangeListWithBlank(link->slot_ranges);
        sync_cmd = sdscatsds(sync_cmd, slot_ranges);
        sync_cmd = sdscatprintf(sync_cmd, "\r\n");

        /* Simulate IO error. */
        if (testInjectError("crs-io-error-beforce-send-sync")) {
            serverLog(LL_WARNING, "inject crs-io-error-beforce-send-sync");
            sdsfree(slot_ranges);
            sdsfree(sync_cmd);
            goto error;
        }

        if (connSyncWrite(link->sync_conn, sync_cmd, sdslen(sync_cmd), server.repl_syncio_timeout * 1000) == -1) {
            serverLog(LL_WARNING, "I/O error writing to slot owner: %s", connGetLastError(link->sync_conn));
            sdsfree(slot_ranges);
            sdsfree(sync_cmd);
            goto error;
        }
        sdsfree(slot_ranges);
        sdsfree(sync_cmd);
        link->sync_state = CLUSTER_SLOTSYNC_STATE_RECV_RDB;
    }

    /* Prepare a suitable temp file for slot rdb transfer. */
    while (maxtries--) {
        snprintf(tmpfile,256, "temp-%d.%ld.rdb", (int)server.unixtime, (long int)getpid());
        tmpfd = open(tmpfile, O_CREAT | O_WRONLY | O_EXCL, 0644);
        if (tmpfd != -1) break;
        sleep(1);
    }
    if (tmpfd == -1) {
        serverLog(LL_WARNING, "Opening the temp file needed for slot synchronization: %s", strerror(errno));
        goto error;
    }

    /* Change the read event handler from syncWithSlotOwner() to readSlotSyncBulkPayload(). */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_RECV_RDB) {
        if (connSetReadHandler(link->sync_conn, readSlotSyncBulkPayload) == C_ERR) {
            char conninfo[CONN_INFO_LEN];
            serverLog(LL_WARNING, "Can't create readable event for slot sync: %s (%s)", strerror(errno),
                      connGetInfo(link->sync_conn, conninfo, sizeof(conninfo)));
            goto error;
        }
    }

    /* Store the name and fd of the rdb file to the clusterSlotSyncLink. */
    link->transfer_tmpfile_fd = tmpfd;
    link->transfer_tmpfile_name = zstrdup(tmpfile);
    return;

error:
    if (tmpfd != -1) close(tmpfd);
    connClose(link->sync_conn);

    /* Set the state to TOCONNECT, so the cron will retry start next time. */
    link->sync_state = CLUSTER_SLOTSYNC_STATE_TOCONNECT;
}

#define SLOTSYNC_MAX_WRITTEN_BEFORE_FSYNC (8<<20)  /* 8MB */
void readSlotSyncBulkPayload(connection *conn) {
    clusterSlotSyncLink *link = connGetPrivateData(conn);
    char buf[PROTO_IOBUF_LEN];
    ssize_t nread, readlen, nwritten;
    int use_diskless_load = useDisklessLoad();
    off_t left;

    if (server.loading == 1) {
        serverLog(LL_NOTICE, "Waiting prev loading finish");
        return;
    }

    /* Static vars used to hold the EOF mark, and the last bytes received
     * from the server: when they match, we reached the end of the transfer. */
    static char eofmark[RDB_EOF_MARK_SIZE];
    static char lastbytes[RDB_EOF_MARK_SIZE];
    static int usemark = 0;

    /* If transfer_total_size == -1, we still have to read the bulk length
     * from the slot owner reply. */
    if (link->transfer_total_size == -1) {
        if (connSyncReadLine(conn, buf, 1024, server.repl_syncio_timeout * 1000) == -1) {
            serverLog(LL_WARNING, "I/O error reading bulk count from slot owner: %s", connGetLastError(conn));
            goto error;
        }

        if (buf[0] == '-') {
            serverLog(LL_WARNING, "Slot owner aborted replication with an error: %s", buf + 1);
            goto error;
        } else if (buf[0] == '\0') {
            /* At this stage just a newline works as a PING in order to take
             * the connection live. So we refresh our last interaction
             * timestamp. */
            link->transfer_lastio = server.unixtime;
            return;
        } else if (buf[0] != '$') {
            serverLog(LL_WARNING,"Bad protocol from slot owner, the first byte is not '$' "
                                 "(we received '%s'), are you sure the host and port are right?", buf);
            goto error;
        }

        /* There are two possible forms for the bulk payload. One is the
         * usual $<count> bulk format. The other is used for diskless transfers
         * when the master does not know beforehand the size of the file to
         * transfer. In the latter case, the following format is used:
         *
         * $EOF:<40 bytes delimiter>
         *
         * At the end of the file the announced delimiter is transmitted. The
         * delimiter is long and random enough that the probability of a
         * collision with the actual file content can be ignored. */
        if (strncmp(buf+1, "EOF:", 4) == 0 && strlen(buf + 5) >= RDB_EOF_MARK_SIZE) {
            usemark = 1;
            memcpy(eofmark, buf + 5, RDB_EOF_MARK_SIZE);
            memset(lastbytes, 0, RDB_EOF_MARK_SIZE);
            /* Set any transfer_total_size to avoid entering this code path
             * at the next call. */
            link->transfer_total_size = 0;
            serverLog(LL_NOTICE, "Cluster slot sync: receiving streamed RDB from slot owner with EOF %s",
                      use_diskless_load ? "to parser": "to disk");
        } else {
            usemark = 0;
            link->transfer_total_size = strtol(buf+1,NULL,10);
            serverLog(LL_NOTICE, "Cluster slot sync: receiving %lld bytes from slot owner %s",
                      (long long)link->transfer_total_size, use_diskless_load ? "to parser" : "to disk");
        }
        return;
    }

    if (!use_diskless_load) {
        /* Read the data from the socket, store it to a file and search
         * for the EOF. */
        if (usemark) {
            readlen = sizeof(buf);
        } else {
            left = link->transfer_total_size - link->transfer_read_size;
            readlen = (left < (signed)sizeof(buf)) ? left : (signed)sizeof(buf);
        }

        if (testInjectError("crs-io-error-recv-rdb")) {
            serverLog(LL_WARNING, "inject crs-io-error-recv-rdb");
            goto error;
        }

        nread = connRead(conn,buf,readlen);
        if (nread <= 0) {
            if (connGetState(conn) == CONN_STATE_CONNECTED) {
                /* equivalent to EAGAIN */
                return;
            }
            serverLog(LL_WARNING, "I/O error trying to sync with slot owner: %s",
                      (nread == -1) ? connGetLastError(conn) : "connection lost");
            goto error;
        }
        // check stat_net_repl_input_bytes?
        server.stat_net_input_bytes += nread;

        /* When a mark is used, we want to detect EOF asap in order to avoid
         * writing the EOF mark into the file... */
        int eof_reached = 0;

        if (usemark) {
            /* Update the last bytes array, and check if it matches our
             * delimiter. */
            if (nread >= RDB_EOF_MARK_SIZE) {
                memcpy(lastbytes, buf + nread - RDB_EOF_MARK_SIZE, RDB_EOF_MARK_SIZE);
            } else {
                int rem = RDB_EOF_MARK_SIZE - nread;
                memmove(lastbytes, lastbytes + nread, rem);
                memcpy(lastbytes + rem, buf, nread);
            }
            if (memcmp(lastbytes, eofmark, RDB_EOF_MARK_SIZE) == 0)
                eof_reached = 1;
        }

        if (testInjectError("crs-io-error-write-rdb")) {
            serverLog(LL_WARNING, "inject crs-io-error-write-rdb");
            goto error;
        }

        /* Update the last I/O time for the replication transfer (used in
         * order to detect timeouts during replication), and write what we
         * got from the socket to the dump file on disk. */
        link->transfer_lastio = server.unixtime;
        if ((nwritten = write(link->transfer_tmpfile_fd,buf,nread)) != nread) {
            serverLog(LL_WARNING,
                      "Write error or short write writing to the DB dump file "
                      "needed for cluster slot synchronization: %s",
                      (nwritten == -1) ? strerror(errno) : "short write");
            goto error;
        }
        link->transfer_read_size += nread;

        if (testInjectError("crs-io-error-truncate-rdb")) {
            serverLog(LL_WARNING, "inject crs-io-error-truncate-rdb");
            goto error;
        }

        /* Delete the last 40 bytes from the file if we reached EOF. */
        if (usemark && eof_reached) {
            if (ftruncate(link->transfer_tmpfile_fd, link->transfer_read_size - RDB_EOF_MARK_SIZE) == -1) {
                serverLog(LL_WARNING,
                          "Error truncating the RDB file received from the slot owner "
                          "for SYNC: %s", strerror(errno));
                goto error;
            }
        }

        /* Sync data on disk from time to time, otherwise at the end of the
         * transfer we may suffer a big delay as the memory buffers are copied
         * into the actual disk. */
        if (link->transfer_read_size >= link->transfer_last_fsync_off + SLOTSYNC_MAX_WRITTEN_BEFORE_FSYNC) {
            off_t sync_size = link->transfer_read_size - link->transfer_last_fsync_off;
            rdb_fsync_range(link->transfer_tmpfile_fd, link->transfer_last_fsync_off, sync_size);
            link->transfer_last_fsync_off += sync_size;
        }

        /* Check if the transfer is now complete */
        if (!usemark) {
            if (link->transfer_read_size == link->transfer_total_size) eof_reached = 1;
        }

        /* If the transfer is yet not complete, we need to read more, so
         * return ASAP and wait for the handler to be called again. */
        if (!eof_reached) return;
    }

    /* We reach this point in one of the following cases:
     *
     * 1. The replica is using diskless replication, that is, it reads data
     *    directly from the socket to the Redis memory, without using
     *    a temporary RDB file on disk. In that case we just block and
     *    read everything from the socket.
     *
     * 2. Or when we are done reading from the socket to the RDB file, in
     *    such case we want just to read the RDB file in memory. */

    /* We need to stop any AOF rewriting child before flusing and parsing
     * the RDB, otherwise we'll create a copy-on-write disaster. */
    if (server.aof_state != AOF_OFF) stopAppendOnly();

    /* Before loading the DB into memory we need to delete the readable
     * handler, otherwise it will get called recursively since
     * rdbLoad() will call the event loop to process events from time to
     * time for non blocking loading.
     *
     * And we are also using a bio to do the load, we need to make sure
     * we won't enter this function again, make sure the connection won't
     * be closed during the loading. */
    connSetReadHandler(conn, NULL);

    link->sync_state = CLUSTER_SLOTSYNC_STATE_RECV_RDB;

    serverLog(LL_NOTICE, "Cluster slot sync: Loading slot DB in memory");

    /* Doing a bio RDB loading */
    rdbLoadJob *job = zmalloc(sizeof(rdbLoadJob));
    job->rdbflags = RDBFLAGS_REPLICATION | RDBFLAGS_SLOT_SYNC;
    job->use_diskless_load = use_diskless_load;
    job->usemark = usemark;
    job->eofmark = sdsnew(eofmark);
    job->link = link;
    bioCreateRdbLoadJob(bioRdbLoad, 1, job);

    return;

error:
    /* Reset the link state to TOCONNECT, the cron will retry start next time. */
    resetSlotSyncLinkForReconnect(link);
    return;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to slot sync messages exchange.
 * -------------------------------------------------------------------------- */

void slotLinkSendMessage(client *c, const char *option, long long value) {
    if (!server.cluster_enabled) return;
    if (!c) return;
    if (!c->flag.slot_sync_primary || c->flag.slot_sync_replica) return;

    struct ClientFlags old_flags = c->flag;

    c->flag.reply_off = 0;
    c->flag.primary_force_reply = 1;
    addReplyArrayLen(c, 3);
    addReplyBulkCString(c, "REPLCONF");
    addReplyBulkCString(c, option);
    addReplyBulkLongLong(c, value);
    c->flag.primary_force_reply = 0;
    c->flag.reply_off = old_flags.reply_off;
}

/* Replica --> Primary: REPLCONF SLOTONLINE <recv_bytes>
 *
 * Replica (slotsync client) send this message to inform the primary (slotsync server)
 * the amount of replication stream that it has processed so far in incremental
 * propagation stage. */
void slotLinkSendOnline(client* c) {
    slotLinkSendMessage(c, "SLOTONLINE", c->slotsync_recv_bytes);
}

/* Replica --> Primary: REPLCONF SLOTFAILOVER 0
 *
 * Replica (slotsync client) send this message to inform the primary (slotsync server)
 * to pause clients for slot failover. */
void slotLinkSendFailover(client* c) {
    slotLinkSendMessage(c, "SLOTFAILOVER", 0);
}

/* Replica --> Primary: REPLCONF SLOTACK <recv_bytes>
 *
 * Replica (slotsync client) send this message to inform the primary (slotsync server)
 * the amount of replication stream that it has processed so far in slot failover
 * stage. */
void slotLinkSendAck(client* c) {
    slotLinkSendMessage(c, "SLOTACK", c->slotsync_recv_bytes);
}

void replyToSlotSyncReplica(client* c, sds reply) {
    if (!c) return;

    c->slotsync_sent_bytes += sdslen(reply);
    addReplySds(c,reply);  /* The sds 'reply' will be freed in addReplySds(). */
}

/* Primary --> Replica: REPLCONF SLOTDIFF <diff_bytes>
 *
 * Primary (slotsync server) send this message to inform the replica (slotsync client)
 * the replication stream lag. */
void replySlotOffsetToReplica(client* c, long long offset) {
    sds soffset = sdscatprintf(sdsempty(), "%llu", offset);
    sds reply = sdscatprintf(sdsempty(), "*3\r\n$8\r\nREPLCONF\r\n$8\r\nSLOTDIFF\r\n$%lu\r\n%s\r\n",
                             sdslen(soffset), soffset);
    sdsfree(soffset);
    replyToSlotSyncReplica(c, reply);
}

/* Primary --> Replica: REPLCONF SLOTREADY 0
 *
 * Primary (slotsync server) send this message to inform the replica (slotsync client)
 * the replication stream lag became zero and is ready for the replica to takeover
 * the slots now. */
void replySlotReadyToReplica(client* c) {
    sds reply = sdscatprintf(sdsempty(), "*3\r\n$8\r\nREPLCONF\r\n$9\r\nSLOTREADY\r\n$1\r\n0\r\n");
    replyToSlotSyncReplica(c, reply);
}

/* Primary --> Replica: CLUSTER INTERNALCLOSESLOTLINK
 *
 * Primary (slotsync server) send this message to inform the replica (slotsync client)
 * to close the slotsync link that bound with this client. */
void replyCloseSlotLinkToReplica(client* c) {
    sds reply = sdscatprintf(sdsempty(), "*2\r\n$7\r\nCLUSTER\r\n$21\r\nINTERNALCLOSESLOTLINK\r\n");
    replyToSlotSyncReplica(c, reply);
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to slot failover.
 * -------------------------------------------------------------------------- */

/* Initialize the state of a new slot failover at the 'client' side and send the
 * REPLCONF SLOTFAILOVER to the 'server' side.
 *
 * Note: 'client' here means the promoter of the slot failover and 'server' here
 *       means the original owner of the slots. */
void clusterInitSlotFailover(void) {
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterSlotSyncLink *link = ln->value;
        link->slot_mf_ready = 0;
        link->slot_mf_end = mstime() + CLUSTER_MF_TIMEOUT;
        slotLinkSendFailover(link->client);
    }
    server.cluster->slot_mf_end = mstime() + CLUSTER_MF_TIMEOUT;
    char *val = getInjectOptionValue("crs-cluster-mf-timeout");
    if (val) {
        server.cluster->slot_mf_end = mstime() + atoi(val);
        zfree(val);
        serverLog(LL_WARNING, "crs-cluster-mf-timeout: %d", atoi(val));
    }
    serverLog(LL_NOTICE, "Slot failover is initialized. slot_mf_end=%lld", server.cluster->slot_mf_end);
}

/* This function implements the final part of manual slot failovers,
 * where the replica grabs all the slotsync link's hash slots, and
 * propagates the new configuration.
 *
 * Note that it's up to the caller to be sure that the node got a new
 * configuration epoch already. */
void clusterSlotFailoverReplace(void) {
    /* If the cluster is failed, can not do slot failover. */
    if (server.cluster->state == CLUSTER_FAIL) {
        serverLog(LL_WARNING, "Cluster is down, can not do slot failover.");
        return;
    }

    /* 1) Clear the importing state for all the slots. */
    clusterCloseAllSlots();

    /* 2) Claim all the slots in the slotsync links to myself. */
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterSlotSyncLink *link = ln->value;

        listNode *ln2;
        listIter li2;
        listRewind(link->slot_ranges, &li2);
        while ((ln2 = listNext(&li2)) != NULL) {
            slotRange *range = ln2->value;
            for (int i = range->start_slot; i<= range->end_slot; i++) {
                clusterDelSlot(i);
                clusterAddSlot(server.cluster->myself,i);
            }
        }
    }

    /* 3) Update state and save config. */
    clusterUpdateState();
    clusterSaveConfigOrDie(1);

    /* 4) Pong all the other nodes so that they can update the state accordingly
     *    and detect that we switched to master role. */
    clusterBroadcastPong(CLUSTER_BROADCAST_ALL);
}

/* Reset the slot failover state. This works for both 'client' and 'server' side
 * as all the state about slot failover is cleared.
 *
 * Note:
 * 1. 'client' here means the promoter of the slot failover and 'server' here
 *    means the original owner of the slots.
 * 2. The function can be used to abort a slot failover in progress or to reset
 *    the state after a successful slot failover.
 */
void clusterResetSlotFailover(void) {
    listIter li;
    listNode *ln;

    /* For the 'client' side. */
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterSlotSyncLink *link = ln->value;
        link->slot_mf_ready = 0;
        link->slot_mf_end = 0;
    }

    /* For the 'server' side. */
    listRewind(server.replicas, &li);
    while ((ln = listNext(&li))) {
        client *replica = listNodeValue(ln);
        replica->slotsync_mf_end = 0;
    }

    /* For both 'client' side and 'server' side. */
    server.cluster->slot_mf_end = 0;
    serverLog(LL_NOTICE, "Slot failover has been reset.");
}

int getSlotFailoverReplicaIngressCount(void) {
    int ret = 0;
    listNode *ln;
    listIter li;
    listRewind(server.replicas,&li);
    while ((ln = listNext(&li))) {
        client *replica = listNodeValue(ln);
        if (replica->slotsync_mf_end) {
            int cross_node = 0;
            clusterNode *n = getClusterNodeBySlotList(replica->slotsync_slots, &cross_node);
            if (cross_node || n == server.cluster->myself) {
                ret++;
            }
        }
    }
    return ret;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to slot sync cron jobs.
 * -------------------------------------------------------------------------- */

void clusterSlotSyncCron(void) {
    if (server.cluster->state == CLUSTER_FAIL) {
        return;
    }

    clusterSlotSyncLink *link;
    listNode *ln;
    listIter li;
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        link = ln->value;

        if (link->sync_state == CLUSTER_SLOTSYNC_STATE_TOCONNECT) {
            /* Firstly we delete the keys in the slots to avoid key corrupt. */
            delkeysNotOwnedByMySelf(link->slot_ranges);

            /* Check if the slots are in the same target node. */
            int cross_node = 0;
            clusterNode *n = getClusterNodeBySlotList(link->slot_ranges, &cross_node);
            if (!n) {
                /* If cross node, this slot sync will never success. */
                if (cross_node) {
                    serverLog(LL_WARNING,"Slot sync removed: slots cross node.");
                    clearSlotSyncImporting(link->slot_ranges);
                    listDelNode(server.cluster->slotsync_links, ln);
                }
                return;
            }

            /* The target node should not be myself. */
            if (n == server.cluster->myself) {
                serverLog(LL_WARNING,"Slot sync removed: slot owned by myself.");
                clearSlotSyncImporting(link->slot_ranges);
                listDelNode(server.cluster->slotsync_links, ln);
                return;
            }

            /* Really do the reconnect for this link. */
            initSlotSyncLink(link, n, NULL);
        } else if (link->sync_state == CLUSTER_SLOTSYNC_STATE_WAIT_SCHED) {
            if (clusterGetSlotSyncLinkRank(link) == 0) {
                link->sync_state = CLUSTER_SLOTSYNC_STATE_SEND_SYNC;
                continueSlotSync(link);
            }
        } else if (link->sync_state == CLUSTER_SLOTSYNC_STATE_LOADING_RDB) {
            /* Check the bio loading result. */
        } else if (link->sync_state == CLUSTER_SLOTSYNC_STATE_DONE_LOADING) {
            /* Create a client, here we don't mark the client as a primary for some reasons.
             * This client is used to receive the subsequent slot replication buffer, and
             * we set reply_off indicates that it does not need reply. */
            client *client = createClient(link->sync_conn);
            client->flag.authenticated = 1;
            client->flag.reply_off = 1;
            client->slotsync_link = link;
            client->slotsync_slots = listDup(link->slot_ranges);
            client->flag.slot_sync_primary = 1;
            link->client = client;
            serverLog(LL_NOTICE, "create link->clinet");

            disconnectReplicas();
            freeReplicationBacklog();
            serverLog(LL_NOTICE, "disconnect replicas");

            link->sync_state = CLUSTER_SLOTSYNC_STATE_CONNECTED;
        } else if (link->sync_state == CLUSTER_SLOTSYNC_STATE_LOADING_FAIL) {
            /* Check the bio loading result. */
            resetSlotSyncLinkForReconnect(link);
        } else if (link->sync_state == CLUSTER_SLOTSYNC_STATE_CONNECTED) {
            if (link->client && link->slot_mf_end == 0) {
                slotLinkSendOnline(link->client);
            }
        }
    }

    if (server.cluster->slot_mf_end) {
        /* Check if the slot failover timed out. */
        if (server.cluster->slot_mf_end < mstime()) {
            serverLog(LL_WARNING, "Manual slot failover timed out.");
            updatePausedActions();
            clusterResetSlotFailover();
            return;
        }

        /* Something we should do for slot failover at the 'client' side.
         * ('client' here means the promoter of the slot failover.) */
        {
            /* Get the total number of the slotsync links that slot failover are
             * in progress and count how many of them are ready. */
            int mf_link_cnt = 0, ready_link_cnt = 0;
            listRewind(server.cluster->slotsync_links, &li);
            while ((ln = listNext(&li)) != NULL) {
                link = ln->value;

                /* Count the links that slot failover are in progress. */
                if (!link->slot_mf_end) {
                    continue;
                } else {
                    mf_link_cnt++;
                }

                /* Count the links that are ready to do slot failover. */
                if (link->sync_state == CLUSTER_SLOTSYNC_STATE_CONNECTED) {
                    /* Keep to send ack until this link marked slot ready. */
                    if (!link->slot_mf_ready) {
                        slotLinkSendAck(link->client);
                    } else {
                        ready_link_cnt++;
                    }
                }
            }

            /* Do the slot failover when all the slotsync links are ready. */
            if (mf_link_cnt && mf_link_cnt == ready_link_cnt) {
                serverLog(LL_WARNING,"All cluster slotsync links are ready!");
                clusterBumpConfigEpochWithoutConsensus();
                clusterSlotFailoverReplace();
                clusterResetSlotFailover();
            } else {
                static long long count = 0;
                if (count++ % 10 == 0) {
                    serverLog(LL_NOTICE,"Slot failover status: wait_links=%d, ready_links=%d",
                              mf_link_cnt, ready_link_cnt);
                }
            }
        }

        /* Something we should do for slot failover at the 'server' side.
         * ('server' here means the original owner of the slots.) */
        {
            /* We need to check if all the slot failover are finished. */
            if (isPausedActionsWithUpdate(PAUSE_ACTION_REPLICA) && getSlotFailoverReplicaIngressCount() == 0) {
                /* Unpause the clients. */
                unpauseActions(PAUSE_DURING_FAILOVER);
                /* Free slot failover replicas. */
                notifyClientsCloseSlotSyncLink();
                /* Reset slot failover state. */
                clusterResetSlotFailover();
            }
        }
    }
}

clusterNode *getClusterNodeBySlotList(list *slot_ranges, int *cross_node) {
    clusterNode *n = NULL;
    listNode *ln;
    listIter li;
    listRewind(slot_ranges,&li);
    while ((ln = listNext(&li)) != NULL) {
        slotRange *range = ln->value;
        for (int i = range->start_slot; i <= range->end_slot; i++) {
            /* If the cluster is not fine, should not do slot sync. */
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

void notifyClientsCloseSlotSyncLink(void) {
    listIter li;
    listNode *ln;
    listRewind(server.replicas, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *replica = listNodeValue(ln);
        if (replica->slotsync_mf_end) {
            replyCloseSlotLinkToReplica(replica);
        }
    }
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to slot pending delete.
 * -------------------------------------------------------------------------- */

#define CLUSTER_SLOTCALC_CYCLE_TIME_PERC 20
void clusterSlotPendingDelete(void) {
    // todo add a info fields if needed to track this info.
    if (!server.cluster->pending_del_slot_count) return;

    // todo see if this needed.
    /* Make sure there is no slotsync replica exists before we delete any slot.
     * This is not a necessary condition, but it can protect the slot data if
     * there are bugs in slotsync. */
    listNode *ln;
    listIter li;
    listRewind(server.replicas, &li);
    while ((ln = listNext(&li))) {
        client *replica = ln->value;
        if (replica->flag.slot_sync_replica) {
            serverLog(LL_WARNING, "skip pending delete");
            return;
        }
    }

    /* Limit the cpu. */
    ustime_t timelimit = CLUSTER_SLOTCALC_CYCLE_TIME_PERC * 1000000 / server.hz / 100;

    /* Handle the pending delete slots one by one with time limit. */
    int i = server.cluster->pending_del_slot_count - 1;
    for (; i >= 0; i--) {
        delKeysInSlotWithTimeLimit(server.cluster->pending_del_slots[i], &timelimit);
        if (timelimit <= 0) {
            break;
        }
    }

    /* Mark the already done pending delete slots. */
    while (server.cluster->pending_del_slot_count > 0) {
        i = server.cluster->pending_del_slot_count - 1;
        if (countKeysInSlot(server.cluster->pending_del_slots[i]) == 0) {
            server.cluster->pending_del_slot_count--;
        } else {
            break;
        }
    }
}

int isSlotInPendingDelete(int slot) {
    for (int i = 0; i < server.cluster->pending_del_slot_count; i++) {
        if (server.cluster->pending_del_slots[i] == slot) {
            return 1;
        }
    }
    return 0;
}

int testInjectError(const char *error) {
    if (server.debug_context) {
        return !strcmp(server.debug_context, error);
    }
    return 0;
}

char *getInjectOptionValue(const char *option) {
    char *res = NULL;
    if (server.debug_context) {
        char *options = zstrdup(server.debug_context);
        char *key = strtok(options, ":");
        char *val = strtok(NULL, ":");
        if (key && !strcmp(key, option)) {
            res = zstrdup(val);
        }
        zfree(options);
    }
    return res;
}
