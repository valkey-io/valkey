/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "../server.h"
#include "test_help.h"

static int trackingTestFailedExpects;
#define TRACKING_TEST_EXPECT(condition)                                                                                \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            TEST_PRINT_ERROR("Failed expectation: " #condition);                                                       \
            trackingTestFailedExpects++;                                                                               \
        }                                                                                                              \
    } while (0)

/* -----------------------------------------------------------------------------
 * Client-side caching: reclaiming dead client IDs from the tracking table.
 *
 * The sweep decides liveness with lookupClientByID(), i.e. server.clients_index,
 * so these tests install a fake client index and a hand-built tracking table.
 * They are white-box by design: budget boundaries, mid-key resumption and
 * cursor invalidation need precise table shapes that cannot be built or
 * observed through the public tracking API.
 * -------------------------------------------------------------------------- */

/* Internal to tracking.c and deliberately absent from server.h. */
extern uint64_t TrackingTableTotalItems;
int trackingSweepStep(monotime endtime, uint64_t *removed);
rax **unitTestOnly_getTrackingTable(void);
int unitTestOnly_trackingSweepChecksPerClock(void);

/* An endtime that is already in the past under the fake clock below: every
 * step stops at its first clock check. */
#define TRACKING_TEST_EXPIRED_DEADLINE ((monotime)1)

static monotime trackingTestFakeMonotimeUs;
static monotime trackingTestFakeGetMonotonicUs(void) {
    return trackingTestFakeMonotimeUs;
}

/* Make lookupClientByID(id) return non-NULL. The key encoding matches
 * linkClient(): the id in network byte order. */
static void trackingTestAddLiveClient(uint64_t id) {
    client *c = zcalloc(sizeof(client));
    c->id = id;
    uint64_t be = htonu64(id);
    raxInsert(server.clients_index, (unsigned char *)&be, sizeof(be), c, NULL);
}

/* Register 'ids' as trackers of 'keyname' the way trackingRememberKeys does,
 * keeping TrackingTableTotalItems consistent with the table. */
static void trackingTestTrackKeyIds(rax *tt, const char *keyname, const uint64_t *ids, int n) {
    void *found;
    rax *inner;
    size_t klen = strlen(keyname);
    if (!raxFind(tt, (unsigned char *)keyname, klen, &found)) {
        inner = raxNew();
        raxInsert(tt, (unsigned char *)keyname, klen, inner, NULL);
    } else {
        inner = found;
    }
    for (int i = 0; i < n; i++) {
        uint64_t id = ids[i];
        if (raxTryInsert(inner, (unsigned char *)&id, sizeof(id), NULL, NULL)) TrackingTableTotalItems++;
    }
}

static int trackingTestKeyHasId(rax *inner, uint64_t id) {
    void *found;
    return raxFind(inner, (unsigned char *)&id, sizeof(id), &found);
}

/* Count every ID in the table and report whether all of them reference a live client. */
static uint64_t trackingTestCountIdsAndCheckLive(rax *tt, int *all_live_out) {
    uint64_t total = 0;
    int all_live = 1;
    raxIterator ri;
    raxStart(&ri, tt);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
        rax *ids = ri.data;
        total += raxSize(ids);
        raxIterator idi;
        raxStart(&idi, ids);
        raxSeek(&idi, "^", NULL, 0);
        while (raxNext(&idi)) {
            uint64_t id;
            memcpy(&id, idi.key, sizeof(id));
            if (lookupClientByID(id) == NULL) all_live = 0;
        }
        raxStop(&idi);
    }
    raxStop(&ri);
    *all_live_out = all_live;
    return total;
}

/* Drop 'keyname' the way the tail of trackingInvalidateKey does (minus the
 * invalidation send), simulating a key invalidated between two sweep steps. */
static void trackingTestRemoveKey(rax *tt, const char *keyname) {
    void *found;
    size_t klen = strlen(keyname);
    if (!raxFind(tt, (unsigned char *)keyname, klen, &found)) return;
    rax *inner = found;
    TrackingTableTotalItems -= raxSize(inner);
    raxFree(inner);
    raxRemove(tt, (unsigned char *)keyname, klen, NULL);
}

