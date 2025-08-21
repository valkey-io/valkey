/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "thread_common.h"
#include "rdb_threads.h"
#include "rdb.h"
#include "module.h"
#include "server.h"

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
#define RDB_BUFFER_MAX_CHUNK_SIZE WORKER_BUFFER_CAPACITY_LIMIT // 64MB example size
RdbChunkBuffer rdb_buffer_pool[RDB_BUFFER_POOL_SIZE];
_Atomic size_t next_buffer_idx = ATOMIC_VAR_INIT(0);

void initRdbBufferPool(void) {
    for (int i = 0; i < RDB_BUFFER_POOL_SIZE; ++i) {
        // sdsnewlen(NULL, 0) is a quick way to create an empty, non-NULL SDS.
        rdb_buffer_pool[i].sds_chunk_buf = sdsnewlen(NULL, RDB_BUFFER_MAX_CHUNK_SIZE);
        if (!rdb_buffer_pool[i].sds_chunk_buf) {
            serverPanic("Failed to allocate RDB buffer pool");
        }
        atomic_init(&rdb_buffer_pool[i].state, 0); // Initialize as idle.
    }
}

// Cleans up the RDB buffer pool.
void destroyRdbBufferPool(void) {
    for (int i = 0; i < RDB_BUFFER_POOL_SIZE; ++i) {
        sdsfree(rdb_buffer_pool[i].sds_chunk_buf);
    }
}


static inline void freeRdbChunkLoadThreadArgs(RdbChunkLoadThreadArgs *args) {
    if (!args) return;

    if (args->buffer) {
        // serverLog(LL_NOTICE, "Clearnig buffer for future usuage");
        sdsfree(args->buffer->sds_chunk_buf);
        // atomic_store(&args->buffer->state, 0);
        args->buffer->buffer_size = 0;
        args->buffer = NULL;
        // sem_post(&rdb_buffer_pool_sem); // signal that it's available

    }

    zfree(args);
}


/* set to 1 by any worker on error; caller should check after drain */
_Atomic int rdb_load_thread_error = 0;

void flush_rdb_batch(
    RdbLoadedKey *batch_buffer,
    int batch_count,
    serverDb *db,
    int rdbflags,
    int current_dbid,
    long long lru_clock) {
    if (batch_count == 0) return;

    for (int i = 0; i < batch_count; i++) {
        RdbLoadedKey *item = &batch_buffer[i];

        /* skipped/errored */
        if (item->val_obj == NULL) {
            if (item->key_sds) sdsfree(item->key_sds);
            continue;
        }

        /* expired fast-path */
        if (iAmPrimary() &&
            !(rdbflags & RDBFLAGS_AOF_PREAMBLE) &&
            item->expiretime != -1 &&
            item->expiretime < item->now) {
            if (rdbflags & RDBFLAGS_FEED_REPL) {
                serverAssert(server.repl_backlog != NULL && listLength(server.replicas) == 0);
                robj keyobj;
                initStaticStringObject(keyobj, item->key_sds);
                robj *argv[2] = {server.lazyfree_lazy_expire ? shared.unlink : shared.del, &keyobj};
                replicationFeedReplicas(current_dbid, argv, 2);
            }
            sdsfree(item->key_sds);
            decrRefCount(item->val_obj);
            server.rdb_last_load_keys_expired++;
            continue;
        }

        /* insert path */
        robj keyobj_temp;
        initStaticStringObject(keyobj_temp, item->key_sds);
        int added = dbAddRDBLoad(db, item->key_sds, &item->val_obj);
        server.rdb_last_load_keys_loaded++;

        if (!added) {
            if (rdbflags & RDBFLAGS_ALLOW_DUP) {
                dbSyncDelete(db, &keyobj_temp);
                added = dbAddRDBLoad(db, item->key_sds, &item->val_obj);
                serverAssert(added);
            } else {
                serverLog(LL_WARNING, "RDB duplicated key '%s' in DB %d", item->key_sds, db->id);
                serverPanic("Duplicated key found in RDB file");
            }
        }

        if (item->expiretime != -1) {
            item->val_obj = setExpire(NULL, db, &keyobj_temp, item->expiretime);
        }

        objectSetLRUOrLFU(item->val_obj, item->lfu_freq, item->lru_idle, lru_clock, 1000);
        moduleNotifyKeyspaceEvent(NOTIFY_LOADED, "loaded", &keyobj_temp, db->id);

        /* DB kept its own copy → free our SDS; DB owns val_obj now */
        sdsfree(item->key_sds);
        item->key_sds = NULL;
        item->val_obj = NULL;

        if (server.key_load_delay) debugDelay(server.key_load_delay);
    }
}


