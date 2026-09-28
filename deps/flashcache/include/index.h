#ifndef __FLASHCACHE_INDEX_H
#define __FLASHCACHE_INDEX_H

#include "include/bitset.h"
#include "include/log_entry.h"
#include "include/flashcache_common.h"
#include "include/staging_buffer.h"

/* Each item present in the store has a corresponding index entry. The index
 * entry points to the location where the key and value is stored. When an
 * item is added to the store, a index entry is created for this item. This
 * index entry is then added to the index hash table.
 *
 * When the item has not been flushed to flash, the index entry points to the
 * item in staging buffer. Once the item has been written to flash, it points
 * to the offset in the log file.
 */
typedef struct indexEntry {
    union {
        struct stagingBufferEntry *staging_buffer_entry;
        struct logEntry log_entry;
    } item_entry;
    struct indexEntry *next;
} indexEntry;

typedef struct indexIteratorCallbackDetails {
    // Callback called during iterating over the hash buckets
    void (*iteration_callback)(void *context, size_t hash_bucket_idx, indexEntry *head_entry);

    // Callback called indicating the completion of iteration
    void (*completion_callback)(void *context);

    // User provided context passed in each callback invocation
    void *context;
} indexIteratorCallbackDetails;

/*
 * Index Iterator Status
 */
typedef enum {
    // Iterator is Running
    RUNNING,

    // Iterator is Paused
    PAUSED,

    // Iterator is not Running
    NOT_RUNNING
} indexIteratorStatus;

/* Iterator that helps iterate over each item present at the point in time when the iterator was created.
 * If an indexEntry is added or removed from a hash bucket after the iterator was created and the hash bucket
 * has not yet been iterated, the hash bucket is expeditely iterated prior to the mutation.
 */
typedef struct pointInTimeIndexIterator {
    // Next hash bucket to iterate
    size_t next_hash_bucket;

    // Bitset of all the hash buckets processed
    bitset *processed_hash_buckets;

    // Contains the detail about the callback called during iterating over the hash buckets
    indexIteratorCallbackDetails callback_details;

    // Status of index iterator.
    indexIteratorStatus status;

    // Number of buckets to process in a single iteration
    size_t batch_size;
} pointInTimeIndexIterator;

/* Index contains a hash table that maintain the entry for each item. The hash
 * table is allocated as a contigous array of hash bucket. Each hash bucket contains
 * a linked list of index entries for items that fall into that bucket.
 */
typedef struct flashcacheIndex {
    indexEntry **table;
    size_t num_items;  // Total number of items in the hash table
    size_t num_hash_bucket_used;  // Total number of hash buckets that has atleast 1 item

    size_t collision_bits_used;  // Number of Collision bits used.
    size_t base_size_bits;  // Number of bits in base size of table.

    flashcache_hash_function hash_function;
    pointInTimeIndexIterator *custom_iterator;  // Custom Iterator used for other purposes like snapshotting
    pointInTimeIndexIterator *growth_iterator;  // Dedicated iterator for growth operation
} flashcacheIndex;

// Creates a index.
struct flashcacheIndex *indexCreate(size_t initial_size,
        flashcache_hash_function hash_function);

// Add a new item to the index.
struct indexEntry *indexAddItem(struct flashcacheIndex *index, char const *key,
        size_t key_len, struct stagingBufferEntry *item);

// Update the item with the specified key and key_len and stored at the specified log offset
indexEntry *indexUpdateItem(flashcacheIndex *index, char const *key, size_t key_len,
        size_t log_offset, struct stagingBufferEntry *staging_buffer_entry);

// If the item exist in the index, returns the its entry else return NULL
struct indexEntry *indexGetItem(struct flashcacheIndex *index, char const *key, size_t key_len,
        size_t log_offset);

// Return the head index entry of the hash bucket in which the key map into.
struct indexEntry *indexGetHeadEntry(struct flashcacheIndex *index, char const *key, size_t key_len);

// Deletes the item represented by the provided index entry from the hash table
void indexDeleteItem(struct flashcacheIndex *index, char const *key,
        size_t key_len, struct indexEntry *index_entry);

// Creates a Active point in time iterator to iterate over the index
void indexCreateCustomIterator(flashcacheIndex *index,
                               indexIteratorCallbackDetails *callback_details, size_t batch_size);

// Iterate over a batch of hash buckets if there is an custom point in time iterator. Returns the number of hash
// buckets iterated in the invocation.
size_t indexIterateCustomIteratorIfRequired(struct flashcacheIndex *index);

// Removes the active point in time iterator if present
void indexReleaseCustomIterator(struct flashcacheIndex *index);

// Deletes all the existing items in the index and resize the index to the specified size
void indexRecreate(flashcacheIndex *index, size_t base_size_bits, size_t collision_bits_used,
                   indexIteratorStatus growth_status, size_t next_hash_bucket,
                   flashcache_hash_function hash_function);

// Deletes all the resources hold by index and frees the index. The caller should
// not use the index after releasing it.
void indexRelease(struct flashcacheIndex *index);

// Grows the Index table if required. Returns 1 when growth iterator status is RUNNING/PAUSED else 0
int indexGrowIfRequired(flashcacheIndex *index);

// Pauses the index growth operation
void indexPauseGrowth(flashcacheIndex *index);

// Unpauses the index growth operation
void indexUnpauseGrowth(flashcacheIndex *index);

// Returns no. of times index has grown
size_t indexNumGrowth(struct flashcacheIndex *index);

// Return the number of items in the index.
size_t indexGetNumItems(struct flashcacheIndex *index);

#endif  // __FLASHCACHE_INDEX_H
