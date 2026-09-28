#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "include/util.h"
#include "include/index.h"
#include "include/serialization.h"

#define INDEX_TABLE_LOAD_FACTOR_THRESHOLD (5.0)
#define INDEX_TABLE_GROWTH_ITERATION_BATCH_SIZE (4)

// Return the current table size of an index. Non static for testing purpose.
size_t indexTableSize(flashcacheIndex *index) {
    return (1LL << (index->base_size_bits + index->collision_bits_used));
}

static size_t indexTableSizeIncludingNewGrowthBucket(flashcacheIndex *index) {
    return indexTableSize(index) + index->growth_iterator->next_hash_bucket;
}

static void indexDeleteItemInHashBucket(flashcacheIndex *index,
        size_t hash_bucket_idx, indexEntry *index_entry) {
    indexEntry *prev_entry = index->table[hash_bucket_idx];

    if (prev_entry == index_entry) {
        index->table[hash_bucket_idx] = index_entry->next;
    } else {
        while (prev_entry->next != index_entry) {
            prev_entry = prev_entry->next;
        }
        prev_entry->next = index_entry->next;
    }
    fcFree(index_entry);

    // If there are no items in the hash bucket reduce the count of used hash buckets
    if (!index->table[hash_bucket_idx]) {
        index->num_hash_bucket_used--;
    }

    flashcacheAssert(index->num_items > 0);
    index->num_items--;
}

static void iterateHashBucketIfRequired(flashcacheIndex *index, size_t bucket_idx,
                                        pointInTimeIndexIterator *iterator) {
    if (iterator == NULL) {
        return;
    }

    indexIteratorCallbackDetails *callback_details = &(iterator->callback_details);

    if ((iterator->processed_hash_buckets == NULL) || (!bitsetGet(iterator->processed_hash_buckets, bucket_idx))) {
        callback_details->iteration_callback(callback_details->context, bucket_idx,
                                             index->table[bucket_idx]);
    }

    if (iterator->processed_hash_buckets != NULL) {
        bitsetSet(iterator->processed_hash_buckets, bucket_idx);
    }
}

// Callback for Bucket Iteration while index growth operation. Non static for testing purpose.
void indexGrowthIterationCallback(void *context, size_t hash_bucket_idx, indexEntry *head_entry) {
    struct flashcacheIndex *index = (flashcacheIndex *) context;
    size_t current_table_size = indexTableSize(index);
    size_t new_bucket_idx = current_table_size + hash_bucket_idx;
    index->table[new_bucket_idx] = NULL;

    // Return if bucket has no item
    if (!head_entry) {
        return;
    }

    indexEntry *last_new_bucket_entry = NULL;
    indexEntry *curr_entry = head_entry;
    indexEntry *prev_entry = NULL;

    while (curr_entry != NULL) {
        indexEntry *next_entry = curr_entry->next;

        // Fetch collision hash either using log entry or staging_buffer_entry
        size_t collision_hash;
        if (curr_entry->item_entry.log_entry.on_flash) {
            collision_hash = curr_entry->item_entry.log_entry.hash;
        } else {
            collision_hash = curr_entry->item_entry.staging_buffer_entry->log_entry.hash;
        }

        // Determine if we need to move the entry to new bucket or not. Lets understand it with example
        // Lets say, collision_hash = 0101010010101001 and collision_bits_used = 2 (100 in binary)
        // So, need_to_move = 0101010010101001 & 100 = 0. Hence we dont need to move this entry to new bucket.
        size_t need_to_move = collision_hash & (1LL << (index->collision_bits_used));
        if (need_to_move) {
            // Removing the entry from existing bucket
            if (curr_entry == index->table[hash_bucket_idx]) {  // When curr_entry is the first entry in bucket
                flashcacheAssert(prev_entry == NULL);
                index->table[hash_bucket_idx] = next_entry;
            } else {
                prev_entry->next = next_entry;
            }

            // If a bucket gets empty after moving the index entry, decrement the count of used hash bucket
            if (!index->table[hash_bucket_idx]) {
                index->num_hash_bucket_used--;
            }

            // Moving the entry to new bucket. The new item is added to the end of item list in the new bucket.
            // The items needs to maintain the same order in the new bucket. The reason for this for the following
            // reason: Lets assume there are 2 entries in a bucket one for key 'A1' and another for key 'A2'. Both
            // the items have same collision hash. Entry for key 'A1' follows the entry for key 'A2' in the hash
            // bucket. A read request arries for key 'A2'. As entry of key 'A1' is before 'A2', the key 'A1' is read
            // from flash first. While this is happening, the bucket gets processed for growing the table. During that
            // both the keys get moved to the new bucket. Now if the order of the entries were reversed, entry for
            // key 'A2' will be the head in the new bucket followed by entry for key 'A1'. So when the read of key 'A1'
            // is done, we find that the key does not match to the key in the request('A2'). So we move to the next
            // entry. But after growing the table, there is no entry after 'A1' so the read return a NULL value which
            // is incorrect. Due to this reason it is *essential* to add the entries in the new bucket in the same
            // order as they were in the old bucket.
            if (last_new_bucket_entry == NULL) {
                index->table[new_bucket_idx] = curr_entry;
                index->num_hash_bucket_used++;
            } else {
                last_new_bucket_entry->next = curr_entry;
            }

            // The item being moved to the new bucket is added to the end so its next bucket is set to NULL
            curr_entry->next = NULL;
            last_new_bucket_entry = curr_entry;
        } else {
            prev_entry = curr_entry;
        }
        curr_entry = next_entry;
    }
}

