/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "thread_common.h"
#include "rdb_threads.h"
#include "rdb.h"
#include "module.h"

static pthread_t rdb_threads[RDB_THREADS_MAX_NUM] = {0};
static pthread_mutex_t rdb_threads_mutex[RDB_THREADS_MAX_NUM];
JobQueue rdb_jobs[RDB_THREADS_MAX_NUM] = {0}; // Job queues for each RDB worker thread.


/* --------- RDB Worker Threads Core Logic --------- */

static void *RDBThreadMain(void *myid) {
    /* The ID is the thread ID number (from 1 to server.rdb_threads_num-1). ID 0 is the main thread. */
    long id = (long)myid;
    char thdname[32];
    thread_id = (int)id; // Thread local var defined in thread_common.h

    snprintf(thdname, sizeof(thdname), "rdb_thd_%ld", id);
    valkey_set_thread_title(thdname);

    /* Ensure the thread's static compression buffer is freed on exit or cancellation. */
    pthread_cleanup_push(freeThreadCompressionBuffer, NULL);

    /*
        Note: CPU Affinity for rdb save cab be added here using:
        'serverSetCpuAffinity(server.rdb_threads_cpulist)'
    */
    size_t jobs_to_process = 0;
    JobQueue *jq = &rdb_jobs[id];
    while (1) {
        /* Cancellation point so that pthread_cancel() from main thread is honored. */
        pthread_testcancel();

        /* Wait for jobs */
        for (int j = 0; j < 1000000; j++) {
            jobs_to_process = JobQueue_availableJobs(jq);
            if (jobs_to_process) break;
        }

        /* Give the main thread a chance to stop this thread. */
        if (jobs_to_process == 0) {
            pthread_mutex_lock(&rdb_threads_mutex[id]);
            pthread_mutex_unlock(&rdb_threads_mutex[id]);
            continue;
        }

        for (size_t j = 0; j < jobs_to_process; j++) {
            job_handler handler;
            void *data;
            /* We keep the job in the queue until it's processed. This ensures that if the main thread checks
             * and finds the queue empty, it can be certain that the RDB thread is not currently handling any job. */
            JobQueue_peek(jq, &handler, &data);
            handler(data);
            /* Remove the job after it was processed */
            JobQueue_removeJob(jq);
        }
        /* Memory barrier to make sure the main thread sees the updated tail index.
         * We do it once per loop and not per tail-update for optimization reasons.
         * As the main-thread main concern is to check if the queue is empty, it's enough to do it once at the end. */
        atomic_thread_fence(memory_order_release);
    }

    pthread_cleanup_pop(0);

    return NULL;
}


/* --------- RDB Threads Lifecycle Management --------- */

/* We only need a small queue for RDB Save because we processes hashtables sequentially (in cluster mode).
 * RDB Load needs larger queues. */
static void createRDBThread(int id, int job_queue_size) {
    serverAssert(server.rdb_threads_num > 0);
    serverAssert(id > 0 && id < server.rdb_threads_num);

    pthread_t tid;
    pthread_mutex_init(&rdb_threads_mutex[id], NULL);
    JobQueue_init(&rdb_jobs[id], job_queue_size);
    pthread_mutex_lock(&rdb_threads_mutex[id]); /* Thread starts paused */
    if (pthread_create(&tid, NULL, RDBThreadMain, (void *)(long)id) != 0) {
        serverLog(LL_WARNING, "Fatal: Can't initialize RDB thread, pthread_create failed with: %s", strerror(errno));
        exit(1);
    }
    rdb_threads[id] = tid;
}

/* Terminates the RDB thread specified by id */
static void shutdownRDBThread(int id) {
    int err;
    pthread_t tid = rdb_threads[id];
    if (tid == pthread_self()) return;
    if (tid == 0) return;
    pthread_mutex_unlock(&rdb_threads_mutex[id]);

    pthread_cancel(tid);

    if ((err = pthread_join(tid, NULL)) != 0) {
        serverLog(LL_WARNING, "RDB thread(tid:%lu) can not be joined: %s", (unsigned long)tid, strerror(err));
    } else {
        serverLog(LL_DEBUG, "RDB thread(tid:%lu) terminated", (unsigned long)tid);
    }
    pthread_mutex_destroy(&rdb_threads_mutex[id]);
    JobQueue_cleanup(&rdb_jobs[id]);
}

/* Terminates all RDB Worker Threads. Called when RDB Save or Load has completed */
void killRDBThreads(void) {
    for (int j = 1; j < server.rdb_threads_num; j++) { /* We don't kill thread 0, which is the main thread. */
        shutdownRDBThread(j);
    }
}

void initRDBThreads(int per_thread_queue_size) {
    if (server.rdb_threads_num == 1) return;
    serverAssert(server.rdb_threads_num <= RDB_THREADS_MAX_NUM);

    /* Spawn and initialize the RDB threads. */
    for (int i = 1; i < server.rdb_threads_num; i++) {
        createRDBThread(i, per_thread_queue_size);
    }
}

