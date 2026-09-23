#ifndef __FLASHCACHE_SNAPSHOTV2_H
#define __FLASHCACHE_SNAPSHOTV2_H

#include "include/hash.h"
#include "include/log.h"
#include "include/snapshot_common.h"

struct snapshotVersionTwoInfo;
struct persistedInfo;
struct flashcacheLog;

typedef struct persistedInfo {
    // Number of DELETE replication command in the last snapshot.
    size_t last_num_delete_repl_cmd;

    // Total size of replication data by DELETE in the last snapshot.
    size_t last_delete_repl_cmd_bytes;

    // Number of items reads in unprocessed part in the last snapshot.
    size_t last_num_items_deleted_from_pending_snapshot_range;

    // Bytes read in unprocessed part in the last snapshot.
    size_t last_items_deleted_from_pending_snapshot_range_bytes;

    // Number of items added to rdb in the last snapshot.
    size_t last_num_items_with_add_to_rdb_flag;
} persistedInfo;

typedef struct snapshotVersionTwoInfo {
    // A struct holding fields common to all snapshotting versions.
    snapshotCommon snapshot_common;

    // Size of snapshot file in bytes
    size_t snapshot_file_data_size_bytes;

    // Size of snapshot file in bytes with snapshot metadata
    size_t snapshot_file_total_size_bytes;

    // Number of bytes written into the snapshot file
    size_t snapshot_file_data_written_bytes;

    // Currently generated snapshot data size (in bytes)
    size_t snapshot_data_generated_size_bytes;

    // Maximum amount of snapshot that can be buffered in memory while waiting for being written to snapshot file.
    size_t max_snapshot_buffer_size_to_stop_read_bytes;

    // Clock
    flashcache_monotonic_clock_us monotonic_clock_us;

    // Function used for checksum calculation.
    flashcache_crc_function crc_function;

    // Buffer used for writing snapshot data
    char *snapshot_data_buffer;

    // Size (in bytes) of snapshot data buffer
    size_t snapshot_data_buffer_size;

    // Offset where data can be written to snapshot data buffer
    size_t snapshot_data_buffer_offset;

    // Current offset where data should be written in data section
    size_t snapshot_file_write_offset;

    // Contains list of buffer containing snapshot data
    stagingBuffer *snapshot_data_buffer_list;

    // Number of items in the stagging buffer after being read.
    size_t number_of_items_in_stagging_buffer;

    // Current checksum value of the data written to snapshot
    uint32_t checksum;

    // Checksum file.
    char *checksum_filename;

    // Number of items in each database
    size_t *num_items_per_db;

    // An instance for the snapshot log iterator.
    flashcacheLogIterator *snapshot_log_iterator;

    // Flag to indicate that EOF has been added to snapshot buffer
    int eof_added;

    // The snapshot save type from Redis.
    flashcacheSnapshotSaveType snapshot_save_type;

    // Callback for notifying the completion of log iteration to ASIO
    flashcacheLogIterationCallbackDetails log_iteration_completion_callback_details;

    // Flag to indicate if FC snapshotting is completed and is waiting
    // for Redis layer to complete snapshotting.
    size_t is_waiting_for_redis_snapshotting_completion;

    /**
     * When we are doing THREADSAVE replication:
     *
     * 1. If a read request comes to the processed part of the snapshot, we delete the item from FDB
     * by adding a DELETE flag to the item in the snapshot because the item will be in RDB.
     *
     * 2. If a read request comes to the pending snapshotting range, we will let the item be
     * read/deleted from FC and move to Redis.
     *
     * Read and delete are used interchangeably since a read into FC item means that the item
     * will be move to Redis.
     **/

    // Number of DELETE replication command written to snapshot during THREADSAVE replication.
    size_t curr_num_delete_repl_cmd;

    // Total size of replication data by DELETE replication command added into snapshot.
    size_t curr_delete_repl_cmd_bytes;

    // Number of items reads in unprocessed part of the snapshot during THREADSAVE.
    size_t curr_num_items_deleted_from_pending_snapshot_range;

    // Bytes read in unprocessed part of the snapshot during THREADSAVE.
    size_t curr_items_deleted_from_pending_snapshot_range_bytes;

    // Number of items that is added to RDB during THREADSAVE
    size_t curr_num_items_with_add_to_rdb_flag;

    // persisted_info collects metrics that is preserved after snapshot finishes.
    persistedInfo persisted_info;

    // Pointer to a variable which controls whether log compaction is allowed during GC
    uint8_t *can_do_log_compaction;

    // Last keep alive message sent time
    uint64_t latest_keep_alive_msg_time_us;

    // Number of keep alive messages sent
    size_t num_replication_link_keep_alive_msg;

    // The time interval in us at which keep alive messages are sent
    uint64_t snapshot_keep_alive_msg_interval_us;

    // Timeout of keep alive replication link in seconds
    size_t replication_link_timeout_secs;
} snapshotVersionTwoInfo;