// Completion callback after completion of index growth of a database.
static void indexGrowthCompletionCallback(void *context) {
    flashcacheIndex *index = (flashcacheIndex *) context;

    // Reinitializing the Iterator status and next_hash_bucket
    index->growth_iterator->status = NOT_RUNNING;
    index->growth_iterator->next_hash_bucket = 0;
    index->collision_bits_used++;  // Increasing the collision bit after completion of growth operation
}

// Fetches index iterator growth callback details
static indexIteratorCallbackDetails *getIndexGrowthCallbackDetails(struct flashcacheIndex *index) {
    indexIteratorCallbackDetails *index_growth_callback_details =
            (indexIteratorCallbackDetails *) fcCalloc(sizeof(indexIteratorCallbackDetails), 1);
    index_growth_callback_details->iteration_callback = indexGrowthIterationCallback;
    index_growth_callback_details->completion_callback = indexGrowthCompletionCallback;
    index_growth_callback_details->context = (void *) index;
    return index_growth_callback_details;
}

// Checks if we need to start growing the index table.
// Returns 1 if Load Factor of the table exceeds a threshold of 5, else 0.
static int indexCheckIfTableGrowthRequired(size_t num_items, size_t num_buckets) {
    double load_factor = ((double) num_items) / num_buckets;
    return load_factor > INDEX_TABLE_LOAD_FACTOR_THRESHOLD ? 1 : 0;
}

// Returns the Index hash based on Growth Iteration Status. Non static for testing purpose.
size_t getIndexHash(flashcacheIndex *index, char const *key, size_t key_len) {
    flashcacheAssert(index->growth_iterator != NULL);

    size_t hash = index->hash_function(key, key_len);
    size_t collision_bits_used = index->collision_bits_used;
    size_t base_size_bits = index->base_size_bits;
    size_t index_hash = computeIndexHash(hash, collision_bits_used, base_size_bits);
    switch (index->growth_iterator->status) {
        case NOT_RUNNING:
            return index_hash;
        case PAUSED:
        case RUNNING:
            return (index_hash < index->growth_iterator->next_hash_bucket) ?
                   computeIndexHash(hash, collision_bits_used + 1, base_size_bits) : index_hash;
    }
    flashcacheAssertWithLogging(0, "Implementation error as this part of code "
                                   "should not be reachable, status: [%d]", index->growth_iterator->status);
    return 0;
}

static pointInTimeIndexIterator *indexCreatePointInTimeIterator(indexIteratorCallbackDetails *callback_details,
                                                                size_t batch_size) {
    pointInTimeIndexIterator *iterator = (pointInTimeIndexIterator *) fcMalloc(sizeof(pointInTimeIndexIterator));
    flashcacheAssert(iterator != NULL);

    iterator->processed_hash_buckets = NULL;
    iterator->next_hash_bucket = 0;
    iterator->callback_details = (*callback_details);
    iterator->status = NOT_RUNNING;
    iterator->batch_size = batch_size;
    return iterator;
}

