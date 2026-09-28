#ifndef __FLASHCACHE_SNAPSHOT_H
#define __FLASHCACHE_SNAPSHOT_H

#include <stdint.h>

#include "include/util.h"
#include "include/hash.h"
#include "include/index.h"
#include "include/staging_buffer.h"
#include "include/log.h"
#include "include/fio.h"
#include "include/snapshot_common.h"

struct snapshotVersionOneInfo;
struct flashcacheLog;

typedef struct indexSerializationContext {
    // Database identifier of the index being serialized
    uint32_t dbid;

    // Contains the details related to ongoing snapshot
    struct snapshotVersionOneInfo *snapshot_info;
} indexSerializationContext;

typedef struct snapshotVersionOneInfo {
    // A struct holding fields common to all snapshotting versions.
    snapshotCommon snapshot_common;

    // Current db whose index is being serialized
    uint32_t current_dbid;

    // Reference to the indices for which snapshot needs to be taken
    flashcacheIndex **index;

    // Context used for index serialization
    indexSerializationContext *index_serialization_contexts;

    // IO context to read from log file
    fioContext *log_file_io_context;

    // Offset in log file from where snapshot has not been read
    size_t log_file_current_offset;

    // Number of bytes read from the log file
    size_t log_file_data_read_bytes;

    // Size of allocated log file in bytes per db
    size_t *log_file_allocated_size_bytes_per_db;

    // Number of bytes flushed into the snapshot file
    size_t snapshot_file_data_flushed_bytes;

    // Size of index data that needs to be written to snapshot
    size_t index_data_size_bytes;

    // Current write offset in snapshot file for writing index data
    size_t snapshot_file_current_index_data_write_offset;

    // Current write offset in snapshot file for writing log data
    size_t snapshot_file_current_log_data_write_offset;

    // Number of ongoing read from the log file
    size_t num_inflight_log_file_read;

    // Buffer in use for serializing the index
    char *current_serialized_index_buffer;

    // Length of current serialized index buffer
    size_t serialized_index_buffer_len;

    // Index into current serialized_index_buffer from where next item should be written
    size_t current_serialized_index_buffer_pos;

    // Linked list used for storing the serialized index buffer that have not been written to snapshot file
    stagingBuffer *index_data_buffer_list;

    // Linked list used for storing the log data that have not been written to snapshot file
    stagingBuffer *log_data_buffer_list;

    // Start offset of the index data in the current snapshot being created
    size_t snapshot_file_index_data_start_offset;

    // Start offset of the log data in the current snapshot being created
    size_t snapshot_file_log_data_start_offset;

    // Status of current index growth operation.
    indexIteratorStatus current_index_growth_status;

    // Index of next hash bucket to process in growth operation.
    size_t current_index_growth_next_hash_bucket_idx;

    // Database id of current index growth.
    size_t current_index_growth_dbid;

    // Current hasher type used by the hasher in the log
    uint32_t hasher_type;

    // Current hasher seed used by the hasher in the log
    uint8_t hasher_seed[FLASHCACHE_HASHER_SEED_SIZE];

    // Maximum amount of snapshot that can be buffered in memory while waiting for being written to snapshot file.
    size_t max_snapshot_buffer_size_bytes;
} snapshotVersionOneInfo;

// Creates snapshot info that is used for snapshotting
snapshotVersionOneInfo *snapshotInfoCreate(snapshotInfoCreateParameters snapshot_info_create_params);

// Frees the resources used by snapshot info. The caller should not use the provided snapshot info after calling
// this function.
void snapshotInfoRelease(snapshotVersionOneInfo *snapshot_info);

// Start writing snapshot to the specified file or stream
void snapshotStartSave(snapshotVersionOneInfo *snapshot_info,
                       flashcacheSnapshotSecret *snapshot_secret,
                       size_t log_file_tail_offset,
                       size_t active_log_file_size_bytes, size_t log_file_allocated_size_bytes,
                       size_t *log_file_allocated_size_bytes_per_db,
                       flashcache_monotonic_clock_us monotonic_clock_us,
                       flashcacheHasher *hasher,
                       size_t current_index_growth_dbid,
                       indexIteratorStatus current_index_growth_status,
                       size_t current_index_growth_next_hash_bucket_idx,
                       flashcacheSnapshotCallbackDetails *callback_details,
                       char const *snapshot_filename,
                       flashcacheSnapshotWriter *snapshot_writer);

// Snapshotting process is incremental. This task ensure that the next batch of work required for current ongoing
// snapshot save is done. This function returns with no action if there is no snapshot save running at the time of
// invocation.
void snapshotCronTask(snapshotVersionOneInfo *snapshot_info);

// Loads the snapshot into the supplied log instance
void snapshotLoad(struct flashcacheLog *log, fioContext *snapshot_file_io_context,
                  flashcacheSnapshotSecret *secret_response, char *snapshot_buffer);

// Return 1 if there is a snapshot save in progress and it has not yet completed reading the log file else returns 0
int snapshotIsLogReadingInProgress(snapshotVersionOneInfo *snapshot_info);

// Cancels the ongoing snapshot save
void snapshotCancelSave(snapshotVersionOneInfo *snapshot_info);

#endif  // __FLASHCACHE_SNAPSHOT_H