void startRDBThreads(void) {
    for (int id = 1; id < server.rdb_threads_num; id++) {
        pthread_mutex_unlock(&rdb_threads_mutex[id]);
    }
}

void stopRDBThreads(void) {
    for (int id = 1; id < server.rdb_threads_num; id++) {
        pthread_mutex_lock(&rdb_threads_mutex[id]);
    }
}


/* --------- Multithreaded RDB Save: Thread Argument Management --------- */

static RdbSaveThreadArgs *createRdbSaveThreadArgs(int num_threads, int dbid, rio *target_rio, long *key_counter, char *pname, long long *info_updated_time) {
    RdbSaveThreadArgs *threadArgs = zcalloc(num_threads * sizeof(RdbSaveThreadArgs));

    pthread_mutex_t *shared_rdb_write_mutex = zmalloc(sizeof(pthread_mutex_t)); /* Shared access to the rdb */
    pthread_mutex_init(shared_rdb_write_mutex, NULL);

    for (int i = 0; i < num_threads; i++) {
        threadArgs[i].dbid = dbid;
        threadArgs[i].ht = NULL; // Set by the main thread in rdbSaveDbMultiThreaded for each hashtable in the database
        threadArgs[i].bucket_stride = (BucketStride){.start_index = i, .stride_size = num_threads};
        atomic_init(&threadArgs[i].keys_processed, 0);
        rioInitWithBufferToTarget(&threadArgs[i].buf_to_target_rio, sdsnewlen(SDS_NOINIT, WORKER_BUFFER_DEFAULT_SIZE), WORKER_BUFFER_CAPACITY_LIMIT, target_rio, shared_rdb_write_mutex);
        threadArgs[i].rdb_write_mutex = shared_rdb_write_mutex;
        threadArgs[i].save_status = C_OK;

        /* The main thread needs this information to report the save progress */
        if (i == 0) {
            threadArgs[i].main_thread_report_info = zcalloc(sizeof(MainThreadRdbInfo));
            threadArgs[i].main_thread_report_info->info_updated_time = info_updated_time;
            threadArgs[i].main_thread_report_info->last_key_counter = key_counter;
            threadArgs[i].main_thread_report_info->pname = pname;
            threadArgs[i].main_thread_report_info->threadArgs = threadArgs;
        } else {
            threadArgs[i].main_thread_report_info = NULL;
        }
    }
    return threadArgs;
}

static void freeRdbSaveThreadArgs(int num_threads, RdbSaveThreadArgs *threadArgs) {
    serverAssert(threadArgs != NULL);

    for (int i = 0; i < num_threads; i++) {
        sdsfree(threadArgs[i].buf_to_target_rio.io.buf_to_target.ptr);
    }
    /* Free the shared write mutex one time*/
    pthread_mutex_destroy(threadArgs[0].rdb_write_mutex);
    zfree(threadArgs[0].rdb_write_mutex);

    /* Free the MainThreadRdbInfo */
    zfree(threadArgs[0].main_thread_report_info);

    zfree(threadArgs);
}


/* --------- Multithreaded RDB Save: Worker Job Handler & Helpers --------- */

