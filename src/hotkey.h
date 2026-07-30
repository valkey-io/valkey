#ifndef HOTKEY_H
#define HOTKEY_H

/*
 * Server-side hot key detection. The Space-Saving algorithm and its
 * frozen-window manager are generic and live in space_saving.{c,h}; this
 * module (hotkey.c) is the Valkey-specific layer: the (key, db) item type, the
 * sampling/enable policy, config wiring, and the HOTKEYS commands.
 */

typedef struct serverObject robj;

/* Config callbacks (wired from config.c). */
int hotKeySamplingCallback(const char **err);
int hotKeyTopKCallback(const char **err);
int hotKeyWindowCallback(const char **err);

/* Is hot-key detection currently enabled (sampling percentage > 0)? */
int hotkeyEnabled(void);
/* Create the manager at server startup if detection is enabled. */
void hotkeyInit(void);

/* Drop tracked keys: all, or scoped to a cluster slot / database. */
void hotkeyPurgeAll(void);
void hotkeyPurgeSlot(int slot);
void hotkeyPurgeDb(int dbid);

/* Record one sampled access of `key` in database `dbid`. */
void recordHotKeySample(robj *key, int dbid);

#endif /* HOTKEY_H */
