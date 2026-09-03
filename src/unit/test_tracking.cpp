/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
/* TrackingTableTotalItems: non-static global in tracking.c, not declared in
 * server.h (it is internal to tracking; INFO reads it via
 * trackingGetTotalItems). The tests below build tracking state by hand and
 * must keep the counter consistent, so they need the symbol directly. */
extern uint64_t TrackingTableTotalItems;

/* Deliberately absent from server.h: the sweep-step primitive and the
 * tracking-table accessor are non-static only so unit tests can drive the
 * sweep one bounded step at a time against a hand-built table. Forward
 * declared here instead of widening the public tracking API. These tests are
 * white-box by design: exercising budget boundaries, mid-key resumption and
 * cursor invalidation requires precise table shapes (e.g. one key holding
 * 1500 dead IDs) that cannot be constructed or observed through the public
 * tracking API alone. */
int trackingSweepStep(monotime endtime, uint64_t *removed);
rax **unitTestOnly_getTrackingTable(void);
int unitTestOnly_trackingSweepReset(void);
}

/* Number of liveness checks a sweep step performs between clock reads,
 * reported by unitTestOnly_trackingSweepReset(). With an already-expired
 * deadline a step stops after exactly this many checks, which the boundary
 * tests below use to place IDs precisely on a stop point. */
static uint64_t sweep_checks_per_clock;

/* An endtime that is already in the past under the fake clock below: every
 * step stops at its first clock check. */
#define EXPIRED_DEADLINE ((monotime)1)

/* Fake monotonic clock installed for the whole suite. The scheduler test
 * advances it explicitly; the step tests rely on EXPIRED_DEADLINE always
 * being in the past. */
static monotime fakeMonotimeUs;
static monotime fakeGetMonotonicUs(void) {
    return fakeMonotimeUs;
}
static monotime (*origGetMonotonicUs)(void);

/* Insert a "live" client into the fake clients_index so that
 * lookupClientByID(id) returns a non-NULL pointer.  The value is a
 * zero-initialised client.  The key encoding matches
 * linkClient()/lookupClientByID(): the id in network byte order. */
static client *makeLiveClient(rax *clients_index, uint64_t id) {
    client *c = (client *)zcalloc(sizeof(client));
    c->id = id;
    uint64_t be = htonu64(id);
    raxInsert(clients_index, (unsigned char *)&be, sizeof(be), c, NULL);
    return c;
}

/* Register the client IDs in 'ids' as trackers of 'keyname', creating the
 * inner radix tree on first use.  Mirrors trackingRememberKeys: each newly
 * inserted id bumps TrackingTableTotalItems, so the global counter stays
 * consistent with the state we build. */
static void trackKeyIds(rax *tt, const char *keyname, const uint64_t *ids, int n) {
    void *found;
    rax *inner;
    size_t klen = strlen(keyname);
    if (!raxFind(tt, (unsigned char *)keyname, klen, &found)) {
        inner = raxNew();
        raxInsert(tt, (unsigned char *)keyname, klen, inner, NULL);
    } else {
        inner = (rax *)found;
    }
    for (int i = 0; i < n; i++) {
        if (raxTryInsert(inner, (unsigned char *)&ids[i], sizeof(ids[i]), NULL, NULL)) TrackingTableTotalItems++;
    }
}

/* Count every id across all inner radix trees and report whether all of them
 * still reference a live client.  Used to check counter consistency and the
 * live-only invariant after a sweep. */
static uint64_t countIdsAndCheckLive(rax *tt, int *all_live_out) {
    uint64_t total = 0;
    int all_live = 1;
    raxIterator ri;
    raxStart(&ri, tt);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
        rax *ids = (rax *)ri.data;
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

/* Remove 'keyname' from the tracking table the way the tail of
 * trackingInvalidateKey does (minus the invalidation send): account the
 * remaining IDs out of TrackingTableTotalItems, free the inner radix tree and
 * drop the key from the outer table.  Used to mutate the table between two
 * sweeper calls, simulating a key invalidated while the sweep cursor points
 * at (or into) it. */
static void removeTrackedKeyForTest(rax *tt, const char *keyname) {
    void *found;
    size_t klen = strlen(keyname);
    if (!raxFind(tt, (unsigned char *)keyname, klen, &found)) return;
    rax *inner = (rax *)found;
    TrackingTableTotalItems -= raxSize(inner);
    raxFree(inner);
    raxRemove(tt, (unsigned char *)keyname, klen, NULL);
}

/* Free the inner radix tree of every remaining key, then the outer table. */
static void freeTrackingTableForTest(rax *tt) {
    raxIterator ri;
    raxStart(&ri, tt);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
        rax *ids = (rax *)ri.data;
        raxFree(ids);
    }
    raxStop(&ri);
    raxFree(tt);
}