/* Job handler for RDB worker threads: encodes a range of hashtable buckets. */
void rdbEncodeHashtableRange(void *arg) {
    RdbSaveThreadArgs *args = (RdbSaveThreadArgs *)arg;
    serverDb *db = server.db[args->dbid];
    hashtable *ht = args->ht;
    BucketStride *bucket_stride = &args->bucket_stride;
    rio *buf_to_target_rio = &args->buf_to_target_rio;

    hashtableIterator ht_iter;
    hashtableInitIterator(&ht_iter, ht, HASHTABLE_ITER_PREFETCH_VALUES);
    void *next;
    ssize_t res;

    /* Iterate through hashtable buckets assigned to this thread and encode keys/values. */
    while (hashtableStrideNext(&ht_iter, &next, bucket_stride->start_index, bucket_stride->stride_size)) {
        robj *o = next;
        sds keystr = objectGetKey(o);
        robj key;
        long long expire;
        size_t processed_bytes_before = buf_to_target_rio->processed_bytes;

        initStaticStringObject(key, keystr);
        expire = getExpire(db, &key);

        /* Attempt to write key-value pair to the thread's local buffer. */
        res = rdbSaveKeyValuePair(buf_to_target_rio, &key, o, expire, args->dbid);
        size_t processed_bytes_after = buf_to_target_rio->processed_bytes;

        if (res < 0) {
            /* Release shared lock if we acquired it */
            if (buf_to_target_rio->io.buf_to_target.cap_reached) {
                pthread_mutex_unlock(buf_to_target_rio->io.buf_to_target.target_rio_mutex);
            }
            goto werr;
        }
        /* Our write was successful. Check if we hit the memory cap while writing this key */
        if (buf_to_target_rio->io.buf_to_target.cap_reached) {
            /* If we hit the memory cap during the call to rdbSaveKeyValuePair we need too:
                1. Unlock the mutex that was acquired in rioBufferToTargetWrite
                2. Clear the buffer since it was already written to the file in rioBufferToTargetWrite
            */
            pthread_mutex_unlock(args->rdb_write_mutex);

            sdsclear(buf_to_target_rio->io.buf_to_target.ptr);
            buf_to_target_rio->io.buf_to_target.cap_reached = 0;
            buf_to_target_rio->io.buf_to_target.pos = 0;

        } else if ((size_t)buf_to_target_rio->io.buf_to_target.pos > (size_t)WORKER_BUFFER_DEFAULT_SIZE) {
            /* We did not hit the memory cap, but we have buffered enough data to write out*/
            if (rioFlush(buf_to_target_rio) == 0) goto werr;
        }

        args->bytes_written += res;
        atomic_fetch_add_explicit(&args->keys_processed, 1, memory_order_release);

        /* In fork child process, we can try to release memory back to the
         * OS and possibly avoid or decrease COW. We give the dismiss
         * mechanism a hint about an estimated size of the object we stored. */
        size_t dump_size = processed_bytes_after - processed_bytes_before;
        if (server.in_fork_child) dismissObject(o, dump_size);

        /* Main thread only: update parent process with progress. */
        if (inMainThread()) {
            MainThreadRdbInfo *reporting_info = args->main_thread_report_info;
            long total_keys_processed = 0;
            for (int i = 0; i < server.rdb_threads_num; i++) {
                total_keys_processed += atomic_load_explicit(&reporting_info->threadArgs[i].keys_processed, memory_order_relaxed);
            }

            /* Update child info periodically to avoid excessive `mstime()` calls and parent notifications. */
            if ((total_keys_processed - *reporting_info->last_key_counter) > 1023) {
                long long now = mstime();
                if (now - *reporting_info->info_updated_time >= 1000) {
                    *reporting_info->last_key_counter = total_keys_processed;
                    sendChildInfo(CHILD_INFO_TYPE_CURRENT_INFO, total_keys_processed, reporting_info->pname);
                    *reporting_info->info_updated_time = now;
                }
            }
        }
    }

    /* Flush any remaining buffered data to the underlying_rio (RDB File). */
    if ((size_t)buf_to_target_rio->io.buf_to_target.pos > 0) {
        if (rioFlush(buf_to_target_rio) == 0) goto werr;
    }

    hashtableResetIterator(&ht_iter);
    return;

werr:
    hashtableResetIterator(&ht_iter);
    args->save_status = C_ERR;
    serverLog(LL_WARNING, "RDB thread (%d): Failed to write buffer to rdb file.", getThreadID());
}


/* --------- Multithreaded RDB Save: Main Thread Orchestration --------- */

/* Drains all RDB thread queues, ensuring all jobs are processed before proceeding.
 * Must be called from the main thread. */
void drainRDBThreadsQueue(void) {
    serverAssert(inMainThread());
    for (int i = 1; i < RDB_THREADS_MAX_NUM; i++) { /* No need to drain thread 0, which is the main thread. */
        while (!JobQueue_isEmpty(&rdb_jobs[i])) {
            /* memory barrier acquire to get the latest job queue state */
            atomic_thread_fence(memory_order_acquire);
        }
    }
}

