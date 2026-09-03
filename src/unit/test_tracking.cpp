/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "gtest/gtest.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

/* wrappers.h shims the C-only keywords used by server.h (_Atomic, protected,
 * ...) so it can be included from C++, and pulls in server.h itself. */
#include "wrappers.h"

extern "C" {
/* TrackingTableTotalItems: non-static global in tracking.c, not declared in
 * server.h (it is internal to tracking; INFO reads it via
 * trackingGetTotalItems). The tests below build tracking state by hand and
 * must keep the counter consistent, so they need the symbol directly. */
extern uint64_t TrackingTableTotalItems;
}

/* Number of client-ID liveness checks one sweep step performs when invoked
 * with no time limit (endtime 0), mirroring TRACKING_SWEEP_MAX_ITEMS_PER_STEP
 * in tracking.c; kept here so the incrementality tests can reason about
 * per-step progress without exporting the macro. */
#define SWEEP_ITEMS_PER_STEP 1000

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

    void SetUp() override {
        /* getMonotonicUs is a function pointer initialized by monotonicInit();
         * the scheduler wrapper reads the clock, so initialize it here (same
         * pattern as the fifo and hashtable test suites). */
        monotonicInit();
        num_fake_clients = 0;
        test_clients_index = raxNew();
        saved_clients_index = server.clients_index;
        server.clients_index = test_clients_index;

        /* Start every test from a fresh, empty tracking table and counter. */
        rax **tt = getTrackingTable();
        *tt = raxNew();
        TrackingTableTotalItems = 0;
    }

    void TearDown() override {
        rax **tt = getTrackingTable();
        if (*tt != NULL) {
            freeTrackingTableForTest(*tt);
            *tt = NULL;
        }
        TrackingTableTotalItems = 0;

        /* With TrackingTable == NULL the sweeper frees and clears its static
         * resume cursor, so no state leaks into the next test. */
        trackingSweepStep(0, NULL);

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
 * inner radix tree became empty are removed. */
TEST_F(TrackingTest, SweeperRemovesDeadIdsAndEmptyKeys) {
    rax *tt = *getTrackingTable();
    uint64_t live1 = 1, dead2 = 2, live3 = 3;

    /* Clients 1 and 3 are connected; client 2 is gone (never registered). */
    newLiveClient(live1);
    newLiveClient(live3);

    uint64_t k1ids[2] = {live1, dead2}; /* one live, one dead */
    uint64_t k2ids[1] = {dead2};        /* only dead -> becomes empty */
    uint64_t k3ids[1] = {live3};        /* only live */
    trackKeyIds(tt, "k1", k1ids, 2);
    trackKeyIds(tt, "k2", k2ids, 1);
    trackKeyIds(tt, "k3", k3ids, 1);

    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)3);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)4);

    /* Fewer than SWEEP_KEYS_PER_CALL keys, so a single call sweeps the whole
     * table and reaches EOF. */
    trackingSweepStep(0, NULL);

    /* k2 became empty and was removed; k1 and k3 remain. */
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)2);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)2);

    void *found;
    EXPECT_FALSE(raxFind(tt, (unsigned char *)"k2", 2, &found));

    /* k1 keeps only the live id. */
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"k1", 2, &found));
    rax *k1inner = (rax *)found;
    EXPECT_EQ(raxSize(k1inner), (uint64_t)1);
    EXPECT_TRUE(raxFind(k1inner, (unsigned char *)&live1, sizeof(live1), &found));
    EXPECT_FALSE(raxFind(k1inner, (unsigned char *)&dead2, sizeof(dead2), &found));

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
 * dead ID regardless of the step budget, and the step reports the number of
 * removed IDs through its out parameter. */
TEST_F(TrackingTest, SweepStepReportsRemovedAndFullSweepDrains) {
    rax *tt = *getTrackingTable();
    uint64_t dead1 = 1, dead2 = 2;
    uint64_t kids[2] = {dead1, dead2}; /* no clients registered: both dead */
    trackKeyIds(tt, "k1", kids, 2);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)2);

    /* One unbounded step drains the table; the step's removed counter
     * accounts for every reclaimed ID. */
    uint64_t removed = 0;
    EXPECT_EQ(trackingSweepStep(0, &removed), 1);
    EXPECT_EQ(removed, (uint64_t)2);
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)0);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);

    /* trackingSweepFull on an already-clean table is a no-op that returns. */
    trackingSweepFull();
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);
}

