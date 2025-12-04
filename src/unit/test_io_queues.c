/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

/* Mock inMainThread for tests - must be declared before io_queues.h */
static _Thread_local int mock_main_thread = 1;
static int inMainThread(void) {
    return mock_main_thread;
}

#define serverAssert(e) assert(e)
#define debugServerAssert(e) assert(e)

#include "../io_queues.h"
#include "test_help.h"

/* SPSC Queue Tests */

int test_spscBasicEnqueueDequeue(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    spscQueue q;
    spscInit(&q);

    void *data1 = (void *)0x1000;
    void *data2 = (void *)0x2000;

    /* Enqueue from main thread */
    mock_main_thread = 1;
    TEST_ASSERT(!spscIsFull(&q));
    spscEnqueue(&q, data1, true);
    spscEnqueue(&q, data2, true);

    /* Dequeue from IO thread */
    mock_main_thread = 0;
    void *jobs[2];
    size_t count = spscDequeueBatch(&q, jobs, 2);

    TEST_ASSERT(count == 2);
    TEST_ASSERT(jobs[0] == data1);
    TEST_ASSERT(jobs[1] == data2);

    /* Queue should be empty now */
    mock_main_thread = 1;
    TEST_ASSERT(spscIsEmpty(&q));

    spscFree(&q);
    return 0;
}

int test_spscBatchCommit(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    spscQueue q;
    spscInit(&q);

    mock_main_thread = 1;

    /* Enqueue without commit */
    spscEnqueue(&q, (void *)0x1000, false);
    spscEnqueue(&q, (void *)0x2000, false);

    /* Consumer shouldn't see uncommitted data */
    mock_main_thread = 0;
    void *jobs[2];
    size_t count = spscDequeueBatch(&q, jobs, 2);
    TEST_ASSERT(count == 0);

    /* Commit and try again */
    mock_main_thread = 1;
    spscCommit(&q);

    mock_main_thread = 0;
    count = spscDequeueBatch(&q, jobs, 2);
    TEST_ASSERT(count == 2);

    /* Test commit with no pending data */
    mock_main_thread = 1;
    spscCommit(&q);

    mock_main_thread = 0;
    count = spscDequeueBatch(&q, jobs, 2);
    TEST_ASSERT(count == 0);

    spscFree(&q);
    return 0;
}

int test_spscFullAndWrapAround(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    spscQueue q;
    spscInit(&q);

    /* Fill, drain, refill to test wrap-around */
    for (int round = 0; round < 3; round++) {
        mock_main_thread = 1;
        for (size_t i = 0; i < SPSC_QUEUE_SIZE; i++) {
            TEST_ASSERT(!spscIsFull(&q));
            spscEnqueue(&q, (void *)(uintptr_t)(i + 1), true);
        }
        TEST_ASSERT(spscIsFull(&q));

        mock_main_thread = 0;
        void *jobs[64];
        size_t total = 0;
        size_t count;
        while ((count = spscDequeueBatch(&q, jobs, 64)) > 0) {
            total += count;
        }
        TEST_ASSERT(total == SPSC_QUEUE_SIZE);

        mock_main_thread = 1;
        TEST_ASSERT(spscIsEmpty(&q));
        TEST_ASSERT(!spscIsFull(&q));
    }

    spscFree(&q);
    return 0;
}

int test_spscEmptyDequeue(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    spscQueue q;
    spscInit(&q);

    mock_main_thread = 0;
    void *jobs[1];
    TEST_ASSERT(spscDequeueBatch(&q, jobs, 1) == 0);

    spscFree(&q);
    return 0;
}

int test_spscPartialBatchDequeue(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    spscQueue q;
    spscInit(&q);

    mock_main_thread = 1;
    for (int i = 0; i < 5; i++) {
        spscEnqueue(&q, (void *)(uintptr_t)(i + 1), true);
    }

    mock_main_thread = 0;
    void *jobs[64];
    TEST_ASSERT(spscDequeueBatch(&q, jobs, 64) == 5);

    spscFree(&q);
    return 0;
}

/* SPMC Queue Tests */

int test_spmcBasicEnqueueDequeue(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    spmcQueue q;
    spmcInit(&q);

    void *data1 = (void *)0x1000;
    void *data2 = (void *)0x2000;

    mock_main_thread = 1;
    TEST_ASSERT(spmcEnqueue(&q, data1));
    TEST_ASSERT(spmcEnqueue(&q, data2));

    mock_main_thread = 0;
    void *result1 = spmcDequeue(&q);
    void *result2 = spmcDequeue(&q);
    void *result3 = spmcDequeue(&q);

    TEST_ASSERT(result1 == data1);
    TEST_ASSERT(result2 == data2);
    TEST_ASSERT(result3 == NULL);

    mock_main_thread = 1;
    TEST_ASSERT(spmcIsEmpty(&q));

    spmcFree(&q);
    return 0;
}

