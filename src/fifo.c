/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* FIFO - A high-performance First-In, First-Out queue implementation */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "fifo.h"
#include "serverassert.h"
#include "zmalloc.h"

/* Items per block was chosen as 7 because, including the next pointer, this gives us a nice even
 * 64-byte block.  Conveniently, the index values 0..6 will fit nicely in the 3 unused bits at the
 * bottom of the next pointer, creating a very compact block. */
#define ITEMS_PER_BLOCK 7
static const uintptr_t IDX_MASK = 0x0007;


/* The FifoBlock contains up to 7 items (pointers).  When compared with adlist, this results in
 * roughly 60% memory reduction and 7x fewer memory allocations.  Memory reduction is guaranteed
 * with 5+ items.
 *
 * In each block, there are 7 slots for item pointers (pointers to the caller's FIFO item).
 *  We need to keep track of the first & last slot used.  Contextually, we will only need
 *  a single index - either the first slot used or the last slot used.  Based on context,
 *  we can determine what is needed.
 *
 * Blocks are linked together in a chain.  If the list is empty, there are no blocks.
 *  For non-empty lists, we will either have a single block OR a chain of blocks.
 *
 * For a SINGLE BLOCK containing (for example) 4 items, the layout looks like this:
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *  SINGLE BLOCK:  | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  |
 *                 |  item  |  item  |  item  |  item  |   -    |   -    |   -    | lastIdx|
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *                                                ^
 *                                             lastIdx (3)
 *  In single blocks, the items are always shifted so that the first item is in slot 0.
 *  We need to keep track of the lastIdx so that we will know where to push the next item.
 *  The last index is stored in the final 3 bits of the (unused) next pointer
 *
 * When MULTIPLE BLOCKS are chained together, items will be popped from the first block, and
 *  pushed onto the last block.  All blocks in the middle are full.  In the first block, we keep
 *  the firstIdx (so we know where to pop) ... on the last block, we keep lastIdx (so we know
 *  where to push).
 *
 * Example FIRST BLOCK with 2 items remaining:
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *  FIRST BLOCK:   | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  |
 *                 |   -    |   -    |   -    |   -    |   -    |  item  |  item  |firstIdx|
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *                                                                  ^
 *                                                              firstIdx (5)
 * Example LAST BLOCK with 3 items pushed so far:
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *  LAST BLOCK:    | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  |
 *                 |  item  |  item  |  item  |   -    |   -    |   -    |   -    | lastIdx|
 *                 +--------+--------+--------+--------+--------+--------+--------+--------+
 *                                        ^
 *                                    lastIdx (2)
 */
typedef struct fifoBlock fifoBlock;

struct fifoBlock {
    void *items[ITEMS_PER_BLOCK];
    union {
        /* The last 3 bits of a pointer to a block allocated by malloc must always be zero as a
         *  minimum of 8-byte alignment is required for all such blocks.  These bits are used as
         *  an index into the block indicating the first or last item in the block, depending on
         *  context.
         *
         * This UNION overlays a pointer with an integral value.  This allows us to look at the
         *  pointer OR the integer without casting - but they use the same memory.
         *
         * If there is MORE THAN ONE block in the chain, the first block has a pointer/index that
         *  looks like this.  However, if there is only a single block, it looks like the LAST block.
         *   +-----------------------------------------------------------+
         *   |                 next pointer                   | firstIdx |
         *   |                  (61 bits)                     | (3 bits) |
         *   +-----------------------------------------------------------+
         *     * The next pointer is only valid after zeroing out the last 3 bits.
         *     * "lastIdx" is implied to be 6 (because there are additional blocks).
         *     * "firstIdx" represents the first filled index (0..6).  POP occurs here.
         *
         * Any blocks in the middle of the chain have a regular pointer like this:
         *   +-----------------------------------------------------------+
         *   |                 next pointer                   |    0*    |
         *   |                  (61 bits)                     | (3 bits) |
         *   +-----------------------------------------------------------+
         *     * The next pointer is valid as-is
         *     * "lastIdx" is implied to be 6 in all middle blocks.
         *     * "firstIdx" is implied to be 0 in all middle blocks.
         *     * NOTE: In middle blocks, the index bits(0) are really still the firstIdx value.
         *             When Fifo's are joined, the O(1) operation may result in a partially
         *             full middle block.  In this case, the items are "right-justified" and
         *             firstIdx indicates where the items start.
         *
         * The last (or only) block in the chain contains only the lastIndex, the pointer is unused.
         *   +-----------------------------------------------------------+
         *   |                      0                         | lastIdx  |
         *   |                  (61 bits)                     | (3 bits) |
         *   +-----------------------------------------------------------+
         *     * The next pointer is unused and guaranteed NULL.
         *     * "lastIdx" represents the last filled index (0..6).
         *     * "firstIdx" is implied to be zero on the last (or only) block.
         */
        uintptr_t last_or_first_idx;
        fifoBlock *next;
    } u;
};