/* Sweep with an expired deadline until the pass completes, checking on every
 * step that it made progress and kept the counter consistent with the table.
 * Returns the number of steps that stopped on the deadline. */
static int trackingTestSweepToEnd(rax *tt, uint64_t expected_live_total) {
    int steps = 0;
    uint64_t removed;
    while (!trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed)) {
        steps++;
        TRACKING_TEST_EXPECT(removed > 0);
        int all_live = 0;
        uint64_t actual = trackingTestCountIdsAndCheckLive(tt, &all_live);
        TRACKING_TEST_EXPECT(actual == TrackingTableTotalItems);
        TRACKING_TEST_EXPECT(actual >= expected_live_total);
        if (steps > 100000) break; /* never an open loop */
    }
    return steps;
}

/* Run 'body' against an empty tracking table, an empty fake client index and a
 * fake monotonic clock, then restore everything so nothing leaks between tests
 * (including when the body bails out on a failed assertion). */
static int runTrackingSweepTest(int (*body)(rax *tt)) {
    monotime (*orig_get_monotonic_us)(void) = getMonotonicUs;
    rax *saved_clients_index = server.clients_index;
    rax **tt = unitTestOnly_getTrackingTable();

    getMonotonicUs = trackingTestFakeGetMonotonicUs;
    trackingTestFakeMonotimeUs = 1000000;
    server.clients_index = raxNew();
    *tt = raxNew();
    TrackingTableTotalItems = 0;

    trackingTestFailedExpects = 0;
    int result = body(*tt);

    raxIterator ri;
    raxStart(&ri, *tt);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) raxFree(ri.data);
    raxStop(&ri);
    raxFree(*tt);
    *tt = NULL;
    TrackingTableTotalItems = 0;

    /* With no tracking table the sweep entry points forget their static state
     * (resume cursor, scheduler period). */
    trackingSweepStep(0, NULL);
    trackingSweepDeadClients();

    raxFreeWithCallback(server.clients_index, zfree);
    server.clients_index = saved_clients_index;
    getMonotonicUs = orig_get_monotonic_us;
    return result || trackingTestFailedExpects;
}

/* A mix of live and dead IDs is swept so that only live IDs remain, the counter
 * matches, and keys whose inner radix tree became empty are removed. The dead
 * ID in k1 sits between two live ones, so the post-removal re-seek must land on
 * the live successor and preserve it. */
static int trackingSweeperRemovesDeadIdsAndEmptyKeys(rax *tt) {
    uint64_t live1 = 1, dead2 = 2, live3 = 3;

    /* Clients 1 and 3 are connected; client 2 is gone (never registered). */
    trackingTestAddLiveClient(live1);
    trackingTestAddLiveClient(live3);

    uint64_t k1ids[3] = {live1, dead2, live3}; /* dead between two live */
    uint64_t k2ids[1] = {dead2};               /* only dead -> becomes empty */
    uint64_t k3ids[1] = {live3};               /* only live */
    trackingTestTrackKeyIds(tt, "k1", k1ids, 3);
    trackingTestTrackKeyIds(tt, "k2", k2ids, 1);
    trackingTestTrackKeyIds(tt, "k3", k3ids, 1);

    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 3);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 5);

    /* No deadline: a single step sweeps the whole table and reaches EOF. */
    TRACKING_TEST_EXPECT(trackingSweepStep(0, NULL) == 1);

    /* k2 became empty and was removed; k1 and k3 remain. */
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 2);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 3);

    void *found;
    TRACKING_TEST_EXPECT(!raxFind(tt, (unsigned char *)"k2", 2, &found));

    /* k1 keeps both live IDs, including the one after the removed ID. */
    TEST_ASSERT(raxFind(tt, (unsigned char *)"k1", 2, &found));
    rax *k1inner = found;
    TRACKING_TEST_EXPECT(raxSize(k1inner) == 2);
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(k1inner, live1));
    TRACKING_TEST_EXPECT(!trackingTestKeyHasId(k1inner, dead2));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(k1inner, live3));

    /* k3's live id is preserved. */
    TEST_ASSERT(raxFind(tt, (unsigned char *)"k3", 2, &found));
    rax *k3inner = found;
    TRACKING_TEST_EXPECT(raxSize(k3inner) == 1);
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(k3inner, live3));

    /* The counter matches the surviving IDs, all of which are live. */
    int all_live = 0;
    uint64_t actual = trackingTestCountIdsAndCheckLive(tt, &all_live);
    TRACKING_TEST_EXPECT(actual == trackingGetTotalItems());
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperRemovesDeadIdsAndEmptyKeys(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperRemovesDeadIdsAndEmptyKeys);
}