int test_spmcFullAndWrapAround(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    spmcQueue q;
    spmcInit(&q);

    for (int round = 0; round < 3; round++) {
        mock_main_thread = 1;
        for (size_t i = 0; i < SPMC_QUEUE_SIZE; i++) {
            TEST_ASSERT(spmcEnqueue(&q, (void *)(uintptr_t)(i + 1)));
        }

        /* Queue should be full */
        TEST_ASSERT(!spmcEnqueue(&q, (void *)0xDEAD));

        mock_main_thread = 0;
        for (size_t i = 0; i < SPMC_QUEUE_SIZE; i++) {
            void *data = spmcDequeue(&q);
            TEST_ASSERT(data == (void *)(uintptr_t)(i + 1));
        }
        TEST_ASSERT(spmcDequeue(&q) == NULL);
    }

    spmcFree(&q);
    return 0;
}

int test_spmcSize(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    spmcQueue q;
    spmcInit(&q);

    mock_main_thread = 1;
    TEST_ASSERT(spmcSize(&q) == 0);
    spmcEnqueue(&q, (void *)0x1);
    TEST_ASSERT(spmcSize(&q) == 1);
    spmcEnqueue(&q, (void *)0x2);
    TEST_ASSERT(spmcSize(&q) == 2);

    mock_main_thread = 0;
    spmcDequeue(&q);
    TEST_ASSERT(spmcSize(&q) == 1);

    spmcFree(&q);
    return 0;
}

/* MPSC Queue Tests */

int test_mpscBasicEnqueueDequeue(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mpscQueue q;
    mpscInit(&q);

    void *data1 = (void *)0x1000;
    void *data2 = (void *)0x2000;

    /* Enqueue from producer */
    mpscTicket ticket = {0};
    TEST_ASSERT(mpscEnqueue(&q, data1, &ticket));
    TEST_ASSERT(!ticket.has_reservation);
    ticket = (mpscTicket){0};
    TEST_ASSERT(mpscEnqueue(&q, data2, &ticket));
    TEST_ASSERT(!ticket.has_reservation);

    /* Dequeue from consumer */
    void *jobs[2];
    size_t count = mpscDequeueBatch(&q, jobs, 2);

    TEST_ASSERT(count == 2);
    TEST_ASSERT(jobs[0] == data1);
    TEST_ASSERT(jobs[1] == data2);

    /* Queue should be empty */
    count = mpscDequeueBatch(&q, jobs, 2);
    TEST_ASSERT(count == 0);

    mpscFree(&q);
    return 0;
}

int test_mpscTicketRetry(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mpscQueue q;
    mpscInit(&q);

    /* Fill the queue */
    for (size_t i = 0; i < MPSC_QUEUE_SIZE; i++) {
        mpscTicket ticket = {0};
        TEST_ASSERT(mpscEnqueue(&q, (void *)(uintptr_t)(i + 1), &ticket));
    }

    /* Next enqueue should fail and return a ticket */
    mpscTicket ticket = {0};
    TEST_ASSERT(!mpscEnqueue(&q, (void *)0xBEEF, &ticket));
    TEST_ASSERT(ticket.has_reservation);

    /* Drain some items */
    void *jobs[100];
    size_t count = mpscDequeueBatch(&q, jobs, 100);
    TEST_ASSERT(count > 0);

    /* Retry with same ticket should succeed */
    TEST_ASSERT(mpscEnqueue(&q, (void *)0xBEEF, &ticket));
    TEST_ASSERT(!ticket.has_reservation);

    mpscFree(&q);
    return 0;
}

int test_mpscFullAndWrapAround(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mpscQueue q;
    mpscInit(&q);

    for (int round = 0; round < 3; round++) {
        for (size_t i = 0; i < MPSC_QUEUE_SIZE; i++) {
            mpscTicket ticket = {0};
            TEST_ASSERT(mpscEnqueue(&q, (void *)(uintptr_t)(i + 1), &ticket));
        }

        void *jobs[64];
        size_t total = 0;
        size_t count;
        while ((count = mpscDequeueBatch(&q, jobs, 64)) > 0) {
            total += count;
        }
        TEST_ASSERT(total == MPSC_QUEUE_SIZE);
    }

    mpscFree(&q);
    return 0;
}

int test_mpscInterleavedOperations(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mpscQueue q;
    mpscInit(&q);

    /* Enqueue some */
    for (size_t i = 0; i < 100; i++) {
        mpscTicket ticket = {0};
        TEST_ASSERT(mpscEnqueue(&q, (void *)(uintptr_t)(i + 1), &ticket));
    }

    /* Dequeue half */
    void *jobs[50];
    size_t count = mpscDequeueBatch(&q, jobs, 50);
    TEST_ASSERT(count == 50);
    for (size_t i = 0; i < 50; i++) {
        TEST_ASSERT(jobs[i] == (void *)(uintptr_t)(i + 1));
    }

    /* Enqueue more */
    for (size_t i = 100; i < 150; i++) {
        mpscTicket ticket = {0};
        TEST_ASSERT(mpscEnqueue(&q, (void *)(uintptr_t)(i + 1), &ticket));
    }

    /* Drain remaining */
    size_t total = 0;
    while ((count = mpscDequeueBatch(&q, jobs, 50)) > 0) {
        total += count;
    }
    TEST_ASSERT(total == 100);

    mpscFree(&q);
    return 0;
}