class TrackingTest : public ::testing::Test {
  protected:
    rax *saved_clients_index;
    client *fake_clients[512];
    int num_fake_clients;
    rax *test_clients_index;

    client *newLiveClient(uint64_t id) {
        client *c = makeLiveClient(test_clients_index, id);
        fake_clients[num_fake_clients++] = c;
        return c;
    }

    static void SetUpTestSuite() {
        origGetMonotonicUs = getMonotonicUs;
        getMonotonicUs = fakeGetMonotonicUs;
    }

    static void TearDownTestSuite() {
        getMonotonicUs = origGetMonotonicUs;
    }

    /* Sweep with an expired deadline until the pass completes, asserting the
     * incremental-progress invariants on every step: each step removes at
     * least one ID (the table here only ever loses dead IDs), the counter
     * stays consistent with the table, and live IDs are never touched.
     * Returns the number of steps taken. */
    int sweepToEndCheckingInvariants(rax *tt, uint64_t expected_live_total) {
        int steps = 0;
        uint64_t removed;
        while (!trackingSweepStep(EXPIRED_DEADLINE, &removed)) {
            steps++;
            EXPECT_GT(removed, (uint64_t)0);
            int all_live = 0;
            uint64_t actual = countIdsAndCheckLive(tt, &all_live);
            EXPECT_EQ(actual, TrackingTableTotalItems);
            EXPECT_GE(actual, expected_live_total);
            if (steps > 100000) break; /* never an open loop */
        }
        return steps;
    }

    void SetUp() override {
        fakeMonotimeUs = 1000000;
        sweep_checks_per_clock = (uint64_t)unitTestOnly_trackingSweepReset();
        num_fake_clients = 0;
        test_clients_index = raxNew();
        saved_clients_index = server.clients_index;
        server.clients_index = test_clients_index;

        /* Start every test from a fresh, empty tracking table and counter. */
        rax **tt = unitTestOnly_getTrackingTable();
        *tt = raxNew();
        TrackingTableTotalItems = 0;
    }

    void TearDown() override {
        rax **tt = unitTestOnly_getTrackingTable();
        if (*tt != NULL) {
            freeTrackingTableForTest(*tt);
            *tt = NULL;
        }
        TrackingTableTotalItems = 0;

        unitTestOnly_trackingSweepReset();

        for (int i = 0; i < num_fake_clients; i++) zfree(fake_clients[i]);
        num_fake_clients = 0;

        /* Restore the original clients_index and config, free the fake index. */
        server.clients_index = saved_clients_index;
        raxFree(test_clients_index);
        test_clients_index = NULL;
    }
};

/* Dead-ID removal: a table with a mix of live and dead IDs is swept so that
 * only live IDs remain, the counter matches the surviving IDs, and keys whose
 * inner radix tree became empty are removed. The dead ID in k1 sits between
 * two live ones, so the post-removal re-seek must land on the live successor
 * and preserve it. */