/* Performs a multithreaded RDB save for a specific database. */
ssize_t rdbSaveDbMultiThreaded(rio *rdb, int dbid, long *key_counter, char *pname) {
    serverAssert(server.rdb_threads_num > 1);
    ssize_t written = 0;

    serverDb *db = server.db[dbid];
    long long info_updated_time = 0;

    /* 1. Create and initialize thread arguments for all RDB threads. */
    RdbSaveThreadArgs *threadArgs = createRdbSaveThreadArgs(server.rdb_threads_num, dbid, rdb, key_counter, pname, &info_updated_time);

    kvstoreIterator *kvs_it = kvstoreIteratorInit(db->keys, HASHTABLE_ITER_PREFETCH_VALUES | HASHTABLE_ITER_INCLUDE_IMPORTING);
    hashtable *ht;
    int last_slot = -1;

    /* 2. Iterate through the hashtables (slots) in the kvstore (in standalone mode there is 1 hashtable). */
    while ((ht = kvstoreIteratorNextHashtable(kvs_it)) != NULL) {
        /* 2.1. Ensure the hashtable will not rehash while being processed */
        hashtablePauseRehashing(ht);

        /* 2.2. Write metadata (e.g., "slot-info") to RDB file if in cluster mode. */
        int curr_slot = kvstoreIteratorGetCurrentHashtableIndex(kvs_it);
        if (server.cluster_enabled && curr_slot != last_slot) {
            sds slot_info = sdscatprintf(sdsempty(), "%i,%lu,%lu", curr_slot,
                                         kvstoreHashtableSize(db->keys, curr_slot),
                                         kvstoreHashtableSize(db->expires, curr_slot));
            rdbSaveAuxFieldStrStr(rdb, "slot-info", slot_info);
            sdsfree(slot_info);
            last_slot = curr_slot;
        }

        /* 2.3. Assign a range of the current hashtable to each RDB thread. */
        for (int i = 0; i < server.rdb_threads_num; i++) {
            RdbSaveThreadArgs *ta = &threadArgs[i];
            ta->ht = ht;

            /* Reset buffer*/
            sdsclear(ta->buf_to_target_rio.io.buf_to_target.ptr);
            ta->buf_to_target_rio.io.buf_to_target.pos = 0;
            ta->buf_to_target_rio.io.buf_to_target.cap_reached = 0;
            ta->buf_to_target_rio.processed_bytes = 0;

            if (i == 0) continue; /* Main thread processes its job directly. Don't need to queue. */

            JobQueue *jq = &rdb_jobs[i];
            if (JobQueue_isFull(jq)) goto werr;
            JobQueue_push(jq, rdbEncodeHashtableRange, ta);
            pthread_mutex_unlock(&rdb_threads_mutex[i]); /* Allow thread to begin its job */
        }
        /* Main thread processes its portion of the hashtable. */
        rdbEncodeHashtableRange(&threadArgs[0]);

        /* 2.4. Wait for all threads to complete their jobs. */
        drainRDBThreadsQueue();

        /* 2.5. Pause worker threads until their next job assignment. */
        for (int i = 1; i < server.rdb_threads_num; i++) {
            pthread_mutex_lock(&rdb_threads_mutex[i]);
        }

        /* 2.6. Check for errors reported by any thread. */
        for (int i = 0; i < server.rdb_threads_num; i++) {
            if (threadArgs[i].save_status == C_ERR) goto werr;
        }

        /* 2.7. Allow rehashing now */
        hashtableResumeRehashing(ht);
    }

    /* 4. Aggregate total bytes written and keys processed from all threads. */
    long long total_keys_written = 0;
    for (int i = 0; i < server.rdb_threads_num; i++) {
        total_keys_written += atomic_load_explicit(&threadArgs[i].keys_processed, memory_order_acquire);
        written += threadArgs[i].bytes_written;
    }

    kvstoreIteratorRelease(kvs_it);
    freeRdbSaveThreadArgs(server.rdb_threads_num, threadArgs);
    serverLog(LL_DEBUG, "rdbSaveHashtablesMultithreaded completed. Num Keys Saved: %llu", total_keys_written);
    return written;

werr:
    hashtableResumeRehashing(ht);
    kvstoreIteratorRelease(kvs_it);
    freeRdbSaveThreadArgs(server.rdb_threads_num, threadArgs);
    return -1;
}

/* --------- Multithreaded RDB Load --------- */

void freeRdbChunkLoadThreadArgs(RdbChunkLoadThreadArgs *args) {
    if (!args) return;
    // free the rio stream's buffer if it owns it. rioFree can often handle this.
    // rioFree(args->chunk_rio);
    zfree(args);
}


// Structure to hold a decoded key-value pair and its associated metadata
typedef struct RdbLoadedKey {
    sds key_sds;         // The SDS string for the key
    robj *val_obj;       // The robj for the value
    long long expiretime; // Expiration time
    long long lfu_freq;   // LFU frequency
    long long lru_idle;   // LRU idle time
} RdbLoadedKey;


// In src/rdb.c (or a new dedicated RDB load threading file)