static size_t indexIterateIfRequired(flashcacheIndex *index,
                                     struct pointInTimeIndexIterator *iterator, size_t table_size) {
    if (iterator == NULL || iterator->status == PAUSED || iterator->status == NOT_RUNNING) {
        return 0;
    }

    size_t iterated_hash_bucket = 0;
    size_t next_bucket = iterator->next_hash_bucket;
    while (next_bucket < table_size) {
        iterateHashBucketIfRequired(index, next_bucket, iterator);
        iterated_hash_bucket++;
        next_bucket++;
        if (iterated_hash_bucket == iterator->batch_size) {
            break;
        }
    }
    iterator->next_hash_bucket = next_bucket;
    if (iterator->next_hash_bucket == table_size) {
        indexIteratorCallbackDetails *callback_details = &(iterator->callback_details);
        callback_details->completion_callback(callback_details->context);
    }
    return iterated_hash_bucket;
}

static void indexReleaseIterator(struct pointInTimeIndexIterator *iterator) {
    if (iterator == NULL) {
        return;
    }
    bitsetRelease(iterator->processed_hash_buckets);
    fcFree(iterator);
    iterator = NULL;
}

// Resize the existing table to new size. Non static for testing purpose.
void indexResizeTable(flashcacheIndex *index, size_t new_size) {
    index->table = (indexEntry **) fcRealloc(index->table, sizeof(indexEntry *) * new_size);
    flashcacheAssert(index->table != NULL);
}

// Creates a Growth point in time iterator to iterate over the index
static void indexCreateGrowthIterator(flashcacheIndex *index,
                               indexIteratorCallbackDetails *callback_details, size_t batch_size) {
    flashcacheAssert(index->growth_iterator == NULL);
    index->growth_iterator = indexCreatePointInTimeIterator(callback_details, batch_size);
}

// Creates a Custom point in time iterator to iterate over the index
void indexCreateCustomIterator(flashcacheIndex *index,
                               indexIteratorCallbackDetails *callback_details, size_t batch_size) {
    flashcacheAssert(index->custom_iterator == NULL);
    size_t table_size = indexTableSizeIncludingNewGrowthBucket(index);
    index->custom_iterator = indexCreatePointInTimeIterator(callback_details, batch_size);
    index->custom_iterator->processed_hash_buckets = bitsetCreate(table_size);
    index->custom_iterator->status = RUNNING;
}

flashcacheIndex *indexCreate(size_t initial_size, flashcache_hash_function hash_function) {
    flashcacheAssert(isPowerOf2(initial_size));

    flashcacheIndex *index = (flashcacheIndex *) fcMalloc(
            sizeof(flashcacheIndex));
    flashcacheAssert(index != NULL);

    index->table = (indexEntry **) fcCalloc(sizeof(indexEntry *), initial_size);
    flashcacheAssert(index->table != NULL);

    index->hash_function = hash_function;
    index->custom_iterator = NULL;
    index->growth_iterator = NULL;
    index->num_items = 0;
    index->num_hash_bucket_used = 0;

    index->base_size_bits = getNumBitsRequired(initial_size);
    index->collision_bits_used = 0;

    indexIteratorCallbackDetails *callback_details = getIndexGrowthCallbackDetails(index);
    indexCreateGrowthIterator(index, callback_details, INDEX_TABLE_GROWTH_ITERATION_BATCH_SIZE);
    fcFree(callback_details);
    return index;
}

indexEntry *indexAddItem(flashcacheIndex *index, char const *key,
        size_t key_len, stagingBufferEntry *item) {
    size_t hash = getIndexHash(index, key, key_len);
    iterateHashBucketIfRequired(index, hash, index->custom_iterator);

    indexEntry *new_entry = (indexEntry *) fcMalloc(sizeof(indexEntry));
    flashcacheAssert(new_entry != NULL);

    new_entry->next = NULL;
    new_entry->item_entry.staging_buffer_entry = item;
    item->index_entry = new_entry;
    item->log_entry.on_flash = 1;
    item->log_entry.hash = computeCollisionHashOfKeyInSerializedItem(
            item->item, index->hash_function);

    indexEntry *prev_entry = index->table[hash];
    index->table[hash] = new_entry;
    new_entry->next = prev_entry;

    index->num_items++;
    // If the new item is the first item in the hash bucket, increment the count of used hash bucket
    if (!prev_entry) {
        index->num_hash_bucket_used++;
    }

    return new_entry;
}

