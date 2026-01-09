/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Background I/O service for the server.
 *
 * This file implements operations that we need to perform in the background.
 * Currently there is only a single operation, that is a background close(2)
 * system call. This is needed as when the process is the last owner of a
 * reference to a file closing it means unlinking it, and the deletion of the
 * file is slow, blocking the server.
 *
 * In the future we'll either continue implementing new things we need or
 * we'll switch to libeio. However there are probably long term uses for this
 * file as we may want to put server specific background tasks here (for instance
 * it is not impossible that we'll need a non blocking FLUSHDB/FLUSHALL
 * implementation).
 *
 * DESIGN
 * ------
 *
 * The design is simple: We have a structure representing a job to perform,
 * and several worker threads and job queues. Every job type is assigned to
 * a specific worker thread, and a single worker may handle several different
 * job types.
 * Every thread waits for new jobs in its queue, and processes every job
 * sequentially.
 *
 * Jobs handled by the same worker are guaranteed to be processed from the
 * least-recently-inserted to the most-recently-inserted (older jobs processed
 * first).
 *
 * Currently there is no way for the creator of the job to be notified about
 * the completion of the operation, this will only be added when/if needed.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2012, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include "server.h"
#include "connection.h"
#include "bio.h"
#include "mutexqueue.h"
#include <stdatomic.h>

static unsigned int bio_job_to_worker[] = {
    [BIO_CLOSE_FILE] = 0,
    [BIO_AOF_FSYNC] = 1,
    [BIO_CLOSE_AOF] = 1,
    [BIO_LAZY_FREE] = 2,
    [BIO_RDB_SAVE] = 3,
};

typedef struct {
    const char *const bio_worker_title;
    pthread_t bio_thread_id;
    mutexQueue *bio_jobs;
} bio_worker_data;

static bio_worker_data bio_workers[] = {
    {"bio_close_file"},
    {"bio_aof"},
    {"bio_lazy_free"},
    {"bio_rdb_save"},
};
static const bio_worker_data *const bio_worker_end = bio_workers + (sizeof bio_workers / sizeof *bio_workers);

static size_t bioWorkerNum(const bio_worker_data *const bwd) {
    /* Ensure the pointer is valid - casting to uintptr_t to not make the comparison itself UB in case
     * the pointer is outside the valid range. It's a best effort thing anyway. */
    serverAssert((uintptr_t)bwd >= (uintptr_t)bio_workers && (uintptr_t)bwd < (uintptr_t)bio_worker_end);
    return (size_t)(bwd - bio_workers);
}

static _Atomic unsigned long bio_jobs_counter[BIO_NUM_OPS] = {0};
static _Thread_local size_t bio_worker_num = 0;

/* This structure represents a background Job. It is only used locally to this
 * file as the API does not expose the internals at all. */
typedef union bio_job {
    struct {
        int type; /* Job-type tag. This needs to appear as the first element in all union members. */
    } header;

    /* Job specific arguments.*/
    struct {
        int type;
        int fd;                          /* Fd for file based background jobs */
        long long offset;                /* A job-specific offset, if applicable */
        unsigned need_fsync : 1;         /* A flag to indicate that a fsync is required before
                                          * the file is closed. */
        unsigned need_reclaim_cache : 1; /* A flag to indicate that reclaim cache is required before
                                          * the file is closed. */
    } fd_args;

    struct {
        int type;
        lazy_free_fn *free_fn; /* Function that will free the provided arguments */
        void *free_args[];     /* List of arguments to be passed to the free function */
    } free_args;

    struct {
        int type;
        connection *conn;    /* Connection to download the RDB from */
        int is_dual_channel; /* Single vs dual channel */
    } save_to_disk_args;
} bio_job;

void *bioProcessBackgroundJobs(void *arg);

/* Make sure we have enough stack to perform all the things we do in the
 * main thread. */
#define VALKEY_THREAD_STACK_SIZE (1024 * 1024 * 4)

