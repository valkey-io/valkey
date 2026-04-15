#include "forkless.h"
#include "server.h"
#include "bgiteration.h"
#include "mutexqueue.h"
#include "rdb.h"
#include "bio.h"

static const void *PROCESS_COMPLETE_ITEM = (void *)-1;
static const int SNAPSHOT_FILE_CLOSE_MONITOR_INTERVAL_MS = 200;
static const int REPLICATION_MONITOR_INTERVAL_MS = 100;

typedef struct {
    rio save_rio; /* Must be 1st to permit cast from rio back to forklessSaveInfo */
    int cur_db;   /* Last selectDb issued */
    bgIterator *iterator;
    uint64_t bytes_written;
    int err_code;
    mutexQueue *foreground_queue;
    bool terminated;
    sds temp_file;
    sds final_file;
    int write_target;
    union {
        struct {
            sds temp_file;
            sds final_file;
        } file;
        struct {
            list *clients;
            char eofmark[RDB_EOF_MARK_SIZE];
        } repl;
    } u;
} forklessSaveInfo;

/* Keep a global indicator of the current iterator (for cancellation purposes). */
static forklessSaveInfo *currentForklessSave = NULL;

/* rio check_abort_between_writes callback: checks if the forkless save iterator is being terminated. */
static int forklessSaveShouldAbort(rio *r) {
    static_assert(offsetof(forklessSaveInfo, save_rio) == 0, "rio must be castable to forklessSaveInfo");
    forklessSaveInfo *saveInfo = (forklessSaveInfo *)r;
    return saveInfo->iterator && bgIteratorIsTerminating(saveInfo->iterator);
}


static int connSetBlocking(connection *conn, bool blocking) {
    int rc = (blocking) ? connBlock(conn) : connNonBlock(conn);
    if (rc == ANET_ERR) {
        serverLog(LL_WARNING, "forkless-save: error setting FD blocking(%d)", blocking);
        return C_ERR;
    }

    if (blocking) {
        if (connSendTimeout(conn, server.repl_timeout * 1000) == ANET_ERR) {
            serverLog(LL_WARNING, "forkless-save: error setting send timeout");
            return C_ERR;
        }
        if (connRecvTimeout(conn, server.repl_timeout * 1000) == ANET_ERR) {
            serverLog(LL_WARNING, "forkless-save: error setting send timeout");
            return C_ERR;
        }
    }

    return C_OK;
}

static int writeSelectDb(forklessSaveInfo *saveInfo, int new_db) {
    if (new_db == saveInfo->cur_db) return C_OK;

    if (rdbSaveType(&saveInfo->save_rio, RDB_OPCODE_SELECTDB) == -1) {
        serverLog(LL_WARNING, "forkless-save: error while writing OPCODE_SELECTDB");
        return C_ERR;
    }
    if (rdbSaveLen(&saveInfo->save_rio, new_db) == -1) {
        serverLog(LL_WARNING, "forkless-save: error while writing selectDb value");
        return C_ERR;
    }
    saveInfo->cur_db = new_db;
    return C_OK;
}

static int writeDbSizeHints(forklessSaveInfo *saveInfo) {
    for (int dbid = 0; dbid < server.dbnum; dbid++) {
        serverDb *db = server.db[dbid];
        if (db == NULL || dbSize(db) == 0) continue;
        if (writeSelectDb(saveInfo, dbid) != C_OK) return C_ERR;
        if (rdbSaveDbSizeHints(&saveInfo->save_rio, db, 0) < 0) return C_ERR;
    }
    return C_OK;
}

/* Forward declarations for helper functions */
static void abandonClient(forklessSaveInfo *saveInfo, client *c);
static int pruneDisconnectedReplicas(forklessSaveInfo *saveInfo);
static void waitForBuffersToDrain(forklessSaveInfo *saveInfo);
static int transitionRioReplicaCobToRioConnset(forklessSaveInfo *saveInfo);

/* Write an inline replication command into the RDB stream using RDB_OPCODE_UPDATE.
 * The command is serialized as RESP (multi-bulk) so the replica can replay it during RDB load. */
static int writeReplicationData(forklessSaveInfo *saveInfo, bgIteratorItem *item) {
    serverAssert(item->type == BGITERATOR_ITEM_REPLICATION);

    if (rdbSaveType(&saveInfo->save_rio, RDB_OPCODE_UPDATE) == -1) return C_ERR;

    char llaux[LONG_STR_SIZE + 3];
    char llstr[LONG_STR_SIZE];
    int auxlen;

    /* Multi bulk length */
    auxlen = 0;
    llaux[auxlen++] = '*';
    auxlen += ll2string(llaux + auxlen, LONG_STR_SIZE, item->u.repl.argc);
    llaux[auxlen++] = '\r';
    llaux[auxlen++] = '\n';
    if (!rioWrite(&saveInfo->save_rio, llaux, auxlen)) return C_ERR;

    for (int i = 0; i < item->u.repl.argc; i++) {
        robj *o = item->u.repl.argv[i];
        int objlen;
        char *p;

        if (sdsEncodedObject(o)) {
            p = objectGetVal(o);
            objlen = sdslen(objectGetVal(o));
        } else {
            p = llstr;
            objlen = ll2string(p, LONG_STR_SIZE, (long)objectGetVal(o));
        }

        auxlen = 0;
        llaux[auxlen++] = '$';
        auxlen += ll2string(llaux + auxlen, LONG_STR_SIZE, objlen);
        llaux[auxlen++] = '\r';
        llaux[auxlen++] = '\n';
        if (!rioWrite(&saveInfo->save_rio, llaux, auxlen)) return C_ERR;
        if (!rioWrite(&saveInfo->save_rio, p, objlen)) return C_ERR;
        if (!rioWrite(&saveInfo->save_rio, "\r\n", 2)) return C_ERR;
    }

    return C_OK;
}

