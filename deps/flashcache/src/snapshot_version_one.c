#include <stdlib.h>
#include "include/snapshot_version_one.h"
#include "include/util.h"

#define FC_SNAPSHOT_VERSION (1)
#define FC_SNAPSHOT_INDEX_ITERATION_BATCH_SIZE (4)
#define FC_SNAPSHOT_SERIALIZED_INDEX_HASH_BUCKET_SIZE_BYTES (24)
#define FC_SNAPSHOT_SERIALIZED_INDEX_ITEM_SIZE_BYTES (8)
#define FC_SNAPSHOT_SERIALIZED_INDEX_BUFFER_LENGTH (1024 * 1024)       // 1 MiB
#define FC_SNAPSHOT_MAX_LOG_COPY_BUFFER_SIZE_BYTES (1024 * 1024)       // 1 MiB
#define FC_DEFAULT_MAX_SNAPSHOT_BUFFER_SIZE_BYTES (100 * 1024 * 1024)  // 100 MiB
#define FC_SNAPSHOT_INDEX_ELEMENT_SIZE_BYTES (8)
#define FC_SNAPSHOT_STAGING_BUFFER_MAXIMUM_SIZE_THRESHOLD \
    ((128 * 1024 * 1024))  // 128 MiB

typedef enum {
    FC_SNAPSHOT_LOG_DATA_TYPE,
    FC_SNAPSHOT_INDEX_DATA_TYPE
} snapshotDataType;

typedef enum {
    // The next item to be read is hash bucket index
    FC_INDEX_LOADER_READ_HASH_BUCKET_IDX,

    // The next item to be read is database identifier
    FC_INDEX_LOADER_READ_DBID,

    // The next item to be read is num of item in the hash bucket
    FC_INDEX_LOADER_READ_ITEM_COUNT,

    // The next item is an item in the hash bucket
    FC_INDEX_LOADER_READ_ITEMS
} indexLoaderState;

// This structure stores the state of the index loader. Index is serialized in the following format:
//
//  +--------+--------+--------+--------+--------+       +--------+--------+
//  | HASH   | DB ID  | ITEM   | ITEM 1 | ITEM 2 |       | ITEM N | HASH   |
//  | BUCKET |        | COUNT  |        |        |  ...  |        | BUCKET | ...
//  | ID     |        |        |        |        |       |        | ID     |
//  +--------+--------+--------+--------+--------+       +--------+--------+
//
//  8 bytes are used for store each component in the above format like hash bucket id, dbid, item count, items.
//  For deserializing we iterate over the serialized bytes, reading 8 bytes at a time. We use a state variable to
//  track what is the next item. We store the hash bucket idx, db id and item_count as we get them. We read item_count
//  number of items and add them item to the index of the current database in the current hash bucket.
typedef struct indexLoaderDetails {
    // The current hash bucket idx that is being deserialized
    size_t hash_bucket_idx;

    // The current database identifier where the hash bucket belongs to
    size_t dbid;

    // The number of items left to read in the current hash bucket
    size_t item_count;

    // The state indicating what is the next item to be read
    indexLoaderState state;
} indexLoaderDetails;

typedef struct perDbMetadata {
    // The size of log file that is allocated per db
    size_t log_file_allocated_size_bytes_per_db;

    // Number of Collision bits used in index growth
    size_t collision_bits_used;

    // Number of bits in base size of index
    size_t index_base_size_bits;
} perDbMetadata;

// The snapshot file contains 3 sections in the following order:
// 1. Metadata section: The metadata of the snapshot is written from offset 0. The metadata contains information used
// for loading the snapshot like the snapshot version, num_databases, size of log data, size of index data etc.
// 2. Index data section: This section contains the serialized index.
// 3. Log data section: This section contains the log data. The log data can have holes.
//
// TODO: Support checksum of the snapshot
typedef struct snapshotMetadatata {
    // Snapshot version
    uint32_t version;

    // Number of databases
    uint32_t num_databases;

    // Offset in log file from where snapshot begins
    size_t log_file_tail_offset;

    // Size of active log file that is written into the snapshot
    size_t active_page_aligned_log_data_size_bytes;

    // The size of log file from which the snapshot is created
    size_t log_file_size_bytes;

    // The size of log file that is allocated
    size_t log_file_allocated_size_bytes;

    // Size of index data that is written into the snapshot
    size_t index_data_size_bytes;

    // Start offset of index data in the snapshot file
    size_t index_data_start_offset;

    // Start offset of log data in the snapshot file
    size_t log_data_start_offset;

    // Index of next hash bucket to process in growth operation.
    size_t current_index_growth_next_hash_bucket_idx;

    // Database id of current index growth.
    size_t current_index_growth_dbid;

    // Status of current index growth operation.
    indexIteratorStatus current_index_growth_status;

    // Hasher type
    uint32_t hasher_type;

    // Hasher seed
    uint8_t hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];

    // the snapshot correlation secret
    flashcacheSnapshotSecret snapshot_secret;

    // Per-db metadata information
    perDbMetadata per_db_metadata[];
} snapshotMetadata;

/*
 * Testing purposes only: we throttle log writes during snapshot loads to fill the staging buffer and make sure
 * it does not overflow during testing.
 * NOTE: we only need to throttle in snapshot V1 as snapshot V2 fills the staging buffer before it starts writes
 * to flash.
 */
typedef struct loadThrottleState {
    int throttle_load_snapshot;
    unsigned int load_throttle_duration;
    unsigned int load_throttle_counter;
} loadThrottleState;

loadThrottleState snapshot_load_write_throttle_state = {0};

/* Data structure to track metrics related to snapshots */
snapshotMetrics snapshot_metrics = {0};

/**
 * Test-only function (kept non-static to use in unit-tests)
 * Sets the system to throttle writes during `throttle_duration` cycles of the snapshot load read/write
 * main loop.
 */
void snapshotThrottleLoadWrite(unsigned int throttle_duration) {
    snapshot_load_write_throttle_state.throttle_load_snapshot = 1;
    snapshot_load_write_throttle_state.load_throttle_duration = throttle_duration;
    snapshot_load_write_throttle_state.load_throttle_counter = throttle_duration;
}

/**
 * Test-only function (kept non-static to use in unit-tests)
 * Disables snapshot load write throttling.
 */
void snapshotUnthrottleLoadWrite() {
    snapshot_load_write_throttle_state.throttle_load_snapshot = 0;
    snapshot_load_write_throttle_state.load_throttle_duration = 0;
    snapshot_load_write_throttle_state.load_throttle_counter = 0;
}

/**
 * Enabled-during-tests-only-function (should not return true in production)
 * Used from the main snapshot load loop to throttle writes and test staging buffer limits.
 */
