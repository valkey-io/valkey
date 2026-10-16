/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Unit tests for the Space-Saving frozen-window top-K used by hot-key
 * detection. These cover the algorithmic properties that integration tests
 * cannot pin down deterministically: the [count - error, count] band across
 * evictions, the "frequency > N/K is tracked" guarantee, window freezing
 * (including a double freeze after an idle gap), top-K selection when the
 * capacity shrinks, and predicate-based removal across both windows.
 *
 * The clock is supplied by the caller, so time is fully deterministic here: no
 * sleeping, no wall-clock dependency.
 */

#include "generated_wrappers.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "sds.h"
#include "space_saving.h"
}

#define WINDOW_US 1000ULL /* 1ms windows keep the arithmetic obvious */

/* Record one observation of `name` in database `dbid`. The key is borrowed by
 * the module (copied only if a slot is committed to it), so we free our copy. */
static void recordName(spaceSavingManager *m, const char *name, int dbid) {
    sds k = sdsnew(name);
    recordSpaceSavingManagerSample(m, k, dbid);
    sdsfree(k);
}

/* Look up a key in the frozen window. Returns 1 and fills count/error when
 * found, 0 otherwise. Out-params may be NULL. */
static int frozenFind(spaceSavingManager *m, const char *name, int dbid, uint64_t *count, uint64_t *error) {
    int n = spaceSavingManagerCount(m);
    for (int i = 0; i < n; i++) {
        sds key = NULL;
        int db = 0;
        uint64_t c = 0, e = 0;
        spaceSavingManagerAt(m, i, &key, &db, &c, &e);
        if (db == dbid && key != NULL && strcmp(key, name) == 0) {
            if (count) *count = c;
            if (error) *error = e;
            return 1;
        }
    }
    return 0;
}

/* Like frozenFind, also returning the secondary accumulator. */
static int frozenFind2(spaceSavingManager *m, const char *name, int dbid, uint64_t *count, uint64_t *error, uint64_t *count2) {
    int n = spaceSavingManagerCount(m);
    for (int i = 0; i < n; i++) {
        sds key = NULL;
        int db = 0;
        uint64_t c = 0, e = 0, c2 = 0;
        spaceSavingManagerAt2(m, i, &key, &db, &c, &e, &c2);
        if (db == dbid && key != NULL && strcmp(key, name) == 0) {
            if (count) *count = c;
            if (error) *error = e;
            if (count2) *count2 = c2;
            return 1;
        }
    }
    return 0;
}

/* Freeze the live window by advancing exactly one window length. Returns the new
 * "now". */
static uint64_t freezeOnce(spaceSavingManager *m, uint64_t now_us) {
    now_us += WINDOW_US;
    spaceSavingManagerRotate(m, now_us);
    return now_us;
}

/* ---------------------------------------------------------------------------
 * 1. The [count - error, count] band always contains the true count, including
 *    for entries that landed in a slot by evicting another.
 * --------------------------------------------------------------------------*/
TEST(SpaceSaving, ErrorBandContainsTrueCountAcrossEvictions) {
    const int k = 3;
    /* Six distinct keys into three slots forces repeated eviction. */
    const int nkeys = 6;
    const char *names[nkeys] = {"k0", "k1", "k2", "k3", "k4", "k5"};
    const int true_counts[nkeys] = {10, 8, 6, 4, 2, 1};

    spaceSavingManager *m = spaceSavingManagerCreate(k, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    /* Interleave the streams so evictions happen throughout, not just at the
     * start: round r records every key whose true count is still >= r. */
    uint64_t total = 0;
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < nkeys; i++) {
            if (true_counts[i] >= r) {
                recordName(m, names[i], 0);
                total++;
            }
        }
    }
    ASSERT_EQ(total, 31u); /* 10+8+6+4+2+1 */

    freezeOnce(m, 0);
    EXPECT_EQ(spaceSavingManagerFrozenTotal(m), total);
    /* Capacity is never exceeded. */
    ASSERT_LE(spaceSavingManagerCount(m), k);
    ASSERT_GT(spaceSavingManagerCount(m), 0);

    int n = spaceSavingManagerCount(m);
    for (int i = 0; i < n; i++) {
        sds key = NULL;
        int db = 0;
        uint64_t count = 0, error = 0;
        spaceSavingManagerAt(m, i, &key, &db, &count, &error);
        ASSERT_NE(key, nullptr);

        int idx = -1;
        for (int j = 0; j < nkeys; j++)
            if (strcmp(key, names[j]) == 0) idx = j;
        ASSERT_NE(idx, -1) << "frozen window reported an unknown key";

        uint64_t truth = (uint64_t)true_counts[idx];
        /* The error can never exceed the count, and the true count must lie
         * within [count - error, count]. */
        EXPECT_LE(error, count) << "key " << key;
        EXPECT_LE(count - error, truth) << "key " << key;
        EXPECT_GE(count, truth) << "key " << key;
    }

    /* Keys that were never recorded are absent, and an unused db is empty. */
    EXPECT_EQ(frozenFind(m, "never-seen", 0, NULL, NULL), 0);
    EXPECT_EQ(frozenFind(m, "k0", 7, NULL, NULL), 0);

    spaceSavingManagerRelease(m);
}

