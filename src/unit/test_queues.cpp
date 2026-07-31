/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <atomic>
#include <pthread.h>
#include <sched.h>

extern "C" {
#include "queues.h"
}

#define STRESS_ITERATIONS 100000
#define NUM_THREADS 4

/* ============== SPSC Queue Tests ============== */

class SpscQueueTest : public ::testing::Test {
  protected:
    spscQueue q;
    static constexpr size_t SPSC_QUEUE_SIZE = 4096;

    struct ConsumerArg {
        spscQueue *q;
        size_t count;
    };

    void SetUp() override {
        spscInit(&q, SPSC_QUEUE_SIZE);
    }

    void TearDown() override {
        spscFree(&q);
    }

    static void *consumerThread(void *arg) {
        ConsumerArg *ta = (ConsumerArg *)arg;
        void *jobs[64];
        size_t total = 0;

        while (total < ta->count) {
            size_t n = spscDequeueBatch(ta->q, jobs, 64);
            total += n;
            if (n == 0) sched_yield();
        }
        return (void *)total;
    }
};

TEST_F(SpscQueueTest, TestSpscBasicEnqueueDequeue) {
    void *data1 = (void *)0x1000;
    void *data2 = (void *)0x2000;

    EXPECT_FALSE(spscIsFull(&q));
    spscEnqueue(&q, data1, true);
    spscEnqueue(&q, data2, true);

    void *jobs[2];
    size_t count = spscDequeueBatch(&q, jobs, 2);

    EXPECT_EQ(count, 2u);
    EXPECT_EQ(jobs[0], data1);
    EXPECT_EQ(jobs[1], data2);
    EXPECT_TRUE(spscIsEmpty(&q));
}

TEST_F(SpscQueueTest, TestSpscBatchCommit) {
    spscEnqueue(&q, (void *)0x1000, false);
    spscEnqueue(&q, (void *)0x2000, false);

    void *jobs[2];
    size_t count = spscDequeueBatch(&q, jobs, 2);
    EXPECT_EQ(count, 0u);

    spscCommit(&q);
    count = spscDequeueBatch(&q, jobs, 2);
    EXPECT_EQ(count, 2u);

    spscCommit(&q);
    count = spscDequeueBatch(&q, jobs, 2);
    EXPECT_EQ(count, 0u);
}

TEST_F(SpscQueueTest, TestSpscFullAndWrapAround) {
    for (int round = 0; round < 3; round++) {
        for (size_t i = 0; i < SPSC_QUEUE_SIZE; i++) {
            EXPECT_FALSE(spscIsFull(&q));
            spscEnqueue(&q, (void *)(uintptr_t)(i + 1), true);
        }
        EXPECT_TRUE(spscIsFull(&q));

        void *jobs[64];
        size_t total = 0;
        size_t count;
        while ((count = spscDequeueBatch(&q, jobs, 64)) > 0) {
            total += count;
        }
        EXPECT_EQ(total, (size_t)SPSC_QUEUE_SIZE);
        EXPECT_TRUE(spscIsEmpty(&q));
        EXPECT_FALSE(spscIsFull(&q));
    }
}

TEST_F(SpscQueueTest, TestSpscEmptyDequeue) {
    void *jobs[1];
    EXPECT_EQ(spscDequeueBatch(&q, jobs, 1), 0u);
}

TEST_F(SpscQueueTest, TestSpscPartialBatchDequeue) {
    for (int i = 0; i < 5; i++) {
        spscEnqueue(&q, (void *)(uintptr_t)(i + 1), true);
    }

    void *jobs[64];
    EXPECT_EQ(spscDequeueBatch(&q, jobs, 64), 5u);
}

/* Stress test: SPSC queue */
TEST_F(SpscQueueTest, TestSpscConcurrent) {
    ConsumerArg arg = {&q, STRESS_ITERATIONS};
    pthread_t consumer;
    pthread_create(&consumer, NULL, consumerThread, &arg);

    for (size_t i = 0; i < STRESS_ITERATIONS; i++) {
        while (spscIsFull(&q)) sched_yield();
        spscEnqueue(&q, (void *)(uintptr_t)(i + 1), true);
    }

    void *result;
    pthread_join(consumer, &result);
    EXPECT_EQ((size_t)result, (size_t)STRESS_ITERATIONS);
}

