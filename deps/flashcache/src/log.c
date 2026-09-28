#include <stdlib.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "include/serialization.h"
#include "include/log.h"
#include "include/util.h"
#include "include/crc.h"
#include "include/snapshot_manager.h"
#include "include/snapshot_common.h"

#define FC_SLEEP_INTERVAL_WAITING_TO_FINISH_PENDING_TASK_MICROSECONDS 100
#define FC_DEFAULT_GARBAGE_COLLECTION_MAX_PROCESSING_TIME_MICROSECONDS 50
#define FC_DEFAULT_MAX_BUFFERED_WRITE_SIZE_BYTES (512 * 1024 * 1024)  // 512 MiB
#define FC_DEFAULT_MAX_DYNAMIC_GARBAGE_COLLECTION_RATE (30 * 1024 * 1024)  // 30 MB/s

// Limit the maximum block size used to read log from the tail during garbage collection 200 MiB.
// This bounds the memory required for garbage collection to 400 MiB (as the alive item read would
// be copied to another buffer to be written to the head of the log).
#define FC_MAX_GARBAGE_COLLECTION_BLOCK_SIZE_BYTES (200LL * 1024 * 1024)
#define FC_DEFAULT_WRITE_RATE_WINDOW_SIZE_MICROSECOND (FC_SECOND_TO_MICROSECOND)
#define FC_DEFAULT_ADAPTIVE_GARBAGE_COLLECTION_RATE_ENABLED (1)
#define FC_MAX_GARBAGE_COLLECTION_RATE_FACTOR (10)
#define FC_DEFAULT_GARBAGE_COLLECTION_ALGORITHM (FC_GARBAGE_COLLECTION_LINEAR_WITH_FREE_SPACE)
#define FC_GARBAGE_COLLECTION_LINEAR_WITH_FREE_SPACE_FACTOR (4)

extern void updateStagingBufferSizeMetric(size_t sz);  // Keep semi private as it is needed in two files

static inline void incrementAllocatedLogSize(flashcacheLog *log, uint32_t dbid, size_t length) {
    log->allocated_log_size_bytes += length;
    log->allocated_log_size_bytes_per_db[dbid] += length;
}

static inline void decrementAllocatedLogSize(flashcacheLog *log, uint32_t dbid, size_t length) {
    log->allocated_log_size_bytes -= length;
    log->allocated_log_size_bytes_per_db[dbid] -= length;
}

static inline uint64_t timeInMicrosecond(flashcacheLog *log) {
    return log->monotonic_clock_us();
}

static inline uint64_t timeInSecond(flashcacheLog *log) {
    return timeInMicrosecond(log) / (1000 * 1000);
}

static inline size_t flashcacheMin(size_t l, size_t r) {
    return (l < r) ? l : r;
}

static inline void updateDiskWriteMetrics(flashcacheLog *log, size_t data_size) {
    log->metrics.num_disk_write++;
    log->metrics.total_disk_write_bytes += data_size;
}

static inline void updateDiskReadMetrics(flashcacheLog *log, size_t data_size) {
    log->metrics.num_disk_read++;
    log->metrics.total_disk_read_bytes += data_size;
}

static inline size_t getNumberPagesRequiredToReadItem(size_t offset, size_t item_len) {
    size_t first_byte_page_num = offset / FC_PAGESIZE;
    size_t last_byte_page_num = (offset + item_len - 1) /
        FC_PAGESIZE;

    return (last_byte_page_num - first_byte_page_num + 1);
}

static inline void deleteEntryFromStagingBuffer(flashcacheLog *log,
        stagingBufferEntry *entry, int8_t free_item) {
    if (entry == log->head_entry_being_flushed_to_flash) {
        log->head_entry_being_flushed_to_flash =
            log->head_entry_being_flushed_to_flash->next;
    }
    stagingBufferDeleteEntry(log->staging_buffer, entry, free_item);
}

static inline int isHeadWrappedToStartOfLogFile(flashcacheLog *log) {
    // If tail_offset is equal to head_offset, these 2 states indicate that the head has wrapped to the start of log:
    // 1. There are live items in the log in which case allocated_log_size_bytes would be greater than zero.
    // 3. There are only garbage items in the log in which case allocated_log_size_bytes is zero, however GC is
    // running. A running GC indicates that there are garbage items ahead of tail_offset.
    if (((log->tail_offset == log->head_offset) && (log->allocated_log_size_bytes ||
                    log->garbage_collector_info.is_running)) || (log->tail_offset > log->head_offset)) {
        return 1;
    }
    return 0;
}

static inline size_t getSpaceLeftBeforeWrappingOrTail(flashcacheLog *log) {
    // As the head has wrapped to the front of the log, the space left is the distance
    // from the tail offset
    if (isHeadWrappedToStartOfLogFile(log)) {
        // Tail offset might not be page aligned. However we cannot write to the page where the tail exist,
        // as it will overwrite the content of the log. So the space left is computed using the floored page aligned
        // offset of tail offset.
        return (getFloorPageAlignedOffset(log->tail_offset) - log->head_offset);
    }

    // Bytes left before head wraps to the front of the log file is log file size minus the
    // offset of head in the log file
    return (log->log_size_bytes - log->head_offset);
}

static inline size_t getMaxAllocatedLogSizeBytes(flashcacheLog *log) {
    if (log->overriden_allocatable_log_size_bytes != -1) {
        return log->overriden_allocatable_log_size_bytes;
    }
    return ((size_t) ((((double) log->max_allocated_log_size_percent) / 100) * log->log_size_bytes));
}

static void validateOffsetInRange(flashcacheLog *log, size_t offset) {
    if (!isHeadWrappedToStartOfLogFile(log)) {
        flashcacheAssertWithLogging((offset >= log->tail_offset) && (offset < log->head_offset), "Log offset: [%lu]",
                offset);
    } else {
        flashcacheAssertWithLogging(((offset >= log->tail_offset) && (offset < log->log_size_bytes))
                || (offset < log->head_offset), "Log offset: [%lu]", offset);
    }
}

// Return the sum of the number of index entries that are stored in the log and have the same hash bucket and collision
// bits as the provided key.
int collisionHashCount(flashcacheLog *log, uint32_t dbid, char *key, size_t key_len) {
    int count = 0;
    indexEntry * index_entry = indexGetHeadEntry(log->index_list[dbid], key, key_len);
    while (index_entry) {
        if (index_entry->item_entry.log_entry.on_flash) {
            if (computeCollisionHash(log->hasher.hash_function, key, key_len) ==
                    index_entry->item_entry.log_entry.hash) {
                count++;
            }
        }
        index_entry = index_entry->next;
    }
    return count;
}

// Determines if a deletion request requires resubmission for the whole item for FDB expedition
int shouldResubmitRequestForExpedition(size_t log_offset, int requested_item_with_value) {
    if (snapshotManagerShouldExpediteItem(log_offset) && !requested_item_with_value) {
        return 1;
    }
    return 0;
}

void logFlushStagingBufferIfRequired(flashcacheLog *log, size_t threshold) {
    if (!fioRequestIsEmpty(&(log->log_flush_fio_request))) {
        return;
    }
    flashcacheAssert(log->head_entry_being_flushed_to_flash == NULL);

    if (threshold > log->max_buffered_write_size_bytes) {
        threshold = log->max_buffered_write_size_bytes;
    }

    size_t total_item_size = stagingBufferGetTotalItemSize(log->staging_buffer);
    if ((total_item_size == 0) || (total_item_size < threshold)) {
        return;
    }

    size_t buffer_size = total_item_size;
    if (buffer_size % FC_PAGESIZE) {
        buffer_size = getCeilPageAlignedOffset(buffer_size);
    }

    size_t bytes_before_wrapping_or_tail = getSpaceLeftBeforeWrappingOrTail(log);
    if (buffer_size > bytes_before_wrapping_or_tail) {
        buffer_size = bytes_before_wrapping_or_tail;
    }

    // Always write in the multiple of FC_PAGESIZE to avoid overwrite in flash
    buffer_size = getFloorPageAlignedOffset(buffer_size);

    // No space to write, wait for garbage collection to free some space.
    // TODO: Add logging
    if (!buffer_size) {
        return;
    }

    char *buf = NULL;
    flashcacheAssert(buffer_size && ((buffer_size % FC_PAGESIZE) == 0));
    buf = fcPosixMemalign(FC_PAGESIZE, buffer_size);

    stagingBufferEntry *entry = stagingBufferGetTail(log->staging_buffer);
    size_t pos = 0;
    size_t last_entry_pos = 0;
    while (entry) {
        size_t item_len = entry->item_len;
        flashcacheAssert(item_len % FC_ITEM_ALIGNMENT_BYTES == 0);
        if ((pos + item_len) > buffer_size) {
           break;
        }

        log->head_entry_being_flushed_to_flash = entry;
        size_t trimmed_log_offset = trimLogOffset(log->head_offset + pos);
        flashcacheAssert(getFlagInSerializedItem(entry->item) == 0);
        memcpy(buf + pos, entry->item, entry->item_len);

        size_t additional_pages = getNumberPagesRequiredToReadItem(pos, entry->item_len) - 1;
        if (additional_pages > FC_MAX_ADDITIONAL_PAGES) {
            additional_pages = FC_MAX_ADDITIONAL_PAGES;
        }
        entry->log_entry.additional_pages = (uint8_t) additional_pages;
        entry->log_entry.trimmed_log_offset = trimmed_log_offset;

        last_entry_pos = pos;
        pos += item_len;
        entry = entry->prev;
    }

    // Incase we have multiple empty pages at the end of the buffer because the last item we tried to write
    // did not fit in the space left in the buffer. We reduce the size of buffer in a way that we throw away
    // pages that are completely free.
    buffer_size = getCeilPageAlignedOffset(pos);

    size_t data_size = buffer_size;
    size_t head_offset_increment_size = buffer_size;
    if (pos > 0) {
        // Mark the last entry of the block so that we can skip empty portion of the last page
        // during garbage collection
        updateFlagInSerializedItem(buf + last_entry_pos, FC_LAST_ITEM_BEFORE_NEXT_PAGE_BOUNDARY,
                log->crc_function);
    } else {
        if (isHeadWrappedToStartOfLogFile(log)) {
            // Free the buffer as it is not used
            flashcacheAssert(buf != NULL);
            fcFree(buf);
            buf = NULL;

            // This happens when we don't have enough space to write to the log, we don't write
            // anything in this scenario and wait for garbage collection to make some space prior
            // to next try to write to the log.
            // TODO: Add logging
            return;
        }
        // As new item cannot be written in the space left, the head offset is increment so that it
        // wraps to the front of the log file (after the marker is written to log).
        head_offset_increment_size = (log->log_size_bytes - log->head_offset);
        flashcacheAssert(head_offset_increment_size >= FC_PAGESIZE);

        // If we were unable to write any item, that means the amount of space left from the end
        // of the log file is not enough to any item. We write a marker indicating the pages needs
        // to be skipped till the end of the log so that the empty portion of log can be skipped
        // during garbage collection. The marker fits into a single page so the data size is adjusted
        // to the size of a single page.
        data_size = FC_PAGESIZE;
        skipSegmentInLogFileMarker(buf, head_offset_increment_size, log->crc_function);
        flashcacheAssert(buffer_size == 0);
    }

    size_t write_offset = log->head_offset;
    flashcacheAssert(write_offset + buffer_size <= log->log_size_bytes);

    fioRequestFill(&(log->log_flush_fio_request), FC_LOG_WRITE_REQUEST_IDENTIFIER,
            buf, data_size, write_offset, FC_FIO_WRITE);
    fioSubmit(log->fio_context, &(log->log_flush_fio_request));

    log->head_offset_after_flush_succeed = (log->head_offset + head_offset_increment_size) % log->log_size_bytes;
}

