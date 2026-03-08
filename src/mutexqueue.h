/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * A thread-safe queue, protected by a mutex.
 *
 * Supports:
 *   - Adding an item to the end of the mutexQueue
 *   - Adding a list of items (fifo) to the end of the mutexQueue
 *   - Insertion of a priority item at the beginning of the mutexQueue (but after existing priority items)
 *   - Removing an item from the beginning of the mutexQueue
 *   - Removing ALL items as (as new fifo) from the mutexQueue
 *   - Synchronous waiting on the mutexQueue for new items
 *
 * Priority Use Case:
 * The priority feature provides a 2-level priority system (priority vs normal) without
 * requiring separate fifos with their own mutexes and condition variables. This simplifies
 * synchronization when you need to occasionally process urgent items ahead of routine work.
 *
 * Example: In a background worker thread processing file operations, you might want to
 * prioritize critical shutdown tasks or urgent fsync operations over routine lazy-free jobs.
 * Priority items are processed in FIFO order, followed by normal items in FIFO order.
 *
 * Implementation: Uses two internal FIFOs (priority_fifo and normal_fifo). Items are always
 * popped from priority_fifo first.
 *
 * The caller is responsible for memory management for items in the mutexQueue.
 */

#ifndef __MUTEXQUEUE_H
#define __MUTEXQUEUE_H

#include <stdbool.h>
#include "fifo.h"

/* The mutexQueue is an opaque structure.  */
typedef struct mutexQueue mutexQueue;

/* Create an empty mutexQueue. */
mutexQueue *mutexQueueCreate(void);

/* Release an empty mutexQueue. */
void mutexQueueRelease(mutexQueue *theQueue);

/* Number of items in the mutexQueue. */
unsigned long mutexQueueLength(mutexQueue *theQueue);

/* Insert a priority item at the beginning of the mutexQueue (but after existing priority items). */
void mutexQueuePushPriority(mutexQueue *theQueue, void *value);

/* Insert an item at the end of the mutexQueue. */
void mutexQueueAdd(mutexQueue *theQueue, void *value);

/* Insert multiple items (from a fifo) to the end of the mutexQueue. */
void mutexQueueAddMultiple(mutexQueue *theQueue, fifo *valueFifo);

/* Retrieves the first item off the mutexQueue (or NULL if mutexQueue is empty). */
void *mutexQueuePop(mutexQueue *theQueue, bool blocking);

/* Retrieves all items from the mutexQueue as a fifo (or NULL if the mutexQueue is empty). */
fifo *mutexQueuePopAll(mutexQueue *theQueue, bool blocking);

#endif
