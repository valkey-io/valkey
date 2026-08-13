#include "forkless.h"
#include "server.h"
#include "bgiteration.h"
#include "mutexqueue.h"
#include "rdb.h"
#include "bio.h"

static const void *PROCESS_COMPLETE_ITEM = (void *)-1;
static const int SNAPSHOT_FILE_CLOSE_MONITOR_INTERVAL_MS = 200;

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
} forklessSaveInfo;

/* Keep a global indicator of the current iterator (for cancellation purposes). */
static forklessSaveInfo *currentForklessSave = NULL;

/* rio check_abort_between_writes callback: checks if the forkless save iterator is being terminated. */
static int forklessSaveShouldAbort(rio *r) {
    static_assert(offsetof(forklessSaveInfo, save_rio) == 0, "rio must be castable to forklessSaveInfo");
    forklessSaveInfo *saveInfo = (forklessSaveInfo *)r;
    return saveInfo->iterator && bgIteratorIsTerminating(saveInfo->iterator);
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

/* Entry point for background thread.
 * Upon entering:
 *  - The RDB header has been written (magic, aux fields, functions)
 *  - The DB size hints have been written
 * This function is responsible for writing all of the dictionary entries. */
static void *forklessSaveProcessor(void *arg) {
    serverAssert(!onValkeyMainThread());
    forklessSaveInfo *saveInfo = arg;

    serverLog(LL_NOTICE, "forkless-save: background processor started");
    int err = C_OK;

    saveInfo->save_rio.check_abort_between_writes = forklessSaveShouldAbort;

    const unsigned statsIntervalMs = 1000;
    monotime lastStatsTime;
    elapsedStart(&lastStatsTime);

    bool done = false;
    bool terminated = false;
    long items = 0;
    while (!done && err == C_OK) {
        bgIteratorItem *item = bgIteratorRead(saveInfo->iterator);

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

    if (err != C_OK && bgIteratorIsTerminating(saveInfo->iterator)) {
        /* We recognized termination before receiving the TERMINATED event */
        terminated = true;
    }

    char *message = "";
    if (terminated)
        message = "TERMINATED";
    else if (err != C_OK)
        message = "***ERROR***";
    serverLog(LL_NOTICE, "forkless-save: background processor finished. %ld items processed. %s",
              items, message);

    currentForklessSave = NULL;
    saveInfo->err_code = err;
    bgIteratorClose(saveInfo->iterator);
    return NULL;
}

static void cleanupSaveInfoAndEmitEndMetrics(forklessSaveInfo *saveInfo) {
    if (saveInfo->terminated && saveInfo->err_code == C_OK) saveInfo->err_code = C_ERR;
    rdbRecordEndMetrics(RDB_BGSAVE_TYPE_FORKLESS, saveInfo->err_code, time(NULL));
    rdbClearSaveState(time(NULL));
    if (saveInfo->err_code == C_OK) {
        serverLog(LL_NOTICE, "forkless-save: forkless save complete. %lld seconds.", (long long)server.rdb_save_time_last);
    } else if (saveInfo->terminated) {
        serverLog(LL_WARNING, "forkless-save: forkless save terminated. %lld seconds.", (long long)server.rdb_save_time_last);
    } else {
        serverLog(LL_WARNING, "forkless-save: forkless save failed. %lld seconds.", (long long)server.rdb_save_time_last);
    }
    stopSaving(saveInfo->err_code == C_OK);
    currentForklessSave = NULL;
    atomic_store_explicit(&server.stat_current_save_keys_processed, 0, memory_order_relaxed);
    atomic_store_explicit(&server.stat_current_save_keys_total, 0, memory_order_relaxed);

    serverAssert(saveInfo->temp_file == NULL);
    zfree(saveInfo);
}

/* Routine for background thread to close and rename the forkless save snapshot file.
 * Closing the file requires synchronously flushing the content to disk, which can
 * take some time. */
static void forklessSaveCloseSnapshotFile(void *args[]) {
    serverAssert(!onValkeyMainThread());
    forklessSaveInfo *saveInfo = (forklessSaveInfo *)args[0];
    /* Error or not, close the file... */
    if (fsync(fileno(saveInfo->save_rio.io.file.fp)) != 0) {
        serverLog(LL_WARNING, "forkless-save: error fsyncing temp file [%s]: %s",
                  saveInfo->temp_file, strerror(errno));
        saveInfo->err_code = C_ERR;
    }
    if (fclose(saveInfo->save_rio.io.file.fp) != 0) {
        serverLog(LL_WARNING, "forkless-save: error closing temp file [%s]: %s",
                  saveInfo->temp_file, strerror(errno));
        saveInfo->err_code = C_ERR;
    }

    if (!saveInfo->terminated && saveInfo->err_code == C_OK) {
        if (rename(saveInfo->temp_file, saveInfo->final_file) != 0) {
            serverLog(LL_WARNING, "forkless-save: error moving temp file [%s] to destination [%s]: %s",
                      saveInfo->temp_file, saveInfo->final_file, strerror(errno));
            saveInfo->err_code = C_ERR;
        }
    }

    if (saveInfo->terminated || saveInfo->err_code != C_OK) {
        rdbRemoveTempFile(getpid(), 0);
    }
    sdsfree(saveInfo->temp_file);
    sdsfree(saveInfo->final_file);
    saveInfo->temp_file = NULL;
    saveInfo->final_file = NULL;
    /* Notify the main thread that I am done closing the file. */
    mutexQueueAdd(saveInfo->foreground_queue, (void *)PROCESS_COMPLETE_ITEM);
}

/* Timer proc which runs in the main valkey event loop. It monitors to see when the background thread
 * completes the action to close and rename the snapshot file at the end of disk based forkless save,
 * and performs the final clean-up actions. */
static long long snapshotEndMonitorTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    serverAssert(onValkeyMainThread());

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

void forklessSaveComplete(bool terminated, void *privdata) {
    serverAssert(onValkeyMainThread());
    serverLog(LL_NOTICE, "forkless-save: completion proc - %s", (terminated) ? "terminated" : "ok");

    forklessSaveInfo *saveInfo = privdata;
    saveInfo->terminated = terminated;
    /* The save iterator should be terminated and freed at this point in time. */
    saveInfo->iterator = NULL;
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

static int forklessSaveCommonStart(forklessSaveInfo *saveInfo) {
    serverAssert(onValkeyMainThread());

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
    serverAssert(onValkeyMainThread());

    pthread_t thread_id;
    pthread_attr_t attr;
    int pthread_rc;
    pthread_rc = pthread_attr_init(&attr);
    serverAssert(pthread_rc == 0);
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
    serverAssert(onValkeyMainThread());
    serverAssert(currentForklessSave == NULL);
    serverAssert(!isSaveInProgress());
    serverAssert(filename);
    serverLog(LL_NOTICE, "Beginning forklessSaveToDisk");

    server.stat_rdb_saves++;

    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "temp-%d.rdb", (int)getpid());

    FILE *file = fopen(tmpfile, "wb");
    if (file == NULL) {
        serverLog(LL_WARNING, "forkless-save: failed to open temp file [%s] for forkless save: %s",
                  tmpfile, strerror(errno));
        return C_ERR;
    }

    forklessSaveInfo *saveInfo = zcalloc(sizeof(forklessSaveInfo));
    saveInfo->temp_file = sdsnew(tmpfile);
    saveInfo->final_file = sdsnew(filename);

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
                      saveInfo->temp_file, strerror(errno));
        }
        if (unlink(saveInfo->temp_file) != 0) {
            serverLog(LL_WARNING, "forkless-save: Could not delete temp file [%s]: %s",
                      saveInfo->temp_file, strerror(errno));
        }
    }
    sdsfree(saveInfo->temp_file);
    sdsfree(saveInfo->final_file);
    zfree(saveInfo);
    return C_ERR;
}

