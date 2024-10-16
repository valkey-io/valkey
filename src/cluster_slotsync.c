#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"
#include "bio.h"

/* The following functions are declared here as they will be used in this file
 * but they are defined in other files. */
char *sendCommand(connection *conn, ...);
char *receiveSynchronousResponse(connection *conn);
int useDisklessLoad(void);
void delkeysNotOwnedByMySelf(list *slot_ranges);
void clusterUpdateState(void);
void clusterSaveConfigOrDie(int do_fsync);

/* The following functions are declared here as they will be used by others
 * before the definition, we will define them in this file later. */
void setSlotSyncImporting(list *slot_ranges, clusterNode *node);
void clearSlotSyncImporting(list *slot_ranges);
void syncWithSlotOwner(connection *conn);
void continueSlotSync(clusterSlotSyncLink *link);
void readSlotSyncBulkPayload(connection *conn);

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

/* This function is called by rdbSaveRio() when we need to generate a RDB file
 * which only include the keys in the specified slots for slot sync usage.
 *
 * Returns 1 if the given key is in the specified slot ranges, 0 otherwise. */
int isKeyInSlotRanges(robj *key, list *slot_ranges) {
    if (!key || !slot_ranges) {
        return 0;
    }

    /* Get the slot of this key and check if the slot in the specified range. */
    int slot = keyHashSlot((char*)key->ptr, sdslen(key->ptr));
    return isSlotInSlotRangeList(slot, slot_ranges);
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

int initSlotSyncLink(clusterSlotSyncLink *link, clusterNode *n) {
    link->ctime = mstime();
    strncpy(link->nodename, n->name, CLUSTER_NAMELEN);
    getRandomHexChars(link->linkname, sizeof(link->linkname));

    link->client = NULL;
    link->transfer_total_size = -1;
    link->transfer_read_size = 0;
    link->transfer_last_fsync_off = 0;
    link->transfer_lastio = server.unixtime;
    link->transfer_tmpfile_fd = -1;
    link->transfer_tmpfile_name = NULL;
    link->slot_mf_ready = 0;
    link->slot_mf_end = 0;
    link->slot_mf_lag = 0;

    link->sync_state = CLUSTER_SLOTSYNC_STATE_CONNECTING;
    link->sync_conn = connCreate(connTypeOfCluster());
    if (connConnect(link->sync_conn, n->ip, getNodeDefaultClientPort(n),
                    server.bind_source_addr, syncWithSlotOwner) == C_ERR) {
        serverLog(LL_WARNING,"Unable to connect to slot MASTER: %s",
                  connGetLastError(link->sync_conn));
        freeSlotSyncConn(link);
        return C_ERR;
    }
    connSetReadHandler(link->sync_conn, syncWithSlotOwner);
    setSlotSyncImporting(link->slot_ranges, n);
    connSetPrivateData(link->sync_conn, link);
    serverLog(LL_WARNING, "Start slot sync from:%.40s.", link->nodename);
    return C_OK;
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

    listNode *ln;
    listIter li;
    listRewind(server.cluster->slotsync_links, &li);
    while ((ln = listNext(&li)) != NULL) {
        clusterSlotSyncLink *link = ln->value;
        if (!strncmp(link->linkname, linkid, CLUSTER_NAMELEN)) {
            /* Remove keys not owned by myself. */
            delkeysNotOwnedByMySelf(link->slot_ranges);

            /* Clear all importing state for this link. */
            clearSlotSyncImporting(link->slot_ranges);

            /* Free this link and the client which bound with it. */
            if (link->client) {
                link->client->slotsync_link = NULL;
                freeClientAsync(link->client);
            }
            listDelNode(server.cluster->slotsync_links, ln);
            addReply(c,shared.ok);

            /* Update state and save config. */
            clusterUpdateState();
            clusterSaveConfigOrDie(1);
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
        for(int i = range->start_slot; i <= range->end_slot; i++){
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
    clusterSlotSyncLink* link = connGetPrivateData(conn);
    char *err = NULL;
    int sockerr = 0;

    /* Check for errors in the socket: after a non blocking connect() we
     * may find that the socket is in error state. */
    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING,"Error condition on socket for slot sync: %s",
                  strerror(sockerr));
        goto error;
    }

    /* CLUSTER_SLOTSYNC_STATE: CONNECTING ==> SEND_AUTH|SEND_CAPA
     *
     * Set the read/write event handler. */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_CONNECTING) {
        serverLog(LL_NOTICE,"Non blocking connect for slotsync fired the event.");
        connSetWriteHandler(conn, NULL);
        connSetReadHandler(conn, syncWithSlotOwner);

        if (server.primary_auth) {
            link->sync_state = CLUSTER_SLOTSYNC_STATE_SEND_AUTH;
        } else {
            link->sync_state = CLUSTER_SLOTSYNC_STATE_SEND_CAPA;
        }
    }

    /* CLUSTER_SLOTSYNC_STATE: SEND_AUTH ==> RECV_AUTH
     *
     * AUTH with the slot owner if required.*/
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
            serverLog(LL_WARNING,"Unable to AUTH to MASTER: %s",err);
            sdsfree(err);
            connClose(conn);
            link->sync_state = CLUSTER_SLOTSYNC_STATE_FAILED;
            return;
        }
        sdsfree(err);
        link->sync_state = CLUSTER_SLOTSYNC_STATE_SEND_CAPA;
    }

    /* CLUSTER_SLOTSYNC_STATE: SEND_CAPA ==> RECV_CAPA
     *
     * Inform the slot owner of our capabilities. */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_SEND_CAPA) {
        err = sendCommand(conn,"REPLCONF","capa","eof",NULL);
        if (err) goto write_error;
        sdsfree(err);
        link->sync_state = CLUSTER_SLOTSYNC_STATE_RECV_CAPA;
        return;
    }

    /* CLUSTER_SLOTSYNC_STATE: RECV_CAPA ==> WAIT_SCHED
     *
     * Receive CAPA reply. */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_RECV_CAPA) {
        err = receiveSynchronousResponse(conn);
        if (err == NULL) goto no_response_error;
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF capa. */
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF capa: %s", err);
        }
        sdsfree(err);
        link->sync_state = CLUSTER_SLOTSYNC_STATE_WAIT_SCHED;
    }

    /* CLUSTER_SLOTSYNC_STATE: WAIT_SCHED ==> SEND_SYNC
     *
     * Wait other slotsync links. */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_WAIT_SCHED) {
        if (clusterGetSlotSyncLinkRank(link) > 0) {
            return;
        } else {
            link->sync_state = CLUSTER_SLOTSYNC_STATE_SEND_SYNC;
        }
    }
    continueSlotSync(link);
    return;

    no_response_error: /* Handle receiveSynchronousResponse() error when master has no reply. */
    serverLog(LL_WARNING, "Master did not respond to command during slotsync handshake");
    /* Fall through to regular error handling */

    error:
    connClose(conn);

    /* Set the state to TOCONNECT, so the cron will retry start next time. */
    link->sync_state = CLUSTER_SLOTSYNC_STATE_TOCONNECT;
    return;

    write_error: /* Handle sendCommand() errors. */
    serverLog(LL_WARNING,"Sending command to target handshake: %s", err);
    sdsfree(err);
    goto error;
}