static void asyncReadItemFromLog(flashcacheLog *log, indexEntry *entry,
        uint32_t dbid, char *key, size_t key_len, flashcacheReadTypes read_type,
        void *request_context, flashcache_get_item_callback completion_callback,
        size_t num_pages_to_read) {
    // If num_pages_to_read is non-zero, this means asyncReadItemFromLog was called by
    // logRunCronTasks because the index's associated fio request only read part of the
    // required item components for processing a read/delete.

    logEntry *log_entry = &(entry->item_entry.log_entry);
    size_t log_offset = expandTrimmedLogOffset(log_entry->trimmed_log_offset);
    validateOffsetInRange(log, log_offset);

    char *buf = NULL;
    size_t block_num_pages;

    /* Attempt to optimize the fio request by only reading the item header. The following concitions
     * must be true:
     * - The caller of this function must be logReadFromIndexEntry. If the caller is logRunCronTasks
     * it means num_pages_to_read is deliberately set (not 0) and cannot be altered.
     * - The read_type must be FC_DELETE. FC_READ requires returning the value to the engine
     * - SnapshotV1 cannot be in progress
     * - The item being read must not be required to be expedited to the FDB. If replication snapshotting
     * is in progress and the item is unprocessed and is in the snapshot range, we must read the entire
     * item because we will need to write the entire item to the FDB.
     */ 
    flashcacheItemRequest requested_item_data = FC_ITEM_WITH_VALUE;
    if (num_pages_to_read == 0 && read_type == FC_DELETE &&
            snapshotManagerIsSnapshotV1Running() == 0 &&
            snapshotManagerShouldExpediteItem(log_offset) == 0 &&
            log->optimized_delete_enabled == 1) {
        log->metrics.num_optimized_deletes++;
        // Optimization is permitted, skip reading the value from flash
        requested_item_data = FC_ITEM_WITHOUT_VALUE;
        size_t bytes_needed = FC_ITEM_HEADER_LEN + key_len;
        block_num_pages = getNumberPagesRequiredToReadItem(log_offset, bytes_needed);
    } else {
        // If no optimizations are possible, the entire item must be read from flash.
        block_num_pages = (log_entry->additional_pages + 1);
        if (block_num_pages == FC_MAX_ADDITIONAL_PAGES + 1) {
            block_num_pages = 1;
            // If the header does not fit in one page, increase the number of pages to read.
            if ((FC_PAGESIZE - ((log_offset) % FC_PAGESIZE)) < sizeof(itemHeader)) {
                block_num_pages++;
            }
        }
        if (num_pages_to_read) {
            block_num_pages = num_pages_to_read;
        }
    }

    size_t blocksize = (FC_PAGESIZE * block_num_pages);
    buf = fcPosixMemalign(FC_PAGESIZE, blocksize);

    size_t page_aligned_offset = getFloorPageAlignedOffset(log_offset);
    validateOffsetInRange(log, page_aligned_offset + blocksize - 1);

    for (size_t i = 0; i < log->max_in_flight_item_read_requests; ++i) {
        if (fioRequestIsEmpty(&(log->inflight_item_read_io_metadata[i].fio_request))) {
            log->inflight_item_read_io_metadata[i].index_entry = entry;
            log->inflight_item_read_io_metadata[i].state = FC_CURRENT_INDEX_ENTRY_VALID;
            log->inflight_item_read_io_metadata[i].request_context = request_context;
            log->inflight_item_read_io_metadata[i].completion_callback = completion_callback;
            log->inflight_item_read_io_metadata[i].dbid = dbid;
            log->inflight_item_read_io_metadata[i].key = key;
            log->inflight_item_read_io_metadata[i].key_len = key_len;
            log->inflight_item_read_io_metadata[i].read_type = read_type;
            log->inflight_item_read_io_metadata[i].requested_item_data = requested_item_data;

            fioRequestFill(&(log->inflight_item_read_io_metadata[i].fio_request), i,
                    buf, blocksize, page_aligned_offset, FC_FIO_READ);
            fioSubmit(log->fio_context, &(log->inflight_item_read_io_metadata[i].fio_request));
            break;
        }
    }
    log->num_inflight_item_read_io_request++;
}

static inline size_t getPositionOfItemInPage(size_t offset) {
    return (offset - getFloorPageAlignedOffset(offset));
}

static size_t getCollisionHashFromIndexEntry(indexEntry *index_entry) {
    flashcacheAssert(index_entry != NULL);
    size_t collision_hash;
    if (index_entry->item_entry.log_entry.on_flash) {
        collision_hash = index_entry->item_entry.log_entry.hash;
    } else {
        collision_hash = index_entry->item_entry.staging_buffer_entry->log_entry.hash;
    }
    return collision_hash;
}

static void deleteItem(flashcacheLog *log, uint32_t dbid, char *key, size_t key_len,
        indexEntry *index_entry) {
    /* If the index_entry was being fetched as part of a different read request,
     * we move the index_entry of that request to the next entry which has same
     * collision hash (considering index growth operation). We also mark that state
     * stating that the entry has not yet been handled. This scenario can happen
     * when 2 different items have the same hash value and there are active read
     * request for both these items.
     *
     * For eg. Lets say we have 3 keys in a bucket in which 2 has same Collision hash
     * K1 -> (KEY1, IH, CH) ,  K2 -> (KEY2, IH, CH1) ,  K3 -> (KEY3, IH, CH)
     * Current status of index
     *          Bucket A : K3 -> K2 -> K1
     * Now if we get a read request for K3 and K1 at the same time. We will process
     * the Request for K3 and request for K1 will be marked Invalid.
     * Current status of index
     *          Bucket A : K2 -> K1
     * Now if at this point Index Growth starts and K1 moves to different bucket
     * before the pending request of K1 has been processed.
     * Current status of index
     *          Bucket A : K2
     *          Bucket B : K1
     * In order to handle this scenario, when we mark a request invalid, we  move the
     * index_entry of that request to the next entry which has same
     * collision hash
     */
    for (size_t i = 0; i < log->max_in_flight_item_read_requests; ++i) {
        if (log->inflight_item_read_io_metadata[i].index_entry == index_entry) {
            indexEntry *next_entry = index_entry->next;
            size_t index_collision_hash = getCollisionHashFromIndexEntry(index_entry);
            while (next_entry != NULL && getCollisionHashFromIndexEntry(next_entry) != index_collision_hash) {
                next_entry = next_entry->next;
            }
            log->inflight_item_read_io_metadata[i].index_entry = next_entry;
            log->inflight_item_read_io_metadata[i].state =
                FC_CURRENT_INDEX_ENTRY_INVALID;
        }
    }
    indexDeleteItem(log->index_list[dbid], key, key_len, index_entry);
    log->num_items--;
}

static void deleteItemAndFreeKey(flashcacheLog *log, uint32_t dbid, char *key, size_t key_len,
        indexEntry *index_entry) {
    deleteItem(log, dbid, key, key_len, index_entry);
    if (key != NULL) {
        fcFree(key);
    }
}

// This function invalidate any IO request fetching the provided index entry. This
// forces the IO request to process the entry again.
static void invalidateIndexEntry(flashcacheLog *log, indexEntry *index_entry) {
    for (size_t i = 0; i < log->max_in_flight_item_read_requests; ++i) {
        if (log->inflight_item_read_io_metadata[i].index_entry == index_entry) {
            log->inflight_item_read_io_metadata[i].state =
                FC_CURRENT_INDEX_ENTRY_INVALID;
        }
    }
}

static flashcacheReturnCode logReadFromIndexEntry(flashcacheLog *log, indexEntry *index_entry,
        uint32_t dbid, char *key, size_t key_len, flashcacheReadTypes read_type, void *request_context,
        flashcache_get_item_callback completion_callback) {
    while (index_entry) {
        if (index_entry->item_entry.log_entry.on_flash) {
            if (computeCollisionHash(log->hasher.hash_function, key, key_len) ==
                    index_entry->item_entry.log_entry.hash) {
                asyncReadItemFromLog(log, index_entry, dbid, key, key_len, read_type,
                        request_context, completion_callback, 0);
                return FC_OK;
            }
        } else if (compareKeyAndDbidInSerializedItem(
                    index_entry->item_entry.staging_buffer_entry->item,
                    dbid, key, key_len)) {
            char *value = NULL;
            size_t value_len = 0;
            char *item = index_entry->item_entry.staging_buffer_entry->item;
            extractValueFromSerializedItem(item, &value, &value_len);

            // Serialized item is not deleted when the staging buffer entry is deleted because
            // extracted value is a pointer in item. Also staging buffer needs to be deleted after
            // deleting index entry as the deletion uses the staging buffer.
            stagingBufferEntry *staging_buffer_entry = index_entry->item_entry.staging_buffer_entry;
            deleteItemAndFreeKey(log, dbid, key, key_len, index_entry);
            deleteEntryFromStagingBuffer(log, staging_buffer_entry, 0);

            // Completion callback is called after cleaning up internal
            // state so that if the callback calls another Flashcache API,
            // there is no side effect due to the uncleaned state.
            // `completion_callback` will be NULL in case when request is generated internally
            // in FlashCache. For e.g. Delete command in FDB file.
            if (completion_callback != NULL) {
                if (read_type == FC_READ && (value == NULL || value_len == 0)) {
                    flashcacheLogger(FC_LL_WARNING, "logReadFromIndexEntry: "
                        "Unexpected a NULL or empty value in the staging buffer. "
                        "This likely indicates a FlashCache read miss.\n"
                        "logReadFromIndexEntry parameters:\n"
                        "dbid: %lu, key: %p, key_len: %zu, read_type: %d , "
                        "request_context: %p"
                        "Item Metadata:\n"
                        "value: %p, value_len: %zu, item: %p"
                        "staging_buffer_entry: %p",
                        dbid, (void *) key, key_len, read_type, request_context,
                        (void *) value, value_len, (void *) item,
                        (void *) staging_buffer_entry);
                }
                completion_callback(request_context, value, value_len, 0);
            }
            fcFree(item);

            return FC_OK;
        }

        index_entry = index_entry->next;
    }

    // call the callback with NULL value if key is not found
    // `completion_callback` will be NULL in case when request is generated internally
    // in FlashCache. For e.g. Delete command in FDB file.
    if (completion_callback != NULL) {
        flashcacheLogger(FC_LL_WARNING, "logReadFromIndexEntry: Calling back a NULL value. "
                                "This likely indicates a FlashCache read miss.\n"
                                "dbid: %lu, key: %p, key_len: %zu, read_type: %d , "
                                "request_context: %p",
                                dbid, (void *) key, key_len, read_type, request_context);
        completion_callback(request_context, NULL, 0, 0);
    }
    if (key != NULL) {
        fcFree(key);
    }

    return FC_OK;
}

static size_t getActiveLogSizeBytesBeforeWrapping(flashcacheLog *log,
        size_t offset) {
    if (isHeadWrappedToStartOfLogFile(log)) {
        return (log->log_size_bytes - offset);
    }

    // The log has not wrapped, so the active log size is the distance from the head
    return (log->head_offset - offset);
}

static size_t getActiveLogSizeBytes(flashcacheLog *log) {
    if (!isHeadWrappedToStartOfLogFile(log)) {
        return (log->head_offset - log->tail_offset);
    }

    return (log->log_size_bytes - log->tail_offset) + log->head_offset;
}

// Returns 1 if index growth operation is running else 0
static size_t isIndexGrowthRunning(flashcacheLog *log) {
    struct flashcacheIndex *index = log->index_list[log->current_index_growth_dbid];
    flashcacheAssert(index != NULL);
    flashcacheAssert(index->growth_iterator != NULL);
    return (index->growth_iterator->status == RUNNING);
}