void processRDBChunk(void *arg) {
    RdbChunkLoadThreadArgs *ta = (RdbChunkLoadThreadArgs *)arg;

    RdbChunkBuffer *buffer = ta->buffer;
    serverDb *db = ta->db;
    const int current_dbid = ta->current_dbid;
    const int rdbver = ta->rdbver;
    long long lru_clock = ta->lru_clock;

    int type, error;
    sds key_sds = NULL;
    robj *val_obj = NULL;
    int empty_keys_skipped = 0;

    // Fix 1: Properly initialize the rio stream for the buffer.
    rio *rdb = &buffer->buffer_rio;

    int batch_element_counts[NUM_LOCKS] = {0};


    // serverLog(LL_NOTICE, "Thread %ld processing a chunk", getThreadID());
    /* End offset of this finite buffer */
    const size_t end_off = buffer->buffer_size;

    while ((size_t)rioTell(rdb) < end_off) {
        long long lru_idle = -1, lfu_freq = -1, expiretime = -1;

        type = rdbLoadType(rdb);
        if (type == -1) {
            serverLog(LL_NOTICE, "ERROR READING TYPE");
            goto chunk_err;
        }

        while (type == RDB_OPCODE_EXPIRETIME ||
               type == RDB_OPCODE_EXPIRETIME_MS ||
               type == RDB_OPCODE_FREQ ||
               type == RDB_OPCODE_IDLE) {
            if (type == RDB_OPCODE_EXPIRETIME) {
                expiretime = rdbLoadTime(rdb) * 1000;
                if (rioGetReadError(rdb)) goto chunk_err;
            } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
                expiretime = rdbLoadMillisecondTime(rdb, rdbver);
                if (rioGetReadError(rdb)) goto chunk_err;
            } else if (type == RDB_OPCODE_FREQ) {
                uint8_t b;
                if (rioRead(rdb, &b, 1) == 0) goto chunk_err;
                lfu_freq = b;
            } else { /* RDB_OPCODE_IDLE */
                uint64_t qword;
                if ((qword = rdbLoadLen(rdb, NULL)) == RDB_LENERR) goto chunk_err;
                lru_idle = qword;
            }

            if ((size_t)rioTell(rdb) >= end_off) goto chunk_err;

            type = rdbLoadType(rdb);
            if (type == -1) goto chunk_err;
        }

        if (!rdbIsObjectType(type)) {
            serverLog(LL_WARNING, "Malformed RDB chunk in DB %d: got 0x%x where object type expected.",
                      current_dbid, type);
            goto chunk_err;
        }

        key_sds = rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL);
        if (key_sds == NULL) {
            serverLog(LL_NOTICE, "ERROR Getting Key");
            goto chunk_err;
        }


        val_obj = rdbLoadObject(type, rdb, key_sds, db->id, &error);


        if (val_obj == NULL) {
            if (error == RDB_LOAD_ERR_EMPTY_KEY) {
                if (empty_keys_skipped++ < 10)
                    serverLog(LL_NOTICE, "rdbLoadObject skipping empty key: %s (in chunk)", key_sds);
                sdsfree(key_sds);
                key_sds = NULL;
            } else {
                serverLog(LL_WARNING, "Error loading RDB object in chunk for key '%s'.", key_sds);
                sdsfree(key_sds);
                key_sds = NULL;
                goto chunk_err;
            }
        } else {
            /* Use the hash of the key to determine what lock to use*/
            uint64_t hash = hashtableSdsHash(key_sds);
            int lock_idx = hash % NUM_LOCKS;
            batch_element_counts[lock_idx]++;


            RdbLoadedKey *item = &ta->batch_buffers[lock_idx][ta->batch_counts[lock_idx]];
            ta->batch_counts[lock_idx]++;
            item->key_sds = key_sds;
            item->val_obj = val_obj;
            item->expiretime = expiretime;
            item->lfu_freq = lfu_freq;
            item->lru_idle = lru_idle;
            item->lru_clock = lru_clock;
            item->now = ta->now;

            key_sds = NULL;
            val_obj = NULL;

            if (ta->batch_counts[lock_idx] == RDB_LOAD_BATCH_SIZE) {
                // This batch is full, we must try to flush it.
                if (pthread_mutex_trylock(&ta->db_insert_mutexes[lock_idx]) == 0) {
                    // Success! Flush it now.
                    flush_rdb_batch(ta->batch_buffers[lock_idx], ta->batch_counts[lock_idx], ta->db, ta->rdbflags, ta->current_dbid, ta->lru_clock);
                    pthread_mutex_unlock(&ta->db_insert_mutexes[lock_idx]);
                    ta->batch_counts[lock_idx] = 0;
                } else {
                    /* The lock is busy. DON'T BLOCK.
                     * Instead, opportunistically flush any OTHER non-empty, non-full batch
                     * this thread owns to make progress while we wait.
                     */
                    for (int i = 0; i < NUM_LOCKS; i++) {
                        if (i != lock_idx && ta->batch_counts[i] > 0) {
                            if (pthread_mutex_trylock(&ta->db_insert_mutexes[i]) == 0) {
                                flush_rdb_batch(ta->batch_buffers[i], ta->batch_counts[i], ta->db, ta->rdbflags, ta->current_dbid, ta->lru_clock);
                                pthread_mutex_unlock(&ta->db_insert_mutexes[i]);
                                ta->batch_counts[i] = 0;
                            }
                        }
                    }
                    // After trying to flush other batches, we MUST now block on the full one.
                    pthread_mutex_lock(&ta->db_insert_mutexes[lock_idx]);
                    flush_rdb_batch(ta->batch_buffers[lock_idx], ta->batch_counts[lock_idx], ta->db, ta->rdbflags, ta->current_dbid, ta->lru_clock);
                    pthread_mutex_unlock(&ta->db_insert_mutexes[lock_idx]);
                    ta->batch_counts[lock_idx] = 0;
                }
            }
        }
    }
    /* Final flush of remaining items in all private batches */
    int still_unflushed = 0;
    do {
        still_unflushed = 0;
        for (int i = 0; i < NUM_LOCKS; i++) {
            if (ta->batch_counts[i] > 0) {
                if (pthread_mutex_trylock(&ta->db_insert_mutexes[i]) == 0) {
                    flush_rdb_batch(ta->batch_buffers[i], ta->batch_counts[i], ta->db, ta->rdbflags, ta->current_dbid, ta->lru_clock);
                    pthread_mutex_unlock(&ta->db_insert_mutexes[i]);
                    ta->batch_counts[i] = 0; // Mark this batch as flushed
                } else {
                    still_unflushed = 1; // We tried but couldn't flush. Will retry.
                }
            }
        }
    } while (still_unflushed);

    // serverLog(LL_DEBUG, "Thread %d: chunk processed successfully.", getThreadID());
    freeRdbChunkLoadThreadArgs(ta);
    // for (int i = 0; i < NUM_LOCKS; i++) {
    //     serverLog(LL_NOTICE, "Thread %d, Lock Index %d: Found %d elements",
    //               getThreadID(), i, batch_element_counts[i]);
    // }
    return;

