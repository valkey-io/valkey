#ifndef VOLATILESET_H
#define VOLATILESET_H

#include <stddef.h>
#include <stdbool.h>

#include "hashtable.h"
#include "rax.h"
#include "sds.h"
#include "monotonic.h" /* for mstime_t*/

/*
 *-----------------------------------------------------------------------------
 * Volatile Set - Adaptive, Expiry-aware Set Structure
 *-----------------------------------------------------------------------------
 *
 * The `vset` is a dynamic, memory-efficient container for managing
 * entries with expiry semantics. It is designed to efficiently track entries
 * that expire at varying times and scales to large sets by adapting its internal
 * representation as it grows or shrinks.
 *
 *-----------------------------------------------------------------------------
 * Public API
 *-----------------------------------------------------------------------------
 *
 * Create/Free:
 *     vsetInit(vset *set) - used in order to initialize a new vset.
 *     void vsetClear(vset *set) - used in order to empty all the data in a vset.
 *     void vsetRelease(vset *set) - just like vsetClear, but also release the set itself so it will become unusable.
 *                                   and will require a new call to vsetInit in order to continue using the set.
 *    Example:
 *      vset set;
 *      vsetInit(&set);
 *      // add some elements to the vset
 *      vsetClear(&set);
 *      // verify the set is empty:
 *      assert(vsetIsEmpty(&set));
 *
 * Mutation:
 *     bool vsetAddEntry(vset *set, vsetGetExpiryFunc getExpiry, void *entry) - used in order to insert a new entry into the set.
 *     The API also make use of the provided getExpiry function in order to compare the 'entry' expiration time of the other existing
 *     entries in the set.
 *
 *     bool vsetRemoveEntry(vset *set, vsetGetExpiryFunc getExpiry, void *entry) - used in order to remove and entry from the set.
 *
 *     bool vsetUpdateEntry(vset *set, vsetGetExpiryFunc getExpiry, void *old_entry,
 *                                void *new_entry, long long old_expiry,
 *                                long long new_expiry) - is used in order to update an existing entry in the set.
 *     Note that the implementation assumes the 'old_entry' might not point to a valid memory location, thus it require that the 'old_expiry'
 *     is provided and matches the old entry expiration time.
 *
 * Expiry Retrieval/Removal:
 *     long long vsetEstimatedEarliestExpiry(vset *set, vsetGetExpiryFunc getExpiry) - will return an estimation to the lowest expiry time of
 *     the entries which currently exists in the set. Because of the semi-sorted ordering this implementation is using, the returned value MIGHT not be the 'real' minimum
 *     but rather some value which is the maximum among a group of entries which are all close or equal to the 'real' minimum.
 *
 *     size_t vsetRemoveExpired(vset *set, vsetGetExpiryFunc getExpiry, vsetExpiryFunc expiryFunc, mstime_t now, size_t max_count, void *ctx) - can be used
 *     in order to remove up to max_count entries from the vset. The removed entries will all satisfy the condition that their expiration time is smaller than the provided now.
 *     Note that there are no guarantees about the order to the entries.
 *
 * Utilities:
 *     bool vsetIsEmpty(vset *set) - used in order to check if a given set has any entries.
 *
 * Iteration:
 *     void vsetInitIterator(vset *set, vsetIterator *it) - used to initialize a new vset iterator.
 *     bool vsetNext(vsetIterator *it, void **entryptr) - used to iterate to the next element. Will return false if there are no more elements.
 *     void vsetResetIterator(vsetIterator *it) - used in order to reset the iterator at the end of the iteration.
 *
 * Note that the vset iterator is NOT safe, Meaning you should not change the set while iterating it. Adding entries and/or removing entries
 * can result in unexpected behavior.! */

/* Return the absolute expiration time in milliseconds for the provided entry */
typedef long long (*vsetGetExpiryFunc)(const void *entry);
/* Callback to be optionally provided to vsetPopExpired. when item is removed from the vset this callback will also be applied. */
typedef int (*vsetExpiryFunc)(void *entry, void *ctx);
// vset is just a pointer to a bucket
typedef void *vset;

typedef uint8_t vsetIterator[560];

bool vsetAddEntry(vset *set, vsetGetExpiryFunc getExpiry, void *entry);
bool vsetRemoveEntry(vset *set, vsetGetExpiryFunc getExpiry, void *entry);
bool vsetUpdateEntry(vset *set, vsetGetExpiryFunc getExpiry, void *old_entry, void *new_entry, long long old_expiry, long long new_expiry);
bool vsetIsEmpty(vset *set);
void vsetInitIterator(vset *set, vsetIterator *it);
bool vsetNext(vsetIterator *it, void **entryptr);
void vsetResetIterator(vsetIterator *it);
void vsetInit(vset *set);
void vsetClear(vset *set);
void vsetRelease(vset *set);
bool vsetIsValid(vset *set);
long long vsetEstimatedEarliestExpiry(vset *set, vsetGetExpiryFunc getExpiry);
size_t vsetRemoveExpired(vset *set, vsetGetExpiryFunc getExpiry, vsetExpiryFunc expiryFunc, mstime_t now, size_t max_count, void *ctx);
size_t vsetMemUsage(vset *set);
size_t vsetScanDefrag(vset *set, size_t cursor, void *(*defragfn)(void *), int (*defragRaxNode)(raxNode **));

#endif
