#include "include/snapshot_version_two.h"
#include "include/serialization.h"
#include "include/util.h"
#include "include/crc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define FC_SNAPSHOT_VERSION (2)
#define FC_SNAPSHOT_MAX_LOG_COPY_BUFFER_SIZE_BYTES (1024 * 1024)                             // 1 MiB
#define FC_SNAPSHOT_DATA_BUFFER_SIZE_BYTES FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES
#define FC_SNAPSHOT_MAX_WRITE_BUFFERED_SIZE_TO_STOP_SNAPSHOT_BYTES (1024LL * 1024 * 1024)    // 1 GiB
#define FC_SNAPSHOT_MAX_BATCH_PROCESSING_TIME_US (20)                                        // 20 us
#define FC_CHECKSUM_FILE_EXTENSION ".crc"
#define FC_SNAPSHOT_DEFAULT_KEEP_ALIVE_MSG_INTERVAL_US (5000000)                             // 5 seconds
#define FC_SNAPSHOT_DEFAULT_MAX_REPLICATION_LINK_SECS (21600)                                // 6 hours

extern snapshotMetrics snapshot_metrics;
// The snapshot file contains 2 sections in the following order:
// 1. Metadata section: The metadata of the snapshot is written from offset 0. The metadata contains information used
// for loading the snapshot like the snapshot version, num_databases, size of data section.
// 2. data section: This section contains the serialized item (header, key, value).

typedef struct snapshotV2Metadata {
    // Snapshot version
    uint32_t version;

    // Number of databases
    uint32_t num_databases;

    // Offset in log file from where snapshot begins
    size_t data_section_start_offset;

    // The size of snapshot file (in bytes)
    size_t snapshot_file_data_size_bytes;

    // the snapshot correlation secret
    flashcacheSnapshotSecret snapshot_secret;

    size_t num_items_per_db[];
} snapshotV2Metadata;

#ifndef __cplusplus
// Verify the assumption that snapshot metadata can fit in a single page
_Static_assert(sizeof(snapshotV2Metadata) <= FC_PAGESIZE, "Snapshot metadata larger than page size");
#endif

static size_t getSnapshotV2MetadataSize(uint32_t num_databases) {
    return sizeof(snapshotV2Metadata) + num_databases * sizeof(size_t);
}

static size_t getSerializedSnapshotV2MetadataSize(uint32_t num_databases) {
    return getCeilPageAlignedOffset(getSnapshotV2MetadataSize(num_databases));
}

/* Send keepalive message to ASIO if it has been at least `snapshot_keep_alive_msg_interval_us`
 * microseconds since the last keepalive message was sent
 * and we have not exceeded the replication link timeout.
 */
static void sendKeepAliveMessageToASIOIfRequired(snapshotVersionTwoInfo *snapshot_info,
                                           flashcacheSnapshotWriter *snapshot_writer) {
    if (snapshot_writer == NULL) {
        return;
    }
    // We will periodically send keep alive message to the replica ASIO layer to keep the replication link alive.
    uint64_t current_time_us = snapshot_info->monotonic_clock_us();
    uint64_t expected_keep_alive_time_us = snapshot_info->latest_keep_alive_msg_time_us +
                                            snapshot_info->snapshot_keep_alive_msg_interval_us;
    size_t max_num_keep_alive_msg = (snapshot_info->replication_link_timeout_secs * FC_SECOND_TO_MICROSECOND) /
                                    (size_t)(snapshot_info->snapshot_keep_alive_msg_interval_us);

    if ((current_time_us >= expected_keep_alive_time_us)
        && snapshot_info->num_replication_link_keep_alive_msg < max_num_keep_alive_msg) {
        snapshot_writer->keep_alive(snapshot_info->snapshot_common.snapshot_writer->callback_context);
        snapshot_info->latest_keep_alive_msg_time_us = current_time_us;
        snapshot_info->num_replication_link_keep_alive_msg++;
    }
}

/*
 * Set of functions to check if an item is within the snapshotting scope but not
 * yet being processed (added to the snapshot).
 *
 * This is identified by comparing the offset of the item to the last processed
 * item (log_processed_offset) and the end of the snapshotting target (log_file_offset_including_pending_bytes).
 * If the item sits between the two values we return 1. Otherwise 0.
 *
 * Note: flashcache log runs from tail (start) to head (end).
 *
 * Legend:
 *   +: Item to be included in snapshot.
 *   -: Item not in the snapshot.
 *   X: last item snapshotted already (log_processed_offset).
 *   O: offset of the item in question.
 *   S: Start offset for the snapshot.
 *   E: Modulo end offset for the snapshot (log_file_offset_including_pending_bytes % log_file_size_bytes)
 *   HEAD: End of the log used to store data on SSD. (log_file_size_bytes)
 *   TAIL: Start of the log to store data on SSD.
 *
 * There are multiple cases that we need to handle because of wrap-around
 * of multiple values.
 *
 * Since there different cases depending on whether a value
 * is wrap-around or not. To simplify, we normalize all possible
 * wrap-around values to non-wrap-around.
 * This includes:
 *      log_processed_offset(X)
 *      offset(O)
 * Then, we can do the comparison without the need for any
 * check for different wrap-around cases.
 *
 * 1- Log not wrapped around:
 *  TAIL                               HEAD
 *    |---S++++++++X+++++++O+++++++E----|
 *  --> No normalization needed.
 *
 * 2- Log wrapped around:
 *  2.1- Last snapshot item not wrapped around
 *   TAIL                              HEAD
 *    |+++O+++E----------S++++++X+++++++|
 *  --> O is wrapped around, so O += log_size_bytes.
 *
 *  Explanation:
 *  E above is actually modulo, the actual end offset is beyond HEAD.
 *   TAIL                          HEAD
 *    |+++O----------S++++++X+++++++|+++++++E
 *
 *  So here we normalize O by O += log_size_bytes.
 *   TAIL                          HEAD
 *    |-------------S++++++X+++++++|++O+++++E
 *
 *  2.2- Last snapshot item wrapped around
 *   TAIL                              HEAD
 *    |++++X++O++E----------S+++++++++++|
 *  --> Both X and O wrapped around, so
 *      X += log_size_bytes and
 *      O += log_size_bytes.
 *
 *  2.3- Both X and O not wrapped around
 *   TAIL                              HEAD
 *    |++++++E----------S++++++X++++O+++|
 *  --> Both X and O are not wrapped around,
 *      so no normalization needed.
 *
 *  3- Any other case will return 0
 */
static void normalizeWrappedAroundValues(size_t log_file_offset_including_pending_bytes, size_t log_file_size_bytes,
                                         size_t *log_processed_offset, size_t *offset,
                                         size_t log_file_pending_processing_bytes) {
    if (log_file_offset_including_pending_bytes > log_file_size_bytes) {
        size_t modulo_end_offset = log_file_offset_including_pending_bytes % log_file_size_bytes;
        if (*offset < modulo_end_offset) {
            *offset += log_file_size_bytes;
        }
        if (*log_processed_offset < modulo_end_offset
            // Handles the case when processed offset, start offset and end offset of snapshot are equal.
            || (*log_processed_offset == modulo_end_offset && log_file_pending_processing_bytes == 0)) {
            *log_processed_offset += log_file_size_bytes;
        }
    }
}
/*
 * This function is used by SnapshotV2 to determine if we need to expedite adding 
 * an item to the snapshot.
 */
