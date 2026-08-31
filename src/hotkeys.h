#ifndef HOTKEYS_H
#define HOTKEYS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Server-side hot key detection. The Space-Saving algorithm, its frozen-window
 * manager and the tracked (key, db) item live in space_saving.{c,h}; this module
 * (hotkeys.c) is the policy layer around it: what counts as a recordable access,
 * the sampling and enable configuration, config wiring, and the HOTKEYS
 * commands.
 */

typedef struct serverObject robj;

/* Config callbacks (wired from config.c). */
int hotKeySamplingCallback(const char **err);
int hotKeyTopKCallback(const char **err);
int hotKeyWindowCallback(const char **err);

/* Is hot-key detection currently enabled (hotkeys-top-k > 0)? */
bool hotkeyEnabled(void);
/* Number of sampled observations in the last completed window (N). */
uint64_t hotkeyLastWindowSamples(void);
/* Real duration of the last completed window, in microseconds. */
uint64_t hotkeyLastWindowDurationUs(void);
/* Create the manager at server startup if detection is enabled. */
void hotkeyInit(void);
/* Periodic maintenance (call from serverCron): freeze elapsed windows on time. */
void hotkeyCron(void);

/* Drop tracked keys: all, or scoped to a cluster slot / database. */
void hotkeyPurgeAll(void);
void hotkeyPurgeSlot(int slot);
void hotkeyPurgeDb(int dbid);

/* Charge a sampled access of `key` in database `dbid` to hot-key detection.
 * Both apply the whole policy themselves (enabled, which activity counts,
 * sampling), so callers in the data path need no hot-key knowledge:
 *  - Lookup: `lookup_flags` are the LOOKUP_* flags of the lookup.
 *  - Delete: `del_flags` are the DB_FLAG_* deletion reasons. */
void hotkeyRecordLookup(robj *key, int dbid, int lookup_flags);
void hotkeyRecordDelete(robj *key, int dbid, int del_flags);

#endif /* HOTKEYS_H */
