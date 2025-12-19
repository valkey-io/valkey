#ifndef _ENTRY_H_
#define _ENTRY_H_

#include "sds.h"
#include <stdbool.h>

/*-----------------------------------------------------------------------------
 * Entry
 *----------------------------------------------------------------------------*/

/*
 * An "entry" is a field/value sds pair, with an optional expiration time.  The
 * entry is used as part of the HASH datatype, and supports hash field expiration.
 */

typedef struct _entry entry;

/* The maximum allocation size we want to use for entries with embedded
 * values. */
#define EMBED_VALUE_MAX_ALLOC_SIZE 128

/* Returns the field string (sds) from the entry. */
sds entryGetField(const entry *entry);

/* Returns the value string (sds) from the entry. */
char *entryGetValue(const entry *entry, size_t *len);

/* Gets the expiration timestamp (UNIX time in milliseconds). */
long long entryGetExpiry(const entry *entry);

/* Returns true if the entry has an expiration timestamp set. */
bool entryHasExpiry(const entry *entry);

/* Returns true if the entry value is externalized. */
bool entryHasStringRef(const entry *entry);

/* Sets the expiration timestamp. */
entry *entrySetExpiry(entry *entry, long long expiry);

/* Returns true if the entry is expired compared to current system time (commandTimeSnapshot). */
bool entryIsExpired(entry *entry);

/* Frees the memory used by the entry (including field/value). */
void entryFree(entry *entry);

/* Creates a new entry with the given field, value, and optional expiry. */
entry *entryCreate(const_sds field, sds value, long long expiry);
/* Sets the entry's value to a string reference object.
 * The reference points to the provided `buf` but does not assume ownership.
 * An external mechanism must handle the eventual memory deallocation of `buf`. */
entry *entryUpdateAsStringRef(entry *entry, const char *buf, size_t len, long long expiry);

/* Updates the value and/or expiry of an existing entry.
 * In case value is NULL, will use the existing entry value.
 * In case expiry is EXPIRE_NONE, will use the existing entry expiration time. */
entry *entryUpdate(entry *entry, sds value, long long expiry);

/* Returns the total memory used by the entry (in bytes). */
size_t entryMemUsage(entry *entry);

/* Defragments the entry and returns the new pointer (if moved). */
entry *entryDefrag(entry *entry, void *(*defragfn)(void *), sds (*sdsdefragfn)(sds));

/* Advises allocator to dismiss memory used by entry. */
void entryDismissMemory(entry *entry);

/* Internal used for debug. No need to use this function except in tests */
bool entryHasEmbeddedValue(const entry *entry);

#endif