int isUnprocessedItemInSnapshotRange(snapshotVersionTwoInfo *snapshot_info, size_t offset) {
    flashcacheLogIterator *snapshot_log_iterator = snapshot_info->snapshot_log_iterator;
    size_t log_file_pending_processing_bytes = snapshot_log_iterator->log_file_pending_processing_bytes;
    size_t log_file_offset_including_pending_bytes =
            *(snapshot_log_iterator->log_processed_offset) + log_file_pending_processing_bytes;
    size_t log_processed_offset = *(snapshot_log_iterator->log_processed_offset);
    size_t log_file_size_bytes = *(snapshot_log_iterator->log_file_size_bytes_ptr);

    normalizeWrappedAroundValues(log_file_offset_including_pending_bytes, log_file_size_bytes,
                                &log_processed_offset, &offset, log_file_pending_processing_bytes);
    flashcacheLogger(FC_LL_DEBUG, "[isItemInUnProcessedSnapshotRange] log_file_offset_including_pending_bytes = %lu,"
                                    " log_processed_offset = %lu,"
                                    " log_file_size_bytes = %lu, offset = %lu",
                                    log_file_offset_including_pending_bytes,
                     log_processed_offset, log_file_size_bytes, offset);
    return (log_processed_offset <= offset) && (log_file_offset_including_pending_bytes > offset);
}

/*
 * Function to check if an item is within the snapshotting scope and already processed 
 * (added to the snapshot) when the range is not wrapped around or wrapped around.
 * 
 * This function is used by forkless save to determine if we need to send a DEL command
 * on this item to Flashcache.
 */
int isProcessedItemInSnapshotRange(snapshotVersionTwoInfo *snapshot_info, size_t offset) {
    size_t snapshot_start_offset = snapshot_info->snapshot_common.log_file_tail_offset;
    flashcacheLogIterator *snapshot_log_iterator = snapshot_info->snapshot_log_iterator;
    size_t log_file_pending_processing_bytes = snapshot_log_iterator->log_file_pending_processing_bytes;
    size_t log_file_offset_including_pending_bytes =
            *(snapshot_log_iterator->log_processed_offset) + log_file_pending_processing_bytes;
    size_t log_processed_offset = *(snapshot_info->snapshot_log_iterator->log_processed_offset);
    size_t log_file_size_bytes = *(snapshot_log_iterator->log_file_size_bytes_ptr);
    if (log_file_offset_including_pending_bytes <= snapshot_start_offset
        && snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes > 0) {
        log_file_offset_including_pending_bytes += log_file_size_bytes;
    }
    normalizeWrappedAroundValues(log_file_offset_including_pending_bytes, log_file_size_bytes,
                                &log_processed_offset, &offset, log_file_pending_processing_bytes);
    flashcacheLogger(FC_LL_DEBUG, "[isItemInProcessedSnapshotRange] log_file_offset_including_pending_bytes = %lu, "
                                  "log_processed_offset = %lu, log_file_size_bytes = %lu, offset = %lu",
                                  log_file_offset_including_pending_bytes, log_processed_offset, log_file_size_bytes,
                                  offset);
    return (snapshot_start_offset <= offset) && (log_processed_offset > offset);
}

/*
 * Functions to check if and item is within the snapshot offset range (from S to E).
 *
 * This function is used by forkless save to determine if we need to propagate
 * a flag add_item_to_rdb on this item back to ASIO.
 */
int isItemInSnapshotRange(snapshotVersionTwoInfo *snapshot_info,
                                 size_t offset) {
    return isProcessedItemInSnapshotRange(snapshot_info, offset) ||
           isUnprocessedItemInSnapshotRange(snapshot_info, offset);
}

// Not static since we use this function in unit test.
int isForklessSaveReplication(snapshotVersionTwoInfo *snapshot_info) {
    return snapshot_info != NULL
           && snapshot_info->snapshot_common.is_running
           && (snapshot_info->snapshot_common.snapshot_writer != NULL
               && snapshot_info->snapshot_save_type == FC_SAVE_TYPE_FORKLESS_SAVE);
}

/*
 * A function to write data (items or metadata) to the snapshot_data_buffer (a staging buffer for snapshots).
 * Items are added to the buffer in chuncks of 1MiB (snapshot_data_buffer_size).
 * An amount of data that doesn't makes the snapshot_data_buffer exceed the limit is copied to buf
 * and added to the snapshot_data_buffer. This function is called recursively with the extra data until all
 * the input is written.
 */
static inline void addDataToSnapshot(snapshotVersionTwoInfo *snapshot_info, char *buf, size_t buf_size) {
    flashcacheAssertWithLogging(!snapshot_info->eof_added, "snapshot bytes getting generated after end of file", 0);
    if (snapshot_info->snapshot_data_buffer_size == 0) {
        flashcacheAssert(snapshot_info->snapshot_data_buffer == NULL);
        snapshot_info->snapshot_data_buffer = createPageAlignedBuffer(FC_SNAPSHOT_DATA_BUFFER_SIZE_BYTES);
        snapshot_info->snapshot_data_buffer_size = FC_SNAPSHOT_DATA_BUFFER_SIZE_BYTES;
        snapshot_info->snapshot_data_buffer_offset = 0;
    }
    flashcacheAssert(snapshot_info->snapshot_data_buffer != NULL);

    size_t writable_bytes = FC_MIN(buf_size,
                                   snapshot_info->snapshot_data_buffer_size -
                                        snapshot_info->snapshot_data_buffer_offset);
    memcpy(snapshot_info->snapshot_data_buffer + snapshot_info->snapshot_data_buffer_offset, buf, writable_bytes);

    buf += writable_bytes;
    buf_size -= writable_bytes;
    snapshot_info->snapshot_data_buffer_offset += writable_bytes;
    snapshot_info->snapshot_data_generated_size_bytes += writable_bytes;


    // If we have fully filled the data buffer and we haven't reached the end of the snapshot file,
    // then add the remaining data to the staging buffer, and create a new data buffer
    bool reached_end_of_snapshot = (snapshot_info->snapshot_data_generated_size_bytes ==
        snapshot_info->snapshot_file_data_size_bytes + snapshot_info->curr_delete_repl_cmd_bytes
        + getEOFItemSizeBytes() - snapshot_info->curr_items_deleted_from_pending_snapshot_range_bytes);
    if (!reached_end_of_snapshot &&
        (snapshot_info->snapshot_data_buffer_offset == snapshot_info->snapshot_data_buffer_size)) {
        stagingBufferAddItem(snapshot_info->snapshot_data_buffer_list, snapshot_info->snapshot_data_buffer,
                             snapshot_info->snapshot_data_buffer_size, 0);
        snapshot_info->snapshot_data_buffer_size = FC_MAX(FC_SNAPSHOT_DATA_BUFFER_SIZE_BYTES,
                                                          getCeilPageAlignedOffset(buf_size));
        snapshot_info->snapshot_data_buffer = createPageAlignedBuffer(snapshot_info->snapshot_data_buffer_size);
        snapshot_info->snapshot_data_buffer_offset = 0;
    }

    // If we have reached the end of the snapshot file, then compute the checksum, and add the remaining
    // data into the staging buffer, and reset the data buffer in snapshot_info
    if (reached_end_of_snapshot) {
        flashcacheAssert(buf_size == 0);
        size_t extra_bytes = getCeilPageAlignedOffset(snapshot_info->snapshot_data_buffer_offset) -
                             snapshot_info->snapshot_data_buffer_offset;
        if (extra_bytes > 0) {
            snapshot_info->checksum = snapshot_info->crc_function(snapshot_info->checksum,
                                                                  (snapshot_info->snapshot_data_buffer +
                                                                    snapshot_info->snapshot_data_buffer_offset),
                                                                  extra_bytes);
        }
        stagingBufferAddItem(snapshot_info->snapshot_data_buffer_list, snapshot_info->snapshot_data_buffer,
                             getCeilPageAlignedOffset(snapshot_info->snapshot_data_buffer_offset), 0);
        snapshot_info->snapshot_data_buffer_size = 0;
        snapshot_info->snapshot_data_buffer = NULL;
        snapshot_info->snapshot_data_buffer_offset = 0;
    }

    if (buf_size > 0) {
        addDataToSnapshot(snapshot_info, buf, buf_size);
    }
}