/* ============== SPMC Queue Tests ============== */

class SpmcQueueTest : public ::testing::Test {
  protected:
    spmcQueue q;
    static constexpr size_t SPMC_QUEUE_SIZE = 4096;

    struct ConsumerArg {
        spmcQueue *q;
        std::atomic<size_t> *consumed;
        std::atomic<int> *done;
    };

    void SetUp() override {
        spmcInit(&q, SPMC_QUEUE_SIZE);
        ASSERT_NE(q.buffer, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(q.buffer) % CACHE_LINE_SIZE, 0u);
    }

    void TearDown() override {
        spmcFree(&q);
    }

    static void *consumerThread(void *arg) {
        ConsumerArg *ta = (ConsumerArg *)arg;
        size_t local_count = 0;

        while (!ta->done->load()) {
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
        ta->consumed->fetch_add(local_count);
        return NULL;
    }
};

TEST_F(SpmcQueueTest, TestSpmcBasicEnqueueDequeue) {
    void *data1 = (void *)0x1000;
    void *data2 = (void *)0x2000;

    EXPECT_TRUE(spmcEnqueue(&q, data1));
    EXPECT_TRUE(spmcEnqueue(&q, data2));

    void *result1 = spmcDequeue(&q);
    void *result2 = spmcDequeue(&q);
    void *result3 = spmcDequeue(&q);

    EXPECT_EQ(result1, data1);
    EXPECT_EQ(result2, data2);
    EXPECT_EQ(result3, nullptr);
    EXPECT_TRUE(spmcIsEmpty(&q));
}

TEST_F(SpmcQueueTest, TestSpmcFullAndWrapAround) {
    for (int round = 0; round < 3; round++) {
        for (size_t i = 0; i < SPMC_QUEUE_SIZE; i++) {
            EXPECT_TRUE(spmcEnqueue(&q, (void *)(uintptr_t)(i + 1)));
        }
        EXPECT_FALSE(spmcEnqueue(&q, (void *)0xDEAD));

        for (size_t i = 0; i < SPMC_QUEUE_SIZE; i++) {
            void *data = spmcDequeue(&q);
            EXPECT_EQ(data, (void *)(uintptr_t)(i + 1));
        }
        EXPECT_EQ(spmcDequeue(&q), nullptr);
    }
}

TEST_F(SpmcQueueTest, TestSpmcSize) {
    EXPECT_EQ(spmcSize(&q), 0u);
    spmcEnqueue(&q, (void *)0x1);
    EXPECT_EQ(spmcSize(&q), 1u);
    spmcEnqueue(&q, (void *)0x2);
    EXPECT_EQ(spmcSize(&q), 2u);

    spmcDequeue(&q);
    EXPECT_EQ(spmcSize(&q), 1u);
}

/* Stress test: SPMC queue */
TEST_F(SpmcQueueTest, TestSpmcConcurrent) {
    std::atomic<size_t> consumed{0};
    std::atomic<int> done{0};
    ConsumerArg arg = {&q, &consumed, &done};

    pthread_t consumers[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&consumers[i], NULL, consumerThread, &arg);
    }

    for (size_t i = 0; i < STRESS_ITERATIONS; i++) {
        while (!spmcEnqueue(&q, (void *)(uintptr_t)(i + 1))) sched_yield();
    }

    while (!spmcIsEmpty(&q)) sched_yield();
    done.store(1);

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(consumers[i], NULL);
    }

    EXPECT_EQ(consumed.load(), (size_t)STRESS_ITERATIONS);
}

/* ============== MPSC Queue Tests ============== */

class MpscQueueTest : public ::testing::Test {
  protected:
    mpscQueue q;
    static constexpr size_t MPSC_QUEUE_SIZE = 16384;

    struct ProducerArg {
        mpscQueue *q;
        size_t items_per_thread;
        int thread_id;
    };

    void SetUp() override {
        mpscInit(&q, MPSC_QUEUE_SIZE);
    }

    void TearDown() override {
        mpscFree(&q);
    }