/* The step reports the number of removed IDs through its out parameter (set,
 * not accumulated), and trackingSweepFull (DEBUG SWEEP-TRACKING-TABLE) is a
 * no-op on a clean table. */
static int trackingSweepStepReportsRemovedAndFullSweepDrains(rax *tt) {
    uint64_t kids[2] = {1, 2}; /* no clients registered: both dead */
    trackingTestTrackKeyIds(tt, "k1", kids, 2);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 2);

    /* One unbounded step drains the table and accounts for every reclaimed ID. */
    uint64_t removed;
    TRACKING_TEST_EXPECT(trackingSweepStep(0, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 2);
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 0);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);

    /* A step over a clean table reports zero. */
    TRACKING_TEST_EXPECT(trackingSweepStep(0, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 0);

    trackingSweepFull();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);
    return 0;
}

int test_trackingSweepStepReportsRemovedAndFullSweepDrains(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweepStepReportsRemovedAndFullSweepDrains);
}

/* The scheduled entry point (serverCron path) self-gates on an adaptive period:
 * a call that reclaims something halves it, a call that finds nothing doubles
 * it, clamped to [100ms, 60s]. Driven under the fake clock. */
static int trackingSweepSchedulerAdaptsPeriodUnderFakeClock(rax *tt) {
    uint64_t dead1 = 1, dead2 = 2, dead3 = 3;
    trackingTestTrackKeyIds(tt, "k1", &dead1, 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Freshly reset: the first call is due and the period starts at its 60s
     * maximum. Reclaiming halves it to 30s. */
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);

    /* Immediately after: inside the period, a new dead ID survives. */
    trackingTestTrackKeyIds(tt, "k2", &dead2, 1);
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Advance just under 30s: still gated. */
    trackingTestFakeMonotimeUs += 29 * 1000 * 1000;
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Cross 30s: due, reclaims, period halves again to 15s. */
    trackingTestFakeMonotimeUs += 2 * 1000 * 1000;
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);

    /* 15s later with nothing to reclaim: runs, finds nothing, period doubles to 30s. */
    trackingTestFakeMonotimeUs += 16 * 1000 * 1000;
    trackingSweepDeadClients();
    trackingTestTrackKeyIds(tt, "k3", &dead3, 1);
    /* 16s later: inside the doubled 30s period, so still gated. */
    trackingTestFakeMonotimeUs += 16 * 1000 * 1000;
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);
    /* Another 15s: past 30s, due. */
    trackingTestFakeMonotimeUs += 15 * 1000 * 1000;
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);
    return 0;
}

int test_trackingSweepSchedulerAdaptsPeriodUnderFakeClock(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweepSchedulerAdaptsPeriodUnderFakeClock);
}

/* Incrementality across keys: with more IDs than one expired-deadline step
 * covers, the sweep takes several steps, each making progress and never
 * touching a live ID, and completes the pass. */
static int trackingSweeperIsIncrementalAndResumesAcrossCalls(rax *tt) {
    const int total_keys = 300;
    const int ids_per_key = 4;
    uint64_t canary = 1000000;
    trackingTestAddLiveClient(canary);

    for (int i = 0; i < total_keys; i++) {
        char kn[16];
        /* Zero-padded so lexicographic order equals numeric order. */
        snprintf(kn, sizeof(kn), "key%03d", i);
        uint64_t dead_ids[4];
        for (int j = 0; j < ids_per_key; j++) dead_ids[j] = (uint64_t)(i * ids_per_key + j + 1); /* no client -> dead */
        trackingTestTrackKeyIds(tt, kn, dead_ids, ids_per_key);
    }
    trackingTestTrackKeyIds(tt, "key150", &canary, 1);

    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == (uint64_t)total_keys);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == (uint64_t)(total_keys * ids_per_key + 1));

    int steps = trackingTestSweepToEnd(tt, 1);
    TRACKING_TEST_EXPECT(steps > 1);

    /* Every dead ID and emptied key is gone; the canary's key survives with only the canary. */
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperIsIncrementalAndResumesAcrossCalls(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperIsIncrementalAndResumesAcrossCalls);
}