static void writeSnapshotV2Metadata(snapshotVersionTwoInfo *snapshot_info) {
    uint32_t num_databases = snapshot_info->snapshot_common.num_databases;
    size_t buf_size = getSerializedSnapshotV2MetadataSize(num_databases);
    char *buf = createPageAlignedBuffer(buf_size);

    snapshotV2Metadata snapshot_metadata = { 0 };
    snapshot_metadata.version = FC_SNAPSHOT_VERSION;
    snapshot_metadata.num_databases = num_databases;
    snapshot_metadata.data_section_start_offset = buf_size;
    snapshot_metadata.snapshot_file_data_size_bytes = snapshot_info->snapshot_file_data_size_bytes;
    snapshot_metadata.snapshot_secret = snapshot_info->snapshot_common.snapshot_secret;
    memcpy(buf, &snapshot_metadata, sizeof(snapshotV2Metadata));
    size_t *num_items_per_db = (size_t *) (buf + offsetof(snapshotV2Metadata, num_items_per_db));
    for (size_t i = 0; i < num_databases; ++i) {
        num_items_per_db[i] = snapshot_info->num_items_per_db[i];
    }

    snapshot_info->checksum = snapshot_info->crc_function(snapshot_info->checksum, buf, buf_size);

    flashcacheAssert(buf_size > 0);
    addDataToSnapshot(snapshot_info, buf, buf_size);
    fcFree(buf);
}

// Write log data to the snapshot. Returns 1 to continue writing else 0
static int writeLogDataToSnapshot(snapshotVersionTwoInfo *snapshot_info) {
    stagingBufferEntry *entry = stagingBufferGetTail(snapshot_info->snapshot_data_buffer_list);
    if (!entry) {
        return 0;
    }
    flashcacheAssert(entry->item_len > 0);
    flashcacheSnapshotWriter *snapshot_writer = snapshot_info->snapshot_common.snapshot_writer;
    size_t offset = snapshot_info->snapshot_file_write_offset;
    if (snapshot_writer == NULL) {  // Write log data to the snapshot file in case of file based save
        if (snapshot_info->snapshot_common.num_inflight_snapshot_file_write < FC_SNAPSHOT_QUEUE_DEPTH) {
            fioRequest *fio_request = getFreeFioData(snapshot_info->snapshot_common.fio_requests);
            snapshot_info->snapshot_file_write_offset += flushItemFromStagingBuffer(
                    snapshot_info->snapshot_common.snapshot_file_io_context, fio_request,
                    snapshot_info->snapshot_data_buffer_list,
                    entry, offset);
            snapshot_info->snapshot_common.num_inflight_snapshot_file_write++;
            return 1;
        }
    } else {  // Write log data to the snapshot stream in case of stream based save
        if (snapshot_writer->is_writable(snapshot_writer->callback_context)) {
            fcPseudoFree(entry->item);
            snapshot_writer->write(snapshot_writer->callback_context,
                                   offset, entry->item, entry->item_len);
            snapshot_info->snapshot_file_data_written_bytes += entry->item_len;
            snapshot_info->snapshot_file_write_offset += entry->item_len;
            stagingBufferDeleteEntry(snapshot_info->snapshot_data_buffer_list, entry, offset);
            return 1;
        }
    }
    return 0;
}

static char *generateChecksumFilename(char const *snapshot_filename) {
    int checksum_filename_size = strlen(snapshot_filename) + strlen(FC_CHECKSUM_FILE_EXTENSION) + 1;
    char *checksum_filename = (char *) fcMalloc(checksum_filename_size);
    flashcacheAssertWithLogging(checksum_filename != NULL, "Could not create a checksum filename for"
                                                           "'%s'", snapshot_filename);
    memcpy(checksum_filename, snapshot_filename, strlen(snapshot_filename)+1);
    size_t len = strlen(checksum_filename);
    snprintf(checksum_filename + len, checksum_filename_size - len, "%s", FC_CHECKSUM_FILE_EXTENSION);
    checksum_filename[checksum_filename_size-1] = '\0';
    return checksum_filename;
}

static int verifyChecksumMatch(char const *snapshot_filename, uint32_t calculated_checksum) {
    char *checksum_filename = generateChecksumFilename(snapshot_filename);
    FILE *fp = fopen(checksum_filename, "r");
    if (!fp) goto err;

    uint32_t checksum_from_file;
    if (fscanf(fp, "%u", &checksum_from_file) == EOF) goto err;

    if (fclose(fp) == EOF) goto err;

    fcFree(checksum_filename);
    int checksum_comparison_result = (checksum_from_file == calculated_checksum);
    if (!checksum_comparison_result) {
        flashcacheLogger(FC_LL_NOTICE, "Checksum verification failed.", 0);
    }
    return checksum_comparison_result;

err:
    flashcacheLogger(FC_LL_NOTICE, "Failed to read checksum from '%s', errno:%d %s",
                     checksum_filename, errno, strerror(errno));
    fcFree(checksum_filename);
    return 0;
}

static int saveChecksumToFile(snapshotVersionTwoInfo *snapshot_info) {
    if (snapshot_info->checksum_filename == NULL) {
        return 1;
    }

    FILE *fp = fopen(snapshot_info->checksum_filename, "w");
    if (!fp) goto err;

    // Write checksum to file.
    if (fprintf(fp, "%u\n", snapshot_info->checksum) == EOF) goto err;

    /* Close and flush data to OS */
    if (fclose(fp) == EOF) goto err;

    return 1;

err:
    flashcacheLogger(FC_LL_NOTICE, "Failed to save checksum to '%s', errno:%d %s",
                     snapshot_info->checksum_filename, errno, strerror(errno));
    return 0;
}

static void resetSnapshotInfo(snapshotVersionTwoInfo *snapshot_info) {
    snapshot_info->snapshot_common.is_running = 0;
    snapshot_info->snapshot_common.has_failed = 0;
    snapshot_info->snapshot_log_iterator->log_file_pending_processing_bytes = 0;
    snapshot_info->snapshot_file_data_written_bytes = 0;
    snapshot_info->snapshot_common.log_file_processed_offset = 0;
    snapshot_info->snapshot_common.log_file_processed_bytes_in_current_second = 0;
    snapshot_info->snapshot_log_iterator->log_data_buffer_offset = 0;
    snapshot_info->snapshot_common.log_file_allocated_size_bytes = 0;
    snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes = 0;
    snapshot_info->snapshot_common.num_inflight_snapshot_file_write = 0;
    snapshot_info->snapshot_log_iterator->pending_log_data_processing = 0;
    snapshot_info->snapshot_log_iterator->partial_item_size_bytes = 0;
    snapshot_info->snapshot_file_total_size_bytes = 0;
    snapshot_info->snapshot_file_data_size_bytes = 0;
    snapshot_info->snapshot_data_buffer_offset = 0;
    snapshot_info->snapshot_data_buffer = NULL;
    snapshot_info->snapshot_data_buffer_size = 0;
    snapshot_info->snapshot_data_buffer_offset = 0;
    snapshot_info->snapshot_data_generated_size_bytes = 0;
    snapshot_info->snapshot_file_write_offset = 0;
    snapshot_info->checksum = 0;
    snapshot_info->checksum_filename = NULL;
    snapshot_info->eof_added = 0;
    snapshot_info->snapshot_common.has_snapshotting_completed_in_engine_layer = 0;
    snapshot_info->snapshot_save_type = FC_SAVE_TYPE_BGSAVE;
    snapshot_info->is_waiting_for_engine_snapshotting_completion = 0;
    snapshot_info->curr_num_delete_repl_cmd = 0;
    snapshot_info->curr_delete_repl_cmd_bytes = 0;
    snapshot_info->curr_num_items_deleted_from_pending_snapshot_range = 0;
    snapshot_info->curr_items_deleted_from_pending_snapshot_range_bytes = 0;
    snapshot_info->curr_num_items_with_add_to_rdb_flag = 0;
    snapshot_info->can_do_log_compaction = NULL;
    snapshot_info->latest_keep_alive_msg_time_us = 0;
    snapshot_info->num_replication_link_keep_alive_msg = 0;
}

