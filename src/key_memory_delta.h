/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef KEY_MEMORY_DELTA_H
#define KEY_MEMORY_DELTA_H

#include "server.h"

/* Key memory delta tracking, backing ValkeyModuleEvent_KeyMemoryDelta (see
 * key_memory_delta.c). Keys are only tracked while some module is subscribed
 * to the event, so that callers in the data path pay a single branch when
 * nobody is: the inline functions below only evaluate anything else, such as
 * the key name, past that branch. */

extern int key_memory_delta_listeners;  /* Number of subscribed modules, maintained by module.c. */
extern size_t key_memory_delta_pending; /* Number of tracked keys. */

/* Record the state of a key before it is modified, unless it was already
 * recorded during this execution unit. 'val' is the key's current value, or
 * NULL if the key doesn't exist. Callers check keyMemoryDeltaEnabled() first,
 * like the inline functions below. */
void keyMemoryDeltaTrackSlow(serverDb *db, sds key, robj *val);
void keyMemoryDeltaTrackLookupSlow(serverDb *db, robj *key, robj *val, int flags);
void keyMemoryDeltaTrackResizeSlow(int dbid, robj *val);
void keyMemoryDeltaLoadedSlow(serverDb *db, sds key, robj *val);

static inline bool keyMemoryDeltaEnabled(void) {
    return unlikely(key_memory_delta_listeners != 0);
}

/* See keyMemoryDeltaTrackSlow(), with the key name given as an object. */
static inline void keyMemoryDeltaTrackKey(serverDb *db, robj *key, robj *val) {
    if (keyMemoryDeltaEnabled()) keyMemoryDeltaTrackSlow(db, objectGetVal(key), val);
}

/* See keyMemoryDeltaTrackSlow(), for an existing value embedding its key. */
static inline void keyMemoryDeltaTrackValue(serverDb *db, robj *val) {
    if (keyMemoryDeltaEnabled()) keyMemoryDeltaTrackSlow(db, objectGetKey(val), val);
}

/* Called by lookupKey() for an existing key. */
static inline void keyMemoryDeltaTrackLookup(serverDb *db, robj *key, robj *val, int flags) {
    if (keyMemoryDeltaEnabled()) keyMemoryDeltaTrackLookupSlow(db, key, val, flags);
}

/* Called before code that doesn't modify a value, but resumes the paused
 * rehashing of its hashtables, which may then shrink. 'dbid' is the database
 * the value is expected to be in. */
static inline void keyMemoryDeltaTrackResize(int dbid, robj *val) {
    if (keyMemoryDeltaEnabled()) keyMemoryDeltaTrackResizeSlow(dbid, val);
}

/* Report a key that was just loaded from an RDB file. */
static inline void keyMemoryDeltaLoaded(serverDb *db, sds key, robj *val) {
    if (keyMemoryDeltaEnabled()) keyMemoryDeltaLoadedSlow(db, key, val);
}

/* Whether tracked keys are waiting to be reported by keyMemoryDeltaFlush(). */
static inline bool keyMemoryDeltaHasPending(void) {
    return key_memory_delta_pending != 0;
}

void keyMemoryDeltaFlush(void);
void keyMemoryDeltaDiscard(serverDb **dbarray, int dbnum);
void keyMemoryDeltaSwapDb(serverDb *db1, serverDb *db2);

#endif /* KEY_MEMORY_DELTA_H */