/* Entry point for background thread.
 * Upon entering:
 *  - The RDB header has been written (magic, aux fields, functions)
 *  - The DB size hints have been written
 * This function is responsible for writing all of the dictionary entries. 
 * Additionally:
 *  - For file-based: flush/close/rename of the output file
 *  - For socket-based: transition from COB to socket IO
 * */
static void *forklessSaveProcessor(void *arg) {
    serverAssert(!onServerMainThread());
    forklessSaveInfo *saveInfo = arg;

    serverLog(LL_NOTICE, "forkless-save: background processor started");
    int err = C_OK;

    bool rioIsConnset = false;
    if (saveInfo->write_target == RDB_WRITE_TARGET_SOCKET) {
        err = transitionRioReplicaCobToRioConnset(saveInfo);
        rioIsConnset = (err == C_OK);
        serverLog(LL_NOTICE, "forkless-save: transition to connset %s, %d clients remain",
                  rioIsConnset ? "OK" : "FAILED", (int)listLength(saveInfo->u.repl.clients));
    }

    saveInfo->save_rio.check_abort_between_writes = forklessSaveShouldAbort;

    const unsigned statsIntervalMs = 1000;
    monotime lastStatsTime;
    elapsedStart(&lastStatsTime);

    bool done = false;
    bool terminated = false;
    long items = 0;
    while (!done && err == C_OK) {
        bgIteratorItem *item = bgIteratorRead(saveInfo->iterator);

        if (saveInfo->write_target == RDB_WRITE_TARGET_SOCKET)
        if (pruneDisconnectedReplicas(saveInfo) <= 0) {
            serverLog(LL_WARNING, "forkless-save: all replicas disconnected, aborting");
            err = C_ERR;
            break;
        }

        switch (item->type) {
        case BGITERATOR_ITEM_COMPLETE:
            done = true;
            break;

        case BGITERATOR_ITEM_TERMINATED:
            terminated = true;
            done = true;
            break;

        case BGITERATOR_ITEM_DBENTRY:
            if ((err = writeSelectDb(saveInfo, item->dbid)) == C_ERR) break;
            items++;

            robj key;
            initStaticStringObject(key, objectGetKey(item->u.dbe.de));
            robj *o = item->u.dbe.de;

                long long expire = objectGetExpire(item->u.dbe.de);
                if (rdbSaveKeyValuePair(&saveInfo->save_rio, &key, o, expire, item->dbid, RDB_VERSION) == -1) {
                    serverLog(LL_WARNING, "forkless-save: error writing KV pair");
                    err = C_ERR;
                }
                break;

            case BGITERATOR_ITEM_REPLICATION:
                serverAssert(saveInfo->write_target == RDB_WRITE_TARGET_SOCKET);
                if ((err = writeSelectDb(saveInfo, item->dbid)) == C_ERR) break;
                err = writeReplicationData(saveInfo, item);
                break;

            case BGITERATOR_ITEM_SWAPDB:
                /* Iterator tracks swapdb internally; no special action needed. */
                break;

            case BGITERATOR_ITEM_FLUSHDB:
                /* Flush command will be replicated via the replication stream. */
                break;
            
            default:
                /* bgIteration may deliver item types that are not necessarily relevant to us.
                * New types may also be added in the future. It is the client's responsibility
                * to filter out irrelevant types, so we simply ignore them here. */
                break;
        }

        if (elapsedMs(lastStatsTime) >= statsIntervalMs) {
            elapsedStart(&lastStatsTime);
            atomic_store_explicit(&server.stat_current_save_keys_processed, items, memory_order_relaxed);
        }
    }

    if (err != C_OK && bgIteratorIsTerminating(saveInfo->iterator) && !rioGetWriteError(&saveInfo->save_rio)) {
        /* The write returned an error because the abort check stopped it, not
         * from a real I/O failure (no RIO write error is set). Treat it as a
         * cancel. */
        terminated = true;
        err = C_OK;
    }

    char *message = "";
    if (terminated)
        message = "TERMINATED";
    else if (err != C_OK)
        message = "***ERROR***";
    serverLog(LL_NOTICE, "forkless-save: background processor finished. %ld items processed. %s",
              items, message);

    /* For socket saves, transition back from connset to COB so the main thread
     * can write the end marker into the COB via finishSocketBasedForklessSaveUsingCob. */
    if (saveInfo->write_target == RDB_WRITE_TARGET_SOCKET) {
        if (!terminated && err == C_OK) {
            rioFlush(&saveInfo->save_rio);
        }

        /* Capture bytes written so far (will be updated on main thread after EOF). */
        saveInfo->bytes_written = saveInfo->save_rio.processed_bytes;

        /* Return clients to non-blocking IO. */
        listNode *ln;
        listIter li;
        listRewind(saveInfo->u.repl.clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            if (connSetBlocking(c->conn, false) == C_ERR) {
                serverLog(LL_WARNING, "forkless-save: error returning client to non-blocking.");
                abandonClient(saveInfo, c);
            }
        }

        if (rioIsConnset) {
            if (terminated || err != C_OK) {
                rioFreeConnset(&saveInfo->save_rio);
            } else {
                /* Success: swap back to COB. End marker will be written on main thread. */
                uint64_t cksum = saveInfo->save_rio.cksum;
                size_t processed = saveInfo->save_rio.processed_bytes;
                rioFreeConnset(&saveInfo->save_rio);
                rioInitWithReplicaCOB(&saveInfo->save_rio);
                saveInfo->save_rio.cksum = cksum;
                saveInfo->save_rio.processed_bytes = processed;
                if (server.rdb_checksum) {
                    saveInfo->save_rio.update_cksum = rioGenericUpdateChecksum;
                }
            }
        } else {
            /* Error before transitioning to connset — still a COB rio. */
            rioFreeReplicaCOB(&saveInfo->save_rio);
        }
    }

    serverLog(LL_NOTICE, "forkless-save: background processor finished. %ld items processed. %s",
            items, message);

    saveInfo->err_code = err;
    bgIteratorClose(saveInfo->iterator);
    return NULL;
}