/* The scheduled entry point (serverCron path) both runs and self-gates: its
 * first invocation is always due and a table this small is fully swept within
 * the step's time budget; an immediately following invocation is inside the
 * adaptive period (>= 100ms) and must not sweep, so dead IDs staged between
 * the two calls survive. No sleeps: the first-call-due and second-call-gated
 * outcomes are both timing-independent. */
TEST_F(TrackingTest, SweepSchedulerRunsWhenDueAndGatesInsidePeriod) {
    rax *tt = *getTrackingTable();
    uint64_t dead1 = 1;
    trackKeyIds(tt, "k1", &dead1, 1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);

    /* First scheduled run: due (initial next-run time is zero), reclaims. */
    trackingSweepDeadClients();
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);

    /* Immediately after, the scheduler is inside its adaptive period: a new
     * dead ID must survive the gated call. */
    uint64_t dead2 = 2;
    trackKeyIds(tt, "k2", &dead2, 1);
    trackingSweepDeadClients();
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);

    /* Drain explicitly so the fixture's teardown checks see a clean table. */
    trackingSweepFull();
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);
}

/* Incrementality across keys: with more total IDs than one call's budget,
 * each invocation checks exactly SWEEP_ITEMS_PER_STEP IDs, the cursor resumes
 * across calls, and a full sweep completes after ceil(items/budget)
 * invocations. Because every id is dead, the remaining item count is a
 * direct, observable measure of the per-call progress. */
TEST_F(TrackingTest, SweeperIsIncrementalAndResumesAcrossCalls) {
    rax *tt = *getTrackingTable();
    const int total_keys = 300;
    const int ids_per_key = 4;
    const int total_items = total_keys * ids_per_key; /* 1200 > one budget */

    for (int i = 0; i < total_keys; i++) {
        char kn[16];
        /* Zero-padded so lexicographic order equals numeric order. */
        snprintf(kn, sizeof(kn), "key%03d", i);
        uint64_t dead_ids[ids_per_key];
        for (int j = 0; j < ids_per_key; j++) dead_ids[j] = (uint64_t)(i * ids_per_key + j + 1); /* no client -> dead */
        trackKeyIds(tt, kn, dead_ids, ids_per_key);
    }

    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)total_keys);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)total_items);

    /* Call 1: checks (and here, removes) exactly SWEEP_ITEMS_PER_STEP IDs.
     * The budget boundary falls on the last id of key249, so that key is left
     * empty with a mid-key cursor and reclaimed by the next call. */
    trackingSweepStep(0, NULL);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)(total_items - SWEEP_ITEMS_PER_STEP));
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)(total_keys - SWEEP_ITEMS_PER_STEP / ids_per_key + 1));

    /* Call 2: resumes from the cursor, sweeps the remaining 200 IDs, and
     * reaches EOF (full pass complete, all emptied keys removed). */
    trackingSweepStep(0, NULL);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)0);
}

/* One key holding many more dead IDs than one call's budget: per-call
 * progress stays bounded by SWEEP_ITEMS_PER_STEP via the mid-key cursor, and
 * the key survives until fully swept. */
TEST_F(TrackingTest, SweeperBoundsWorkWithinOneHugeKey) {
    rax *tt = *getTrackingTable();
    const int total_ids = 2 * SWEEP_ITEMS_PER_STEP + 500; /* 2500: 3 calls */

    for (int i = 0; i < total_ids; i++) {
        uint64_t dead_id = (uint64_t)(i + 1); /* no client -> dead */
        trackKeyIds(tt, "hotkey", &dead_id, 1);
    }

    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)total_ids);

    /* Calls 1 and 2: each checks exactly one budget's worth of IDs, resuming
     * inside the key; the key itself is preserved while IDs remain. */
    trackingSweepStep(0, NULL);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)(total_ids - SWEEP_ITEMS_PER_STEP));
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);

    trackingSweepStep(0, NULL);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)(total_ids - 2 * SWEEP_ITEMS_PER_STEP));
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);

    /* Call 3: sweeps the final 500 IDs; the emptied key is removed. */
    trackingSweepStep(0, NULL);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)0);
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)0);
}