static int isSnapshotLoadWriteThrottled() {
    if (!snapshot_load_write_throttle_state.throttle_load_snapshot) return 0;
    if (snapshot_load_write_throttle_state.load_throttle_counter == 0) {
        snapshot_load_write_throttle_state.load_throttle_counter =
            snapshot_load_write_throttle_state.load_throttle_duration;
        return 0;
    } else {
        snapshot_load_write_throttle_state.load_throttle_counter--;
        return 1;
    }
}

/**
 * Used to track the maximum load staging buffer size seeing during the last load operation.
 * For the time being this is used for test only but could be exported as a Flahscache metric
 * at a later time.
 */
void updateStagingBufferSizeMetric(size_t size) {
    if (size > snapshot_metrics.max_load_staging_buffer_size) {
        snapshot_metrics.max_load_staging_buffer_size = size;
    }
}

indexEntry *indexAddLogEntry(flashcacheIndex *index, size_t hash_bucket_idx, logEntry *log_entry);

static void writeDataToSerializedIndexBuffer(snapshotVersionOneInfo *snapshot_info, char *data, size_t data_len) {
    size_t data_written = 0;
    while (data_written < data_len) {
        // If the current buffer used for serialization has been completely written we add the buffer a buffer list.
        if (snapshot_info->current_serialized_index_buffer_pos ==
                snapshot_info->serialized_index_buffer_len) {
            stagingBufferAddItem(snapshot_info->index_data_buffer_list,
                    snapshot_info->current_serialized_index_buffer,
                    snapshot_info->serialized_index_buffer_len, 0);

            snapshot_info->current_serialized_index_buffer_pos = 0;
            snapshot_info->current_serialized_index_buffer = NULL;
        }

        // If there no active buffer for index serialization, we create a new one
        if (snapshot_info->current_serialized_index_buffer == NULL) {
            flashcacheAssert(snapshot_info->current_serialized_index_buffer_pos == 0);
            snapshot_info->current_serialized_index_buffer = createPageAlignedBuffer(
                    snapshot_info->serialized_index_buffer_len);
        }

        snapshot_info->current_serialized_index_buffer[snapshot_info->current_serialized_index_buffer_pos++] =
            data[data_written++];
    }
}

static void indexSerializationIterationCallback(void *context, size_t hash_bucket_idx, indexEntry *head_entry) {
    // We exclude serializing the hash bucket that is empty. This helps save space in the snapshot.
    if (!head_entry) {
        return;
    }

    size_t item_count = 0;
    indexEntry *tmp_entry = head_entry;
    while (tmp_entry) {
        tmp_entry = tmp_entry->next;
        item_count++;
    }

    indexSerializationContext *index_serialization_context = (indexSerializationContext *) context;
    snapshotVersionOneInfo *snapshot_info = index_serialization_context->snapshot_info;
    size_t dbid = index_serialization_context->dbid;

    // We write the hash bucket idx, dbid and item_count in binary format according the format specified earlier
    // in this file.
    writeDataToSerializedIndexBuffer(snapshot_info, (char *) &hash_bucket_idx, sizeof(size_t));
    writeDataToSerializedIndexBuffer(snapshot_info, (char *) &dbid, sizeof(size_t));
    writeDataToSerializedIndexBuffer(snapshot_info, (char *) &item_count, sizeof(size_t));

    // We serialize and write all the log entries containing the offset of item in the log to the serialized index
    // buffer
    tmp_entry = head_entry;
    while (tmp_entry) {
        flashcacheAssert(tmp_entry->item_entry.log_entry.on_flash == 1);
        writeDataToSerializedIndexBuffer(snapshot_info, (char *) &(tmp_entry->item_entry.log_entry),
                sizeof(logEntry));
        tmp_entry = tmp_entry->next;
    }
}

static void indexSerializationCompletionCallback(void *context) {
    indexSerializationContext *index_serialization_context = (indexSerializationContext *) context;
    snapshotVersionOneInfo *snapshot_info = index_serialization_context->snapshot_info;

    // This function is called when the index of the current database has been completely serialized. We increment
    // the database id to start serialization of the next database index during its iteration.
    snapshot_info->current_dbid++;

    // If this is the last database, the current serialized buffer would not have been added to the index buffer list.
    // We add the current serialized buffer to the index buffer list so that it can be written to the snapshot file.
    // We avoid adding empty serialized buffer.
    if ((snapshot_info->current_dbid == snapshot_info->snapshot_common.num_databases) &&
            (snapshot_info->current_serialized_index_buffer_pos > 0)) {
        stagingBufferAddItem(snapshot_info->index_data_buffer_list,
                snapshot_info->current_serialized_index_buffer,
                getCeilPageAlignedOffset(snapshot_info->current_serialized_index_buffer_pos), 0);
        snapshot_info->current_serialized_index_buffer_pos = 0;
        snapshot_info->current_serialized_index_buffer = NULL;
    }
}

static void loadIndex(flashcacheIndex **index, indexLoaderDetails *details, char *buf, size_t buf_len,
        size_t source_log_file_page_aligned_tail_offset, size_t source_log_file_size_bytes) {
    flashcacheAssert(buf_len % FC_SNAPSHOT_INDEX_ELEMENT_SIZE_BYTES == 0);

    size_t buf_pos = 0;
    logEntry log_entry = { 0 };
    while (buf_pos < buf_len) {
        switch (details->state) {
            case FC_INDEX_LOADER_READ_HASH_BUCKET_IDX:
                memcpy(&(details->hash_bucket_idx), buf, FC_SNAPSHOT_INDEX_ELEMENT_SIZE_BYTES);
                details->state = FC_INDEX_LOADER_READ_DBID;
                break;
            case FC_INDEX_LOADER_READ_DBID:
                memcpy(&(details->dbid), buf, FC_SNAPSHOT_INDEX_ELEMENT_SIZE_BYTES);
                details->state = FC_INDEX_LOADER_READ_ITEM_COUNT;
                break;
            case FC_INDEX_LOADER_READ_ITEM_COUNT:
                memcpy(&(details->item_count), buf, FC_SNAPSHOT_INDEX_ELEMENT_SIZE_BYTES);
                details->state = FC_INDEX_LOADER_READ_ITEMS;
                break;
            case FC_INDEX_LOADER_READ_ITEMS:
                memcpy(&(log_entry), buf, FC_SNAPSHOT_INDEX_ELEMENT_SIZE_BYTES);

                // We adjust the offset of each item in the log. This is required as when we load the new snapshot
                // we write the log from offset 0 instead of the old tail_offset.
                size_t log_offset = expandTrimmedLogOffset(log_entry.trimmed_log_offset);
                if (log_offset >= source_log_file_page_aligned_tail_offset) {
                    log_offset -= source_log_file_page_aligned_tail_offset;
                } else {
                    log_offset += (source_log_file_size_bytes - source_log_file_page_aligned_tail_offset);
                }
                log_entry.trimmed_log_offset = trimLogOffset(log_offset);

                indexAddLogEntry(index[details->dbid], details->hash_bucket_idx, &log_entry);
                details->item_count--;
                if (!details->item_count) {
                    details->state = FC_INDEX_LOADER_READ_HASH_BUCKET_IDX;
                }
                break;
            default:
                flashcacheAssert(0);
        }
        buf += FC_SNAPSHOT_INDEX_ELEMENT_SIZE_BYTES;
        buf_pos += FC_SNAPSHOT_INDEX_ELEMENT_SIZE_BYTES;
    }
}

