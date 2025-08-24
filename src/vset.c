#include "vset.h"
#include "rax.h"
#include "endianconv.h"
#include "serverassert.h"
#include "hashtable.h"
#include "util.h"
#include "zmalloc.h"
#include "server.h" // for activeDefragAlloc

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

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
 * Expiry Buckets and Pointer Tagging
 *-----------------------------------------------------------------------------
 *
 * Internally, the `vset` maintains a single `vsetBucket*` pointer,
 * which can point to different types of buckets depending on the number of
 * entries and the needed resolution. The pointer is tagged using the lowest 3 bits:
 *
 *     #define VSET_BUCKET_NONE   -1
 *     #define VSET_BUCKET_SINGLE 0x1ULL  // pointer to single entry (odd ptr)
 *     #define VSET_BUCKET_VECTOR 0x2ULL  // pointer to pointer vector
 *     #define VSET_BUCKET_HT     0x4ULL  // pointer to hashtable
 *     #define VSET_BUCKET_RAX    0x6ULL  // pointer to radix tree
 *
 *     #define VSET_TAG_MASK      0x7ULL
 *     #define VSET_PTR_MASK      (~VSET_TAG_MASK)
 *
 * IMPORTANT!!!! - All entries must have LSB set (i.e., be odd-aligned) to be compatible with !!!!
 * tagging constraints.
 *
 *-----------------------------------------------------------------------------
 * Time Bucket Management
 *-----------------------------------------------------------------------------
 *
 * Entries are grouped into **time buckets** based on their expiry time.
 * Each time bucket represents a window aligned to:
 *
 *     #define VOLATILESET_BUCKET_INTERVAL_MIN  (1 << 4)  // 16ms
 *     #define VOLATILESET_BUCKET_INTERVAL_MAX  (1 << 13) // 8192ms
 *
 * A time bucket key is computed by rounding the expiry timestamp up to the
 * nearest aligned window using `get_bucket_ts()`.
 *
 *-----------------------------------------------------------------------------
 * Entry Addition and Bucket Promotion
 *-----------------------------------------------------------------------------
 *
 * When a new entry is added:
 *
 * 1. If the current set is `NONE`, it becomes a `SINGLE` bucket.
 * 2. If the set is a `SINGLE` bucket and another entry arrives:
 *      -> it is promoted to a `VECTOR` bucket (sorted by expiry).
 * 3. If the `VECTOR` exceeds `VOLATILESET_VECTOR_BUCKET_MAX_SIZE` (127):
 *      -> the set becomes a `RAX`, and existing entries are migrated.
 * 4. IF the set is using RAX encoding it will locate a bucket to add the entry
 *    following the strategy explained below.
 *
 *-----------------------------------------------------------------------------
 * RAX Bucket and Dynamic Splitting
 *-----------------------------------------------------------------------------
 *
 * Each bucket in the RAX bucket corresponds to a **time window**, defined by
 * its bucket timestamp (`bucket_ts`). This timestamp represents the **END** of
 * the time window. Entries in the bucket must expire *before* this timestamp.
 *
 * Time windows are defined in granular ranges:
 *   - Minimum granularity: VOLATILESET_BUCKET_INTERVAL_MIN (16 ms)
 *   - Maximum granularity: VOLATILESET_BUCKET_INTERVAL_MAX (8192 ms)
 *
 * A bucket can only contain entries that:
 *   1. Have expiry < bucket_ts
 *   2. Do not fit into any bucket with a smaller timestamp (i.e., earlier window)
 *
 * The structure allows multiple encodings:
 *     VSET_BUCKET_SINGLE  - A single pointer to one entry.
 *     VSET_BUCKET_VECTOR  - A sorted vector of pointers (up to 127 entries).
 *     VSET_BUCKET_HT      - A hashtable used when vectors become too dense.
 *
 * Bucket Timestamp (END of window):
 *
 *        |------------------ Bucket Span ------------------|
 *        [window_start .................................. bucket_ts)
 *
 * Layout Example:
 *
 *   Timeline:         ----------> increasing time ----------->
 *                     +--------------+-------------+---------+
 *                     | B0           | B1          |   B2    |
 *                     | ts=32        | ts=128      | ts=2048 |
 *                     +--------------+-------------+---------+
 *                     ^              ^             ^
 *                     |              |             |
 *           [E1,E2] ∈ B0      [E3...E7] ∈ B1     [E8...E15] ∈ B2
 *
 *           All entries expire BEFORE their bucket_ts
 *
 * Bucket Splitting Strategy:
 * ----------------------------------
 *
 * When a bucket (e.g. VECTOR) becomes too dense or needs realignment:
 *
 * 1. Re-align to lower granularity:
 *      - Adjust the bucket timestamp down to a finer granularity (e.g. 16ms).
 *      - Only done if ALL entries still fit in the tighter window.
 *      - Effectively “moves” the bucket to an earlier timestamp.
 *
 *        Example: B(ts=128, span=128ms) -> B(ts=64, span=16ms)
 *
 * 2. Split into two buckets:
 *      - Use binary search to find a “natural” boundary based on entry expiry.
 *      - Original bucket retains its timestamp (but holds fewer entries).
 *      - New bucket is inserted before the current one with its own tighter timestamp.
 *
 *        Example:
 *
 *        Before:
 *             [ Entry0 ... Entry126 ]  -> B(ts=128)
 *
 *        After Split:
 *             [ Entry0...Entry62 ]     -> New B(ts=64)
 *             [ Entry63...Entry126 ]   -> Original B(ts=128)
 *
 * 3. Convert to hashtable:
 *      - When no clean split is found (e.g. all entries share similar expiry),
 *        and realignment is not possible.
 *      - This allows efficient O(1) lookups even with clustered expiry values.
 *
 *        Vector B(ts=128) -> Hashtable B(ts=128)
 *
 * This hierarchical design ensures:
 *   - Efficient memory usage (tight buckets)
 *   - Predictable iteration by expiry time
 *   - Low overhead insertions & deletions
 *   - Graceful promotion & demotion of bucket types
 *
 * NOTE: Buckets are always sorted by their `bucket_ts` in the radix tree (RAX),
 *       which allows efficient search for insertion/removal based on expiry.
 *
 *-----------------------------------------------------------------------------
 * RAX Bucket Layout
 *-----------------------------------------------------------------------------
 *
 * * RAX View with Time Keys:
 *
 *     expiry_buckets = rax * | 0x6
 *
 *     +--------------------------+
 *     | RAX (key = bucket_ts)    |
 *     |--------------------------|
 *     | "000016" -> [entry1]     |  <- Vector (SINGLE->VECTOR->HT)
 *     | "000032" -> [entry2...]  |  <- Full vector, might split
 *     | "000048" -> [entry...]   |
 *     +--------------------------+
 *
 * * Splitting a Full Vector in RAX:
 *
 *     Suppose vector at key "000032" has 13 entries:
 *
 *     1. Use binary search to find a transition point in expiry bucket_ts.
 *        We search the first 2 following entries which belong to different lwo granularity time windows,
 *        but as close as possible to the middle of the vector:
 *            [entry1, entry7, ..., entry13]
 *                          ↑
 *                         split (first where get_bucket_ts(entry) > min_ts)
 *
 *     2. Create two vectors:
 *            bucket A -> [entry1..entry6]  with key = "000032"
 *            bucket B -> [entry7..entry13] with key = "000048"
 *
 *     3. Insert both back to the RAX.
 *
 *-----------------------------------------------------------------------------
 * Bucket Lifecycle
 *-----------------------------------------------------------------------------
 *
 *     NONE
 *       |
 *       v
 *     SINGLE (1 entry)
 *       |
 *       v
 *     VECTOR (sorted, up to 127)
 *       |
 *       v
 *     RAX (holds multiple buckets, keyed by each bucket's end timestamp)
 *     Bucket types within a RAX:
 *
 *                    SINGLE
 *                      |
 *                      v
 *                    VECTOR (sorted, up to 127, can split
 *                      |     into multiple vectors)
 *                      |
 *                      v
 *                   HASHTABLE (only when a vector can't split)
 */

/*************************************************************************************************************
 *                                pVector Implementation
 *************************************************************************************************************/

#define PV_CARD_BITS 30
#define PV_ALLOC_BITS 34

/* Custom vector structure with embedded allocation and length counters */
typedef struct {
    uint64_t len : PV_CARD_BITS;    /* Number of elements (cardinality) */
    uint64_t alloc : PV_ALLOC_BITS; /* Allocated memory (zmalloc_size of the current vector allocation) */
    void *data[];                   /* Flexible array member */
} pVector;

static const size_t PV_HEADER_SIZE = (sizeof(pVector));


/* Returns the number of elements currently stored in the pVector.
 *
 * Arguments:
 *   vec - The pVector to query.
 *
 * Return:
 *   The number of elements in the vector.
 *   Note that a NULL is a !!!valid!!! vector - returns 0 if the vector is NULL. */
static inline uint32_t
pvLen(pVector *vec) {
    return (vec ? vec->len : 0);
}

/* Returns the number of bytes allocated by the os to store the vector.
 * This value is equal to the usable size returned by calling zrealloc_usable.
 *
 * Arguments:
 *   vec - The pVector to query.
 *
 * Return:
 *   The allocation size of the vector
 *   Note that a NULL is a !!!valid!!! vector - returns 0 if the vector is NULL. */
static inline uint32_t pvAlloc(pVector *vec) {
    return (vec ? vec->alloc : 0);
}

/* Ensures that a pVector has enough capacity to hold additional elements.
 *
 * This function guarantees that the given pVector `pv` has at least enough
 * allocated space to accommodate `additional` more elements, growing it if necessary.
 * If the vector is currently `NULL`, it will be newly allocated.
 *
 * The allocation is handled using `zmalloc` or `zrealloc_usable`, depending on whether
 * the vector is new or already initialized. The internal `alloc` field is updated to
 * reflect the actual allocated size.
 *
 * Arguments:
 *   pv       - Pointer to an existing pVector or NULL.
 *   additional - The number of additional elements the vector should be able to accommodate.
 *
 * Return:
 *   A pointer to the resized (or newly allocated) pVector with sufficient capacity.
 *
 * Note:
 *   The `additional` is the number of *additional* elements beyond the current length.
 *   This function does not modify the vector's logical length (`len`), only its allocation. */
static pVector *pvMakeRoomFor(pVector *pv, size_t additional) {
    if (additional == 0) return pv;
    /* Make sure we will have the capacity to store the extra number of elements */
    assert(pvLen(pv) + additional <= (1UL << PV_CARD_BITS) - 1);

    size_t required = PV_HEADER_SIZE + (pvLen(pv) + additional) * sizeof(void *);

    if (pvAlloc(pv) >= required) return pv;

    if (!pv) {
        pv = zmalloc(required);
        pv->len = 0;
    } else {
        pv = zrealloc_usable(pv, required, &required);
    }
    /* Make sure we have the capacity to save the alloation size */
    assert(required <= (size_t)((1ULL << PV_ALLOC_BITS) - 1));
    pv->alloc = required;
    return pv;
}

/* Shrinks a pVector to release unused allocated memory.
 *
 * This function checks if the current allocation (`used`) for the given
 * `pVector` exceeds the memory actually required to store its elements.
 * If so, it reallocates the vector to use only the needed memory, helping reduce
 * memory overhead and improve space efficiency.
 *
 * The function uses `zrealloc_usable()` to reallocate memory in a way compatible
 * with jemalloc (or other zmalloc backends) and updates the internal allocation
 * size (`alloc`) to reflect the new length.
 *
 * Arguments:
 *  pv - A pointer to the `pVector` to shrink.
 *
 * Return:
 *  A potentially reallocated `pVector` with minimized memory usage.
 *
 *  This function does not change the logical contents of the vector.
 *  It only adjusts the allocated memory footprint. If no reallocation
 *  is needed, the original pointer is returned unchanged.
 *
 * Example:
 *     pVector *vec = pvNew();
 *     // After some insertions and deletions
 *     vec = pvShrinkToFit(vec); */
static pVector *pvShrinkToFit(pVector *pv) {
    if (!pv) return NULL;

    size_t used = pvAlloc(pv);
    size_t required = pvLen(pv) == 0 ? 0 : PV_HEADER_SIZE + pvLen(pv) * sizeof(void *);

    if (used > required) {
        if (!required) {
            zfree(pv);
            return NULL;
        }
        pv = zrealloc_usable(pv, required, &required);
        pv->alloc = required;
    }
    return pv;
}

/**
 * pvSplit - Splits a pVector into two parts at a given index.
 *
 * Arguments:
 * pv_ptr:       A pointer to the pVector* to split. This pointer is
 *                updated in-place to point to the left portion (elements [0..split_index-1]).
 * split_index:  The index at which to split the vector. The resulting right
 *                vector will contain elements [split_index..len-1].
 *
 * This function is used to **efficiently split a sorted vector of pointers**
 * into two separate vectors. The original vector is truncated in-place to
 * only contain the first half, and a new vector is returned containing the
 * second half. This allows for logical partitioning of data without scanning
 * or reallocating unnecessary memory.
 *
 * The vector is assumed to be densely packed and its elements are of type `void*`.
 *
 * Memory is allocated for the new right vector using `zmalloc`, and the unused
 * portion of the original vector may be freed or shrunk via `pvShrinkToFit`
 * to optimize memory usage.
 *
 * Return:
 *   - A new pVector containing the right split [split_index..len-1].
 *
 * Side effects:
 *   - The original vector pointer (`*pv_ptr`) is modified to point to the
 *     resized left portion.
 *
 * Example:
 * --------
 * Suppose `pv_ptr` points to a vector of 5 elements:
 *     [A, B, C, D, E]
 *
 * Calling:
 *     pVector *right = pvSplit(&pv_ptr, 3);
 *
 * Results in:
 *     pv_ptr -> [A, B, C]
 *     right   -> [D, E]
 *
 * If the split_index is 5 (i.e. the end), the function returns NULL and the
 * original vector is unchanged. */
pVector *pvSplit(pVector **pv_ptr, uint32_t split_index) {
    pVector *pv = *pv_ptr;

    /* Handle edge cases: */

    /* 1. null vector, ot split index which includes the entire vector in the left size
     * Should simply return a NULL vector (right size).
     */
    if (!pv || split_index >= pvLen(pv)) return NULL;

    /* 2. zero split index means no left side. just return the existing vector and zero the input vector. */
    if (split_index == 0) {
        *pv_ptr = NULL;
        return pv;
    }

    // Number of elements for the right half
    uint64_t right_len = pv->len - split_index;

    // Allocate new vector for right part
    size_t item_bytes = sizeof(void *);
    size_t total_bytes = sizeof(pVector) + right_len * item_bytes;
    size_t new_alloc;
    pVector *right = zmalloc_usable(total_bytes, &new_alloc);
    right->alloc = new_alloc;
    right->len = right_len;

    // Copy the right part
    memcpy(&right->data[0], &pv->data[split_index], right_len * item_bytes);

    // Shrink original vector
    pv->len = split_index;
    *pv_ptr = pvShrinkToFit(pv);

    return right;
}

/* Creates a new pVector with the specified initial capacity.
 *
 * This function initializes a new pVector capable of holding at least
 * `capacity` elements. Internally, it delegates allocation and setup to
 * `pvMakeRoomFor`, starting from a NULL vector.
 *
 * Arguments:
 *   capacity - The initial number of elements the vector should be able to store.
 *
 * Return:
 *   A pointer to the newly allocated pVector.
 *   Note that a NULL is a !!valid!! cector which size is zero.
 *
 * Note:
 *   The logical length (`len`) of the returned vector is initialized to 0.
 */
pVector *pvNew(uint32_t capacity) {
    return pvMakeRoomFor(NULL, capacity);
}

/* Inserts an element at the specified position in the pVector.
 *
 * Ensures enough capacity for the new element, shifts elements to make space,
 * and inserts the given element at the desired position.
 *
 * Arguments:
 *   pv   - The pVector to insert into (can be NULL).
 *   elem - The pointer to be inserted.
 *   idx  - The index at which to insert the element (must be ≤ pv->len).
 *
 * Return:
 *   The updated pVector with the element inserted. */
pVector *pvInsertAt(pVector *pv, void *elem, uint32_t idx) {
    assert(idx <= pv->len);
    pv = pvMakeRoomFor(pv, 1);

    if (idx < pv->len) {
        memmove(&pv->data[idx + 1], &pv->data[idx], (pv->len - idx) * sizeof(void *));
    }

    pv->data[idx] = elem;
    pv->len++;
    return pv;
}

/* Finds the index of the given element in the pVector.
 *
 * Parameters:
 *   pv   - The vector to search.
 *   elem - The element to look for (pointer equality).
 *
 * Returns:
 *   The index of the element if found; otherwise, returns pv->len (i.e., not found).
 *
 * Notes:
 *   - This compares elements using raw pointer equality (`==`).
 *   - If pv is NULL or empty, returns 0 as a safe fallback.
 *   - Return value being equal to pv->len can be used to check for absence. */
uint32_t pvFind(pVector *pv, void *elem) {
    if (!pv || pv->len == 0) return 0;

    for (uint32_t i = 0; i < pv->len; i++) {
        if (pv->data[i] == elem) {
            return i;
        }
    }
    return pv->len;
}


/* Removes the element at the specified index from the pVector.
 *
 * Shifts elements as necessary and optionally shrinks the vector if memory can be saved.
 * If this is the last element in the vector, the vector is freed and NULL is returned.
 *
 * Arguments:
 *   pv  - The pVector to remove from.
 *   idx - The index of the element to remove (must be < pv->len).
 *
 * Return:
 *   The updated pVector after removal.
 *   Returns NULL if the last element was removed and the vector was freed. */
pVector *pvRemoveAt(pVector *pv, uint32_t idx) {
    assert(pv && pv->len > 0);
    assert(idx < pv->len);
    if (pv->len == 1) {
        /* Last element being removed; delete vector */
        zfree(pv);
        return NULL;
    } else if (idx < pv->len - 1UL)
        memmove(&pv->data[idx], &pv->data[idx + 1], (pv->len - idx - 1) * sizeof(void *));
    pv->len--;
    return pvShrinkToFit(pv);
}

/* Removes the first matching element from the pVector.
 *
 * Performs a linear search for the given pointer and removes the first match.
 * Updates the vector pointer in case a removal was done.
 *
 * Arguments:
 *   pv   - A pointer to the pVector to remove from.
 *   elem - The element pointer to match and remove.
 *   removed - A pointer to a memory location to store the result of the removal.
 *
 * Return:
 *   the vector after the removal attempt */
pVector *pvRemove(pVector *pv, void *elem, bool *removed) {
    bool was_removed = false;
    if (pv && pvLen(pv) > 0) {
        uint32_t idx = pvFind(pv, elem);
        if (idx < pvLen(pv)) {
            pv = pvRemoveAt(pv, idx);
            was_removed = true;
        }
    }
    *removed = was_removed;
    return pv;
}

/* Retrieves the element at the specified index in the pVector.
 *
 * Arguments:
 *   vec - The pVector to retrieve from.
 *   idx - The index of the element to access.
 *
 * Return:
 *   A pointer to the element at the given index.
 *   Returns NULL if the vector is NULL or the index is out of bounds. */
void *pvGet(pVector *pv, uint32_t idx) {
    assert(pv && idx < pvLen(pv));
    return pv->data[idx];
}

/* Frees the memory used by the pVector.
 *
 * Arguments:
 *   pv - The pVector to free.
 *
 * Return:
 *   None. */
void pvFree(pVector *pv) {
    if (pv) zfree(pv);
}

/* Appends an element to the end of the given pVector.
 *
 * Parameters:
 *   pv   - The vector to append to.
 *   elem - The element to append.
 *
 * Returns:
 *   A (possibly reallocated) pVector with the new element inserted at the end.
 *
 * Notes:
 *   Internally this uses pvInsert() with the current length of the vector,
 *   effectively appending the element. */
pVector *pvPush(pVector *pv, void *elem) {
    return pvInsertAt(pv, elem, pvLen(pv));
}

/* Removes and optionally returns the last element from the given pVector.
 *
 * Parameters:
 *   pv    - The vector to remove the element from.
 *   pelem - Optional pointer to store the popped element. Can be NULL.
 *
 * Returns:
 *   A (possibly reallocated) pVector with the last element removed.
 *
 * Notes:
 *   Calling this function on an empty vector will trigger assertion.
 *   You can pass NULL for `pelem` if you don't need the removed value. */
pVector *pvPop(pVector *pv, void **pelem) {
    assert(pvLen(pv) > 0);
    uint32_t last_idx = pvLen(pv) - 1;
    if (pelem) *pelem = pvGet(pv, last_idx);
    return pvRemoveAt(pv, last_idx);
}

/* Set the element at given index inside the pVector.
 *
 * Parameters:
 *   pv    - The vector containing the elements to swap.
 *   idx   - Index of the element.
 *   elem  - pointer to the new element.
 *
 * Returns:
 *   None.
 *
 * Preconditions:
 *   - idx must be valid indices within the vector. */
void pvSet(pVector *pv, uint32_t idx, void *elem) {
    assert(idx < pvLen(pv));
    pv->data[idx] = elem;
}

/* Swaps two elements at given indices inside the pVector.
 *
 * Parameters:
 *   pv    - The vector containing the elements to swap.
 *   idx1  - Index of the first element.
 *   idx2  - Index of the second element.
 *
 * Returns:
 *   None.
 *
 * Preconditions:
 *   - idx1 and idx2 must both be valid indices within the vector.
 *
 * Notes:
 *   This is a simple in-place swap that uses direct pointer assignment. */
void pvSwap(pVector *pv, uint32_t idx1, uint32_t idx2) {
    assert(pv && pvLen(pv) > 0 && idx1 < pvLen(pv) && idx2 < pvLen(pv));
    void *temp = pv->data[idx1];
    pv->data[idx1] = pv->data[idx2];
    pv->data[idx2] = temp;
}

/* Sort the elements of a pVector using a user-provided comparison function.
 *
 * This function performs an in-place sort of the elements in the given pVector.
 * It uses the standard C library `qsort()` function under the hood and assumes
 * the elements are pointers. The caller must supply a comparison function
 * compatible with `qsort()`, which determines the ordering of the elements.
 *
 * Parameters:
 *   pv      - A pointer to the pVector to sort.
 *   compare - A function pointer used to compare two elements. This function must
 *             match the signature: int compare(const void *a, const void *b)
 *             and return:
 *                < 0 if *a < *b
 *                > 0 if *a > *b
 *                0   if *a == *b
 *
 * Returns:
 *   None. The pVector is sorted in place.
 *
 * Example:
 *   int cmp(const void *a, const void *b) {
 *       return strcmp(*(const char **)a, *(const char **)b);
 *   }
 *
 *   pvSort(my_vector, cmp); */
void pvSort(pVector *pv, int (*compare)(const void *a, const void *b)) {
    if (pvLen(pv) <= 1) return;
    qsort(pv->data, pv->len, sizeof(void *), compare);
}

/*************************************************************************************************************
 *                                pVector End
 *************************************************************************************************************/

#define VOLATILESET_BUCKET_INTERVAL_MAX (1LL << 13LL) // 2^13 = 8192 milliseconds
#define VOLATILESET_BUCKET_INTERVAL_MIN (1LL << 4LL)  // 2^4 = 16 milliseconds

#define VOLATILESET_VECTOR_BUCKET_MAX_SIZE 127

#define VSET_NONE_BUCKET_PTR ((void *)(uintptr_t) - 1)
#define VSET_BUCKET_NONE -1      // matching the NULL case
#define VSET_BUCKET_SINGLE 0x1UL // xx1 (assuming sds)
#define VSET_BUCKET_VECTOR 0x2UL // 010
#define VSET_BUCKET_HT 0x4UL     // 100
#define VSET_BUCKET_RAX 0x6UL    // 110

#define VSET_TAG_MASK 0x7UL
#define VSET_PTR_MASK (~VSET_TAG_MASK)

// Generic bucket type
typedef void vsetBucket;

typedef struct vsetInternalIterator {
    /* for rax bucket */
    raxIterator riter;
    union {
        /* for hashtable bucket */
        hashtableIterator hiter;
        /* for vector bucket */
        uint32_t viter;
        /* for single bucket */
        void *vsingle;
    };
    /* the parent of the bucket we are currently iterating on */
    vsetBucket *parent_bucket;
    /* the bucket we are currently iterating on */
    vsetBucket *bucket;
    /* the pointer entry */
    void *entry;
    /* In case of rax encoded set, this is the current iterated bucket timestamp */
    long long bucket_ts;
    /* the state of the iteration */
    int iteration_state;
} vsetInternalIterator;

/* The opaque hashtableIterator is defined as a blob of bytes. */
static_assert(sizeof(vsetIterator) >= sizeof(vsetInternalIterator),
              "Opaque iterator size");

/* Conversion from user-facing opaque iterator type to internal struct. */
static inline vsetInternalIterator *iteratorFromOpaque(vsetIterator *iterator) {
    return (vsetInternalIterator *)(void *)iterator;
}

/* Conversion from user-facing opaque iterator type to internal struct. */
static inline vsetIterator *opaqueFromIterator(vsetInternalIterator *iterator) {
    return (vsetIterator *)(void *)iterator;
}


/* Determine bucket type */
static inline int vsetBucketType(vsetBucket *b) {
    assert(b);
    if (b == VSET_NONE_BUCKET_PTR) return VSET_BUCKET_NONE;

    uintptr_t bits = (uintptr_t)b;
    if (bits & 0x1)
        return VSET_BUCKET_SINGLE;
    return bits & VSET_TAG_MASK;
}

/* Access raw pointer */
static inline void *vsetBucketRawPtr(vsetBucket *b) {
    return (void *)((uintptr_t)b & VSET_PTR_MASK);
}

// Accessors with type assertions
static inline pVector *vsetBucketVector(vsetBucket *b) {
    assert(vsetBucketType(b) == VSET_BUCKET_VECTOR);
    return (pVector *)vsetBucketRawPtr(b);
}

static inline hashtable *vsetBucketHashtable(vsetBucket *b) {
    assert(vsetBucketType(b) == VSET_BUCKET_HT);
    return (hashtable *)vsetBucketRawPtr(b);
}

static inline rax *vsetBucketRax(vsetBucket *b) {
    assert(vsetBucketType(b) == VSET_BUCKET_RAX);
    return (rax *)vsetBucketRawPtr(b);
}

static inline void *vsetBucketSingle(vsetBucket *b) {
    return b;
}

static inline vsetBucket *vsetBucketFromRawPtr(void *ptr, int type) {
    assert(ptr != NULL);
    uintptr_t p = (uintptr_t)ptr;
    return (vsetBucket *)(p | (type & VSET_TAG_MASK));
}

static inline vsetBucket *vsetBucketFromVector(pVector *vec) {
    return vsetBucketFromRawPtr(vec, VSET_BUCKET_VECTOR);
}

static inline vsetBucket *vsetBucketFromHashtable(hashtable *ht) {
    return vsetBucketFromRawPtr(ht, VSET_BUCKET_HT);
}

static inline vsetBucket *vsetBucketFromSingle(void *ptr) {
    return ptr;
}

static inline vsetBucket *vsetBucketFromNone(void) {
    return VSET_NONE_BUCKET_PTR;
}

static inline vsetBucket *vsetBucketFromRax(rax *r) {
    return vsetBucketFromRawPtr(r, VSET_BUCKET_RAX);
}

/****************** Helper Functions *******************************************/

/* compare 2 expiration times */
#define EXPIRE_COMPARE(exp1, exp2) (exp1 < exp2 ? -1 : exp1 == exp2 ? 0 \
                                                                    : 1)

/* Since we do not have native posix support for qsort_r, we use this variable to help the vset
 * compare function operate entry comparison given a dynamic getExpiry function is passed to
 * different vset functions. */
static __thread vsetGetExpiryFunc current_getter_func;

static inline void vsetSetExpiryGetter(vsetGetExpiryFunc f) {
    assert(current_getter_func == NULL);
    current_getter_func = f;
}

static inline void vsetUnsetExpiryGetter(void) {
    current_getter_func = NULL;
}

static inline vsetGetExpiryFunc vsetGetExpiryGetter(void) {
    return current_getter_func;
}

static int vsetCompareEntries(const void *a, const void *b) {
    vsetGetExpiryFunc getExpiry = vsetGetExpiryGetter();
    long long ea = getExpiry(*(void **)a);
    long long eb = getExpiry(*(void **)b);
    return (ea > eb) - (ea < eb);
}

/* used for popping form rax bucket where we KNOW all entries are expired. */
static long long vsetGetExpiryZero(const void *entry) {
    UNUSED(entry);
    return 0;
}

static inline long long get_bucket_ts(long long expiry) {
    return (expiry & ~(VOLATILESET_BUCKET_INTERVAL_MIN - 1LL)) + VOLATILESET_BUCKET_INTERVAL_MIN;
}

static inline long long get_max_bucket_ts(long long expiry) {
    return (expiry & ~(VOLATILESET_BUCKET_INTERVAL_MAX - 1LL)) + VOLATILESET_BUCKET_INTERVAL_MAX;
}

static inline size_t encodeExpiryKey(long long expiry, unsigned char *key) {
    long long be_ts = htonu64(expiry);
    size_t size = sizeof(be_ts);
    memcpy(key, &be_ts, size);
    return size;
}

static inline long long decodeExpiryKey(unsigned char *key) {
    long long res;
    memcpy(&res, key, sizeof(res));
    res = ntohu64(res);
    return res;
}

static inline size_t encodeNewExpiryBucketKey(unsigned char *key, long long expiry) {
    long long bucket_ts = get_max_bucket_ts(expiry);
    long long be_ts = htonu64(bucket_ts);
    size_t size = sizeof(be_ts);
    memcpy(key, &be_ts, size);
    return size;
}

/**
 * Performs binary search to find the index where the element should be inserted.
 * Returns the index where the element should be placed to keep the array sorted.
 *
 * pv Pointer to the sorted vector
 * elem Pointer to the element to insert
 * cmp Comparison function (like strcmp-style: <0, ==0, >0)
 * returns the insertion index (between 0 and pv->len) */
static inline uint32_t findInsertPosition(vsetGetExpiryFunc getExpiry, vsetBucket *bucket, long long expiry) {
    pVector *pv = vsetBucketVector(bucket);
    uint32_t left = 0;
    uint32_t right = pvLen(pv);
    while (left < right) {
        uint32_t mid = (left + right) / 2;
        int res = EXPIRE_COMPARE(expiry, getExpiry(pv->data[mid]));
        if (res <= 0)
            right = mid;
        else
            left = mid + 1;
    }

    return left; // Final position to insert the element
}

/* findSplitPosition - Locate the first index where a bucket timestamp transition occurs
 *
 * This function finds a split point in a sorted pointer vector (`pVector`) of elements,
 * where elements are grouped by their coarse-grained expiry time buckets.
 * The goal is to identify the first pair of adjacent elements `e[i-1]` and `e[i]`
 * such that:
 *
 *     get_bucket_ts(getExpiry(e[i - 1])) < get_bucket_ts(getExpiry(e[i]))
 *
 * The vector is assumed to be sorted by the raw expiry timestamp (in ascending order).
 * Bucket timestamps are derived using `get_bucket_ts()` on each element's expiry value.
 *
 * Arguments:
 *   - getExpiry: A function pointer that extracts an expiry timestamp from an element.
 *   - bucket:    A pointer to a `vsetBucket` containing a sorted `pVector` of elements.
 *   - split_ts_out (optional): If provided, it will be set to the bucket timestamp of
 *                              the last element in the lower (left) partition.
 *
 * The search begins from the middle of the vector and expands outwards in both
 * directions, checking for the earliest position where a bucket transition occurs.
 * This approach improves locality and helps produce balanced splits where possible.
 *
 * If a valid split is found, the function returns the index `i` at which the split
 * should occur (i.e., elements `[0..i-1]` belong to one bucket, and `[i..len-1]` to another).
 * If no split is found (i.e., all elements map to the same bucket), the function
 * returns `pv->len`, indicating the entire vector belongs to one bucket.
 *
 * Return:
 *   - A split index in the range [1, pv->len), or
 *   - `pv->len` if no transition is found (no split possible).
 *
 * Example:
 * --------
 * Raw expiry values:       [1001, 1002, 1003, 2048, 2049]
 * Bucket timestamps:       [1024, 1024, 1024, 4096, 4096]
 *
 * This function returns index 3, as:
 *     get_bucket_ts(1003) == 1024
 *     get_bucket_ts(2048) == 4096 → transition point
 *
 * So the vector can be split as:
 *   - Left partition:  [1001, 1002, 1003]
 *   - Right partition: [2048, 2049] */
static uint32_t findSplitPosition(vsetGetExpiryFunc getExpiry, vsetBucket *bucket, long long *split_ts_out) {
    pVector *pv = vsetBucketVector(bucket);
    if (!pv || pv->len < 2) return pv ? pv->len : 0;

    int mid = pv->len / 2;
    int offset = 0;

    while (1) {
        int left = mid - offset;
        int right = mid + offset;

        // Check left side (as long as i > 0 to allow e[i-1])
        if (left > 0) {
            long long ts1 = get_bucket_ts(getExpiry(pvGet(pv, left - 1)));
            long long ts2 = get_bucket_ts(getExpiry(pvGet(pv, left)));
            if (ts1 < ts2) {
                if (split_ts_out) *split_ts_out = ts1;
                return left;
            }
        }

        // Check right side (as long as i > 0 to allow e[i-1])
        if (right > 0 && right < pv->len) {
            long long ts1 = get_bucket_ts(getExpiry(pvGet(pv, right - 1)));
            long long ts2 = get_bucket_ts(getExpiry(pvGet(pv, right)));
            if (ts1 < ts2) {
                if (split_ts_out) *split_ts_out = ts1;
                return right;
            }
        }

        offset++;
        if (mid - offset < 1 && mid + offset >= pv->len) break; // searched entire vector
    }

    return pv->len; // no split found
}

#define VSET_BUCKET_KEY_LEN 8

/* hash_pointer - Computes a high-quality 64-bit hash from a pointer value.
 *
 * This function is designed to produce a well-distributed hash from a memory
 * pointer, avoiding the common pitfall of poor entropy due to pointer alignment.
 * It uses a platform-dependent mixing strategy based on MurmurHash3 finalization
 * constants, ensuring good avalanche behavior and low collision rates.
 *
 * For 32-bit systems:
 *   The function uses a reduced MurmurHash3 32-bit finalizer:
 *     - XORs and right shifts to mix higher-order bits into lower ones.
 *     - Multiplies by large constants to further spread the bits.
 *
 *
 * For 64-bit systems:
 *   The function uses MurmurHash3 64-bit finalizer constants:
 *     - These constants are chosen to maximize bit diffusion and avoid hash clustering.
 *     - This version benefits from the full 64-bit pointer space.
 *
 * Why this works:
 *   - Pointers tend to have low entropy in their lower bits (due to alignment).
 *   - A naive cast to integer leads to clustering and collisions in hash tables.
 *   - This function performs fast and effective bit mixing to reduce collisions.
 *   - Ideal for use in pointer-keyed hash tables, interning systems, or caches.
 *
 * Note:
 *   - This is not a cryptographic hash. It is suitable for fast, internal use only.
 *   - Returns a 64-bit hash value, even on 32-bit systems.
 *
 * Returns:
 *   A 64-bit hash value derived from the input pointer. */
static uint64_t hash_pointer(const void *ptr) {
    uintptr_t x = (uintptr_t)ptr;
#if UINTPTR_MAX == 0xFFFFFFFF
    // 32-bit platform
    x ^= x >> 16;
    x *= 0x85ebca6b;
    x ^= x >> 13;
    x *= 0xc2b2ae35;
    x ^= x >> 16;

#else
    // 64-bit platform
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
#endif
    return (uint64_t)x;
}

hashtableType pointerHashtableType = {
    .hashFunction = hash_pointer,
};

static inline vsetBucket *findBucket(rax *expiry_buckets, long long expiry, unsigned char *key, size_t *key_len, long long *pbucket_ts, raxNode **node) {
    *key_len = encodeExpiryKey(expiry, key);
    vsetBucket *bucket = vsetBucketFromNone();
    /* First try to locate the first bucket which is larger than the specified key */
    raxIterator iter;
    raxStart(&iter, expiry_buckets);
    raxSeek(&iter, ">", (unsigned char *)key, *key_len);

    if (raxNext(&iter)) {
        long long bucket_ts = decodeExpiryKey(iter.key);
        /* If this bucket span over a window to far in the future, it is not a candidate. */
        if (get_max_bucket_ts(expiry) < bucket_ts) {
            raxStop(&iter);
            return vsetBucketFromNone();
        }
        bucket = iter.data;
        assert(iter.node->iskey);
        if (node) *node = iter.node;
        if (key) {
            assert(iter.key_len == VSET_BUCKET_KEY_LEN);
            memcpy(key, iter.key, iter.key_len);
        }
        if (pbucket_ts) *pbucket_ts = decodeExpiryKey(iter.key);
    }
    raxStop(&iter);
    return bucket;
}

/* Free all the vsetBucket memory.
 * Since the bucket only holds references to entries the entries themselves are NOT freed */
static void freeVsetBucket(vsetBucket *bucket) {
    switch (vsetBucketType(bucket)) {
    case VSET_BUCKET_NONE:
    case VSET_BUCKET_SINGLE:
        // No internal memory to free
        break;
    case VSET_BUCKET_VECTOR:
        pvFree(vsetBucketVector(bucket));
        break;
    case VSET_BUCKET_HT:
        hashtableRelease(vsetBucketHashtable(bucket));
        break;
    case VSET_BUCKET_RAX:
        raxFreeWithCallback(vsetBucketRax(bucket), freeVsetBucket);
        break;
    default:
        panic("Unknown volatile set type in freeVsetBucket");
    }
}

static bool splitBucketIfPossible(vsetBucket *parent, vsetGetExpiryFunc getExpiry, vsetBucket *bucket, long long bucket_ts, raxNode *node) {
    /* We can only split vector encoded buckets */
    if (vsetBucketType(bucket) != VSET_BUCKET_VECTOR) {
        return false;
    }
    size_t key_len;
    long long target_bucket_ts = bucket_ts;
    unsigned char key[VSET_BUCKET_KEY_LEN] = {0};
    vsetBucket *new_bucket = vsetBucketFromNone();
    pVector *pv = vsetBucketVector(bucket);
    rax *expiry_buckets = vsetBucketRax(parent);
    /* first lets sort the vector. we cannot take a decision without it.
     * We set the global expiry getter so we can sort according to the provided getExpiry function.
     * TODO: After some thought I think it might be better to avoid sorting and attempt a quickselect. just allocate a new vector with the same size.
     * Than scan once and choose a pivot which is the median or average bucket_ts. Then move all entries smaller to the new vector. then shrink both vectors as needed. */
    vsetSetExpiryGetter(getExpiry);
    pvSort(pv, vsetCompareEntries);
    vsetUnsetExpiryGetter();

    long long max_bucket_ts = get_bucket_ts(getExpiry(pv->data[pvLen(pv) - 1]));
    long long min_bucket_ts = get_bucket_ts(getExpiry(pv->data[0]));

    if (max_bucket_ts < bucket_ts) {
        /* In case the bucket is already spanning over a larger window than needed, just place the bucket in a new place */
        key_len = encodeExpiryKey(bucket_ts, key);
        assert(raxRemove(expiry_buckets, key, key_len, (void **)&new_bucket));
        assert(new_bucket == bucket);
        target_bucket_ts = max_bucket_ts;

    } else if (min_bucket_ts != max_bucket_ts) {
        /* lets split the bucket. we know we can do it. */
        uint32_t split_index = findSplitPosition(getExpiry, bucket, &target_bucket_ts);
        assert(target_bucket_ts < bucket_ts);
        assert(split_index != pvLen(pv)); /* no way to split it ???  */
        pVector *new_bucket_vector = vsetBucketVector(bucket);
        bucket = vsetBucketFromVector(pvSplit(&new_bucket_vector, split_index));
        new_bucket = vsetBucketFromVector(new_bucket_vector);
        assert(pvLen(vsetBucketVector(new_bucket)) > 0);
        assert(pvLen(vsetBucketVector(bucket)) > 0);
        /* modify the current bucket data pointer */
        key_len = encodeExpiryKey(bucket_ts, key);
        /* In order to avoid rax override, we directly change the node data */
        // alternative: raxInsert(*set, key, key_len, bucket, NULL);
        raxSetData(node, bucket);

    } else {
        /* We cannot split the bucket. just return false */
        return false;
    }
    /* We change the current bucket position OR we split it, either way we have a new bucket to insert. */
    key_len = encodeExpiryKey(target_bucket_ts, key);
    raxInsert(expiry_buckets, key, key_len, new_bucket, NULL);
    return true;
}

static inline vsetBucket *insertToBucket_NONE(vsetGetExpiryFunc getExpiry, vsetBucket *bucket, void *entry, long long expiry) {
    UNUSED(getExpiry);
    UNUSED(expiry);
    UNUSED(bucket);
    return vsetBucketFromSingle(entry);
}

static inline vsetBucket *insertToBucket_SINGLE(vsetGetExpiryFunc getExpiry, vsetBucket *bucket, void *entry, long long expiry) {
    /* Upgrade to vector */
    pVector *pv = pvNew(2);
    void *curr_entry = vsetBucketSingle(bucket);
    long long curr_expiry = getExpiry(curr_entry);
    if (curr_expiry < expiry) {
        pv = pvPush(pv, curr_entry);
        pv = pvPush(pv, entry);
    } else {
        pv = pvPush(pv, entry);
        pv = pvPush(pv, curr_entry);
    }
    bucket = vsetBucketFromVector(pv);
    return bucket;
}

static inline vsetBucket *insertToBucket_VECTOR(vsetGetExpiryFunc getExpiry, vsetBucket *bucket, void *entry, long long expiry, int pos) {
    UNUSED(getExpiry);
    UNUSED(expiry);
    pVector *pv = vsetBucketVector(bucket);
    /* limit of the number of elements in a vector. */
    if (pvLen(pv) >= VOLATILESET_VECTOR_BUCKET_MAX_SIZE) {
        //  Upgrade to hashtable
        hashtable *ht = hashtableCreate(&pointerHashtableType);
        for (uint32_t i = 0; i < pvLen(pv); i++) {
            hashtableAdd(ht, pvGet(pv, i));
        }
        pvFree(pv);
        /* Add the new entry as well */
        hashtableAdd(ht, entry);

        return vsetBucketFromHashtable(ht);
    } else {
        if (pos >= 0)
            /* In case we are explicitly provided a position to insert place the entry there */
            return vsetBucketFromVector(pvInsertAt(pv, entry, pos));
        else
            /* Otherwise it is better to just push the entry to the vector with less change of memmove and reallocation. */
            return vsetBucketFromVector(pvPush(pv, entry));
    }
    return vsetBucketFromNone();
}

static inline vsetBucket *insertToBucket_HASHTABLE(vsetGetExpiryFunc getExpiry, vsetBucket *bucket, void *entry, long long expiry) {
    UNUSED(getExpiry);
    UNUSED(expiry);

    hashtable *ht = vsetBucketHashtable(bucket);
    assert(hashtableAdd(ht, entry));
    return bucket;
}

static inline vsetBucket *insertToBucket_RAX(vsetGetExpiryFunc getExpiry, vsetBucket *target, void *entry, long long expiry) {
    unsigned char key[VSET_BUCKET_KEY_LEN] = {0};
    size_t key_len;
    long long bucket_ts;
    rax *expiry_buckets = vsetBucketRax(target);
    raxNode *node;
    vsetBucket *bucket = findBucket(expiry_buckets, expiry, key, &key_len, &bucket_ts, &node);
    int type = vsetBucketType(bucket);
    if (type == VSET_BUCKET_NONE) {
        /* No bucket: create single-entry bucket */
        bucket = insertToBucket_NONE(getExpiry, bucket, entry, expiry);
        assert(vsetBucketType(bucket) == VSET_BUCKET_SINGLE);
        size_t key_size = encodeNewExpiryBucketKey(key, expiry);
        raxInsert(expiry_buckets, key, key_size, bucket, NULL);
        return target;
    } else if (type == VSET_BUCKET_SINGLE) {
        /* Upgrade to vector */
        bucket = insertToBucket_SINGLE(getExpiry, bucket, entry, expiry);
        assert(vsetBucketType(bucket) == VSET_BUCKET_VECTOR);
        /* In order to avoid rax override, we directly change the node data */
        // alternative: raxInsert(expiry_buckets, key, key_len, bucket, NULL);
        raxSetData(node, bucket);
    } else if (type == VSET_BUCKET_VECTOR) {
        pVector *pv = vsetBucketVector(bucket);
        if (pvLen(pv) == VOLATILESET_VECTOR_BUCKET_MAX_SIZE) {
            /* Try to split the bucket. If not possible switch to hashtable encoding. */
            if (!splitBucketIfPossible(target, getExpiry, bucket, bucket_ts, node)) {
                /* Can't split? insrt to the vector anyway, it will just expand to hashtable */
                bucket = insertToBucket_VECTOR(getExpiry, bucket, entry, expiry, -1);
                assert(vsetBucketType(bucket) == VSET_BUCKET_HT);
                /* In order to avoid rax override, we directly change the node data */
                // alternative raxInsert(expiry_buckets, key, key_len, bucket, NULL);
                raxSetData(node, bucket);
            } else {
                /* we split the bucket. go and find again a bucket to place the entry since there can be new options now. */
                return insertToBucket_RAX(getExpiry, target, entry, expiry);
            }
        } else {
            vsetBucket *new_bucket = insertToBucket_VECTOR(getExpiry, bucket, entry, expiry, -1);
            if (new_bucket != bucket)
                /* In order to avoid rax override, we directly change the node data */
                // alternative: raxInsert(expiry_buckets, key, key_len, new_bucket, NULL);
                raxSetData(node, new_bucket);
        }
    } else if (vsetBucketType(bucket) == VSET_BUCKET_HT) {
        bucket = insertToBucket_HASHTABLE(getExpiry, bucket, entry, expiry);
    } else {
        panic("Unknown bucket type in insertToBucket_RAX");
    }
    return target;
}

static inline vsetBucket *removeFromBucket_SINGLE(vsetGetExpiryFunc getExpiry, vsetBucket *bucket, void *entry, long long expiry, bool *removed) {
    UNUSED(getExpiry);
    UNUSED(expiry);

    if (vsetBucketSingle(bucket) == entry) {
        *removed = true;
        return vsetBucketFromNone();
    } else {
        *removed = false;
        return bucket;
    }
}

static inline vsetBucket *removeFromBucket_VECTOR(vsetGetExpiryFunc getExpiry, vsetBucket *bucket, void *entry, long long expiry, bool *removed, bool pop) {
    UNUSED(getExpiry);
    UNUSED(expiry);

    vsetBucket *new_bucket = bucket;
    bool success = false;
    pVector *pv = vsetBucketVector(bucket);
    /* In case we we removed the entry */
    uint32_t vlen = pvLen(pv);
    if (vlen <= 2) {
        /* convert to single if needed */
        uint32_t idx = pvFind(pv, entry);
        if (idx == vlen) {
            success = false;
        } else {
            if (vlen == 1)
                new_bucket = vsetBucketFromNone();
            else
                new_bucket = vsetBucketFromSingle(pvGet(pv, idx == 0 ? 1 : 0));
            success = true;
            pvFree(pv);
        }
    } else {
        /* pop is a more efficient way to remove an element from the vector. However it may
         * change the order of the elements in the vector, so we should ask the user to indicate if to use pop or not. */
        if (pop) {
            uint32_t idx = pvFind(pv, entry);
            if (idx < vlen) {
                void *popped_entry = NULL;
                pvSwap(pv, idx, pvLen(pv) - 1);
                success = true;
                new_bucket = vsetBucketFromVector(pvPop(pv, &popped_entry));
                assert(popped_entry == entry);
            }
        } else {
            pv = pvRemove(pv, entry, &success);
            if (success)
                new_bucket = vsetBucketFromVector(pv);
        }
    }
    if (removed) *removed = success;
    return new_bucket;
}

static inline vsetBucket *removeFromBucket_HASHTABLE(vsetGetExpiryFunc getExpiry, vsetBucket *bucket, void *entry, long long expiry, bool *removed) {
    UNUSED(getExpiry);
    UNUSED(expiry);

    bool success = false;
    vsetBucket *new_bucket = bucket;
    hashtable *ht = vsetBucketHashtable(bucket);
    if (hashtableDelete(ht, entry)) {
        success = true;
        assert(hashtableSize(ht) > 0);
        if (hashtableSize(ht) == 1) {
            // Downgrade to SINGLE
            hashtableIterator hi;
            hashtableInitIterator(&hi, ht, 0);
            void *ptr;
            hashtableNext(&hi, &ptr);
            hashtableRelease(ht);
            new_bucket = vsetBucketFromSingle(ptr);
        }
    }
    if (removed) *removed = success;
    return new_bucket;
}
static bool removeEntryFromRaxBucket(vsetBucket *rax_bucket, vsetGetExpiryFunc getExpiry, void *entry, vsetBucket *bucket, unsigned char *key, size_t key_len, vsetBucket **pbucket, raxNode *node) {
    bool removed = false;
    switch (vsetBucketType(bucket)) {
    case VSET_BUCKET_SINGLE:
        bucket = removeFromBucket_SINGLE(getExpiry, bucket, entry, 0, &removed);
        if (removed) {
            raxRemove(vsetBucketRax(rax_bucket), key, key_len, NULL);
            if (pbucket) *pbucket = vsetBucketFromNone();
        }
        break;
    case VSET_BUCKET_VECTOR: {
        vsetBucket *new_bucket = removeFromBucket_VECTOR(getExpiry, bucket, entry, 0, &removed, true);
        if (new_bucket != bucket) {
            if (vsetBucketType(new_bucket) == VSET_BUCKET_NONE) {
                raxRemove(vsetBucketRax(rax_bucket), key, key_len, NULL);
                if (pbucket) *pbucket = vsetBucketFromNone();
            } else {
                /* In order to avoid rax override, we directly change the node data */
                // alternative: raxInsert(*set, key, key_len, new_bucket, NULL);
                raxSetData(node, new_bucket);
                if (pbucket) *pbucket = new_bucket;
            }
        }
        break;
    }
    case VSET_BUCKET_HT: {
        vsetBucket *new_bucket = removeFromBucket_HASHTABLE(getExpiry, bucket, entry, 0, &removed);
        if (new_bucket != bucket)
            /* In order to avoid rax override, we directly change the node data */
            // alternative: raxInsert(*set, key, key_len, bucket, NULL);
            raxSetData(node, new_bucket);

        if (pbucket) *pbucket = new_bucket;
        break;
    }
    default:
        panic("Unknown bucket type for removeEntryFromRaxBucket");
        return false;
    }
    return removed;
}

static inline bool shrinkRaxBucketIfPossible(vsetBucket **target, vsetGetExpiryFunc getExpiry) {
    rax *expiry_buckets = vsetBucketRax(*target);
    if (raxSize(expiry_buckets) == 1) {
        raxIterator it;
        raxStart(&it, expiry_buckets);
        assert(raxSeek(&it, "^", NULL, 0));
        assert(raxNext(&it));
        vsetBucket *bucket = it.data;
        int bucket_type = vsetBucketType(bucket);
        raxStop(&it);
        /* We will not convert hashtable to our only bucket since we will lose the ability to scan the items in a sorted way.
         * We will also not shrink when we have a full vector, since it might immediately be repopulated.  */
        if (bucket_type == VSET_BUCKET_SINGLE ||
            (bucket_type == VSET_BUCKET_VECTOR && pvLen(vsetBucketVector(bucket)) < VOLATILESET_VECTOR_BUCKET_MAX_SIZE)) {
            if (bucket_type == VSET_BUCKET_VECTOR) {
                pVector *pv = vsetBucketVector(bucket);
                /* first lets sort the vector. we cannot set the target bucket as unsorted vector bucket */
                vsetSetExpiryGetter(getExpiry);
                pvSort(pv, vsetCompareEntries);
                vsetUnsetExpiryGetter();
            }
            /* lets make our bucket to be the only left bucket */
            *target = bucket;
            raxFree(expiry_buckets);
            return true;
        }
    }
    return false;
}

static inline vsetBucket *removeFromBucket_RAX(vsetGetExpiryFunc getExpiry, vsetBucket *target, void *entry, long long expiry, bool *removed) {
    unsigned char key[VSET_BUCKET_KEY_LEN] = {0};
    long long bucket_ts;
    size_t key_len;
    raxNode *node;
    rax *expiry_buckets = vsetBucketRax(target);
    vsetBucket *bucket = findBucket(expiry_buckets, expiry, key, &key_len, &bucket_ts, &node);
    assert(bucket != VSET_NONE_BUCKET_PTR);
    bool success = removeEntryFromRaxBucket(target, getExpiry, entry, bucket, key, key_len, NULL, node);
    if (removed) *removed = success;
    // shrink to single bucket if possible
    shrinkRaxBucketIfPossible(&target, getExpiry);
    return target;
}

static inline size_t vsetBucketRemoveExpired_NONE(vsetBucket **bucket, vsetGetExpiryFunc getExpiry, vsetExpiryFunc expiryFunc, mstime_t now, size_t max_count, void *ctx) {
    UNUSED(bucket);
    UNUSED(getExpiry);
    UNUSED(expiryFunc);
    UNUSED(now);
    UNUSED(max_count);
    UNUSED(ctx);
    return 0;
}

static inline size_t vsetBucketRemoveExpired_SINGLE(vsetBucket **bucket, vsetGetExpiryFunc getExpiry, vsetExpiryFunc expiryFunc, mstime_t now, size_t max_count, void *ctx) {
    void *entry = vsetBucketSingle(*bucket);
    if (max_count && getExpiry(entry) <= now) {
        freeVsetBucket(*bucket);
        *bucket = vsetBucketFromNone();
        if (expiryFunc) expiryFunc(entry, ctx);
        return 1;
    }
    return 0;
}

static inline size_t vsetBucketRemoveExpired_VECTOR(vsetBucket **bucket, vsetGetExpiryFunc getExpiry, vsetExpiryFunc expiryFunc, mstime_t now, size_t max_count, void *ctx) {
    pVector *pv = vsetBucketVector(*bucket);
    uint32_t len = min(pvLen(pv), max_count);
    uint32_t i = 0;
    for (; i < len; i++) {
        void *entry = pvGet(pv, i);
        /* break as soon as the expiryFunc stops us OR we reached an entry which is not expired */
        if (getExpiry(entry) > now)
            break;
        if (expiryFunc) expiryFunc(entry, ctx);
    }
    /* If no expiry occurred, no need to split. */
    if (i > 0) {
        pVector *new_pv = pvSplit(&pv, i);
        *bucket = (new_pv ? vsetBucketFromVector(new_pv) : vsetBucketFromNone());
        pvFree(pv);
    }
    return i;
}

static inline size_t vsetBucketRemoveExpired_HASHTABLE(vsetBucket **bucket, vsetGetExpiryFunc getExpiry, vsetExpiryFunc expiryFunc, mstime_t now, size_t max_count, void *ctx) {
    UNUSED(getExpiry);
    UNUSED(now);
    hashtable *ht = vsetBucketHashtable(*bucket);
    hashtableIterator it;
    void *entry;
    size_t count = 0;
    hashtableInitIterator(&it, ht, HASHTABLE_ITER_SAFE);
    while (count < max_count && hashtableNext(&it, &entry)) {
        assert(hashtableDelete(ht, entry));
        expiryFunc(entry, ctx);
        count++;
    }
    hashtableResetIterator(&it);

    /* in case we completed scanning the hashtable which is now empty */
    size_t ht_size = hashtableSize(ht);
    if (ht_size == 0) {
        hashtableRelease(ht);
        *bucket = vsetBucketFromNone();
    }
    return count;
}

static inline size_t vsetBucketRemoveExpired_RAX(vsetBucket **bucket, vsetGetExpiryFunc getExpiry, vsetExpiryFunc expiryFunc, mstime_t now, size_t max_count, void *ctx) {
    UNUSED(getExpiry);
    rax *buckets = vsetBucketRax(*bucket);
    size_t count = 0;
    while (count < max_count && raxSize(buckets) > 0) {
        raxIterator it;
        raxStart(&it, buckets);
        raxSeek(&it, "^", NULL, 0);
        assert(raxNext(&it));
        /* lets start again by going into the first bucket. */
        unsigned char key[VSET_BUCKET_KEY_LEN] = {0};
        vsetBucket *time_bucket = it.data;
        int time_bucket_type = vsetBucketType(time_bucket);
        long long time_bucket_ts = decodeExpiryKey(it.key);
        memcpy(key, it.key, it.key_len);
        size_t key_len = it.key_len;
        raxNode *node = it.node;
        raxStop(&it);
        if (time_bucket_ts > now)
            break;
        switch (time_bucket_type) {
        case VSET_BUCKET_SINGLE:
            count += vsetBucketRemoveExpired_SINGLE(&time_bucket, vsetGetExpiryZero, expiryFunc, now, max_count - count, ctx);
            break;
        case VSET_BUCKET_VECTOR:
            count += vsetBucketRemoveExpired_VECTOR(&time_bucket, vsetGetExpiryZero, expiryFunc, now, max_count - count, ctx);
            break;
        case VSET_BUCKET_HT:
            count += vsetBucketRemoveExpired_HASHTABLE(&time_bucket, vsetGetExpiryZero, expiryFunc, now, max_count - count, ctx);
            break;
        default:
            panic("Cannot expire entries from bucket which is not single, vector or hashtable");
        }
        if (time_bucket == VSET_NONE_BUCKET_PTR) {
            /* in case the bucket is freed, we can just remove it and continue to the next bucket. */
            raxRemove(buckets, key, key_len, NULL);
        } else {
            /* in case the bucket still exists, it must be since we reached the max_count or stopped due to expiry function.
             * So we save the new bucket to the rax and bail. */
            raxSetData(node, time_bucket);
            break;
        }
    }
    /* if all buckets are removed, */
    if (raxSize(buckets) == 0) {
        raxFree(buckets);
        *bucket = vsetBucketFromNone();
    } else {
        shrinkRaxBucketIfPossible(bucket, getExpiry);
    }
    return count;
}

static int vsetBucketNext_NONE(vsetInternalIterator *it, void **entryptr) {
    UNUSED(it);
    UNUSED(entryptr);
    return 0;
}

static inline int vsetBucketNext_SINGLE(vsetInternalIterator *it, void **entryptr) {
    bool init_bucket_scan = (it->iteration_state == VSET_BUCKET_NONE);
    if (init_bucket_scan) {
        it->iteration_state = VSET_BUCKET_SINGLE;
        it->entry = vsetBucketSingle(it->bucket);
        if (entryptr) *entryptr = it->entry;
        return 1;
    }
    return 0;
}

static inline int vsetBucketNext_VECTOR(vsetInternalIterator *it, void **entryptr) {
    bool init_bucket_scan = (it->iteration_state == VSET_BUCKET_NONE);
    pVector *pv = vsetBucketVector(it->bucket);
    if (init_bucket_scan) {
        it->iteration_state = VSET_BUCKET_VECTOR;
        it->viter = 0;
    } else {
        it->viter++;
    }
    if (it->viter < pvLen(pv)) {
        it->entry = pvGet(pv, it->viter);
    } else {
        return 0;
    }
    if (entryptr) *entryptr = it->entry;
    return 1;
}

static inline int vsetBucketNext_HASHTABLE(vsetInternalIterator *it, void **entryptr) {
    bool init_bucket_scan = (it->iteration_state == VSET_BUCKET_NONE);
    hashtable *ht = vsetBucketHashtable(it->bucket);
    if (init_bucket_scan) {
        it->iteration_state = VSET_BUCKET_HT;
        hashtableInitIterator(&it->hiter, ht, 0);
    }
    if (!hashtableNext(&it->hiter, &it->entry)) {
        hashtableResetIterator(&it->hiter);
        return 0;
    }
    if (entryptr) *entryptr = it->entry;
    return 1;
}

static inline int vsetBucketNext_RAX(vsetInternalIterator *it, void **entryptr) {
    bool init_bucket_scan = (it->iteration_state == VSET_BUCKET_NONE);
    if (init_bucket_scan) {
        /* set myself as the parent bucket */
        it->parent_bucket = it->bucket;
        raxStart(&it->riter, vsetBucketRax(it->bucket));
        raxSeek(&it->riter, "^", NULL, 0);
    }
    if (raxNext(&it->riter)) {
        /* lets start again by going into the first bucket. */
        it->iteration_state = vsetBucketType(it->riter.data);
        it->bucket_ts = decodeExpiryKey(it->riter.key);
        it->bucket = it->riter.data;
        it->iteration_state = VSET_BUCKET_NONE;
        return vsetNext(opaqueFromIterator(it), entryptr);
    } else {
        /* We currently do not support nested RAX buckets */
        it->parent_bucket = vsetBucketFromNone();
        return 0;
    }
    return 1;
}

static inline size_t vsetBucketMemUsage_NONE(vsetBucket *bucket) {
    UNUSED(bucket);
    return 0;
}

static inline size_t vsetBucketMemUsage_SINGLE(vsetBucket *bucket) {
    UNUSED(bucket);
    return 0;
}

static inline size_t vsetBucketMemUsage_VECTOR(vsetBucket *bucket) {
    pVector *pv = vsetBucketVector(bucket);
    assert(pv);
    return pv->alloc;
}

static inline size_t vsetBucketMemUsage_HASHTABLE(vsetBucket *bucket) {
    hashtable *ht = vsetBucketHashtable(bucket);
    return hashtableMemUsage(ht);
}

static inline size_t vsetBucketMemUsage_RAX(vsetBucket *bucket) {
    rax *r = vsetBucketRax(bucket);
    size_t total_mem = raxAllocSize(r);
    raxIterator it;
    raxStart(&it, r);
    assert(raxSeek(&it, "^", NULL, 0));
    while (raxNext(&it)) {
        switch (vsetBucketType(it.data)) {
        case VSET_BUCKET_NONE:
            total_mem += vsetBucketMemUsage_NONE(it.data);
            break;
        case VSET_BUCKET_SINGLE:
            total_mem += vsetBucketMemUsage_SINGLE(it.data);
            break;
        case VSET_BUCKET_VECTOR:
            total_mem += vsetBucketMemUsage_VECTOR(it.data);
            break;
        case VSET_BUCKET_HT:
            total_mem += vsetBucketMemUsage_HASHTABLE(it.data);
            break;
        default:
            panic("Unknown bucket type encountered in vsetBucketMemUsage_HASHTABLE");
        }
    }
    raxStop(&it);
    return total_mem;
}

/* Adds an entry to a volatile set (vset) based on its expiration time.
 *
 * The volatile set maintains buckets of entries grouped by time windows. Each
 * entry is inserted into an appropriate bucket based on its expiry timestamp.
 * Buckets are memory-efficient and use dynamic representations that evolve as
 * the number of entries grows:
 *
 *  - VSET_BUCKET_NONE:
 *      Indicates the set is empty. A new SINGLE bucket is created to hold the entry.
 *
 *  - VSET_BUCKET_SINGLE:
 *      Holds a single entry directly. Upon inserting a second entry, the bucket
 *      is promoted to a VECTOR, preserving the sorted order.
 *
 *  - VSET_BUCKET_VECTOR:
 *      Stores entries in a compact, sorted vector. The maximum size is 127 entries.
 *      If inserting a new entry exceeds the limit:
 *        - If all entries share the same bucket timestamp (same high-resolution time window),
 *          the entire vector is moved into a RAX bucket as a single node.
 *        - Otherwise, each vector entry is redistributed into the new RAX structure.
 *
 *  - VSET_BUCKET_RAX:
 *      A radix tree (RAX) used for scalable management of multiple time-based buckets.
 *      Entries are inserted by computing their bucket key based on their expiration timestamp.
 *
 * The function uses the entry’s expiration time (provided via the getExpiry function)
 * to determine the correct bucket. It promotes bucket types as needed to maintain
 * sorted and efficient storage.
 *
 * In all cases, if the insertion causes a structural change (e.g., bucket promotion),
 * the pointer to the root of the bucket tree is updated via the `set` pointer.
 *
 * This function always returns true, as insertion is guaranteed to succeed
 * (barring internal memory allocation failure, which is outside its concern).
 *
 * Notes:
 *  - Buckets are upgraded in-place based on size and time span distribution.
 *  - Vector buckets allow binary search insertion to maintain order.
 *  - Tagged pointers are used to determine bucket types efficiently.
 *  - It is assumed that all entries have odd-valued pointers (LSB set).
 *  - Key encoding in RAX is based on the maximum expiration timestamp
 *    that falls within a fixed window granularity.
 *
 * Example:
 *     vset *myset = NULL;
 *     vsetAddEntry(&myset, extract_expiry, my_object);
 *
 *     // Internally, my_object is placed into the appropriate bucket. */
bool vsetAddEntry(vset *set, vsetGetExpiryFunc getExpiry, void *entry) {
    long long expiry = getExpiry(entry);
    vsetBucket *expiry_buckets = *set;
    assert(expiry_buckets);
    int bucket_type = vsetBucketType(expiry_buckets);
    switch (bucket_type) {
    case VSET_BUCKET_NONE:
        expiry_buckets = insertToBucket_NONE(getExpiry, expiry_buckets, entry, expiry);
        break;
    case VSET_BUCKET_SINGLE:
        expiry_buckets = insertToBucket_SINGLE(getExpiry, expiry_buckets, entry, expiry);
        break;
    case VSET_BUCKET_VECTOR: {
        pVector *vec = vsetBucketVector(expiry_buckets);
        uint32_t len = pvLen(vec);
        /* in case the vector is full, we need to turn into RAX */
        if (len == VOLATILESET_VECTOR_BUCKET_MAX_SIZE) {
            rax *r = raxNew();
            long long min_expiry = getExpiry(pvGet(vec, 0));
            long long max_expiry = getExpiry(pvGet(vec, len - 1));
            if (get_max_bucket_ts(min_expiry) == get_max_bucket_ts(max_expiry)) {
                /* In case we can just insert the bucket, no need to iterate and insert it's elements. we can just push the bucket as a whole. */
                unsigned char key[VSET_BUCKET_KEY_LEN] = {0};
                size_t key_len = encodeNewExpiryBucketKey(key, max_expiry);
                raxInsert(r, key, key_len, expiry_buckets, NULL);
                expiry_buckets = vsetBucketFromRax(r);
                expiry_buckets = insertToBucket_RAX(getExpiry, expiry_buckets, entry, expiry);
            } else {
                /* We need to migrate entries to the new set of buckets since we do not know all entries are in the same bucket */
                expiry_buckets = vsetBucketFromRax(r);
                for (uint32_t i = 0; i < len; i++) {
                    void *moved_entry = pvGet(vec, i);
                    expiry_buckets = insertToBucket_RAX(getExpiry, expiry_buckets, moved_entry, getExpiry(moved_entry));
                }
                /* free the vector */
                pvFree(vec);
                /* now insert the new entry to the buckets */
                expiry_buckets = insertToBucket_RAX(getExpiry, expiry_buckets, entry, expiry);
            }
        } else {
            uint32_t pos = findInsertPosition(getExpiry, expiry_buckets, expiry);
            expiry_buckets = insertToBucket_VECTOR(getExpiry, expiry_buckets, entry, expiry, pos);
        }
        break;
    }
    case VSET_BUCKET_RAX:
        expiry_buckets = insertToBucket_RAX(getExpiry, expiry_buckets, entry, expiry);
        break;
    default:
        panic("Cannot insert to bucket which is not single, vector or rax");
    }
    /* update the set */
    *set = expiry_buckets;
    return true;
}

static inline bool vsetRemoveEntryWithExpiry(vset *set, vsetGetExpiryFunc getExpiry, void *entry, long long expiry) {
    bool removed;
    vsetBucket *bucket = *set;
    assert(bucket);
    int bucket_type = vsetBucketType(bucket);
    switch (bucket_type) {
    case VSET_BUCKET_NONE:
        /* We cannot remove from empty set */
        return false;
    case VSET_BUCKET_SINGLE:
        bucket = removeFromBucket_SINGLE(getExpiry, bucket, entry, expiry, &removed);
        break;
    case VSET_BUCKET_VECTOR:
        bucket = removeFromBucket_VECTOR(getExpiry, bucket, entry, expiry, &removed, false);
        break;
    case VSET_BUCKET_HT:
        bucket = removeFromBucket_HASHTABLE(getExpiry, bucket, entry, expiry, &removed);
        break;
    case VSET_BUCKET_RAX:
        bucket = removeFromBucket_RAX(getExpiry, bucket, entry, expiry, &removed);
        break;
    default:
        panic("Cannot remove from bucket which is not single, vector, hashtable or rax");
    }
    *set = bucket;
    return removed;
}

/* Removes an entry from the volatile set (vset), based on its expiration time.
 *
 * The volatile set organizes entries into time-based buckets of varying types:
 * SINGLE, VECTOR, or RAX. The bucket type determines how entries are stored
 * and managed internally. This function will locate and remove the entry
 * from its appropriate bucket.
 *
 * The removal process works as follows:
 *
 *  1. The expiration timestamp of the entry is used to compute which bucket
 *     (based on its end time) the entry should reside in.
 *
 *  2. Depending on the current top-level bucket type of the vset, the function
 *     dispatches to the appropriate removal handler:
 *
 *     - VSET_BUCKET_SINGLE:
 *         If the stored entry matches, the bucket is set to NONE.
 *
 *     - VSET_BUCKET_VECTOR:
 *         Performs a binary search to find and remove the entry from the vector.
 *         If the resulting vector size drops to 1, it is converted to a SINGLE bucket.
 *         If the vector becomes empty, it is removed entirely (set to NONE).
 *
 *     - VSET_BUCKET_RAX:
 *         The function decodes the appropriate bucket key (based on the expiration
 *         time), looks up the RAX node, and dispatches removal to the sub-bucket.
 *         If a sub-bucket becomes empty or has only one entry left, its bucket
 *         type may be downgraded (e.g., to SINGLE or removed).
 *
 *  3. If the removal results in a structural change (e.g., shrinking a bucket),
 *     the bucket type may be changed, and the root pointer is updated accordingly.
 *
 *  4. If the entry is not found in the expected bucket, no action is taken.
 *
 * Notes:
 *  - Buckets self-adjust during removal for memory efficiency.
 *  - The vector bucket keeps entries sorted for fast search/removal.
 *  - RAX-based sets support a large number of buckets and scale well
 *    with many time windows.
 *  - Entries are assumed to have pointer identity (odd-valued pointers).
 *  - Correct expiration timestamp must be provided for accurate removal.
 *
 * Return value:
 *     Returns true if the entry was found and removed successfully.
 *     Returns false if the entry was not found.
 *
 * Example usage:
 *     vsetRemoveEntry(myset, extract_expiry, my_object);
 *
 *     // my_object is removed from the appropriate bucket in myset BUT is not freed. */
bool vsetRemoveEntry(vset *set, vsetGetExpiryFunc getExpiry, void *entry) {
    return vsetRemoveEntryWithExpiry(set, getExpiry, entry, getExpiry(entry));
}

static inline vsetBucket *vsetBucketUpdateEntry_SINGLE(vsetBucket *bucket, vsetGetExpiryFunc getExpiry, void *old_entry, void *new_entry, long long old_expiry, long long new_expiry) {
    UNUSED(getExpiry);
    UNUSED(old_expiry);
    UNUSED(new_expiry);

    if (vsetBucketSingle(bucket) == old_entry) {
        return vsetBucketFromSingle(new_entry);
    }
    return vsetBucketFromNone();
}

static inline vsetBucket *vsetBucketUpdateEntry_VECTOR(vsetBucket *bucket, vsetGetExpiryFunc getExpiry, void *old_entry, void *new_entry, long long old_expiry, long long new_expiry) {
    UNUSED(getExpiry);
    UNUSED(old_expiry);
    UNUSED(new_expiry);

    pVector *pv = vsetBucketVector(bucket);
    uint32_t idx = pvFind(pv, old_entry);
    /* in case we did not locate the entry, just return NONE bucket */
    if (idx == pvLen(pv))
        return vsetBucketFromNone();
    pvSet(pv, idx, new_entry);
    return bucket;
}

static inline vsetBucket *vsetBucketUpdateEntry_HASHTABLE(vsetBucket *bucket, vsetGetExpiryFunc getExpiry, void *old_entry, void *new_entry, long long old_expiry, long long new_expiry) {
    UNUSED(getExpiry);
    UNUSED(old_expiry);
    UNUSED(new_expiry);

    /* In this case no need to change anything. */
    if (old_entry == new_entry)
        return bucket;

    hashtable *ht = vsetBucketHashtable(bucket);
    hashtableDelete(ht, old_entry);
    assert(hashtableAdd(ht, new_entry));
    return bucket;
}

static inline vsetBucket *vsetBucketUpdateEntry_RAX(vsetBucket *target, vsetGetExpiryFunc getExpiry, void *old_entry, void *new_entry, long long old_expiry, long long new_expiry) {
    unsigned char key[VSET_BUCKET_KEY_LEN] = {0};
    size_t key_len;
    long long bucket_ts;
    rax *expiry_buckets = vsetBucketRax(target);
    raxNode *node;
    /* In case new and old are to be updated in the same bucket - just update the bucket. */
    bool update_bucket = (get_bucket_ts(old_expiry) == get_bucket_ts(new_expiry));
    vsetBucket *bucket = findBucket(expiry_buckets, old_expiry, key, &key_len, &bucket_ts, &node);

    if (!update_bucket) {
        /* if the old and new entries are in different buckets, remove the old entry and add the new one. */
        if (removeEntryFromRaxBucket(target, getExpiry, old_entry, bucket, key, key_len, NULL, node))
            target = insertToBucket_RAX(getExpiry, target, new_entry, new_expiry);
        else
            return vsetBucketFromNone();
    } else {
        /* Just update the current bucket */
        switch (vsetBucketType(bucket)) {
        case VSET_BUCKET_NONE:
            /* No bucket means there is no such old entry. return NONE */
            return vsetBucketFromNone();
        case VSET_BUCKET_SINGLE:
            bucket = vsetBucketUpdateEntry_SINGLE(bucket, getExpiry, old_entry, new_entry, old_expiry, new_expiry);
            break;
        case VSET_BUCKET_VECTOR:
            bucket = vsetBucketUpdateEntry_VECTOR(bucket, getExpiry, old_entry, new_entry, old_expiry, new_expiry);
            break;
        case VSET_BUCKET_HT:
            bucket = vsetBucketUpdateEntry_HASHTABLE(bucket, getExpiry, old_entry, new_entry, old_expiry, new_expiry);
            break;
        default:
            panic("Unknown bucket type to update entry");
        }
        if (bucket)
            raxSetData(node, bucket);
        else
            return vsetBucketFromNone();
    }
    return target;
}

/**
 * Updates an existing entry in the volatile set (vset), optionally replacing it
 * with a new entry and expiration time.
 *
 * This function provides a unified interface for removing an old entry and
 * adding a new one. It supports three main cases:
 *
 *  1. Entry identity or expiry time didn't change:
 *     If the `old_entry` and `new_entry` are the same, and their expiration
 *     timestamps are also equal, the function returns early with no action taken.
 *
 *  2. Removal of the old entry:
 *     If `old_entry` is provided (i.e., not NULL) and its old expiration time
 *     is valid (`old_expiry != -1`), the function will remove it from the set.
 *
 *     Note: Since the object might already be deallocated (or changed), the
 *     expiration time is passed explicitly as an argument, rather than
 *     relying on `getExpiry(old_entry)` which might not be safe to call.
 *
 *  3. Insertion of the new entry:
 *     If `new_entry` is provided (i.e., not NULL) and its new expiration time
 *     is valid (`new_expiry != -1`), the function will insert it into the set.
 *
 * The function assumes both `vsetRemoveEntryWithExpiry()` and
 * `vsetAddEntry()` succeed. It uses assertions to enforce this at runtime,
 * assuming this function is used in trusted code paths.
 *
 * Notes:
 *  - The update is not atomic. If the removal fails (assertion fails),
 *    insertion of the new entry does not occur.
 *  - If the new entry is the same as the old one, but the expiry changed,
 *    the entry is effectively reinserted in the correct bucket.
 *  - This is useful for renewal or replacement logic where entries may
 *    need to change time buckets due to updated TTLs or key mutation.
 *
 * Return value:
 *     Always returns true on success.
 *     In case of assertion failures, the program will abort.
 *
 * Example usage:
 *     vsetUpdateEntry(myset, getExpiry, old_ptr, new_ptr, old_ts, new_ts);
 */
bool vsetUpdateEntry(vset *set, vsetGetExpiryFunc getExpiry, void *old_entry, void *new_entry, long long old_expiry, long long new_expiry) {
    assert(*set);
    /* Nothing to do */
    if (old_entry == new_entry && old_expiry == new_expiry)
        return true;
    vsetBucket *updated = vsetBucketFromNone();
    /* case 1 - both entries were tracked. update the bucket */
    if (old_entry && old_expiry != -1 && new_entry && new_expiry != -1) {
        switch (vsetBucketType(*set)) {
        case VSET_BUCKET_NONE:
            return false;
        case VSET_BUCKET_SINGLE:
            updated = vsetBucketUpdateEntry_SINGLE(*set, getExpiry, old_entry, new_entry, old_expiry, new_expiry);
            break;
        case VSET_BUCKET_VECTOR:
            if (old_expiry != new_expiry) {
                /* NOTE! - in this specific case we might have changed the vector order - need to sort it again (NLogN) */
                /* or remove it from the vector and re-add it (N+LogN). the later also looks cleaner... */
                if (!vsetRemoveEntryWithExpiry(set, getExpiry, old_entry, old_expiry))
                    return false;
                return vsetAddEntry(set, getExpiry, new_entry);
            }
            /* We are just updating the entry ref, so sorting is not impacted */
            updated = vsetBucketUpdateEntry_VECTOR(*set, getExpiry, old_entry, new_entry, old_expiry, new_expiry);
            break;

        case VSET_BUCKET_RAX:
            updated = vsetBucketUpdateEntry_RAX(*set, getExpiry, old_entry, new_entry, old_expiry, new_expiry);
        }
        if (updated == VSET_NONE_BUCKET_PTR)
            return false;
        *set = updated;
        return true;
    }
    /* case 2 - old entry was not tracked. just add the new entry */
    else if ((!old_entry || old_expiry == -1) && new_entry && new_expiry != -1)
        return vsetAddEntry(set, getExpiry, new_entry);
    /* case 3 - old entry was tracked. new entry is not. just remove the old entry */
    else if ((!new_entry || new_expiry == -1) && old_entry && old_expiry != -1)
        /* We cannot take the expiration time from the removed entry, since it might not be allocated anymore.
         * For this reason we ask the API user to provide us the removed entry expiration time. */
        return vsetRemoveEntryWithExpiry(set, getExpiry, old_entry, old_expiry);
    else
        return false;

    return false;
}

/* vsetPopExpired - Remove expired entries from a volatile set up to a maximum count.
 *
 * Parameters:
 *     set: Pointer to the volatile set (vset *) to operate on.
 *     getExpiry: Function to retrieve the expiration time from an entry.
 *     expiryFunc: Function to call on each expired entry (e.g., to free or notify).
 *     now: Current time in milliseconds used to compare against expiry times.
 *     max_count: Maximum number of expired entries to remove.
 *     ctx: Opaque context pointer passed through to the expiryFunc callback.
 *
 * This function delegates expiration popping to a type-specific handler based on the
 * internal bucket type of the set. It supports various bucket encodings:
 *   - NONE
 *   - SINGLE
 *   - VECTOR
 *   - RAX (radix tree)
 *   - HT (hashtable)
 *
 * Returns the number of expired entries successfully removed (and passed to expiryFunc).
 *
 * Panics if the bucket type is unknown or unsupported.
 *
 * Return:
 *     Number of expired entries removed (size_t). */
size_t vsetRemoveExpired(vset *set, vsetGetExpiryFunc getExpiry, vsetExpiryFunc expiryFunc, mstime_t now, size_t max_count, void *ctx) {
    vsetBucket *bucket = *set;
    int bucket_type = vsetBucketType(bucket);
    switch (bucket_type) {
    case VSET_BUCKET_NONE:
        return vsetBucketRemoveExpired_NONE(set, getExpiry, expiryFunc, now, max_count, ctx);
        break;
    case VSET_BUCKET_RAX:
        return vsetBucketRemoveExpired_RAX(set, getExpiry, expiryFunc, now, max_count, ctx);
        break;
    case VSET_BUCKET_SINGLE:
        return vsetBucketRemoveExpired_SINGLE(set, getExpiry, expiryFunc, now, max_count, ctx);
        break;
    case VSET_BUCKET_VECTOR:
        return vsetBucketRemoveExpired_VECTOR(set, getExpiry, expiryFunc, now, max_count, ctx);
        break;
    case VSET_BUCKET_HT:
        return vsetBucketRemoveExpired_HASHTABLE(set, getExpiry, expiryFunc, now, max_count, ctx);
        break;
    default:
        panic("Unknown volatile set bucket type in vsetPopExpired");
    }
    return 0;
}

/* vsetEstimatedEarliestExpiry - Estimate the earliest expiration time in a volatile set.
 *
 * Parameters:
 *     set: Pointer to the volatile set (vset *) to inspect.
 *     getExpiry: Callback function used to extract the expiration time from a set entry.
 *
 * Returns the earliest expiration time based on the structure of the volatile set.
 * This is an *approximate* value:
 *   - For bucketed types (e.g., radix tree, vector), it returns the expiry of the first bucket or entry,
 *     which may not be the actual earliest expiring item.
 *   - For single-entry sets, it returns the expiry of the sole item.
 *   - For VSET_BUCKET_NONE, it returns -1 to indicate there is no data.
 *
 * Supported bucket types:
 *   - VSET_BUCKET_SINGLE
 *   - VSET_BUCKET_VECTOR
 *   - VSET_BUCKET_RAX
 *
 * Panics if called with an unsupported bucket type.
 *
 * Return:
 *     Estimated earliest expiry time in milliseconds, or -1 if the set is empty. */
long long vsetEstimatedEarliestExpiry(vset *set, vsetGetExpiryFunc getExpiry) {
    int set_type = vsetBucketType(*set);
    void *entry = NULL;
    long long expiry;
    switch (set_type) {
    case VSET_BUCKET_NONE:
        return -1;
        break;
    case VSET_BUCKET_RAX: {
        rax *r = vsetBucketRax(*set);
        raxIterator it;
        raxStart(&it, r);
        expiry = decodeExpiryKey(it.key);
        raxStop(&it);
        break;
    }
    case VSET_BUCKET_SINGLE: {
        entry = vsetBucketSingle(*set);
        expiry = getExpiry(entry);
        break;
    }
    case VSET_BUCKET_VECTOR: {
        entry = pvGet(vsetBucketVector(*set), 0);
        expiry = getExpiry(entry);
        break;
    }
    default:
        panic("Unsupported vset encoding type. Only supported types are single, vector or rax");
    }
    return expiry;
}

/* Advances the volatile set iterator to the next entry.
 *
 * This function handles iteration over various bucket types in the set. It attempts
 * to return the next valid entry, updating the iterator state accordingly.
 *
 * If the current bucket is exhausted, the iterator automatically switches back to
 * the parent bucket (typically used when iterating nested structures, such as RAX buckets).
 *
 * Parameters:
 *   - it: Pointer to an initialized vsetInternalIterator.
 *   - entryptr: Output pointer to receive the next entry.
 *
 * Returns:
 *   - true if a next entry is found.
 *   - false if iteration is complete. */
bool vsetNext(vsetIterator *iter, void **entryptr) {
    vsetInternalIterator *it = iteratorFromOpaque(iter);
    vsetBucket *bucket = it->bucket;
    int bucket_type = vsetBucketType(bucket);
    int ret = 0;
    switch (bucket_type) {
    case VSET_BUCKET_NONE:
        return vsetBucketNext_NONE(it, entryptr);
        break;
    case VSET_BUCKET_RAX:
        return vsetBucketNext_RAX(it, entryptr);
        break;
    case VSET_BUCKET_SINGLE:
        ret = vsetBucketNext_SINGLE(it, entryptr);
        break;
    case VSET_BUCKET_VECTOR:
        ret = vsetBucketNext_VECTOR(it, entryptr);
        break;
    case VSET_BUCKET_HT:
        ret = vsetBucketNext_HASHTABLE(it, entryptr);
        break;
    default:
        panic("Unknown volatile set bucket type in vsetNext");
    }
    if (ret == 0) {
        /* continue iterating the parent bucket */
        it->iteration_state = vsetBucketType(it->parent_bucket);
        it->bucket = it->parent_bucket;
        return vsetNext(opaqueFromIterator(it), entryptr);
    }
    return ret == 1;
}

size_t vsetMemUsage(vset *set) {
    int bucket_type = vsetBucketType(*set);
    switch (bucket_type) {
    case VSET_BUCKET_NONE:
        return vsetBucketMemUsage_NONE(*set);
    case VSET_BUCKET_SINGLE:
        return vsetBucketMemUsage_SINGLE(*set);
    case VSET_BUCKET_VECTOR:
        return vsetBucketMemUsage_VECTOR(*set);
    case VSET_BUCKET_HT:
        panic("Unsupported hashtable bucket type for vset");
    case VSET_BUCKET_RAX:
        return vsetBucketMemUsage_RAX(*set);
    default:
        panic("Unknown set type encountered in vsetMemUsage");
    }
    return 0;
}

/* Initializes a volatile set iterator.
 *
 * This function prepares the iterator for scanning a volatile set from the beginning.
 * It sets the internal state, pointing to the main set bucket, and uses VSET_BUCKET_NONE
 * as an initial placeholder to transition correctly into the actual bucket logic.
 *
 * Parameters:
 *   - set: Pointer to the volatile set to iterate.
 *   - it: Pointer to a vsetInternalIterator structure to initialize. */
void vsetInitIterator(vset *set, vsetIterator *iter) {
    vsetInternalIterator *it = iteratorFromOpaque(iter);
    it->iteration_state = VSET_BUCKET_NONE; /*lets start by going to the first bucket. */
    it->bucket = *set;
    it->bucket_ts = -1;
    it->parent_bucket = vsetBucketFromNone();
}

/* Finalizes and cleans up an active volatile set iterator.
 *
 * Some internal iterators (e.g., RAX, hashtable) allocate temporary state.
 * This function ensures proper cleanup of those structures when the iteration is done.
 *
 * Parameters:
 *   - it: Pointer to the vsetInternalIterator that was previously initialized with vsetInitIterator(). */
void vsetResetIterator(vsetIterator *iter) {
    vsetInternalIterator *it = iteratorFromOpaque(iter);
    int bucket_type = vsetBucketType(it->bucket);
    int parent_bucket_type = vsetBucketType(it->parent_bucket);
    if (parent_bucket_type == VSET_BUCKET_RAX)
        raxStop(&it->riter);
    if (bucket_type == VSET_BUCKET_HT)
        hashtableResetIterator(&it->hiter);
}

/* Initializes an empty volatile set.
 *
 * The function sets the set to its initial state by assigning a "NONE" bucket.
 * This is the starting point for all volatile sets before entries are inserted.
 *
 * Parameters:
 *   - set: Pointer to the volatile set to initialize. */
void vsetInit(vset *set) {
    *set = vsetBucketFromNone();
}

/* Clears the volatile set, freeing all memory used for internal buckets.
 *
 * This function deallocates all internal data structures used by the set (buckets, vectors,
 * hash tables, etc.). It does NOT free the entries themselves, since the set only holds
 * references.
 *
 * After this call, the set is reset to an empty state.
 *
 * Parameters:
 *   - set: Pointer to the volatile set to clear. */
void vsetClear(vset *set) {
    if (*set == VSET_NONE_BUCKET_PTR) return;
    freeVsetBucket(*set);
    *set = vsetBucketFromNone();
}

/* Same as calling vsetClear, but also de-initialize the set.
 * After this call you will have to call vsetInit again in order to continue using the set. */
void vsetRelease(vset *set) {
    vsetClear(set);
    *set = NULL;
}

/* Return true in case this set is an initialized set and false otherwise. */
bool vsetIsValid(vset *set) {
    if (set && *set) {
        switch (vsetBucketType(*set)) {
        case VSET_BUCKET_NONE:
        case VSET_BUCKET_SINGLE:
        case VSET_BUCKET_VECTOR:
        case VSET_BUCKET_HT:
        case VSET_BUCKET_RAX:
            return true;
        }
    }
    return false;
}

/* Checks whether a volatile set is empty.
 *
 * This function simply checks if the set's current bucket type is VSET_BUCKET_NONE.
 *
 * Parameters:
 *   - set: Pointer to the volatile set.
 *
 * Returns:
 *   - true if the set contains no entries.
 *   - false otherwise. */
bool vsetIsEmpty(vset *set) {
    assert(*set);
    return vsetBucketType(*set) == VSET_BUCKET_NONE;
}

/**************** Defrag Logic *********************/
static struct vsetDefragState {
    long long bucket_ts;
    size_t bucket_cursor;
} defragState;

static size_t vsetBucketDefrag_VECTOR(vsetBucket **bucket, size_t cursor, void *(*defragfn)(void *)) {
    UNUSED(cursor);
    pVector *pv = vsetBucketVector(*bucket);
    pv = defragfn(pv);
    if (pv)
        *bucket = vsetBucketFromVector(pv);
    return 0;
}

static size_t vsetBucketDefrag_HASHTABLE(vsetBucket **bucket, size_t cursor, void *(*defragfn)(void *)) {
    hashtable *ht = vsetBucketHashtable(*bucket);
    if (cursor == 0) {
        /* First time we enter this hashtable, defrag the tables first. */
        hashtable *new_ht = hashtableDefragTables(ht, defragfn);
        if (new_ht) {
            ht = new_ht;
            *bucket = vsetBucketFromHashtable(ht);
        }
    }
    return hashtableScanDefrag(ht, cursor, NULL, NULL, defragfn, 0);
}

static size_t vsetBucketDefrag_RAX(vsetBucket **bucket, size_t cursor, void *(*defragfn)(void *), int (*defragRaxNode)(raxNode **)) {
    struct vsetDefragState *state = (struct vsetDefragState *)cursor;
    size_t bucket_cursor = 0;
    unsigned char key[VSET_BUCKET_KEY_LEN] = {0};
    size_t key_len;
    long long bucket_ts;
    rax *r = vsetBucketRax(*bucket);
    raxIterator ri;

    /* init the state if this is the first time we enter the bucket */
    if (!state) {
        state = &defragState;
        state->bucket_ts = -1;
        state->bucket_cursor = 0;
        if ((r = defragfn(r))) *bucket = vsetBucketFromRax(r);
        r = vsetBucketRax(*bucket);
    }
    raxStart(&ri, r);
    ri.node_cb = defragRaxNode;
    if (state->bucket_ts < 0) {
        /* No prev timestamp, meaning we are starting a new RAX bucket scan */
        assert(raxSeek(&ri, "^", NULL, 0));
        assert(raxNext(&ri)); /* there MUST be at least one bucket! */
        bucket_ts = decodeExpiryKey(ri.key);
    } else {
        /* we are continuing a RAX bucket scan. lets try and locate the last scanned bucket.
         * If not found we can search for the next one. */
        key_len = encodeExpiryKey(state->bucket_ts, key);
        if (state->bucket_cursor) {
            /* We were in the middle of scanning a bucket. lets try and continue there.
             * It is possible that this bucket was deleted. if so we will get to a new bucket
             * which is also fine. */
            assert(raxSeek(&ri, ">=", key, key_len));
        } else {
            /* in case we completed the last bucket, lets progress to a later bucket */
            assert(raxSeek(&ri, ">", key, key_len));
        }
        /* in case we reached the end of the RAX, we are done. */
        if (!raxNext(&ri)) {
            return 0;
        }
        bucket_ts = decodeExpiryKey(ri.key);
        if (state->bucket_ts != bucket_ts) {
            /* if this is a new bucket, lets start from the beginning */
            bucket_cursor = 0;
        } else {
            bucket_cursor = state->bucket_cursor;
        }
    }
    raxStop(&ri);
    vsetBucket *time_bucket = ri.data;
    switch (vsetBucketType(time_bucket)) {
    case VSET_BUCKET_NONE:
    case VSET_BUCKET_SINGLE:
        bucket_cursor = 0;
        break;
    case VSET_BUCKET_VECTOR:
        bucket_cursor = vsetBucketDefrag_VECTOR(&time_bucket, bucket_cursor, defragfn);
        if (time_bucket != ri.data)
            raxSetData(ri.node, time_bucket);
        break;
    case VSET_BUCKET_HT:
        bucket_cursor = vsetBucketDefrag_HASHTABLE(&time_bucket, bucket_cursor, defragfn);
        if (time_bucket != ri.data)
            raxSetData(ri.node, time_bucket);
        break;
    default:
        panic("Unsupported vset bucket type for RAX bucket. Only supported types are single, vector or hashtable");
    }
    /* if we reached here, we are not done. lets return the state and next time we can continue from this bucket. */
    state->bucket_ts = bucket_ts;
    state->bucket_cursor = bucket_cursor;
    return (size_t)state;
}

/* Defrag callback for radix tree iterator, called for each node,
 * used in order to defrag the nodes allocations. */
static int defragRaxNode(raxNode **noderef) {
    raxNode *newnode = activeDefragAlloc(*noderef);
    if (newnode) {
        *noderef = newnode;
        return 1;
    }
    return 0;
}

size_t vsetScanDefrag(vset *set, size_t cursor, void *(*defragfn)(void *)) {
    switch (vsetBucketType(*set)) {
    case VSET_BUCKET_NONE:
    case VSET_BUCKET_SINGLE:
        /* nothing to do */
        return 0;
    case VSET_BUCKET_VECTOR:
        return vsetBucketDefrag_VECTOR(set, cursor, defragfn);
    case VSET_BUCKET_RAX:
        return vsetBucketDefrag_RAX(set, cursor, defragfn, defragRaxNode);
    default:
        panic("Unknown vset node type to defrag");
    }
    return 0;
}