// Helper function to process a batch of decoded keys and insert them into the DB
// This function acquires the insert_mutex once for the entire batch.
void flush_rdb_batch(RdbLoadedKey **batch_buffer, int batch_count, serverDb *db, int rdbflags, int current_dbid, pthread_mutex_t *insert_mutex, long long lru_clock) {
    if (batch_count == 0) return; // Nothing to do if buffer is empty

    pthread_mutex_lock(insert_mutex); // Acquire lock for the entire batch

    long long now_in_lock = mstime(); // Get 'now' inside the locked section for accurate expiry checks

    for (int i = 0; i < batch_count; i++) {
        RdbLoadedKey *item = batch_buffer[i];

        // Handle expired keys or errors during previous decoding
        if (item->val_obj == NULL) { // This condition covers skipped empty keys or errored keys
            if (item->key_sds) sdsfree(item->key_sds); // Free key if it wasn't inserted
            zfree(item); // Free the RdbLoadedKey struct itself
            continue; // Skip to the next item in the batch
        }

        // Re-check for expired keys based on the 'now' at time of insertion
        if (iAmPrimary() && !(rdbflags & RDBFLAGS_AOF_PREAMBLE) && item->expiretime != -1 && item->expiretime < now_in_lock) {
            // Handle expired keys: replicationFeedReplicas for unlink/del commands
            if (rdbflags & RDBFLAGS_FEED_REPL) {
                serverAssert(server.repl_backlog != NULL && listLength(server.replicas) == 0);
                robj keyobj;
                initStaticStringObject(keyobj, item->key_sds);
                robj *argv[2];
                argv[0] = server.lazyfree_lazy_expire ? shared.unlink : shared.del;
                argv[1] = &keyobj;
                replicationFeedReplicas(current_dbid, argv, 2);
            }
            // Free the key and value objects as they won't be inserted
            sdsfree(item->key_sds);
            decrRefCount(item->val_obj);
            server.rdb_last_load_keys_expired++; // Increment counter for expired keys
        } else {
            // Key is valid and should be added to the database
            robj keyobj_temp;
            initStaticStringObject(keyobj_temp, item->key_sds); // Temporary obj for modules/delete

            int added = dbAddRDBLoad(db, item->key_sds, &item->val_obj);
            server.rdb_last_load_keys_loaded++;

            if (!added) {
                if (rdbflags & RDBFLAGS_ALLOW_DUP) {
                    /* This flag is useful for DEBUG RELOAD special modes.
                     * When it's set we allow new keys to replace the current
                     * keys with the same name. */
                    dbSyncDelete(db, &keyobj_temp);
                    added = dbAddRDBLoad(db, item->key_sds, &item->val_obj);
                    serverAssert(added);
                } else {
                    serverLog(LL_WARNING, "RDB has duplicated key '%s' in DB %d", item->key_sds, db->id);
                    serverPanic("Duplicated key found in RDB file");
                }
            }

            // Set expiration if applicable
            if (item->expiretime != -1) {
                setExpire(NULL, db, &keyobj_temp, item->expiretime);
            }

            // Set LRU/LFU info for eviction
            objectSetLRUOrLFU(item->val_obj, item->lfu_freq, item->lru_idle, lru_clock, 1000);

            // Notify modules about the loaded key
            moduleNotifyKeyspaceEvent(NOTIFY_LOADED, "loaded", &keyobj_temp, db->id);

            // Clear item pointers as ownership has been transferred to the DB
            item->key_sds = NULL;
            item->val_obj = NULL;
        }
        zfree(item); // Free the RdbLoadedKey struct itself
    }
    pthread_mutex_unlock(insert_mutex); // Release lock after the entire batch
}

// ... (existing freeRdbChunkLoadThreadArgs function) ...
#define RDB_LOAD_BATCH_SIZE 256
void processRDBChunk(void *arg) {
    RdbChunkLoadThreadArgs *ta = (RdbChunkLoadThreadArgs *)arg;
    int rdbver = ta->rdbver;
    rio *rdb = ta->chunk_rio;
    serverDb *db = ta->db;
    int rdbflags = ta->rdbflags;
    pthread_mutex_t *insert_mutex = ta->insert_mutex;
    int current_dbid = ta->current_dbid;

    // Thread-local buffer for decoded key-value pairs
    RdbLoadedKey *batch_buffer[RDB_LOAD_BATCH_SIZE];
    int batch_count = 0; // Number of items currently in the buffer

    /* Key-specific attributes, set by opcodes before the key type. */
    long long lru_idle = -1, lfu_freq = -1, expiretime = -1;
    long long lru_clock = LRU_CLOCK(); // Get once per thread for consistency

    int type;
    int error;
    sds key_sds = NULL; // Variable to store the loaded SDS key
    robj *val_obj = NULL; // Variable to store the loaded robj value

    int empty_keys_skipped = 0;

    // Main loop for processing RDB chunk
    while (1) {
        key_sds = NULL; // Reset for new key
        val_obj = NULL; // Reset for new value
        expiretime = -1; // Reset metadata
        lfu_freq = -1;
        lru_idle = -1;

        if ((type = rdbLoadType(rdb)) == -1) goto chunk_err;

        // Loop to consume all optional metadata opcodes before the actual key type
        while (type == RDB_OPCODE_EXPIRETIME ||
               type == RDB_OPCODE_EXPIRETIME_MS ||
               type == RDB_OPCODE_FREQ ||
               type == RDB_OPCODE_IDLE) {
            if (type == RDB_OPCODE_EXPIRETIME) {
                expiretime = rdbLoadTime(rdb);
                expiretime *= 1000;
                if (rioGetReadError(rdb)) goto chunk_err;
            } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
                expiretime = rdbLoadMillisecondTime(rdb, rdbver);
                if (rioGetReadError(rdb)) goto chunk_err;
            } else if (type == RDB_OPCODE_FREQ) {
                uint8_t byte;
                if (rioRead(rdb, &byte, 1) == 0) goto chunk_err;
                lfu_freq = byte;
            } else if (type == RDB_OPCODE_IDLE) {
                uint64_t qword;
                if ((qword = rdbLoadLen(rdb, NULL)) == RDB_LENERR) goto chunk_err;
                lru_idle = qword;
            }
            type = rdbLoadType(rdb); // Read the *next* opcode for the *same* key
            if (type == -1) goto chunk_err; // End of chunk after metadata
        }

        // After the loop, 'type' should now be an actual RDB object type or EOF
        if (!rdbIsObjectType(type)) {
            if (type == RDB_OPCODE_EOF) break; // Reached the end of the current chunk
            serverLog(LL_WARNING, "Malformed RDB chunk in DB %d: Expected object type, got 0x%x.", current_dbid, type);
            goto chunk_err;
        }

        /* Read key and value */
        if ((key_sds = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL)) == NULL) goto chunk_err;
        val_obj = rdbLoadObject(type, rdb, key_sds, db->id, &error);

        // Handle errors or already expired keys *before* adding to the batch
        if (val_obj == NULL) {
            if (error == RDB_LOAD_ERR_EMPTY_KEY) {
                if (empty_keys_skipped++ < 10) serverLog(LL_NOTICE, "rdbLoadObject skipping empty key: %s (in chunk)", key_sds);
                sdsfree(key_sds); // Free the sds for the skipped empty key
                key_sds = NULL;   // Clear pointer for safety
            } else {
                serverLog(LL_WARNING, "Error loading RDB object in chunk for key '%s'.", key_sds);
                sdsfree(key_sds); // Free the sds for the errored key
                key_sds = NULL;   // Clear pointer for safety
                goto chunk_err;
            }
        } else {
            // Key and value successfully loaded, add to thread-local batch buffer
            RdbLoadedKey *new_item = zmalloc(sizeof(RdbLoadedKey));
            new_item->key_sds = key_sds;     // Transfer ownership of sds
            new_item->val_obj = val_obj;     // Transfer ownership of robj*
            new_item->expiretime = expiretime;
            new_item->lfu_freq = lfu_freq;
            new_item->lru_idle = lru_idle;
            batch_buffer[batch_count] = new_item; // Add to buffer
            batch_count += 1;

            // If batch is full, flush it to the database by acquiring the lock
            if (batch_count == RDB_LOAD_BATCH_SIZE) {
                // Call the new standalone function
                flush_rdb_batch(batch_buffer, batch_count, db, rdbflags, current_dbid, insert_mutex, lru_clock);
                batch_count = 0; // Reset batch count after flushing
            }
        }
    } // End of while(1) loop

    // After the main loop, flush any remaining items in the batch buffer
    if (batch_count > 0) {
        flush_rdb_batch(batch_buffer, batch_count, db, rdbflags, current_dbid, insert_mutex, lru_clock);
        batch_count = 0; // Reset batch count
    }

    // // Final cleanup for the rio stream's internal buffer, if applicable.
    // if (rdb && rdb->io_type == RIO_BUFFER && rdb->io.buffer.ptr) {
    //     sdsfree(rdb->io.buffer.ptr);
    //     rdb->io.buffer.ptr = NULL;
    // }
    serverLog(LL_NOTICE, "Thread: %d, done with chunk", getThreadID());