static size_t getSnapshotMetadataSize(size_t num_databases) {
    size_t res = (sizeof(snapshotMetadata) + sizeof(perDbMetadata) * num_databases);
    flashcacheAssert(res > 0);
    return res;
}

static size_t getSerializedSnapshotMetadataSize(size_t num_databases) {
    size_t res = getCeilPageAlignedOffset(getSnapshotMetadataSize(num_databases));
    flashcacheAssert(res > 0);
    return res;
}

static void writeSnapshotMetadata(snapshotVersionOneInfo *snapshot_info) {
    uint32_t num_databases = snapshot_info->snapshot_common.num_databases;
    flashcacheSnapshotWriter *snapshot_writer = snapshot_info->snapshot_common.snapshot_writer;
    if ((snapshot_writer != NULL) && !snapshot_writer->is_writable(snapshot_writer->callback_context)) {
        return;
    }
    size_t buf_size = getSerializedSnapshotMetadataSize(num_databases);
    char *buf = createPageAlignedBuffer(buf_size);

    snapshotMetadata snapshot_metadata = { 0 };
    snapshot_metadata.version = FC_SNAPSHOT_VERSION;
    snapshot_metadata.num_databases = num_databases;
    snapshot_metadata.log_file_tail_offset = snapshot_info->snapshot_common.log_file_tail_offset;
    snapshot_metadata.active_page_aligned_log_data_size_bytes =
        snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes;
    snapshot_metadata.index_data_size_bytes = snapshot_info->index_data_size_bytes;
    snapshot_metadata.index_data_start_offset = snapshot_info->snapshot_file_index_data_start_offset;
    snapshot_metadata.log_data_start_offset = snapshot_info->snapshot_file_log_data_start_offset;
    snapshot_metadata.log_file_size_bytes = snapshot_info->snapshot_common.log_file_size_bytes;
    snapshot_metadata.log_file_allocated_size_bytes = snapshot_info->snapshot_common.log_file_allocated_size_bytes;
    snapshot_metadata.current_index_growth_status = snapshot_info->current_index_growth_status;
    snapshot_metadata.current_index_growth_dbid = snapshot_info->current_index_growth_dbid;
    snapshot_metadata.current_index_growth_next_hash_bucket_idx =
            snapshot_info->current_index_growth_next_hash_bucket_idx;
    memcpy(&snapshot_metadata.snapshot_secret, &snapshot_info->snapshot_common.snapshot_secret,
           sizeof(flashcacheSnapshotSecret));
    snapshot_metadata.hasher_type = snapshot_info->hasher_type;
    memcpy(snapshot_metadata.hasher_seed, snapshot_info->hasher_seed, FLASHCACHE_HASHER_SEED_SIZE);
    memcpy(buf, &(snapshot_metadata), sizeof(snapshotMetadata));

    perDbMetadata *per_db_metadata_ptr = (perDbMetadata *)(buf + offsetof(snapshotMetadata, per_db_metadata));
    for (size_t i = 0; i < num_databases; ++i) {
        per_db_metadata_ptr->log_file_allocated_size_bytes_per_db =
            snapshot_info->log_file_allocated_size_bytes_per_db[i];
        per_db_metadata_ptr->collision_bits_used = snapshot_info->index[i]->collision_bits_used;
        per_db_metadata_ptr->index_base_size_bits = snapshot_info->index[i]->base_size_bits;
        per_db_metadata_ptr++;
    }

    flashcacheAssert(buf_size > 0);
    if (snapshot_writer != NULL) {
        fcPseudoFree(buf);
        snapshot_writer->write(snapshot_writer->callback_context, 0, buf, buf_size);
        snapshot_info->snapshot_file_data_flushed_bytes += buf_size;
    } else {
        fioRequest *fio_request = getFreeFioData(snapshot_info->snapshot_common.fio_requests);
        fioRequestFill(fio_request, 0, buf, buf_size, 0, FC_FIO_WRITE);
        fioSubmit(snapshot_info->snapshot_common.snapshot_file_io_context, fio_request);
        snapshot_info->snapshot_common.num_inflight_snapshot_file_write++;
    }
}

static snapshotMetadata *readSnapshotMetadata(fioContext *snapshot_file_io_context,
                                              char *snapshot_buffer) {
    size_t initial_buf_size = FC_PAGESIZE;

    snapshotMetadata initial_metadata = { 0 };
    memcpy(&initial_metadata, snapshot_buffer, sizeof(snapshotMetadata));

    size_t buf_size = getSerializedSnapshotMetadataSize(initial_metadata.num_databases);
    if (buf_size > initial_buf_size) {
        fcFree(snapshot_buffer);
        snapshot_buffer = readItemBlocking(snapshot_file_io_context, 0, buf_size);
    }

    size_t snapshot_metadata_size = getSnapshotMetadataSize(initial_metadata.num_databases);
    snapshotMetadata *metadata = (snapshotMetadata *) fcMalloc(sizeof(char) * buf_size);
    flashcacheAssert(metadata != NULL);
    memcpy(metadata, snapshot_buffer, snapshot_metadata_size);

    fcFree(snapshot_buffer);
    return metadata;
}


static size_t getLogDataReadBytesWithInflightRead(snapshotVersionOneInfo *snapshot_info) {
    flashcacheAssert(snapshot_info->snapshot_file_current_log_data_write_offset >=
            snapshot_info->snapshot_file_log_data_start_offset);
    return (snapshot_info->snapshot_file_current_log_data_write_offset -
        snapshot_info->snapshot_file_log_data_start_offset);
}

static size_t getBufferSizeToCopyLogFile(snapshotVersionOneInfo *snapshot_info) {
    size_t buffer_size = snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes -
        getLogDataReadBytesWithInflightRead(snapshot_info);

    if (snapshot_info->log_file_current_offset + buffer_size > snapshot_info->snapshot_common.log_file_size_bytes) {
        buffer_size = snapshot_info->snapshot_common.log_file_size_bytes - snapshot_info->log_file_current_offset;
    }

    if (buffer_size > FC_SNAPSHOT_MAX_LOG_COPY_BUFFER_SIZE_BYTES) {
        buffer_size = FC_SNAPSHOT_MAX_LOG_COPY_BUFFER_SIZE_BYTES;
    }

    return buffer_size;
}

static size_t getIndexDataSize(size_t index_item_count, size_t index_used_hash_bucket_count) {
    return (FC_SNAPSHOT_SERIALIZED_INDEX_HASH_BUCKET_SIZE_BYTES *
            index_used_hash_bucket_count) + (FC_SNAPSHOT_SERIALIZED_INDEX_ITEM_SIZE_BYTES * index_item_count);
}