struct fifo {
    long length; /* Number of items */
    fifoBlock *first;
    fifoBlock *last;
};


fifo *fifoCreate(void) {
    fifo *q = zmalloc(sizeof(fifo));
    q->length = 0;
    q->first = q->last = NULL;
    return q;
}


void fifoPush(fifo *q, void *ptr) {
    if (q->first == NULL) {
        /* fifo was empty - create block */
        assert(q->last == NULL && q->length == 0);
        q->last = q->first = zmalloc(sizeof(fifoBlock));
        q->last->u.last_or_first_idx = 0; /* Item 0 is the last item in this block */
        q->last->items[0] = ptr;
    } else {
        int lastIdx = q->last->u.last_or_first_idx; /* pointer portion is 0 on last (or only) block */
        assert(lastIdx < ITEMS_PER_BLOCK);

        if (lastIdx < ITEMS_PER_BLOCK - 1) {
            /* If the last block has space, just add the item */
            q->last->items[lastIdx + 1] = ptr;
            q->last->u.last_or_first_idx++;
        } else {
            /* Otherwise, last block is full - add a new block */
            fifoBlock *newblock = zmalloc(sizeof(fifoBlock));
            newblock->u.last_or_first_idx = 0;
            newblock->items[0] = ptr;
            q->last->u.next = newblock; /* overwrites the index, setting it to 0 */
            q->last = newblock;
        }
    }

    q->length++;
}


bool fifoPeek(fifo *q, void **item) {
    if (q->length == 0) return false;
    int firstIdx = (q->first == q->last) ? 0 : q->first->u.last_or_first_idx & IDX_MASK;
    *item = q->first->items[firstIdx];
    return true;
}


bool fifoPop(fifo *q, void **item) {
    if (q->length == 0) return false;

    if (q->first == q->last) {
        /* With only 1 block, POP occurs at index 0 and items 1..6 are shifted.
         *
         * Example SINGLE BLOCK with 4 items BEFORE pop:
         *                 +--------+--------+--------+--------+--------+--------+--------+--------+
         *  BEFORE POP:    | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  |
         *                 |  item  |  item  |  item  |  item  |   -    |   -    |   -    | lastIdx|
         *                 +--------+--------+--------+--------+--------+--------+--------+--------+
         *                    ^                                   ^
         *                  POP here                           lastIdx (3)
         *
         * Example SINGLE BLOCK with 3 items AFTER pop (items shifted left):
         *                 +--------+--------+--------+--------+--------+--------+--------+--------+
         *  AFTER POP:     | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  |
         *                 |  item  |  item  |  item  |   -    |   -    |   -    |   -    | lastIdx|
         *                 +--------+--------+--------+--------+--------+--------+--------+--------+
         *                                        ^
         *                                     lastIdx (2)
         *
         * Items are shifted left to keep them left-justified in the single block.
         * This avoids needing to allocate a new block when pushing more items.
         */
        *item = q->last->items[0];

        int lastIdx = q->last->u.last_or_first_idx; /* pointer portion is 0 on last (or only) block */
        assert(lastIdx < ITEMS_PER_BLOCK);

        if (lastIdx > 0) {
            /* With only 1 block, shift the items rather than eventually needing new block.
             * Use memmove to shift pointers to the left by 1 */
            memmove(q->last->items, q->last->items + 1, lastIdx * sizeof(q->last->items[0]));
            q->last->u.last_or_first_idx--; /* Decrement the last index */
        } else {
            /* Just finished the only block.  Release it. */
            zfree(q->last);
            q->first = q->last = NULL;
        }
    } else {
        /* With more than 1 block, POP occurs at firstIdx, and firstIdx is incremented. */
        int firstIdx = q->first->u.last_or_first_idx & IDX_MASK;
        *item = q->first->items[firstIdx];

        if (firstIdx < ITEMS_PER_BLOCK - 1) {
            /* Just increment the first index to the next slot. */
            q->first->u.last_or_first_idx++;
        } else {
            /* Finished with this block, move to next */
            q->first->u.last_or_first_idx &= ~IDX_MASK; /* restores the next pointer */
            fifoBlock *next = q->first->u.next;
            zfree(q->first);
            q->first = next;
        }
    }

    q->length--;
    return true;
}


long fifoLength(fifo *q) {
    return q->length;
}


void fifoRelease(fifo *q) {
    if (q->length > 0) {
        fifoBlock *cur = q->first;
        while (cur != NULL) {
            cur->u.last_or_first_idx &= ~IDX_MASK; /* zero out the last 3 bits */
            fifoBlock *next = cur->u.next;
            zfree(cur);
            cur = next;
        }
    }
    zfree(q);
}