chunk_err:
    // Handle cleanup for any items remaining in the batch buffer if an error occurred mid-batch
    serverLog(LL_WARNING, "Thread: %d, RDB chunk processing error in DB %d.", getThreadID(), current_dbid);
    if (rioGetReadError(rdb)) {
        serverLog(LL_WARNING, "Thread: %d, RDB chunk read error: %s", getThreadID(), strerror(errno));
    } else {
        serverLog(LL_WARNING, "Thread: %d, RDB chunk processing stopped due to internal error.", getThreadID());
    }
    if (batch_count > 0) {
        for (int i = 0; i < batch_count; i++) {
            RdbLoadedKey *item = batch_buffer[i];
            if (item) {
                if (item->key_sds) sdsfree(item->key_sds);
                if (item->val_obj) decrRefCount(item->val_obj);
                zfree(item);
            }
        }
        batch_count = 0;
    }
    
    // Original cleanup for 'key_sds' or 'val_obj' that might not have been added to the batch
    // These are only non-NULL if an error occurred before they were added to batch_buffer
    if (key_sds) sdsfree(key_sds);
    if (val_obj) decrRefCount(val_obj);

    freeRdbChunkLoadThreadArgs(ta); // Frees the thread arguments struct
    return;
}


static size_t next_worker_thread_idx_to_try = 1; // Start from the first worker thread (index 1)