static void preservePersistedInfo(snapshotVersionTwoInfo *snapshot_info) {
    snapshot_info->persisted_info.last_num_delete_repl_cmd = snapshot_info->curr_num_delete_repl_cmd;
    snapshot_info->persisted_info.last_delete_repl_cmd_bytes = snapshot_info->curr_delete_repl_cmd_bytes;
    snapshot_info->persisted_info.last_num_items_deleted_from_pending_snapshot_range =
        snapshot_info->curr_num_items_deleted_from_pending_snapshot_range;
    snapshot_info->persisted_info.last_items_deleted_from_pending_snapshot_range_bytes =
        snapshot_info->curr_items_deleted_from_pending_snapshot_range_bytes;
    snapshot_info->persisted_info.last_num_items_with_add_to_rdb_flag =
        snapshot_info->curr_num_items_with_add_to_rdb_flag;
}

static void snapshotV2Stop(snapshotVersionTwoInfo *snapshot_info, int completed) {
    if (!snapshot_info->snapshot_common.is_running) {
        return;
    }

    if (completed) {
        flashcacheAssert(snapshot_info->snapshot_data_generated_size_bytes ==
                         snapshot_info->snapshot_file_data_size_bytes +
                         snapshot_info->curr_delete_repl_cmd_bytes + getEOFItemSizeBytes() -
                         snapshot_info->curr_items_deleted_from_pending_snapshot_range_bytes);
        snapshot_info->snapshot_common.log_metrics->num_save_completed++;
    } else {
        snapshot_info->snapshot_common.log_metrics->num_save_cancelled++;
    }

    // Unpause the GC log compaction after forkless save replication is done.
    if (snapshot_info->can_do_log_compaction && *(snapshot_info->can_do_log_compaction) == 0) {
        flashcacheAssert(isForklessSaveReplication(snapshot_info));
        *(snapshot_info->can_do_log_compaction) = 1;
    }

    fioReleaseContext(snapshot_info->snapshot_log_iterator->log_file_io_context);
    snapshot_info->snapshot_log_iterator->log_file_io_context = NULL;
    stagingBufferRelease(snapshot_info->snapshot_data_buffer_list);
    snapshot_info->snapshot_data_buffer_list = NULL;

    if (snapshot_info->snapshot_data_buffer != NULL) {
        fcFree(snapshot_info->snapshot_data_buffer);
    }
    if (!fioRequestIsEmpty(&(snapshot_info->snapshot_log_iterator->log_file_fio_request))) {
        fcFree(fioRequestGetBuffer(&(snapshot_info->snapshot_log_iterator->log_file_fio_request)));
    }
    fioRequestClear(&(snapshot_info->snapshot_log_iterator->log_file_fio_request));
    invokeCompletionCallback(&snapshot_info->snapshot_common, completed);
    for (int i = 0; i < FC_SNAPSHOT_NUM_FIO_DATA; ++i) {
        fioRequestClear(&(snapshot_info->snapshot_common.fio_requests[i]));
    }

    // Free checksum filename if it was ever used
    if (snapshot_info->checksum_filename != NULL) {
        fcFree(snapshot_info->checksum_filename);
    }

    // Preserve persisted info.
    preservePersistedInfo(snapshot_info);

    // Reset snapshot info.
    resetSnapshotInfo(snapshot_info);
}

size_t logIteratorPreProcessingSnapshotCallback(void *context) {
    snapshotVersionTwoInfo *snapshot_info = (snapshotVersionTwoInfo *)context;
    flashcacheLogIterator *log_iterator = snapshot_info->snapshot_log_iterator;

    snapshot_info->number_of_items_in_stagging_buffer =
            stagingBufferGetTotalItemSize(snapshot_info->snapshot_data_buffer_list);

    if (!fioRequestIsEmpty(&(log_iterator->log_file_fio_request)) ||
        (snapshot_info->snapshot_log_iterator->log_file_pending_processing_bytes == 0) ||
        (snapshot_info->number_of_items_in_stagging_buffer >
         snapshot_info->max_snapshot_buffer_size_to_stop_read_bytes)) {
        return 0;
    }
    size_t buffer_size = getCeilPageAlignedOffset(log_iterator->log_file_pending_processing_bytes);

    size_t current_offset = getFloorPageAlignedOffset(snapshot_info->snapshot_common.log_file_processed_offset);
    if (current_offset + buffer_size > *log_iterator->log_file_size_bytes_ptr) {
        buffer_size = *log_iterator->log_file_size_bytes_ptr - current_offset;
    }

    if (buffer_size > FC_SNAPSHOT_MAX_LOG_COPY_BUFFER_SIZE_BYTES) {
        buffer_size = FC_SNAPSHOT_MAX_LOG_COPY_BUFFER_SIZE_BYTES;
    }

    if (log_iterator->partial_item_size_bytes > buffer_size) {
        buffer_size = getCeilPageAlignedOffset(log_iterator->partial_item_size_bytes);
    }

    flashcacheAssert(current_offset + buffer_size <= *log_iterator->log_file_size_bytes_ptr);
    log_iterator->partial_item_size_bytes = 0;
    return buffer_size;
}

void logIteratorCoreLogicProcessingSnapshotCallback(void *snapshot_info_input, void *item_in, void *entry) {
    UNUSED(entry);

    snapshotVersionTwoInfo *snapshot_info = (snapshotVersionTwoInfo *)snapshot_info_input;
    char *item = (char *)item_in;

    size_t total_item_len = extractTotalLenFromSerializedItem(item);
    flashcacheAssert(validateValueInSerializedItem(item, snapshot_info->crc_function));
    snapshot_info->checksum = snapshot_info->crc_function(snapshot_info->checksum,
                                                          item, total_item_len);
    addDataToSnapshot(snapshot_info, item, total_item_len);
}

