/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Implementation of MPSC, SPMC, and SPSC queues
 *
 */

#include "queues.h"
#include "zmalloc.h"
inline void mpscInit(mpscQueue *q) {
    q->buffer = (_Atomic(void *) *)zmalloc(sizeof(_Atomic(void *)) * MPSC_QUEUE_SIZE);
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    atomic_init(&q->head_cache, 0);
    q->tail_cache = 0;

    for (size_t i = 0; i < MPSC_QUEUE_SIZE; ++i) {
        atomic_init(&q->buffer[i], NULL);
    }
}

inline void mpscFree(mpscQueue *q) {
    if (q->buffer) {
        zfree(q->buffer);
        q->buffer = NULL;
    }
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&q->head_cache, 0, memory_order_relaxed);
    q->tail_cache = 0;
}

inline bool mpscEnqueue(mpscQueue *q, void *data, mpscTicket *ticket) {
    size_t tail;
    assert(data);

    /* Reserve a slot (or use existing reservation) */
    if (!ticket->has_reservation) {
        tail = atomic_fetch_add_explicit(&q->tail, 1, memory_order_relaxed);
    } else {
        tail = ticket->index;
    }

    /* Check limits (Fullness check) */
    size_t head = atomic_load_explicit(&q->head_cache, memory_order_acquire);
    if ((tail - head) >= MPSC_QUEUE_SIZE) {
        /* Cached limit reached, refresh from actual head */
        head = atomic_load_explicit(&q->head, memory_order_acquire);
        atomic_store_explicit(&q->head_cache, head, memory_order_release);

        if (unlikely((tail - head) >= MPSC_QUEUE_SIZE)) {
            /* Queue is full - Persist reservation for retry */
            ticket->index = tail;
            ticket->has_reservation = true;
            return false;
        }
    }

    /* Commit data */
    atomic_store_explicit(&q->buffer[tail & MPSC_QUEUE_MASK], data, memory_order_release);

    ticket->has_reservation = false;
    return true;
}

inline size_t mpscDequeueBatch(mpscQueue *q, void **jobs_out, size_t max_jobs) {
    size_t popped_count = 0;
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = q->tail_cache;

    /* Refresh tail cache if it looks empty */
    if (head == tail) {
        tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        q->tail_cache = tail;
        if (head == tail) return 0;
    }

    size_t limit = tail - head;
    if (limit > max_jobs) limit = max_jobs;

    for (size_t i = 0; i < limit; ++i) {
        void *data = atomic_load_explicit(&q->buffer[head & MPSC_QUEUE_MASK], memory_order_relaxed);

        /* Stop if slot is reserved but data not yet written */
        if (!data) break;

        jobs_out[popped_count++] = data;
        atomic_store_explicit(&q->buffer[head & MPSC_QUEUE_MASK], NULL, memory_order_relaxed);
        head++;
    }

    if (popped_count > 0) {
        atomic_store_explicit(&q->head, head, memory_order_release);
        /* Ensure data visibility for the caller */
        atomic_thread_fence(memory_order_acquire);
    }
    return popped_count;
}

/* ==========================================================================
 * SPMC QUEUE (Single-Producer Multi-Consumer)
 * ========================================================================== */

inline void spmcInit(spmcQueue *q) {
    q->buffer = (spmcCell *)zmalloc(sizeof(spmcCell) * SPMC_QUEUE_SIZE);
    atomic_init(&q->head, 0);
    q->tail = 0;
    q->head_cache = 0;

    for (size_t i = 0; i < SPMC_QUEUE_SIZE; i++) {
        atomic_init(&q->buffer[i].sequence, i);
        q->buffer[i].data = NULL;
    }
}

inline void spmcFree(spmcQueue *q) {
    if (q->buffer) {
        zfree(q->buffer);
        q->buffer = NULL;
    }
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    q->tail = 0;
    q->head_cache = 0;
}

inline bool spmcIsEmpty(spmcQueue *q) {
    /* Fast path: Check against cached consumer position */
    if (q->tail == q->head_cache) {
        return true;
    }

    /* Slow path: Refresh atomic head and update cache */
    size_t curr_head = atomic_load_explicit(&q->head, memory_order_acquire);
    q->head_cache = curr_head;

    return q->tail == curr_head;
}

inline size_t spmcSize(spmcQueue *q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    return (q->tail >= head) ? (q->tail - head) : 0;
}