void offloadRDBChunkToThread(
    rio *rdb_main_stream,         // The main RDB stream from rdbLoadRioWithLoadingCtx
    unsigned long chunk_size,     // Size of the chunk to read
    int rdbver,                   // RDB version from main thread
    serverDb *current_db,         // The current database object
    int rdbflags,                 // RDB flags
    pthread_mutex_t *db_insert_mutex, // The shared mutex for DB operations
    int current_dbid,              // The ID of the current database
    long long num_thread_tasks
) {
    sds sds_chunk_buf = sdsnewlen(NULL, chunk_size);
    if (sds_chunk_buf == NULL) {
        serverLog(LL_WARNING, "Failed to allocate SDS buffer of %lu bytes for chunk. Cannot offload chunk.", chunk_size);
        return;
    }
    // serverLog("")


    if (rioRead(rdb_main_stream, sds_chunk_buf, chunk_size) == 0) {
        serverLog(LL_WARNING, "Short read when loading RDB chunk of %lu bytes into SDS.", chunk_size);
        sdsfree(sds_chunk_buf);
        return;
    }

    rio *chunk_rio = zmalloc(sizeof(rio));
    if (!chunk_rio) {
        serverLog(LL_WARNING, "Failed to allocate chunk rio object. Cannot offload chunk.");
        sdsfree(sds_chunk_buf);
        return;
    }

    rioInitWithBuffer(chunk_rio, sds_chunk_buf);

    RdbChunkLoadThreadArgs *threadChunkArgs = zmalloc(sizeof(RdbChunkLoadThreadArgs));

    threadChunkArgs->rdbver = rdbver;
    threadChunkArgs->chunk_rio = chunk_rio;
    threadChunkArgs->db = current_db;
    threadChunkArgs->rdbflags = rdbflags;
    threadChunkArgs->insert_mutex = db_insert_mutex;
    threadChunkArgs->current_dbid = current_dbid;

    size_t num_worker_threads = server.rdb_threads_num - 1;

    // Flag to track if the job was successfully offloaded
    int job_offloaded = 0;

    // Only attempt to offload if there are actual worker threads
    if (num_worker_threads > 0) {
        // Start round-robin search from 'next_worker_thread_idx_to_try'
        // Iterate through all worker threads (from index 1 to num_worker_threads)
        for (size_t i = 0; i < num_worker_threads; ++i) {
            size_t current_attempt_thread_idx = next_worker_thread_idx_to_try;
            JobQueue *jq = &rdb_jobs[current_attempt_thread_idx]; // Get the JobQueue for this thread

            if (!JobQueue_isFull(jq)) {
                // serverLog(LL_NOTICE, "Giving chunk to worker job queue %lu.", current_attempt_thread_idx);
                JobQueue_push(jq, processRDBChunk, threadChunkArgs);
                job_offloaded = 1;

                // Update next_worker_thread_idx_to_try for the next call
                next_worker_thread_idx_to_try = (current_attempt_thread_idx % num_worker_threads) + 1;
                break; // Job offloaded, exit loop
            } else {
                serverLog(LL_DEBUG, "Worker job queue %lu is full. Trying next.", current_attempt_thread_idx);
                // Move to the next thread in round-robin fashion for the next attempt
                next_worker_thread_idx_to_try = (current_attempt_thread_idx % num_worker_threads) + 1;
            }
        }
    }

    if (!job_offloaded) {
        serverLog(LL_WARNING, "All worker job queues are full or no worker threads. Processing chunk in main thread.");
        // If not offloaded, we must free the allocated resources immediately
        freeRdbChunkLoadThreadArgs(threadChunkArgs);

        // Fallback: Process the chunk synchronously in the main thread
        // This means calling processRDBChunk directly.
        // Be very careful about the context (e.g., now, lru_clock need to be in sync)
        processRDBChunk((void*)threadChunkArgs); // Pass the args, process it immediately
    }
}





// #define RDB_LOAD_BATCH_SIZE 256
// void processRDBChunk(void *arg) {
//     RdbChunkLoadThreadArgs *ta = (RdbChunkLoadThreadArgs *)arg;
//     int rdbver = ta->rdbver;
//     rio *rdb = ta->chunk_rio;
//     serverDb *db = ta->db;
//     int rdbflags = ta->rdbflags;
//     pthread_mutex_t *insert_mutex = ta->insert_mutex;
//     int current_dbid = ta->current_dbid;
//     RdbLoadedKey *batch_buffer[RDB_LOAD_BATCH_SIZE];
//     int batch_count = 0; // Number of items currently in the buffer


//     /* Key-specific attributes, set by opcodes before the key type. */
//     long long lru_idle = -1, lfu_freq = -1, expiretime = -1, now = mstime();
//     long long lru_clock = LRU_CLOCK();

//     int type;
//     int error;
//     sds key;
//     robj *val;

//     int empty_keys_skipped = 0;
//     while (1) {
//         key = NULL;
//         val = NULL;
//         expiretime = -1; // Reset for each key
//         lfu_freq = -1;
//         lru_idle = -1;


//         if ((type = rdbLoadType(rdb)) == -1) goto chunk_err;

//         // Handle key-specific metadata opcodes produced by rdbSaveKeyValuePair
//         /* Handle special types. */
//         // Loop to consume all optional metadata opcodes before the actual key type
//         while (type == RDB_OPCODE_EXPIRETIME ||
//                type == RDB_OPCODE_EXPIRETIME_MS ||
//                type == RDB_OPCODE_FREQ ||
//                type == RDB_OPCODE_IDLE) {
//             if (type == RDB_OPCODE_EXPIRETIME) {
//                 expiretime = rdbLoadTime(rdb);
//                 expiretime *= 1000;
//                 if (rioGetReadError(rdb)) goto chunk_err;
//             } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
//                 expiretime = rdbLoadMillisecondTime(rdb, rdbver);
//                 if (rioGetReadError(rdb)) goto chunk_err;
//             } else if (type == RDB_OPCODE_FREQ) {
//                 uint8_t byte;
//                 if (rioRead(rdb, &byte, 1) == 0) goto chunk_err;
//                 lfu_freq = byte;
//             } else if (type == RDB_OPCODE_IDLE) {
//                 uint64_t qword;
//                 if ((qword = rdbLoadLen(rdb, NULL)) == RDB_LENERR) goto chunk_err;
//                 lru_idle = qword;
//             }
//             type = rdbLoadType(rdb);        // Read the *next* opcode for the *same* key
//             if (type == -1) goto chunk_err; // End of chunk after metadata
//         }