/* This method determines the garbage collection rate.
 *
 * The space used by the log to store the items is called the active log. The active log can also contains deleted
 * items. The unused space is called free log. Garbage collection run on the active log to move the active items to the
 * front of the log. This helps remove the deleted items from the active log. The new items and the active items moved
 * during garbage collection are written to the free log section.
 *
 *   +-----------------------+------------------------------------------------------+
 *   |                       |                                                      |
 *   |  ACTIVE LOG           |                 FREE LOG                             |
 *   |                       |                                                      |
 *   |                       |                                                      |
 *   |<------(T - L)-------->|<-------------------L-------------------------------->|
 *   |                       |                                                      |
 *   |                       |                                                      |
 *   |                       |                                                      |
 *   |                       |                                                      |
 *   +-----------------------+------------------------------------------------------+
 *
 *   |--------> G            |----------> W
 * (Garbage collection Rate)          (Write Rate)
 *
 * ### GC Algorithm where GC rate is inversely proportional to free space:
 *
 * Rate at which garbage collection should be run is determined using the amount of free log left and amount of
 * log current active. Lets say 'L' is the space left in the log of size 'T'. The current write rate is W. The
 * time taken to fill the log is (L / W). In the same amount of time, if we are able to garbage collect the active
 * size of log (T - L), we would be able to free unused space from the current active log. Inorder to do that the
 * garbage collection rate should be 'W * (T - L) / L'. If we garbage collect at this rate, it is guaranteed to not
 * run out of space for writing new item. We also simulated the algorithm using different setting and ensured that
 * the garbage collection factor '(T - L) / L' is never more than 3 with 25% overprovisioning.
 *
 *   G = (T - L) / L * W
 *
 * Garbage collection runs all the time. The rate of garbage collection increases with the decrease in free
 * log size. For instance example if T = 100 and L = 10, the garbage collection rate is 1/9 of the write rate (W).
 * However if L = 40, the garbage collection rate is equal to 3/2 of the write rate (W).
 *
 * ### GC Algorithm where GC rate is linearly proportional to free space:
 *
 * Lets say M is the max allocated log size, and S is the spare log size (T - M).
 *
 * The GC rate is calculated using the following formula:
 * 1. When the used log size is less than the max allocated log size: `G = (T - L) / M * W`
 * 2. When used log size is greater than max allocated log size:
 * `G = (1 + ((S - L) / S * FC_GARBAGE_COLLECTION_LINEAR_WITH_FREE_SPACE_FACTOR)) * W`
 *
 * In this algorithm, We start increasing the garbage collection rate at a higher rate when we start using up the
 * spare space.
 */
// Non-static function to enable unit testing
void updateGarbageCollectionRate(flashcacheLog *log) {
    garbageCollectorInfo *gc_info = &(log->garbage_collector_info);
    writeRateInfo *write_rate_info = &(log->write_rate_info);
    if (gc_info->enable_adaptive_garbage_collection_rate) {
        size_t active_log_size = getActiveLogSizeBytes(log);
        size_t free_log_size = (log->log_size_bytes - active_log_size);

        double garbage_collection_rate_factor = 0;
        flashcacheGarbageCollectionAlgorithm gc_algorithm = gc_info->garbage_collection_rate_algorithm;
        size_t max_allocated_log_size_bytes = getMaxAllocatedLogSizeBytes(log);
        size_t spare_log_size_bytes = log->log_size_bytes - max_allocated_log_size_bytes;

        switch (gc_algorithm) {
            case FC_GARBAGE_COLLECTION_INVERSE_WITH_FREE_SPACE:
                if (free_log_size == 0) {
                    garbage_collection_rate_factor = FC_MAX_GARBAGE_COLLECTION_RATE_FACTOR;
                } else {
                    garbage_collection_rate_factor = (((double) active_log_size) / free_log_size);
                }
                break;
            case FC_GARBAGE_COLLECTION_LINEAR_WITH_FREE_SPACE:
                if (active_log_size <= max_allocated_log_size_bytes) {
                    garbage_collection_rate_factor = ((double) active_log_size) / max_allocated_log_size_bytes;
                } else {
                    flashcacheAssert(free_log_size < spare_log_size_bytes);
                    garbage_collection_rate_factor = 1 + FC_GARBAGE_COLLECTION_LINEAR_WITH_FREE_SPACE_FACTOR *
                        ((double) (spare_log_size_bytes - free_log_size)) / spare_log_size_bytes;
                }
                break;
            default:
                flashcacheAssertWithLogging(0, "Unknown garbage collection algorithm: %d", gc_algorithm);
        }

        if (garbage_collection_rate_factor > FC_MAX_GARBAGE_COLLECTION_RATE_FACTOR) {
            garbage_collection_rate_factor = FC_MAX_GARBAGE_COLLECTION_RATE_FACTOR;
        }

        gc_info->required_garbage_collection_bytes_per_second = garbage_collection_rate_factor *
            write_rate_info->last_window_data_written_bytes_per_second;

        // Compute FlashcacheStorageOverheadRatio (garbage/active log size)
        double flashcache_storage_overhead_ratio = 0;
        size_t garbage_size_bytes = 0;
        if (active_log_size > 0) {
            // Get size of live items in log and compute size of garbage (all items - live items)
            garbage_size_bytes = active_log_size - log->allocated_log_size_bytes;
            flashcache_storage_overhead_ratio = garbage_size_bytes / (long double) active_log_size;
        }
        /* calculate the dynamic rate and take max with existing rate */
       double log_util_ratio = ((double) active_log_size) / log->log_size_bytes;
       size_t dynamic_garbage_collection_bytes_per_second =
            flashcache_storage_overhead_ratio * log->max_dynamic_garbage_collection_rate * log_util_ratio;
       if (dynamic_garbage_collection_bytes_per_second > gc_info->required_garbage_collection_bytes_per_second) {
            gc_info->required_garbage_collection_bytes_per_second = dynamic_garbage_collection_bytes_per_second;
        }
    }

    // Override the rate with the larger of min GC rate/ eviction under max logsize rate if needed
    uint32_t rate_override = (log->evict_under_max_logsize_min_rate > log->min_garbage_collection_rate) ?
                                log->evict_under_max_logsize_min_rate : log->min_garbage_collection_rate;

    if (gc_info->required_garbage_collection_bytes_per_second < rate_override) {
        gc_info->required_garbage_collection_bytes_per_second = rate_override;
    }
}

// Non-static function to enable unit testing
void updateWriteRateInfo(uint64_t current_time_us, writeRateInfo *info, size_t item_len) {
    uint64_t time_diff_us = current_time_us - info->current_window_start_time_us;
    if (time_diff_us >= info->window_size_us) {
        info->last_window_data_written_bytes_per_second = (info->current_window_data_written
                * FC_SECOND_TO_MICROSECOND / time_diff_us);
        info->current_window_data_written = 0;
        info->current_window_start_time_us = current_time_us;
    }

    info->current_window_data_written += item_len;
}

// Non-static function to enable unit testing
void resetGarbageCollectionTimeWindowIfRequired(uint64_t current_time_us,
        garbageCollectorInfo *gc_info) {
    flashcacheAssert(current_time_us >= gc_info->current_aggregation_window_start_time_us);

    if (current_time_us - gc_info->current_aggregation_window_start_time_us >=
            FC_SECOND_TO_MICROSECOND) {
        gc_info->total_garbage_collected_bytes +=
            gc_info->garbage_collection_bytes_in_current_second;
        gc_info->garbage_collection_bytes_in_current_second = 0;
        gc_info->current_aggregation_window_start_time_us = current_time_us;
    }
}

// Non-static function to enable unit testing
size_t computeGarbageCollectionBlockSizeInBytes(flashcacheLog *log) {
    garbageCollectorInfo *gc_info = &(log->garbage_collector_info);

    // If we already have garbage collected enough for this second, we just return
    if (gc_info->garbage_collection_bytes_in_current_second >=
            gc_info->required_garbage_collection_bytes_per_second) {
        return 0;
    }

    // Compute the block of garbage collection by reducing the amount of garbage collection already performed in
    // the current window
    size_t garbage_collection_block_size = gc_info->required_garbage_collection_bytes_per_second
        - gc_info->garbage_collection_bytes_in_current_second;

    // Ensure that the amount of data read from tail of the log is within bounds
    if (garbage_collection_block_size > FC_MAX_GARBAGE_COLLECTION_BLOCK_SIZE_BYTES) {
        garbage_collection_block_size = FC_MAX_GARBAGE_COLLECTION_BLOCK_SIZE_BYTES;
    }

    // Ensure that the block size is less than the active log size before wrapping to the front of
    // the log file
    size_t page_aligned_active_log_size_bytes_before_wrapping = getCeilPageAlignedOffset(
            getActiveLogSizeBytesBeforeWrapping(log, log->tail_offset));
    if (garbage_collection_block_size > page_aligned_active_log_size_bytes_before_wrapping) {
        garbage_collection_block_size = page_aligned_active_log_size_bytes_before_wrapping;
    }
    // If we have performed extra garbage collection in the previous cycle, we reduce the needed garbage collection
    // in the current cycle. This helps in scenarios where we have large objects for example 10 MiB in size and the
    // required garbage collection rate of 4 KiB. Without accounting for the extra GC done before, we will be
    // constantly running GC at 10 MiB/sec.
    if (garbage_collection_block_size <= gc_info->extra_garbage_collected_bytes) {
        gc_info->extra_garbage_collected_bytes -= garbage_collection_block_size;
        gc_info->garbage_collection_bytes_in_current_second += garbage_collection_block_size;
        garbage_collection_block_size = 0;
        return 0;
    } else {
        garbage_collection_block_size -= gc_info->extra_garbage_collected_bytes;
        gc_info->garbage_collection_bytes_in_current_second += gc_info->extra_garbage_collected_bytes;
        gc_info->extra_garbage_collected_bytes = 0;
    }

    // Ensure that we are not reading a partial item
    if (garbage_collection_block_size < log->log_iterator->partial_item_size_bytes) {
        garbage_collection_block_size = log->log_iterator->partial_item_size_bytes;
    }

    // Ensure that garbage collection block size is PAGESIZE aligned
    garbage_collection_block_size = getCeilPageAlignedOffset(garbage_collection_block_size);

    // The garbage collection block size should not be greater than the active log size to the end of log file.
    // This only can happen due to algorithmic error.
    flashcacheAssertWithLogging(garbage_collection_block_size <= page_aligned_active_log_size_bytes_before_wrapping,
            "GC block size bytes: [%lu], Log size before wrapping bytes: [%lu]", garbage_collection_block_size,
            page_aligned_active_log_size_bytes_before_wrapping);

    // We cannot garbage collect the portion of the log that is currently being snapshotted. We check if current
    // garbage collection block would fall in the portion of the log being snapshotted, we return the garbage
    // collection block size of 0 to prevent garbage collection.
    size_t page_aligned_tail_offset = getFloorPageAlignedOffset(log->tail_offset);
    size_t log_file_processed_offset = snapshotManagerGetLogFileProcessedOffset();
    if (snapshotManagerIsRunning() &&
            (log_file_processed_offset >= page_aligned_tail_offset) &&
            ((log_file_processed_offset - page_aligned_tail_offset) < garbage_collection_block_size)) {
        return 0;
    }
    return garbage_collection_block_size;
}

static size_t getAllocatedLogSizeBytesWithBufferedWrites(flashcacheLog *log) {
    return log->allocated_log_size_bytes + log->staging_buffer->total_item_size;
}

// Non-static function to enable unit testing
int needToPerformEviction(flashcacheLog *log) {
    if (log->evict_under_max_logsize_min_rate &&
        timeInMicrosecond(log) - log->last_evict_under_max_logsize_start > log->evict_under_max_logsize_time_limit) {
        log->evict_under_max_logsize_min_rate = 0;
    }

    if (log->eviction_enabled &&
        ((getAllocatedLogSizeBytesWithBufferedWrites(log) > getMaxAllocatedLogSizeBytes(log)) ||
        log->evict_under_max_logsize_min_rate)) {
        return 1;
    }
    return 0;
}

