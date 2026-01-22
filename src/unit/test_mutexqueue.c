/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "../mutexqueue.h"
#include "test_help.h"
#include <pthread.h>
#include <time.h>

/* Helper functions */
static void add(mutexQueue *q, long value) {
    unsigned long len = mutexQueueLength(q);
    mutexQueueAdd(q, (void *)value);
    TEST_EXPECT(mutexQueueLength(q) == len + 1);
}

static void priorityAdd(mutexQueue *q, long value) {
    unsigned long len = mutexQueueLength(q);
    mutexQueuePushPriority(q, (void *)value);
    TEST_EXPECT(mutexQueueLength(q) == len + 1);
}

static void popTest(mutexQueue *q, long expected) {
    unsigned long len = mutexQueueLength(q);
    long value = (long)mutexQueuePop(q, false);
    TEST_EXPECT(mutexQueueLength(q) == len - 1);
    TEST_EXPECT(value == expected);
}

/* Test: simplePushPop */
int test_mutexQueueSimplePushPop(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mutexQueue *q = mutexQueueCreate();
    TEST_EXPECT(mutexQueueLength(q) == 0ul);
    add(q, 1);
    popTest(q, 1);
    TEST_EXPECT(mutexQueuePop(q, false) == NULL);

    mutexQueueRelease(q);
    return 0;
}

/* Test: doublePushPop */
int test_mutexQueueDoublePushPop(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mutexQueue *q = mutexQueueCreate();
    add(q, 1);
    add(q, 2);
    popTest(q, 1);
    popTest(q, 2);

    mutexQueueRelease(q);
    return 0;
}

/* Test: priorityOrdering */
int test_mutexQueuePriorityOrdering(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mutexQueue *q = mutexQueueCreate();
    add(q, 10);
    priorityAdd(q, 1);
    add(q, 11);
    priorityAdd(q, 2);
    popTest(q, 1);
    popTest(q, 2);
    popTest(q, 10);
    popTest(q, 11);
    TEST_EXPECT(mutexQueuePop(q, false) == NULL);

    mutexQueueRelease(q);
    return 0;
}

/* Test: fifoPopAll */
int test_mutexQueueFifoPopAll(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mutexQueue *q = mutexQueueCreate();
    add(q, 10);
    priorityAdd(q, 1);
    add(q, 11);
    priorityAdd(q, 2);

    fifo *f = mutexQueuePopAll(q, false);
    TEST_ASSERT(f != NULL); /* Fatal - can't continue if NULL */
    TEST_EXPECT(mutexQueuePop(q, false) == NULL);
    TEST_EXPECT(mutexQueuePopAll(q, false) == NULL);
    TEST_EXPECT(mutexQueueLength(q) == 0ul);

    void *ptr;
    TEST_EXPECT(fifoPop(f, &ptr) && (unsigned long)ptr == 1ul);
    TEST_EXPECT(fifoPop(f, &ptr) && (unsigned long)ptr == 2ul);
    TEST_EXPECT(fifoPop(f, &ptr) && (unsigned long)ptr == 10ul);
    TEST_EXPECT(fifoPop(f, &ptr) && (unsigned long)ptr == 11ul);
    TEST_EXPECT(fifoLength(f) == 0);

    fifoRelease(f);
    mutexQueueRelease(q);
    return 0;
}

/* Test: fifoAddMultiple */
int test_mutexQueueFifoAddMultiple(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mutexQueue *q = mutexQueueCreate();
    add(q, 1);

    fifo *f = fifoCreate();
    fifoPush(f, (void *)2);
    fifoPush(f, (void *)3);
    mutexQueueAddMultiple(q, f);
    TEST_EXPECT(fifoLength(f) == 0u);
    fifoRelease(f);

    add(q, 4);
    priorityAdd(q, 0);
    popTest(q, 0);
    popTest(q, 1);
    popTest(q, 2);
    popTest(q, 3);
    popTest(q, 4);
    TEST_EXPECT(mutexQueuePop(q, false) == NULL);
    TEST_EXPECT(mutexQueueLength(q) == 0ul);

    mutexQueueRelease(q);
    return 0;
}

/* Thread functions for concurrent tests */
static void *queue_writer(void *arg) {
    mutexQueue *queue = (mutexQueue *)arg;
    for (int i = 1; i <= 1000; i++) {
        mutexQueueAdd(queue, (void *)(long)i);
    }
    return NULL;
}