//         // After the loop, 'type' should now be an actual RDB object type
//         // or an unexpected opcode (which indicates a malformed chunk).
//         if (!rdbIsObjectType(type)) {
//             // If it's RDB_OPCODE_EOF, that means the chunk writer explicitly put an EOF
//             // inside the chunk, or we've simply reached the end of the rio buffer.
//             if (type == RDB_OPCODE_EOF) break;
//             serverLog(LL_WARNING, "Malformed RDB chunk in DB %d: Expected object type, got 0x%x.", current_dbid, type);
//             goto chunk_err;
//         }

//         /* Read key */
//         if ((key = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL)) == NULL) goto chunk_err;
//         /* Read value */
//         val = rdbLoadObject(type, rdb, key, db->id, &error);


//         pthread_mutex_lock(insert_mutex);

//         if (val == NULL) {
//             if (error == RDB_LOAD_ERR_EMPTY_KEY) {
//                 if (empty_keys_skipped++ < 10) serverLog(LL_NOTICE, "rdbLoadObject skipping empty key: %s (in chunk)", key);
//                 sdsfree(key); // Free the sds for the skipped empty key
//                 key = NULL;   // Clear pointer for safety
//             } else {
//                 serverLog(LL_WARNING, "Error loading RDB object in chunk for key '%s'.", key);
//                 sdsfree(key); // Free the sds for the errored key
//                 key = NULL;   // Clear pointer for safety
//                 goto chunk_err;
//             }
//         } else if (iAmPrimary() && !(rdbflags & RDBFLAGS_AOF_PREAMBLE) && expiretime != -1 && expiretime < now) {
//             if (rdbflags & RDBFLAGS_FEED_REPL) {
//                 /* Caller should have created replication backlog,
//                  * and now this path only works when rebooting,
//                  * so we don't have replicas yet. */
//                 serverAssert(server.repl_backlog != NULL && listLength(server.replicas) == 0);
//                 robj keyobj;
//                 initStaticStringObject(keyobj, key);
//                 robj *argv[2];
//                 argv[0] = server.lazyfree_lazy_expire ? shared.unlink : shared.del;
//                 argv[1] = &keyobj;
//                 replicationFeedReplicas(current_dbid, argv, 2);
//             }
//             sdsfree(key);
//             decrRefCount(val);
//             server.rdb_last_load_keys_expired++;

//         } else {
//             // Key is valid and should be added
//             robj keyobj;
//             initStaticStringObject(keyobj, key);

//             int added = dbAddRDBLoad(db, key, &val);
//             server.rdb_last_load_keys_loaded++;
//             if (!added) {
//                 if (rdbflags & RDBFLAGS_ALLOW_DUP) {
//                     /* This flag is useful for DEBUG RELOAD special modes.
//                      * When it's set we allow new keys to replace the current
//                      * keys with the same name. */
//                     dbSyncDelete(db, &keyobj);
//                     added = dbAddRDBLoad(db, key, &val);
//                     serverAssert(added);
//                 } else {
//                     serverLog(LL_WARNING, "RDB has duplicated key '%s' in DB %d", key, db->id);
//                     serverPanic("Duplicated key found in RDB file");
//                 }
//             }

//             /* Set the expire time if needed */
//             if (expiretime != -1) {
//                 val = setExpire(NULL, db, &keyobj, expiretime);
//             }

//             /* Set usage information (for eviction). */
//             objectSetLRUOrLFU(val, lfu_freq, lru_idle, lru_clock, 1000);

//             /* call key space notification on key loaded for modules only */
//             moduleNotifyKeyspaceEvent(NOTIFY_LOADED, "loaded", &keyobj, db->id);

//             // pthread_mutex_unlock(insert_mutex); // Unlock after DB modification


//             /* Release key (sds), dictEntry stores a copy of it in embedded data */
//             sdsfree(key);
//             key = NULL; // Clear pointers for safety
//             val = NULL;
//         }
//         pthread_mutex_unlock(insert_mutex); 

//     }
//     sdsfree(rdb->io.buffer.ptr);

// chunk_err:
//     // Handle cleanup for any key/val that might still be allocated on error/break
//     if (key) sdsfree(key);
//     if (val) decrRefCount(val);

//     // Any thread-local counters can be reported/aggregated here if needed.

//     freeRdbChunkLoadThreadArgs(ta); // Frees the args struct, rio, and its buffer
//     return;
// }