static size_t getSnapshotFileSizeWithoutMetadata(size_t active_page_aligned_log_data_size_bytes,
        size_t index_data_size_bytes) {
    return active_page_aligned_log_data_size_bytes + getCeilPageAlignedOffset(index_data_size_bytes);
}

static int readLogFileIfPossible(snapshotVersionOneInfo *snapshot_info) {
    int has_pending_log_data_to_read = (getLogDataReadBytesWithInflightRead(snapshot_info) <
            snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes);
    int is_within_memory_bound = (stagingBufferGetTotalItemSize(snapshot_info->log_data_buffer_list) <
            snapshot_info->max_snapshot_buffer_size_bytes);
    if (!(has_pending_log_data_to_read &&
                is_within_memory_bound &&
                (snapshot_info->num_inflight_log_file_read < FC_SNAPSHOT_QUEUE_DEPTH))) {
        return 0;
    }
    fioRequest *fio_request = getFreeFioData(snapshot_info->snapshot_common.fio_requests);
    size_t buf_size = getBufferSizeToCopyLogFile(snapshot_info);
    char *buf = createPageAlignedBuffer(buf_size);

    fioRequestFill(fio_request, snapshot_info->snapshot_file_current_log_data_write_offset,
            buf, buf_size, snapshot_info->log_file_current_offset, FC_FIO_READ);
    fioSubmit(snapshot_info->log_file_io_context, fio_request);

    snapshot_info->log_file_current_offset = (snapshot_info->log_file_current_offset +
            buf_size) % snapshot_info->snapshot_common.log_file_size_bytes;
    snapshot_info->snapshot_file_current_log_data_write_offset += buf_size;
    snapshot_info->num_inflight_log_file_read++;
    return 1;
}


// Write index data to the snapshot file or Stream. Returns 1 to continue writing else 0
static int writeIndexDataToSnapshot(snapshotVersionOneInfo *snapshot_info) {
    stagingBufferEntry *entry = stagingBufferGetTail(snapshot_info->index_data_buffer_list);
    if (!entry) {
        return 0;
    }
    flashcacheAssert(entry->item_len > 0);
    flashcacheSnapshotWriter *snapshot_writer = snapshot_info->snapshot_common.snapshot_writer;
    if (snapshot_writer == NULL) {  // Write index data to the snapshot file in case of file based save
        if (snapshot_info->snapshot_common.num_inflight_snapshot_file_write < FC_SNAPSHOT_QUEUE_DEPTH) {
            fioRequest *fio_request = getFreeFioData(snapshot_info->snapshot_common.fio_requests);
            size_t flushed_item_size = flushItemFromStagingBuffer(
                    snapshot_info->snapshot_common.snapshot_file_io_context,
                    fio_request, snapshot_info->index_data_buffer_list, entry,
                    snapshot_info->snapshot_file_current_index_data_write_offset);
            snapshot_info->snapshot_file_current_index_data_write_offset += flushed_item_size;
            snapshot_info->snapshot_common.num_inflight_snapshot_file_write++;
            return 1;
        }
    } else {  // Write index data to the snapshot stream in case of stream based save
        if (snapshot_writer->is_writable(snapshot_writer->callback_context)) {
            fcPseudoFree(entry->item);
            snapshot_writer->write(snapshot_writer->callback_context,
                                   snapshot_info->snapshot_file_current_index_data_write_offset,
                                   entry->item, entry->item_len);
            snapshot_info->snapshot_file_current_index_data_write_offset += entry->item_len;
            snapshot_info->snapshot_file_data_flushed_bytes += entry->item_len;
            stagingBufferDeleteEntry(snapshot_info->index_data_buffer_list, entry, 0);
            return 1;
        }
    }
    return 0;
}

// Write log data to the snapshot. Returns 1 to continue writing else 0
static int writeLogDataToSnapshot(snapshotVersionOneInfo *snapshot_info) {
    stagingBufferEntry *entry = stagingBufferGetTail(snapshot_info->log_data_buffer_list);
    if (!entry) {
        return 0;
    }
    flashcacheAssert(entry->item_len > 0);
    flashcacheSnapshotWriter *snapshot_writer = snapshot_info->snapshot_common.snapshot_writer;
    if (snapshot_writer == NULL) {  // Write log data to the snapshot file in case of file based save
        if (snapshot_info->snapshot_common.num_inflight_snapshot_file_write < FC_SNAPSHOT_QUEUE_DEPTH) {
            fioRequest *fio_request = getFreeFioData(snapshot_info->snapshot_common.fio_requests);
            flushItemFromStagingBuffer(snapshot_info->snapshot_common.snapshot_file_io_context,
                                       fio_request, snapshot_info->log_data_buffer_list, entry, entry->user_data);
            snapshot_info->snapshot_common.num_inflight_snapshot_file_write++;
            return 1;
        }
    } else {  // Write log data to the snapshot stream in case of stream based save
        if (snapshot_writer->is_writable(snapshot_writer->callback_context)) {
            fcPseudoFree(entry->item);
            snapshot_writer->write(snapshot_writer->callback_context,
                                   entry->user_data, entry->item, entry->item_len);
            snapshot_info->snapshot_file_data_flushed_bytes += entry->item_len;
            stagingBufferDeleteEntry(snapshot_info->log_data_buffer_list, entry, 0);
            return 1;
        }
    }
    return 0;
}