chunk_err:
    rdb_load_thread_error = 1;
    for (int i = 0; i < NUM_LOCKS; i++) {
        for (int j = 0; j < ta->batch_counts[i]; j++) {
            if (ta->batch_buffers[i][j].key_sds) sdsfree(ta->batch_buffers[i][j].key_sds);
            if (ta->batch_buffers[i][j].val_obj) decrRefCount(ta->batch_buffers[i][j].val_obj);
        }
    }
    if (key_sds) sdsfree(key_sds);
    if (val_obj) decrRefCount(val_obj);

    if (rioGetReadError(rdb)) {
        serverLog(LL_WARNING, "Thread %d: RDB chunk read error: %s", getThreadID(), strerror(errno));
    } else {
        serverLog(LL_WARNING, "Thread %d: RDB chunk processing stopped due to internal error.", getThreadID());
    }

    freeRdbChunkLoadThreadArgs(ta);
    return;
}


static size_t next_worker_thread_idx_to_try = 1; // valid: [1 .. server.rdb_threads_num-1]
sem_t rdb_buffer_pool_sem;

int offloadRDBChunkToThread(
    rio *rdb_main_stream,
    unsigned long chunk_size,
    int rdbver,
    serverDb *current_db,
    int rdbflags,
    int current_dbid,
    long long lru_clock,
    long long now,
    pthread_mutex_t *db_insert_mutexes) {
    serverAssert(server.rdb_threads_num >= 2);
    serverAssert(chunk_size > 0);

    if (chunk_size > RDB_BUFFER_MAX_CHUNK_SIZE) {
        serverLog(LL_WARNING, "Chunk is too big (%lu) for pool capacity (%lu)", (size_t)chunk_size, (size_t)RDB_BUFFER_MAX_CHUNK_SIZE);
        return C_ERR;
    }

    // RdbChunkBuffer *chunk_buf_wrapper = NULL;
    // sem_wait(&rdb_buffer_pool_sem); 
    // size_t tries = 0;
    // size_t start_idx = atomic_load(&next_buffer_idx);

    // while (1) {
    //     size_t idx = (start_idx + tries) % RDB_BUFFER_POOL_SIZE;
    //     if (atomic_compare_exchange_strong(&rdb_buffer_pool[idx].state, (int[]){0}, 1)) {
    //         chunk_buf_wrapper = &rdb_buffer_pool[idx];
    //         atomic_store(&next_buffer_idx, (idx + 1) % RDB_BUFFER_POOL_SIZE);
    //         // serverLog(LL_NOTICE, "Found buffer! Tries: %lu, Buffer idx: %ld", tries, idx);
    //         break;
    //     }
    //     tries++;
    // }
    // serverAssert(chunk_buf_wrapper != NULL); // Assuming we'll always find one.
    RdbChunkBuffer *chunk_buf_wrapper = zmalloc(sizeof(RdbChunkBuffer));

    chunk_buf_wrapper->sds_chunk_buf = sdsnewlen(NULL, chunk_size);
    // FIX: Read payload into the SDS buffer directly, not into a pointer.
    if (rioRead(rdb_main_stream, chunk_buf_wrapper->sds_chunk_buf, chunk_size) == 0) {
        serverLog(LL_WARNING, "Short read for RDB chunk of %lu bytes", chunk_size);
        atomic_store(&chunk_buf_wrapper->state, 0);
        return C_ERR;
    }

    // FIX: Set the buffer size after the read completes successfully.
    chunk_buf_wrapper->buffer_size = chunk_size;


    rioInitWithBuffer(&chunk_buf_wrapper->buffer_rio, chunk_buf_wrapper->sds_chunk_buf);


    /* 2) Build args */
    RdbChunkLoadThreadArgs *thread_args = zmalloc(sizeof(*thread_args));
    if (!thread_args) {
        serverLog(LL_WARNING, "OOM allocating chunk args");
        atomic_store(&chunk_buf_wrapper->state, 0);
        return C_ERR;
    }

    /* 3) Fill fields */
    thread_args->rdbver = rdbver;
    thread_args->db = current_db;
    thread_args->rdbflags = rdbflags;
    thread_args->current_dbid = current_dbid;
    thread_args->lru_clock = lru_clock;
    thread_args->now = now;
    thread_args->buffer = chunk_buf_wrapper;            // Pass the buffer pointer
    thread_args->db_insert_mutexes = db_insert_mutexes; // Pass the mutexes here

    // Initialize the private batch counts
    for (int i = 0; i < NUM_LOCKS; i++) {
        thread_args->batch_counts[i] = 0;
    }


    /* 4) Round-robin enqueue across workers [1..num_workers] */
    size_t num_workers = server.rdb_threads_num - 1;
    if (next_worker_thread_idx_to_try < 1 || next_worker_thread_idx_to_try > num_workers)
        next_worker_thread_idx_to_try = 1;

    while (1) {
        size_t start = next_worker_thread_idx_to_try;
        for (size_t tries = 0; tries < num_workers; ++tries) {
            size_t idx = 1 + ((start - 1 + tries) % num_workers);
            JobQueue *jq = &rdb_jobs[idx];
            if (!JobQueue_isFull(jq)) {
                JobQueue_push(jq, processRDBChunk, thread_args);
                next_worker_thread_idx_to_try = (idx % num_workers) + 1;
                return C_OK;
            } else {
                atomic_thread_fence(memory_order_acquire);
            }
        }
        // Check if there were errors:
        if (atomic_load(&rdb_load_thread_error) == 1) return C_ERR;
    }

    /* 5) All queues full → process synchronously in main thread */
    serverLog(LL_DEBUG, "All worker queues busy; processing RDB chunk (%luB) in main thread", chunk_size);
    processRDBChunk((void *)thread_args);
    return C_OK;
}
