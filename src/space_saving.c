/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Generic Space-Saving top-K. See space_saving.h for the algorithm, the
 * ownership model, and usage. Depends only on the zmalloc allocator.
 *
 * Storage is a flat, unordered array of `capacity` slots. Membership lookup and
 * smallest-count selection are done in a single linear scan; K is expected to
 * be small (tens), so the scan is a handful of cache lines and beats the
 * bookkeeping of a heap/linked structure at this size.
 */

#include "zmalloc.h" /* zmalloc / zcalloc / zfree */
#include "space_saving.h"

/* ===========================================================================
 * spaceSavingWindow — a single summary
 * ==========================================================================*/

typedef struct {
    void *item;     /* Owned identity (produced by type->dup), or NULL if free */
    uint32_t hash;  /* Cached type->hash(item) for fast reject (0 if no hash fn) */
    uint64_t count; /* Estimated count (upper bound on the true count) */
    uint64_t error; /* Maximum overestimate vs. the true count */
} spaceSavingSlot;

struct spaceSavingWindow {
    spaceSavingSlot *slots; /* capacity-sized array */
    int capacity;           /* K */
    int size;               /* number of occupied slots (0..capacity) */
    spaceSavingType *type;  /* caller-provided item vtable */
};

spaceSavingWindow *spaceSavingWindowCreate(int k, spaceSavingType *type) {
    if (k <= 0 || !type || !type->cmp || !type->dup || !type->free) return NULL;
    spaceSavingWindow *w = zcalloc(sizeof(*w));
    w->slots = zcalloc((size_t)k * sizeof(spaceSavingSlot));
    w->capacity = k;
    w->size = 0;
    w->type = type;
    return w;
}

void spaceSavingWindowReset(spaceSavingWindow *w) {
    if (!w) return;
    for (int i = 0; i < w->size; i++) {
        if (w->slots[i].item) w->type->free(w->slots[i].item);
        w->slots[i].item = NULL;
    }
    w->size = 0;
}

void spaceSavingWindowRelease(spaceSavingWindow *w) {
    if (!w) return;
    spaceSavingWindowReset(w);
    zfree(w->slots);
    zfree(w);
}

void recordSpaceSavingWindowSample(spaceSavingWindow *w, const void *item) {
    if (!w || !item) return;
    uint32_t h = w->type->hash ? w->type->hash(item) : 0;

    /* Single pass: look for an existing slot (fast-rejecting on the cached hash
     * before the potentially expensive cmp) while tracking the smallest-count
     * slot for the eviction path. */
    int min_idx = 0;
    uint64_t min_count = UINT64_MAX;
    for (int i = 0; i < w->size; i++) {
        spaceSavingSlot *e = &w->slots[i];
        if (e->hash == h && w->type->cmp(item, e->item) == 0) {
            e->count += 1;
            return;
        }
        if (e->count < min_count) {
            min_count = e->count;
            min_idx = i;
        }
    }

    /* Room available: insert with count = 1, error = 0. */
    if (w->size < w->capacity) {
        spaceSavingSlot *e = &w->slots[w->size++];
        e->item = w->type->dup(item);
        e->hash = h;
        e->count = 1;
        e->error = 0;
        return;
    }

    /* Full: evict the smallest-count slot. The new item inherits count = the
     * evicted count + 1; error records the maximum possible overestimate. */
    spaceSavingSlot *e = &w->slots[min_idx];
    w->type->free(e->item);
    e->item = w->type->dup(item);
    e->hash = h;
    e->count = min_count + 1;
    e->error = min_count;
}

int spaceSavingWindowCount(spaceSavingWindow *w) {
    return w ? w->size : 0;
}

void spaceSavingWindowAt(spaceSavingWindow *w, int i, void **item, uint64_t *count, uint64_t *error) {
    if (!w || i < 0 || i >= w->size) return;
    spaceSavingSlot *e = &w->slots[i];
    if (item) *item = e->item;
    if (count) *count = e->count;
    if (error) *error = e->error;
}

void spaceSavingWindowRemoveIf(spaceSavingWindow *w, int (*pred)(const void *item, void *arg), void *arg) {
    if (!w || !pred) return;
    int out = 0;
    for (int i = 0; i < w->size; i++) {
        if (pred(w->slots[i].item, arg)) {
            w->type->free(w->slots[i].item);
            continue; /* drop: do not advance the write cursor */
        }
        if (out != i) w->slots[out] = w->slots[i];
        out++;
    }
    w->size = out;
}