size_t logIteratorPreProcessingGarbageCollectionCallback(void *context) {
    flashcacheLog *log = (flashcacheLog *)context;
    garbageCollectorInfo *gc_info = &(log->garbage_collector_info);

    int can_read = !gc_info->is_running && gc_info->can_start_garbage_collection &&
         (needToPerformEviction(log) || gc_info->can_do_log_compaction);
    if (!can_read) {
        return 0;
    }

    // Update Garbage Collection rate prior to computing the garbage collection blocksize
    updateGarbageCollectionRate(log);

    // compute GC block size
    size_t buf_size = computeGarbageCollectionBlockSizeInBytes(log);
    if (!buf_size) {
        return buf_size;
    }

    size_t read_offset = getFloorPageAlignedOffset(log->tail_offset);
    // Verify that the last byte in the garbage collection block is in between the head offset and tail offset
    validateOffsetInRange(log, read_offset + buf_size - 1);
    return buf_size;
}

void logIteratorPostProcessingGarbageCollectionCallback(void *context, size_t is_running) {
    flashcacheLog *log = (flashcacheLog *)context;
    garbageCollectorInfo *gc_info = &(log->garbage_collector_info);
    log->garbage_collector_info.is_running = is_running;

    // Update GC metrics
    if (gc_info->garbage_collection_bytes_in_current_second >
            gc_info->required_garbage_collection_bytes_per_second) {
        gc_info->extra_garbage_collected_bytes +=
                (gc_info->garbage_collection_bytes_in_current_second -
                        gc_info->required_garbage_collection_bytes_per_second);
    }
}

void logIteratorCoreLogicProcessingGarbageCollectionCallback(void *context, void *item_input, void *entry) {
    flashcacheLog *log = (flashcacheLog *)context;
    char * item = (char *)item_input;
    indexEntry *index_entry = (indexEntry *)entry;
    uint32_t dbid = extractDbidFromSerializedItem(item);
    size_t total_item_len = extractTotalLenFromSerializedItem(item);
    size_t tail_offset = log->tail_offset;
    char *key = NULL;
    size_t key_len = 0;
    extractKeyFromSerializedItem(item, &key, &key_len);
    flashcacheAssert(validateValueInSerializedItem(item, log->crc_function));
    snapshotManagerAddExpeditedItem(tail_offset, item, total_item_len);
    if (needToPerformEviction(log) || !log->garbage_collector_info.can_do_log_compaction) {
        // We perform item eviction in 3 scenarios:
        // 1. The current allocated log size bytes is greater than the max allowed allocated log size
        // 2. We are not allowed to move items for log compaction during stream-based forkless save
        // 3. There are too few spillable values in memory
        char *value = NULL;
        size_t value_len = 0;
        // For forkless save replication, we need to propagate a DELETE command for the item
        size_t log_offset = expandTrimmedLogOffset(index_entry->item_entry.log_entry.trimmed_log_offset);
        extractValueFromSerializedItem(item, &value, &value_len);
        snapshotManagerAddReplicationCommandIfRequired(log_offset, dbid, key, key_len,
                                                       value, value_len, log->crc_function);
        deleteItem(log, dbid, key, key_len, index_entry);
        decrementAllocatedLogSize(log, dbid, total_item_len);
        log->metrics.total_item_evicted_size_bytes += total_item_len;
        log->metrics.num_items_evicted++;
        if (log->evict_under_max_logsize_min_rate) {
            log->metrics.num_items_evicted_under_logsize++;
        }
        // Eviction callback is called to notify the client
        log->eviction_details.callback(log->eviction_details.context, dbid, key, key_len);
    } else {
        // Log compaction by moving the item from the tail to the head of the log
        char *copied_item = (char *) fcMalloc(total_item_len);
        flashcacheAssert(copied_item != NULL);
        memcpy(copied_item, item, total_item_len);

        // Reset the flags in the copied serialized item as they are not longer valid.
        updateFlagInSerializedItem(copied_item, 0, log->crc_function);

        invalidateIndexEntry(log, index_entry);
        stagingBufferEntry *staging_buffer_entry = stagingBufferAddItem(
                log->staging_buffer, copied_item, total_item_len, dbid);
        flashcacheAssert(index_entry ==
                         indexUpdateItem(log->index_list[dbid], key, key_len, tail_offset,
                                         staging_buffer_entry));
        decrementAllocatedLogSize(log, dbid, total_item_len);
        log->metrics.garbage_collection_write_bytes += total_item_len;
        log->metrics.garbage_collection_num_items_moved++;
        logFlushStagingBufferIfRequired(log, log->staging_buffer_flush_size_threshold_bytes);
    }
}

// Selects the next database to grow.
static void selectNextDbToGrow(struct flashcacheLog *log) {
    log->current_index_growth_dbid++;
    if (log->current_index_growth_dbid == log->num_databases) {
        log->current_index_growth_dbid = 0;
    }
}

static void indexGrowthCronTask(struct flashcacheLog *log) {
    flashcacheIndex *index = log->index_list[log->current_index_growth_dbid];
    flashcacheAssert(index->growth_iterator != NULL);

    // In case of growth iterator NOT_RUNNING status, this check would be required to avoid growth during snapshot.
    if (snapshotManagerIsRunning()) {
        return;
    }

    // No. of index growth before running index growth operation
    size_t num_index_growth_before_growing = indexNumGrowth(index);

    // Grow the index table if required.
    if (!indexGrowIfRequired(index)) {
        selectNextDbToGrow(log);  // Selects next database to growth if growth is not required for current database.
    }
    // No. of index growth after running index growth operation
    size_t num_index_growth_after_growing = indexNumGrowth(index);
    log->num_index_growth_run += (num_index_growth_after_growing - num_index_growth_before_growing);
}

// Resets the Head and tail offset and partial_item_size_bytes if we only have garbage data in log (no live items).
// This way we optimize the compute of GC and avoid replicating garbage.
static void resetHeadTailOffsetOfLogIfRequired(struct flashcacheLog *log) {
    if (log->should_reset_head_tail_offset_of_log &&
        log->allocated_log_size_bytes == 0 && getActiveLogSizeBytes(log) > 0 &&
        !log->garbage_collector_info.is_running &&
        fioRequestIsEmpty(&(log->log_flush_fio_request)) &&
        !(snapshotManagerIsRunning())) {
        log->head_offset = 0;
        log->tail_offset = 0;
        log->log_iterator->partial_item_size_bytes = 0;
    }
}

flashcacheReturnCode logCreate(flashcacheLog **flashcache_log, char const *log_filename,
        size_t log_size_bytes, size_t intial_index_size_per_db, uint32_t num_databases,
        size_t staging_buffer_flush_size_threshold_bytes,
        uint32_t max_allocated_log_size_percent,
        uint32_t max_num_in_flight_read_requests,
        uint32_t min_garbage_collection_rate,
        uint32_t evict_under_max_logsize_time_limit,
        uint8_t optimized_delete_enabled,
        flashcacheHasher *hasher,
        flashcache_monotonic_clock_us monotonic_clock_us,
        flashcacheEvictionDetails *eviction_details) {
    flashcacheAssert(log_size_bytes % FC_PAGESIZE == 0);

    *flashcache_log = NULL;
    flashcacheLog *log = (flashcacheLog *) fcMalloc(sizeof(flashcacheLog));
    flashcacheAssert(log != NULL);

    log->max_buffered_write_size_bytes = FC_DEFAULT_MAX_BUFFERED_WRITE_SIZE_BYTES;
    log->max_in_flight_item_read_requests = max_num_in_flight_read_requests;
    log->eviction_enabled = 1;
    log->num_items = 0;
    log->num_databases = num_databases;
    log->head_offset = 0;
    log->tail_offset = 0;
    log->should_reset_head_tail_offset_of_log = 1;
    log->allocated_log_size_bytes_per_db = (size_t *) fcCalloc(sizeof(size_t), num_databases);
    flashcacheAssert(log->allocated_log_size_bytes_per_db != NULL);
    log->allocated_log_size_bytes = 0;
    log->log_size_bytes = log_size_bytes;
    log->overriden_allocatable_log_size_bytes = -1;
    log->staging_buffer_flush_size_threshold_bytes = staging_buffer_flush_size_threshold_bytes;
    log->crc_function = flashcacheCrc32c;
    log->evict_under_max_logsize_min_rate = 0;
    log->evict_under_max_logsize_time_limit = evict_under_max_logsize_time_limit;
    log->last_evict_under_max_logsize_start = 0;
    log->optimized_delete_enabled = optimized_delete_enabled;

    flashcacheHasherValidate(hasher);
    log->hasher = *hasher;
    hasher->init(NULL);

    log->max_allocated_log_size_percent = max_allocated_log_size_percent;
    log->num_inflight_item_read_io_request = 0;
    log->head_entry_being_flushed_to_flash = NULL;
    log->monotonic_clock_us = monotonic_clock_us;
    log->current_index_growth_dbid = 0;
    log->num_index_growth_run = 0;

    uint32_t aio_queue_length = max_num_in_flight_read_requests + FC_INTERNAL_REQUEST_QUEUE_LENGTH;
    // FC_SNAPSHOT_NUM_FIO_DATA number of fio data is allocated for writing the log during loading snapshot.
    // This check ensures that queue depth of the FIO context used for writing to the log is greater than
    // the number of fio object allocated for writing. This ensures that fioSubmit for writing the log file
    // during loading does not crash due to breaching queue depth limit.
    flashcacheAssert(aio_queue_length >= FC_SNAPSHOT_NUM_FIO_DATA);

    log->staging_buffer = stagingBufferCreate();
    log->fio_context = fioCreateContext(log_filename, FILE_READ_WRITE, aio_queue_length, log->monotonic_clock_us);
    flashcacheAssert(log->fio_context != NULL);
    log->index_list = (flashcacheIndex **) fcMalloc(sizeof(flashcacheIndex *) * num_databases);
    flashcacheAssert(log->index_list != NULL);

    for (size_t i = 0; i < num_databases; ++i) {
        log->index_list[i] = indexCreate(intial_index_size_per_db, log->hasher.hash_function);
    }

    fioRequestClear(&(log->log_flush_fio_request));
    memset(&(log->metrics), 0, sizeof(log->metrics));
    snapshotManagerInfoCreate(log_filename,
                              num_databases,
                              log_size_bytes,
                              log->index_list,
                              &(log->metrics),
                              monotonic_clock_us,
                              log->crc_function);
    log->inflight_item_read_io_metadata = (inflightItemReadIoMetadata *)fcCalloc(max_num_in_flight_read_requests,
                                                   sizeof(inflightItemReadIoMetadata));
    memset(&(log->garbage_collector_info), 0,
            sizeof(log->garbage_collector_info));
    log->garbage_collector_info.can_start_garbage_collection = 1;
    log->garbage_collector_info.can_do_log_compaction = 1;
    log->min_garbage_collection_rate = min_garbage_collection_rate;
    log->garbage_collector_info.required_garbage_collection_bytes_per_second =
        log->min_garbage_collection_rate;
    log->max_dynamic_garbage_collection_rate = FC_DEFAULT_MAX_DYNAMIC_GARBAGE_COLLECTION_RATE;
    log->garbage_collector_info.enable_adaptive_garbage_collection_rate =
        FC_DEFAULT_ADAPTIVE_GARBAGE_COLLECTION_RATE_ENABLED;
    log->garbage_collector_info.garbage_collection_rate_algorithm =
        FC_DEFAULT_GARBAGE_COLLECTION_ALGORITHM;

    log->log_iterator = logIteratorCreate(log->index_list,
                                          monotonic_clock_us,
                                          log->crc_function,
                                          FC_DEFAULT_GARBAGE_COLLECTION_MAX_PROCESSING_TIME_MICROSECONDS,
                                          FC_GARBAGE_COLLECTION_LOG_READ_REQUEST_IDENTIFIER,
                                          &log->log_size_bytes,
                                          &log->tail_offset,
                                          &log->garbage_collector_info.garbage_collection_bytes_in_current_second,
                                          &log->garbage_collector_info.is_running,
                                          log->fio_context,
                                          log,
                                          logIteratorPreProcessingGarbageCollectionCallback,
                                          logIteratorCoreLogicProcessingGarbageCollectionCallback,
                                          logIteratorPostProcessingGarbageCollectionCallback);

    memset(&(log->write_rate_info), 0, sizeof(log->write_rate_info));
    log->write_rate_info.current_window_start_time_us = timeInMicrosecond(log);
    log->write_rate_info.window_size_us = FC_DEFAULT_WRITE_RATE_WINDOW_SIZE_MICROSECOND;

    memcpy(&(log->eviction_details), eviction_details, sizeof(flashcacheEvictionDetails));

    *flashcache_log = log;
    return FC_OK;
}

