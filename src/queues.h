/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Implements different types of queues
 *
 * 1. SPMC - Single Producer Multi Consumer
 *    - Automatic load balancing:
 *      Busy threads take less work, idle threads take more.
 *    - Each ring buffer cell is cache-line padded to prevent consumer contention.
 *    - Sequence numbers indicate empty/populated state for safe work claiming.
 *
 * 2. Multi Producer Single Consumer
 *    - Producer threads push jobs; consumer thread checks if queue is non-empty.
 *    - Producer threads reserve slots via atomic tail increment.
 *    - If full, jobs are buffered locally until space is available.
 *
 * 3. SPSC - Single Producer Single Consumer
 *    - Allows producer to batch jobs
 */

#ifndef __QUEUES_H__
#define __QUEUES_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

#ifndef __cplusplus
#include <stdatomic.h>
#include "serverassert.h"
#endif

/* ==========================================================================
 * MPSC QUEUE (Multi-Producer Single-Consumer)
 * ========================================================================== */

#define MPSC_QUEUE_SIZE 16384
#define MPSC_QUEUE_MASK (MPSC_QUEUE_SIZE - 1)
#ifndef __cplusplus
static_assert((MPSC_QUEUE_SIZE & (MPSC_QUEUE_SIZE - 1)) == 0, "MPSC_QUEUE_SIZE must be power of 2");
#endif

typedef struct mpscTicket {
    size_t index;
    bool has_reservation;
} mpscTicket;

typedef struct mpscQueue {
    /* Consumer cache line */
    _Alignas(CACHE_LINE_SIZE) _Atomic(size_t) head;
    size_t tail_cache;

    /* Producer cache line */
    _Alignas(CACHE_LINE_SIZE) _Atomic(size_t) tail;
    _Atomic(size_t) head_cache;

    /* Data buffer */
    _Alignas(CACHE_LINE_SIZE) _Atomic(void *) *buffer;
} mpscQueue;

void mpscInit(mpscQueue *q);
void mpscFree(mpscQueue *q);

/* Pushes an item into the queue and returns true if the queue is not full.
 * Otherwise, a slot index is reserved and saved in the ticket, and returns false.
 * Subsequent retries must pass the same ticket to fill the reserved slot, provided the queue is not full */
bool mpscEnqueue(mpscQueue *q, void *data, mpscTicket *ticket);

/* Pops a batch of items from the queue.
 * Stops at the first empty slot. */
size_t mpscDequeueBatch(mpscQueue *q, void **jobs_out, size_t max_jobs);

/* ==========================================================================
 * SPMC QUEUE (Single-Producer Multi-Consumer)
 * ========================================================================== */

#define SPMC_QUEUE_SIZE 4096
#define SPMC_QUEUE_MASK (SPMC_QUEUE_SIZE - 1)
#ifndef __cplusplus
static_assert((SPMC_QUEUE_SIZE & (SPMC_QUEUE_SIZE - 1)) == 0, "SPMC_QUEUE_SIZE must be power of 2");
#endif

typedef struct spmcCell {
    _Alignas(CACHE_LINE_SIZE) _Atomic(size_t) sequence;
    void *data;
} spmcCell;

typedef struct spmcQueue {
    /* Shared Read/Write (High Contention) */
    _Alignas(CACHE_LINE_SIZE) _Atomic(size_t) head;

    /* Producer Cache line */
    _Alignas(CACHE_LINE_SIZE) size_t tail;
    size_t head_cache;

    /* Data buffer */
    _Alignas(CACHE_LINE_SIZE) spmcCell *buffer;
} spmcQueue;

void spmcInit(spmcQueue *q);
void spmcFree(spmcQueue *q);
bool spmcIsEmpty(spmcQueue *q);
size_t spmcSize(spmcQueue *q);
bool spmcEnqueue(spmcQueue *q, void *data);
void *spmcDequeue(spmcQueue *q);

/* ==========================================================================
 * SPSC QUEUE (Single-Producer Single-Consumer)
 * ========================================================================== */

#define SPSC_QUEUE_SIZE 4096
#define SPSC_QUEUE_MASK (SPSC_QUEUE_SIZE - 1)
#ifndef __cplusplus
static_assert((SPSC_QUEUE_SIZE & (SPSC_QUEUE_SIZE - 1)) == 0, "SPSC_QUEUE_SIZE must be power of 2");
#endif

typedef struct spscQueue {
    /* Consumer cache line */
    _Alignas(CACHE_LINE_SIZE) _Atomic(size_t) head;
    size_t tail_cache;

    /* Producer cache line */
    _Alignas(CACHE_LINE_SIZE) _Atomic(size_t) tail;
    size_t tail_local; /* Private write index */
    size_t head_cache;

    /* Dynamic buffer */
    _Alignas(CACHE_LINE_SIZE) void **buffer;
} spscQueue;

void spscInit(spscQueue *q);
void spscFree(spscQueue *q);
bool spscIsFull(spscQueue *q);
/* Push data to the queue. Caller must ensure queue is not full via spscIsFull().
 * If commit is true, the tail pointer is updated immediately (visible to consumer) else,
 * only local index is updated (batching). */
void spscEnqueue(spscQueue *q, void *data, bool commit);
void spscCommit(spscQueue *q);
size_t spscDequeueBatch(spscQueue *q, void **jobs_out, size_t num_jobs);
/* Check if queue is empty from producer's perspective. */
bool spscIsEmpty(spscQueue *q);

#endif /* __QUEUES_H__ */
