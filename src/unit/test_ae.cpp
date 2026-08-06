/*
 * Test: ae event loop survives when all time events are deleted.
 *
 * This test verifies that aeProcessEvents() does not crash when
 * all time events have been lazy-deleted (id == AE_DELETED_EVENT_ID).
 * The bug was a NULL dereference in usUntilEarliestTimer() when the
 * scan loop found no valid time events.
 */

#include "gtest/gtest.h"

extern "C" {
#include "../ae.h"
#include "../anet.h"
#include <unistd.h>
}

/* Simple time-event callback that self-deletes. */
static long long selfDeletingProc(aeEventLoop *el, long long id, void *clientData) {
    (void)el;
    (void)id;
    int *called = (int *)clientData;
    *called = 1;
    return AE_NOMORE;
}

static long long noopProc(aeEventLoop *el, long long id, void *clientData) {
    (void)el;
    (void)id;
    (void)clientData;
    return 1000000;
}

TEST(AeEventLoopTest, SurvivesWhenAllTimeEventsDeleted) {
    aeEventLoop *el = aeCreateEventLoop(128);
    ASSERT_NE(el, nullptr);

    /* Create two time events that will self-delete on first fire. */
    int called1 = 0, called2 = 0;
    ASSERT_NE(aeCreateTimeEvent(el, 0, selfDeletingProc, &called1, NULL), -1);
    ASSERT_NE(aeCreateTimeEvent(el, 0, selfDeletingProc, &called2, NULL), -1);

    /*
     * Process time events once. Both events should fire and self-delete,
     * setting their id to AE_DELETED_EVENT_ID. The deletion is lazy --
     * the nodes remain in the linked list until the next cleanup pass.
     */
    aeProcessEvents(el, AE_TIME_EVENTS);

    EXPECT_EQ(called1, 1);
    EXPECT_EQ(called2, 1);

    /*
     * Process time events again. With all events deleted, the scan
     * loop in usUntilEarliestTimer() should find no valid events.
     * Before the fix, this would dereference a NULL pointer and crash.
     * After the fix, it should return -1 and the caller should handle it.
     */
    aeProcessEvents(el, AE_TIME_EVENTS);

    /* If we reach here without crashing, the fix works. */
    SUCCEED() << "Survived aeProcessEvents with all time events deleted";

    aeDeleteEventLoop(el);
}

TEST(AeEventLoopTest, HandlesMixedDeletedAndActiveTimers) {
    aeEventLoop *el = aeCreateEventLoop(128);
    ASSERT_NE(el, nullptr);

    int called1 = 0, called2 = 0;
    ASSERT_NE(aeCreateTimeEvent(el, 0, selfDeletingProc, &called1, NULL), -1);
    ASSERT_NE(aeCreateTimeEvent(el, 0, noopProc, &called2, NULL), -1);

    /* First fire: event1 self-deletes, event2 reschedules. */
    aeProcessEvents(el, AE_TIME_EVENTS);

    EXPECT_EQ(called1, 1);

    /* Second fire: only event2 is active, should not crash. */
    aeProcessEvents(el, AE_TIME_EVENTS);

    SUCCEED() << "Survived with mixed deleted and active timers";

    aeDeleteEventLoop(el);
}