    static void *producerThread(void *arg) {
        ProducerArg *pa = (ProducerArg *)arg;

        for (size_t i = 0; i < pa->items_per_thread; i++) {
            mpscTicket ticket = {0};
            void *data = (void *)(uintptr_t)((pa->thread_id << 20) | (i + 1));
            while (!mpscEnqueue(pa->q, data, &ticket)) sched_yield();
        }
        return NULL;
    }
};

TEST_F(MpscQueueTest, TestMpscBasicEnqueueDequeue) {
    void *data1 = (void *)0x1000;
    void *data2 = (void *)0x2000;

    mpscTicket ticket = {0};
    EXPECT_TRUE(mpscEnqueue(&q, data1, &ticket));
    EXPECT_FALSE(ticket.has_reservation);
    ticket = mpscTicket{0};
    EXPECT_TRUE(mpscEnqueue(&q, data2, &ticket));
    EXPECT_FALSE(ticket.has_reservation);

    void *jobs[2];
    size_t count = mpscDequeueBatch(&q, jobs, 2);

    EXPECT_EQ(count, 2u);
    EXPECT_EQ(jobs[0], data1);
    EXPECT_EQ(jobs[1], data2);

    count = mpscDequeueBatch(&q, jobs, 2);
    EXPECT_EQ(count, 0u);
}

TEST_F(MpscQueueTest, TestMpscTicketRetry) {
    for (size_t i = 0; i < MPSC_QUEUE_SIZE; i++) {
        mpscTicket ticket = {0};
        EXPECT_TRUE(mpscEnqueue(&q, (void *)(uintptr_t)(i + 1), &ticket));
    }

    mpscTicket ticket = {0};
    EXPECT_FALSE(mpscEnqueue(&q, (void *)0xBEEF, &ticket));
    EXPECT_TRUE(ticket.has_reservation);

    void *jobs[100];
    size_t count = mpscDequeueBatch(&q, jobs, 100);
    EXPECT_GT(count, 0u);

    EXPECT_TRUE(mpscEnqueue(&q, (void *)0xBEEF, &ticket));
    EXPECT_FALSE(ticket.has_reservation);
}

TEST_F(MpscQueueTest, TestMpscFullAndWrapAround) {
    for (int round = 0; round < 3; round++) {
        for (size_t i = 0; i < MPSC_QUEUE_SIZE; i++) {
            mpscTicket ticket = {0};
            EXPECT_TRUE(mpscEnqueue(&q, (void *)(uintptr_t)(i + 1), &ticket));
        }

        void *jobs[64];
        size_t total = 0;
        size_t count;
        while ((count = mpscDequeueBatch(&q, jobs, 64)) > 0) {
            total += count;
        }
        EXPECT_EQ(total, (size_t)MPSC_QUEUE_SIZE);
    }
}

TEST_F(MpscQueueTest, TestMpscInterleavedOperations) {
    for (size_t i = 0; i < 100; i++) {
        mpscTicket ticket = {0};
        EXPECT_TRUE(mpscEnqueue(&q, (void *)(uintptr_t)(i + 1), &ticket));
    }

    void *jobs[50];
    size_t count = mpscDequeueBatch(&q, jobs, 50);
    EXPECT_EQ(count, 50u);
    for (size_t i = 0; i < 50; i++) {
        EXPECT_EQ(jobs[i], (void *)(uintptr_t)(i + 1));
    }

    for (size_t i = 100; i < 150; i++) {
        mpscTicket ticket = {0};
        EXPECT_TRUE(mpscEnqueue(&q, (void *)(uintptr_t)(i + 1), &ticket));
    }

    size_t total = 0;
    while ((count = mpscDequeueBatch(&q, jobs, 50)) > 0) {
        total += count;
    }
    EXPECT_EQ(total, 100u);
}

/* Stress test: MPSC queue */
TEST_F(MpscQueueTest, TestMpscConcurrent) {
    size_t items_per_thread = STRESS_ITERATIONS / NUM_THREADS;
    pthread_t producers[NUM_THREADS];
    ProducerArg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i] = ProducerArg{&q, items_per_thread, i};
        pthread_create(&producers[i], NULL, producerThread, &args[i]);
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

    EXPECT_EQ(total, expected);
}