/* After the bg thread finishes, some clients may have been marked for close
 * (e.g., by the main thread's freeClient path). Remove them from the client
 * list so we don't try to finish the RDB stream to dead connections. */
static void freeRecentlyTerminatedClients(forklessSaveInfo *saveInfo) {
    serverAssert(onServerMainThread());

    listNode *ln;
    listIter li;
    listRewind(saveInfo->u.repl.clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        if (c->flag.close_asap || c->flag.close_after_reply) {
            listDelNode(saveInfo->u.repl.clients, ln);
            c->flag.forkless_managed = 0;
            if (c->repl_data) c->repl_data->using_cob = 0;
            freeClient(c);
        }
    }
}

/* If a client is unresponsive or is being closed by the main thread, we might have to drop it
 * from the current replication activity.
 */
static void abandonClient(forklessSaveInfo *saveInfo, client *c) {
    serverAssert(saveInfo->write_target == RDB_WRITE_TARGET_SOCKET);
    listNode *ln = listSearchKey(saveInfo->u.repl.clients, c);
    serverAssert(ln != NULL);
    listDelNode(saveInfo->u.repl.clients, ln);

    int remaining = listLength(saveInfo->u.repl.clients);
    serverLog(LL_WARNING, "forkless-save: client(%llu) unresponsive.  %d clients remain.",
            (unsigned long long)c->id, remaining);

    // If the client connection is part of a connection set, remove it
    if (rioCheckType(&saveInfo->save_rio) == RIO_TYPE_CONNSET) {
        rioFreeConnectionFromConnset(&saveInfo->save_rio, c->conn);
    }

    // Before passing it back to the main thread, set the connection back to non-blocking
    if (connSetBlocking(c->conn, false) == C_ERR) {
        serverLog(LL_WARNING, "forkless-save: error returning client(%llu) to non-blocking.", (unsigned long long)c->id);
    }

    c->flag.forkless_managed = 0;
    if (c->repl_data) c->repl_data->using_cob = 0;
    mutexQueueAdd(saveInfo->foreground_queue, c);
}

/* Returns the number of remaining connected replicas. */
static int pruneDisconnectedReplicas(forklessSaveInfo *saveInfo) {
    serverAssert(saveInfo->write_target == RDB_WRITE_TARGET_SOCKET);
    listNode *ln;
    listIter li;
    listRewind(saveInfo->u.repl.clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        waitForClientIO(c);
        // Check if client should be abandoned (simplified - no ElastiCache flags)
    }
    return listLength(saveInfo->u.repl.clients);
}

// Before writing directly to the connection, we need to wait for various buffers to drain.
static void waitForBuffersToDrain(forklessSaveInfo *saveInfo) {
    serverAssert(!onServerMainThread());
    serverAssert(saveInfo->write_target == RDB_WRITE_TARGET_SOCKET);

    listNode *ln;
    listIter li;

    monotime startTimeMono;
    elapsedStart(&startTimeMono);

    const unsigned long loopDelayUs = 100000; // 100ms

    // Give clients a chance to flush COBs
    while (elapsedUs(startTimeMono) < (unsigned long long)server.repl_timeout * 1000000) {
        usleep(loopDelayUs);
        atomic_thread_fence(__ATOMIC_ACQUIRE);

        pruneDisconnectedReplicas(saveInfo);

        listRewind(saveInfo->u.repl.clients, &li);
        bool allFlushed = true;
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            // Check for pending data in the COB
            if (clientHasPendingReplies(c)) {
                allFlushed = false;
                break;
            }
        }
        if (allFlushed) break;
    }

    // Kill off clients which still have COB data
    listRewind(saveInfo->u.repl.clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        if (clientHasPendingReplies(c)) {
            serverLog(LL_WARNING, "forkless-save: socket not draining (COB), client(%llu)", (unsigned long long)c->id);
            abandonClient(saveInfo, c);
        }
    }

    pruneDisconnectedReplicas(saveInfo);
}

/* This function waits until all of the COBs have drained and transitions RIO to a CONNSET.
 * Returns:  C_OK - successful transition, saveInfo->save_rio is now a CONNSET
 *           C_ERR - failure, saveInfo->save_rio is still ReplicaCOB
 */
static int transitionRioReplicaCobToRioConnset(forklessSaveInfo *saveInfo) {
    serverAssert(!onServerMainThread());
    serverAssert(saveInfo->write_target == RDB_WRITE_TARGET_SOCKET);

    waitForBuffersToDrain(saveInfo);

    listNode *ln;
    listIter li;

    // Set remaining clients to blocking
    listRewind(saveInfo->u.repl.clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        if (connBlock(c->conn) == C_ERR) {
            serverLog(LL_WARNING, "forkless-save: unable to set blocking on client(%llu)", (unsigned long long)c->id);
            abandonClient(saveInfo, c);
        }
    }

    // Hopefully we still have some clients left
    int numConns = listLength(saveInfo->u.repl.clients);
    if (numConns == 0) return C_ERR;

    // At this point, we are done with ReplicaCOB RIO.
    uint64_t current_cksum = saveInfo->save_rio.cksum;
    size_t current_processed_bytes = saveInfo->save_rio.processed_bytes;
    rioFreeReplicaCOB(&saveInfo->save_rio);

    connection **conns = zmalloc(sizeof(connection*) * numConns);
    listRewind(saveInfo->u.repl.clients, &li);
    int pos = 0;
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        conns[pos++] = c->conn;
    }

    rioInitWithConnset(&saveInfo->save_rio, conns, numConns);
    saveInfo->save_rio.cksum = current_cksum;
    saveInfo->save_rio.processed_bytes = current_processed_bytes;
    if (server.rdb_checksum) {
        saveInfo->save_rio.update_cksum = rioGenericUpdateChecksum;
    }

    zfree(conns);
    return C_OK;
}

