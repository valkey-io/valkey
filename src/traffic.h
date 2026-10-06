#ifndef TRAFFIC_H
#define TRAFFIC_H

#include <stdbool.h>
#include <stdint.h>

#include "sds.h"

/*
 * Server-side per-key traffic tracking. The weighted Space-Saving manager and
 * its frozen windows live in space_saving.{c,h}; this module (traffic.c) is the
 * policy layer around it: how many estimated bytes an access moves, the
 * sampling and enable configuration, config wiring, and the TRAFFIC commands.
 * Independent of hot-key detection (hotkeys.{c,h}) — each may be enabled,
 * sampled and windowed on its own.
 */

typedef struct serverObject robj;

/* Config callbacks (wired from config.c). */
int trafficSamplingCallback(const char **err);
int trafficTopKCallback(const char **err);
int trafficWindowCallback(const char **err);

/* Is traffic tracking currently enabled (traffic-top-k > 0)? */
bool trafficEnabled(void);
/* Append the INFO "traffic" section fields (the caller emits the header). */
sds genTrafficInfoString(sds info);
/* Create the manager at server startup if tracking is enabled. */
void trafficInit(void);
/* Periodic maintenance (call from databasesCron): freeze elapsed windows on time. */
void trafficCron(void);

/* Drop tracked keys: all, or scoped to a cluster slot / database. */
void trafficPurgeAll(void);
void trafficPurgeSlot(int slot);
void trafficPurgeDb(int dbid);

/* Charge a sampled read of `key` in database `dbid` whose lookup returned `val`
 * (NULL on a miss — nothing was read, so no bytes are charged). `lookup_flags`
 * are the LOOKUP_* flags of the lookup; write lookups are charged by
 * trafficRecordSetKey instead, with the value actually written. */
void trafficRecordLookup(robj *key, int dbid, int lookup_flags, robj *val);
/* Charge a sampled write of `key` in database `dbid` storing value `val` (the
 * setKey path: new keys and overwrites both carry the written value). */
void trafficRecordSetKey(robj *key, int dbid, robj *val);

#endif /* TRAFFIC_H */
