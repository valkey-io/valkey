#ifndef _ENTRY_H_
#define _ENTRY_H_

#include "sds.h"
#include <stdbool.h>

/*-----------------------------------------------------------------------------
 * Entry
 *----------------------------------------------------------------------------*/

/*
 * The entry pointer is the field `sds`. We encode the entry layout type
 * in the SDS header.
 *
 * An entry represents a key–value pair with an optional expiration timestamp.
 * The pointer of type `entry *` always points to the VALUE `sds`.
 *
 * Layout 1: Embedded Field and Value (Compact Form)
 *
 *   +-------------------+-------------------+-------------------+
 *   | Expiration (opt)  | Field (sds)       | Value (sds)       |
 *   | 8 bytes (int64_t) | "field" + header  | "value" + header  |
 *   +-------------------+-------------------+-------------------+
 *                                   ^
 *                                   |
 *                             entry pointer
 *
 * - Both field and value are small and embedded.
 * - The expiration is stored just before the first sds.
 *
 *
 * Layout 2: Pointer-Based Value (Large Values)
 *
 *   +-------------------+-------------------+------------------+
 *   | Expiration (opt)  | Value pointer     | Field (sds)      |
 *   | 8 bytes (int64_t) | 8 bytes (void *)  | "field" + header |
 *   +-------------------+-------------------+------------------+
 *                                           ^
 *                                           |
 *                                           entry pointer
 *
 * - The value is stored separately via a pointer.
 * - Used for large value sizes. */
typedef void entry;

/* The maximum allocation size we want to use for entries with embedded
 * values. */
#define EMBED_VALUE_MAX_ALLOC_SIZE 128

/* Returns the field string (sds) from the entry. */
sds entryGetField(const entry *entry);

/* Returns the value string (sds) from the entry. */
sds entryGetValue(const entry *entry);

/* Sets or replaces the value string in the entry. May reallocate and return a new pointer. */
entry *entrySetValue(entry *entry, sds value);

/* Gets the expiration timestamp (UNIX time in milliseconds). */
long long entryGetExpiry(const entry *entry);

/* Returns true if the entry has an expiration timestamp set. */
bool entryHasExpiry(const entry *entry);

/* Sets the expiration timestamp. */
entry *entrySetExpiry(entry *entry, long long expiry);

/* Returns true if the entry is expired compared to current system time (commandTimeSnapshot). */
bool entryIsExpired(entry *entry);

/* Frees the memory used by the entry (including field/value). */
void entryFree(entry *entry);

/* Creates a new entry with the given field, value, and optional expiry. */
entry *entryCreate(const_sds field, sds value, long long expiry);

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
bool entryHasEmbeddedValue(entry *entry);

#endif