static void resumeRegularReplicaActivity(client *c) {
    serverAssert(onServerMainThread());

    // Set the connection back to non-block to make sure
    // don't hang the main thread.
    if (connSetBlocking(c->conn, false) == C_ERR) {
        serverLog(LL_WARNING, "forkless-save: error returning client(%llu) to non-blocking.", (unsigned long long)c->id);
    }

    c->flag.forkless_managed = 0;

    // Since this is a replica client, re-register with priority
    connSetReadHandler(c->conn, readQueryFromClient);
    connSetPrivateData(c->conn, c);
}

static void resumeClientsAndFreeClientList(forklessSaveInfo *saveInfo, bool successful) {
    serverAssert(onServerMainThread());

    listNode *ln;
    listIter li;
    listRewind(saveInfo->u.repl.clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        if (successful) {
            serverLog(LL_NOTICE, "forkless-save: resuming regular activity for client(%llu)", (unsigned long long)c->id);
            resumeRegularReplicaActivity(c);
        } else {
            serverLog(LL_NOTICE, "forkless-save: error or canceled, terminating client(%llu)", (unsigned long long)c->id);
            c->flag.forkless_managed = 0;
            if (c->repl_data) c->repl_data->using_cob = 0;
            freeClient(c);
        }
    }

    listRelease(saveInfo->u.repl.clients);
    saveInfo->u.repl.clients = NULL;
}

static void cleanupSaveInfoAndEmitEndMetrics(forklessSaveInfo *saveInfo) {
    bool cancelled = saveInfo->terminated && saveInfo->err_code == C_OK;
    bool success = !saveInfo->terminated && saveInfo->err_code == C_OK;

    /* A cancel must not count as a failed save, so skip the metrics that set
     * lastbgsave_status (like the fork child's SIGUSR1 whitelist). */
    if (!cancelled) rdbRecordEndMetrics(RDB_BGSAVE_TYPE_FORKLESS, saveInfo->err_code, time(NULL));
    /* startSaving() fired the persistence start event in this process, so a
     * terminal event must be emitted even on cancel to balance it. */
    stopSaving(success);
    /* Finalize the save state in any case. */
    rdbClearSaveState(time(NULL));

    if (cancelled) {
        serverLog(LL_WARNING, "forkless-save: forkless save cancelled. %lld seconds.", (long long)server.rdb_save_time_last);
    } else if (success) {
        serverLog(LL_NOTICE, "forkless-save: forkless save complete. %lld seconds.", (long long)server.rdb_save_time_last);
    } else {
        serverLog(LL_WARNING, "forkless-save: forkless save failed. %lld seconds.", (long long)server.rdb_save_time_last);
    }

    /* Notify replicas waiting for BGSAVE to complete */
    updateReplicasWaitingBgsave(saveInfo->err_code, saveInfo->write_target);

    currentForklessSave = NULL;
    atomic_store_explicit(&server.stat_current_save_keys_processed, 0, memory_order_relaxed);
    atomic_store_explicit(&server.stat_current_save_keys_total, 0, memory_order_relaxed);

    serverAssert(saveInfo->u.file.temp_file == NULL);
    zfree(saveInfo);
}

/* Routine for background thread to close and rename the forkless save snapshot file.
 * Closing the file requires synchronously flushing the content to disk, which can
 * take some time. */
static void forklessSaveCloseSnapshotFile(void *args[]) {
    serverAssert(!onServerMainThread());
    forklessSaveInfo *saveInfo = (forklessSaveInfo *)args[0];
    /* Error or not, close the file... */
    /* Flush the RIO buffer to the OS before fsync, otherwise any bytes still
     * buffered (including the tail written after the last autosync boundary and
     * the RDB footer) are not covered by the fsync below. */
    if (rioFlush(&saveInfo->save_rio) == 0) {
        serverLog(LL_WARNING, "forkless-save: error flushing temp file [%s]: %s",
                  saveInfo->u.file.temp_file, strerror(errno));
        saveInfo->err_code = C_ERR;
    }
    if (valkey_fsync(fileno(saveInfo->save_rio.io.file.fp)) != 0) {
        serverLog(LL_WARNING, "forkless-save: error fsyncing temp file [%s]: %s",
                  saveInfo->u.file.temp_file, strerror(errno));
        saveInfo->err_code = C_ERR;
    }
    if (fclose(saveInfo->save_rio.io.file.fp) != 0) {
        serverLog(LL_WARNING, "forkless-save: error closing temp file [%s]: %s",
                  saveInfo->u.file.temp_file, strerror(errno));
        saveInfo->err_code = C_ERR;
    }

    if (!saveInfo->terminated && saveInfo->err_code == C_OK) {
        if (rename(saveInfo->u.file.temp_file, saveInfo->u.file.final_file) != 0) {
            serverLog(LL_WARNING, "forkless-save: error moving temp file [%s] to destination [%s]: %s",
                      saveInfo->u.file.temp_file, saveInfo->u.file.final_file, strerror(errno));
            saveInfo->err_code = C_ERR;
        } else if (fsyncFileDir(saveInfo->u.file.final_file) != 0) {
            /* fsync the directory so the rename itself survives a crash. */
            serverLog(LL_WARNING, "forkless-save: error syncing directory for [%s]: %s",
                      saveInfo->u.file.final_file, strerror(errno));
            saveInfo->err_code = C_ERR;
        }
    }

    if (saveInfo->terminated || saveInfo->err_code != C_OK) {
        bg_unlink(saveInfo->u.file.temp_file);
    }
    sdsfree(saveInfo->u.file.temp_file);
    sdsfree(saveInfo->u.file.final_file);
    saveInfo->u.file.temp_file = NULL;
    saveInfo->u.file.final_file = NULL;
    /* Notify the main thread that I am done closing the file. */
    mutexQueueAdd(saveInfo->foreground_queue, (void *)PROCESS_COMPLETE_ITEM);
}