/* ---------------------------------------------------------------------------
 * 2. Any item whose frequency exceeds N/K is guaranteed to be tracked, even
 *    when the rest of the stream is a flood of one-hit keys competing for slots.
 * --------------------------------------------------------------------------*/
TEST(SpaceSaving, FrequencyAboveNOverKIsTracked) {
    const int k = 4;
    const int hot_hits = 40;
    const int noise_hits = 60; /* 60 distinct keys, one hit each */
    const uint64_t n = (uint64_t)(hot_hits + noise_hits);

    /* The guarantee only applies when the frequency is above N/K. */
    ASSERT_GT((uint64_t)hot_hits, n / (uint64_t)k);

    spaceSavingManager *m = spaceSavingManagerCreate(k, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    /* Interleave: the hot key must survive continuous eviction pressure rather
     * than simply being recorded last. */
    int noise_emitted = 0, hot_emitted = 0;
    while (noise_emitted < noise_hits || hot_emitted < hot_hits) {
        for (int i = 0; i < 3 && noise_emitted < noise_hits; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "noise:%d", noise_emitted++);
            recordName(m, buf, 0);
        }
        for (int i = 0; i < 2 && hot_emitted < hot_hits; i++) {
            recordName(m, "hot", 0);
            hot_emitted++;
        }
    }
    ASSERT_EQ(hot_emitted, hot_hits);
    ASSERT_EQ(noise_emitted, noise_hits);

    freezeOnce(m, 0);
    EXPECT_EQ(spaceSavingManagerFrozenTotal(m), n);

    uint64_t count = 0, error = 0;
    ASSERT_EQ(frozenFind(m, "hot", 0, &count, &error), 1) << "a key above N/K must be tracked";
    /* Its band must still contain the true frequency. */
    EXPECT_LE(count - error, (uint64_t)hot_hits);
    EXPECT_GE(count, (uint64_t)hot_hits);

    spaceSavingManagerRelease(m);
}

/* ---------------------------------------------------------------------------
 * 3. Rotation policy. A window is published only if it was closed within twice
 *    its configured length; past that its counts span too coarse an interval to
 *    label "the last window", so they are dropped — even when the window did
 *    receive traffic. A merely-late rotation keeps its counts.
 * --------------------------------------------------------------------------*/
