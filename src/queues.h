/*
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
    size_t queue_size;
} mpscQueue;

/* Initializes an MPSC queue with a size that must be a power of 2 */
void mpscInit(mpscQueue *q, size_t queue_size);
/* Frees the MPSC queue's internal buffer and resets its state */
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
    size_t queue_size;
} spmcQueue;

/* Initializes an SPMC queue with a size that must be a power of 2 */
void spmcInit(spmcQueue *q, size_t queue_size);
/* Frees the SPMC queue's internal buffer and resets its state */
void spmcFree(spmcQueue *q);
/* Returns true if the SPMC queue has no items */
bool spmcIsEmpty(spmcQueue *q);
/* Returns an approximate number of items currently in the queue */
size_t spmcSize(spmcQueue *q);
/* Pushes an item to the SPMC queue. Returns true on success, false if the queue is full. */
bool spmcEnqueue(spmcQueue *q, void *data);
/* Pops and returns the next item from the queue, or NULL if the queue is empty */
void *spmcDequeue(spmcQueue *q);

/* ==========================================================================
 * SPSC QUEUE (Single-Producer Single-Consumer)
 * ========================================================================== */

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
    size_t queue_size;
} spscQueue;

/* Initializes an SPSC queue with a size that must be a power of 2 */
void spscInit(spscQueue *q, size_t queue_size);
/* Frees the SPSC queue's internal buffer and resets its state */
void spscFree(spscQueue *q);
/* Returns true if the queue is full, or false otherwise */
bool spscIsFull(spscQueue *q);
/* Push data to the queue. Caller must ensure queue is not full via spscIsFull().
 * If commit is true, the tail pointer is updated immediately (visible to consumer) else,
 * only local index is updated (batching). */
void spscEnqueue(spscQueue *q, void *data, bool commit);
/* Publishes any pending batched enqueues by advancing the shared tail pointer */
void spscCommit(spscQueue *q);
/* Pops up to num_jobs items from the queue and returns the actual number popped */
size_t spscDequeueBatch(spscQueue *q, void **jobs_out, size_t num_jobs);
/* Check if queue is empty from producer's perspective. */
bool spscIsEmpty(spscQueue *q);

#endif /* __QUEUES_H__ */