snapshotVersionTwoInfo *snapshotV2InfoCreate(snapshotInfoCreateParameters snapshot_info_create_params) {
    snapshotVersionTwoInfo *snapshot_info = (snapshotVersionTwoInfo *) fcCalloc(sizeof(snapshotVersionTwoInfo), 1);
    snapshot_info->snapshot_common.num_databases = snapshot_info_create_params.num_databases;
    snapshot_info->max_snapshot_buffer_size_to_stop_read_bytes =
            flashcache_snapshot_config.load_staging_buffer_max_size;
    snapshot_info->snapshot_common.log_metrics = snapshot_info_create_params.log_metrics;
    snapshot_info->snapshot_common.log_file_size_bytes = snapshot_info_create_params.log_file_size_bytes;
    snapshot_info->monotonic_clock_us = snapshot_info_create_params.monotonic_clock_us;
    snapshot_info->crc_function = snapshot_info_create_params.crc_function;
    snapshot_info->snapshot_common.log_filename = snapshot_info_create_params.log_filename;
    snapshot_info->snapshot_common.snapshot_secret.size = 0;
    snapshot_info->num_items_per_db = (size_t *) fcMalloc(sizeof(size_t) * snapshot_info_create_params.num_databases);
    flashcacheAssertWithLogging(snapshot_info->num_items_per_db != NULL,
                                "num_items_per_db is NULL in snapshotV2InfoCreate", 0);
    snapshot_info->snapshot_keep_alive_msg_interval_us =
                                FC_SNAPSHOT_DEFAULT_KEEP_ALIVE_MSG_INTERVAL_US;
    snapshot_info->replication_link_timeout_secs =
                                FC_SNAPSHOT_DEFAULT_MAX_REPLICATION_LINK_SECS;

    snapshot_info->snapshot_log_iterator = logIteratorCreate(snapshot_info_create_params.index,
           snapshot_info_create_params.monotonic_clock_us, snapshot_info_create_params.crc_function,
           FC_SNAPSHOT_MAX_BATCH_PROCESSING_TIME_US, 0, &snapshot_info->snapshot_common.log_file_size_bytes,
           &snapshot_info->snapshot_common.log_file_processed_offset,
           &snapshot_info->snapshot_common.log_file_processed_bytes_in_current_second,
           &snapshot_info->snapshot_common.is_running, NULL, snapshot_info,
           logIteratorPreProcessingSnapshotCallback, logIteratorCoreLogicProcessingSnapshotCallback, NULL);
    fioRequestClear(&(snapshot_info->snapshot_log_iterator->log_file_fio_request));
    for (int i = 0; i < FC_SNAPSHOT_NUM_FIO_DATA; ++i) {
        fioRequestClear(&(snapshot_info->snapshot_common.fio_requests[i]));
    }
    return snapshot_info;
}

// Start snapshotting
void snapshotV2StartSave(snapshotVersionTwoInfo *snapshot_info,
                         flashcacheSnapshotSecret *snapshot_secret,
                         size_t log_file_tail_offset,
                         size_t log_file_active_size_bytes,
                         size_t log_file_allocated_size_bytes,
                         flashcacheSnapshotCallbackDetails *file_based_snapshot_callback_details,
                         char const *snapshot_filename,
                         flashcacheSnapshotWriter *snapshot_writer,
                         int checksum_verification_enabled,
                         flashcacheSnapshotSaveType snapshot_save_type,
                         uint8_t *can_do_log_compaction,
                         flashcacheLogIterationCallbackDetails *log_iteration_completion_callback_details) {
    flashcacheAssert(snapshot_info != NULL);
    flashcacheAssert(!(snapshot_info->snapshot_common.is_running));
    flashcacheAssert(snapshot_secret != NULL);
    flashcacheAssert((snapshot_secret->size > 0) && (snapshot_secret->size <= FC_SNAPSHOT_MAX_SECRET_SIZE));
    flashcacheAssert((snapshot_writer != NULL && file_based_snapshot_callback_details == NULL &&
                      snapshot_filename == NULL) || (snapshot_writer == NULL &&
                                                     file_based_snapshot_callback_details != NULL &&
                                                     snapshot_filename != NULL));
    flashcacheAssert(snapshot_save_type == FC_SAVE_TYPE_BGSAVE || snapshot_save_type == FC_SAVE_TYPE_FORKLESS_SAVE);

    // Reset snapshot info
    resetSnapshotInfo(snapshot_info);

    // Copy the secret to use in the snapshot.
    memcpy(&snapshot_info->snapshot_common.snapshot_secret, snapshot_secret, sizeof(flashcacheSnapshotSecret));

    snapshot_info->snapshot_common.is_running = 1;
    snapshot_info->snapshot_save_type = snapshot_save_type;
    snapshot_info->can_do_log_compaction = can_do_log_compaction;
    if (file_based_snapshot_callback_details != NULL) {
        snapshot_info->snapshot_common.callback_details = *file_based_snapshot_callback_details;
    }
    snapshot_info->snapshot_common.log_file_tail_offset = log_file_tail_offset;
    snapshot_info->snapshot_common.log_file_processed_offset = log_file_tail_offset;
    snapshot_info->snapshot_common.log_file_processed_bytes_in_current_second = 0;
    snapshot_info->snapshot_common.log_file_allocated_size_bytes = log_file_allocated_size_bytes;
    snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes =
            getCeilPageAlignedOffset(log_file_active_size_bytes);
    snapshot_info->snapshot_log_iterator->log_file_pending_processing_bytes = log_file_active_size_bytes;
    snapshot_info->snapshot_common.snapshot_writer = snapshot_writer;
    if (log_iteration_completion_callback_details != NULL) {
        snapshot_info->log_iteration_completion_callback_details = *log_iteration_completion_callback_details;
    }
    snapshot_info->snapshot_log_iterator->log_data_buffer_offset =
            snapshot_info->snapshot_common.log_file_processed_offset % FC_PAGESIZE;
    snapshot_info->snapshot_data_buffer_list = stagingBufferCreate();
    for (uint32_t i = 0; i < snapshot_info->snapshot_common.num_databases; ++i) {
        snapshot_info->num_items_per_db[i] = indexGetNumItems(snapshot_info->snapshot_log_iterator->index[i]);
    }
    size_t serialized_snapshot_metadata_size_bytes = getSerializedSnapshotV2MetadataSize(
            snapshot_info->snapshot_common.num_databases);
    flashcacheAssert(serialized_snapshot_metadata_size_bytes % FC_PAGESIZE == 0);
    snapshot_info->snapshot_file_data_size_bytes = log_file_allocated_size_bytes +
                                                   serialized_snapshot_metadata_size_bytes;
    snapshot_info->snapshot_file_total_size_bytes = getCeilPageAlignedOffset(log_file_allocated_size_bytes) +
                                                    serialized_snapshot_metadata_size_bytes;

    snapshot_info->snapshot_log_iterator->log_file_io_context =
            fioCreateContext(snapshot_info->snapshot_common.log_filename,
                             FILE_READ_WRITE,
                             FC_SNAPSHOT_QUEUE_DEPTH,
                             snapshot_info->monotonic_clock_us);
    flashcacheAssert(snapshot_info->snapshot_log_iterator->log_file_io_context != NULL);

    // Allocates memory for snapshot
    if (snapshotAllocateStorage(&snapshot_info->snapshot_common,
                                snapshot_filename,
                                snapshot_info->monotonic_clock_us,
                                snapshot_info->snapshot_file_total_size_bytes) != 0) {
        snapshot_info->snapshot_common.has_failed = 1;
        return;
    } else if ((snapshot_info->snapshot_common.snapshot_writer == NULL) && checksum_verification_enabled)  {
        // snapshotAllocateStorage call returned 0 AND
        // file based snapshot (snapshot_writer == NULL) AND
        // checksum_verification_enabled is required.

        // Create the checksum filename.
        snapshot_info->checksum_filename = generateChecksumFilename(snapshot_filename);
    }

    flashcacheAssert(snapshot_info->snapshot_file_write_offset == 0);
    writeSnapshotV2Metadata(snapshot_info);

    // Pause GC log compaction in case of forkless save stream based snapshot. It will be
    // resumed after snapshotting completes. We need this to avoid any scenario of
    // item getting moved from snapshotting range which brings lot of complexity.
    if (snapshot_save_type == FC_SAVE_TYPE_FORKLESS_SAVE && snapshot_writer != NULL
           && can_do_log_compaction) {
        *can_do_log_compaction = 0;
    }
}