/* Timer proc which runs in the main valkey event loop. It monitors to see when the background thread
 * completes the action to close and rename the snapshot file at the end of disk based forkless save,
 * and performs the final clean-up actions. */
static long long snapshotEndMonitorTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    serverAssert(onServerMainThread());

    forklessSaveInfo *saveInfo = (forklessSaveInfo *)clientData;

    /* I own this mutex queue from the main thread, check to see if the background
       job is done or not. Note we only expect a single notification event here. */
    if (mutexQueuePop(saveInfo->foreground_queue, false) != NULL) {
        mutexQueueRelease(saveInfo->foreground_queue);
        saveInfo->foreground_queue = NULL;
        cleanupSaveInfoAndEmitEndMetrics(saveInfo);
        return AE_NOMORE;
    }
    return SNAPSHOT_FILE_CLOSE_MONITOR_INTERVAL_MS;
}


/* After a socket-based forkless save, set each replica's ref_repl_buf_node to the
 * tail of the shared replication buffer so that future replication data
 * (starting at server.primary_repl_offset+1) will be sent to the replica. */
static void fixReplicationOffset(forklessSaveInfo *saveInfo) {
    serverAssert(onServerMainThread());

    serverLog(LL_NOTICE, "forkless-save: queuing repl_offset: %lld (on COB)", server.primary_repl_offset);

    listNode *repl_node = listLast(server.repl_buffer_blocks);
    replBufBlock *tail = repl_node ? listNodeValue(repl_node) : NULL;
    listNode *target_node = NULL;
    size_t target_pos = 0;

    if (tail != NULL) {
        target_node = repl_node;
        target_pos = tail->used;
    }

    listNode *ln;
    listIter li;
    listRewind(saveInfo->u.repl.clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);

        if (target_node != NULL) {
            ((replBufBlock *)listNodeValue(target_node))->refcount++;
            serverAssert(c->repl_data);
            if (c->repl_data->ref_repl_buf_node != NULL) {
                ((replBufBlock *)listNodeValue(c->repl_data->ref_repl_buf_node))->refcount--;
            }
            c->repl_data->ref_repl_buf_node = target_node;
            c->repl_data->ref_block_pos = target_pos;
        }

        /* Tell the replica the final replication offset so it can continue
         * replicating from the correct point after loading the RDB. */
        addReplyArrayLen(c, 3);
        addReplyBulkCString(c, "REPLCONF");
        addReplyBulkCString(c, "psync-offset");
        addReplyBulkLongLong(c, server.primary_repl_offset);
    }
}

/* After the background thread finishes writing keys via the connset, the main
 * thread finishes the RDB stream using the COB:
 *   1) Update the RDB checksum with any data already in the COB
 *   2) Write the EOF, checksum, and eofmark into the COB
 *   3) Suspend writes until ACK, then resume so the COB drains
 *   4) Fix the replication offset so future repl data flows correctly */
static int finishSocketBasedForklessSaveUsingCob(forklessSaveInfo *saveInfo) {
    serverAssert(onServerMainThread());
    serverAssert(listLength(saveInfo->u.repl.clients) > 0);

    /* Update checksum with any replication data already in the COB.
     * All forkless clients share the same COB content, so process once. */
    client *c = listNodeValue(listFirst(saveInfo->u.repl.clients));
    serverAssert(c->io_last_written.data_len == 0);

    if (c->bufpos > 0) {
        if (server.rdb_checksum) {
            saveInfo->save_rio.update_cksum(&saveInfo->save_rio, c->buf, c->bufpos);
        }
        saveInfo->save_rio.processed_bytes += c->bufpos;
    }

    listNode *ln;
    listIter li;
    listRewind(c->reply, &li);
    while ((ln = listNext(&li)) != NULL) {
        clientReplyBlock *replyBlock = listNodeValue(ln);
        if (server.rdb_checksum) {
            saveInfo->save_rio.update_cksum(&saveInfo->save_rio, replyBlock->buf, replyBlock->used);
        }
        saveInfo->save_rio.processed_bytes += replyBlock->used;
    }

    /* Write EOF, checksum, and eofmark into the COB */
    // After loading a for-sync save, the replica needs to continue replicating from the
    //  correct point in the replication stream.
    // If using socket based replication, the replication stream is included with the
    //  snapshot data.  The replica will continue after the last item seen.  Since this end
    //  marker is being written synchronously on the main thread, we could simply save replication
    //  AUX fields based on the latest replication staus on the main thread.
    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    serverAssert(rsiptr);
    if (rdbSaveInfoReplAuxFields(&saveInfo->save_rio, rsiptr) == -1) {
        serverLog(LL_WARNING, "forkless-save: error while writing AUX fields for replication, err=%s",
                strerror(errno));
        return C_ERR;
    }
    int err = rdbWriteFooter(&saveInfo->save_rio, REPLICA_REQ_NONE);
    if (rioWrite(&saveInfo->save_rio, saveInfo->u.repl.eofmark, RDB_EOF_MARK_SIZE) == 0) {
            serverLog(LL_WARNING, "forkless-save: error while writing valkey end eof string");
            return C_ERR;
        }
    rioFlush(&saveInfo->save_rio);

    saveInfo->bytes_written = saveInfo->save_rio.processed_bytes;

    if (err == C_OK) {
        // The COB is currently not sending.  At this point, we set a STOP position after the end
        //  marker and re-enable COB writes.
        listRewind(saveInfo->u.repl.clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);

            // Don't write past the current point in the COB
            pauseCobSendAtCurrentPositionForAck(c);

            // Start sending
            resumeReplicaWrites(c);
        }

        // REPLCONF will be sent immediately after ACK is received
        fixReplicationOffset(saveInfo);
    }

    rioFlush(&saveInfo->save_rio);  // Force from RIO buffer into COB
    rioFreeReplicaCOB(&saveInfo->save_rio);
    return err;
}