/* Live IDs inside a huge key are preserved across mid-key resumptions. */
TEST_F(TrackingTest, SweeperPreservesLiveIdsInHugeKey) {
    rax *tt = *getTrackingTable();
    const int total_ids = SWEEP_ITEMS_PER_STEP + 200; /* spans 2 calls */
    const uint64_t live_ids[3] = {5, 500, (uint64_t)total_ids};

    for (int i = 0; i < total_ids; i++) {
        uint64_t id = (uint64_t)(i + 1);
        trackKeyIds(tt, "hotkey", &id, 1);
    }
    for (int i = 0; i < 3; i++) newLiveClient(live_ids[i]);

    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)total_ids);

    /* Two calls cover all IDs regardless of where the budget boundary falls
     * in the (endianness-dependent) inner iteration order. */
    trackingSweepStep(0, NULL);
    trackingSweepStep(0, NULL);

    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)3);

    int all_live = 0;
    uint64_t actual = countIdsAndCheckLive(tt, &all_live);
    EXPECT_EQ(actual, (uint64_t)3);
    EXPECT_EQ(all_live, 1);
}

/* Cursor tolerance to table mutation between calls, removal and re-creation
 * of the cursor key: the key the cursor stopped inside may be invalidated
 * (trackingInvalidateKey) before the next call.  The ">=" seek must then land
 * on the successor key and the sweep must finish the pass normally - never a
 * crash, never a live-ID removal.  If the same key name is re-created before
 * the next call holding an ID that sorts lexicographically before the saved
 * in-key cursor, that ID is missed by the resumed pass (expected) and
 * reclaimed by the next pass once the cursor wraps: the guarantee is eventual
 * reclamation, not single-pass completeness. */