flashcacheReturnCode logWrite(flashcacheLog *log, uint32_t dbid, char const *key, size_t key_len,
        char const *value, size_t value_len) {
    flashcacheAssert(dbid < log->num_databases);
    log->metrics.num_write_request++;

    // Throttle the request to create backpressure if the current item in staging buffer is
    // greater than the specified threshold
    if (stagingBufferGetTotalItemSize(log->staging_buffer) > log->max_buffered_write_size_bytes) {
        return FC_ERR_THROTTLED;
    }

    size_t item_len = 0;
    char *item = NULL;

    serializeKeyValuePair(dbid, key, key_len, value, value_len, &item,
            &item_len, log->crc_function);

    stagingBufferEntry *staging_buffer_entry = stagingBufferAddItem(
            log->staging_buffer, item, item_len, dbid);
    updateStagingBufferSizeMetric(log->staging_buffer->total_item_size);
    indexAddItem(log->index_list[dbid], key, key_len, staging_buffer_entry);

    logFlushStagingBufferIfRequired(log, log->staging_buffer_flush_size_threshold_bytes);
    updateWriteRateInfo(timeInMicrosecond(log), &(log->write_rate_info), item_len);

    log->num_items++;
    return FC_OK;
}

flashcacheReturnCode logRead(flashcacheLog *log, uint32_t dbid, char const *key, size_t key_len,
        flashcacheReadTypes read_type, void *request_context, flashcache_get_item_callback completion_callback) {
    flashcacheAssert(dbid < log->num_databases);
    if (read_type == FC_READ) {
        log->metrics.num_read_request++;
    } else {
        log->metrics.num_delete_request++;
    }

    if (log->num_inflight_item_read_io_request >= (int)log->max_in_flight_item_read_requests) {
        return FC_ERR_THROTTLED;
    }

    char *key_copy = NULL;
    if (key_len > 0) {
        key_copy = (char *) fcMalloc(key_len);
        flashcacheAssert(key_copy != NULL);
        memcpy(key_copy, key, key_len);
    }

    indexEntry *index_entry = indexGetHeadEntry(log->index_list[dbid], key_copy, key_len);
    return logReadFromIndexEntry(log, index_entry, dbid, key_copy, key_len, read_type,
            request_context, completion_callback);
}

// When new cron tasks are added, we need to ensure that 'logShouldRunCronTasksImmediately'
// function is updated to take into account the urgency of the new cron task.
/* Snapshot support: when non-zero, logRunCronTasks skips the GC iterator so
 * on-flash offsets stay stable for a concurrent fork-based snapshot child.
 * Set/cleared by the embedding application around fork()/child-exit. */
static int fc_gc_paused = 0;

void logSetGcPaused(int paused) {
    fc_gc_paused = paused;
}

int logGetGcPaused(void) {
    return fc_gc_paused;
}

flashcacheReturnCode logRunCronTasks(flashcacheLog *log) {
    fioRequest **completed_fio_requests;
    int num_events = fioGetCompletedRequest(log->fio_context, &completed_fio_requests);
    // Iterate over the completed fio requests
    for (int i = 0; i < num_events; ++i) {
        fioRequest *fio_request = completed_fio_requests[i];
        flashcacheAssertHandledCrashWithLogging(fio_request->err_no == 0,
                "IO failure with non retryable error [Invalid res: %lld]", fio_request->err_no);
        size_t id = fio_request->user_data;
        if (id == FC_LOG_WRITE_REQUEST_IDENTIFIER) {
            // Iterate over the staging buffer entries that had been written to flash
            stagingBufferEntry *entry = log->head_entry_being_flushed_to_flash;
            while (entry) {
                // Overwrite the index's item_entry to change it from a stagingBufferEntry to a logEntry because
                // the item now resides in flash. The logEntry members were prepared when the fio request was submitted.
                memcpy(&(entry->index_entry->item_entry.log_entry), &(entry->log_entry),
                        sizeof(entry->log_entry));
                // Increment the log size, move the pointer forward, and free the now unnecessary staging buffer entry
                incrementAllocatedLogSize(log, entry->user_data, entry->item_len);
                stagingBufferEntry *last_entry = entry;
                entry = entry->next;
                deleteEntryFromStagingBuffer(log, last_entry, 1);
            }

            updateDiskWriteMetrics(log, fioRequestGetBufferSize(fio_request));
            flashcacheAssert(fio_request == &(log->log_flush_fio_request));
            fcFree(fioRequestGetBuffer(fio_request));
            fioRequestClear(fio_request);
            log->head_offset = log->head_offset_after_flush_succeed;
            flashcacheAssert(log->head_entry_being_flushed_to_flash == NULL);
        } else if (id == FC_GARBAGE_COLLECTION_LOG_READ_REQUEST_IDENTIFIER) {
            flashcacheAssert(fio_request == &(log->log_iterator->log_file_fio_request));
            // Core GC logic is executed by processReadItem which is called by logIteratorCron at the end of
            // logRunCronTasks. This flag notifies the GC logic that fio completed reading a new buffer for GC.
            log->log_iterator->pending_log_data_processing = 1;

            size_t data_buffer_size = fioRequestGetBufferSize(fio_request);
            updateDiskReadMetrics(log, data_buffer_size);
            log->metrics.garbage_collection_num_disk_read++;
            log->metrics.garbage_collection_read_bytes += data_buffer_size;
        } else {
            // Logical flow for completed fio read and delete requests
            size_t io_metadata_idx = id;
            flashcacheAssert(io_metadata_idx < log->max_in_flight_item_read_requests);

            // Unpack the request metadata
            log->num_inflight_item_read_io_request--;
            indexEntry *index_entry =
                log->inflight_item_read_io_metadata[io_metadata_idx].index_entry;
            ioRequestState state =
                log->inflight_item_read_io_metadata[io_metadata_idx].state;
            uint32_t dbid =
                log->inflight_item_read_io_metadata[io_metadata_idx].dbid;
            char *key =
                log->inflight_item_read_io_metadata[io_metadata_idx].key;
            size_t key_len =
                log->inflight_item_read_io_metadata[io_metadata_idx].key_len;
            flashcacheReadTypes read_type =
                log->inflight_item_read_io_metadata[io_metadata_idx].read_type;
            flashcacheItemRequest requested_item_data =
                log->inflight_item_read_io_metadata[io_metadata_idx].requested_item_data;
            void *request_context =
                log->inflight_item_read_io_metadata[io_metadata_idx].request_context;
            flashcache_get_item_callback completion_callback =
                log->inflight_item_read_io_metadata[io_metadata_idx].completion_callback;
            char *data_buffer = fioRequestGetBuffer(fio_request);
            size_t data_buffer_size = fioRequestGetBufferSize(fio_request);
            flashcacheAssert(data_buffer_size % FC_PAGESIZE == 0);  // Validate that data buffer is page aligned

            updateDiskReadMetrics(log, data_buffer_size);
            flashcacheAssert(fio_request == &(log->inflight_item_read_io_metadata[io_metadata_idx].fio_request));
            fioRequestClear(fio_request);
            memset(&(log->inflight_item_read_io_metadata[io_metadata_idx]),
                    0, sizeof(inflightItemReadIoMetadata));

            if (state == FC_CURRENT_INDEX_ENTRY_VALID) {
                flashcacheAssert(index_entry != NULL);
                size_t log_offset = expandTrimmedLogOffset(
                        index_entry->item_entry.log_entry.trimmed_log_offset);
                size_t pos = getPositionOfItemInPage(log_offset);

                char *value = NULL;
                size_t value_len = 0;
                char *item = data_buffer + pos;
                flashcacheAssert(validateHeaderInSerializedItem(item, log->crc_function));
                size_t total_len = extractTotalLenFromSerializedItem(item);
                size_t num_pages_required_to_read_key = getNumberPagesRequiredToReadItem(log_offset,
                        extractTotalLenWithoutValueFromSerializedItem(item));
                size_t num_pages_required_to_read_item = getNumberPagesRequiredToReadItem(log_offset, total_len);
                size_t num_data_buffer_pages = (data_buffer_size / FC_PAGESIZE);
                int found_item = 0;
                int partial_key = 0;

                // If the buffer is large enough to contain the entire key, attempt to verify the log entry's key
                if (num_data_buffer_pages >= num_pages_required_to_read_key) {
                    flashcacheAssert(validateKeyInSerializedItem(item, log->crc_function));
                    found_item = compareKeyAndDbidInSerializedItem(item, dbid, key, key_len);
                } else {
                    // If the requested key length and dbid doesn't match the log item's key length and dbid, then
                    // there was a hash collision and the index entry and log item do not match, so we do not need
                    // to attempt to fetch the full key.
                    if (compareKeyLengthAndDbidInSerializedItem(item, dbid, key_len)) {
                        // If the buffer is not large enough to contain the entire key, then buffer contains a partial
                        // key, so the log entry cannot be verified
                        partial_key = 1;
                    }
                }
                // If only a partial key was read, or the full item is required and only a partial item was read,
                // submit a new request to fetch the correct required amount of pages
                int requested_item_with_value = requested_item_data == FC_ITEM_WITH_VALUE;
                int partial_item = num_pages_required_to_read_item > num_data_buffer_pages;
                if (partial_key || (found_item && requested_item_with_value && partial_item)) {
                    log->metrics.num_partial_item_read++;
                    log->metrics.partial_item_read_bytes += data_buffer_size;
                    size_t num_pages_required_for_request = (requested_item_with_value ?
                            num_pages_required_to_read_item : num_pages_required_to_read_key);
                    flashcacheAssertWithLogging(num_pages_required_for_request >= (FC_MAX_ADDITIONAL_PAGES + 1),
                            "Unexpected fio re-submission."
                            " num_pages_required_for_request: [%lu], key_len: [%lu], log_offset: [%lu],"
                            " dbid: [%u], read_type: [%u], hash collision cardinality [%d]"
                            " requested_item_with_value: [%d], partial_item: [%d], partial_key: [%d],"
                            " found_item: [%d]",
                            num_pages_required_for_request, key_len, log_offset, dbid, read_type,
                            collisionHashCount(log, dbid, key, key_len), requested_item_with_value,
                            partial_item, partial_key, found_item);
                    flashcacheAssert(index_entry->item_entry.log_entry.additional_pages == FC_MAX_ADDITIONAL_PAGES);

                    asyncReadItemFromLog(log, index_entry, dbid, key, key_len, read_type, request_context,
                            completion_callback, num_pages_required_for_request);
                    goto finish_processing_request;
                }

                // If the buffer contained the incorrect key due to hash collision, retry on the next index entry
                // that shares the same hash bucket collision
                if (!found_item) {
                    log->metrics.num_unused_disk_read_hash_collision++;
                    log->metrics.unused_disk_read_bytes_hash_collision += data_buffer_size;
                    logReadFromIndexEntry(log, index_entry->next, dbid, key, key_len, read_type,
                            request_context, completion_callback);
                    goto finish_processing_request;
                }

                // At this point, the key has been verified and the buffer contains the required amount of pages
                // Extract the value if it was read into the buffer
                if (requested_item_with_value) {
                    flashcacheAssert(validateValueInSerializedItem(item, log->crc_function));
                    extractValueFromSerializedItem(item, &value, &value_len);
                }
                // Note: both operations are currently applying on items that are mutually exclusive in the log.
                // If the item must be expedited to the FDB, but only the key was read from flash, resubmit the
                // fio request for the entire item.
                // Note: This code block should be unreachable. snapshotStartSave functions waits until all outgoing fio
                // requests are processed before beginning snapshotting, and we do not request optimized deletions if
                // the item will need to be expedited to the FDB.
                if (shouldResubmitRequestForExpedition(log_offset, requested_item_with_value)) {
                    log->metrics.num_partial_item_read++;
                    log->metrics.partial_item_read_bytes += data_buffer_size;
                    asyncReadItemFromLog(log, index_entry, dbid, key, key_len, read_type, request_context,
                            completion_callback, num_pages_required_to_read_item);
                    goto finish_processing_request;
                }
                snapshotManagerAddExpeditedItem(log_offset, item, total_len);
                // Value has isn't necessary for DEL replication commands (it can safely be NULL within length 0)
                snapshotManagerAddReplicationCommandIfRequired(log_offset, dbid, key, key_len,
                                                                value, value_len, log->crc_function);
                deleteItemAndFreeKey(log, dbid, key, key_len, index_entry);
                decrementAllocatedLogSize(log, dbid, total_len);
                // Completion callback is called after cleaning up internal
                // state so that if the callback calls another Flashcache API,
                // there is no side effect due to the uncleaned state.
                // `completion_callback` will be NULL in case when request is generated internally
                // in FlashCache. For e.g. Delete command in FDB file.
                if (completion_callback != NULL) {
                    int should_add_item_to_rdb = 0;
                    int is_item_in_ts_snapshot_range =
                            snapshotManagerIsItemInForklessSaveSnapshotRange(log_offset);
                    if (read_type == FC_READ) {
                        if (is_item_in_ts_snapshot_range) {
                            snapshotManagerIncrementNumItemsAddedToRDB();
                            should_add_item_to_rdb = 1;
                            log->metrics.item_bytes_moved_from_disk_during_forkless_save += key_len + value_len;
                        }
                    } else {
                        if (is_item_in_ts_snapshot_range) {
                            log->metrics.item_bytes_deleted_from_disk_during_forkless_save +=
                                (total_len - FC_ITEM_HEADER_LEN);
                        }
                    }
                    if (read_type == FC_READ && (value == NULL || value_len == 0)) {
                        flashcacheLogger(FC_LL_WARNING,
                            "logRunCronTasks: Unexpected a NULL or empty value from Flash.\n"
                            "fio request metadata:\n"
                            "index_entry: %p, state: %d, dbid: %lu, key: %p, key_len: %zu, read_type: %d, "
                            "requested_item_data: %d, request_context %p, "
                            "data_buffer_size: %zu"
                            "logRunCronTask states:\n"
                            "log_offset: %zu, pos: %zu, value: %p, value_len: %zu, item: %p, "
                            "total_len: %zu, num_pages_required_to_read_key: %zu, "
                            "num_pages_required_to_read_item: %zu, "
                            "num_data_buffer_pages: %zu, found_item: %d, partial_key: %d, "
                            "requested_item_with_value: %d, "
                            "partial_item: %d"
                            "Snapshot Related states:\n"
                            "should_add_item_to_rdb: %d, is_item_in_ts_snapshot_range: %d",
                            (void *)index_entry, state, dbid, (void *)key, key_len, read_type, requested_item_data,
                            (void *)request_context, data_buffer_size,
                            log_offset, pos, (void *)value, value_len, (void *)item, total_len,
                            num_pages_required_to_read_key, num_pages_required_to_read_item,
                            num_data_buffer_pages, found_item, partial_key,
                            requested_item_with_value, partial_item, should_add_item_to_rdb,
                            is_item_in_ts_snapshot_range);
                    }
                    completion_callback(request_context, value, value_len, should_add_item_to_rdb);
                }
            } else {
                // if the fio request was invalidated, retry reading the index entry
                log->metrics.num_unused_disk_read_hash_collision++;
                log->metrics.unused_disk_read_bytes_hash_collision += data_buffer_size;
                logReadFromIndexEntry(log, index_entry, dbid, key, key_len, read_type,
                        request_context, completion_callback);
            }
finish_processing_request:
            fcFree(data_buffer);
        }
    }
    invokeAsioControlMsgCallback();
    logFlushStagingBufferIfRequired(log, log->staging_buffer_flush_size_threshold_bytes);

    // If we have only dead/garbage items in log, we will reset the head/tail offset so that we can avoid the compute
    // of GC and start the log from scratch.
    resetHeadTailOffsetOfLogIfRequired(log);
    snapshotManagerCronTask();
    indexGrowthCronTask(log);

    uint64_t current_time_us = timeInMicrosecond(log);
    // Try to update the write rate. This ensure that the write rate gets updated even when there are not actual
    // writes.
    updateWriteRateInfo(current_time_us, &(log->write_rate_info), 0);

    // When we move to a new time window, we refresh the counter keeping track of the amount garbage collected bytes
    resetGarbageCollectionTimeWindowIfRequired(current_time_us, &(log->garbage_collector_info));

    // Try to start garbage collection after processing the previous block that read for garbage collection. This will
    // ensure that garbage collection is started as soon as the processing of the previous block is done if required.
    // In logShouldTriggerCronTasks function, we rely on this because if garbage collection is not running we assume
    // that garbage collection for the current window is done. This indicate that there is no pending cron task related
    // to garbage collection.
    //
    // Snapshot support: while a fork-based snapshot child is alive, GC is
    // paused so that log offsets frozen in the child's CoW index remain
    // valid on the shared flash file (GC relocates/frees live regions;
    // appends at the tail are safe). Read/write completions above are NOT
    // affected -- only the iterator that drives GC is skipped.
    if (!fc_gc_paused) logIteratorCron(log->log_iterator);

    // Update the snapshotting range start offset if tail offset has been moved due to evictions during forkless save
    // replication. This is required because whenever eviction happens in flash, we move the log tail offset. As tail
    // offset moves, head offset can overwrite the original snapshotting range of forkless save. Hence we need to update
    // the snapshotting range accordingly.
    snapshotManagerUpdateSnapshottingRangeTailOffset(log->tail_offset);
    return FC_OK;
}