void snapshotV2CronTask(snapshotVersionTwoInfo *snapshot_info) {
    flashcacheAssert(snapshot_info != NULL);
    if (!(snapshot_info->snapshot_common.is_running)) {
        return;
    }

    if (snapshot_info->snapshot_common.has_failed ||
        (stagingBufferGetTotalItemSize(snapshot_info->snapshot_data_buffer_list) >
                                      FC_SNAPSHOT_MAX_WRITE_BUFFERED_SIZE_TO_STOP_SNAPSHOT_BYTES)) {
        snapshotV2Stop(snapshot_info, 0);
        return;
    }

    // Free the buffers used for writing to the snapshot file
    fioRequest **completed_fio_requests = NULL;
    size_t num_events = snapshot_info->snapshot_common.num_inflight_snapshot_file_write ? fioGetCompletedRequest(
            snapshot_info->snapshot_common.snapshot_file_io_context, &completed_fio_requests) : 0;
    ssize_t err_no = 0;
    for (size_t i = 0; i < num_events; ++i) {
        fioRequest *fio_request = completed_fio_requests[i];
        err_no = (err_no != 0) ? err_no : fio_request->err_no;
        snapshot_info->snapshot_file_data_written_bytes += fioRequestGetBufferSize(fio_request);
        fcFree(fioRequestGetBuffer(fio_request));
        fioRequestClear(fio_request);
        if (err_no != 0) {
            continue;
        }
        flashcacheAssert(snapshot_info->snapshot_common.num_inflight_snapshot_file_write);
        snapshot_info->snapshot_common.num_inflight_snapshot_file_write--;
    }

    if (err_no != 0) {
        snapshotV2Stop(snapshot_info, 0);
        return;
    }

    // Add the data read from log file to the log data buffer list so that it can be written to snapshot file
    num_events = (snapshot_info->snapshot_log_iterator->pending_log_data_processing == 0 && !fioRequestIsEmpty(
            &(snapshot_info->snapshot_log_iterator->log_file_fio_request))) ?
                    fioGetCompletedRequest(snapshot_info->snapshot_log_iterator->log_file_io_context,
                                           &completed_fio_requests) : 0;
    if (num_events > 0) {
        flashcacheAssert(num_events == 1);
        fioRequest *fio_request = completed_fio_requests[0];
        flashcacheAssert(fio_request == &(snapshot_info->snapshot_log_iterator->log_file_fio_request));
        flashcacheAssertHandledCrashWithLogging(fio_request->err_no == 0,
                                    "IO failure with non retryable error [Invalid res: %lld]", fio_request->err_no);
        snapshot_info->snapshot_log_iterator->pending_log_data_processing = 1;
    }

    // Send keepalive message
    flashcacheSnapshotWriter *snapshot_writer = snapshot_info->snapshot_common.snapshot_writer;
    sendKeepAliveMessageToASIOIfRequired(snapshot_info, snapshot_writer);

    // Use iterator
    logIteratorCron(snapshot_info->snapshot_log_iterator);

    // Write log data to the snapshot file in case of file based save
    while (writeLogDataToSnapshot(snapshot_info)) {
        // Busy Writing log data
    }

    // If the metadata and log data has been written, the snapshot might be ready for EOF. Write EOF indicating the
    // end of Snapshot file
    size_t snapshot_file_size_at_completion = snapshot_info->snapshot_file_data_size_bytes +
                                    snapshot_info->curr_delete_repl_cmd_bytes -
                                    snapshot_info->curr_items_deleted_from_pending_snapshot_range_bytes;

    if (!snapshot_info->eof_added && !snapshotV2IsLogReadingInProgress(snapshot_info) &&
        (snapshot_info->snapshot_data_generated_size_bytes == snapshot_file_size_at_completion)) {
        // For stream based snapshot using forkless save when the engine layer has not completed, skip EOF and continue.
        // In this case we will wait and add EOF once the engine layer completes the snapshotting generation.
        if (snapshot_writer != NULL && snapshot_info->snapshot_save_type == FC_SAVE_TYPE_FORKLESS_SAVE &&
            !snapshot_info->snapshot_common.has_snapshotting_completed_in_engine_layer) {
            // Invoke log iteration completion callback
            if (!snapshot_info->is_waiting_for_engine_snapshotting_completion) {
                flashcacheLogIterationCallbackDetails log_iteration_completion_callback_details =
                        snapshot_info->log_iteration_completion_callback_details;
                memset(&(snapshot_info->log_iteration_completion_callback_details), 0,
                       sizeof(flashcacheLogIterationCallbackDetails));
                log_iteration_completion_callback_details.callback(
                        log_iteration_completion_callback_details.context);
                snapshot_info->is_waiting_for_engine_snapshotting_completion = 1;
            }
            return;
        }

        // Add EOF in snapshot after writing log data
        flashcacheLogger(FC_LL_NOTICE, "Adding EOF in FDB file after writing log data", 0);
        size_t serialized_item_len = 0;
        char *serialized_item = NULL;
        serializeKeyValuePairWithFlag(0, NULL, 0, NULL, 0, &serialized_item,
                                      &serialized_item_len, snapshot_info->crc_function, FC_EOF_INDICATOR);
        snapshot_info->checksum = snapshot_info->crc_function(snapshot_info->checksum,
                                                              serialized_item, serialized_item_len);
        addDataToSnapshot(snapshot_info, serialized_item, serialized_item_len);
        snapshot_info->eof_added = 1;
        fcFree(serialized_item);
    }

    // If the metadata, log data and EOF has been written, the snapshot has been taken. Update the state
    // indicating that the snapshot has been taken and invoke the snapshot completion callback
    if (snapshot_info->eof_added && snapshot_info->snapshot_file_data_written_bytes ==
        getCeilPageAlignedOffset(snapshot_file_size_at_completion + getEOFItemSizeBytes())) {
        flashcacheAssert(!snapshot_info->snapshot_common.num_inflight_snapshot_file_write);
        int checksumSaveToFile = saveChecksumToFile(snapshot_info);
        snapshotV2Stop(snapshot_info, checksumSaveToFile);
    }
}

static snapshotV2Metadata *readSnapshotV2Metadata(fioContext *snapshot_file_io_context,
                                                  flashcache_crc_function crc_function,
                                                  uint32_t *checksum,
                                                  char *snapshot_buffer) {
    size_t initial_buf_size = FC_PAGESIZE;

    snapshotV2Metadata initial_metadata = { 0 };
    memcpy(&initial_metadata, snapshot_buffer, sizeof(snapshotV2Metadata));

    size_t buf_size = getSerializedSnapshotV2MetadataSize(initial_metadata.num_databases);
    if (buf_size > initial_buf_size) {
        fcFree(snapshot_buffer);
        snapshot_buffer = readItemBlocking(snapshot_file_io_context, 0, buf_size);
    }

    *checksum = crc_function(*checksum, snapshot_buffer, buf_size);

    size_t metadata_size = getSnapshotV2MetadataSize(initial_metadata.num_databases);
    snapshotV2Metadata *metadata = (snapshotV2Metadata *) fcMalloc(getSnapshotV2MetadataSize(metadata_size));
    flashcacheAssert(metadata != NULL);
    memcpy(metadata, snapshot_buffer, metadata_size);
    fcFree(snapshot_buffer);
    return metadata;
}

// Calculate the number of base size bits for the index.
// Use the base_size_bits unless the number of items requires more bits.
static size_t getBaseSizeBits(size_t expected_num_items, size_t base_size_bits) {
    if (expected_num_items == 0) return base_size_bits;

    size_t new_size_bits = (size_t)getNumBitsRequired(expected_num_items);
    return new_size_bits <= base_size_bits ? base_size_bits : new_size_bits;
}