static void snapshotStop(snapshotVersionOneInfo *snapshot_info, int completed) {
    if (!snapshot_info->snapshot_common.is_running) {
        return;
    }

    if (completed) {
        snapshot_info->snapshot_common.log_metrics->num_save_completed++;
        flashcacheAssert(snapshot_info->current_dbid == snapshot_info->snapshot_common.num_databases);
    } else {
        snapshot_info->snapshot_common.log_metrics->num_save_cancelled++;
    }

    // Release all the point in time iterators of the dbs that have not been iterated completely
    for (uint32_t idx = snapshot_info->current_dbid; idx < snapshot_info->snapshot_common.num_databases; ++idx) {
        indexReleaseCustomIterator(snapshot_info->index[idx]);
    }

    snapshot_info->snapshot_common.is_running = 0;
    snapshot_info->snapshot_common.has_failed = 0;
    snapshot_info->current_dbid = 0;
    snapshot_info->snapshot_common.log_file_tail_offset = 0;
    snapshot_info->log_file_current_offset = 0;
    snapshot_info->snapshot_common.log_file_processed_offset = 0;
    snapshot_info->snapshot_common.log_file_processed_bytes_in_current_second = 0;

    snapshot_info->snapshot_file_data_flushed_bytes = 0;
    snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes = 0;
    snapshot_info->index_data_size_bytes = 0;
    snapshot_info->snapshot_common.snapshot_file_size_bytes = 0;
    snapshot_info->snapshot_common.snapshot_file_size_bytes_without_metadata = 0;
    snapshot_info->snapshot_file_current_log_data_write_offset = 0;
    snapshot_info->log_file_data_read_bytes = 0;
    snapshot_info->snapshot_file_current_index_data_write_offset = 0;
    snapshot_info->snapshot_file_index_data_start_offset = 0;
    snapshot_info->snapshot_file_log_data_start_offset = 0;

    // Unpause the Growth if it is in Paused state and reinitialize snapshot growth related info.
    indexUnpauseGrowth(snapshot_info->index[snapshot_info->current_index_growth_dbid]);
    snapshot_info->current_index_growth_status = NOT_RUNNING;
    snapshot_info->current_index_growth_dbid = 0;
    snapshot_info->current_index_growth_next_hash_bucket_idx = 0;

    snapshot_info->current_serialized_index_buffer_pos = 0;
    if (snapshot_info->current_serialized_index_buffer) {
        fcFree(snapshot_info->current_serialized_index_buffer);
    }

    fioReleaseContext(snapshot_info->log_file_io_context);
    snapshot_info->log_file_io_context = NULL;
    snapshot_info->num_inflight_log_file_read = 0;
    snapshot_info->snapshot_common.num_inflight_snapshot_file_write = 0;

    stagingBufferRelease(snapshot_info->index_data_buffer_list);
    snapshot_info->index_data_buffer_list = NULL;
    stagingBufferRelease(snapshot_info->log_data_buffer_list);
    snapshot_info->log_data_buffer_list = NULL;

    memset(snapshot_info->index_serialization_contexts, 0,
           sizeof(indexSerializationContext) * snapshot_info->snapshot_common.num_databases);
    invokeCompletionCallback(&snapshot_info->snapshot_common, completed);
    for (int i = 0; i < FC_SNAPSHOT_NUM_FIO_DATA; ++i) {
        fioRequestClear(&(snapshot_info->snapshot_common.fio_requests[i]));
    }
}

snapshotVersionOneInfo *snapshotInfoCreate(snapshotInfoCreateParameters snapshot_info_create_params) {
    snapshotVersionOneInfo *snapshot_info = (snapshotVersionOneInfo *) fcCalloc(sizeof(snapshotVersionOneInfo), 1);
    flashcacheAssert(snapshot_info != NULL);

    snapshot_info->snapshot_common.log_file_size_bytes = snapshot_info_create_params.log_file_size_bytes;
    snapshot_info->index = snapshot_info_create_params.index;
    snapshot_info->snapshot_common.num_databases = snapshot_info_create_params.num_databases;
    snapshot_info->serialized_index_buffer_len = FC_SNAPSHOT_SERIALIZED_INDEX_BUFFER_LENGTH;
    snapshot_info->max_snapshot_buffer_size_bytes = FC_DEFAULT_MAX_SNAPSHOT_BUFFER_SIZE_BYTES;
    snapshot_info->snapshot_common.log_metrics = snapshot_info_create_params.log_metrics;

    snapshot_info->log_file_allocated_size_bytes_per_db = (size_t *)
        fcCalloc(sizeof(size_t), snapshot_info_create_params.num_databases);
    flashcacheAssert(snapshot_info->log_file_allocated_size_bytes_per_db != NULL);

    snapshot_info->snapshot_common.log_filename = snapshot_info_create_params.log_filename;

    snapshot_info->index_serialization_contexts = (indexSerializationContext *) fcCalloc(
            sizeof(indexSerializationContext), snapshot_info_create_params.num_databases);
    flashcacheAssert(snapshot_info->index_serialization_contexts != NULL);

    for (int i = 0; i < FC_SNAPSHOT_NUM_FIO_DATA; ++i) {
        fioRequestClear(&(snapshot_info->snapshot_common.fio_requests[i]));
    }
    return snapshot_info;
}