void forklessSaveComplete(bool terminated, void *privdata) {
    serverAssert(onServerMainThread());
    serverLog(LL_NOTICE, "forkless-save: completion proc - %s", (terminated) ? "terminated" : "ok");

    forklessSaveInfo *saveInfo = privdata;
    saveInfo->terminated = terminated;
    /* The save iterator should be terminated and freed at this point in time. */
    saveInfo->iterator = NULL;
    currentForklessSave = NULL;

    if (saveInfo->write_target == RDB_WRITE_TARGET_SOCKET) {
        /* Get rid of any clients which may have been closed after the bg thread completed. */
        freeRecentlyTerminatedClients(saveInfo);
        if (listLength(saveInfo->u.repl.clients) == 0) saveInfo->terminated = true;

        if (!saveInfo->terminated && saveInfo->err_code == C_OK) {
            /* Finish the RDB stream on the main thread using the COB. */
            int err = finishSocketBasedForklessSaveUsingCob(saveInfo);
            if (err != C_OK) saveInfo->terminated = true;
        } else {
            /* Error/terminated path: still need to free the COB rio. */
            rioFreeReplicaCOB(&saveInfo->save_rio);
        }

        resumeClientsAndFreeClientList(saveInfo, !saveInfo->terminated && saveInfo->err_code == C_OK);

        /* Shut down the replication monitor timer */
        mutexQueueAdd(saveInfo->foreground_queue, (void *)PROCESS_COMPLETE_ITEM);
        saveInfo->foreground_queue = NULL;

        cleanupSaveInfoAndEmitEndMetrics(saveInfo);
    } else {
        /* For file based forkless save, we need to generate the RDB end marker. and complete the save */
        if (!saveInfo->terminated && saveInfo->err_code == C_OK) {
            saveInfo->err_code = rdbWriteFooter(&saveInfo->save_rio, REPLICA_REQ_NONE) == C_ERR ? C_ERR : C_OK;
        }

        /* Done writing, capture bytes written (regardless of pass/fail) */
        saveInfo->bytes_written = saveInfo->save_rio.processed_bytes;

        /* Start a cron job to check for the background job completion */
        aeCreateTimeEvent(server.el, SNAPSHOT_FILE_CLOSE_MONITOR_INTERVAL_MS, snapshotEndMonitorTimeProc, saveInfo, NULL);
        /* Submit a background job to close and rename the snapshot file */
        saveInfo->foreground_queue = mutexQueueCreate(); // The monitor proc will delete this
        bioCreateLazyFreeJob(forklessSaveCloseSnapshotFile, 1, saveInfo);
        serverLog(LL_NOTICE, "forkless-save: created background thread to perform snapshot file close and rename");
        /* We will now wait for the background closeSnapshotFile job to complete.
        * The remainder of the cleanup will be performed in the snapshotEndMonitorTimeProc. */
    }
}

static int forklessSaveCommonStart(forklessSaveInfo *saveInfo) {
    serverAssert(onServerMainThread());

    saveInfo->cur_db = -1;

    serverLog(LL_NOTICE, "Using forkless save for next backup");
    rdbRecordStartMetrics(RDB_BGSAVE_TYPE_FORKLESS);
    startSaving(RDBFLAGS_FORKLESS_SAVE);

    rdbSaveInfo rsi, *rsiptr = rdbPopulateSaveInfo(&rsi);
    if (rdbWriteHeader(&saveInfo->save_rio, REPLICA_REQ_NONE, RDB_VERSION, RDBFLAGS_NONE, rsiptr) == C_ERR) return C_ERR;

    if (writeDbSizeHints(saveInfo) == C_ERR) return C_ERR;

    return C_OK;
}

static void startBackgroundThread(forklessSaveInfo *saveInfo) {
    serverAssert(onServerMainThread());

    pthread_t thread_id;
    pthread_attr_t attr;
    int pthread_rc;
    serverInitThreadAttribute(&attr);
    pthread_rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    serverAssert(pthread_rc == 0);
    pthread_rc = pthread_create(&thread_id, &attr, &forklessSaveProcessor, saveInfo);
    serverAssert(pthread_rc == 0);
    pthread_rc = pthread_attr_destroy(&attr);
    serverAssert(pthread_rc == 0);
}

/* Save a point-in-time snapshot to the given filename.
 * The filename must be under the server's current working directory.
 * Writes to a temp file and renames to the final filename on completion. */
int forklessSaveToDisk(const char *filename) {
    serverAssert(onServerMainThread());
    serverAssert(currentForklessSave == NULL);
    serverAssert(!isSaveInProgress());
    serverAssert(filename);
    serverLog(LL_NOTICE, "Beginning forklessSaveToDisk");

    server.stat_rdb_saves++;

    /* Use a forkless-specific name with a unique counter so the temp file can't
     * collide with a fork-based rdbSave() (same process) or another forkless
     * save. */
    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "temp-forkless-%d-%lld.rdb", (int)getpid(), (long long)server.stat_rdb_saves);

    FILE *file = fopen(tmpfile, "wb");
    if (file == NULL) {
        serverLog(LL_WARNING, "forkless-save: failed to open temp file [%s] for forkless save: %s",
                  tmpfile, strerror(errno));
        return C_ERR;
    }

    forklessSaveInfo *saveInfo = zcalloc(sizeof(forklessSaveInfo));
    saveInfo->u.file.temp_file = sdsnew(tmpfile);
    saveInfo->u.file.final_file = sdsnew(filename);
    saveInfo->write_target = RDB_WRITE_TARGET_DISK;

    rioInitWithFile(&saveInfo->save_rio, file);
    if (server.rdb_save_incremental_fsync) {
        rioSetAutoSync(&saveInfo->save_rio, REDIS_AUTOSYNC_BYTES);
        rioSetReclaimCache(&saveInfo->save_rio, 1);
    }

    int rc = forklessSaveCommonStart(saveInfo);
    if (rc != C_OK) goto werr;

    /* Saving to a file indicates a consistent snapshot (a backup at a point in time) */
    saveInfo->iterator = bgIteratorCreateFullScanIter(FORKLESS_SAVE_FILE_ITER_NAME,
                                                      BGITERATOR_CONSISTENCY_START, NULL, forklessSaveComplete, saveInfo);
    if (saveInfo->iterator == NULL) {
        serverLog(LL_WARNING, "forkless-save: error creating iterator");
        goto werr;
    }
    currentForklessSave = saveInfo;

    atomic_store_explicit(&server.stat_current_save_keys_total, dbTotalServerKeyCount(), memory_order_relaxed);
    atomic_store_explicit(&server.stat_current_save_keys_processed, 0, memory_order_relaxed);

    startBackgroundThread(saveInfo);

    /* at this point, background iteration has started (saveInfo will be freed later) */
    return C_OK;