// Process Replication commands or write key/value pair based on item flag.
static void processItemBasedOnFlag(flashcacheLog *log, char *item, int *eof_reached) {
    flashcacheAssert(validateKeyInSerializedItem(item, log->crc_function));
    flashcacheAssert(validateValueInSerializedItem(item, log->crc_function));
    uint32_t dbid = extractDbidFromSerializedItem(item);
    char *key, *value;
    size_t key_len, value_len;
    extractKeyFromSerializedItem(item, &key, &key_len);
    extractValueFromSerializedItem(item, &value, &value_len);
    uint32_t item_flag = getFlagInSerializedItem(item);

    // Process Delete Replication command
    if (item_flag == FC_REPL_CMD_DELETE) {
        while (logRead(log, dbid, key, key_len, FC_DELETE, NULL, NULL) != FC_OK) {
            logRunCronTasks(log);  // Process the completed request in case read is throttled.
        }
    } else if (item_flag == FC_EOF_INDICATOR) {
        *eof_reached = 1;
    } else {
        // Write Key/value pair
        flashcacheAssertWithLogging(item_flag <= FC_LAST_ITEM_BEFORE_NEXT_PAGE_BOUNDARY,
                                    "Unexpected Item Flag while snapshot loading: %d", item_flag);
        logWrite(log, dbid, key, key_len, value, value_len);
    }
}

void snapshotV2Load(flashcacheLog *log, char const *snapshot_filename,
                    fioContext *snapshot_file_io_context,
                    flashcacheSnapshotSecret *shared_secret,
                    int *checksum_comparison_result,
                    char *snapshot_buffer) {
    log->garbage_collector_info.can_start_garbage_collection = 0;

    uint32_t checksum = 0;
    flashcacheAssert(shared_secret != NULL);
    // Read the snapshot metadata
    snapshotV2Metadata *snapshot_metadata = readSnapshotV2Metadata(snapshot_file_io_context, log->crc_function,
                                                                   &checksum, snapshot_buffer);
    flashcacheAssertWithLogging(snapshot_metadata != NULL, "snapshot_metadata is NULL before loading", 0);
    flashcacheAssertWithLogging(snapshot_metadata->version == FC_SNAPSHOT_VERSION,
                                "Unexpected snapshot version `%d`. It should be `%d`",
                                snapshot_metadata->version,
                                FC_SNAPSHOT_VERSION);
    flashcacheAssertWithLogging(snapshot_metadata->num_databases <= log->num_databases,
                                "Unexpected num_databases `%d`. It should be less than or equal `%d`",
                                snapshot_metadata->num_databases,
                                log->num_databases);
    memcpy(shared_secret, &snapshot_metadata->snapshot_secret, sizeof(flashcacheSnapshotSecret));

    log->num_items = 0;
    log->log_iterator->partial_item_size_bytes = 0;
    log->head_offset = 0;
    log->tail_offset = 0;
    log->allocated_log_size_bytes = 0;
    for (size_t i = 0; i < snapshot_metadata->num_databases; ++i) {
        indexRecreate(log->index_list[i],
                      getBaseSizeBits(snapshot_metadata->num_items_per_db[i], log->index_list[i]->base_size_bits),
                      0, NOT_RUNNING,
                      0, log->hasher.hash_function);
        log->allocated_log_size_bytes_per_db[i] = 0;
    }

    // Reset the indices, to base size, of the databases that are not present in the snapshot.
    for (size_t i = snapshot_metadata->num_databases; i < log->num_databases; ++i) {
        indexRecreate(log->index_list[i], log->index_list[i]->base_size_bits, 0, NOT_RUNNING,
                      0, log->hasher.hash_function);
        log->allocated_log_size_bytes_per_db[i] = 0;
    }

    size_t snapshot_file_processed_offset = snapshot_metadata->data_section_start_offset;
    fioRequest fio_request = {0};
    size_t partial_item_size_bytes = 0;
    // Clear metric before we start current load
    snapshot_metrics.max_load_staging_buffer_size = 0;
    int eof_reached = 0;
    while (!eof_reached) {
        if (fioRequestIsEmpty(&fio_request)) {
            invokeAsioControlMsgCallback();
            // We will take a page size buffer for reading items from snapshot file.
            size_t buf_size = FC_MAX(FC_PAGESIZE, getCeilPageAlignedOffset(partial_item_size_bytes));
            partial_item_size_bytes = 0;

            char *buf = createPageAlignedBuffer(buf_size);
            fioRequestFill(&fio_request, 0, buf, buf_size, getFloorPageAlignedOffset(snapshot_file_processed_offset),
                           FC_FIO_READ);
            fioSubmit(snapshot_file_io_context, &fio_request);
        }

        while (stagingBufferGetTotalItemSize(log->staging_buffer) >
               flashcache_snapshot_config.load_staging_buffer_max_size) {
            logFlushStagingBufferIfRequired(log, log->staging_buffer_flush_size_threshold_bytes);
            logRunCronTasks(log);
        }

        // Get completed request after reading items from Snapshot file
        fioRequest **completed_fio_requests = NULL;
        size_t num_events = fioGetCompletedRequest(snapshot_file_io_context, &completed_fio_requests);
        // If fio request is not completed, continue in order to run the `logRunCronTasks` in the meanwhile.
        // A new fioRequest will not be submitted until the current one is processed.
        if (num_events == 0) continue;

        flashcacheAssert(num_events == 1);
        flashcacheAssert(&fio_request == completed_fio_requests[0]);
        flashcacheAssertHandledCrashWithLogging(fio_request.err_no == 0,
                                    "IO failure with non retryable error [Invalid res: %lld]", fio_request.err_no);

        char *buf = fioRequestGetBuffer(&fio_request);
        size_t buf_size = fioRequestGetBufferSize(&fio_request);
        size_t offset = snapshot_file_processed_offset % FC_PAGESIZE;

        // Process the read buffer item from snapshot file and write it in Log
        while (offset < buf_size && !eof_reached) {
            // Header is partially read so the last cannot be processed. Bailing out.
            if (buf_size - offset < FC_ITEM_HEADER_LEN) {
                partial_item_size_bytes = 2 * FC_PAGESIZE;
                break;
            }

            char *item = buf + offset;
            flashcacheAssert(validateHeaderInSerializedItem(item, log->crc_function));
            size_t total_item_len = extractTotalLenFromSerializedItem(item);
            if (offset + total_item_len > buf_size) {
                // This logic does not currently support streaming over a pipe as it does not
                // store what has already been read in a buffer, but rather sends another fio request
                // to read the same bytes again for partial items.
                partial_item_size_bytes = getPartialItemReadSizeBytes(total_item_len,
                                                                      snapshot_file_processed_offset);
                break;
            }
            checksum = log->crc_function(checksum, item, total_item_len);
            processItemBasedOnFlag(log, item, &eof_reached);

            size_t last_offset = offset;
            offset += total_item_len;
            size_t change_in_offset = (offset - last_offset);
            snapshot_file_processed_offset += change_in_offset;
        }

        if (eof_reached && offset < buf_size) {
            checksum = log->crc_function(checksum, buf + offset, buf_size - offset);
        }

        fcFree(buf);
        fioRequestClear(&fio_request);
        logRunCronTasks(log);
    }

    *checksum_comparison_result = verifyChecksumMatch(snapshot_filename, checksum);

    fcFree(snapshot_metadata);
    fioReleaseContext(snapshot_file_io_context);
    log->garbage_collector_info.can_start_garbage_collection = 1;
}