// Start snapshotting
void snapshotStartSave(snapshotVersionOneInfo *snapshot_info,
                       flashcacheSnapshotSecret *snapshot_secret,
                       size_t log_file_tail_offset,
                       size_t active_log_file_size_bytes,
                       size_t log_file_allocated_size_bytes,
                       size_t *log_file_allocated_size_bytes_per_db,
                       flashcache_monotonic_clock_us monotonic_clock_us,
                       flashcacheHasher *hasher,
                       size_t current_index_growth_dbid,
                       indexIteratorStatus current_index_growth_status,
                       size_t current_index_growth_next_hash_bucket_idx,
                       flashcacheSnapshotCallbackDetails *file_based_snapshot_callback_details,
                       char const *snapshot_filename,
                       flashcacheSnapshotWriter *snapshot_writer) {
    flashcacheAssert(snapshot_info != NULL);
    flashcacheAssert(!(snapshot_info->snapshot_common.is_running));
    flashcacheAssert(snapshot_secret != NULL);
    flashcacheAssert(hasher != NULL);
    flashcacheAssert((snapshot_secret->size > 0) && (snapshot_secret->size <= FC_SNAPSHOT_MAX_SECRET_SIZE));
    flashcacheAssert((snapshot_writer != NULL && file_based_snapshot_callback_details == NULL &&
        snapshot_filename == NULL) || (snapshot_writer == NULL && file_based_snapshot_callback_details != NULL &&
        snapshot_filename != NULL));
    memcpy(&snapshot_info->snapshot_common.snapshot_secret, snapshot_secret, sizeof(flashcacheSnapshotSecret));

    size_t total_num_used_hash_buckets = 0;
    size_t total_num_items = 0;
    indexIteratorCallbackDetails index_serialization_callback_details = { 0 };
    index_serialization_callback_details.iteration_callback = indexSerializationIterationCallback;
    index_serialization_callback_details.completion_callback = indexSerializationCompletionCallback;
    for (uint32_t idx = 0; idx < snapshot_info->snapshot_common.num_databases; ++idx) {
        indexSerializationContext *index_serialization_context =
            &(snapshot_info->index_serialization_contexts[idx]);
        index_serialization_context->snapshot_info = snapshot_info;
        index_serialization_context->dbid = idx;
        index_serialization_callback_details.context = (void *) index_serialization_context;

        flashcacheIndex *current_index = snapshot_info->index[idx];
        indexCreateCustomIterator(current_index, &(index_serialization_callback_details),
                                  FC_SNAPSHOT_INDEX_ITERATION_BATCH_SIZE);
        total_num_items += current_index->num_items;
        total_num_used_hash_buckets += current_index->num_hash_bucket_used;

        snapshot_info->log_file_allocated_size_bytes_per_db[idx] =
            log_file_allocated_size_bytes_per_db[idx];
    }

    snapshot_info->snapshot_common.is_running = 1;
    snapshot_info->snapshot_common.has_failed = 0;
    snapshot_info->hasher_type = hasher->get_type();
    hasher->get_seed(snapshot_info->hasher_seed);
    snapshot_info->current_dbid = 0;
    snapshot_info->snapshot_file_data_flushed_bytes = 0;
    snapshot_info->log_file_data_read_bytes = 0;
    snapshot_info->snapshot_common.log_file_processed_bytes_in_current_second = 0;
    snapshot_info->snapshot_common.log_file_tail_offset = log_file_tail_offset;
    snapshot_info->log_file_current_offset = getFloorPageAlignedOffset(log_file_tail_offset);
    snapshot_info->snapshot_common.log_file_processed_offset = snapshot_info->log_file_current_offset;
    snapshot_info->snapshot_common.log_file_allocated_size_bytes = log_file_allocated_size_bytes;
    snapshot_info->num_inflight_log_file_read = 0;
    snapshot_info->snapshot_common.num_inflight_snapshot_file_write = 0;
    snapshot_info->current_index_growth_dbid = current_index_growth_dbid;
    snapshot_info->current_index_growth_status = current_index_growth_status;
    snapshot_info->current_index_growth_next_hash_bucket_idx = current_index_growth_next_hash_bucket_idx;
    if (file_based_snapshot_callback_details != NULL) {
        snapshot_info->snapshot_common.callback_details = (*file_based_snapshot_callback_details);
    }
    snapshot_info->snapshot_common.snapshot_writer = snapshot_writer;

    snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes =
            getCeilPageAlignedOffset(active_log_file_size_bytes);
    // If Allocated size bytes is 0, then avoid replicating the garbage data.
    if (log_file_allocated_size_bytes == 0) {
        snapshot_info->snapshot_common.log_file_tail_offset = 0;
        snapshot_info->log_file_current_offset = 0;
        snapshot_info->snapshot_common.log_file_processed_offset = 0;
        snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes = 0;
    }
    snapshot_info->index_data_size_bytes = getIndexDataSize(total_num_items, total_num_used_hash_buckets);

    size_t serialized_snapshot_metadata_size_bytes =
            getSerializedSnapshotMetadataSize(snapshot_info->snapshot_common.num_databases);
    snapshot_info->snapshot_common.snapshot_file_size_bytes_without_metadata = getSnapshotFileSizeWithoutMetadata(
            snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes,
            snapshot_info->index_data_size_bytes);
    snapshot_info->snapshot_common.snapshot_file_size_bytes =
            snapshot_info->snapshot_common.snapshot_file_size_bytes_without_metadata +
            serialized_snapshot_metadata_size_bytes;

    snapshot_info->snapshot_file_current_index_data_write_offset = serialized_snapshot_metadata_size_bytes;
    snapshot_info->snapshot_file_current_log_data_write_offset =
        snapshot_info->snapshot_file_current_index_data_write_offset +
        getCeilPageAlignedOffset(snapshot_info->index_data_size_bytes);

    snapshot_info->snapshot_file_index_data_start_offset =
        snapshot_info->snapshot_file_current_index_data_write_offset;
    snapshot_info->snapshot_file_log_data_start_offset =
        snapshot_info->snapshot_file_current_log_data_write_offset;

    snapshot_info->current_serialized_index_buffer_pos = 0;
    snapshot_info->current_serialized_index_buffer = NULL;

    snapshot_info->index_data_buffer_list = stagingBufferCreate();
    snapshot_info->log_data_buffer_list = stagingBufferCreate();

    snapshot_info->log_file_io_context =
            fioCreateContext(snapshot_info->snapshot_common.log_filename, FILE_READ_WRITE,
                             FC_SNAPSHOT_QUEUE_DEPTH, monotonic_clock_us);
    flashcacheAssert(snapshot_info->log_file_io_context != NULL);

    // Allocates memory for snapshot
    if (snapshotAllocateStorage(&snapshot_info->snapshot_common,
                                snapshot_filename,
                                monotonic_clock_us,
                                snapshot_info->snapshot_common.snapshot_file_size_bytes) != 0) {
        snapshot_info->snapshot_common.has_failed = 1;
    }
}

void snapshotCronTask(snapshotVersionOneInfo *snapshot_info) {
    if (!(snapshot_info->snapshot_common.is_running)) {
        return;
    }

    if (snapshot_info->snapshot_common.has_failed) {
        snapshotStop(snapshot_info, 0);
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
        snapshot_info->snapshot_file_data_flushed_bytes += fioRequestGetBufferSize(fio_request);
        fcFree(fioRequestGetBuffer(fio_request));
        fioRequestClear(fio_request);
        if (err_no != 0) {
            continue;
        }
        flashcacheAssert(snapshot_info->snapshot_common.num_inflight_snapshot_file_write);
        snapshot_info->snapshot_common.num_inflight_snapshot_file_write--;
    }

    if (err_no != 0) {
        snapshotStop(snapshot_info, 0);
        return;
    }

    // Add the data read from log file to the log data buffer list so that it can be written to snapshot file
    num_events = snapshot_info->num_inflight_log_file_read ? fioGetCompletedRequest(
            snapshot_info->log_file_io_context, &completed_fio_requests) : 0;
    for (size_t i = 0; i < num_events; ++i) {
        fioRequest *fio_request = completed_fio_requests[i];
        flashcacheAssertHandledCrashWithLogging(fio_request->err_no == 0,
                "IO failure with non retryable error [Invalid res: %lld]", fio_request->err_no);
        size_t buf_size = fioRequestGetBufferSize(fio_request);
        stagingBufferAddItem(snapshot_info->log_data_buffer_list, fioRequestGetBuffer(fio_request),
                buf_size, fio_request->user_data);

        // Set the log file processed offset to log file current offset. As there can be only a single read request to
        // read the log file at any point in time, the current offset is the offset upto which the data has been copied
        snapshot_info->snapshot_common.log_file_processed_offset = snapshot_info->log_file_current_offset;
        snapshot_info->log_file_data_read_bytes += buf_size;
        fioRequestClear(fio_request);
        flashcacheAssert(snapshot_info->num_inflight_log_file_read);
        snapshot_info->num_inflight_log_file_read--;
    }

    // If the current index data that is waiting to be written to snapshot is less than the max data buffer list size
    // and if the index has not been completely serialized, iterate over the index to serialize it.
    if (snapshot_info->current_dbid < snapshot_info->snapshot_common.num_databases && stagingBufferGetTotalItemSize(
                snapshot_info->index_data_buffer_list) < snapshot_info->max_snapshot_buffer_size_bytes) {
        indexIterateCustomIteratorIfRequired(snapshot_info->index[snapshot_info->current_dbid]);
    }

    // Read data from the log file asynchonously
    while (readLogFileIfPossible(snapshot_info)) {
        // Busy reading log file
    }

    // Write index data to the snapshot file in case of file based save
    while (writeIndexDataToSnapshot(snapshot_info)) {
        // Busy Writing index data
    }

    // Write log data to the snapshot file in case of file based save
    while (writeLogDataToSnapshot(snapshot_info)) {
        // Busy Writing log data
    }

    // If the index data and log data has been written, write the snapshot metadata to the snapshot file or stream
    if ((snapshot_info->snapshot_file_data_flushed_bytes ==
                snapshot_info->snapshot_common.snapshot_file_size_bytes_without_metadata) &&
            (snapshot_info->snapshot_common.num_inflight_snapshot_file_write != 1) &&
            (snapshot_info->current_dbid == snapshot_info->snapshot_common.num_databases)) {
        flashcacheAssert(!snapshot_info->num_inflight_log_file_read);
        flashcacheAssert(!snapshot_info->snapshot_common.num_inflight_snapshot_file_write);

        writeSnapshotMetadata(snapshot_info);
    }

    // If the metadata, index data and log data has been written, the snapshot has been taken. Update the state
    // indicating that the snapshot has been taken and invoke the snapshot completion callback
    if (snapshot_info->snapshot_file_data_flushed_bytes == snapshot_info->snapshot_common.snapshot_file_size_bytes) {
        flashcacheAssert(!snapshot_info->num_inflight_log_file_read);
        flashcacheAssert(!snapshot_info->snapshot_common.num_inflight_snapshot_file_write);

        snapshotStop(snapshot_info, 1);
    }
}

