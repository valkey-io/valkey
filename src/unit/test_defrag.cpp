/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Component tests for active defrag's bounded-latency contract, verified deterministically.
 *
 * Active defrag is incremental: each cycle runs stage functions bounded by an `endtime`
 * (monotonic microseconds) and must YIELD once the time budget is exceeded, so it never blocks
 * the event loop for long. Verifying this by measuring the wall-clock latency of a defrag cycle is
 * non-deterministic: wall-clock latency depends on how much CPU time the process is scheduled,
 * which is outside the engine's control.
 *
 * These tests verify the *mechanism* instead of the emergent wall-clock timing: they drive the
 * real defrag stage/cycle code with `getMonotonicUs` swapped for a deterministic mock clock, and
 * assert that a stage/cycle yields exactly when its time budget is exceeded and completes when the
 * budget is ample -- independent of the machine's timing.
 */

#include "generated_wrappers.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "allocator_defrag.h"
#include "server.h"
}

#include "test_server_fixture.hpp"

/* Active defrag and its test seams in defrag.c are compiled only when the allocator supports defrag
 * (HAVE_DEFRAG). defrag.c derives HAVE_DEFRAG from the vendored jemalloc (or DEBUG_FORCE_DEFRAG),
 * but that macro isn't visible to this unit-test translation unit -- the unit build only exposes
 * USE_JEMALLOC (and drops it for the libc builds used by the sanitizer and macOS CI jobs). Guard on
 * that so the suite is compiled out exactly where the seam symbols don't exist -- mirroring how
 * memefficiency.tcl skips its defrag tests on non-jemalloc. */
#if defined(USE_JEMALLOC) || defined(DEBUG_FORCE_DEFRAG)

/* Defined in defrag.c (compiled only under HAVE_DEFRAG). Declared here rather than in a public
 * header because they exist only for these tests -- server.h should not carry test-facing surface.
 * (defragComputeDutyCycleUs is also used within defrag.c; the test is its only external caller.) */
extern "C" {
int defragTestRunKvstoreStage(monotime endtime, kvstore *kvs);
int defragRunCycleForTest(monotime endtime);
long defragComputeDutyCycleUs(int targetCpuPercent, long cycleUs, long waitedUs, long *overageUs);
}