/* Initialize the background system, spawning the thread. */
void bioInit(void) {
    pthread_attr_t attr;
    size_t stacksize;

    /* Initialization of state vars and objects */
    for (bio_worker_data *bwd = bio_workers; bwd != bio_worker_end; ++bwd) {
        bwd->bio_jobs = mutexQueueCreate();
    }

    /* Set the stack size as by default it may be small in some system */
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr, &stacksize);
    if (!stacksize) stacksize = 1; /* The world is full of Solaris Fixes */
    while (stacksize < VALKEY_THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&attr, stacksize);

    /* Ready to spawn our threads. We use the single argument the thread
     * function accepts in order to pass a pointer to the data that the
     * thread is responsible for. */
    for (bio_worker_data *bwd = bio_workers; bwd != bio_worker_end; ++bwd) {
        int err = pthread_create(&bwd->bio_thread_id, &attr, bioProcessBackgroundJobs, (void *)bwd);
        if (err) {
            serverLog(LL_WARNING, "Fatal: Can't initialize Background Jobs. Error message: %s", strerror(err));
            exit(1);
        }
    }
}

void bioSubmitJob(int type, bio_job *job) {
    job->header.type = type;
    bio_worker_data *const bwd = &bio_workers[bio_job_to_worker[type]];
    mutexQueueAdd(bwd->bio_jobs, job);
    atomic_fetch_add(&bio_jobs_counter[type], 1);
}

void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...) {
    va_list valist;
    /* Allocate memory for the job structure and all required
     * arguments */
    bio_job *job = zmalloc(sizeof(*job) + sizeof(void *) * (arg_count));
    job->free_args.free_fn = free_fn;

    va_start(valist, arg_count);
    for (int i = 0; i < arg_count; i++) {
        job->free_args.free_args[i] = va_arg(valist, void *);
    }
    va_end(valist);
    bioSubmitJob(BIO_LAZY_FREE, job);
}

void bioCreateCloseJob(int fd, int need_fsync, int need_reclaim_cache) {
    bio_job *job = zmalloc(sizeof(*job));
    job->fd_args.fd = fd;
    job->fd_args.need_fsync = need_fsync;
    job->fd_args.need_reclaim_cache = need_reclaim_cache;

    bioSubmitJob(BIO_CLOSE_FILE, job);
}

void bioCreateCloseAofJob(int fd, long long offset, int need_reclaim_cache) {
    bio_job *job = zmalloc(sizeof(*job));
    job->fd_args.fd = fd;
    job->fd_args.offset = offset;
    job->fd_args.need_fsync = 1;
    job->fd_args.need_reclaim_cache = need_reclaim_cache;

    bioSubmitJob(BIO_CLOSE_AOF, job);
}

void bioCreateFsyncJob(int fd, long long offset, int need_reclaim_cache) {
    bio_job *job = zmalloc(sizeof(*job));
    job->fd_args.fd = fd;
    job->fd_args.offset = offset;
    job->fd_args.need_reclaim_cache = need_reclaim_cache;

    bioSubmitJob(BIO_AOF_FSYNC, job);
}

void bioCreateSaveRDBToDiskJob(connection *conn, int is_dual_channel) {
    bio_job *job = zmalloc(sizeof(*job));
    job->save_to_disk_args.conn = conn;
    job->save_to_disk_args.is_dual_channel = is_dual_channel;
    bioSubmitJob(BIO_RDB_SAVE, job);
}