/* One key holding many more dead IDs than one step covers: the mid-key cursor
 * resumes inside the key across steps, the key survives while IDs remain, and
 * is reclaimed once drained. */
static int trackingSweeperBoundsWorkWithinOneHugeKey(rax *tt) {
    const uint64_t total_ids = 2500;

    for (uint64_t id = 1; id <= total_ids; id++) trackingTestTrackKeyIds(tt, "hotkey", &id, 1); /* all dead */
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids);

    /* The first step stops inside the key: the key is still there, with fewer IDs. */
    uint64_t removed;
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(removed > 0);
    TRACKING_TEST_EXPECT(removed < total_ids);
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids - removed);

    int steps = trackingTestSweepToEnd(tt, 0);
    TRACKING_TEST_EXPECT(steps > 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 0);
    return 0;
}

int test_trackingSweeperBoundsWorkWithinOneHugeKey(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperBoundsWorkWithinOneHugeKey);
}

/* Live IDs scattered through a huge key are preserved across mid-key
 * resumptions, wherever the step boundaries fall. */
static int trackingSweeperPreservesLiveIdsInHugeKey(rax *tt) {
    const uint64_t total_ids = 1200;
    const uint64_t live_ids[3] = {5, 500, 1200};

    for (uint64_t id = 1; id <= total_ids; id++) trackingTestTrackKeyIds(tt, "hotkey", &id, 1);
    for (int i = 0; i < 3; i++) trackingTestAddLiveClient(live_ids[i]);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids);

    int steps = trackingTestSweepToEnd(tt, 3);
    TRACKING_TEST_EXPECT(steps > 1);

    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 3);
    void *found;
    TEST_ASSERT(raxFind(tt, (unsigned char *)"hotkey", 6, &found));
    rax *inner = found;
    for (int i = 0; i < 3; i++) TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, live_ids[i]));
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 3);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperPreservesLiveIdsInHugeKey(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperPreservesLiveIdsInHugeKey);
}

/* Boundary: a step that stops exactly on a live ID must neither remove it nor
 * skip past the IDs after it. Live IDs are placed precisely on the first two
 * stop points. */
static int trackingSweeperStopsOnLiveIdAndResumesPastIt(rax *tt) {
    const uint64_t s = (uint64_t)unitTestOnly_trackingSweepChecksPerClock();
    const uint64_t total_ids = 2 * s + 8;
    void *found;

    for (uint64_t id = 1; id <= total_ids; id++) trackingTestTrackKeyIds(tt, "k", &id, 1);
    trackingTestAddLiveClient(s);
    trackingTestAddLiveClient(2 * s);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids);

    uint64_t removed;
    /* Step 1: checks 1..s; removes 1..s-1; stops on the live ID s. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(removed == s - 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids - (s - 1));
    TEST_ASSERT(raxFind(tt, (unsigned char *)"k", 1, &found));
    rax *inner = found;
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, s));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, s + 1)); /* not yet visited */

    /* Step 2: resumes after s; removes s+1..2s-1; stops on the live ID 2s. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(removed == s - 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids - 2 * (s - 1));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, s));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, 2 * s));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, 2 * s + 1));

    /* Step 3: the remaining 8 dead IDs (fewer than a clock cadence) are
     * removed, the key stays with its two live IDs, the pass completes. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 8);
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 2);
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 2);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperStopsOnLiveIdAndResumesPastIt(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperStopsOnLiveIdAndResumesPastIt);
}

/* Boundary: the deadline hits exactly as a key's last ID is removed. The
 * emptied key must be reclaimed right away, and the next step must start at
 * the successor key's first ID rather than resuming inside a key that no
 * longer exists. */