TEST_F(TrackingTest, SweeperRemovesDeadIdsAndEmptyKeys) {
    rax *tt = *unitTestOnly_getTrackingTable();
    uint64_t live1 = 1, dead2 = 2, live3 = 3;

    /* Clients 1 and 3 are connected; client 2 is gone (never registered). */
    newLiveClient(live1);
    newLiveClient(live3);

    uint64_t k1ids[3] = {live1, dead2, live3}; /* dead between two live */
    uint64_t k2ids[1] = {dead2};               /* only dead -> becomes empty */
    uint64_t k3ids[1] = {live3};               /* only live */
    trackKeyIds(tt, "k1", k1ids, 3);
    trackKeyIds(tt, "k2", k2ids, 1);
    trackKeyIds(tt, "k3", k3ids, 1);

    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)3);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)5);

    /* No deadline: a single step sweeps the whole table and reaches EOF. */
    EXPECT_EQ(trackingSweepStep(0, NULL), 1);

    /* k2 became empty and was removed; k1 and k3 remain. */
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)2);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)3);

    void *found;
    EXPECT_FALSE(raxFind(tt, (unsigned char *)"k2", 2, &found));

    /* k1 keeps both live IDs, including the one after the removed ID. */
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"k1", 2, &found));
    rax *k1inner = (rax *)found;
    EXPECT_EQ(raxSize(k1inner), (uint64_t)2);
    EXPECT_TRUE(raxFind(k1inner, (unsigned char *)&live1, sizeof(live1), &found));
    EXPECT_FALSE(raxFind(k1inner, (unsigned char *)&dead2, sizeof(dead2), &found));
    EXPECT_TRUE(raxFind(k1inner, (unsigned char *)&live3, sizeof(live3), &found));

    /* k3's live id is preserved. */
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"k3", 2, &found));
    rax *k3inner = (rax *)found;
    EXPECT_EQ(raxSize(k3inner), (uint64_t)1);
    EXPECT_TRUE(raxFind(k3inner, (unsigned char *)&live3, sizeof(live3), &found));

    /* The counter matches the actual number of surviving IDs, and every
     * surviving id references a live client. */
    int all_live = 0;
    uint64_t actual = countIdsAndCheckLive(tt, &all_live);
    EXPECT_EQ(actual, trackingGetTotalItems());
    EXPECT_EQ(all_live, 1);
}

/* trackingSweepFull (DEBUG SWEEP-TRACKING-TABLE) synchronously drains every
 * dead ID, and the step reports the number of removed IDs through its out
 * parameter (set, not accumulated: no pre-initialization needed). */
TEST_F(TrackingTest, SweepStepReportsRemovedAndFullSweepDrains) {
    rax *tt = *unitTestOnly_getTrackingTable();
    uint64_t dead1 = 1, dead2 = 2;
    uint64_t kids[2] = {dead1, dead2}; /* no clients registered: both dead */
    trackKeyIds(tt, "k1", kids, 2);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)2);

    /* One unbounded step drains the table; the step's removed counter
     * accounts for every reclaimed ID. */
    uint64_t removed;
    EXPECT_EQ(trackingSweepStep(0, &removed), 1);
    EXPECT_EQ(removed, (uint64_t)2);
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)0);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);

    /* A step over a clean table reports zero. */
    EXPECT_EQ(trackingSweepStep(0, &removed), 1);
    EXPECT_EQ(removed, (uint64_t)0);

    /* trackingSweepFull on an already-clean table is a no-op that returns. */
    trackingSweepFull();
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);
}

/* The scheduled entry point (serverCron path) self-gates on an adaptive
 * period: a call that reclaims something halves it, a call that finds nothing
 * doubles it, clamped to [100ms, 60s]. Driven under the fake clock. */
TEST_F(TrackingTest, SweepSchedulerAdaptsPeriodUnderFakeClock) {
    rax *tt = *unitTestOnly_getTrackingTable();
    uint64_t dead1 = 1;
    trackKeyIds(tt, "k1", &dead1, 1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);

    /* Freshly reset: the first call is due and the period starts at its 60s
     * maximum. Reclaiming halves it to 30s. */
    trackingSweepDeadClients();
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);

    /* Immediately after: inside the period, a new dead ID survives. */
    uint64_t dead2 = 2;
    trackKeyIds(tt, "k2", &dead2, 1);
    trackingSweepDeadClients();
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);

    /* Advance just under 30s: still gated. */
    fakeMonotimeUs += 29 * 1000 * 1000;
    trackingSweepDeadClients();
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);

    /* Cross 30s: due, reclaims, period halves again to 15s. */
    fakeMonotimeUs += 2 * 1000 * 1000;
    trackingSweepDeadClients();
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);

    /* 15s later with nothing to reclaim: runs (empty table is a valid
     * target now that it exists), finds nothing, period doubles to 30s. */
    fakeMonotimeUs += 16 * 1000 * 1000;
    uint64_t dead3 = 3;
    trackingSweepDeadClients();
    trackKeyIds(tt, "k3", &dead3, 1);
    /* 16s later: inside the doubled 30s period, so still gated. */
    fakeMonotimeUs += 16 * 1000 * 1000;
    trackingSweepDeadClients();
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);
    /* Another 15s: past 30s, due. */
    fakeMonotimeUs += 15 * 1000 * 1000;
    trackingSweepDeadClients();
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);
}