int snapshotV2IsLogReadingInProgress(snapshotVersionTwoInfo *snapshot_info) {
    return (snapshot_info->snapshot_common.is_running &&
                (snapshot_info->snapshot_log_iterator->log_file_pending_processing_bytes > 0)) ? 1 : 0;
}

void snapshotV2CancelSave(snapshotVersionTwoInfo *snapshot_info) {
    if (!snapshot_info->snapshot_common.is_running) {
        return;
    }
    snapshotV2Stop(snapshot_info, 0);
}

void snapshotV2InfoRelease(snapshotVersionTwoInfo *snapshot_info) {
    snapshotV2Stop(snapshot_info, 0);
    logIteratorRelease(snapshot_info->snapshot_log_iterator);
    fcFree(snapshot_info->num_items_per_db);
    fcFree(snapshot_info);
}

int isUnprocessedItemInActiveSnapshotRange(snapshotVersionTwoInfo *snapshot_info, size_t offset) {
    if (!snapshot_info->snapshot_common.is_running || snapshot_info->snapshot_common.has_failed) return 0;

    return isUnprocessedItemInSnapshotRange(snapshot_info, offset);
}

int snapshotV2ShouldExpediteItem(snapshotVersionTwoInfo *snapshot_info, size_t offset) {
    return isUnprocessedItemInActiveSnapshotRange(snapshot_info, offset) && !isForklessSaveReplication(snapshot_info);
}

// A function to expedite adding an item to the snapshot. If a snapshot is in progress and an item that
// has not been included in the snapshot is currently being processes, this function is called to
// include the item in the snapshot before being deleted from FC.
void snapshotV2AddExpeditedItem(snapshotVersionTwoInfo *snapshot_info, size_t offset, char *item, size_t item_size) {
    if (isUnprocessedItemInActiveSnapshotRange(snapshot_info, offset)) {
        // When we are doing forkless save replication, we will let read request to unprocessed item in
        // snapshot range actually delete the item without expediting them and adding them to the snapshot.
        if (isForklessSaveReplication(snapshot_info)) {
            snapshot_info->curr_num_items_deleted_from_pending_snapshot_range++;
            snapshot_info->curr_items_deleted_from_pending_snapshot_range_bytes += item_size;
        } else {
            snapshot_info->checksum = snapshot_info->crc_function(snapshot_info->checksum, item, item_size);
            addDataToSnapshot(snapshot_info, item, item_size);
        }
    }
}

void snapshotV2AddReplicationCommandIfRequired(snapshotVersionTwoInfo *snapshot_info, size_t offset,
                                               uint32_t dbid, char const *key, size_t key_len, char const *value,
                                               size_t value_len, flashcache_crc_function crc_function) {
    if (!snapshot_info->snapshot_common.is_running || snapshot_info->snapshot_common.has_failed
        || !isForklessSaveReplication(snapshot_info))
        return;

    // If item is already iterated by the log iterator, we will
    // add a DELETE replication command for that key to the FDB snapshot.
    if (isProcessedItemInSnapshotRange(snapshot_info, offset)) {
        char *serialized_item = NULL;
        size_t serialized_item_len = 0;
        serializeKeyValuePairWithFlag(dbid, key, key_len, value, value_len, &serialized_item,
                                      &serialized_item_len, crc_function, FC_REPL_CMD_DELETE);
        snapshot_info->checksum = snapshot_info->crc_function(snapshot_info->checksum,
                                                              serialized_item, serialized_item_len);

        // We need to update `curr_delete_repl_cmd_bytes` metric before calling `addDataToSnapshot` as its updated
        // value is used in `addDataToSnapshot` function to determine the end of forkless save replication.
        snapshot_info->curr_num_delete_repl_cmd++;
        snapshot_info->curr_delete_repl_cmd_bytes += serialized_item_len;
        addDataToSnapshot(snapshot_info, serialized_item, serialized_item_len);
        fcFree(serialized_item);
    }
}

void snapshotV2IncrementNumItemsAddedToRDB(snapshotVersionTwoInfo *snapshot_info) {
        snapshot_info->curr_num_items_with_add_to_rdb_flag++;
}

int snapshotV2IsItemInForklessSaveSnapshotRange(snapshotVersionTwoInfo *snapshot_info, size_t offset) {
    int is_forkless_save_replication = isForklessSaveReplication(snapshot_info);
    int is_item_in_snapshot_range = isItemInSnapshotRange(snapshot_info, offset);
    flashcacheLogger(FC_LL_DEBUG, "Forkless save related info : "
                                  "HasSnapshotFailed = %d, IsForklessSaveReplication = %d, IsItemInSnapshotRange = %d, "
                                  "Offset = %lu", snapshot_info->snapshot_common.has_failed,
                                  is_forkless_save_replication, is_item_in_snapshot_range, offset);
    if (snapshot_info->snapshot_common.has_failed || !is_forkless_save_replication || !is_item_in_snapshot_range) return 0;
    return 1;
}

size_t snapshotV2GetCountBasedMetric(snapshotVersionTwoInfo *snapshot_info, flashcacheCountBasedMetrics metric) {
    size_t ret = 0;
    switch (metric) {
        case FC_IS_WAITING_FOR_ENGINE_SNAPSHOTTING_COMPLETION:
            ret = snapshot_info->is_waiting_for_engine_snapshotting_completion;
            break;
        case FC_CURR_NUM_DELETE_REPL_CMD:
            ret = snapshot_info->curr_num_delete_repl_cmd;
            break;
        case FC_CURR_DELETE_REPL_CMD_BYTES:
            ret = snapshot_info->curr_delete_repl_cmd_bytes;
            break;
        case FC_CURR_NUM_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE:
            ret = snapshot_info->curr_num_items_deleted_from_pending_snapshot_range;
            break;
        case FC_CURR_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE_BYTES:
            ret = snapshot_info->curr_items_deleted_from_pending_snapshot_range_bytes;
            break;
        case FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG:
            ret = snapshot_info->curr_num_items_with_add_to_rdb_flag;
            break;
        case FC_LAST_NUM_DELETE_REPL_CMD:
            ret = snapshot_info->persisted_info.last_num_delete_repl_cmd;
            break;
        case FC_LAST_DELETE_REPL_CMD_BYTES:
            ret = snapshot_info->persisted_info.last_delete_repl_cmd_bytes;
            break;
        case FC_LAST_NUM_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE:
            ret = snapshot_info->persisted_info.last_num_items_deleted_from_pending_snapshot_range;
            break;
        case FC_LAST_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE_BYTES:
            ret = snapshot_info->persisted_info.last_items_deleted_from_pending_snapshot_range_bytes;
            break;
        case FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG:
            ret = snapshot_info->persisted_info.last_num_items_with_add_to_rdb_flag;
            break;
        case FC_LATEST_KEEP_ALIVE_MSG_TIME_US:
            ret = snapshot_info->latest_keep_alive_msg_time_us;
            break;
        default:
            flashcacheAssertWithLogging(0, "Unknown snapshotting metric: [%d]", metric);
    }
    return ret;
}

void snapshotV2UpdateSnapshottingRangeDuringForklessSave(snapshotVersionTwoInfo *snapshot_info,
                                                       size_t updated_log_tail_offset_after_eviction) {
    if (!isForklessSaveReplication(snapshot_info) || snapshot_info->snapshot_common.has_failed) return;
    snapshot_info->snapshot_common.log_file_tail_offset = updated_log_tail_offset_after_eviction;
}

// Only used for unit testing purposes
void addDataToSnapshotForTest(snapshotVersionTwoInfo *snapshot_info, char *buf, size_t buf_size) {
    addDataToSnapshot(snapshot_info, buf, buf_size);
}
