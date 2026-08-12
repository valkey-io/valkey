/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SORTED_ARRAY_H__
#define __SORTED_ARRAY_H__

/* Collection of opaque elements held in sorted order.
 *
 * Elements are plain void pointers and ordering is supplied by the caller as a
 * comparison function, so the collection has no knowledge of what it stores.
 *
 * The array is kept in reverse comparator order, so the element that sorts
 * *first* lives in the last slot.
 *
 *   operation                        cost
 *   -------------------------------  --------------------------------------
 *   peek first-sorting element       O(1), reads the last slot
 *   extract first-sorting element    O(1), drops the last slot, no data moved
 *   read the i-th element            O(1), already in order
 *   enumerate in order               O(N), a straight walk, no sorting needed
 *   insert                           O(log N) compares + one memmove
 */

#include <stddef.h>

typedef int (*sortedArrayCompareFn)(const void *a, const void *b);

typedef struct sortedArrayNode {
    void *value;
} sortedArrayNode;

typedef struct sortedArray {
    sortedArrayNode *nodes;
    unsigned long len;
    unsigned long capacity;
    int (*compare)(const void *, const void *);
    void (*free)(void *ptr);
} sortedArray;

sortedArray *sortedArrayCreate(sortedArrayCompareFn compare);
void sortedArrayRelease(sortedArray *sa);
void sortedArrayEmpty(sortedArray *sa);
void sortedArrayInsert(sortedArray *sa, void *value);
void *sortedArrayExtractMin(sortedArray *sa);
void *sortedArrayPeekMin(sortedArray *sa);
void *sortedArrayGet(sortedArray *sa, unsigned long idx);

#define sortedArrayLen(sa) ((sa)->len)
#define sortedArraySetFreeMethod(sa, m) ((sa)->free = (m))
#define sortedArrayGetFreeMethod(sa) ((sa)->free)

#endif /* __SORTED_ARRAY_H__ */
