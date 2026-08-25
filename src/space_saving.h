#ifndef SPACE_SAVING_H
#define SPACE_SAVING_H

#include <stdint.h>
#include <stddef.h>

#include "sds.h"

/*
 * space_saving — Space-Saving top-K frequency tracking over fixed time windows.
 *
 * Space-Saving (Metwally, Agrawal & El Abbadi, 2005) approximates the K most
 * frequent items in a stream using O(K) memory. It keeps K (item, count, error)
 * slots and, for each observation:
 *   1. if the item is already tracked, increment its count;
 *   2. else if a slot is free, insert it with count = 1, error = 0;
 *   3. else evict the smallest-count slot and reuse it: the new item takes
 *      count = min_count + 1, error = min_count.
 * Per-window guarantees: a tracked item's true count is in [count - error,
 * count], and any item whose true frequency exceeds N/K (N = observations in
 * the window) is guaranteed tracked.
 *
 * The tracked item is a (key name, database id) pair — the hot-key identity.
 * Keeping it concrete keeps the hot path free of indirect calls: comparison,
 * hashing and copying are all inlined here. The stored key is an owned `sds`
 * copy, duplicated ONLY after the item is confirmed absent and a slot is
 * committed to it, so the "already tracked" path performs no allocation.
 * Anything derived from the key (for example the cluster hash slot) is computed
 * on demand by the caller rather than stored per entry.
 *
 * The manager keeps two windows: a `live` one accumulating the current interval
 * and a `frozen` snapshot of the last completed interval. Readers observe only
 * the frozen window, never a partial one. Each window also records the real
 * interval it accumulated over and the sampling percentage its counts were
 * gathered under, so a reader can turn a frozen window into a rate correctly
 * even after the configuration has since changed.
 *
 * The caller supplies a monotonic microsecond clock on each call, so this
 * module has no global-clock dependency, and drives window boundaries by
 * calling spaceSavingManagerRotate() on a timer — recording a sample never
 * reads the clock.
 *
 * Not thread-safe: guard externally if shared across threads.
 */

typedef struct spaceSavingManager spaceSavingManager;

/* Create a manager tracking up to `k` items per window, with a window length of
 * `window_us` microseconds. `now_us` seeds the first window start. */
spaceSavingManager *spaceSavingManagerCreate(int k, uint64_t window_us, uint64_t now_us);
/* Free the manager and every key it owns. NULL-safe. */
void spaceSavingManagerRelease(spaceSavingManager *m);
/* Clear both windows (including their recorded timing) and restart measuring at
 * `now_us`. The configured sampling percentage is preserved, since it describes
 * how the next observations will be gathered rather than the data dropped. */
void spaceSavingManagerReset(spaceSavingManager *m, uint64_t now_us);
/* Close the live window if its configured length has fully elapsed (no-op if it
 * is still open). A window that ran past TWICE the configured length is dropped
 * instead of frozen: its counts span too coarse an interval to publish as "the
 * last window". So a frozen window always spans [length, 2 * length) — never
 * shorter than configured, and never a long-run average mislabelled as one
 * window. Boundaries are measured from the live window's real start, so a late
 * call cannot shorten the following window. */
void spaceSavingManagerRotate(spaceSavingManager *m, uint64_t now_us);
/* Record one observation of (`key`, `dbid`) into the current (live) window.
 * `key` is borrowed — it is copied only if a slot is committed to it. Does NOT
 * rotate: the caller must drive boundaries via spaceSavingManagerRotate() on a
 * timer, keeping this hot path free of any clock read. */
void recordSpaceSavingManagerSample(spaceSavingManager *m, sds key, int dbid);
/* Number of items in the last completed (frozen) window. */
int spaceSavingManagerCount(spaceSavingManager *m);
/* Read the i-th item of the frozen window (0 <= i < count). Out-params may be
 * NULL; `*key` remains owned by the module and is valid until the next mutating
 * call. Slots are unordered. */
void spaceSavingManagerAt(spaceSavingManager *m, int i, sds *key, int *dbid, uint64_t *count, uint64_t *error);
/* Remove every item for which `pred(key, dbid, arg)` is non-zero, from BOTH the
 * live and frozen windows. */
void spaceSavingManagerRemoveIf(spaceSavingManager *m, int (*pred)(sds key, int dbid, void *arg), void *arg);
/* Total observations recorded in the last completed (frozen) window (N). */
uint64_t spaceSavingManagerFrozenTotal(spaceSavingManager *m);

/* Record the sampling percentage that the current (live) window's counts are
 * being gathered under. It travels with the window when it is frozen, so a
 * reader can scale the frozen counts by the percentage that produced them even
 * after a later change. */
void spaceSavingManagerSetLiveSamplingPercentage(spaceSavingManager *m, int sampling_percentage);
/* Sampling percentage that was in effect for the last completed (frozen)
 * window; 0 when the window never had one (freshly created or reset). */
int spaceSavingManagerFrozenSamplingPercentage(spaceSavingManager *m);
/* Real time the last completed (frozen) window spent accumulating, in
 * microseconds, or 0 if there is no completed window yet — which also covers a
 * window that was dropped for being too coarse (see spaceSavingManagerRotate),
 * so 0 does not distinguish "just started" from "just dropped one". Rotation is
 * driven by the caller's timer, so a window is closed at or after its nominal
 * boundary and this is its configured length plus the rotation lag. Rates must
 * be derived from THIS, not from the configured window length, or they
 * over-report by that lag. */
uint64_t spaceSavingManagerFrozenDurationUs(spaceSavingManager *m);

/* Reset only the live window and restart it at `now_us` with the given capacity
 * and window length, keeping the last completed (frozen) window and its
 * sampling config. Use on a config change instead of releasing/recreating the
 * manager. Shrinking the capacity keeps the highest-count entries. */
void spaceSavingManagerReconfigure(spaceSavingManager *m, int new_k, uint64_t new_window_us, uint64_t now_us);

#endif /* SPACE_SAVING_H */
