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
int defragTestIsRunning(void);
void defragTestBeginCycle(void);
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

/*
 * ---- Big-key (defrag_later) coverage ------------------------------------------------------------
 *
 * Keys whose field count exceeds `active-defrag-max-scan-fields` are not defragged inline during
 * the db-keys scan; they are deferred to the `defrag_later` queue and defragged incrementally
 * (defragLaterStep + per-type scanLater* resume machinery). Single huge keys are where bounded
 * latency is hardest to maintain -- the cycle must be able to yield *mid-key* and resume. The TCL
 * big hash/set/zset/list/stream scenarios exercised this path under the wall-clock latency
 * assertion; these tests pin the same contract deterministically: with a tight mock-clock budget
 * the full cycle repeatedly yields (never overrunning its deadline), makes progress across calls,
 * completes, and leaves the data intact.
 */

#define BIGKEY_FIELDS 10000
#define BIGKEY_DEFER_THRESHOLD 100 /* test override for active-defrag-max-scan-fields */

static robj *buildBigHash(void) {
    robj *o = createHashObject();
    sds val = sdsnew("hash-value-payload-aaaaaaaaaaaaa");
    for (int i = 0; i < BIGKEY_FIELDS; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "field:%d", i);
        sds f = sdsnew(buf);
        bool expired_overwritten = false;
        /* HASH_SET_COPY: the hash keeps its own copies; converts to hashtable encoding once
         * hash-max-listpack-entries (default 128) is exceeded. */
        hashTypeSet(o, f, val, EXPIRY_NONE, HASH_SET_COPY, &expired_overwritten);
        sdsfree(f);
    }
    sdsfree(val);
    return o;
}

static robj *buildBigSet(void) {
    robj *o = createSetObject(); /* hashtable-encoded */
    for (int i = 0; i < BIGKEY_FIELDS; i++) {
        char buf[48];
        snprintf(buf, sizeof(buf), "member-payload-aaaaaaaaaaaaa:%d", i);
        sds m = sdsnew(buf);
        setTypeAdd(o, m);
        sdsfree(m);
    }
    return o;
}

static robj *buildBigZset(void) {
    robj *o = createZsetObject(); /* skiplist-encoded */
    for (int i = 0; i < BIGKEY_FIELDS; i++) {
        char buf[48];
        snprintf(buf, sizeof(buf), "element-payload-aaaaaaaaaaaa:%d", i);
        sds e = sdsnew(buf);
        int out_flags = 0;
        double newscore = 0;
        zsetAdd(o, (double)i, e, ZADD_IN_NONE, &out_flags, &newscore);
        sdsfree(e);
    }
    return o;
}

/* Small fill so the quicklist has many nodes: scanLaterList yields (and bookmarks) between
 * nodes, so node count -- not element count -- is what drives its incrementality. */
#define BIGLIST_FILL 4

static robj *buildBigList(void) {
    robj *o = createQuicklistObject(BIGLIST_FILL, 0);
    for (int i = 0; i < BIGKEY_FIELDS; i++) {
        char buf[48];
        snprintf(buf, sizeof(buf), "list-item-payload-aaaaaaaaaa:%d", i);
        robj *v = createStringObject(buf, strlen(buf));
        listTypePush(o, v, LIST_TAIL);
        decrRefCount(v);
    }
    return o;
}

static robj *buildBigStream(void) {
    /* One rax entry per append, so raxSize() (the stream defer criterion and the unit
     * scanLaterStreamListpacks iterates by) exceeds the defer threshold. */
    long long saved_node_max_entries = server.stream_node_max_entries;
    server.stream_node_max_entries = 1;
    robj *o = createStreamObject();
    stream *s = (stream *)objectGetVal(o);
    robj *f = createStringObject("field", 5);
    for (int i = 0; i < BIGKEY_FIELDS; i++) {
        char buf[48];
        snprintf(buf, sizeof(buf), "stream-value-payload-aaaaaa:%d", i);
        robj *v = createStringObject(buf, strlen(buf));
        robj *argv[2] = {f, v};
        streamID added_id;
        streamAppendItem(s, argv, 1, &added_id, NULL, 0);
        decrRefCount(v);
    }
    decrRefCount(f);
    server.stream_node_max_entries = saved_node_max_entries;
    return o;
}

static unsigned long bigKeyLenHash(robj *o) {
    return hashTypeLength(o);
}
static unsigned long bigKeyLenSet(robj *o) {
    return setTypeSize(o);
}
static unsigned long bigKeyLenZset(robj *o) {
    return zsetLength(o);
}
static unsigned long bigKeyLenList(robj *o) {
    return listTypeLength(o);
}
static unsigned long bigKeyLenStream(robj *o) {
    return (unsigned long)((stream *)objectGetVal(o))->length;
}