indexEntry *indexAddLogEntry(flashcacheIndex *index, size_t hash_bucket_idx, logEntry *log_entry) {
    flashcacheAssert(hash_bucket_idx < indexTableSizeIncludingNewGrowthBucket(index));
    iterateHashBucketIfRequired(index, hash_bucket_idx, index->custom_iterator);

    indexEntry *new_entry = (indexEntry *) fcMalloc(sizeof(indexEntry));
    flashcacheAssert(new_entry != NULL);

    new_entry->next = NULL;
    new_entry->item_entry.log_entry = *log_entry;

    indexEntry *prev_entry = index->table[hash_bucket_idx];
    index->table[hash_bucket_idx] = new_entry;
    new_entry->next = prev_entry;

    index->num_items++;
    // If the new item is the first item in the hash bucket, increment the count of used hash bucket
    if (!prev_entry) {
        index->num_hash_bucket_used++;
    }

    return new_entry;
}

indexEntry *indexUpdateItem(flashcacheIndex *index, char const *key, size_t key_len,
        size_t log_offset, stagingBufferEntry *staging_buffer_entry) {
    size_t hash_bucket_idx = getIndexHash(index, key, key_len);
    iterateHashBucketIfRequired(index, hash_bucket_idx, index->custom_iterator);

    indexEntry *entry = index->table[hash_bucket_idx];
    while (entry) {
        if (entry->item_entry.log_entry.on_flash &&
                (entry->item_entry.log_entry.trimmed_log_offset == trimLogOffset(log_offset))) {
            staging_buffer_entry->log_entry.on_flash = 1;
            staging_buffer_entry->log_entry.hash = entry->item_entry.log_entry.hash;
            entry->item_entry.staging_buffer_entry = staging_buffer_entry;
            staging_buffer_entry->index_entry = entry;
            return entry;
        }
        entry = entry->next;
    }

    flashcacheAssert(0);
    return NULL;
}