static void *queue_reader(void *arg) {
    mutexQueue *queue = (mutexQueue *)arg;
    int count = 0;
    while (count < 1000) {
        long value = (long)mutexQueuePop(queue, true);
        TEST_EXPECT(value != 0); /* Should never be null if blocking */
        count++;
    }
    return NULL;
}

/* Test: simpleThread */
int test_mutexQueueSimpleThread(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mutexQueue *q = mutexQueueCreate();
    int rc;
    pthread_t writer, reader;

    rc = pthread_create(&writer, NULL, &queue_writer, q);
    TEST_ASSERT(rc == 0); /* Fatal - can't continue if thread creation fails */
    rc = pthread_create(&reader, NULL, &queue_reader, q);
    TEST_ASSERT(rc == 0);
    rc = pthread_join(writer, NULL);
    TEST_ASSERT(rc == 0);
    rc = pthread_join(reader, NULL);
    TEST_ASSERT(rc == 0);
    TEST_EXPECT(mutexQueueLength(q) == 0ul);

    rc = pthread_create(&reader, NULL, &queue_reader, q);
    TEST_ASSERT(rc == 0);
    rc = pthread_create(&writer, NULL, &queue_writer, q);
    TEST_ASSERT(rc == 0);
    rc = pthread_join(writer, NULL);
    TEST_ASSERT(rc == 0);
    rc = pthread_join(reader, NULL);
    TEST_ASSERT(rc == 0);
    TEST_EXPECT(mutexQueueLength(q) == 0ul);

    mutexQueueRelease(q);
    return 0;
}

/* Test: parallelWriters */
int test_mutexQueueParallelWriters(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mutexQueue *q = mutexQueueCreate();
    const int num_threads = 20;
    int rc;
    pthread_t writer[num_threads];

    for (int i = 0; i < num_threads; i++) {
        rc = pthread_create(&writer[i], NULL, &queue_writer, q);
        TEST_ASSERT(rc == 0);
    }

    for (int i = 0; i < num_threads; i++) {
        rc = pthread_join(writer[i], NULL);
        TEST_ASSERT(rc == 0);
    }

    TEST_EXPECT(mutexQueueLength(q) == (unsigned long)(num_threads * 1000));

    fifo *f = mutexQueuePopAll(q, false);
    fifoRelease(f);
    mutexQueueRelease(q);
    return 0;
}

/* Test: parallelReaders */
int test_mutexQueueParallelReaders(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mutexQueue *q = mutexQueueCreate();
    const int num_threads = 20;
    int rc;
    pthread_t reader[num_threads];

    /* Start readers in advance - we want them fighting... */
    for (int i = 0; i < num_threads; i++) {
        rc = pthread_create(&reader[i], NULL, &queue_reader, q);
        TEST_ASSERT(rc == 0); /* Fatal - can't continue if thread creation fails */
    }

    /* Now perform writes serially... */
    for (int i = 0; i < num_threads; i++) {
        queue_writer(q);
        /* make sure other threads get to fight with a short sleep (don't write all at once!) */
        nanosleep((const struct timespec[]){{0, 10000000L}}, NULL); /* 10ms */
    }

    /* Readers should finish */
    for (int i = 0; i < num_threads; i++) {
        rc = pthread_join(reader[i], NULL);
        TEST_ASSERT(rc == 0);
    }

    TEST_EXPECT(mutexQueueLength(q) == 0ul);

    mutexQueueRelease(q);
    return 0;
}

/* Test: parallelReadWrite */
int test_mutexQueueParallelReadWrite(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    mutexQueue *q = mutexQueueCreate();
    const int num_threads = 20;
    int rc;
    pthread_t reader[num_threads];
    pthread_t writer[num_threads];

    for (int i = 0; i < num_threads; i++) {
        rc = pthread_create(&writer[i], NULL, &queue_writer, q);
        TEST_ASSERT(rc == 0);
        rc = pthread_create(&reader[i], NULL, &queue_reader, q);
        TEST_ASSERT(rc == 0);
    }

    for (int i = 0; i < num_threads; i++) {
        rc = pthread_join(writer[i], NULL);
        TEST_ASSERT(rc == 0);
        rc = pthread_join(reader[i], NULL);
        TEST_ASSERT(rc == 0);
    }

    TEST_EXPECT(mutexQueueLength(q) == 0ul);

    mutexQueueRelease(q);
    return 0;
}