/* Incrementality across keys: with more IDs than one expired-deadline step
 * covers, the sweep takes several steps, each making progress, keeping the
 * counter consistent and never touching a live ID, and completes the pass. */
TEST_F(TrackingTest, SweeperIsIncrementalAndResumesAcrossCalls) {
    rax *tt = *unitTestOnly_getTrackingTable();
    const int total_keys = 300;
    const int ids_per_key = 4;
    const uint64_t canary = 1000000;
    newLiveClient(canary);

    for (int i = 0; i < total_keys; i++) {
        char kn[16];
        /* Zero-padded so lexicographic order equals numeric order. */
        snprintf(kn, sizeof(kn), "key%03d", i);
        uint64_t dead_ids[ids_per_key];
        for (int j = 0; j < ids_per_key; j++) dead_ids[j] = (uint64_t)(i * ids_per_key + j + 1); /* no client -> dead */
        trackKeyIds(tt, kn, dead_ids, ids_per_key);
    }
    trackKeyIds(tt, "key150", &canary, 1);

    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)total_keys);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)(total_keys * ids_per_key + 1));

    int steps = sweepToEndCheckingInvariants(tt, 1);
    EXPECT_GT(steps, 1);

    /* Every dead ID is gone, every emptied key is gone, the canary's key
     * survives with only the canary. */
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);
    int all_live = 0;
    EXPECT_EQ(countIdsAndCheckLive(tt, &all_live), (uint64_t)1);
    EXPECT_EQ(all_live, 1);
}

/* One key holding many more dead IDs than one step covers: the mid-key
 * cursor resumes inside the key across steps, the key survives while IDs
 * remain, and is reclaimed once drained. */
TEST_F(TrackingTest, SweeperBoundsWorkWithinOneHugeKey) {
    rax *tt = *unitTestOnly_getTrackingTable();
    const int total_ids = 2500;

    for (int i = 0; i < total_ids; i++) {
        uint64_t dead_id = (uint64_t)(i + 1); /* no client -> dead */
        trackKeyIds(tt, "hotkey", &dead_id, 1);
    }
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)total_ids);

    /* The first step stops inside the key: the key is still there, with
     * fewer IDs. */
    uint64_t removed;
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 0);
    EXPECT_GT(removed, (uint64_t)0);
    EXPECT_LT(removed, (uint64_t)total_ids);
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)(total_ids - removed));

    int steps = sweepToEndCheckingInvariants(tt, 0);
    EXPECT_GT(steps, 1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)0);
}

/* Live IDs scattered through a huge key are preserved across mid-key
 * resumptions, wherever the step boundaries fall. */
TEST_F(TrackingTest, SweeperPreservesLiveIdsInHugeKey) {
    rax *tt = *unitTestOnly_getTrackingTable();
    const int total_ids = 1200;
    const uint64_t live_ids[3] = {5, 500, (uint64_t)total_ids};

    for (int i = 0; i < total_ids; i++) {
        uint64_t id = (uint64_t)(i + 1);
        trackKeyIds(tt, "hotkey", &id, 1);
    }
    for (int i = 0; i < 3; i++) newLiveClient(live_ids[i]);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)total_ids);

    int steps = sweepToEndCheckingInvariants(tt, 3);
    EXPECT_GT(steps, 1);

    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)3);
    void *found;
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"hotkey", 6, &found));
    rax *inner = (rax *)found;
    for (int i = 0; i < 3; i++) {
        EXPECT_TRUE(raxFind(inner, (unsigned char *)&live_ids[i], sizeof(live_ids[i]), &found)) << "live id " << live_ids[i];
    }
    int all_live = 0;
    EXPECT_EQ(countIdsAndCheckLive(tt, &all_live), (uint64_t)3);
    EXPECT_EQ(all_live, 1);
}

/* Boundary: a step that stops exactly on a live ID must neither remove it
 * nor skip past the IDs after it; a step whose last removal is immediately
 * followed by the deadline check must resume cleanly. Live IDs are placed
 * precisely on the first two stop points. */