void snapshotLoad(flashcacheLog *log, fioContext *snapshot_file_io_context, flashcacheSnapshotSecret *shared_secret,
                  char *snapshot_buffer) {
    fioContext *log_file_io_context = log->fio_context;

    // Read the snapshot metadata
    snapshotMetadata *snapshot_metadata = readSnapshotMetadata(snapshot_file_io_context, snapshot_buffer);

    flashcacheAssert(snapshot_metadata->version == FC_SNAPSHOT_VERSION);
    flashcacheAssert(snapshot_metadata->num_databases <= log->num_databases);

    // Prepare rdb/fdb correlation secret to be returned to the engine
    flashcacheAssert(snapshot_metadata != NULL);
    flashcacheAssert(shared_secret != NULL);
    memcpy(shared_secret, &snapshot_metadata->snapshot_secret, sizeof(flashcacheSnapshotSecret));
    log->num_index_growth_run = 0;  // Reinitalize number of times index growth operation has run metric

    // Initialize the hasher
    log->hasher = *(flashcacheHasherGetByType(snapshot_metadata->hasher_type));
    log->hasher.init(snapshot_metadata->hasher_seed);

    for (size_t i = 0; i < snapshot_metadata->num_databases; ++i) {
        indexIteratorStatus growth_status = NOT_RUNNING;
        size_t next_hash_bucket = 0;

        // Update the growth_status and next_hash_bucket if we are recreating the index
        // which was under growth operation at the time of snapshot.
        if (i == snapshot_metadata->current_index_growth_dbid) {
            growth_status = snapshot_metadata->current_index_growth_status;
            next_hash_bucket = snapshot_metadata->current_index_growth_next_hash_bucket_idx;
        }
        indexRecreate(log->index_list[i], snapshot_metadata->per_db_metadata[i].index_base_size_bits,
                      snapshot_metadata->per_db_metadata[i].collision_bits_used,
                      growth_status, next_hash_bucket, log->hasher.hash_function);
        log->allocated_log_size_bytes_per_db[i] = 0;
    }

    // Reset the indices, to base size, of the databases that are not present in the snapshot.
    for (size_t i = snapshot_metadata->num_databases; i < log->num_databases; ++i) {
        indexRecreate(log->index_list[i], log->index_list[i]->base_size_bits, 0, NOT_RUNNING,
                0, log->hasher.hash_function);
        log->allocated_log_size_bytes_per_db[i] = 0;
    }

    // Update the current index growth dbid in log.
    log->current_index_growth_dbid = snapshot_metadata->current_index_growth_dbid;

    size_t num_bytes_read = 0;
    size_t num_bytes_processed = 0;

    size_t source_log_file_tail_offset = snapshot_metadata->log_file_tail_offset;
    size_t source_log_file_page_aligned_tail_offset = getFloorPageAlignedOffset(source_log_file_tail_offset);
    size_t source_log_file_size_bytes = snapshot_metadata->log_file_size_bytes;
    size_t log_data_size_bytes = snapshot_metadata->active_page_aligned_log_data_size_bytes;
    flashcacheAssert(log_data_size_bytes % FC_PAGESIZE == 0);
    flashcacheAssert(log_data_size_bytes <= log->log_size_bytes);

    size_t index_data_size_bytes = snapshot_metadata->index_data_size_bytes;
    size_t total_num_bytes_to_process = (log_data_size_bytes + index_data_size_bytes);
    size_t num_read_requests_inflight = 0;
    size_t num_write_requests_inflight = 0;
    int index_read_in_progress = 0;

    size_t num_log_data_bytes_read = 0;
    size_t num_index_data_bytes_read = 0;
    size_t num_index_data_bytes_processed = 0;
    size_t snapshot_file_index_data_offset = snapshot_metadata->index_data_start_offset;
    size_t snapshot_file_log_data_start_offset = snapshot_metadata->log_data_start_offset;
    size_t snapshot_file_log_data_current_offset = snapshot_file_log_data_start_offset;

    fioRequest snapshot_read_fio_requests[FC_SNAPSHOT_LOADING_QUEUE_DEPTH];
    for (size_t i = 0; i < FC_SNAPSHOT_LOADING_QUEUE_DEPTH; ++i) {
        fioRequestClear(&snapshot_read_fio_requests[i]);
    }

    fioRequest log_write_fio_requests[FC_SNAPSHOT_LOADING_QUEUE_DEPTH];
    for (size_t i = 0; i < FC_SNAPSHOT_LOADING_QUEUE_DEPTH; ++i) {
        fioRequestClear(&log_write_fio_requests[i]);
    }

    stagingBuffer *log_data_buffer_list = stagingBufferCreate();
    stagingBuffer *index_data_buffer_list = stagingBufferCreate();

    indexLoaderDetails index_loader_details = { 0 };
    index_loader_details.state = FC_INDEX_LOADER_READ_HASH_BUCKET_IDX;

    // Clear metric before we start current load
    snapshot_metrics.max_load_staging_buffer_size = 0;
    while (num_bytes_processed < total_num_bytes_to_process) {
        invokeAsioControlMsgCallback();
        // Once an block is read from snapshot file, the block is added to the index data buffer list or log data
        // buffer list for further processing
        fioRequest **completed_fio_requests = NULL;
        size_t num_events = fioGetCompletedRequest(snapshot_file_io_context, &completed_fio_requests);
        for (size_t i = 0; i < num_events; ++i) {
            fioRequest *fio_request = completed_fio_requests[i];
            flashcacheAssertHandledCrashWithLogging(fio_request->err_no == 0,
                    "IO failure with non retryable error [Invalid res: %lld]", fio_request->err_no);
            num_bytes_read += fioRequestGetBufferSize(fio_request);

            if (fio_request->user_data == FC_SNAPSHOT_INDEX_DATA_TYPE) {
                stagingBufferAddItem(index_data_buffer_list, fioRequestGetBuffer(fio_request),
                        fioRequestGetBufferSize(fio_request), 0);
                index_read_in_progress = 0;
            } else if (fio_request->user_data == FC_SNAPSHOT_LOG_DATA_TYPE) {
                stagingBufferAddItem(log_data_buffer_list, fioRequestGetBuffer(fio_request),
                        fioRequestGetBufferSize(fio_request), fio_request->offset);
                updateStagingBufferSizeMetric(log_data_buffer_list->total_item_size);
            } else {
                flashcacheAssert(0);
            }

            fioRequestClear(fio_request);
            num_read_requests_inflight--;
        }

        // Read index data from the snapshot file
        if (num_read_requests_inflight < FC_SNAPSHOT_LOADING_QUEUE_DEPTH &&
                num_index_data_bytes_read < index_data_size_bytes &&
                (!index_read_in_progress)) {
            fioRequest *fio_request = getFreeFioData(snapshot_read_fio_requests);
            size_t buf_size = FC_MIN(FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES,
                    getCeilPageAlignedOffset(index_data_size_bytes - num_index_data_bytes_read));
            char *buf = createPageAlignedBuffer(buf_size);

            fioRequestFill(fio_request, FC_SNAPSHOT_INDEX_DATA_TYPE, buf, buf_size, snapshot_file_index_data_offset,
                    FC_FIO_READ);
            fioSubmit(snapshot_file_io_context, fio_request);
            snapshot_file_index_data_offset += buf_size;
            num_index_data_bytes_read += buf_size;
            num_read_requests_inflight++;
            index_read_in_progress = 1;
        }

        // Read log data from the snapshot file
        if (num_read_requests_inflight < flashcache_snapshot_config.load_queue_depth &&
            num_log_data_bytes_read < log_data_size_bytes
            && (log_data_buffer_list->total_item_size <
                flashcache_snapshot_config.load_staging_buffer_max_size)) {
            fioRequest *fio_request = getFreeFioData(snapshot_read_fio_requests);
            size_t buf_size = FC_MIN(FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES,
                    getCeilPageAlignedOffset(log_data_size_bytes - num_log_data_bytes_read));
            char *buf = createPageAlignedBuffer(buf_size);

            fioRequestFill(fio_request, FC_SNAPSHOT_LOG_DATA_TYPE, buf, buf_size, snapshot_file_log_data_current_offset,
                    FC_FIO_READ);
            fioSubmit(snapshot_file_io_context, fio_request);
            snapshot_file_log_data_current_offset += buf_size;
            num_log_data_bytes_read += buf_size;
            num_read_requests_inflight++;
        }

        // Free the buffer used for writing the log data
        completed_fio_requests = NULL;
        num_events = fioGetCompletedRequest(log_file_io_context, &completed_fio_requests);
        for (size_t i = 0; i < num_events; ++i) {
            fioRequest *fio_request = completed_fio_requests[i];
            flashcacheAssertHandledCrashWithLogging(fio_request->err_no == 0,
                    "IO failure with non retryable error [Invalid res: %lld]", fio_request->err_no);
            num_bytes_processed += fioRequestGetBufferSize(fio_request);
            fcFree(fioRequestGetBuffer(fio_request));
            fioRequestClear(fio_request);
            num_write_requests_inflight--;
        }
        // Write data copied from snapshot file into the log file
        stagingBufferEntry* entry = NULL;
        while ((entry = stagingBufferGetTail(log_data_buffer_list)) != NULL) {
            if ((num_write_requests_inflight < FC_SNAPSHOT_LOADING_QUEUE_DEPTH) && !isSnapshotLoadWriteThrottled()) {
                fioRequest *fio_request = getFreeFioData(log_write_fio_requests);
                size_t offset = entry->user_data - snapshot_file_log_data_start_offset;
                flushItemFromStagingBuffer(log_file_io_context,
                                           fio_request, log_data_buffer_list, entry, offset);
                num_write_requests_inflight++;
            } else {
                /* If there are too many write requests in flight, wait for them to flush
                 * to Flash before we queue more of them.
                 */
                break;
            }
        }

        // Load index incrementally from the index data read from snapshot file
        entry = stagingBufferGetTail(index_data_buffer_list);
        if (entry) {
            char *buf = entry->item;
            size_t buf_len = FC_MIN(entry->item_len, (index_data_size_bytes - num_index_data_bytes_processed));

            loadIndex(log->index_list, &index_loader_details, buf, buf_len, source_log_file_page_aligned_tail_offset,
                   source_log_file_size_bytes);
            num_index_data_bytes_processed += buf_len;
            num_bytes_processed += entry->item_len;
            stagingBufferDeleteEntry(index_data_buffer_list, entry, 1);
        }
    }

    log->num_items = 0;
    size_t allocated_log_size_bytes = 0;
    for (size_t i = 0; i < snapshot_metadata->num_databases; ++i) {
        log->num_items += log->index_list[i]->num_items;
        log->allocated_log_size_bytes_per_db[i] =
            snapshot_metadata->per_db_metadata[i].log_file_allocated_size_bytes_per_db;
        allocated_log_size_bytes += log->allocated_log_size_bytes_per_db[i];
    }
    flashcacheAssert(allocated_log_size_bytes == snapshot_metadata->log_file_allocated_size_bytes);

    log->head_offset = log_data_size_bytes;
    log->tail_offset = source_log_file_tail_offset - source_log_file_page_aligned_tail_offset;
    log->allocated_log_size_bytes = allocated_log_size_bytes;
    log->log_iterator->partial_item_size_bytes = 0;
    fcFree(snapshot_metadata);
    fioReleaseContext(snapshot_file_io_context);
    stagingBufferRelease(log_data_buffer_list);
    stagingBufferRelease(index_data_buffer_list);
}

int snapshotIsLogReadingInProgress(snapshotVersionOneInfo *snapshot_info) {
    return (snapshot_info->snapshot_common.is_running && (snapshot_info->log_file_data_read_bytes <
                snapshot_info->snapshot_common.active_page_aligned_log_data_size_bytes)) ? 1 : 0;
}

void snapshotCancelSave(snapshotVersionOneInfo *snapshot_info) {
    if (!snapshot_info->snapshot_common.is_running) {
        return;
    }
    snapshotStop(snapshot_info, 0);
}

void snapshotInfoRelease(snapshotVersionOneInfo *snapshot_info) {
    snapshotStop(snapshot_info, 0);
    fcFree(snapshot_info->index_serialization_contexts);
    fcFree(snapshot_info->log_file_allocated_size_bytes_per_db);
    fcFree(snapshot_info);
}