// Non static for unit test
void waitTillNoPendingIoAndGarbageCollection(flashcacheLog *log, int is_empty_staging_buffer_required) {
    // Spin till:
    // 1. There are pending read request that came before logStartSnaphotting invocation.
    // 2. There is a GC run in progress.
    // 3. There are items in the staging buffer that has not been written to the log depending on
    //    `is_empty_staging_buffer_required` flag. In case of end of forkless save replication, when log is full and items
    //    are still present in staging buffer we wont be able to flush it in log as GC are disabled during forkless save.
    //    In that case we dont wait for staging buffer to become empty.
    // 4. There is a log flush in progress
    while ((!fioRequestIsEmpty(&(log->log_flush_fio_request))) ||
           log->num_inflight_item_read_io_request ||
           log->garbage_collector_info.is_running ||
           (is_empty_staging_buffer_required && stagingBufferGetTotalItemSize(log->staging_buffer))) {
        logFlushStagingBufferIfRequired(log, 0);
        flashcacheAssert(logRunCronTasks(log) == FC_OK);
    }
}

static void logStartSave(flashcacheLog *log, flashcacheSnapshotSecret *snapshot_secret,
                         char const *snapshot_filename,
                         flashcacheSnapshotCallbackDetails *file_based_snapshot_callback_details,
                         flashcacheSnapshotWriter *snapshot_writer,
                         int checksum_verification_enabled,
                         flashcacheSnapshotVersion snapshot_version,
                         flashcacheSnapshotSaveType snapshot_save_type,
                         flashcacheLogIterationCallbackDetails *log_iteration_completion_callback_details) {
    log->metrics.num_start_save_request++;
    log->metrics.item_bytes_moved_from_disk_during_forkless_save = 0;

    // Pause garbage collection and eviction so no new garbage collection starts
    log->garbage_collector_info.can_start_garbage_collection = 0;

    // Wait till all all the pending IO request has been served and there is no garbage collection in progress
    waitTillNoPendingIoAndGarbageCollection(log, 1);

    resetHeadTailOffsetOfLogIfRequired(log);  // Reset the head and tail offset of the log if required.
    flashcacheLogger(FC_LL_NOTICE, "Starting save operation for Bgsave or forkless save with Head offset : %lu, "
                                   "Tail offset : %lu, Active size = %lu",
                     log->head_offset,  log->tail_offset, getActiveLogSizeBytes(log));
    snapshotManagerStartSave(snapshot_secret,
                             log->tail_offset,
                             getActiveLogSizeBytes(log), log->allocated_log_size_bytes,
                             log->allocated_log_size_bytes_per_db,
                             log->monotonic_clock_us,
                             &(log->hasher),
                             log->index_list[log->current_index_growth_dbid],
                             log->current_index_growth_dbid,
                             file_based_snapshot_callback_details, snapshot_filename,
                             snapshot_writer,
                             checksum_verification_enabled,
                             snapshot_version,
                             snapshot_save_type,
                             &(log->garbage_collector_info.can_do_log_compaction),
                             log_iteration_completion_callback_details);

    // Unpause garbage collection / eviction
    log->garbage_collector_info.can_start_garbage_collection = 1;
}

void logStartFileBasedSave(flashcacheLog *log, char const *snapshot_filename,
        flashcacheSnapshotSecret *snapshot_secret,
        flashcacheSnapshotCallbackDetails *file_based_snapshot_callback_details,
        int checksum_verification_enabled, flashcacheSnapshotVersion snapshot_version,
        flashcacheSnapshotSaveType snapshot_save_type) {
    logStartSave(log, snapshot_secret, snapshot_filename,
                 file_based_snapshot_callback_details, NULL,
                 checksum_verification_enabled, snapshot_version, snapshot_save_type, NULL);
}

void logStartStreamBasedSave(flashcacheLog *log, flashcacheSnapshotSecret *snapshot_secret,
                             flashcacheSnapshotWriter *snapshot_writer,
                             flashcacheSnapshotVersion snapshot_version,
                             flashcacheSnapshotSaveType snapshot_save_type,
                             flashcacheLogIterationCallbackDetails *log_iteration_completion_callback_details) {
    logStartSave(log, snapshot_secret, NULL, NULL, snapshot_writer, 0, snapshot_version, snapshot_save_type,
                 log_iteration_completion_callback_details);
}

void logCancelSave(flashcacheLog *log) {
    log->metrics.num_cancel_save_request++;
    snapshotManagerCancelSave();
}

void logLoadSnapshot(flashcacheLog *log,
                     char const *snapshot_filename,
                     flashcacheSnapshotSecret *secret_response,
                     int *checksum_comparison_result) {
    log->metrics.num_load_request++;

    // Pause garbage collection so no new garbage collection starts
    log->garbage_collector_info.can_start_garbage_collection = 0;

    // Wait till all all the pending IO request has been served and there is no garbage collection in progress
    waitTillNoPendingIoAndGarbageCollection(log, 1);

    // Cancel any ongoing snapshot before loading a new snapshot
    if (snapshotManagerIsRunning()) {
        snapshotManagerCancelSave();
    }

    // Load the new snapshot
    snapshotManagerLoad(log, snapshot_filename, secret_response, checksum_comparison_result);

    // Unpause garbage collection
    log->garbage_collector_info.can_start_garbage_collection = 1;

    // Unpause running growth operation. This is required when we load a snapshot with Paused Status.
    indexUnpauseGrowth(log->index_list[log->current_index_growth_dbid]);
}