/* ===========================================================================
 * spaceSavingManager — frozen-window top-K over fixed time windows.
 * See space_saving.h for the model and usage.
 * ==========================================================================*/

struct spaceSavingManager {
    spaceSavingWindow *live;   /* Current (open) window */
    spaceSavingWindow *frozen; /* Last completed window (read path) */
    uint64_t window_us;        /* Window length in microseconds */
    uint64_t window_start_us;  /* Start time of the current window */
};

spaceSavingManager *spaceSavingManagerCreate(int k, uint64_t window_us, uint64_t now_us, spaceSavingType *type) {
    spaceSavingManager *m = zcalloc(sizeof(*m));
    m->live = spaceSavingWindowCreate(k, type);
    m->frozen = spaceSavingWindowCreate(k, type);
    m->window_us = window_us;
    m->window_start_us = now_us;
    if (!m->live || !m->frozen) {
        spaceSavingManagerRelease(m);
        return NULL;
    }
    return m;
}

void spaceSavingManagerRelease(spaceSavingManager *m) {
    if (!m) return;
    spaceSavingWindowRelease(m->live);
    spaceSavingWindowRelease(m->frozen);
    zfree(m);
}

void spaceSavingManagerReset(spaceSavingManager *m, uint64_t now_us) {
    if (!m) return;
    spaceSavingWindowReset(m->live);
    spaceSavingWindowReset(m->frozen);
    m->window_start_us = now_us;
}

void spaceSavingManagerSetWindow(spaceSavingManager *m, uint64_t window_us) {
    if (m) m->window_us = window_us;
}

/* Freeze the live window: the previous snapshot is discarded, the live window
 * becomes the new frozen snapshot, and a fresh empty live window starts.
 * A pointer swap + reset, so it is O(K) with no reallocation and transfers item
 * ownership without copying. */
static void spaceSavingManagerFreeze(spaceSavingManager *m) {
    spaceSavingWindow *tmp = m->frozen;
    m->frozen = m->live;
    m->live = tmp;
    spaceSavingWindowReset(m->live);
}

void spaceSavingManagerRotate(spaceSavingManager *m, uint64_t now_us) {
    uint64_t elapsed, windows;
    if (!m || m->window_us == 0) return;
    if (now_us <= m->window_start_us) return; /* monotonic guard */
    elapsed = now_us - m->window_start_us;
    if (elapsed < m->window_us) return; /* current window still open */
    windows = elapsed / m->window_us;   /* full windows elapsed */

    /* One freeze snapshots the just-completed window. If two or more whole
     * windows elapsed (an idle gap), a second freeze pushes that snapshot out
     * too, so the frozen window correctly ends up empty. Any freeze past the
     * second is a no-op on an already-empty summary, so cap at two. */
    int freeze_count = (windows >= 2) ? 2 : 1;
    for (int i = 0; i < freeze_count; i++) spaceSavingManagerFreeze(m);
    m->window_start_us += windows * m->window_us;
}

void recordSpaceSavingManagerSample(spaceSavingManager *m, uint64_t now_us, const void *item) {
    if (!m) return;
    spaceSavingManagerRotate(m, now_us);
    recordSpaceSavingWindowSample(m->live, item);
}

int spaceSavingManagerCount(spaceSavingManager *m) {
    return m ? spaceSavingWindowCount(m->frozen) : 0;
}

void spaceSavingManagerAt(spaceSavingManager *m, int i, void **item, uint64_t *count, uint64_t *error) {
    if (m) spaceSavingWindowAt(m->frozen, i, item, count, error);
}

void spaceSavingManagerRemoveIf(spaceSavingManager *m, int (*pred)(const void *item, void *arg), void *arg) {
    if (!m) return;
    spaceSavingWindowRemoveIf(m->live, pred, arg);
    spaceSavingWindowRemoveIf(m->frozen, pred, arg);
}

uint64_t spaceSavingEstimateRate(uint64_t count, uint64_t error, int sample_percentage, int window_seconds) {
    if (sample_percentage <= 0 || window_seconds <= 0) return 0;
    /* Midpoint of [count-error, count] is (count - error/2); the *2 below keeps
     * error/2 exact. Scale the sampled count back up by 100/sample_percentage,
     * then divide by the window length to get a per-second rate. */
    uint64_t num = (2 * count - error) * 100;
    uint64_t den = 2ULL * (uint64_t)sample_percentage * (uint64_t)window_seconds;
    return (num + den / 2) / den; /* rounded to nearest */
}