void continueSlotSync(clusterSlotSyncLink *link) {
    if (!link) return;

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
        if (connSyncWrite(link->sync_conn, sync_cmd, sdslen(sync_cmd), server.repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,"I/O error writing to MASTER: %s",strerror(errno));
            sdsfree(slot_ranges);
            sdsfree(sync_cmd);
            goto error;
        }
        sdsfree(slot_ranges);
        sdsfree(sync_cmd);
        link->sync_state = CLUSTER_SLOTSYNC_STATE_RECV_RDB;
    }

    /* Prepare a suitable temp file for rdb transfer. */
    while (maxtries--) {
        snprintf(tmpfile,256,"temp-%d.%ld.rdb",(int)server.unixtime,(long int)getpid());
        tmpfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);
        if (tmpfd != -1) break;
        sleep(1);
    }
    if (tmpfd == -1) {
        serverLog(LL_WARNING,"Opening the temp file needed for slot synchronization: %s",strerror(errno));
        goto error;
    }

    /* Change the read event handler from syncWithSlotOwner() to readSlotSyncBulkPayload(). */
    if (link->sync_state == CLUSTER_SLOTSYNC_STATE_RECV_RDB) {
        if (connSetReadHandler(link->sync_conn, readSlotSyncBulkPayload) == C_ERR) {
            serverLog(LL_WARNING,
                      "Can't create readable event for slot SYNC: %s (fd=%d)",
                      strerror(errno),link->sync_conn->fd);
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
#define SLOTSYNC_DEFAULT_LAG 20000000000           /* Just a value large enough */
void readSlotSyncBulkPayload(connection *conn) {
    clusterSlotSyncLink *link = connGetPrivateData(conn);
    char buf[PROTO_IOBUF_LEN];
    ssize_t nread, readlen, nwritten;
    int use_diskless_load = useDisklessLoad();
    char rdbpath[1024];
    off_t left;

    if (server.loading == 1) {
        serverLog(LL_NOTICE, "Waiting prev loading finish");
        return;
    }

    /* Static vars used to hold the EOF mark, and the last bytes received
     * from the server: when they match, we reached the end of the transfer. */
    static char eofmark[CONFIG_RUN_ID_SIZE];
    static char lastbytes[CONFIG_RUN_ID_SIZE];
    static int usemark = 0;

    /* If transfer_total_size == -1, we still have to read the bulk length
     * from the master reply. */
    if (link->transfer_total_size == -1) {
        if (connSyncReadLine(conn,buf,1024,server.repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,
                      "I/O error reading bulk count from MASTER: %s",
                      strerror(errno));
            goto error;
        }

        if (buf[0] == '-') {
            serverLog(LL_WARNING,
                      "MASTER aborted replication with an error: %s",
                      buf+1);
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
        if (strncmp(buf+1,"EOF:",4) == 0 && strlen(buf+5) >= CONFIG_RUN_ID_SIZE) {
            usemark = 1;
            memcpy(eofmark,buf+5,CONFIG_RUN_ID_SIZE);
            memset(lastbytes,0,CONFIG_RUN_ID_SIZE);
            /* Set any transfer_total_size to avoid entering this code path
             * at the next call. */
            link->transfer_total_size = 0;
            serverLog(LL_NOTICE,
                      "Cluster slot sync: receiving streamed RDB from master with EOF %s",
                      use_diskless_load? "to parser":"to disk");
        } else {
            usemark = 0;
            link->transfer_total_size = strtol(buf+1,NULL,10);
            serverLog(LL_NOTICE,
                      "Cluster slot sync: receiving %lld bytes from master %s",
                      (long long) link->transfer_total_size,
                      use_diskless_load? "to parser":"to disk");
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

        nread = connRead(conn,buf,readlen);
        if (nread <= 0) {
            if (connGetState(conn) == CONN_STATE_CONNECTED) {
                /* equivalent to EAGAIN */
                return;
            }
            serverLog(LL_WARNING,"I/O error trying to sync with slot owner: %s",
                      (nread == -1) ? strerror(errno) : "connection lost");
            goto error;
        }
        server.stat_net_input_bytes += nread;

        /* When a mark is used, we want to detect EOF asap in order to avoid
         * writing the EOF mark into the file... */
        int eof_reached = 0;

        if (usemark) {
            /* Update the last bytes array, and check if it matches our
             * delimiter. */
            if (nread >= CONFIG_RUN_ID_SIZE) {
                memcpy(lastbytes,buf+nread-CONFIG_RUN_ID_SIZE,
                       CONFIG_RUN_ID_SIZE);
            } else {
                int rem = CONFIG_RUN_ID_SIZE-nread;
                memmove(lastbytes,lastbytes+nread,rem);
                memcpy(lastbytes+rem,buf,nread);
            }
            if (memcmp(lastbytes,eofmark,CONFIG_RUN_ID_SIZE) == 0)
                eof_reached = 1;
        }

        /* Update the last I/O time for the replication transfer (used in
         * order to detect timeouts during replication), and write what we
         * got from the socket to the dump file on disk. */
        link->transfer_lastio = server.unixtime;
        if ((nwritten = write(link->transfer_tmpfile_fd,buf,nread)) != nread) {
            serverLog(LL_WARNING,
                      "Write error or short write writing to the DB dump file "
                      "needed for Cluster slot synchronization: %s",
                      (nwritten == -1) ? strerror(errno) : "short write");
            goto error;
        }
        link->transfer_read_size += nread;

        /* Delete the last 40 bytes from the file if we reached EOF. */
        if (usemark && eof_reached) {
            if (ftruncate(link->transfer_tmpfile_fd,
                          link->transfer_read_size - CONFIG_RUN_ID_SIZE) == -1)
            {
                serverLog(LL_WARNING,
                          "Error truncating the RDB file received from the master "
                          "for SYNC: %s", strerror(errno));
                goto error;
            }
        }

        /* Sync data on disk from time to time, otherwise at the end of the
         * transfer we may suffer a big delay as the memory buffers are copied
         * into the actual disk. */
        if (link->transfer_read_size >=
            link->transfer_last_fsync_off + SLOTSYNC_MAX_WRITTEN_BEFORE_FSYNC)
        {
            off_t sync_size = link->transfer_read_size -
                              link->transfer_last_fsync_off;
            rdb_fsync_range(link->transfer_tmpfile_fd,
                            link->transfer_last_fsync_off, sync_size);
            link->transfer_last_fsync_off += sync_size;
        }

        /* Check if the transfer is now complete */
        if (!usemark) {
            if (link->transfer_read_size == link->transfer_total_size)
                eof_reached = 1;
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
     * time for non blocking loading. */
    connSetReadHandler(conn, NULL);
    serverLog(LL_NOTICE, "Cluster slot sync: Loading DB in memory");
    rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
    if (use_diskless_load) {
        rio rdb;
        int async = 0; /* Do not use async loading. */
        rioInitWithConn(&rdb,conn,link->transfer_total_size);

        /* Put the socket in blocking mode to simplify RDB transfer.
         * We'll restore it when the RDB is received. */
        connBlock(conn);
        connRecvTimeout(conn, server.repl_timeout*1000);
        startLoading(link->transfer_total_size, RDBFLAGS_REPLICATION, async);

        if (rdbLoadRio(&rdb,RDBFLAGS_REPLICATION,&rsi) != C_OK) {
            /* RDB loading failed. */
            stopLoading(0);
            serverLog(LL_WARNING,
                      "Failed trying to load the slot owner synchronization DB "
                      "from socket");
            rioFreeConn(&rdb, NULL);

            /* Note that there's no point in restarting the AOF on SYNC
             * failure, it'll be restarted when sync succeeds or the replica
             * gets promoted. */
            goto error;
        }

        /* Verify the end mark is correct. */
        if (usemark) {
            if (!rioRead(&rdb,buf,CONFIG_RUN_ID_SIZE) ||
                memcmp(buf,eofmark,CONFIG_RUN_ID_SIZE) != 0)
            {
                stopLoading(0);
                serverLog(LL_WARNING,"Replication stream EOF marker is broken");
                rioFreeConn(&rdb, NULL);
                goto error;
            }
        }

        stopLoading(1);

        /* Cleanup and restore the socket to the original state to continue
         * with the normal replication. */
        rioFreeConn(&rdb, NULL);
        connNonBlock(conn);
        connRecvTimeout(conn,0);
    } else {
        /* Ensure background save doesn't overwrite synced data */
        if (server.child_type == CHILD_TYPE_RDB) {
            serverLog(LL_NOTICE,
                      "Replica is about to load the RDB file received from the "
                      "master, but there is a pending RDB child running. "
                      "Killing process %ld and removing its temp file to avoid "
                      "any race",
                      (long) server.child_pid);
            killRDBChild();
        }

        /* Make sure the new file (also used for persistence) is fully synced
         * (not covered by earlier calls to rdb_fsync_range). */
        if (fsync(link->transfer_tmpfile_fd) == -1) {
            serverLog(LL_WARNING,
                      "Failed trying to sync the temp DB to disk in "
                      "Cluster slot synchronization: %s",
                      strerror(errno));
            goto error;
        }

        /* Rename rdb like renaming rewrite aof asynchronously. */
        sprintf(rdbpath, "%s_slot", server.rdb_filename);
        int old_rdb_fd = open(rdbpath,O_RDONLY|O_NONBLOCK);
        if (rename(link->transfer_tmpfile_name, rdbpath) == -1) {
            serverLog(LL_WARNING,
                      "Failed trying to rename the temp DB into %s in "
                      "Cluster slot synchronization: %s",
                      rdbpath, strerror(errno));
            if (old_rdb_fd != -1) close(old_rdb_fd);
            goto error;
        }
        /* Close old rdb asynchronously. */
        if (old_rdb_fd != -1) bioCreateCloseJob(old_rdb_fd, 0, 1);

        if (rdbLoad(rdbpath,&rsi,RDBFLAGS_REPLICATION) != C_OK) {
            serverLog(LL_WARNING,
                      "Failed trying to load the MASTER synchronization "
                      "DB from disk");
            if (server.rdb_del_sync_files && allPersistenceDisabled()) {
                serverLog(LL_NOTICE,"Removing the RDB file obtained from "
                                    "the master. This replica has persistence "
                                    "disabled");
                bg_unlink(rdbpath);
            }

            /* Note that there's no point in restarting the AOF on sync failure,
               it'll be restarted when sync succeeds or replica promoted. */
            goto error;
        }

        /* Cleanup. */
        if (server.rdb_del_sync_files && allPersistenceDisabled()) {
            serverLog(LL_NOTICE,"Removing the RDB file obtained from "
                                "the master. This replica has persistence "
                                "disabled");
            bg_unlink(rdbpath);
        }

        zfree(link->transfer_tmpfile_name);
        close(link->transfer_tmpfile_fd);
        link->transfer_tmpfile_fd = -1;
        link->transfer_tmpfile_name = NULL;
    }

    /* Mark the synchronization has done. */
    link->sync_state = CLUSTER_SLOTSYNC_STATE_CONNECTED;

    /* Set to a value large enough after first init. */
    link->slot_mf_lag = SLOTSYNC_DEFAULT_LAG;

    /* Create client */
    client* client = createClient(link->sync_conn);
    client->flag.authenticated = 1;
    client->slotsync_link = link;
    client->slotsync_slots = listDup(link->slot_ranges);
    client->flag.reply_off = 0;
    link->client = client;

    moduleFireServerEvent(VALKEYMODULE_EVENT_PRIMARY_LINK_CHANGE, VALKEYMODULE_SUBEVENT_PRIMARY_LINK_UP, NULL);

    serverLog(LL_NOTICE, "Cluster slot sync: Finished with success");
    if (server.supervised_mode == SUPERVISED_SYSTEMD) {
        serverCommunicateSystemd("STATUS=Cluster slot sync: Finished with success. "
                                "Ready to accept connections in read-write mode.\n");
    }

    /* Restart the AOF subsystem now that we finished the sync. This
     * will trigger an AOF rewrite, and when done will start appending
     * to the new file. */
    if (server.aof_enabled) restartAOFAfterSYNC();
    return;

    error:
    /* Reset the link state to TOCONNECT, the cron will retry start next time. */
    resetSlotSyncLinkForReconnect(link);
    return;
}