inline bool spmcEnqueue(spmcQueue *q, void *data) {
    spmcCell *cell = &q->buffer[q->tail & SPMC_QUEUE_MASK];
    size_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);

    /* Sequence Check:
     * seq == tail: Slot is empty and ready for current generation.
     * seq < tail:  Slot still occupied by consumer or stale. */
    if (unlikely(seq != q->tail)) {
        return false;
    }

    cell->data = data;

    /* Increment sequence to (tail + 1) to publish availability */
    atomic_store_explicit(&cell->sequence, q->tail + 1, memory_order_release);
    q->tail++;

    return true;
}

inline void *spmcDequeue(spmcQueue *q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    spmcCell *cell;
    void *data;

    while (1) {
        cell = &q->buffer[head & SPMC_QUEUE_MASK];
        size_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);

        intptr_t diff = (intptr_t)seq - (intptr_t)(head + 1);

        if (diff == 0) {
            /* Slot has data. Attempt to claim via CAS on head. */
            if (atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {
                data = cell->data;

                /* Mark slot empty for next generation (pos + size) */
                atomic_store_explicit(&cell->sequence, head + SPMC_QUEUE_SIZE, memory_order_release);
                return data;
            }
        } else if (diff < 0) {
            /* Sequence is old; Producer hasn't filled this slot yet. Queue empty. */
            return NULL;
        } else {
            /* diff > 0: Local 'pos' is stale. Reload head. */
            head = atomic_load_explicit(&q->head, memory_order_relaxed);
        }
    }
}

/* ==========================================================================
 * SPSC QUEUE (Single-Producer Single-Consumer)
 * ========================================================================== */

inline void spscInit(spscQueue *q) {
    q->buffer = (void **)zmalloc(sizeof(void *) * SPSC_QUEUE_SIZE);
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    q->head_cache = 0;
    q->tail_cache = 0;
    q->tail_local = 0;
}

inline void spscFree(spscQueue *q) {
    if (q->buffer) {
        zfree(q->buffer);
        q->buffer = NULL;
    }
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
    q->head_cache = 0;
    q->tail_cache = 0;
    q->tail_local = 0;
}

inline bool spscIsFull(spscQueue *q) {
    const size_t curr_tail = q->tail_local;

    if (curr_tail - q->head_cache >= SPSC_QUEUE_SIZE) {
        q->head_cache = atomic_load_explicit(&q->head, memory_order_acquire);

        if (curr_tail - q->head_cache >= SPSC_QUEUE_SIZE) {
            /* Flush any local changes before reporting full */
            if (q->tail_local != q->tail) {
                atomic_store_explicit(&q->tail, q->tail_local, memory_order_release);
            }
            return true;
        }
    }
    return false;
}

inline void spscEnqueue(spscQueue *q, void *data, bool commit) {
    q->buffer[q->tail_local & SPSC_QUEUE_MASK] = data;
    q->tail_local++;

    if (commit) {
        atomic_store_explicit(&q->tail, q->tail_local, memory_order_release);
    }
}

inline void spscCommit(spscQueue *q) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (q->tail_local == tail) return;
    atomic_store_explicit(&q->tail, q->tail_local, memory_order_release);
}

inline size_t spscDequeueBatch(spscQueue *q, void **jobs_out, size_t num_jobs) {
    size_t curr_head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t curr_tail_cache = q->tail_cache;

    if (curr_head == curr_tail_cache) {
        curr_tail_cache = atomic_load_explicit(&q->tail, memory_order_acquire);
        q->tail_cache = curr_tail_cache;
        if (curr_head == curr_tail_cache) return 0;
    }

    size_t available = curr_tail_cache - curr_head;
    size_t count = (num_jobs < available) ? num_jobs : available;

    for (size_t i = 0; i < count; ++i) {
        jobs_out[i] = q->buffer[(curr_head + i) & SPSC_QUEUE_MASK];
    }
    atomic_store_explicit(&q->head, curr_head + count, memory_order_release);
    return count;
}

inline bool spscIsEmpty(spscQueue *q) {
    /* Fast path */
    if (q->tail_local == q->head_cache) {
        return true;
    }
    /* Slow path: refresh head */
    size_t curr_head = atomic_load_explicit(&q->head, memory_order_acquire);
    q->head_cache = curr_head;

    return q->tail_local == curr_head;
}