TEST(SpaceSaving, LateRotationKeepsCountsUntilTheStalenessCutoff) {
    spaceSavingManager *m = spaceSavingManagerCreate(8, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    for (int i = 0; i < 3; i++) recordName(m, "a", 0);

    /* Still inside the first window: nothing is readable yet. */
    spaceSavingManagerRotate(m, WINDOW_US - 1);
    EXPECT_EQ(spaceSavingManagerCount(m), 0);
    EXPECT_EQ(spaceSavingManagerFrozenTotal(m), 0u);

    /* Crossing the boundary freezes exactly the completed window. */
    spaceSavingManagerRotate(m, WINDOW_US);
    ASSERT_EQ(spaceSavingManagerCount(m), 1);
    uint64_t count = 0;
    ASSERT_EQ(frozenFind(m, "a", 0, &count, NULL), 1);
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(spaceSavingManagerFrozenTotal(m), 3u);
    EXPECT_EQ(spaceSavingManagerFrozenDurationUs(m), WINDOW_US);

    /* A rotate that crosses no new boundary leaves the snapshot untouched. */
    spaceSavingManagerRotate(m, WINDOW_US + 1);
    EXPECT_EQ(spaceSavingManagerCount(m), 1);
    EXPECT_EQ(spaceSavingManagerFrozenTotal(m), 3u);

    /* A late rotation, still within the cutoff: the counts are kept and the
     * reported duration includes the lag rather than the nominal length. */
    for (int i = 0; i < 5; i++) recordName(m, "b", 0);
    uint64_t late = WINDOW_US + WINDOW_US + (WINDOW_US - 1); /* just under 2x */
    spaceSavingManagerRotate(m, late);
    ASSERT_EQ(frozenFind(m, "b", 0, &count, NULL), 1) << "a late rotation must not lose its counts";
    EXPECT_EQ(count, 5u);
    EXPECT_EQ(spaceSavingManagerFrozenDurationUs(m), 2 * WINDOW_US - 1);

    /* Past the cutoff the window is dropped, INCLUDING the traffic it saw: its
     * span is too coarse to publish as one window. */
    for (int i = 0; i < 7; i++) recordName(m, "c", 0);
    spaceSavingManagerRotate(m, late + 2 * WINDOW_US);
    EXPECT_EQ(spaceSavingManagerCount(m), 0) << "an over-long window must be dropped, not reported";
    EXPECT_EQ(spaceSavingManagerFrozenTotal(m), 0u);
    EXPECT_EQ(frozenFind(m, "c", 0, NULL, NULL), 0);
    EXPECT_EQ(spaceSavingManagerFrozenDurationUs(m), 0u);

    /* A long stall behaves the same way, and measuring re-bases on the drop, so
     * the very next window is a normal one. */
    for (int i = 0; i < 4; i++) recordName(m, "d", 0);
    spaceSavingManagerRotate(m, 1000 * WINDOW_US);
    EXPECT_EQ(spaceSavingManagerCount(m), 0);
    for (int i = 0; i < 6; i++) recordName(m, "e", 0);
    spaceSavingManagerRotate(m, 1001 * WINDOW_US);
    ASSERT_EQ(spaceSavingManagerCount(m), 1);
    ASSERT_EQ(frozenFind(m, "e", 0, &count, NULL), 1);
    EXPECT_EQ(count, 6u);
    EXPECT_EQ(spaceSavingManagerFrozenDurationUs(m), WINDOW_US);

    spaceSavingManagerRelease(m);
}

/* A late rotation must not shorten the FOLLOWING window: boundaries are measured
 * from when a window really started, so the lag does not propagate. */
TEST(SpaceSaving, RotationLagDoesNotShortenTheNextWindow) {
    spaceSavingManager *m = spaceSavingManagerCreate(8, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    const uint64_t lag_us = WINDOW_US / 2;
    recordName(m, "a", 0);
    spaceSavingManagerRotate(m, WINDOW_US + lag_us); /* late by half a window */
    ASSERT_EQ(spaceSavingManagerCount(m), 1);

    /* On a nominal grid the next window would already be due at 2 * WINDOW_US,
     * i.e. only half a length after this one opened. Measuring from the real
     * start keeps it open for a full length. */
    recordName(m, "b", 0);
    spaceSavingManagerRotate(m, 2 * WINDOW_US);
    EXPECT_EQ(frozenFind(m, "a", 0, NULL, NULL), 1) << "the next window must not be cut short by the lag";
    EXPECT_EQ(frozenFind(m, "b", 0, NULL, NULL), 0);

    /* It closes a full length after it actually opened. */
    spaceSavingManagerRotate(m, WINDOW_US + lag_us + WINDOW_US);
    ASSERT_EQ(frozenFind(m, "b", 0, NULL, NULL), 1);
    EXPECT_EQ(spaceSavingManagerFrozenDurationUs(m), WINDOW_US) << "no window may be shorter than configured";

    spaceSavingManagerRelease(m);
}

/* ---------------------------------------------------------------------------
 * 4. Reconfiguring the capacity: shrinking keeps the highest-count entries of
 *    the frozen window (and drops the rest), growing keeps everything.
 * --------------------------------------------------------------------------*/
TEST(SpaceSaving, TopKSelectionWhenCapacityShrinks) {
    spaceSavingManager *m = spaceSavingManagerCreate(5, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    /* Five keys in five slots: no eviction, so the counts are exact. */
    const int nkeys = 5;
    const char *names[nkeys] = {"a", "b", "c", "d", "e"};
    const int hits[nkeys] = {5, 4, 3, 2, 1};
    for (int i = 0; i < nkeys; i++)
        for (int h = 0; h < hits[i]; h++) recordName(m, names[i], 0);

    uint64_t now = freezeOnce(m, 0);
    ASSERT_EQ(spaceSavingManagerCount(m), 5);

    /* Shrink to 2: the two hottest survive with their counts intact. */
    spaceSavingManagerReconfigure(m, 2, WINDOW_US, now);
    ASSERT_EQ(spaceSavingManagerCount(m), 2);
    uint64_t count = 0;
    ASSERT_EQ(frozenFind(m, "a", 0, &count, NULL), 1);
    EXPECT_EQ(count, 5u);
    ASSERT_EQ(frozenFind(m, "b", 0, &count, NULL), 1);
    EXPECT_EQ(count, 4u);
    EXPECT_EQ(frozenFind(m, "c", 0, NULL, NULL), 0);
    EXPECT_EQ(frozenFind(m, "d", 0, NULL, NULL), 0);
    EXPECT_EQ(frozenFind(m, "e", 0, NULL, NULL), 0);

    /* The new capacity is honoured by the live window too: three distinct keys
     * cannot all be tracked at K=2. */
    recordName(m, "x", 0);
    recordName(m, "y", 0);
    recordName(m, "z", 0);
    now = freezeOnce(m, now);
    EXPECT_EQ(spaceSavingManagerCount(m), 2);

    /* Growing preserves what is already tracked. */
    for (int i = 0; i < 3; i++) recordName(m, "p", 0);
    recordName(m, "q", 0);
    now = freezeOnce(m, now);
    ASSERT_EQ(spaceSavingManagerCount(m), 2);
    spaceSavingManagerReconfigure(m, 8, WINDOW_US, now);
    EXPECT_EQ(spaceSavingManagerCount(m), 2) << "growing must not drop entries";
    ASSERT_EQ(frozenFind(m, "p", 0, &count, NULL), 1);
    EXPECT_EQ(count, 3u);

    spaceSavingManagerRelease(m);
}

/* ---------------------------------------------------------------------------
 * 5. RemoveIf applies to BOTH the live and the frozen window, so invalidated
 *    entries neither show up now nor resurface on the next rotation.
 * --------------------------------------------------------------------------*/

/* Predicate: drop everything in the database passed via `arg`. */
static int dropDb(sds key, int dbid, void *arg) {
    (void)key;
    return dbid == *(int *)arg;
}

/* Predicate: drop the single key named by `arg`, in any database. */
static int dropNamed(sds key, int dbid, void *arg) {
    (void)dbid;
    return strcmp(key, (const char *)arg) == 0;
}

TEST(SpaceSaving, RemoveIfPurgesLiveAndFrozenWindows) {
    spaceSavingManager *m = spaceSavingManagerCreate(8, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    /* Frozen window: one entry in db 0, one in db 1. */
    recordName(m, "keep-frozen", 0);
    recordName(m, "drop-frozen", 1);
    uint64_t now = freezeOnce(m, 0);
    ASSERT_EQ(spaceSavingManagerCount(m), 2);

    /* Live window: another pair, again split across the two databases. */
    recordName(m, "keep-live", 0);
    recordName(m, "drop-live", 1);

    int victim_db = 1;
    spaceSavingManagerRemoveIf(m, dropDb, &victim_db);

    /* The frozen window is purged immediately. */
    ASSERT_EQ(spaceSavingManagerCount(m), 1);
    EXPECT_EQ(frozenFind(m, "keep-frozen", 0, NULL, NULL), 1);
    EXPECT_EQ(frozenFind(m, "drop-frozen", 1, NULL, NULL), 0);

    /* Rotating promotes the live window: the dropped entry must not resurface,
     * which proves the live window was purged as well. */
    now = freezeOnce(m, now);
    ASSERT_EQ(spaceSavingManagerCount(m), 1);
    EXPECT_EQ(frozenFind(m, "keep-live", 0, NULL, NULL), 1);
    EXPECT_EQ(frozenFind(m, "drop-live", 1, NULL, NULL), 0);

    /* Removing by key name keeps the surrounding entries and their counts. */
    for (int i = 0; i < 4; i++) recordName(m, "target", 0);
    for (int i = 0; i < 2; i++) recordName(m, "bystander", 0);
    now = freezeOnce(m, now);
    ASSERT_EQ(spaceSavingManagerCount(m), 2);
    char victim[] = "target";
    spaceSavingManagerRemoveIf(m, dropNamed, victim);
    ASSERT_EQ(spaceSavingManagerCount(m), 1);
    uint64_t count = 0;
    ASSERT_EQ(frozenFind(m, "bystander", 0, &count, NULL), 1);
    EXPECT_EQ(count, 2u);

    /* A predicate matching nothing is a no-op; one matching everything empties
     * both windows. */
    char absent[] = "no-such-key";
    spaceSavingManagerRemoveIf(m, dropNamed, absent);
    EXPECT_EQ(spaceSavingManagerCount(m), 1);
    char keeper[] = "bystander";
    spaceSavingManagerRemoveIf(m, dropNamed, keeper);
    EXPECT_EQ(spaceSavingManagerCount(m), 0);

    spaceSavingManagerRelease(m);
}

/* ---------------------------------------------------------------------------
 * The per-window sampling percentage travels with the window it was recorded
 * for, so a frozen window stays interpretable after the configuration changes.
 * --------------------------------------------------------------------------*/
TEST(SpaceSaving, FrozenWindowKeepsTheSamplingPercentageThatProducedIt) {
    spaceSavingManager *m = spaceSavingManagerCreate(4, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    /* Nothing has been recorded or configured yet. */
    EXPECT_EQ(spaceSavingManagerFrozenSamplingPercentage(m), 0);

    spaceSavingManagerSetLiveSamplingPercentage(m, 100);
    recordName(m, "a", 0);
    uint64_t now = freezeOnce(m, 0);
    EXPECT_EQ(spaceSavingManagerFrozenSamplingPercentage(m), 100);

    /* Sampling is lowered afterwards. The already-frozen window must still
     * report the value its counts were gathered under. */
    spaceSavingManagerReconfigure(m, 4, WINDOW_US, now);
    spaceSavingManagerSetLiveSamplingPercentage(m, 10);
    EXPECT_EQ(spaceSavingManagerFrozenSamplingPercentage(m), 100)
        << "the frozen window must keep its own sampling percentage";
    EXPECT_EQ(frozenFind(m, "a", 0, NULL, NULL), 1) << "reconfigure must keep the frozen window";

    /* Once that window rotates out, the new percentage applies. */
    recordName(m, "b", 0);
    freezeOnce(m, now);
    EXPECT_EQ(spaceSavingManagerFrozenSamplingPercentage(m), 10);

    /* A full reset clears the percentage along with the data. */
    spaceSavingManagerReset(m, 0);
    EXPECT_EQ(spaceSavingManagerFrozenSamplingPercentage(m), 0);
    EXPECT_EQ(spaceSavingManagerCount(m), 0);

    spaceSavingManagerRelease(m);
}

/* The configured sampling percentage must survive a window being dropped. The
 * discard path resets both windows, and if that cleared the percentage the next
 * frozen window would carry 0 and every rate derived from it would silently come
 * back as zero until a config change re-set it. */
TEST(SpaceSaving, SamplingPercentageSurvivesADroppedWindow) {
    spaceSavingManager *m = spaceSavingManagerCreate(4, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    spaceSavingManagerSetLiveSamplingPercentage(m, 100);
    recordName(m, "a", 0);

    /* Stall well past the cutoff, so the window is dropped rather than frozen. */
    spaceSavingManagerRotate(m, 10 * WINDOW_US);
    ASSERT_EQ(spaceSavingManagerCount(m), 0);

    /* The next window still knows how its counts are being sampled. */
    recordName(m, "b", 0);
    spaceSavingManagerRotate(m, 11 * WINDOW_US);
    ASSERT_EQ(spaceSavingManagerCount(m), 1);
    EXPECT_EQ(spaceSavingManagerFrozenSamplingPercentage(m), 100) << "a dropped window must not clear the config";

    /* An explicit reset preserves it as well. */
    spaceSavingManagerReset(m, 11 * WINDOW_US);
    recordName(m, "c", 0);
    spaceSavingManagerRotate(m, 12 * WINDOW_US);
    ASSERT_EQ(spaceSavingManagerCount(m), 1);
    EXPECT_EQ(spaceSavingManagerFrozenSamplingPercentage(m), 100);

    spaceSavingManagerRelease(m);
}

/* Record one observation of `name` in database `dbid` carrying `weight`, of
 * which `weight2` is the secondary-accumulator component — the weighted entry
 * point traffic tracking uses. */
static void recordNameWeighted(spaceSavingManager *m, const char *name, int dbid, uint64_t weight, uint64_t weight2) {
    sds k = sdsnew(name);
    recordSpaceSavingManagerSampleWeighted(m, k, dbid, weight, weight2);
    sdsfree(k);
}

/* ---------------------------------------------------------------------------
 * Weighted Space-Saving (the traffic-tracking use). Weights replace the +1 per
 * observation; the band and N/K guarantees must hold over the summed weight.
 * --------------------------------------------------------------------------*/

/* Weights accumulate: a slot's count is the sum of the weights recorded for it
 * and the window total is the sum over all observations, unweighted or not. */
TEST(SpaceSaving, WeightedSamplesAccumulateWeights) {
    spaceSavingManager *m = spaceSavingManagerCreate(4, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    recordNameWeighted(m, "big", 0, 1000, 0);
    recordNameWeighted(m, "big", 0, 500, 0);
    recordNameWeighted(m, "small", 0, 3, 0);
    recordName(m, "counted", 0); /* the unweighted entry point still weighs 1 */

    freezeOnce(m, 0);
    uint64_t count = 0, error = 0;
    ASSERT_EQ(frozenFind(m, "big", 0, &count, &error), 1);
    EXPECT_EQ(count, 1500u);
    EXPECT_EQ(error, 0u) << "no eviction, so the sum is exact";
    ASSERT_EQ(frozenFind(m, "small", 0, &count, NULL), 1);
    EXPECT_EQ(count, 3u);
    ASSERT_EQ(frozenFind(m, "counted", 0, &count, NULL), 1);
    EXPECT_EQ(count, 1u);
    /* The window total is the summed weight, 1000+500+3+1. */
    EXPECT_EQ(spaceSavingManagerFrozenTotal(m), 1504u);

    spaceSavingManagerRelease(m);
}

/* Across evictions, the [count - error, count] band must contain the item's
 * true summed weight, exactly as it contains the true count in the unweighted
 * case. */
TEST(SpaceSaving, WeightedErrorBandContainsTrueWeight) {
    const int k = 3;
    /* Six distinct keys into three slots with distinct weights forces repeated
     * eviction under weight pressure. */
    const int nkeys = 6;
    const char *names[nkeys] = {"w0", "w1", "w2", "w3", "w4", "w5"};
    const uint64_t true_weights[nkeys] = {100, 80, 60, 40, 20, 10};

    spaceSavingManager *m = spaceSavingManagerCreate(k, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    /* Interleave so evictions happen throughout: round r records every key
     * whose remaining weight still covers a 10-unit slice. */
    uint64_t total = 0;
    for (int r = 0; r < 10; r++) {
        for (int i = 0; i < nkeys; i++) {
            if (true_weights[i] / 10 > (uint64_t)r) {
                recordNameWeighted(m, names[i], 0, 10, 0);
                total += 10;
            }
        }
    }
    ASSERT_EQ(total, 310u); /* 100+80+60+40+20+10 */

    freezeOnce(m, 0);
    EXPECT_EQ(spaceSavingManagerFrozenTotal(m), total);
    ASSERT_LE(spaceSavingManagerCount(m), k);

    int n = spaceSavingManagerCount(m);
    for (int i = 0; i < n; i++) {
        sds key = NULL;
        int db = 0;
        uint64_t count = 0, error = 0;
        spaceSavingManagerAt(m, i, &key, &db, &count, &error);
        ASSERT_NE(key, nullptr);

        int idx = -1;
        for (int j = 0; j < nkeys; j++)
            if (strcmp(key, names[j]) == 0) idx = j;
        ASSERT_NE(idx, -1) << "frozen window reported an unknown key";

        uint64_t truth = true_weights[idx];
        EXPECT_LE(error, count) << "key " << key;
        EXPECT_LE(count - error, truth) << "key " << key;
        EXPECT_GE(count, truth) << "key " << key;
    }

    spaceSavingManagerRelease(m);
}

/* The traffic scenario the weighted stream exists for: a key observed rarely
 * but carrying a large weight (a big value read a few times) must outrank and
 * be tracked ahead of a key observed often with tiny weights (a small value
 * read constantly) — the reverse of the unweighted ranking. */
TEST(SpaceSaving, FewHeavyObservationsOutrankManyLightOnes) {
    const int k = 2;
    spaceSavingManager *m = spaceSavingManagerCreate(k, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    /* "huge": 5 accesses x 1,000,000 bytes. "tiny": 5000 accesses x 10 bytes.
     * Byte totals: 5,000,000 vs 50,000 — huge holds over N/K, so it must be
     * tracked even though tiny's access count is 1000x higher. */
    const uint64_t huge_bytes = 5ULL * 1000000ULL;
    const uint64_t tiny_bytes = 5000ULL * 10ULL;
    const uint64_t total = huge_bytes + tiny_bytes;
    ASSERT_GT(huge_bytes, total / (uint64_t)k) << "huge must exceed N/K to be guaranteed tracked";

    for (int i = 0; i < 5000; i++) {
        recordNameWeighted(m, "tiny", 0, 10, 0);
        if (i % 1000 == 0) recordNameWeighted(m, "huge", 0, 1000000, 0);
    }

    freezeOnce(m, 0);
    EXPECT_EQ(spaceSavingManagerFrozenTotal(m), total);

    uint64_t count = 0, error = 0;
    ASSERT_EQ(frozenFind(m, "huge", 0, &count, &error), 1) << "a key above N/K by weight must be tracked";
    EXPECT_LE(count - error, huge_bytes);
    EXPECT_GE(count, huge_bytes);

    /* And it ranks first: its (lower-bound) byte total beats tiny's. */
    uint64_t tiny_count = 0, tiny_error = 0;
    ASSERT_EQ(frozenFind(m, "tiny", 0, &tiny_count, &tiny_error), 1);
    EXPECT_GT(count - error, tiny_count) << "5 heavy accesses must outrank 5000 light ones";

    spaceSavingManagerRelease(m);
}

/* ---------------------------------------------------------------------------
 * A window reports the interval it REALLY accumulated over, not its configured
 * length. Rotation is timer-driven, so it runs at or after the nominal
 * boundary; using the configured length as a rate denominator would
 * systematically over-report by that lag.
 * --------------------------------------------------------------------------*/
TEST(SpaceSaving, FrozenDurationIsTheRealSpanIncludingRotationLag) {
    spaceSavingManager *m = spaceSavingManagerCreate(8, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    /* No completed window yet. */
    EXPECT_EQ(spaceSavingManagerFrozenDurationUs(m), 0u);

    /* Rotation runs late: the boundary is at WINDOW_US but cron only gets to it
     * half a window later, and the samples in between land in this window. */
    const uint64_t lag_us = WINDOW_US / 2;
    for (int i = 0; i < 10; i++) recordName(m, "a", 0);
    spaceSavingManagerRotate(m, WINDOW_US + lag_us);
    ASSERT_EQ(spaceSavingManagerCount(m), 1);
    EXPECT_EQ(spaceSavingManagerFrozenDurationUs(m), WINDOW_US + lag_us)
        << "the frozen duration must include the rotation lag";

    /* The next window starts when the rotation actually happened, not at the
     * nominal grid position, so consecutive durations do not double-count the
     * lag: closing the next window one length later spans exactly one length. */
    for (int i = 0; i < 4; i++) recordName(m, "b", 0);
    spaceSavingManagerRotate(m, 2 * WINDOW_US + lag_us);
    ASSERT_EQ(frozenFind(m, "b", 0, NULL, NULL), 1);
    EXPECT_EQ(spaceSavingManagerFrozenDurationUs(m), WINDOW_US);

    /* Reset clears the recorded timing. */
    spaceSavingManagerReset(m, 5 * WINDOW_US);
    EXPECT_EQ(spaceSavingManagerFrozenDurationUs(m), 0u);
    for (int i = 0; i < 3; i++) recordName(m, "c", 0);
    spaceSavingManagerRotate(m, 6 * WINDOW_US);
    ASSERT_EQ(frozenFind(m, "c", 0, NULL, NULL), 1);
    EXPECT_EQ(spaceSavingManagerFrozenDurationUs(m), WINDOW_US)
        << "the window must be measured from the reset, not from creation";

    spaceSavingManagerRelease(m);
}

/* ---------------------------------------------------------------------------
 * The secondary accumulator: an exact, caller-chosen component of the weight
 * (traffic tracking uses it for the write-direction split). It must accumulate
 * independently of the primary, stay untouched by the unweighted API, and
 * restart from the new identity's own observations on eviction — unlike the
 * primary count, which inherits the evicted minimum.
 * --------------------------------------------------------------------------*/
TEST(SpaceSaving, SecondaryAccumulatorIsExactAndRestartsOnEviction) {
    const int k = 2;
    spaceSavingManager *m = spaceSavingManagerCreate(k, WINDOW_US, 0);
    ASSERT_NE(m, nullptr);

    /* Accumulation: (1000, 300) then (500, 200) gives count 1500 / count2 500.
     * The unweighted API contributes nothing to the secondary. */
    recordNameWeighted(m, "a", 0, 1000, 300);
    recordNameWeighted(m, "a", 0, 500, 200);
    recordName(m, "a", 0);
    recordNameWeighted(m, "b", 0, 50, 50);

    freezeOnce(m, 0);
    uint64_t count = 0, count2 = 0;
    ASSERT_EQ(frozenFind2(m, "a", 0, &count, NULL, &count2), 1);
    EXPECT_EQ(count, 1501u);
    EXPECT_EQ(count2, 500u);
    ASSERT_EQ(frozenFind2(m, "b", 0, &count, NULL, &count2), 1);
    EXPECT_EQ(count, 50u);
    EXPECT_EQ(count2, 50u) << "a full-write observation charges its whole weight as secondary";

    /* Eviction: the live window is fresh, so refill both slots first, then
     * overflow with "e" to evict the smaller ("c", count 10). The primary
     * inherits c's minimum (10 + 7 = 17) but the secondary must restart from
     * e's own observation only (7, not 4 + 7). */
    uint64_t now = WINDOW_US;
    recordNameWeighted(m, "c", 0, 10, 4);
    recordNameWeighted(m, "d", 0, 50, 50);
    recordNameWeighted(m, "e", 0, 7, 7);
    now = freezeOnce(m, now);
    ASSERT_EQ(frozenFind2(m, "e", 0, &count, NULL, &count2), 1);
    EXPECT_EQ(count, 17u) << "the primary inherits the evicted minimum (10 + 7)";
    EXPECT_EQ(count2, 7u) << "the secondary must NOT inherit; it counts only e's own observations";
    EXPECT_LE(count2, count) << "the secondary is a component of the primary";
    EXPECT_EQ(frozenFind(m, "c", 0, NULL, NULL), 0) << "the evicted key is gone";

    spaceSavingManagerRelease(m);
}