static int trackingSweeperReclaimsKeyEmptiedExactlyAtDeadline(rax *tt) {
    const uint64_t s = (uint64_t)unitTestOnly_trackingSweepChecksPerClock();
    uint64_t live = 100;
    void *found;

    /* "kA" < "kB" < "kC". kA holds exactly one step's worth of dead IDs. */
    for (uint64_t id = 1; id <= s; id++) trackingTestTrackKeyIds(tt, "kA", &id, 1);
    uint64_t kb_ids[3] = {live, 101, 102};
    trackingTestTrackKeyIds(tt, "kB", kb_ids, 3);
    uint64_t kc_id = 200;
    trackingTestTrackKeyIds(tt, "kC", &kc_id, 1);
    trackingTestAddLiveClient(live);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == s + 4);

    uint64_t removed;
    /* Step 1: drains kA on the s-th check, which is also the deadline check.
     * kA is reclaimed in place; kB and kC are untouched. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(removed == s);
    TRACKING_TEST_EXPECT(!raxFind(tt, (unsigned char *)"kA", 2, &found));
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kB", 2, &found));
    TRACKING_TEST_EXPECT(raxSize(found) == 3);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kC", 2, &found));
    TRACKING_TEST_EXPECT(raxSize(found) == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 4);

    /* Step 2: starts at kB's first ID (live, kept), removes 101 and 102,
     * drains and reclaims kC, and reaches EOF. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 3);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kB", 2, &found));
    rax *kb = found;
    TRACKING_TEST_EXPECT(raxSize(kb) == 1);
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(kb, live));
    TRACKING_TEST_EXPECT(!raxFind(tt, (unsigned char *)"kC", 2, &found));
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Step 3: a fresh pass finds nothing to do. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 0);
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperReclaimsKeyEmptiedExactlyAtDeadline(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperReclaimsKeyEmptiedExactlyAtDeadline);
}

/* The key the cursor stopped inside may be invalidated before the next call:
 * the ">=" seek must then land on the successor key and finish the pass. If
 * the same key name is re-created holding an ID that sorts before the saved
 * in-key cursor, that ID is missed by the resumed pass and reclaimed by the
 * next one: the guarantee is eventual reclamation, not single-pass completeness. */
static int trackingSweeperToleratesCursorKeyRemovalBetweenCalls(rax *tt) {
    const int mid_ids = 4 * unitTestOnly_trackingSweepChecksPerClock(); /* several steps' worth */
    uint64_t canary = 42;
    void *found;
    uint64_t removed;

    trackingTestAddLiveClient(canary);

    /* "kaaa" < "kmid" < "kzzz": the first step drains "kaaa" and stops inside "kmid". */
    for (int i = 0; i < 3; i++) {
        uint64_t dead_id = (uint64_t)(100 + i); /* no client -> dead */
        trackingTestTrackKeyIds(tt, "kaaa", &dead_id, 1);
    }
    for (int i = 0; i < mid_ids; i++) {
        /* Every ID carries 0xFF in its lowest byte so that, in either byte
         * order, the ID 1 re-inserted below sorts before any of them - and
         * thus before the saved in-key cursor. */
        uint64_t dead_id = ((uint64_t)(i + 1) << 8) | 0xFF;
        trackingTestTrackKeyIds(tt, "kmid", &dead_id, 1);
    }
    for (int i = 0; i < 5; i++) {
        uint64_t dead_id = (uint64_t)(9000 + i);
        trackingTestTrackKeyIds(tt, "kzzz", &dead_id, 1);
    }
    trackingTestTrackKeyIds(tt, "kzzz", &canary, 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == (uint64_t)(3 + mid_ids + 6));

    /* Call 1: stops inside "kmid". */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(!raxFind(tt, (unsigned char *)"kaaa", 4, &found));
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kmid", 4, &found));
    TRACKING_TEST_EXPECT(raxSize(found) > 0);

    /* Remove the cursor key between calls, as trackingInvalidateKey would. */
    trackingTestRemoveKey(tt, "kmid");
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 6);

    /* The ">=" seek lands on the successor "kzzz"; the pass finishes, its dead
     * IDs are reclaimed and the live canary survives. */
    trackingTestSweepToEnd(tt, 1);
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TRACKING_TEST_EXPECT(all_live == 1);

    /* Re-creation variant: park the cursor inside "kmid" again... */
    for (int i = 0; i < 3; i++) {
        uint64_t dead_id = (uint64_t)(200 + i);
        trackingTestTrackKeyIds(tt, "kaaa", &dead_id, 1);
    }
    for (int i = 0; i < mid_ids; i++) {
        uint64_t dead_id = ((uint64_t)(i + 1) << 8) | 0xFF;
        trackingTestTrackKeyIds(tt, "kmid", &dead_id, 1);
    }
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kmid", 4, &found));

    /* ...remove it, then re-create the same key name with a single dead ID
     * that sorts before the saved cursor. */
    trackingTestRemoveKey(tt, "kmid");
    uint64_t small_dead = 1;
    trackingTestTrackKeyIds(tt, "kmid", &small_dead, 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 2);

    /* The resumed call matches the re-created key by name and seeks strictly
     * past the saved cursor, so ID 1 is missed by this pass while the rest of
     * the table is still processed to EOF. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 0);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kmid", 4, &found));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(found, small_dead));
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 2);

    /* The next pass restarts from the beginning and reclaims the missed ID. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 1);
    TRACKING_TEST_EXPECT(!raxFind(tt, (unsigned char *)"kmid", 4, &found));
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);
    all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperToleratesCursorKeyRemovalBetweenCalls(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperToleratesCursorKeyRemovalBetweenCalls);
}

/* A key added lexicographically before the in-flight cursor is not seen by the
 * current pass and is reclaimed on the next pass once the cursor wraps at EOF. */
