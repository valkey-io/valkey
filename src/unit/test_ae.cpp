/*
 * Test: ae event loop survives when all time events are deleted.
 *
 * This test verifies that aeProcessEvents() does not crash when
 * all time events have been lazy-deleted (id == AE_DELETED_EVENT_ID).
 * The bug was a NULL dereference in usUntilEarliestTimer() when the
 * scan loop found no valid time events.
 *
 * A permanently-readable pipe is registered as a file event so that
 * aeApiPoll() returns immediately instead of blocking forever (the
 * test would otherwise hang because there are no file events and no
 * valid time events to wake the loop).
 */

#include "gtest/gtest.h"

extern "C" {
#include "../ae.h"
#include <unistd.h>
}

/* File event callback for the always-readable pipe. */
static void pipeFileProc(aeEventLoop *el, int fd, void *clientData, int mask) {
    (void)el;
    (void)fd;
    (void)clientData;
    (void)mask;
    /* Intentionally do nothing; the pipe stays readable via the unread byte. */
}

/* Time-event callback that self-deletes on first fire. */
static long long selfDeletingProc(aeEventLoop *el, long long id, void *clientData) {
    (void)el;
    (void)id;
    int *called = (int *)clientData;
    *called = 1;
    return AE_NOMORE;  /* delete this event */
}

/* Time-event callback that does nothing (stays active). */
static long long noopProc(aeEventLoop *el, long long id, void *clientData) {
    (void)el;
    (void)id;
    (void)clientData;
    return 1000000;  /* reschedule in 1 second */
}

TEST(AeEventLoopTest, SurvivesWhenAllTimeEventsDeleted) {
    int pipefds[2];
    ASSERT_EQ(pipe(pipefds), 0);

    aeEventLoop *el = aeCreateEventLoop(128);
    ASSERT_NE(el, nullptr);

    /* Register the always-readable pipe so aeApiPoll returns immediately. */
    ASSERT_EQ(aeCreateFileEvent(el, pipefds[0], AE_READABLE, pipeFileProc, NULL), AE_OK);

    /* Write a byte so the pipe is readable (aeApiPoll wakes on it). */
    char byte = 'x';
    ASSERT_EQ((int)write(pipefds[1], &byte, 1), 1);

    /* Create two time events that will self-delete on first fire. */
    int called1 = 0, called2 = 0;
    ASSERT_NE(aeCreateTimeEvent(el, 0, selfDeletingProc, &called1, NULL), -1);
    ASSERT_NE(aeCreateTimeEvent(el, 0, selfDeletingProc, &called2, NULL), -1);

    /* First pass: both events fire and self-delete. */
    aeProcessEvents(el, AE_ALL_EVENTS);
    EXPECT_EQ(called1, 1);
    EXPECT_EQ(called2, 1);

    /*
     * Second pass: with all time events deleted, the scan loop in
     * usUntilEarliestTimer() finds no valid events. Before the fix this
     * would dereference a NULL pointer and crash; after the fix it should
     * return -1 and the caller handles it. The pipe keeps the loop alive.
     */
    aeProcessEvents(el, AE_ALL_EVENTS);

    aeDeleteFileEvent(el, pipefds[0], AE_READABLE);
    aeDeleteEventLoop(el);
    close(pipefds[0]);
    close(pipefds[1]);

    SUCCEED() << "Survived aeProcessEvents with all time events deleted";
}

TEST(AeEventLoopTest, HandlesMixedDeletedAndActiveTimers) {
    int pipefds[2];
    ASSERT_EQ(pipe(pipefds), 0);

    aeEventLoop *el = aeCreateEventLoop(128);
    ASSERT_NE(el, nullptr);

    ASSERT_EQ(aeCreateFileEvent(el, pipefds[0], AE_READABLE, pipeFileProc, NULL), AE_OK);

    /* Write a byte so the pipe is readable (aeApiPoll wakes on it). */
    char byte = 'x';
    ASSERT_EQ((int)write(pipefds[1], &byte, 1), 1);

    int called1 = 0, called2 = 0;
    ASSERT_NE(aeCreateTimeEvent(el, 0, selfDeletingProc, &called1, NULL), -1);
    ASSERT_NE(aeCreateTimeEvent(el, 0, noopProc, &called2, NULL), -1);

    /* First pass: event1 self-deletes, event2 reschedules. */
    aeProcessEvents(el, AE_ALL_EVENTS);
    EXPECT_EQ(called1, 1);

    /* Second pass: only event2 is active, should not crash. */
    aeProcessEvents(el, AE_ALL_EVENTS);

    aeDeleteFileEvent(el, pipefds[0], AE_READABLE);
    aeDeleteEventLoop(el);
    close(pipefds[0]);
    close(pipefds[1]);

    SUCCEED() << "Survived with mixed deleted and active timers";
}