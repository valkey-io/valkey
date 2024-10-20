#ifndef HASHSET_H
#define HASHSET_H

/* Hash table implementation.
 *
 * This is a cache-friendly hash table implementation. For details about the
 * implementation and documentation of functions, se comments in hashset.c.
 *
 * The elements in a hashset are of a user-defined type, but an element needs to
 * contain a key. It can represent a key-value entry, or it can be just a key,
 * if set semantics are desired.
 *
 * Terminology:
 *
 * hashset
 *         An instance of the data structure.
 *
 * element
 *         An element in the hashset. This may be of the same type as the key,
 *         or a struct containing a key and other fields.
 * key
 *         The part of the element used for looking the element up in the hashset.
 *         May be the entire element or a struct field within the element.
 *
 * type
 *         A struct containing callbacks, such as hash function, key comparison
 *         function and how to get the key in an element.
 */

#include "fmacros.h"
#include <stddef.h>
#include <stdint.h>

/* --- Opaque types --- */

typedef struct hashset hashset;

/* --- Non-opaque types --- */

/* The hashsetType is a set of callbacks for a hashset. All callbacks are
 * optional. With all callbacks omitted, the hashset is effectively a set of
 * pointer-sized integers. */
typedef struct {
    /* If the type of an element is not the same as the type of a key used for
     * lookup, this callback needs to return the key within an element. */
    const void *(*elementGetKey)(const void *element);
    /* Hash function. Defaults to hashing the bits in the pointer, effectively
     * treating the pointer as an integer. */
    uint64_t (*hashFunction)(const void *key);
    /* Compare function, returns 0 if the keys are equal. Defaults to just
     * comparing the pointers for equality. */
    int (*keyCompare)(hashset *s, const void *key1, const void *key2);
    /* Callback to free an element when it's overwritten or deleted.
     * Optional. */
    void (*elementDestructor)(hashset *s, void *element);
    /* Callback to control when resizing should be allowed. */
    int (*resizeAllowed)(size_t moreMem, double usedRatio);
    /* Invoked at the start of rehashing. Both tables are already created. */
    void (*rehashingStarted)(hashset *s);
    /* Invoked at the end of rehashing. Both tables still exist and are cleaned
     * up after this callback. */
    void (*rehashingCompleted)(hashset *s);
    /* Allow a hashset to carry extra caller-defined metadata. The extra memory
     * is initialized to 0. */
    size_t (*getMetadataSize)(void);
    /* Flag to disable incremental rehashing */
    unsigned instant_rehashing : 1;
} hashsetType;

typedef enum {
    HASHSET_RESIZE_ALLOW = 0,
    HASHSET_RESIZE_AVOID,
    HASHSET_RESIZE_FORBID,
} hashsetResizePolicy;

typedef void (*hashsetScanFunction)(void *privdata, void *element);

/* Constants */
#define HASHSET_BUCKET_SIZE 64 /* bytes */

/* Scan flags */
#define HASHSET_SCAN_EMIT_REF (1 << 0)
#define HASHSET_SCAN_SINGLE_STEP (1 << 2)

typedef struct {
    hashset *hashset;
    long index;
    int table;
    int posInBucket;
    uint64_t safe : 1;
    /* unsafe iterator fingerprint for misuse detection. */
    uint64_t fingerprint : 63;
} hashsetIterator;

typedef struct hashsetStats {
    int htidx;
    unsigned long buckets;       /* num buckets */
    unsigned long maxChainLen;   /* probing chain length */
    unsigned long totalChainLen; /* buckets with probing flag */
    unsigned long htSize;        /* buckets * positions-per-bucket */
    unsigned long htUsed;        /* num elements */
    unsigned long *clvector;     /* (probing-)chain length vector; element i is
                                  * the number of probing chains of length i. */
} hashsetStats;

/* --- Prototypes --- */

/* Hash function (global seed) */
void hashsetSetHashFunctionSeed(const uint8_t *seed);
uint8_t *hashsetGetHashFunctionSeed(void);
uint64_t hashsetGenHashFunction(const char *buf, size_t len);
uint64_t hashsetGenCaseHashFunction(const char *buf, size_t len);

/* Global resize policy */
void hashsetSetResizePolicy(hashsetResizePolicy policy);

/* Hashset instance */
hashset *hashsetCreate(hashsetType *type);
void hashsetRelease(hashset *s);
void hashsetEmpty(hashset *s, void(callback)(hashset *));
hashsetType *hashsetGetType(hashset *s);
void *hashsetMetadata(hashset *s);
size_t hashsetSize(hashset *s);
size_t hashsetBuckets(hashset *s);
size_t hashsetProbeCounter(hashset *s, int table);
size_t hashsetMemUsage(hashset *s);
void hashsetPauseAutoShrink(hashset *s);
void hashsetResumeAutoShrink(hashset *s);
int hashsetIsRehashing(hashset *s);
int hashsetIsRehashingPaused(hashset *s);
void hashsetRehashingInfo(hashset *s, size_t *from_size, size_t *to_size);
int hashsetRehashMicroseconds(hashset *s, uint64_t us);
int hashsetExpand(hashset *s, size_t size);
int hashsetTryExpand(hashset *s, size_t size);
int hashsetExpandIfNeeded(hashset *s);
int hashsetShrinkIfNeeded(hashset *s);
hashset *hashsetDefragInternals(hashset *s, void *(*defragfn)(void *));

/* Elements */
int hashsetFind(hashset *s, const void *key, void **found);
void **hashsetFindRef(hashset *s, const void *key);
int hashsetAdd(hashset *s, void *element);
int hashsetAddOrFind(hashset *s, void *element, void **existing);
void *hashsetFindPositionForInsert(hashset *s, void *key, void **existing);
void hashsetInsertAtPosition(hashset *s, void *element, void *position);
int hashsetReplace(hashset *s, void *element);
int hashsetPop(hashset *s, const void *key, void **popped);
int hashsetDelete(hashset *s, const void *key);
void **hashsetTwoPhasePopFindRef(hashset *s, const void *key, void **position);
void hashsetTwoPhasePopDelete(hashset *s, void *position);

/* Iteration & scan */
size_t hashsetScan(hashset *s, size_t cursor, hashsetScanFunction fn, void *privdata, int flags);
void hashsetInitIterator(hashsetIterator *iter, hashset *s);
void hashsetInitSafeIterator(hashsetIterator *iter, hashset *s);
void hashsetResetIterator(hashsetIterator *iter);
hashsetIterator *hashsetCreateIterator(hashset *s);
hashsetIterator *hashsetCreateSafeIterator(hashset *s);
void hashsetReleaseIterator(hashsetIterator *iter);
int hashsetNext(hashsetIterator *iter, void **elemptr);

/* Random elements */
int hashsetRandomElement(hashset *s, void **found);
int hashsetFairRandomElement(hashset *s, void **found);
unsigned hashsetSampleElements(hashset *s, void **dst, unsigned count);

/* Debug & stats */

void hashsetFreeStats(hashsetStats *stats);
void hashsetCombineStats(hashsetStats *from, hashsetStats *into);
hashsetStats *hashsetGetStatsHt(hashset *s, int htidx, int full);
size_t hashsetGetStatsMsg(char *buf, size_t bufsize, hashsetStats *stats, int full);
void hashsetGetStats(char *buf, size_t bufsize, hashset *s, int full);

#endif /* HASHSET_H */
