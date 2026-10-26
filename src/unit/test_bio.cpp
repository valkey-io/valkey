/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <pthread.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include "bio.h"
#include "server.h"
}

#define TEST_POOL_THREADS 4

static long tasks_done = 0;
static long tasks_started = 0;
static long tasks_saw_all = 0;
static int task_in_bio_thread = -1;
static char task_thread_name[16];

static void incrementTask(void *context) {
    UNUSED(context);
    __atomic_add_fetch(&tasks_done, 1, __ATOMIC_RELAXED);
}

/* Wait until all TEST_POOL_THREADS tasks run at the same time, or give up
 * after approx 5 seconds. */
static void waitForAllTask(void *context) {
    UNUSED(context);
    __atomic_add_fetch(&tasks_started, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < 5000; i++) {
        if (__atomic_load_n(&tasks_started, __ATOMIC_RELAXED) >= TEST_POOL_THREADS) {
            __atomic_add_fetch(&tasks_saw_all, 1, __ATOMIC_RELAXED);
            break;
        }
        usleep(1000);
    }
}

static void inBioThreadTask(void *context) {
    UNUSED(context);
    task_in_bio_thread = inBioThread();
}

static void threadNameTask(void *context) {
    UNUSED(context);
#if defined(__linux__) || defined(__APPLE__) || defined(__NetBSD__)
    pthread_getname_np(pthread_self(), task_thread_name, sizeof(task_thread_name));
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_get_name_np(pthread_self(), task_thread_name, sizeof(task_thread_name));
#endif
}

class BioPoolTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        /* The pool is created on the first bioCreateTask() call, with the
         * size set here. It is created once for the whole test binary. */
        server.bio_pool_threads = TEST_POOL_THREADS;
    }
};

TEST_F(BioPoolTest, RunsAllTasks) {
    tasks_done = 0;
    for (int i = 0; i < 1000; i++) bioCreateTask(incrementTask, NULL);
    bioDrainWorker(BIO_USER_WORK);
    EXPECT_EQ(__atomic_load_n(&tasks_done, __ATOMIC_RELAXED), 1000);
    EXPECT_EQ(bioPendingJobsOfType(BIO_USER_WORK), 0ul);
}

TEST_F(BioPoolTest, RunsTasksInParallel) {
    tasks_started = 0;
    tasks_saw_all = 0;
    for (int i = 0; i < TEST_POOL_THREADS; i++) bioCreateTask(waitForAllTask, NULL);
    bioDrainWorker(BIO_USER_WORK);
    /* Each task sees all the others running only if they run in parallel. */
    EXPECT_EQ(__atomic_load_n(&tasks_saw_all, __ATOMIC_RELAXED), TEST_POOL_THREADS);
}

TEST_F(BioPoolTest, InBioThread) {
    task_in_bio_thread = -1;
    bioCreateTask(inBioThreadTask, NULL);
    bioDrainWorker(BIO_USER_WORK);
    EXPECT_EQ(task_in_bio_thread, 1);
    EXPECT_EQ(inBioThread(), 0);
}

TEST_F(BioPoolTest, ThreadNameHasNumber) {
#if !(defined(__linux__) || defined(__APPLE__) || defined(__NetBSD__) || defined(__FreeBSD__) || defined(__OpenBSD__))
    GTEST_SKIP() << "No way to read the thread name on this platform";
#endif
    task_thread_name[0] = '\0';
    bioCreateTask(threadNameTask, NULL);
    bioDrainWorker(BIO_USER_WORK);
    /* The name is "bio_pool-N", with N from 1 to TEST_POOL_THREADS. */
    EXPECT_EQ(strncmp(task_thread_name, "bio_pool-", 9), 0) << task_thread_name;
    int num = atoi(task_thread_name + 9);
    EXPECT_GE(num, 1);
    EXPECT_LE(num, TEST_POOL_THREADS);
}