werr:
    saveInfo->err_code = C_ERR;
    rdbRecordEndMetrics(RDB_BGSAVE_TYPE_FORKLESS, C_ERR, time(NULL));
    rdbClearSaveState(time(NULL));
    serverLog(LL_WARNING, "forkless-save: forkless save failed. %lld seconds.", (long long)server.rdb_save_time_last);
    stopSaving(0);
    currentForklessSave = NULL;

    if (file != NULL) {
        if (fclose(file) != 0) {
            serverLog(LL_WARNING, "forkless-save: Could not close temp file [%s]: %s",
                      saveInfo->u.file.temp_file, strerror(errno));
        }
        if (unlink(saveInfo->u.file.temp_file) != 0) {
            serverLog(LL_WARNING, "forkless-save: Could not delete temp file [%s]: %s",
                      saveInfo->u.file.temp_file, strerror(errno));
        }
    }
    sdsfree(saveInfo->u.file.temp_file);
    sdsfree(saveInfo->u.file.final_file);
    zfree(saveInfo);
    return C_ERR;
}

/* Cancels the currently running forkless save, if one is in progress. */
void forklessSaveCancel(void) {
    serverAssert(onServerMainThread());
    if (currentForklessSave == NULL) return;
    bgIteratorTerminate(currentForklessSave->iterator);
}

int isForklessSaveInProgress(void) {
    return server.cur_bgsave_type == RDB_BGSAVE_TYPE_FORKLESS;
}

/* Timer proc that runs on the main thread during socket-based forkless save.
 * The bg thread may need to abandon unresponsive replicas, but can't free
 * clients from a non-main thread. It queues them to foreground_queue, and
 * this timer frees them. Also handles the PROCESS_COMPLETE_ITEM sentinel
 * which signals the timer to stop. */
static long long replicationMonitorTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    serverAssert(onServerMainThread());

    mutexQueue *monitorQueue = clientData;

    void *item;
    while ((item = mutexQueuePop(monitorQueue, false)) != NULL) {
        if (item == PROCESS_COMPLETE_ITEM) {
            serverLog(LL_DEBUG, "forkless-save: replication monitor timer proc completing");
            mutexQueueRelease(monitorQueue);
            return AE_NOMORE;
        }

        client *c = item;
        serverLog(LL_WARNING, "forkless-save: client(%llu) ended replication early",
                  (unsigned long long)c->id);
        c->flag.forkless_managed = 0;
        if (c->repl_data) c->repl_data->using_cob = 0;
        if (c->repl_data) c->repl_data->repl_state = REPL_STATE_NONE;
        freeClient(c);
    }
    return REPLICATION_MONITOR_INTERVAL_MS;
}

/* Called on the main thread when the bg iterator finishes iterating all keys.
 * This is the point where the bg thread is done writing key data, but the
 * end marker hasn't been written yet (that happens in forklessSaveComplete via
 * finishSocketBasedForklessSaveUsingCob). */
static bool forklessSaveReplDone(void *privdata) {
    serverAssert(onServerMainThread());

    forklessSaveInfo *saveInfo = privdata;
    serverAssert(saveInfo->write_target == RDB_WRITE_TARGET_SOCKET);

    serverLog(LL_NOTICE, "forkless-save: replication done - primary_repl_offset: %lld",
              server.primary_repl_offset);

    listNode *ln;
    listIter li;
    listRewind(saveInfo->u.repl.clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        if (c->flag.close_asap) {
            serverLog(LL_NOTICE, "forkless-save: skipping client(%llu) marked for closure in repl done",
                      (unsigned long long)c->id);
            continue;
        }
        suspendReplicaWritesUntilAck(c);
    }

    return true;
}