/* ============== Multi-threaded Stress Tests ============== */

#define STRESS_ITERATIONS 100000
#define NUM_THREADS 4

/* SPSC concurrent test */
typedef struct {
    spscQueue *q;
    size_t count;
} spscThreadArg;

static void *spscConsumerThread(void *arg) {
    spscThreadArg *ta = (spscThreadArg *)arg;
    mock_main_thread = 0;
    void *jobs[64];
    size_t total = 0;

    while (total < ta->count) {
        size_t n = spscDequeueBatch(ta->q, jobs, 64);
        total += n;
        if (n == 0) sched_yield();
    }
    return (void *)total;
}

int test_spscConcurrent(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    spscQueue q;
    spscInit(&q);

    spscThreadArg arg = {&q, STRESS_ITERATIONS};
    pthread_t consumer;
    pthread_create(&consumer, NULL, spscConsumerThread, &arg);

    mock_main_thread = 1;
    for (size_t i = 0; i < STRESS_ITERATIONS; i++) {
        while (spscIsFull(&q)) sched_yield();
        spscEnqueue(&q, (void *)(uintptr_t)(i + 1), true);
    }

    void *result;
    pthread_join(consumer, &result);
    TEST_ASSERT((size_t)result == STRESS_ITERATIONS);

    spscFree(&q);
    return 0;
}

/* SPMC concurrent test - multiple consumers */
typedef struct {
    spmcQueue *q;
    atomic_size_t *consumed;
    atomic_int *done;
} spmcThreadArg;

static void *spmcConsumerThread(void *arg) {
    spmcThreadArg *ta = (spmcThreadArg *)arg;
    mock_main_thread = 0;
    size_t local_count = 0;

    while (!atomic_load(ta->done)) {
        void *data = spmcDequeue(ta->q);
        if (data) {
            local_count++;
        } else {
            sched_yield();
        }
    }
    /* Drain remaining after done signal */
    void *data;
    while ((data = spmcDequeue(ta->q)) != NULL) {
        local_count++;
    }
    atomic_fetch_add(ta->consumed, local_count);
    return NULL;
}

int test_spmcConcurrent(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    spmcQueue q;
    spmcInit(&q);

    atomic_size_t consumed = 0;
    atomic_int done = 0;
    spmcThreadArg arg = {&q, &consumed, &done};

    pthread_t consumers[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&consumers[i], NULL, spmcConsumerThread, &arg);
    }

    mock_main_thread = 1;
    for (size_t i = 0; i < STRESS_ITERATIONS; i++) {
        while (!spmcEnqueue(&q, (void *)(uintptr_t)(i + 1))) sched_yield();
    }

    while (!spmcIsEmpty(&q)) sched_yield();
    atomic_store(&done, 1);

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(consumers[i], NULL);
    }

    TEST_ASSERT(atomic_load(&consumed) == STRESS_ITERATIONS);

    spmcFree(&q);
    return 0;
}

/* MPSC concurrent test - multiple producers */
typedef struct {
    mpscQueue *q;
    size_t items_per_thread;
    int thread_id;
} mpscProducerArg;

static void *mpscProducerThread(void *arg) {
    mpscProducerArg *pa = (mpscProducerArg *)arg;

    for (size_t i = 0; i < pa->items_per_thread; i++) {
        mpscTicket ticket = {0};
        void *data = (void *)(uintptr_t)((pa->thread_id << 20) | (i + 1));
        while (!mpscEnqueue(pa->q, data, &ticket)) sched_yield();
    }
    return NULL;
}

int test_mpscConcurrent(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mpscQueue q;
    mpscInit(&q);

    size_t items_per_thread = STRESS_ITERATIONS / NUM_THREADS;
    pthread_t producers[NUM_THREADS];
    mpscProducerArg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i] = (mpscProducerArg){&q, items_per_thread, i};
        pthread_create(&producers[i], NULL, mpscProducerThread, &args[i]);
    }

    size_t total = 0;
    void *jobs[64];
    size_t expected = items_per_thread * NUM_THREADS;

    while (total < expected) {
        size_t n = mpscDequeueBatch(&q, jobs, 64);
        total += n;
        if (n == 0) sched_yield();
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(producers[i], NULL);
    }

    TEST_ASSERT(total == expected);

    mpscFree(&q);
    return 0;
}