TEST_F(TrackingTest, SweeperToleratesCursorKeyRemovalBetweenCalls) {
    rax *tt = *getTrackingTable();
    const int mid_ids = SWEEP_ITEMS_PER_STEP + 50;
    const uint64_t canary = 42;
    void *found;

    newLiveClient(canary);

    /* "kaaa" < "kmid" < "kzzz": the first call drains "kaaa" (10 checks),
     * then exhausts its budget inside "kmid" (990 checks), leaving 60 of its
     * IDs unchecked and the mid-key cursor set. */
    for (int i = 0; i < 10; i++) {
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
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)(10 + mid_ids + 6));

    /* Call 1: stops inside "kmid". */
    trackingSweepStep(0, NULL);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)(10 + mid_ids + 6 - SWEEP_ITEMS_PER_STEP));

    /* Remove the cursor key between calls, as trackingInvalidateKey would. */
    removeTrackedKeyForTest(tt, "kmid");
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)6);

    /* Call 2: the ">=" seek lands on the successor "kzzz"; the pass finishes,
     * its dead IDs are reclaimed and the live canary survives. */
    trackingSweepStep(0, NULL);
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);
    int all_live = 0;
    EXPECT_EQ(countIdsAndCheckLive(tt, &all_live), (uint64_t)1);
    EXPECT_EQ(all_live, 1);

    /* Re-creation variant: park the cursor inside "kmid" again... */
    for (int i = 0; i < 10; i++) {
        uint64_t dead_id = (uint64_t)(200 + i);
        trackKeyIds(tt, "kaaa", &dead_id, 1);
    }
    for (int i = 0; i < mid_ids; i++) {
        uint64_t dead_id = ((uint64_t)(i + 1) << 8) | 0xFF;
        trackKeyIds(tt, "kmid", &dead_id, 1);
    }
    trackingSweepStep(0, NULL);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)(1 + 60));

    /* ...remove it, then re-create the same key name with a single dead ID
     * that sorts before the saved cursor. */
    removeTrackedKeyForTest(tt, "kmid");
    uint64_t small_dead = 1;
    trackKeyIds(tt, "kmid", &small_dead, 1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)2);

    /* The resumed call matches the re-created key by name and seeks strictly
     * past the saved cursor, so ID 1 is missed by this pass - expected -
     * while the rest of the table is still processed to EOF. */
    trackingSweepStep(0, NULL);
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"kmid", 4, &found));
    EXPECT_TRUE(raxFind((rax *)found, (unsigned char *)&small_dead, sizeof(small_dead), &found));
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)2);

    /* The next pass restarts from the beginning and reclaims the missed ID:
     * bounded number of calls, never an open loop. */
    int calls = 0;
    while (trackingGetTotalItems() > 1 && calls < 10) {
        trackingSweepStep(0, NULL);
        calls++;
    }
    EXPECT_LT(calls, 10);
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
    rax *tt = *getTrackingTable();
    const uint64_t canary = 42;
    void *found;

    newLiveClient(canary);

    /* "ka"(100) + "kb"(100) + "kc"(800) consume exactly one budget, so the
     * first call stops at "kc"'s last ID with the mid-key cursor set, leaving
     * "kd", "ke" and "kf" untouched. */
    const char *names[6] = {"ka", "kb", "kc", "kd", "ke", "kf"};
    const int counts[6] = {100, 100, 800, 50, 50, 50};
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < counts[k]; i++) {
            uint64_t dead_id = (uint64_t)((k + 1) * 10000 + i); /* no client -> dead */
            trackKeyIds(tt, names[k], &dead_id, 1);
        }
    }
    trackKeyIds(tt, "kf", &canary, 1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1151);

    /* Call 1: "ka" and "kb" are drained and removed; "kc" is drained but kept
     * (budget boundary), so the cursor sits inside "kc". */
    trackingSweepStep(0, NULL);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)151);
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)4);

    /* Insert a new key behind the cursor: "kb" < "kb2" < "kc". */
    uint64_t behind_dead = 7777;
    trackKeyIds(tt, "kb2", &behind_dead, 1);

    /* Call 2 completes the pass: the emptied resume key "kc" is reclaimed,
     * "kd".."kf" are swept and EOF resets the cursor. "kb2" sits behind the
     * cursor for the whole call and must be untouched. */
    trackingSweepStep(0, NULL);
    ASSERT_TRUE(raxFind(tt, (unsigned char *)"kb2", 3, &found));
    EXPECT_TRUE(raxFind((rax *)found, (unsigned char *)&behind_dead, sizeof(behind_dead), &found));
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)2); /* "kb2" + "kf" (canary) */
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)2);

    /* The next pass starts from the beginning and reclaims "kb2": bounded
     * number of calls, never an open loop. */
    int calls = 0;
    while (trackingGetTotalItems() > 1 && calls < 10) {
        trackingSweepStep(0, NULL);
        calls++;
    }
    EXPECT_LT(calls, 10);
    EXPECT_FALSE(raxFind(tt, (unsigned char *)"kb2", 3, &found));
    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)1);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)1);
    int all_live = 0;
    EXPECT_EQ(countIdsAndCheckLive(tt, &all_live), (uint64_t)1);
    EXPECT_EQ(all_live, 1);
}


/* Liveness edge: when every id references a still-connected client, the
 * sweeper removes nothing and the counter is unchanged. */
TEST_F(TrackingTest, SweeperPreservesLiveIds) {
    rax *tt = *getTrackingTable();
    uint64_t ka_ids[3] = {1, 2, 3};
    uint64_t kb_ids[2] = {1, 2};

    newLiveClient(1);
    newLiveClient(2);
    newLiveClient(3);

    trackKeyIds(tt, "ka", ka_ids, 3);
    trackKeyIds(tt, "kb", kb_ids, 2);

    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)2);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)5);

    /* Two sweep calls (more than enough for a full pass over 2 keys). */
    trackingSweepStep(0, NULL);
    trackingSweepStep(0, NULL);

    EXPECT_EQ(trackingGetTotalKeys(), (uint64_t)2);
    EXPECT_EQ(trackingGetTotalItems(), (uint64_t)5);

    int all_live = 0;
    uint64_t actual = countIdsAndCheckLive(tt, &all_live);
    EXPECT_EQ(actual, (uint64_t)5);
    EXPECT_EQ(all_live, 1);
}
