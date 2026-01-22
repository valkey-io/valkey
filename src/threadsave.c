#include "threadsave.h"
#include "server.h"
#include "bgiteration.h"
#include "mutexqueue.h"
#include <assert.h>
#include "rdb.h"
#include "bio.h"

static const void *PROCESS_COMPLETE_ITEM = (void *)-1;
static const int SNAPSHOT_FILE_CLOSE_MONITOR_INTERVAL_MS = 200;

typedef struct {
    rio save_rio;                                                       /* Must be 1st to permit cast from rio back to threadsaveInfo */
    int cur_db;                                                         /* Last selectDb issued */
    size_t (*realRioWrite)(struct _rio *, const void *buf, size_t len); /* See rioWriteWrapper */
    bgIterator *iterator;
    uint64_t bytes_written;
    int err_code;
    mutexQueue *foreground_queue;
    bool terminated;
    sds temp_file;
    sds final_file;
} threadsaveInfo;

/* Keep a global indicator of the current iterator (for cancellation purposes). */
static threadsaveInfo *currentThreadsave = NULL;

/* When saving a large object, there is no mechanism to break out and perform periodic status
 * checks. To get around this, the rioWrite routine is replaced with this function. The original
 * write routine is saved in the threadsaveInfo. */
static size_t rioWriteWrapper(rio *r, const void *buf, size_t len) {
    static_assert(offsetof(threadsaveInfo, save_rio) == 0, "rio must be castable to threadsaveInfo");
    threadsaveInfo *saveInfo = (threadsaveInfo *)r;

    if (saveInfo->iterator && bgIteratorIsTerminating(saveInfo->iterator)) return 0; // indicate error

    return saveInfo->realRioWrite(r, buf, len);
}

static void installRioWriteWrapper(threadsaveInfo *saveInfo) {
    serverAssert(saveInfo->realRioWrite == NULL);
    saveInfo->realRioWrite = saveInfo->save_rio.write;
    saveInfo->save_rio.write = rioWriteWrapper;
}

static int writeSelectDb(threadsaveInfo *saveInfo, int new_db) {
    if (new_db == saveInfo->cur_db) return C_OK;

    if (rdbSaveType(&saveInfo->save_rio, RDB_OPCODE_SELECTDB) == -1) {
        serverLog(LL_WARNING, "threadsave: error while writing OPCODE_SELECTDB");
        return C_ERR;
    }
    if (rdbSaveLen(&saveInfo->save_rio, new_db) == -1) {
        serverLog(LL_WARNING, "threadsave: error while writing selectDb value");
        return C_ERR;
    }
    saveInfo->cur_db = new_db;
    return C_OK;
}

static int writeDbSizeHints(threadsaveInfo *saveInfo) {
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
static void *threadsaveProcessor(void *arg) {
    serverAssert(!onValkeyMainThread());
    threadsaveInfo *saveInfo = arg;

    serverLog(LL_NOTICE, "threadsave: background processor started");
    int err = C_OK;

    installRioWriteWrapper(saveInfo);

    const unsigned progressIntervalSecs = 120;
    monotime lastStatusTime;
    elapsedStart(&lastStatusTime);

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
                serverLog(LL_WARNING, "threadsave: error writing KV pair");
                err = C_ERR;
            }
            break;
        default:
            /* bgIteration may deliver item types that are not necessarily relevant to us.
             * New types may also be added in the future. It is the client's responsibility
             * to filter out irrelevant types, so we simply ignore them here. */
            break;
        }

        if (elapsedMs(lastStatusTime) >= progressIntervalSecs * 1000) {
            elapsedStart(&lastStatusTime);
            serverLog(LL_NOTICE, "threadsave: progress - keys processed: %ld", items);
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
    serverLog(LL_NOTICE, "threadsave: background processor finished.  %ld items processed.  %s",
              items, message);

    currentThreadsave = NULL;
    saveInfo->err_code = err;
    bgIteratorClose(saveInfo->iterator);
    return NULL;
}