TEST_F(TrackingTest, SweeperStopsOnLiveIdAndResumesPastIt) {
    rax *tt = *unitTestOnly_getTrackingTable();
    const uint64_t s = sweep_checks_per_clock;
    const uint64_t total_ids = 2 * s + 8;
    void *found;

    for (uint64_t id = 1; id <= total_ids; id++) trackKeyIds(tt, "k", &id, 1);
    newLiveClient(s);
    newLiveClient(2 * s);
    EXPECT_EQ(trackingGetTotalItems(), total_ids);

    uint64_t removed;
    /* Step 1: checks 1..s; removes 1..s-1; stops on the live ID s. */
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 0);
    EXPECT_EQ(removed, s - 1);
    EXPECT_EQ(trackingGetTotalItems(), total_ids - (s - 1));
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"k", 1, &found));
    rax *inner = (rax *)found;
    uint64_t probe = s;
    EXPECT_TRUE(raxFind(inner, (unsigned char *)&probe, sizeof(probe), &found));
    probe = s + 1; /* not yet visited */
    EXPECT_TRUE(raxFind(inner, (unsigned char *)&probe, sizeof(probe), &found));

    /* Step 2: resumes after s; removes s+1..2s-1; stops on the live ID 2s. */
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 0);
    EXPECT_EQ(removed, s - 1);
    EXPECT_EQ(trackingGetTotalItems(), total_ids - 2 * (s - 1));
    probe = s;
    EXPECT_TRUE(raxFind(inner, (unsigned char *)&probe, sizeof(probe), &found));
    probe = 2 * s;
    EXPECT_TRUE(raxFind(inner, (unsigned char *)&probe, sizeof(probe), &found));
    probe = 2 * s + 1;
    EXPECT_TRUE(raxFind(inner, (unsigned char *)&probe, sizeof(probe), &found));

    /* Step 3: the remaining 8 dead IDs (fewer than a clock cadence) are
     * removed, the key stays with its two live IDs, the pass completes. */
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 1);
    EXPECT_EQ(removed, (uint64_t)8);
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)2);
    int all_live = 0;
    EXPECT_EQ(countIdsAndCheckLive(tt, &all_live), (uint64_t)2);
    EXPECT_EQ(all_live, 1);
}

/* Boundary: the deadline hits exactly as a key's last ID is removed. The
 * emptied key must be reclaimed right away (not left for a later step), the
 * outer iterator must re-seek past it, and the next step must start at the
 * successor key's first ID rather than resuming inside a key that no longer
 * exists. */
TEST_F(TrackingTest, SweeperReclaimsKeyEmptiedExactlyAtDeadline) {
    rax *tt = *unitTestOnly_getTrackingTable();
    const uint64_t s = sweep_checks_per_clock;
    const uint64_t live = 100;
    void *found;

    /* "kA" < "kB" < "kC". kA holds exactly one step's worth of dead IDs. */
    for (uint64_t id = 1; id <= s; id++) trackKeyIds(tt, "kA", &id, 1);
    uint64_t kb_ids[3] = {live, 101, 102};
    trackKeyIds(tt, "kB", kb_ids, 3);
    uint64_t kc_id = 200;
    trackKeyIds(tt, "kC", &kc_id, 1);
    newLiveClient(live);
    EXPECT_EQ(trackingGetTotalItems(), s + 4);

    uint64_t removed;
    /* Step 1: drains kA on the s-th check, which is also the deadline check.
     * kA is reclaimed in place; kB and kC are untouched. */
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 0);
    EXPECT_EQ(removed, s);
    EXPECT_FALSE(raxFind(tt, (unsigned char *)"kA", 2, &found));
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"kB", 2, &found));
    EXPECT_EQ(raxSize((rax *)found), (uint64_t)3);
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"kC", 2, &found));
    EXPECT_EQ(raxSize((rax *)found), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)4);

    /* Step 2: starts at kB's first ID (live, kept), removes 101 and 102,
     * drains and reclaims kC, and reaches EOF - the re-seek past the last
     * key sets EOF directly. */
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 1);
    EXPECT_EQ(removed, (uint64_t)3);
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"kB", 2, &found));
    rax *kb = (rax *)found;
    EXPECT_EQ(raxSize(kb), (uint64_t)1);
    EXPECT_TRUE(raxFind(kb, (unsigned char *)&live, sizeof(live), &found));
    EXPECT_FALSE(raxFind(tt, (unsigned char *)"kC", 2, &found));
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);

    /* Step 3: a fresh pass finds nothing to do. */
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 1);
    EXPECT_EQ(removed, (uint64_t)0);
    int all_live = 0;
    EXPECT_EQ(countIdsAndCheckLive(tt, &all_live), (uint64_t)1);
    EXPECT_EQ(all_live, 1);
}