void *bioProcessBackgroundJobs(void *arg) {
    bio_worker_data *const bwd = arg;
    sigset_t sigset;

    valkey_set_thread_title(bwd->bio_worker_title);

    serverSetCpuAffinity(server.bio_cpulist);

    makeThreadKillable();

    /* Block SIGALRM so we are sure that only the main thread will
     * receive the watchdog signal. */
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    int err = pthread_sigmask(SIG_BLOCK, &sigset, NULL);
    if (err)
        serverLog(LL_WARNING, "Warning: can't mask SIGALRM in bio.c thread: %s", strerror(err));

    bio_worker_num = bioWorkerNum(bwd);

    while (1) {
        /* Get job - blocking until available */
        bio_job *job = mutexQueuePop(bwd->bio_jobs, true);

        /* Process the job accordingly to its type. */
        int job_type = job->header.type;

        if (job_type == BIO_CLOSE_FILE) {
            if (job->fd_args.need_fsync && valkey_fsync(job->fd_args.fd) == -1 && errno != EBADF && errno != EINVAL) {
                serverLog(LL_WARNING, "Fail to fsync the AOF file: %s", strerror(errno));
            }
            if (job->fd_args.need_reclaim_cache) {
                if (reclaimFilePageCache(job->fd_args.fd, 0, 0) == -1) {
                    serverLog(LL_NOTICE, "Unable to reclaim page cache: %s", strerror(errno));
                }
            }
            close(job->fd_args.fd);
        } else if (job_type == BIO_AOF_FSYNC || job_type == BIO_CLOSE_AOF) {
            /* The fd may be closed by main thread and reused for another
             * socket, pipe, or file. We just ignore these errno because
             * aof fsync did not really fail. */
            if (valkey_fsync(job->fd_args.fd) == -1 && errno != EBADF && errno != EINVAL) {
                int last_status = atomic_load_explicit(&server.aof_bio_fsync_status, memory_order_relaxed);

                atomic_store_explicit(&server.aof_bio_fsync_errno, errno, memory_order_relaxed);
                atomic_store_explicit(&server.aof_bio_fsync_status, C_ERR, memory_order_release);
                if (last_status == C_OK) {
                    serverLog(LL_WARNING, "Fail to fsync the AOF file: %s", strerror(errno));
                }
            } else {
                atomic_store_explicit(&server.aof_bio_fsync_status, C_OK, memory_order_relaxed);
                atomic_store_explicit(&server.fsynced_reploff_pending, job->fd_args.offset, memory_order_relaxed);
            }

            if (job->fd_args.need_reclaim_cache) {
                if (reclaimFilePageCache(job->fd_args.fd, 0, 0) == -1) {
                    serverLog(LL_NOTICE, "Unable to reclaim page cache: %s", strerror(errno));
                }
            }
            if (job_type == BIO_CLOSE_AOF) close(job->fd_args.fd);
        } else if (job_type == BIO_LAZY_FREE) {
            job->free_args.free_fn(job->free_args.free_args);
        } else if (job_type == BIO_RDB_SAVE) {
            replicaReceiveRDBFromPrimaryToDisk(job->save_to_disk_args.conn, job->save_to_disk_args.is_dual_channel);
        } else {
            serverPanic("Wrong job type in bioProcessBackgroundJobs().");
        }
        zfree(job);
        atomic_fetch_sub(&bio_jobs_counter[job_type], 1);
    }
}

/* Return the number of pending jobs of the specified type. */
unsigned long bioPendingJobsOfType(int type) {
    return atomic_load(&bio_jobs_counter[type]);
}

/* Wait for the job queue of the worker for jobs of specified type to become empty. */
void bioDrainWorker(int type) {
    while (bioPendingJobsOfType(type) > 0) {
        usleep(100);
    }
}

/* Kill the running bio threads in an unclean way. This function should be
 * used only when it's critical to stop the threads for some reason.
 * Currently the server does this only on crash (for instance on SIGSEGV) in order
 * to perform a fast memory check without other threads messing with memory. */
void bioKillThreads(void) {
    int err;

    for (bio_worker_data *bwd = bio_workers; bwd != bio_worker_end; ++bwd) {
        if (pthread_equal(bwd->bio_thread_id, pthread_self())) continue;
        if (bwd->bio_thread_id && pthread_cancel(bwd->bio_thread_id) == 0) {
            if ((err = pthread_join(bwd->bio_thread_id, NULL)) != 0) {
                serverLog(LL_WARNING, "Bio worker thread #%zu can not be joined: %s", bioWorkerNum(bwd), strerror(err));
            } else {
                serverLog(LL_WARNING, "Bio worker thread #%zu terminated", bioWorkerNum(bwd));
            }
        }
    }
}

int inBioThread(void) {
    return bio_worker_num != 0;
}