// Creates snapshot info that is used for snapshotting
snapshotVersionTwoInfo *snapshotV2InfoCreate(snapshotInfoCreateParameters snapshot_info_create_params);

// Frees the resources used by snapshot info. The caller should not use the provided snapshot info after calling
// this function.
void snapshotV2InfoRelease(snapshotVersionTwoInfo *snapshot_info);

// Start writing snapshot to the specified file or stream
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
                         flashcacheLogIterationCallbackDetails *log_iteration_completion_callback_details);

// Snapshotting process is incremental. This task ensure that the next batch of work required for current ongoing
// snapshot save is done. This function returns with no action if there is no snapshot save running at the time of
// invocation.
void snapshotV2CronTask(snapshotVersionTwoInfo *snapshot_info);

// Loads the snapshot into the supplied log instance
void snapshotV2Load(struct flashcacheLog *log, char const *snapshot_filename, fioContext *snapshot_file_io_context,
                    flashcacheSnapshotSecret *secret_response, int *checksum_comparison_result,
                    char *snapshot_buffer);

// Return 1 if there is a snapshot save in progress and it has not yet completed reading the log file else returns 0
int snapshotV2IsLogReadingInProgress(snapshotVersionTwoInfo *snapshot_info);

// Cancels the ongoing snapshot save
void snapshotV2CancelSave(snapshotVersionTwoInfo *snapshot_info);

// Given an item offset, determines if the item is within the unprocessed range of an active snapshotting process.
int isUnprocessedItemInActiveSnapshotRange(snapshotVersionTwoInfo *snapshot_info, size_t offset);

// Given an item offset, determines if the item will be expedited to the FDB when read/deleted.
int snapshotV2ShouldExpediteItem(snapshotVersionTwoInfo *snapshot_info, size_t offset);

// A function to expedite adding an item to the snapshot. If a snapshot is in progress and an item that
// has not been included in the snapshot is currently being processed, this function is called to
// include the item in the snapshot before being deleted from FC.
void snapshotV2AddExpeditedItem(snapshotVersionTwoInfo *snapshot_info, size_t offset, char *item, size_t item_size);

// A function to add replication commands to the snapshot.
void snapshotV2AddReplicationCommandIfRequired(snapshotVersionTwoInfo *snapshot_info,
                                               size_t offset, uint32_t dbid, char const *key, size_t key_len,
                                               char const *value, size_t value_len,
                                               flashcache_crc_function crc_function);

// Increments the snapshot manager's tracker for number of items added to the RDB
void snapshotV2IncrementNumItemsAddedToRDB(snapshotVersionTwoInfo *snapshot_info);

// Returns 1 if the item is in an active threadsave's snapshot range
int snapshotV2IsItemInThreadsaveSnapshotRange(snapshotVersionTwoInfo *snapshot_info, size_t offset);

size_t snapshotV2GetCountBasedMetric(snapshotVersionTwoInfo *snapshot_info, flashcacheCountBasedMetrics metric);

// Updates the Snapshotting range after eviction in flash during Threadsave replication.
void snapshotV2UpdateSnapshottingRangeDuringThreadsave(snapshotVersionTwoInfo *snapshot_info,
                                                       size_t updated_log_tail_offset_after_eviction);

#endif  // __FLASHCACHE_SNAPSHOTV2_H