struct DefragBigKeyParam {
    const char *name;
    robj *(*build)(void);
    int expected_encoding;
    unsigned long (*length)(robj *o);
    /* Minimum stat_active_defrag_scanned attributable to the deferred key's field/node scan.
     * 0 for hash and set: their scanLater callbacks defrag fields without bumping the counter,
     * so per-field progress isn't observable through it. */
    long long min_scanned;
};

class DefragBigKeyTestBase : public ::testing::Test {
  protected:
    monotime (*saved_clock)(void);
    unsigned long saved_max_scan_fields;

    void SetUp() override {
        if (!defragTestSupported()) GTEST_SKIP() << "active defrag requires jemalloc";
        testServerInitMinimal(4);
        testServerEmptyAllDbs();
        saved_max_scan_fields = server.active_defrag_max_scan_fields;
        server.active_defrag_max_scan_fields = BIGKEY_DEFER_THRESHOLD;
        server.stat_active_defrag_hits = 0;
        server.stat_active_defrag_scanned = 0;
        saved_clock = getMonotonicUs;
        getMonotonicUs = mockClock;
        g_now = 0;
        g_step = 0;
    }

    void TearDown() override {
        if (!defragTestSupported()) return; /* SetUp skipped; nothing was set up */
        getMonotonicUs = saved_clock;
        server.active_defrag_max_scan_fields = saved_max_scan_fields;
        testServerEmptyAllDbs();
    }
};

class DefragBigKeyTest : public DefragBigKeyTestBase, public ::testing::WithParamInterface<DefragBigKeyParam> {};

/* Per-call mock-clock budget (~10 clock reads at g_step=10) and the permitted overshoot. After the
 * deadline passes, every code path reaches a time check within a bounded number of reads, so the
 * clock may only run a few reads past the deadline -- THAT bound is the latency contract. Time
 * checks happen at bounded work intervals (16 kvstore buckets / 128 stream entries / each list
 * node), so per-call work is bounded even though the intervals themselves don't read the clock. */
#define BIGKEY_CALL_BUDGET 100
#define BIGKEY_OVERSHOOT_SLACK 200

TEST_P(DefragBigKeyTest, DeferredKeyYieldsIncrementallyAndCompletes) {
    const DefragBigKeyParam &p = GetParam();

    robj *ob = p.build();
    ASSERT_EQ((int)ob->encoding, p.expected_encoding);
    unsigned long len_before = p.length(ob);
    ASSERT_GT(len_before, (unsigned long)BIGKEY_DEFER_THRESHOLD); /* must take the defrag_later path */
    testServerAddObjectKey(0, "bigkey", ob);
    /* NOTE: `ob` may be relocated by defrag below; re-find it from the db afterwards. */

    g_step = 10;
    int done = 0;
    int calls = 0;
    while (!done && calls < 10000) {
        monotime deadline = g_now + BIGKEY_CALL_BUDGET;
        done = defragRunCycleForTest(deadline);
        calls++;
        ASSERT_LE(g_now, deadline + BIGKEY_OVERSHOOT_SLACK) << "defrag cycle overran its time budget";
    }
    ASSERT_EQ(done, 1) << "cycle did not complete within the call guard";
    /* The deferred key must force multiple yield/resume rounds -- an empty-db cycle finishes in
     * ~2 calls at this budget; the big key's incremental scan dominates. */
    EXPECT_GE(calls, 4);
    if (p.min_scanned > 0) {
        EXPECT_GE(server.stat_active_defrag_scanned, p.min_scanned);
    }

    /* Data integrity across the incremental defrag. */
    sds key = sdsnew("bigkey");
    robj *found = dbFind(server.db[0], key);
    sdsfree(key);
    ASSERT_TRUE(found != NULL);
    EXPECT_EQ((int)found->encoding, p.expected_encoding);
    EXPECT_EQ(p.length(found), len_before);
}

INSTANTIATE_TEST_SUITE_P(
    Types,
    DefragBigKeyTest,
    ::testing::Values(DefragBigKeyParam{"hash", buildBigHash, OBJ_ENCODING_HASHTABLE, bigKeyLenHash, 0},
                      DefragBigKeyParam{"set", buildBigSet, OBJ_ENCODING_HASHTABLE, bigKeyLenSet, 0},
                      DefragBigKeyParam{"zset", buildBigZset, OBJ_ENCODING_SKIPLIST, bigKeyLenZset, BIGKEY_FIELDS},
                      DefragBigKeyParam{"list", buildBigList, OBJ_ENCODING_QUICKLIST, bigKeyLenList,
                                        BIGKEY_FIELDS / BIGLIST_FILL},
                      DefragBigKeyParam{"stream", buildBigStream, OBJ_ENCODING_STREAM, bigKeyLenStream,
                                        BIGKEY_FIELDS}),
    [](const ::testing::TestParamInfo<DefragBigKeyParam> &info) { return std::string(info.param.name); });