/* Overwrites target from source. */
static void overwriteFifoContents(fifo *target, fifo *source) {
    target->length = source->length;
    target->first = source->first;
    target->last = source->last;
    source->length = 0;
    source->first = source->last = NULL;
}


void fifoJoin(fifo *q, fifo *other) {
    /* When joining a fifo onto an existing fifo, we might be left with partially full blocks in the
     * middle of the list.  In the usual case, any blocks in the middle of the list have the index
     * bits set to zero.  This actually represents the firstIdx - which would normally be zero for
     * blocks in the middle of the list.  In the case of joining lists, we allow partially full
     * blocks in the middle, but the values are "right-justified" and the firstIdx is set.
     *
     * To perform the join, we take the current last (or only) block - which is "left-justified" and
     * shift the items so that the block becomes right-justified.  Then the index is corrected,
     * replacing the lastIdx with the firstIdx.
     *
     * Example: Joining two fifos where "q" has 3 items in its last block:
     *
     * BEFORE JOIN - "q" fifo (last block is left-justified):
     *                 +--------+--------+--------+--------+--------+--------+--------+--------+
     *  q->last:       | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  |
     *                 |  item  |  item  |  item  |   -    |   -    |   -    |   -    | lastIdx|
     *                 +--------+--------+--------+--------+--------+--------+--------+--------+
     *                                        ^                                           (2)
     *
     * BEFORE JOIN - "other" fifo (first block is right-justified if multiple blocks):
     *                 +--------+--------+--------+--------+--------+--------+--------+--------+
     *  other->first:  | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  |
     *                 |   -    |   -    |   -    |   -    |  item  |  item  |  item  |firstIdx|
     *                 +--------+--------+--------+--------+--------+--------+--------+--------+
     *                                                         ^                          (4)
     *
     * AFTER JOIN - q's last block is shifted right and linked to other:
     *                 +--------+--------+--------+--------+--------+--------+--------+--------+
     *  (was q->last)  | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  |
     *  now middle:    |   -    |   -    |   -    |   -    |  item  |  item  |  item  |firstIdx| --+
     *                 +--------+--------+--------+--------+--------+--------+--------+--------+   |
     *                                                         ^                          (4)      |
     *                                                                                             |
     *                    +------------------------------------------------------------------------+
     *                    |
     *                    v
     *                 +--------+--------+--------+--------+--------+--------+--------+--------+
     *  other->first:  | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 | slot 6 | next/  |
     *  (now linked):  |   -    |   -    |   -    |   -    |  item  |  item  |  item  |firstIdx|
     *                 +--------+--------+--------+--------+--------+--------+--------+--------+
     *                                                         ^                          (4)
     *                                                         |
     *                                                     firstIdx
     *
     * The shift ensures q's last block becomes a valid middle block (right-justified with firstIdx).
     *
     * The "other" list maintains its structure when appended:
     * - If "other" has a single block (left-justified), it becomes q's new last block
     * - If "other" has multiple blocks, its first block (right-justified if partial) becomes
     *   a middle block, and its last block becomes q's new last block
     * This is essentially a "pop all from other into q" operation that preserves invariants.
     */
    if (other->length == 0) return;

    if (q->length == 0) {
        /* If "q" is empty, it's a simple operation. */
        overwriteFifoContents(q, other);
        return;
    }

    if (other->length < ITEMS_PER_BLOCK) {
        /* In the case of a short "other" fifo, move each item.  This prevents creation of a string
         * of half-empty blocks if fifoJoin is repeatedly used on small fifos. */
        void *value;
        while (fifoPop(other, &value)) fifoPush(q, value);
        return;
    }

    fifoBlock *curLast = q->last;
    int lastIdx = curLast->u.last_or_first_idx;
    /* Shift the items to the right in the last block if it is partially full */
    int shift = (ITEMS_PER_BLOCK - 1) - lastIdx;
    if (shift > 0) {
        memmove(q->last->items + shift, q->last->items, (lastIdx + 1) * sizeof(q->last->items[0]));
    }

    /* Now fix up the next pointer to point to the next block */
    curLast->u.next = other->first;
    curLast->u.last_or_first_idx += shift; /* Mask on the firstIdx for the shifted block */

    /* Finally, clean up the main list structures */
    q->length += other->length;
    q->last = other->last;
    other->length = 0;
    other->first = other->last = NULL;
}


fifo *fifoPopAll(fifo *q) {
    fifo *newQ = zmalloc(sizeof(fifo));
    overwriteFifoContents(newQ, q);
    return newQ;
}
