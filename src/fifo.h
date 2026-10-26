/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* A space/time efficient First-In, First-Out queue of pointers.
 *
 * Implemented with an unrolled single-linked list, the implementation packs multiple pointers into
 * a single block.  This increases space efficiency and cache locality over the Valkey `list` for the
 * purpose of a simple fifo.
 *
 * IMPORTANT: NULL fifo are NOT supported by these APIs.
 * All functions expect a valid fifo created by fifoCreate().
 * Passing NULL to any function will result in undefined behavior (likely a crash).
 *
 * STORED VALUES: The fifo supports arbitrary void* values with the following guarantees:
 * - All bit patterns are preserved exactly as provided (no modifications)
 * - NULL pointers can be stored as items (distinct from NULL fifo above)
 * - Integer values (e.g., intptr_t) can be stored by casting to void*
 * - Pointers with custom bit flags/tags are fully supported
 * The implementation makes no assumptions about the stored values and keeps them intact.
 */

#ifndef __FIFO_H_
#define __FIFO_H_

#include <stdbool.h>

typedef struct fifo fifo;

/* Create a new fifo. */
fifo *fifoCreate(void);

/* Push an item onto the end of the fifo. */
void fifoPush(fifo *q, void *ptr);

/* Look at the first item in the fifo (without removing it).
 * Returns true if an item exists. If false, `item` is undefined. */
bool fifoPeek(fifo *q, void **item);

/* Return and remove the first item from the fifo.
 * Returns true if an item exists. If false, `item` is not updated. */
bool fifoPop(fifo *q, void **item);

/* Return the number of items in the fifo. */
long fifoLength(fifo *q);

/* Release the fifo.
 * NOTE: this does not free items which may be referenced by inserted pointers. */
void fifoRelease(fifo *q);

/* Joins the fifo "other" to the end of "q".  "other" becomes empty, but remains valid.
 * This is an O(1) operation.
 *
 * Use case: Appending items from one fifo to another existing fifo.
 * Example: fifoJoin(destination_q, source_q);
 *          // destination_q now contains all items from both fifos
 *          // source_q is now empty but still valid */
void fifoJoin(fifo *q, fifo *other);

/* Copy all of the items into a new fifo (emptying the original)
 * Returns a new fifo, containing all of the items from "q".  "q" remains valid, but becomes empty.
 * This is an O(1) operation.
 *
 * Use case: Retrieving/removing all items from a fifo as a batch.
 * This is cleaner and more descriptive than using fifoJoin with an empty fifo when the intent
 * is to extract items rather than merge fifos.
 * Example: fifo *batch = fifoPopAll(source_q);
 *          // batch contains all items, source_q is now empty
 *          // Process batch, then fifoRelease(batch) when done */
fifo *fifoPopAll(fifo *q);

#endif
