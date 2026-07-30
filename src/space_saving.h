#ifndef SPACE_SAVING_H
#define SPACE_SAVING_H

#include <stdint.h>
#include <stddef.h>

/*
 * space_saving — generic Space-Saving top-K frequency tracking.
 *
 * Space-Saving (Metwally, Agrawal & El Abbadi, 2005) approximates the K most
 * frequent items in a stream using O(K) memory. It keeps K (item, count, error)
 * slots and, for each observation:
 *   1. if the item is already tracked, increment its count;
 *   2. else if a slot is free, insert it with count = 1, error = 0;
 *   3. else evict the smallest-count slot and reuse it: the new item takes
 *      count = min_count + 1, error = min_count.
 * Per-window guarantees: a tracked item's true count is in [count - error,
 * count], and any item whose true frequency exceeds N/K is guaranteed tracked.
 *
 * Two layers are provided:
 *   - spaceSavingWindow  — a single summary (the raw algorithm above).
 *   - spaceSavingManager — a frozen-window manager over two windows (a live one
 *     and the last completed one), for rate reporting over fixed time windows.
 *
 * The library owns `count`/`error` (algorithm state) and is otherwise
 * item-agnostic: the caller decides what an "item" is (a key name, a (key, db)
 * pair, a client id, ...) and supplies callbacks to compare, duplicate and free
 * item identities. The library owns the identities it stores (produced by
 * `dup`) and releases them on eviction, reset and release. An item is
 * duplicated ONLY after it is confirmed absent and a slot is committed to it,
 * so the hot "already tracked" path performs no allocation.
 *
 * Not thread-safe: guard externally if shared across threads. Depends only on
 * the zmalloc allocator — no other project coupling.
 */

/* --------------------------------------------------------------------------
 * Item vtable (shared by both layers)
 * --------------------------------------------------------------------------*/

/* Compare two item identities. Return 0 when equal, non-zero otherwise.
 * `b` is always a stored item; `a` is the probe passed to the mutating call. */
typedef int (*spaceSavingCmpFn)(const void *a, const void *b);
/* Return a newly heap-allocated, independent copy of `item`. Called at most
 * once per inserted item, only after it was confirmed absent. Ownership passes
 * to the library. */
typedef void *(*spaceSavingDupFn)(const void *item);
/* Free an identity previously returned by `dup`. */
typedef void (*spaceSavingFreeFn)(void *item);
/* Optional fast-reject hash. When non-NULL it is cached per slot and checked
 * before `cmp`, so only equal hashes reach `cmp`. NULL to rely on `cmp` alone. */
typedef uint32_t (*spaceSavingHashFn)(const void *item);

typedef struct {
    spaceSavingCmpFn cmp;
    spaceSavingDupFn dup;
    spaceSavingFreeFn free;
    spaceSavingHashFn hash; /* optional, may be NULL */
} spaceSavingType;

/* --------------------------------------------------------------------------
 * spaceSavingWindow — a single Space-Saving summary
 * --------------------------------------------------------------------------*/

typedef struct spaceSavingWindow spaceSavingWindow;

/* Create a summary tracking up to `k` items. `type` must outlive it. */
spaceSavingWindow *spaceSavingWindowCreate(int k, spaceSavingType *type);
/* Free the summary and every identity it owns. NULL-safe. */
void spaceSavingWindowRelease(spaceSavingWindow *w);
/* Drop all tracked items but keep capacity and type. */
void spaceSavingWindowReset(spaceSavingWindow *w);
/* Record one observation of `item` (borrowed; copied via `dup` only on insert). */
void recordSpaceSavingWindowSample(spaceSavingWindow *w, const void *item);
/* Number of items currently tracked (0..k). */
int spaceSavingWindowCount(spaceSavingWindow *w);
/* Read the i-th tracked slot (0 <= i < count). Out-params may be NULL; `*item`
 * is library-owned and valid until the next mutating call. Slots are unordered. */
void spaceSavingWindowAt(spaceSavingWindow *w, int i, void **item, uint64_t *count, uint64_t *error);
/* Remove every tracked item for which `pred(item, arg)` is non-zero. */
void spaceSavingWindowRemoveIf(spaceSavingWindow *w, int (*pred)(const void *item, void *arg), void *arg);

/* --------------------------------------------------------------------------
 * spaceSavingManager — frozen-window top-K over fixed time windows
 *
 * Keeps a `live` window accumulating the current interval and a `frozen`
 * snapshot of the last completed one. The caller supplies a monotonic
 * microsecond clock on each call, so the module has no global-clock dependency.
 * When `window_us` elapses, the live window is frozen and a fresh one starts;
 * readers observe only the last completed window, never a partial one.
 * --------------------------------------------------------------------------*/

typedef struct spaceSavingManager spaceSavingManager;

/* Create a manager tracking up to `k` items per window, with a window length of
 * `window_us` microseconds. `now_us` seeds the first window start. */
spaceSavingManager *spaceSavingManagerCreate(int k, uint64_t window_us, uint64_t now_us, spaceSavingType *type);
/* Free the manager and both windows. NULL-safe. */
void spaceSavingManagerRelease(spaceSavingManager *m);
/* Clear both windows and restart the current window at `now_us`. */
void spaceSavingManagerReset(spaceSavingManager *m, uint64_t now_us);
/* Change the window length; takes effect at the next boundary check. */
void spaceSavingManagerSetWindow(spaceSavingManager *m, uint64_t window_us);
/* Freeze whichever window(s) fully elapsed by `now_us` (no-op if still open). */
void spaceSavingManagerRotate(spaceSavingManager *m, uint64_t now_us);
/* Record one observation into the current window (rotating first if due). */
void recordSpaceSavingManagerSample(spaceSavingManager *m, uint64_t now_us, const void *item);
/* Number of items in the last completed (frozen) window. */
int spaceSavingManagerCount(spaceSavingManager *m);
/* Read the i-th item of the frozen window (see spaceSavingWindowAt). */
void spaceSavingManagerAt(spaceSavingManager *m, int i, void **item, uint64_t *count, uint64_t *error);
/* Remove matching items from BOTH the live and frozen windows. */
void spaceSavingManagerRemoveIf(spaceSavingManager *m, int (*pred)(const void *item, void *arg), void *arg);

/* Recover a per-second rate from a Space-Saving (count, error) pair whose counts
 * were produced by Bernoulli sampling at `sample_percentage` percent over a
 * window of `window_seconds`:
 *
 *   rate = (count - error/2) * (100 / sample_percentage) / window_seconds
 *
 * Uses the midpoint of the [count-error, count] band and exact integer
 * arithmetic (rounded to nearest). Returns 0 for non-positive inputs. */
uint64_t spaceSavingEstimateRate(uint64_t count, uint64_t error, int sample_percentage, int window_seconds);

#endif /* SPACE_SAVING_H */