size_t logGetCountBasedMetric(flashcacheLog *log, flashcacheCountBasedMetrics metric) {
    size_t max_allocated_log_size_bytes = getMaxAllocatedLogSizeBytes(log);
    size_t allocated_log_size_bytes = getAllocatedLogSizeBytesWithBufferedWrites(log);
    switch (metric) {
        case FC_FREE_ALLOCATABLE_DB_SIZE_BYTES:
            if (allocated_log_size_bytes > max_allocated_log_size_bytes) {
                return 0ul;
            }
            return max_allocated_log_size_bytes - allocated_log_size_bytes;
        case FC_DB_USED_OVER_ALLOCATABLE_DB_SIZE_BYTES:
            if (max_allocated_log_size_bytes > allocated_log_size_bytes) {
                return 0ul;
            }
            return allocated_log_size_bytes - max_allocated_log_size_bytes;
        case FC_ALLOCATED_DB_SIZE_BYTES:
            return allocated_log_size_bytes;
        case FC_ACTIVE_DB_SIZE_BYTES:
            return getActiveLogSizeBytes(log);
        case FC_TOTAL_DB_SIZE_BYTES:
            return log->log_size_bytes;
        case FC_ITEM_PENDING_FLUSH_SIZE_BYTES:
            return log->staging_buffer->total_item_size;
        case FC_NUM_READ_IN_FLIGHT:
            return log->num_inflight_item_read_io_request;
        case FC_GARBAGE_COLLECTION_CURR_RATE_BYTES_PER_SECOND:
            return log->garbage_collector_info.required_garbage_collection_bytes_per_second;
        case FC_MIN_GARBAGE_COLLECTION_RATE:
            return log->min_garbage_collection_rate;
        case FC_GARBAGE_COLLECTION_NUM_DISK_READ:
            return log->metrics.garbage_collection_num_disk_read;
        case FC_GARBAGE_COLLECTION_READ_BYTES:
            return log->metrics.garbage_collection_read_bytes;
        case FC_GARBAGE_COLLECTION_WRITE_BYTES:
            return log->metrics.garbage_collection_write_bytes;
        case FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED:
            return log->metrics.garbage_collection_num_items_moved;
        case FC_NUM_UNUSED_DISK_READ_HASH_COLLISION:
            return log->metrics.num_unused_disk_read_hash_collision;
        case FC_UNUSED_DISK_READ_BYTES_HASH_COLLISION:
            return log->metrics.unused_disk_read_bytes_hash_collision;
        case FC_NUM_PARTIAL_ITEM_READ:
            return log->metrics.num_partial_item_read;
        case FC_PARTIAL_ITEM_READ_BYTES:
            return log->metrics.partial_item_read_bytes;
        case FC_SAVE_NUM_START_REQUEST:
            return log->metrics.num_start_save_request;
        case FC_SAVE_NUM_CANCEL_REQUEST:
            return log->metrics.num_cancel_save_request;
        case FC_SAVE_NUM_COMPLETED:
            return log->metrics.num_save_completed;
        case FC_SAVE_NUM_CANCELLED:
            return log->metrics.num_save_cancelled;
        case FC_NUM_LOAD_REQUEST:
            return log->metrics.num_load_request;
        case FC_NUM_READ_REQUEST:
            return log->metrics.num_read_request;
        case FC_NUM_WRITE_REQUEST:
            return log->metrics.num_write_request;
        case FC_NUM_DELETE_REQUEST:
            return log->metrics.num_delete_request;
        case FC_NUM_OPTIMIZED_DELETES:
            return log->metrics.num_optimized_deletes;
        case FC_TOTAL_DISK_WRITE_BYTES:
            return log->metrics.total_disk_write_bytes;
        case FC_TOTAL_DISK_READ_BYTES:
            return log->metrics.total_disk_read_bytes;
        case FC_NUM_DISK_WRITE:
            return log->metrics.num_disk_write;
        case FC_NUM_DISK_READ:
            return log->metrics.num_disk_read;
        case FC_NUM_ITEMS:
            return log->num_items;
        case FC_NUM_ITEMS_EVICTED:
            return log->metrics.num_items_evicted;
        case FC_TOTAL_EVICTED_ITEMS_SIZE_BYTES:
            return log->metrics.total_item_evicted_size_bytes;
        case FC_IS_INDEX_GROWING:
            return isIndexGrowthRunning(log);
        case FC_NUM_INDEX_GROWTH_RUN:
            return log->num_index_growth_run;
        case FC_IS_EVICTING_UNDER_MAX_LOGSIZE:
            return log->evict_under_max_logsize_min_rate && log->eviction_enabled;
        case FC_NUM_ITEMS_EVICTED_UNDER_MAX_LOGSIZE:
            return log->metrics.num_items_evicted_under_logsize;
        case FC_NUM_RETRYABLE_DISK_ERROR:
            return fioGetCountBasedMetric(FC_NUM_RETRYABLE_DISK_ERROR);
        case FC_IS_WAITING_FOR_ENGINE_SNAPSHOTTING_COMPLETION:
            return snapshotManagerGetCountBasedMetric(FC_IS_WAITING_FOR_ENGINE_SNAPSHOTTING_COMPLETION);
        case FC_CURR_NUM_DELETE_REPL_CMD:
            return snapshotManagerGetCountBasedMetric(FC_CURR_NUM_DELETE_REPL_CMD);
        case FC_CURR_DELETE_REPL_CMD_BYTES:
            return snapshotManagerGetCountBasedMetric(FC_CURR_DELETE_REPL_CMD_BYTES);
        case FC_CURR_NUM_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE:
            return snapshotManagerGetCountBasedMetric(FC_CURR_NUM_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE);
        case FC_CURR_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE_BYTES:
            return snapshotManagerGetCountBasedMetric(FC_CURR_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE_BYTES);
        case FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG:
            return snapshotManagerGetCountBasedMetric(FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG);
        case FC_LAST_NUM_DELETE_REPL_CMD:
            return snapshotManagerGetCountBasedMetric(FC_LAST_NUM_DELETE_REPL_CMD);
        case FC_LAST_DELETE_REPL_CMD_BYTES:
            return snapshotManagerGetCountBasedMetric(FC_LAST_DELETE_REPL_CMD_BYTES);
        case FC_LAST_NUM_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE:
            return snapshotManagerGetCountBasedMetric(FC_LAST_NUM_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE);
        case FC_LAST_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE_BYTES:
            return snapshotManagerGetCountBasedMetric(FC_LAST_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE_BYTES);
        case FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG:
            return snapshotManagerGetCountBasedMetric(FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG);
        case FC_LATEST_KEEP_ALIVE_MSG_TIME_US:
            return snapshotManagerGetCountBasedMetric(FC_LATEST_KEEP_ALIVE_MSG_TIME_US);
        case FC_ITEM_BYTES_MOVED_FROM_DISK:
            return log->metrics.item_bytes_moved_from_disk_during_forkless_save;
        case FC_ITEM_BYTES_DELETED_FROM_DISK:
            return log->metrics.item_bytes_deleted_from_disk_during_forkless_save;
        default:
            flashcacheAssertWithLogging(0, "Unknown metric: [%d]", metric);
    }
    flashcacheAssertWithLogging(0, "Implementation error as this part of code "
            "should not be reachable, metric: [%d]", metric);
    return 0;
}

void logRelease(flashcacheLog *log) {
    // Set required garbage collection to 0 so that no more garbage collection get started
    log->garbage_collector_info.required_garbage_collection_bytes_per_second = 0;
    while ((!fioRequestIsEmpty(&(log->log_flush_fio_request))) || log->num_inflight_item_read_io_request
            || log->garbage_collector_info.is_running) {
        flashcacheAssert(logRunCronTasks(log) == FC_OK);
        usleep(FC_SLEEP_INTERVAL_WAITING_TO_FINISH_PENDING_TASK_MICROSECONDS);
    }
    stagingBufferRelease(log->staging_buffer);
    snapshotManagerInfoRelease();

    for (size_t i = 0; i < log->num_databases; ++i) {
        indexRelease(log->index_list[i]);
    }
    fcFree(log->index_list);
    fcFree(log->allocated_log_size_bytes_per_db);
    fcFree(log->log_iterator);
    fcFree(log->inflight_item_read_io_metadata);
    fioReleaseContext(log->fio_context);
    fcFree(log);
}

void logFlush(flashcacheLog *log, uint64_t dbid) {
    flashcacheAssert((log != NULL) &&
                     ((dbid < log->num_databases) || (dbid == FC_FLUSH_ALL_DBIDS)));
    if (snapshotManagerIsRunning()) {
        snapshotManagerCancelSave();
    }

    log->garbage_collector_info.can_start_garbage_collection = 0;
    waitTillNoPendingIoAndGarbageCollection(log, 1);

    if (dbid == FC_FLUSH_ALL_DBIDS) {
        for (size_t ii = 0; ii < log->num_databases; ++ii) {
            log->num_items -= log->index_list[ii]->num_items;
            decrementAllocatedLogSize(log, (uint32_t) ii, log->allocated_log_size_bytes_per_db[ii]);
            indexRecreate(log->index_list[ii], log->index_list[ii]->base_size_bits, 0, NOT_RUNNING,
                    0, log->hasher.hash_function);
        }
    } else {
        uint32_t curr_dbid = (uint32_t)dbid;
        log->num_items -= log->index_list[curr_dbid]->num_items;
        decrementAllocatedLogSize(log, curr_dbid, log->allocated_log_size_bytes_per_db[curr_dbid]);
        indexRecreate(log->index_list[curr_dbid], log->index_list[curr_dbid]->base_size_bits,
                0, NOT_RUNNING, 0, log->hasher.hash_function);
    }
    resetHeadTailOffsetOfLogIfRequired(log);  // Reset the head and tail offset of the log if required.
    log->garbage_collector_info.can_start_garbage_collection = 1;
}

void logDumpState(flashcacheLog *log, int level) {
    flashcacheLogger(level, "# Logging log state\r\n"
            "num databases: [%u]\r\n"
            "num items: [%lu]\r\n"
            "head offset: [%lu]\r\n"
            "tail offset: [%lu]\r\n"
            "allocated log size bytes: [%lu]\r\n"
            "log size bytes: [%lu]\r\n"
            "Overriden log size bytes: [%lld]\r\n"
            "max allocated log size percent: [%u]\r\n"
            "head offset after flush: [%lu]\r\n"
            "num inflight read requests: [%d]\r\n"
            "flush in progress: [%s]",
            log->num_databases,
            log->num_items,
            log->head_offset,
            log->tail_offset,
            log->allocated_log_size_bytes,
            log->log_size_bytes,
            log->overriden_allocatable_log_size_bytes,
            log->max_allocated_log_size_percent,
            log->head_offset_after_flush_succeed,
            log->num_inflight_item_read_io_request,
            fioRequestIsEmpty(&(log->log_flush_fio_request)) ? "no" : "yes");

    flashcacheLogger(level, "# Logging garbage collection state\r\n"
            "garbage collection in progress: [%s]\r\n"
            "garbage collection can be started: [%s]\r\n"
            "pending read buffer processing: [%s]\r\n"
            "adaptive garbage collection: [%s]\r\n"
            "read buffer offset: [%lu]\r\n"
            "max batch processing time (us): [%lu]\r\n"
            "required garbage collection rate (bytes/second): [%lu]\r\n"
            "last partial item read size bytes: [%lu]\r\n"
            "garbage collected bytes in current second: [%lu]\r\n"
            "current aggregation window start time (us): [%lu]\r\n"
            "total garbage collected bytes: [%lu]\r\n"
            "total active items moved bytes: [%lu]\r\n",
            log->garbage_collector_info.is_running ? "yes" : "no",
            log->garbage_collector_info.can_start_garbage_collection ? "yes" : "no",
            log->log_iterator->pending_log_data_processing ? "yes" : "no",
            log->garbage_collector_info.enable_adaptive_garbage_collection_rate ? "yes" : "no",
            log->log_iterator->log_data_buffer_offset,
            log->log_iterator->max_batch_processing_time_microseconds,
            log->garbage_collector_info.required_garbage_collection_bytes_per_second,
            log->log_iterator->partial_item_size_bytes,
            log->garbage_collector_info.garbage_collection_bytes_in_current_second,
            log->garbage_collector_info.current_aggregation_window_start_time_us,
            log->garbage_collector_info.total_garbage_collected_bytes,
            log->metrics.garbage_collection_write_bytes);
}