/* ---- kvstore entry callbacks: entries are plain C strings (the entry IS the key) ---- */
static uint64_t defragTestHash(const void *key) {
    return hashtableGenHashFunction((const char *)key, strlen((const char *)key));
}
static int defragTestCmp(const void *k1, const void *k2) {
    return strcmp((const char *)k1, (const char *)k2) == 0;
}
static void defragTestFree(void *entry) {
    zfree(entry);
}
static char *strFromInt(int v) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", v);
    char *s = (char *)zmalloc(len + 1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

/* ---- deterministic mock clock: advances by a fixed step on each read ---- */
static monotime g_now;
static monotime g_step;
static monotime mockClock(void) {
    g_now += g_step;
    return g_now;
}

#define DEFRAG_TEST_ENTRIES 5000

class DefragStageTest : public ::testing::Test {
  protected:
    hashtableType type;
    kvstore *kvs;
    monotime (*saved_clock)(void);

    /* One-time allocator-defrag init is handled lazily by defragTestSupported(). */

    void SetUp() override {
        /* Active defrag requires an allocator with defrag support (jemalloc). Sanitizer and macOS
         * builds use libc malloc, where activeDefragAlloc() would hit allocatorShouldDefrag()'s
         * defrag_supported assertion -- skip there, mirroring memefficiency.tcl's jemalloc gate. */
        if (!defragTestSupported()) GTEST_SKIP() << "active defrag requires jemalloc";

        memset(&type, 0, sizeof(type));
        type.hashFunction = defragTestHash;
        type.keyCompare = defragTestCmp;
        type.entryDestructor = defragTestFree;
        /* kvstore requires its own rehashing/mem-tracking callbacks: it maintains an internal
         * list of rehashing hashtables. Omitting these corrupts that bookkeeping (crash). */
        type.rehashingStarted = kvstoreHashtableRehashingStarted;
        type.rehashingCompleted = kvstoreHashtableRehashingCompleted;
        type.trackMemUsage = kvstoreHashtableTrackMemUsage;
        type.getMetadataSize = kvstoreHashtableMetadataSize;

        kvs = kvstoreCreate(&type, 0, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND);
        for (int i = 0; i < DEFRAG_TEST_ENTRIES; i++) {
            kvstoreHashtableAdd(kvs, 0, strFromInt(i));
        }

        server.stat_active_defrag_hits = 0;
        server.stat_active_defrag_scanned = 0;

        saved_clock = getMonotonicUs;
        getMonotonicUs = mockClock;
        g_now = 0;
        g_step = 0;
    }

    void TearDown() override {
        if (!defragTestSupported()) return; /* SetUp skipped; nothing was allocated */
        getMonotonicUs = saved_clock;
        kvstoreRelease(kvs);
    }
};

/* With an effectively-infinite budget (clock frozen well below endtime), the stage scans every
 * entry and reports completion. */
TEST_F(DefragStageTest, CompletesWhenBudgetAmple) {
    g_now = 0;
    g_step = 0; /* clock never advances -> never exceeds endtime */

    (void)defragTestRunKvstoreStage(0, kvs);            /* initialize the stage */
    int done = defragTestRunKvstoreStage(1000000, kvs); /* huge endtime */

    EXPECT_EQ(done, 1); /* DEFRAG_DONE */
    /* Reverse-binary scan of a stable table visits each entry at least once. */
    EXPECT_GE(server.stat_active_defrag_scanned, (long long)DEFRAG_TEST_ENTRIES);
}

/* With a tight budget and an advancing clock, the stage must yield before finishing. This is the
 * incrementality contract that keeps per-cycle latency bounded. */
TEST_F(DefragStageTest, YieldsWhenBudgetExceeded) {
    g_now = 0;
    g_step = 10; /* each clock read advances 10us */

    (void)defragTestRunKvstoreStage(0, kvs);        /* initialize the stage (no clock read) */
    int done = defragTestRunKvstoreStage(100, kvs); /* tiny budget: exceeded after ~10 reads */

    EXPECT_EQ(done, 0);                                                           /* DEFRAG_NOT_DONE -> yielded */
    EXPECT_GT(server.stat_active_defrag_scanned, (long long)0);                   /* made progress */
    EXPECT_LT(server.stat_active_defrag_scanned, (long long)DEFRAG_TEST_ENTRIES); /* did not finish */
}

/*
 * Same contract, but over a REAL server.db populated via the reusable server-bootstrap fixture.
 * This exercises the actual database keys kvstore (kvstoreKeysHashtableType + real robjs), proving
 * the fixture stands up server state that defrag can traverse -- the foundation for the full
 * cycle-level tests below (DefragCycleTest).
 */
class DefragDbStageTest : public ::testing::Test {
  protected:
    monotime (*saved_clock)(void);

    void SetUp() override {
        if (!defragTestSupported()) GTEST_SKIP() << "active defrag requires jemalloc";
        testServerInitMinimal(4);
        testServerEmptyAllDbs();
        for (int i = 0; i < DEFRAG_TEST_ENTRIES; i++) {
            char kbuf[32];
            snprintf(kbuf, sizeof(kbuf), "key:%d", i);
            testServerAddStringKey(0, kbuf, "v");
        }
        server.stat_active_defrag_hits = 0;
        server.stat_active_defrag_scanned = 0;
        saved_clock = getMonotonicUs;
        getMonotonicUs = mockClock;
        g_now = 0;
        g_step = 0;
    }

    void TearDown() override {
        if (!defragTestSupported()) return; /* SetUp skipped */
        getMonotonicUs = saved_clock;
        testServerEmptyAllDbs();
    }
};

TEST_F(DefragDbStageTest, CompletesOverRealDb) {
    g_now = 0;
    g_step = 0;

    (void)defragTestRunKvstoreStage(0, server.db[0]->keys);
    int done = defragTestRunKvstoreStage(1000000, server.db[0]->keys);

    EXPECT_EQ(done, 1);
    EXPECT_GE(server.stat_active_defrag_scanned, (long long)DEFRAG_TEST_ENTRIES);
}

TEST_F(DefragDbStageTest, YieldsOverRealDb) {
    g_now = 0;
    g_step = 10;

    (void)defragTestRunKvstoreStage(0, server.db[0]->keys);
    int done = defragTestRunKvstoreStage(100, server.db[0]->keys);

    EXPECT_EQ(done, 0);
    EXPECT_GT(server.stat_active_defrag_scanned, (long long)0);
    EXPECT_LT(server.stat_active_defrag_scanned, (long long)DEFRAG_TEST_ENTRIES);
}

/*
 * Full-cycle contract: drive defragRunCycleForTest() -- the whole scheduler (all stages, outer
 * loop, teardown) -- with the mock clock. This is the faithful, sufficient bounded-latency test:
 * it covers the outer stage loop and every stage, not just one kvstore helper. Uses the same
 * server fixture as DefragDbStageTest.
 */
class DefragCycleTest : public DefragDbStageTest {};

/* With an ample budget (clock frozen below endtime), a full cycle runs to completion. */
TEST_F(DefragCycleTest, CompletesFullCycle) {
    g_now = 0;
    g_step = 0;

    int done = 0;
    int guard = 0;
    while (!done && guard++ < 1000) {
        done = defragRunCycleForTest(1000000);
    }
    EXPECT_EQ(done, 1);
    EXPECT_LT(guard, 1000); /* completed, not spinning */
}

/* With a tight budget and an advancing clock, the first cycle invocation yields (work remains);
 * draining it with an ample budget then completes. Exercises the yield-and-resume path across the
 * whole scheduler. */
TEST_F(DefragCycleTest, YieldsThenCompletes) {
    g_now = 0;
    g_step = 10; /* clock advances -> budget exceeded mid-cycle */

    int first = defragRunCycleForTest(200);
    EXPECT_EQ(first, 0); /* yielded with work remaining */

    /* Drain with an ample (frozen) budget. */
    g_now = 0;
    g_step = 0;
    int done = 0;
    int guard = 0;
    while (!done && guard++ < 1000) {
        done = defragRunCycleForTest(1000000);
    }
    EXPECT_EQ(done, 1);
}

/*
 * Pure-function test of the defrag duty-cycle budget math (defragComputeDutyCycleUs), the piece the
 * cycle/stage tests structurally can't cover. No server state or clock -- deterministic arithmetic.
 * dutyCycle = P*W/(100-P), minus carried overage, clamped up to cycleUs (shortfall -> overage).
 */
TEST(DefragDutyCycleTest, FormulaClampAndOverage) {
    long overage;

    /* P=50, W=1000 -> 50*1000/50 = 1000 >= cycle(500): use 1000, no overage. */
    overage = 0;
    EXPECT_EQ(defragComputeDutyCycleUs(50, 500, 1000, &overage), 1000L);
    EXPECT_EQ(overage, 0L);

    /* P=25, W=1000 -> 25000/75 = 333 < 500: clamp to 500, carry overage 500-333=167. */
    overage = 0;
    EXPECT_EQ(defragComputeDutyCycleUs(25, 500, 1000, &overage), 500L);
    EXPECT_EQ(overage, 167L);

    /* Carried overage is subtracted first: P=50, W=1000, overage=200 -> 1000-200=800 >= 500. */
    overage = 200;
    EXPECT_EQ(defragComputeDutyCycleUs(50, 500, 1000, &overage), 800L);
    EXPECT_EQ(overage, 0L);

    /* Short wait clamps to the minimum cycle time: P=50, W=100 -> 100 < 500 -> 500, overage 400. */
    overage = 0;
    EXPECT_EQ(defragComputeDutyCycleUs(50, 500, 100, &overage), 500L);
    EXPECT_EQ(overage, 400L);
}

#endif /* USE_JEMALLOC || DEBUG_FORCE_DEFRAG */
