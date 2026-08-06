/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sorted_array.h"
#include "zmalloc.h"

#include <assert.h>
#include <string.h>

#define SORTED_ARRAY_INITIAL_CAPACITY 8

sortedArray *sortedArrayCreate(sortedArrayCompareFn compare) {
    assert(compare != NULL);
    sortedArray *sa = zmalloc(sizeof(sortedArray));
    sa->nodes = NULL;
    sa->len = 0;
    sa->capacity = 0;
    sa->compare = compare;
    sa->free = NULL;
    return sa;
}

/* Remove all the elements from the sorted array without destroying the array itself. */
void sortedArrayEmpty(sortedArray *sa) {
    if (!sa) return;
    if (sa->free) {
        for (unsigned long i = 0; i < sa->len; i++) sa->free(sa->nodes[i].value);
    }
    sa->len = 0;
}

/* Free the whole sorted array.
 *
 * This function can't fail. */
void sortedArrayRelease(sortedArray *sa) {
    if (!sa) return;
    sortedArrayEmpty(sa);
    if (sa->nodes) zfree(sa->nodes);
    zfree(sa);
}

/* Return the element that sorts first, or NULL when empty.
 * Ownership is retained by the sorted array, caller must not free it. */
void *sortedArrayPeekMin(sortedArray *sa) {
    if (sa->len == 0) return NULL;
    return sa->nodes[sa->len - 1].value;
}

/* Locate the slot a new element belongs in. */
static unsigned long sortedArrayFindSlot(sortedArray *sa, void *value) {
    unsigned long lo = 0, hi = sa->len;
    while (lo < hi) {
        unsigned long mid = lo + (hi - lo) / 2;
        if (sa->compare(sa->nodes[mid].value, value) < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

/* Insert an element, keeping the array in reverse comparator order.
 *
 * This grows without bound; enforcing a maximum length is the caller's job.
 * Ownership of 'value' is transferred to the sorted array. The caller must not
 * free it or access it after this call. */
void sortedArrayInsert(sortedArray *sa, void *value) {
    if (sa->len >= sa->capacity) {
        unsigned long new_cap = sa->capacity == 0 ? SORTED_ARRAY_INITIAL_CAPACITY : sa->capacity * 2;
        sa->nodes = zrealloc(sa->nodes, new_cap * sizeof(sortedArrayNode));
        sa->capacity = new_cap;
    }

    unsigned long slot = sortedArrayFindSlot(sa, value);

    /* Open a gap at the insertion point by shifting the tail right. */
    if (slot < sa->len) {
        memmove(&sa->nodes[slot + 1], &sa->nodes[slot], (sa->len - slot) * sizeof(sortedArrayNode));
    }

    sa->nodes[slot].value = value;
    sa->len++;
}

/* Remove and return the element that sorts first, or NULL when empty.
 * Ownership passes back to the caller. */
void *sortedArrayExtractMin(sortedArray *sa) {
    if (sa->len == 0) return NULL;
    sa->len--;
    return sa->nodes[sa->len].value;
}

/* Element at the given position, or NULL when idx is out of range. */
void *sortedArrayGet(sortedArray *sa, unsigned long idx) {
    if (idx >= sa->len) return NULL;
    return sa->nodes[idx].value;
}