/*
 * scanLaterList suspend/resume is uniquely stateful: a mid-list yield records position via a
 * quicklist bookmark ("_AD") and completion deletes it. Verify the bookmark's lifecycle across
 * yields -- present during at least one yielded state, absent after completion.
 */
class DefragListBookmarkTest : public DefragBigKeyTestBase {};

TEST_F(DefragListBookmarkTest, BookmarkLifecycleAcrossYields) {
    robj *ob = buildBigList();
    ASSERT_EQ((int)ob->encoding, OBJ_ENCODING_QUICKLIST);
    unsigned long len_before = listTypeLength(ob);
    testServerAddObjectKey(0, "biglist", ob);
    sds key = sdsnew("biglist");

    g_step = 10;
    bool saw_bookmark = false;
    int done = 0;
    int calls = 0;
    while (!done && calls < 10000) {
        done = defragRunCycleForTest(g_now + BIGKEY_CALL_BUDGET);
        calls++;
        robj *cur = dbFind(server.db[0], key);
        ASSERT_TRUE(cur != NULL);
        quicklist *ql = (quicklist *)objectGetVal(cur);
        if (!done && quicklistBookmarkFind(ql, "_AD") != NULL) saw_bookmark = true;
    }
    ASSERT_EQ(done, 1);
    EXPECT_TRUE(saw_bookmark) << "expected a mid-list yield to leave the _AD resume bookmark";

    robj *cur = dbFind(server.db[0], key);
    ASSERT_TRUE(cur != NULL);
    quicklist *ql = (quicklist *)objectGetVal(cur);
    EXPECT_TRUE(quicklistBookmarkFind(ql, "_AD") == NULL) << "resume bookmark must be removed on completion";
    EXPECT_EQ(listTypeLength(cur), len_before);
    sdsfree(key);
}

/*
 * ---- defragWhileBlocked (AOF loading / long script) driver --------------------------------------
 *
 * During loading and long scripts, timers are inactive and defrag is driven by whileBlockedCron()
 * -> defragWhileBlocked(), which simulates a single timer-proc call: it computes its own duty-cycle
 * budget and must stay within it. This is the path the TCL "Active defrag - AOF loading" scenario's
 * validate_latency(500) covered. Verify with the mock clock: each defragWhileBlocked() call
 * consumes at most the duty cycle (+ bounded overshoot), and repeated calls finish the cycle.
 *
 * The cycle is started via defragTestBeginCycle (production beginDefragCycle, including the
 * event-loop timer registration that defragIsRunning() keys on) rather than letting
 * defragWhileBlocked go through monitorActiveDefrag(), whose start/stop decision depends on live
 * allocator fragmentation stats and would be nondeterministic.
 */
class DefragBlockedCronTest : public DefragBigKeyTestBase {};

TEST_F(DefragBlockedCronTest, BlockedDriverRespectsDutyCycleAndCompletes) {
    robj *ob = buildBigHash();
    unsigned long len_before = hashTypeLength(ob);
    testServerAddObjectKey(0, "bigkey", ob);

    /* activeDefragTimeProc preconditions: enabled, a legal CPU percent, and no child process
     * (a child pauses defrag indefinitely). */
    server.active_defrag_enabled = 1;
    server.active_defrag_cpu_percent = 50;
    server.child_pid = -1;
    int saved_cycle_us = server.active_defrag_cycle_us;
    server.active_defrag_cycle_us = 500; /* the per-call duty cycle floor */

    g_step = 10;

    defragTestBeginCycle();
    ASSERT_TRUE(defragTestIsRunning());

    int calls = 0;
    while (defragTestIsRunning() && calls < 10000) {
        monotime before = g_now;
        defragWhileBlocked();
        calls++;
        /* Duty cycle is clamped to at least active_defrag_cycle_us; allow bounded overshoot for
         * the driver's own bookkeeping clock reads. */
        ASSERT_LE(g_now - before, (monotime)(2 * 500)) << "blocked-cron defrag call overran its duty cycle";
    }
    server.active_defrag_cycle_us = saved_cycle_us;
    ASSERT_LT(calls, 10000) << "blocked-cron driver did not finish the cycle";
    EXPECT_GE(calls, 2); /* the deferred big hash must span multiple blocked-cron calls */

    sds key = sdsnew("bigkey");
    robj *found = dbFind(server.db[0], key);
    sdsfree(key);
    ASSERT_TRUE(found != NULL);
    EXPECT_EQ(hashTypeLength(found), len_before);
}

#endif /* USE_JEMALLOC || DEBUG_FORCE_DEFRAG */