int forklessSaveToSockets(void) {
    serverAssert(onServerMainThread());
    serverAssert(currentForklessSave == NULL);
    serverLog(LL_NOTICE, "Beginning forklessSaveToSockets");

    forklessSaveInfo *saveInfo = zmalloc(sizeof(forklessSaveInfo));
    memset(saveInfo, 0, sizeof(forklessSaveInfo));
    saveInfo->terminated = false;
    saveInfo->write_target = RDB_WRITE_TARGET_SOCKET;

    /* Collect replicas in WAIT_BGSAVE_END state (already set up by startBgsaveForReplication) */
    saveInfo->u.repl.clients = listCreate();
    listNode *ln;
    listIter li;
    listRewind(server.replicas, &li);
    while((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        serverAssert(c->repl_data);
        if (c->repl_data->repl_state == REPLICA_STATE_WAIT_BGSAVE_END) {
            serverLog(LL_NOTICE, "forkless-save: beginning save to client ID: %llu", (unsigned long long)c->id);
            listAddNodeTail(saveInfo->u.repl.clients, c);
            c->flag.forkless_managed = 1;
            c->repl_data->using_cob = 1;
        }
    }
    serverAssert(listLength(saveInfo->u.repl.clients) > 0);
    saveInfo->foreground_queue = NULL;

    /* Generate EOF marker for diskless sync */
    getRandomHexChars(saveInfo->u.repl.eofmark, RDB_EOF_MARK_SIZE);

    /* Initialize RIO with ReplicaCOB. This lets the main thread write sync metadata
     * to all pending replicas through a single rioWrite() call in a non-blocking
     * manner — it places the data into each replica's Client Output Buffer (COB),
     * and the event loop flushes it to the network asynchronously.
     *
     * Later, when the background thread takes over, it waits for the COBs to drain,
     * switches the connections to blocking mode, and begins writing directly to the
     * sockets — blocking is acceptable since it's no longer on the main thread. */
    rioInitWithReplicaCOB(&saveInfo->save_rio);

    int rc = forklessSaveCommonStart(saveInfo);
    if (rc != C_OK) goto werr;

    /* Flush the ReplicaCOB intermediate buffer into the actual COBs so the
     * background thread only needs to wait for COBs to drain. */
    if (!rioFlush(&saveInfo->save_rio)) {
        serverLog(LL_WARNING, "forkless-save: unable to flush replica COB before transition");
        goto werr;
    }

    /* Disable read handlers on replica connections. We don't want the main
     * thread processing REPLCONF ACK or other commands from replicas while
     * the bg thread is writing to their sockets. Read handlers are restored
     * in resumeRegularReplicaActivity(). */
    listRewind(saveInfo->u.repl.clients, &li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);
        connSetReadHandler(c->conn, NULL);
    }

    /* Create a timer to process abandoned clients from the bg thread.
     * The bg thread can't free clients directly — it queues them to
     * foreground_queue, and this timer frees them on the main thread. */
    saveInfo->foreground_queue = mutexQueueCreate();
    long long timeProcId = aeCreateTimeEvent(server.el, REPLICATION_MONITOR_INTERVAL_MS,
                       replicationMonitorTimeProc, saveInfo->foreground_queue, NULL);
    if (timeProcId == AE_ERR) {
        mutexQueueRelease(saveInfo->foreground_queue);
        saveInfo->foreground_queue = NULL;
        serverLog(LL_WARNING, "forkless-save: error creating replicationMonitorTimeProc");
        goto werr;
    }

    /* Create iterator with CONSISTENCY_EVENTUAL flag (no consistent snapshot needed).
     * forklessSaveReplDone is called on the main thread when iteration finishes. */
    saveInfo->iterator = bgIteratorCreateFullScanIter(FORKLESS_SOCKET_ITER_NAME,
            BGITERATOR_CONSISTENCY_EVENTUAL, forklessSaveReplDone, forklessSaveComplete, saveInfo);
    if (saveInfo->iterator == NULL) {
        serverLog(LL_WARNING, "forkless-save: error creating iterator");
        goto werr;
    }
    currentForklessSave = saveInfo;

    startBackgroundThread(saveInfo);

    return C_OK;

werr:
    currentForklessSave = NULL;
    if (saveInfo->foreground_queue) {
        /* Signal the monitor timer to stop */
        mutexQueueAdd(saveInfo->foreground_queue, (void *)PROCESS_COMPLETE_ITEM);
        saveInfo->foreground_queue = NULL;
    }
    if (saveInfo->u.repl.clients) {
        resumeClientsAndFreeClientList(saveInfo, false);
    }
    saveInfo->err_code = C_ERR;
    rdbRecordEndMetrics(RDB_BGSAVE_TYPE_FORKLESS, C_ERR, time(NULL));
    serverLog(LL_WARNING, "forkless-save: save failed.  %lld seconds.", (long long)server.rdb_save_time_last);
    stopSaving(0);
    zfree(saveInfo);
    serverLog(LL_WARNING, "Error in forklessSaveToSockets before starting thread");
    return C_ERR;
}

/* Appends forkless save INFO metrics to the provided sds string. */
sds forkless_catInfo(sds info) {
    long long estimated_seconds_remaining = -1;

    if (onServerMainThread()) {
        bgIterator *iter = bgIteratorFind(FORKLESS_SAVE_FILE_ITER_NAME);
        if (iter != NULL) {
            bgIteratorStatus status = {0};
            bgIteratorGetStatus(iter, &status);

            if (status.dbentries_processed > 0) {
                long long total_keys =
                    (long long)atomic_load_explicit(&server.stat_current_save_keys_total, memory_order_relaxed);
                /* The ETA is best effort. Clamp at 0 since dbentries_processed
                 * can exceed total_keys (e.g. a full sync may process more than
                 * the start-time key count). */
                long long remaining = max(total_keys - (long long)status.dbentries_processed, 0);
                estimated_seconds_remaining = remaining * status.runtime_ms / status.dbentries_processed / 1000;
            }
        }
    }

    return sdscatprintf(info, "forkless_estimated_seconds_remaining:%lld\r\n", estimated_seconds_remaining);
}

/* Appends forkless debug metrics to the provided sds string. */
sds forkless_catDebugInfo(sds info) {
    bgIteratorStatus status = {0};
    long long current_item_ms = -1;

    if (onServerMainThread()) {
        bgIterator *iter = bgIteratorFind(FORKLESS_SAVE_FILE_ITER_NAME);
        if (iter != NULL) {
            bgIteratorGetStatus(iter, &status);
            current_item_ms = status.current_item_ms;
        }
    }

    return sdscatprintf(info,
                        "forkless_current_item_ms:%lld\r\n"
                        "forkless_current_queue_length:%lu\r\n"
                        "forkless_queue_length_target:%lu\r\n"
                        "forkless_dbentries_queued:%lu\r\n"
                        "forkless_dbentries_processed:%lu\r\n",
                        current_item_ms,
                        status.queue_length,
                        status.queue_length_target,
                        status.dbentries_queued,
                        status.dbentries_processed);
}