/* Cursor tolerance to table mutation between calls, removal and re-creation
 * of the cursor key: the key the cursor stopped inside may be invalidated
 * (trackingInvalidateKey) before the next call. The ">=" seek must then land
 * on the successor key and the sweep must finish the pass normally - never a
 * crash, never a live-ID removal. If the same key name is re-created before
 * the next call holding an ID that sorts lexicographically before the saved
 * in-key cursor, that ID is missed by the resumed pass (expected) and
 * reclaimed by the next pass once the cursor wraps: the guarantee is eventual
 * reclamation, not single-pass completeness. */
TEST_F(TrackingTest, SweeperToleratesCursorKeyRemovalBetweenCalls) {
    rax *tt = *unitTestOnly_getTrackingTable();
    const int mid_ids = (int)(4 * sweep_checks_per_clock); /* several steps' worth */
    const uint64_t canary = 42;
    void *found;
    uint64_t removed;

    newLiveClient(canary);

    /* "kaaa" < "kmid" < "kzzz": the first step drains "kaaa" and stops inside
     * "kmid" (which holds more IDs than one step covers). */
    for (int i = 0; i < 3; i++) {
        uint64_t dead_id = (uint64_t)(100 + i); /* no client -> dead */
        trackKeyIds(tt, "kaaa", &dead_id, 1);
    }
    for (int i = 0; i < mid_ids; i++) {
        /* Every ID carries 0xFF in its lowest byte so that, in either byte
         * order, the ID 1 re-inserted below sorts lexicographically before
         * any of them - and thus before the saved in-key cursor. */
        uint64_t dead_id = ((uint64_t)(i + 1) << 8) | 0xFF;
        trackKeyIds(tt, "kmid", &dead_id, 1);
    }
    for (int i = 0; i < 5; i++) {
        uint64_t dead_id = (uint64_t)(9000 + i);
        trackKeyIds(tt, "kzzz", &dead_id, 1);
    }
    trackKeyIds(tt, "kzzz", &canary, 1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)(3 + mid_ids + 6));

    /* Call 1: stops inside "kmid". */
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 0);
    EXPECT_FALSE(raxFind(tt, (unsigned char *)"kaaa", 4, &found));
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"kmid", 4, &found));
    EXPECT_GT(raxSize((rax *)found), (uint64_t)0);

    /* Remove the cursor key between calls, as trackingInvalidateKey would. */
    removeTrackedKeyForTest(tt, "kmid");
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)6);

    /* The ">=" seek lands on the successor "kzzz"; the pass finishes, its
     * dead IDs are reclaimed and the live canary survives. */
    sweepToEndCheckingInvariants(tt, 1);
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);
    int all_live = 0;
    EXPECT_EQ(countIdsAndCheckLive(tt, &all_live), (uint64_t)1);
    EXPECT_EQ(all_live, 1);

    /* Re-creation variant: park the cursor inside "kmid" again... */
    for (int i = 0; i < 3; i++) {
        uint64_t dead_id = (uint64_t)(200 + i);
        trackKeyIds(tt, "kaaa", &dead_id, 1);
    }
    for (int i = 0; i < mid_ids; i++) {
        uint64_t dead_id = ((uint64_t)(i + 1) << 8) | 0xFF;
        trackKeyIds(tt, "kmid", &dead_id, 1);
    }
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 0);
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"kmid", 4, &found));

    /* ...remove it, then re-create the same key name with a single dead ID
     * that sorts before the saved cursor. */
    removeTrackedKeyForTest(tt, "kmid");
    uint64_t small_dead = 1;
    trackKeyIds(tt, "kmid", &small_dead, 1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)2);

    /* The resumed call matches the re-created key by name and seeks strictly
     * past the saved cursor, so ID 1 is missed by this pass - expected -
     * while the rest of the table is still processed to EOF. */
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 1);
    EXPECT_EQ(removed, (uint64_t)0);
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"kmid", 4, &found));
    EXPECT_TRUE(raxFind((rax *)found, (unsigned char *)&small_dead, sizeof(small_dead), &found));
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)2);

    /* The next pass restarts from the beginning and reclaims the missed ID. */
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 1);
    EXPECT_EQ(removed, (uint64_t)1);
    EXPECT_FALSE(raxFind(tt, (unsigned char *)"kmid", 4, &found));
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);
    all_live = 0;
    EXPECT_EQ(countIdsAndCheckLive(tt, &all_live), (uint64_t)1);
    EXPECT_EQ(all_live, 1);
}