static void cleanupSaveInfoAndEmitEndMetrics(threadsaveInfo *saveInfo) {
    if (saveInfo->terminated && saveInfo->err_code == C_OK) saveInfo->err_code = C_ERR;
    rdbRecordEndMetrics(RDB_BGSAVE_TYPE_THREAD, saveInfo->err_code, time(NULL));
    if (saveInfo->err_code == C_OK) {
        serverLog(LL_NOTICE, "threadsave: threadsave complete.  %lld seconds.", (long long)server.rdb_save_time_last);
    } else if (saveInfo->terminated) {
        serverLog(LL_WARNING, "threadsave: threadsave terminated.  %lld seconds.", (long long)server.rdb_save_time_last);
    } else {
        serverLog(LL_WARNING, "threadsave: threadsave failed.  %lld seconds.", (long long)server.rdb_save_time_last);
    }
    stopSaving(saveInfo->err_code == C_OK);
    currentThreadsave = NULL;

    serverAssert(saveInfo->temp_file == NULL);
    zfree(saveInfo);
}

/* Routine for background thread to close and rename the threadsave snapshot file.
 * Closing the file requires synchronously flushing the content to disk, which can
 * take some time. */
static void threadsaveCloseSnapshotFile(void *args[]) {
    serverAssert(!onValkeyMainThread());
    threadsaveInfo *saveInfo = (threadsaveInfo *)args[0];
    /* Error or not, close the file... */
    if (fsync(fileno(saveInfo->save_rio.io.file.fp)) != 0) {
        serverLog(LL_WARNING, "threadsave: error fsyncing temp file [%s]: %s",
                  saveInfo->temp_file, strerror(errno));
        saveInfo->err_code = C_ERR;
    }
    if (fclose(saveInfo->save_rio.io.file.fp) != 0) {
        serverLog(LL_WARNING, "threadsave: error closing temp file [%s]: %s",
                  saveInfo->temp_file, strerror(errno));
        saveInfo->err_code = C_ERR;
    }

    if (!saveInfo->terminated && saveInfo->err_code == C_OK) {
        if (rename(saveInfo->temp_file, saveInfo->final_file) != 0) {
            serverLog(LL_WARNING, "threadsave: error moving temp file [%s] to destination [%s]: %s",
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

/* Timer proc which runs in the main redis event loop. It monitors to see when the background thread
 * completes the action to close and rename the snapshot file at the end of disk based threadsave,
 * and performs the final clean-up actions. */
static long long snapshotEndMonitorTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    UNUSED(eventLoop);
    UNUSED(id);
    serverAssert(onValkeyMainThread());

    threadsaveInfo *saveInfo = (threadsaveInfo *)clientData;

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

void threadsaveComplete(bool terminated, void *privdata) {
    serverAssert(onValkeyMainThread());
    serverLog(LL_NOTICE, "threadsave: completion proc - %s", (terminated) ? "terminated" : "ok");

    threadsaveInfo *saveInfo = privdata;
    saveInfo->terminated = terminated;
    /* The save iterator should be terminated and freed at this point in time. */
    saveInfo->iterator = NULL;
    /* For file based threadsave, we need to generate the RDB end marker. and complete the save */
    if (!saveInfo->terminated && saveInfo->err_code == C_OK) {
        saveInfo->err_code = rdbWriteFooter(&saveInfo->save_rio, REPLICA_REQ_NONE) == C_ERR ? C_ERR : C_OK;
    }

    /* Done writing, capture bytes written (regardless of pass/fail) */
    saveInfo->bytes_written = saveInfo->save_rio.processed_bytes;

    /* Start a cron job to check for the background job completion */
    aeCreateTimeEvent(server.el, SNAPSHOT_FILE_CLOSE_MONITOR_INTERVAL_MS, snapshotEndMonitorTimeProc, saveInfo, NULL);
    /* Submit a background job to close and rename the snapshot file */
    saveInfo->foreground_queue = mutexQueueCreate(); // The monitor proc will delete this
    bioCreateLazyFreeJob(threadsaveCloseSnapshotFile, 1, saveInfo);
    serverLog(LL_NOTICE, "threadsave: created background thread to perform snapshot file close and rename");
    /* We will now wait for the background closeSnapshotFile job to complete.
     * The remainder of the cleanup will be performed in the snapshotEndMonitorTimeProc. */
}

static int threadsaveCommonStart(threadsaveInfo *saveInfo) {
    serverAssert(onValkeyMainThread());

    saveInfo->cur_db = -1;

    serverLog(LL_NOTICE, "Using Threadsave for next backup");
    rdbRecordStartMetrics(RDB_BGSAVE_TYPE_THREAD);
    startSaving(RDBFLAGS_THREADSAVE);

    rdbSaveInfo rsi, *rsiptr = rdbPopulateSaveInfo(&rsi);
    if (rdbWriteHeader(&saveInfo->save_rio, REPLICA_REQ_NONE, RDB_VERSION, RDBFLAGS_NONE, rsiptr) == C_ERR) return C_ERR;

    if (writeDbSizeHints(saveInfo) == C_ERR) return C_ERR;

    return C_OK;
}

static void startBackgroundThread(threadsaveInfo *saveInfo) {
    serverAssert(onValkeyMainThread());

    pthread_t thread_id;
    pthread_attr_t attr;
    int pthread_rc;
    pthread_rc = pthread_attr_init(&attr);
    serverAssert(pthread_rc == 0);
    pthread_rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    serverAssert(pthread_rc == 0);
    pthread_rc = pthread_create(&thread_id, &attr, &threadsaveProcessor, saveInfo);
    serverAssert(pthread_rc == 0);
    pthread_rc = pthread_attr_destroy(&attr);
    serverAssert(pthread_rc == 0);
}

/* Save a point-in-time snapshot to the given filename.
 * The filename must be under the server's current working directory.
 * Writes to a temp file and renames to the final filename on completion. */
int threadsaveToDisk(const char *filename) {
    serverAssert(onValkeyMainThread());
    serverAssert(currentThreadsave == NULL);
    serverAssert(!isSaveInProgress());
    serverAssert(filename);
    serverLog(LL_NOTICE, "Beginning threadsaveToDisk");

    threadsaveInfo *saveInfo = zcalloc(sizeof(threadsaveInfo));

    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "temp-%d.rdb", (int)getpid());
    saveInfo->temp_file = sdsnew(tmpfile);
    saveInfo->final_file = sdsnew(filename);

    FILE *file = fopen(saveInfo->temp_file, "wb");
    if (file == NULL) {
        serverLog(LL_WARNING, "threadsave: failed to open temp file [%s] for threadsave: %s",
                  saveInfo->temp_file, strerror(errno));
        goto werr;
    }

    rioInitWithFile(&saveInfo->save_rio, file);
    if (server.rdb_save_incremental_fsync) {
        rioSetAutoSync(&saveInfo->save_rio, REDIS_AUTOSYNC_BYTES);
        rioSetReclaimCache(&saveInfo->save_rio, 1);
    }

    int rc = threadsaveCommonStart(saveInfo);
    if (rc != C_OK) goto werr;

    /* Saving to a file indicates a consistent snapshot (a backup at a point in time) */
    saveInfo->iterator = bgIteratorCreateFullScanIter(THREADSAVE_FILE_ITER_NAME,
                                                      BGITERATOR_CONSISTENCY_START, NULL, threadsaveComplete, saveInfo);
    if (saveInfo->iterator == NULL) {
        serverLog(LL_WARNING, "threadsave: error creating iterator");
        goto werr;
    }
    currentThreadsave = saveInfo;

    startBackgroundThread(saveInfo);

    /* at this point, background iteration has started (saveInfo will be freed later) */
    return C_OK;

werr:
    saveInfo->err_code = C_ERR;
    rdbRecordEndMetrics(RDB_BGSAVE_TYPE_THREAD, C_ERR, time(NULL));
    serverLog(LL_WARNING, "threadsave: threadsave failed.  %lld seconds.", (long long)server.rdb_save_time_last);
    stopSaving(0);
    currentThreadsave = NULL;

    if (file != NULL) {
        if (fclose(file) != 0) {
            serverLog(LL_WARNING, "threadsave: Could not close temp file [%s]: %s",
                      saveInfo->temp_file, strerror(errno));
        }
        if (unlink(saveInfo->temp_file) != 0) {
            serverLog(LL_WARNING, "threadsave: Could not delete temp file [%s]: %s",
                      saveInfo->temp_file, strerror(errno));
        }
    }
    sdsfree(saveInfo->temp_file);
    sdsfree(saveInfo->final_file);
    zfree(saveInfo);
    return C_ERR;
}

/* Cancels the currently running threadsave, if one is in progress. */
void threadsaveCancel(void) {
    serverAssert(onValkeyMainThread());
    if (currentThreadsave == NULL) return;
    bgIteratorTerminate(currentThreadsave->iterator);
}