int logShouldRunCronTasksImmediately(struct flashcacheLog *log) {
    // Cron task should be run immediately if there is an ongoing log flush
    if (!fioRequestIsEmpty(&(log->log_flush_fio_request))) {
        return 1;
    }

    // Cron task should be run immediately if there are pending read request to be processed
    if (log->num_inflight_item_read_io_request) {
        return 1;
    }

    // Cron task should be run immediately if snapshotting is in progress
    if (snapshotManagerIsRunning()) {
        return 1;
    }

    // Cron task should be run immediately if garbage collection for current window is in progress
    if (log->garbage_collector_info.is_running) {
        return 1;
    }

    // Cron task should be run immediately if index growth operation is in progress
    if (isIndexGrowthRunning(log)) {
        return 1;
    }

    return 0;
}

void logCompleteForklessSaveReplication(flashcacheLog *log) {
    // Pause garbage collection and eviction so no new garbage collection starts
    log->garbage_collector_info.can_start_garbage_collection = 0;
    // Wait till all all the pending IO request has been served
    waitTillNoPendingIoAndGarbageCollection(log, 0);
    snapshotManagerSetHasSnapshottingCompletedInEngineLayer(1);
    // Synchronously wait for Replication to complete.
    while (snapshotManagerIsRunning()) {
        logRunCronTasks(log);
    }
    // Resume garbage collection
    log->garbage_collector_info.can_start_garbage_collection = 1;
}

void logSetConfig(flashcacheLog *log, flashcacheConfig *config) {
    flashcacheAssert(config != NULL);

    ssize_t value = config->numeric_value;
    switch (config->key) {
        case FC_CONFIG_KEY_EVICTION_ENABLED:
            flashcacheAssert(value == 0 || value == 1);
            log->eviction_enabled = value;
            break;
        case FC_CONFIG_KEY_OVERRIDEN_ALLOCATABLE_DB_SIZE_BYTES:
            flashcacheAssert(value >= -1);
            flashcacheAssert(value <= (int64_t) log->log_size_bytes);
            log->overriden_allocatable_log_size_bytes = value;
            break;
        case FC_CONFIG_KEY_MAX_BUFFERED_WRITE_SIZE_BYTES:
            flashcacheAssert(value >= 0);
            log->max_buffered_write_size_bytes = (size_t) value;
            break;
        case FC_CONFIG_KEY_BUFFERED_WRITE_FLUSH_THRESHOLD_BYTES:
            flashcacheAssert(value >= 0);
            log->staging_buffer_flush_size_threshold_bytes = (size_t) value;
            break;
        case FC_CONFIG_KEY_MAX_SNAPSHOT_BUFFER_SIZE_BYTES:
            flashcacheAssert(value >= 1);
            snapshotManagerSetMaxSnapshotBufferSizeBytes((size_t) value);
            break;
        case FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS:
            flashcacheAssert(value == 0 || value == 1);
            snapshotManagerSetHasSnapshottingCompletedInEngineLayer((uint8_t) value);
            break;
        case FC_CONFIG_KEY_SNAPSHOT_KEEP_ALIVE_MSG_INTERVAL_US:
            flashcacheAssert(value >= 1);
            snapshotManagerSetSnapshotKeepAliveMsgIntervalUs((uint64_t) value);
            break;
        case FC_CONFIG_KEY_REPLICATION_LINK_TIMEOUT_SECS:
            flashcacheAssert(value >= 1);
            snapshotManagerSetReplicationLinkTimeoutSecs((size_t) value);
            break;
        case FC_CONFIG_KEY_MIN_GARBAGE_COLLECTION_RATE:
            log->min_garbage_collection_rate = (uint32_t) value;
            break;
        case FC_CONFIG_KEY_MAX_DYNAMIC_GARBAGE_COLLECTION_RATE:
            log->max_dynamic_garbage_collection_rate = (uint32_t) value;
            break;
        case FC_CONFIG_KEY_EVICT_UNDER_MAX_LOGSIZE_RATE:
            flashcacheAssert(value >= 0);
            log->evict_under_max_logsize_min_rate = (uint32_t) value;
            if (log->evict_under_max_logsize_min_rate) {
                log->last_evict_under_max_logsize_start = timeInMicrosecond(log);
            }
            break;
        case FC_CONFIG_KEY_EVICT_UNDER_MAX_LOGSIZE_TIME_LIMIT:
            flashcacheAssert(value >= 1);
            log->evict_under_max_logsize_time_limit = (uint32_t) value;
            break;
        case FC_CONFIG_KEY_OPTIMIZED_DELETE_ENABLED:
            flashcacheAssert(value == 0 || value == 1);
            log->optimized_delete_enabled = (uint8_t) value;
            break;
        default:
            flashcacheAssertWithLogging(0, "Unknown type: %d", config->key);
    }
}

void logGetConfig(flashcacheLog *log, flashcacheConfig *config) {
    flashcacheAssert(config != NULL);

    switch (config->key) {
        case FC_CONFIG_KEY_EVICTION_ENABLED:
            config->numeric_value = log->eviction_enabled;
            break;
        case FC_CONFIG_KEY_OVERRIDEN_ALLOCATABLE_DB_SIZE_BYTES:
            config->numeric_value = log->overriden_allocatable_log_size_bytes;
            break;
        case FC_CONFIG_KEY_MAX_BUFFERED_WRITE_SIZE_BYTES:
            config->numeric_value = (ssize_t) log->max_buffered_write_size_bytes;
            break;
        case FC_CONFIG_KEY_BUFFERED_WRITE_FLUSH_THRESHOLD_BYTES:
            config->numeric_value = (ssize_t) log->staging_buffer_flush_size_threshold_bytes;
            break;
        case FC_CONFIG_KEY_MAX_SNAPSHOT_BUFFER_SIZE_BYTES:
            config->numeric_value = (ssize_t) snapshotManagerGetMaxSnapshotBufferSizeBytes();
            break;
        default:
            flashcacheAssertWithLogging(0, "Unknown type: %d", config->key);
    }
}

void logFsyncBufferedWrites(flashcacheLog *log) {
    log->garbage_collector_info.can_start_garbage_collection = 0;
    waitTillNoPendingIoAndGarbageCollection(log, 1);
    log->garbage_collector_info.can_start_garbage_collection = 1;
}

void invokeAsioControlMsgCallback() {
    if (asio_control_msg_callback.callback != NULL) {
        asio_control_msg_callback.callback(asio_control_msg_callback.context);
    }
}

/* ---------------------------------------------------------------------------
 * Fork-child synchronous read (snapshot support)
 *
 * Reads one item's serialized bytes without touching the async fio path.
 * Designed to be called from a fork()ed child process (or from the main
 * thread of the parent while the IO thread is parked):
 *
 *  - The index is walked directly (CoW memory in a child; quiesced in the
 *    parent case). No index mutation of any kind.
 *  - Staging-buffer entries are served from memory (CoW heap).
 *  - On-flash entries are read with pread(2) on the inherited fd. pread does
 *    not share a file offset, and the io_uring/libaio ring is never touched,
 *    so this is safe alongside the parent's async traffic.
 *  - Offset validity across the child's lifetime is guaranteed by the GC
 *    pause (logSetGcPaused): GC is the only writer that relocates or frees
 *    live regions; normal writes append at the tail.
 *
 * Returns FC_OK and a malloc'd (*out_item) holding the full serialized item
 * (header+key+value) on success; FC_ERR_CATCH_ALL if the key is absent or
 * fails validation. The caller must free(*out_item).
 * ---------------------------------------------------------------------------*/
flashcacheReturnCode logForkChildReadItem(flashcacheLog *log, uint32_t dbid,
        char const *key, size_t key_len, char **out_item, size_t *out_len) {
    *out_item = NULL;
    *out_len = 0;
    if (dbid >= log->num_databases) return FC_ERR_CATCH_ALL;

    indexEntry *entry = indexGetHeadEntry(log->index_list[dbid], key, key_len);
    uint64_t want_hash = computeCollisionHash(log->hasher.hash_function, key, key_len);

    while (entry) {
        if (!entry->item_entry.log_entry.on_flash) {
            /* Staging buffer: item bytes are in (CoW) memory. */
            stagingBufferEntry *sbe = entry->item_entry.staging_buffer_entry;
            if (compareKeyAndDbidInSerializedItem(sbe->item, dbid, key, key_len)) {
                char *copy = malloc(sbe->item_len);
                if (copy == NULL) return FC_ERR_CATCH_ALL;
                memcpy(copy, sbe->item, sbe->item_len);
                *out_item = copy;
                *out_len = sbe->item_len;
                return FC_OK;
            }
        } else if (entry->item_entry.log_entry.hash == want_hash) {
            /* On flash. The 16-bit collision hash can false-positive, so the
             * key inside the read bytes must be verified; on mismatch we keep
             * walking the chain. */
            logEntry *le = &entry->item_entry.log_entry;
            size_t log_offset = expandTrimmedLogOffset(le->trimmed_log_offset);
            size_t block_num_pages = (size_t)le->additional_pages + 1;
            int pages_unknown = 0;
            if (block_num_pages == (size_t)FC_MAX_ADDITIONAL_PAGES + 1) {
                /* Very large item: page count saturated; read the header
                 * first, then the exact remainder. */
                pages_unknown = 1;
                block_num_pages = 1;
                if ((FC_PAGESIZE - (log_offset % FC_PAGESIZE)) < sizeof(itemHeader))
                    block_num_pages = 2;
            }

            size_t page_aligned_offset = getFloorPageAlignedOffset(log_offset);
            size_t item_pos = log_offset - page_aligned_offset;
            size_t blocksize = FC_PAGESIZE * block_num_pages;
            char *buf = NULL;
            if (posix_memalign((void **)&buf, FC_PAGESIZE, blocksize) != 0)
                return FC_ERR_CATCH_ALL;

            ssize_t n = pread(log->fio_context->fd, buf, blocksize,
                              (off_t)page_aligned_offset);
            if (n < 0 || (size_t)n < item_pos + sizeof(itemHeader)) {
                free(buf);
                return FC_ERR_CATCH_ALL;
            }

            char *item = buf + item_pos;
            if (!validateHeaderInSerializedItem(item, log->crc_function)) {
                /* Torn/garbage header -- treat as not found rather than
                 * asserting: the snapshot child must never kill itself on a
                 * single bad item. */
                free(buf);
                entry = entry->next;
                continue;
            }
            size_t total_len = extractTotalLenFromSerializedItem(item);

            if (pages_unknown && item_pos + total_len > (size_t)n) {
                /* Re-read with the exact size now that the header told us. */
                size_t full_pages =
                    (item_pos + total_len + FC_PAGESIZE - 1) / FC_PAGESIZE;
                char *big = NULL;
                if (posix_memalign((void **)&big, FC_PAGESIZE,
                                   full_pages * FC_PAGESIZE) != 0) {
                    free(buf);
                    return FC_ERR_CATCH_ALL;
                }
                ssize_t n2 = pread(log->fio_context->fd, big,
                                   full_pages * FC_PAGESIZE,
                                   (off_t)page_aligned_offset);
                free(buf);
                if (n2 < 0 || (size_t)n2 < item_pos + total_len) {
                    free(big);
                    return FC_ERR_CATCH_ALL;
                }
                buf = big;
                item = buf + item_pos;
            } else if (item_pos + total_len > (size_t)n) {
                free(buf);
                return FC_ERR_CATCH_ALL;
            }

            if (compareKeyAndDbidInSerializedItem(item, dbid, key, key_len)) {
                char *copy = malloc(total_len);
                if (copy == NULL) {
                    free(buf);
                    return FC_ERR_CATCH_ALL;
                }
                memcpy(copy, item, total_len);
                free(buf);
                *out_item = copy;
                *out_len = total_len;
                return FC_OK;
            }
            free(buf); /* collision false positive: keep walking */
        }
        entry = entry->next;
    }
    return FC_ERR_CATCH_ALL;
}