/* Cursor tolerance to table mutation between calls, insertion behind the
 * cursor: a key added lexicographically before the in-flight cursor is not
 * seen by the current pass (expected) and is reclaimed on the next pass once
 * the cursor wraps at EOF - a delay bounded by one full pass, with no live-ID
 * removal. */
TEST_F(TrackingTest, SweeperReclaimsKeyInsertedBehindCursorOnNextPass) {
    rax *tt = *unitTestOnly_getTrackingTable();
    const uint64_t canary = 42;
    void *found;
    uint64_t removed;

    newLiveClient(canary);

    /* "kc" holds more IDs than one step covers, so the first step stops
     * inside it, leaving "kd" untouched. */
    const char *names[4] = {"ka", "kc", "kd", "kf"};
    const int counts[4] = {2, (int)(4 * sweep_checks_per_clock), 5, 5};
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < counts[k]; i++) {
            uint64_t dead_id = (uint64_t)((k + 1) * 10000 + i); /* no client -> dead */
            trackKeyIds(tt, names[k], &dead_id, 1);
        }
    }
    trackKeyIds(tt, "kf", &canary, 1);
    const uint64_t total = (uint64_t)(counts[0] + counts[1] + counts[2] + counts[3] + 1);
    EXPECT_EQ(trackingGetTotalItems(), total);

    /* Call 1: "ka" is drained and removed; the cursor sits inside "kc". */
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 0);
    EXPECT_FALSE(raxFind(tt, (unsigned char *)"ka", 2, &found));
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"kc", 2, &found));
    EXPECT_GT(raxSize((rax *)found), (uint64_t)0);
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"kd", 2, &found));
    EXPECT_EQ(raxSize((rax *)found), (uint64_t)counts[2]);

    /* Insert a new key behind the cursor: "ka" < "kb" < "kc". */
    uint64_t behind_dead = 7777;
    trackKeyIds(tt, "kb", &behind_dead, 1);

    /* Finish the pass: "kc" is drained and reclaimed, "kd" and "kf" are
     * swept and EOF resets the cursor. "kb" sits behind the cursor for the
     * whole pass and must be untouched. */
    sweepToEndCheckingInvariants(tt, 2);
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"kb", 2, &found));
    EXPECT_TRUE(raxFind((rax *)found, (unsigned char *)&behind_dead, sizeof(behind_dead), &found));
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)2); /* "kb" + "kf" (canary) */
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)2);

    /* The next pass starts from the beginning and reclaims "kb". */
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 1);
    EXPECT_EQ(removed, (uint64_t)1);
    EXPECT_FALSE(raxFind(tt, (unsigned char *)"kb", 2, &found));
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);
    int all_live = 0;
    EXPECT_EQ(countIdsAndCheckLive(tt, &all_live), (uint64_t)1);
    EXPECT_EQ(all_live, 1);
}

/* Liveness edge: when every id references a still-connected client, the
 * sweeper removes nothing and the counter is unchanged. */
TEST_F(TrackingTest, SweeperPreservesLiveIds) {
    rax *tt = *unitTestOnly_getTrackingTable();
    uint64_t ka_ids[3] = {1, 2, 3};
    uint64_t kb_ids[2] = {1, 2};

    newLiveClient(1);
    newLiveClient(2);
    newLiveClient(3);

    trackKeyIds(tt, "ka", ka_ids, 3);
    trackKeyIds(tt, "kb", kb_ids, 2);

    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)2);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)5);

    /* A full pass (5 checks, under one clock cadence) removes nothing. */
    uint64_t removed;
    EXPECT_EQ(trackingSweepStep(EXPIRED_DEADLINE, &removed), 1);
    EXPECT_EQ(removed, (uint64_t)0);

    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)2);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)5);

    int all_live = 0;
    uint64_t actual = countIdsAndCheckLive(tt, &all_live);
    EXPECT_EQ(actual, (uint64_t)5);
    EXPECT_EQ(all_live, 1);
}