indexEntry *indexGetItem(flashcacheIndex *index, char const *key, size_t key_len, size_t log_offset) {
    indexEntry *entry = indexGetHeadEntry(index, key, key_len);

    while (entry) {
        if (entry->item_entry.log_entry.on_flash &&
                (entry->item_entry.log_entry.trimmed_log_offset == trimLogOffset(log_offset))) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

indexEntry *indexGetHeadEntry(flashcacheIndex *index, char const *key, size_t key_len) {
    size_t hash = getIndexHash(index, key, key_len);

    indexEntry *entry = index->table[hash];
    return entry;
}

void indexDeleteItem(flashcacheIndex *index, char const *key,
        size_t key_len, indexEntry *index_entry) {
    size_t hash_bucket_idx = getIndexHash(index, key, key_len);
    iterateHashBucketIfRequired(index, hash_bucket_idx, index->custom_iterator);
    indexDeleteItemInHashBucket(index, hash_bucket_idx, index_entry);
}

static size_t indexIterateGrowthIteratorIfRequired(flashcacheIndex *index) {
    size_t table_size = indexTableSize(index);
    size_t iterated_hash_bucket = indexIterateIfRequired(index, index->growth_iterator, table_size);
    return iterated_hash_bucket;
}

size_t indexIterateCustomIteratorIfRequired(flashcacheIndex *index) {
    flashcacheAssert(index->growth_iterator != NULL);
    size_t table_size = indexTableSizeIncludingNewGrowthBucket(index);
    size_t iterated_hash_bucket = indexIterateIfRequired(index, index->custom_iterator, table_size);
    if (index->custom_iterator != NULL && index->custom_iterator->next_hash_bucket == table_size) {
        indexReleaseCustomIterator(index);
    }
    return iterated_hash_bucket;
}

// Removes the custom point in time iterator if present
void indexReleaseCustomIterator(flashcacheIndex *index) {
    indexReleaseIterator(index->custom_iterator);
    index->custom_iterator = NULL;
}

// Removes the growth point in time iterator if present
static void indexReleaseGrowthIterator(flashcacheIndex *index) {
    indexReleaseIterator(index->growth_iterator);
    index->growth_iterator = NULL;
}

void indexRecreate(flashcacheIndex *index, size_t base_size_bits, size_t collision_bits_used,
                   indexIteratorStatus growth_status, size_t next_hash_bucket,
                   flashcache_hash_function hash_function) {
    flashcacheAssert(index->custom_iterator == NULL);
    flashcacheAssert(collision_bits_used <= FC_LOG_ENTRY_COLLISION_HASHBITS);
    flashcacheAssert(hash_function != NULL);

    size_t new_size = 1LL << (base_size_bits + collision_bits_used);
    if (growth_status == RUNNING || growth_status == PAUSED) {
        flashcacheAssert(collision_bits_used < FC_LOG_ENTRY_COLLISION_HASHBITS);
        new_size = new_size << 1;  // As index growth is in progress, adjusting the table size accordingly.
    }
    size_t table_size_including_new_growth_bucket = indexTableSizeIncludingNewGrowthBucket(index);
    for (size_t i = 0; i < table_size_including_new_growth_bucket; ++i) {
        while (index->table[i]) {
            indexDeleteItemInHashBucket(index, i, index->table[i]);
        }
    }

    indexReleaseGrowthIterator(index);  // Release the growth iterator.
    if (indexTableSize(index) != new_size) {
        fcFree(index->table);
        index->table = (indexEntry **) fcCalloc(sizeof(indexEntry *), new_size);
        flashcacheAssert(index->table != NULL);
    } else {
        // Set buckets to NULL beyond table_size_including_new_growth_bucket if growth is in progress.
        for (size_t bucket = table_size_including_new_growth_bucket; bucket < new_size; bucket++) {
            index->table[bucket] = NULL;
        }
    }

    index->num_items = 0;
    index->num_hash_bucket_used = 0;
    index->hash_function = hash_function;

    index->base_size_bits = base_size_bits;
    index->collision_bits_used = collision_bits_used;

    indexIteratorCallbackDetails *callback_details = getIndexGrowthCallbackDetails(index);
    indexCreateGrowthIterator(index, callback_details, INDEX_TABLE_GROWTH_ITERATION_BATCH_SIZE);
    index->growth_iterator->status = growth_status;
    index->growth_iterator->next_hash_bucket = next_hash_bucket;
    fcFree(callback_details);
}

void indexRelease(flashcacheIndex *index) {
    flashcacheAssert(index->growth_iterator != NULL);
    for (size_t i = 0; i < indexTableSizeIncludingNewGrowthBucket(index); ++i) {
        while (index->table[i] != NULL) {
            indexDeleteItemInHashBucket(index, i, index->table[i]);
        }
    }
    indexReleaseCustomIterator(index);  // Releases Custom Iterator
    indexReleaseGrowthIterator(index);  // Releases Growth Iterator
    fcFree(index->table);
    fcFree(index);
}

void indexPauseGrowth(flashcacheIndex *index) {
    if (index->growth_iterator->status == RUNNING) {
        index->growth_iterator->status = PAUSED;
    }
}

void indexUnpauseGrowth(flashcacheIndex *index) {
    if (index->growth_iterator->status == PAUSED) {
        index->growth_iterator->status = RUNNING;
    }
}

// Returns 1 when Growth is Running or Paused else 0.
int indexGrowIfRequired(flashcacheIndex *index) {
    // Return if we have consumed all bits of collision hash.
    if (index->collision_bits_used >= FC_LOG_ENTRY_COLLISION_HASHBITS) {
        return 0;
    }
    // Start the growth operation if load factor of table breaches threshold
    if (index->growth_iterator->status == NOT_RUNNING) {
        if (indexCheckIfTableGrowthRequired(index->num_items, indexTableSize(index))) {
            // Doubling the table size for growth operation
            indexResizeTable(index, (indexTableSize(index) << 1));
            index->growth_iterator->status = RUNNING;
        }
    }

    // Start Iterating the table
    if (index->growth_iterator->status == RUNNING) {
        indexIterateGrowthIteratorIfRequired(index);
    }
    return index->growth_iterator->status != NOT_RUNNING ? 1 : 0;
}

// Returns no. of times index table has grown
size_t indexNumGrowth(struct flashcacheIndex *index) {
    return index->collision_bits_used;
}

size_t indexGetNumItems(struct flashcacheIndex *index) {
    return index->num_items;
}