/* Cancels the currently running forkless save, if one is in progress. */
void forklessSaveCancel(void) {
    serverAssert(onValkeyMainThread());
    if (currentForklessSave == NULL) return;
    bgIteratorTerminate(currentForklessSave->iterator);
}

int isForklessSaveInProgress(void) {
    return server.cur_bgsave_type == RDB_BGSAVE_TYPE_FORKLESS;
}

/* Appends forkless save INFO metrics to the provided sds string. */
sds forkless_catInfo(sds info) {
    long long estimated_seconds_remaining = -1;
    long long current_item_millis = -1;

    if (onValkeyMainThread()) {
        bgIterator *iter = bgIteratorFind(FORKLESS_SAVE_FILE_ITER_NAME);
        if (iter != NULL) {
            bgIteratorStatus status = {0};
            bgIteratorGetStatus(iter, &status);
            current_item_millis = status.current_item_ms;

            if (status.dbentries_processed > 0) {
                long long total_keys = 0;
                for (int i = 0; i < server.dbnum; i++) {
                    total_keys += server.db[i] ? dbSize(server.db[i]) : 0;
                }
                estimated_seconds_remaining = (total_keys - status.dbentries_processed) *
                                              status.runtime_ms / status.dbentries_processed / 1000;
            }
        }
    }

    return sdscatprintf(info,
                        "forkless_current_item_millis:%lld\r\n"
                        "forkless_estimated_seconds_remaining:%lld\r\n",
                        current_item_millis,
                        estimated_seconds_remaining);
}

/* Appends forkless debug metrics to the provided sds string. */
sds forkless_catDebugInfo(sds info) {
    bgIteratorStatus status = {0};

    if (onValkeyMainThread()) {
        bgIterator *iter = bgIteratorFind(FORKLESS_SAVE_FILE_ITER_NAME);
        if (iter != NULL) bgIteratorGetStatus(iter, &status);
    }

    return sdscatprintf(info,
                        "forkless_current_queue_length:%lu\r\n"
                        "forkless_queue_length_target:%lu\r\n"
                        "forkless_dbentries_queued:%lu\r\n"
                        "forkless_dbentries_processed:%lu\r\n",
                        status.queue_length,
                        status.queue_length_target,
                        status.dbentries_queued,
                        status.dbentries_processed);
}