static int trackingSweeperReclaimsKeyInsertedBehindCursorOnNextPass(rax *tt) {
    uint64_t canary = 42;
    void *found;
    uint64_t removed;

    trackingTestAddLiveClient(canary);

    /* "kc" holds more IDs than one step covers, so the first step stops
     * inside it, leaving "kd" untouched. */
    const char *names[4] = {"ka", "kc", "kd", "kf"};
    const int counts[4] = {2, 4 * unitTestOnly_trackingSweepChecksPerClock(), 5, 5};
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < counts[k]; i++) {
            uint64_t dead_id = (uint64_t)((k + 1) * 10000 + i); /* no client -> dead */
            trackingTestTrackKeyIds(tt, names[k], &dead_id, 1);
        }
    }
    trackingTestTrackKeyIds(tt, "kf", &canary, 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == (uint64_t)(counts[0] + counts[1] + counts[2] + counts[3] + 1));

    /* Call 1: "ka" is drained and removed; the cursor sits inside "kc". */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(!raxFind(tt, (unsigned char *)"ka", 2, &found));
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kc", 2, &found));
    TRACKING_TEST_EXPECT(raxSize(found) > 0);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kd", 2, &found));
    TRACKING_TEST_EXPECT(raxSize(found) == (uint64_t)counts[2]);

    /* Insert a new key behind the cursor: "ka" < "kb" < "kc". */
    uint64_t behind_dead = 7777;
    trackingTestTrackKeyIds(tt, "kb", &behind_dead, 1);

    /* Finish the pass: "kc" is drained and reclaimed, "kd" and "kf" are swept
     * and EOF resets the cursor. "kb" sits behind the cursor for the whole
     * pass and must be untouched. */
    trackingTestSweepToEnd(tt, 2);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kb", 2, &found));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(found, behind_dead));
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 2); /* "kb" + "kf" (canary) */
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 2);

    /* The next pass starts from the beginning and reclaims "kb". */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 1);
    TRACKING_TEST_EXPECT(!raxFind(tt, (unsigned char *)"kb", 2, &found));
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperReclaimsKeyInsertedBehindCursorOnNextPass(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperReclaimsKeyInsertedBehindCursorOnNextPass);
}

/* When every ID references a still-connected client, the sweeper removes
 * nothing and the counter is unchanged. */
static int trackingSweeperPreservesLiveIds(rax *tt) {
    uint64_t ka_ids[3] = {1, 2, 3};
    uint64_t kb_ids[2] = {1, 2};

    trackingTestAddLiveClient(1);
    trackingTestAddLiveClient(2);
    trackingTestAddLiveClient(3);

    trackingTestTrackKeyIds(tt, "ka", ka_ids, 3);
    trackingTestTrackKeyIds(tt, "kb", kb_ids, 2);

    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 2);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 5);

    /* A full pass (5 checks, under one clock cadence) removes nothing. */
    uint64_t removed;
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 0);

    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 2);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 5);

    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 5);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperPreservesLiveIds(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperPreservesLiveIds);
}
