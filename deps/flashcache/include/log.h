#ifndef __FLASHCACHE_LOG_H
#define __FLASHCACHE_LOG_H

#include <stdint.h>

#include "include/fio.h"
#include "include/flashcache_common.h"
#include "include/log_entry.h"
#include "include/util.h"
#include "include/index.h"
#include "include/hash.h"
#include "include/log_iterator.h"

// Used to indicate flush of all db ids
#ifdef __cplusplus
#define FC_FLUSH_ALL_DBIDS (static_cast<uint64_t>(~0))
#else
#define FC_FLUSH_ALL_DBIDS ((uint64_t)~0)
#endif

#define FC_LOG_WRITE_REQUEST_IDENTIFIER (SIZE_MAX)
#define FC_GARBAGE_COLLECTION_LOG_READ_REQUEST_IDENTIFIER (SIZE_MAX-1)
#define FC_INTERNAL_REQUEST_QUEUE_LENGTH (2)

#define FC_NUM_BLOCKS_WRITTEN_IN_BATCH (256)

// The default threshold used for flushing item to flash from the staging buffer. This is
// currently set to 1 MiB to ensure sequential write performance on the flash.
#define FC_DEFAULT_STAGING_BUFFER_FLUSH_SIZE_THRESHOLD (1LL<<20)

typedef enum {
    /* This state indicate that the current index entry being fetched from disk
     * is still valid.
     */
    FC_CURRENT_INDEX_ENTRY_VALID,

    /* This state indicate that the current index entry being fetched from disk to
     * handle this Read request has been deleted. The new index entry in IO
     * metadata is the next index entry in the hash bucket and has not been used
     * to fetch the item this entry points to.
     */
    FC_CURRENT_INDEX_ENTRY_INVALID
} ioRequestState;

typedef struct inflightItemReadIoMetadata {
    // The context associated to the Read request.
    void *request_context;

    // The callback to be called when the item is fetched from disk.
    flashcache_get_item_callback completion_callback;

    // The state that indicate that the current index entry is still valid
    // or needs to be used to fetch the item from flash.
    ioRequestState state;

    // Database identifier of the requested item
    uint32_t dbid;

    // The key associated to the IO request
    char *key;

    // The length of the key associated to the IO request
    size_t key_len;

    // The type of flash read being processed (true read or delete)
    flashcacheReadTypes read_type;

    // The item data being request from flash (header, key, and value)
    flashcacheItemRequest requested_item_data;

    // The index entry that is being fetched or should be fetched next
    indexEntry *index_entry;

    // File IO data used for fetching the data from flash
    fioRequest fio_request;
} inflightItemReadIoMetadata;

typedef struct writeRateInfo {
    // Rate at which data was written in the last window
    size_t last_window_data_written_bytes_per_second;

    // Data written in the current window
    size_t current_window_data_written;

    // Size of window in microseconds
    uint64_t window_size_us;

    // Current window start time in microseconds
    uint64_t current_window_start_time_us;
} writeRateInfo;

typedef struct garbageCollectorInfo {
    // Set to 1 if there is an ongoing garbage collection else its 0
    uint8_t is_running;

    // Set to 0 if new garbage collection cannot be started else its 1. When set to 0, new
    // garbage collection is not started, but if it is running then it is not paused.
    // Garbage collection includes log compaction by moving log items, and item eviction
    uint8_t can_start_garbage_collection;

    // Whether we can perform log compaction (via GC) by moving items. This excludes evictions
    uint8_t can_do_log_compaction;

    // Set to 1 to use adaptive garbage collection rate else set to 0 to have
    // a fixed garbage collection rate.
    uint8_t enable_adaptive_garbage_collection_rate;

    // The algorithm used for determining the garbage collection rate
    flashcacheGarbageCollectionAlgorithm garbage_collection_rate_algorithm;

    // Bytes to garbage collect from tail of the log every second
    size_t required_garbage_collection_bytes_per_second;

    // Amount of log garbage collected in the current second
    size_t garbage_collection_bytes_in_current_second;

    // Number of additional bytes that were garbage collected. This helps pace garbage collector by avoiding eager
    // garbage collector.
    size_t extra_garbage_collected_bytes;

    // We aggregrate the amount of log garbage collected in time window that is 1 second long. This
    // property represent the start of the current time window for which aggregation is being done.
    uint64_t current_aggregation_window_start_time_us;

    // Total number of garbage collected bytes
    size_t total_garbage_collected_bytes;

    // Total size of items in bytes moved during garbage collection
    size_t total_active_items_moved_bytes;
} garbageCollectorInfo;

// Metrics generated from the usage of the log
typedef struct logMetrics {
    // Number of bytes read for garbage collection
    size_t garbage_collection_read_bytes;

    // Number of bytes written for garbage collection when we move active items from the tail to the
    // head of the log
    size_t garbage_collection_write_bytes;

    // Number of items moved during garbage collection
    size_t garbage_collection_num_items_moved;

    // Number of disk read caused due to garbage collection
    size_t garbage_collection_num_disk_read;

    // Number of disk read that were not used due to false positive caused by hash collision
    size_t num_unused_disk_read_hash_collision;

    // Number of bytes read from disk that were not used due to false positive caused by hash collision
    size_t unused_disk_read_bytes_hash_collision;

    // Number of disk reads that were not used as did not contain the entire data
    size_t num_partial_item_read;

    // Number of bytes read from disk that were not used as did not contain the entire data
    size_t partial_item_read_bytes;

    // Number of times SAVE was started
    size_t num_start_save_request;

    // Number of times SAVE was cancelled
    size_t num_cancel_save_request;

    // Number of times SAVE completed
    size_t num_save_completed;

    // Number of times SAVE was stopped before completion
    size_t num_save_cancelled;

    // Number of times LOAD snapshot was requested
    size_t num_load_request;

    // Number of times READ item was requested
    size_t num_read_request;

    // Number of times WRITE item was requested
    size_t num_write_request;

    // Number of times DELETE item was requested
    size_t num_delete_request;

    // Number of times DELETE item was requested
    size_t num_optimized_deletes;

    // Number of bytes written to disk
    size_t total_disk_write_bytes;

    // Number of bytes read from disk
    size_t total_disk_read_bytes;

    // Number of disk writes
    size_t num_disk_write;

    // Number of disk reads
    size_t num_disk_read;

    // Number of items evicted from log
    size_t num_items_evicted;

    // Total size of items evicted from log
    size_t total_item_evicted_size_bytes;

    // Size bytes which is moved out of disk during forkless save
    size_t item_bytes_moved_from_disk_during_forkless_save;

    // Size bytes which is deleted from disk during forkless save
    size_t item_bytes_deleted_from_disk_during_forkless_save;

    // Is log iterator currently evicting while log has not reached maximum size
    size_t is_evicting_under_max_logsize;

    // Number of items evicted when log was not above maximum size
    size_t num_items_evicted_under_logsize;
} logMetrics;

typedef struct flashcacheLog {
    // Number of items currently stored in log
    size_t num_items;

    // Number of databases
    uint32_t num_databases;

    // The offset of the head of the log on flash
    size_t head_offset;

    // Items are written to flash asynchronously. This variable points to the new head offset
    // once items that are inflight of being written to flash are successfully written.
    size_t head_offset_after_flush_succeed;

    // The offset of the tail of the log on flash
    size_t tail_offset;

    // Number of bytes allocated on log per db (This only contains the live items)
    size_t *allocated_log_size_bytes_per_db;

    // Number of bytes allocated on log (This only contains the live items)
    size_t allocated_log_size_bytes;

    // Total size of the log file on flash
    size_t log_size_bytes;

    // A value to override the allocatable log_size_bytes.
    ssize_t overriden_allocatable_log_size_bytes;

    // Maximum allowed allocated log size as a percentage of log size. When the allocated log size becomes
    // greater than this threshold, keys are evicted during garbage collection
    uint32_t max_allocated_log_size_percent;

    // Maximum amount of data that can buffered when written to the log
    size_t max_buffered_write_size_bytes;

    // Maximum number of in-flight read requests to flash before throttling.
    size_t max_in_flight_item_read_requests;

    // Stores the callback that is triggered when a key is evicted
    flashcacheEvictionDetails eviction_details;

    // The staging buffer to hold the recently put item. Once the size of staging
    // buffer crosses a certain threshold, the items in it are flushed to flash.
    struct stagingBuffer *staging_buffer;

    // All the items from this entry to the tail entry in the staging buffer are being
    // flushed to flash.
    struct stagingBufferEntry *head_entry_being_flushed_to_flash;

    // The size of staging buffer beyond which all the items in the staging buffer is
    // flushed to flash.
    size_t staging_buffer_flush_size_threshold_bytes;

    // The index hash table list that contains the location of all the items in the store.
    struct flashcacheIndex **index_list;

    // The hasher used for determining the hash bucket in the index and computing
    // hash to avoid collision in a hash bucket.
    flashcacheHasher hasher;

    // CRC function used for computing the checksum of data stored in the log.
    flashcache_crc_function crc_function;

    // Async IO context for the log file on flash
    fioContext *fio_context;

    // Number of pending read fio requests (used for both reads and deltes)
    int num_inflight_item_read_io_request;

    // Metadata about the inflight read requests from the log file on flash
    struct inflightItemReadIoMetadata *inflight_item_read_io_metadata;

    // File IO data used to flush items from staging buffer to the log on flash
    fioRequest log_flush_fio_request;

    // Stores garbage collection details
    struct garbageCollectorInfo garbage_collector_info;

    // Store write rate details
    struct writeRateInfo write_rate_info;

    // Metrics generated from the usage of the log instance
    struct logMetrics metrics;

    // Database id of current index growth
    size_t current_index_growth_dbid;

    // Number of times index growth operation has run
    size_t num_index_growth_run;

    // Clock used for finding elapsed time
    flashcache_monotonic_clock_us monotonic_clock_us;

    // Enable/Disable eviction during garbage collection
    int eviction_enabled;

    // Flag to enable/disable optimization of resetting head/tail offset.
    // Currently it is for controlling unit tests for debugging. Enabled by default.
    int should_reset_head_tail_offset_of_log;

    // An instance of the log iterator.
    flashcacheLogIterator *log_iterator;

    // The flashcache garbage collection base rate (bytes per second)
    uint32_t min_garbage_collection_rate;

    // The flashcache dynamic garbage collection max rate (bytes per second)
    uint32_t max_dynamic_garbage_collection_rate;

    // Rate at which eviction occurs from flashcache log before it has reached maximum size (bytes per second)
    // If value is 0, eviction under max logsize is disabled
    uint32_t evict_under_max_logsize_min_rate;

    // Last timestamp of enabling evict_under_max_logsize.
    uint64_t last_evict_under_max_logsize_start;

    // Longest amount of time that eviction under max logsize can run without a signal from the engine
    uint32_t evict_under_max_logsize_time_limit;

    // Tracks if flashcache is optimizing deletes (versus using reads for deleting)
    uint8_t optimized_delete_enabled;
} flashcacheLog;

// Creates an instance of the log object
flashcacheReturnCode logCreate(flashcacheLog **log, char const *log_filename, size_t log_size_bytes,
        size_t intial_index_size_per_db, uint32_t num_databases, size_t staging_buffer_flush_size_threshold_bytes,
        uint32_t max_allocated_log_size_percent, uint32_t max_num_in_flight_read_requests,
        uint32_t min_garbage_collection_rate,
        uint32_t evict_under_max_logsize_time_limit, uint8_t optimized_delete_enabled, flashcacheHasher *hasher,
        flashcache_monotonic_clock_us monotonic_clock_us, flashcacheEvictionDetails *eviction_details);

// Writes a new item to the log. This write first goes to the staging buffer which is
// eventually flushed to the log on flash.
flashcacheReturnCode logWrite(struct flashcacheLog *log, uint32_t dbid, char const *key, size_t key_len,
        char const *value, size_t value_len);

// Reads the value corresponding to the key and db id. The provided callback is called when the value is
// retrieved.
flashcacheReturnCode logRead(struct flashcacheLog *log, uint32_t dbid, char const *key, size_t key_len,
        flashcacheReadTypes read_type, void *request_context, flashcache_get_item_callback completion_callback);

// Start the process of taking a point in time snapshot. All the data written before calling this method will be
// present in the snapshot
// logStartFileBasedSave consumes an rdb/fdb correlation secret. The size of the correlation secret must be
// 0 to FC_SNAPSHOT_MAX_SECRET_SIZE bytes!
// The checksum_verification_enabled is a flag to indicate whether we need to save the snapshot's checksum
// in a file of not. The snapshot_version is used to select the algorithm version for snapshotting.
void logStartFileBasedSave(struct flashcacheLog *log,
                  char const* snapshot_filename,
                  flashcacheSnapshotSecret *snapshot_secret,
                  flashcacheSnapshotCallbackDetails *callback_details,
                  int checksum_verification_enabled,
                  flashcacheSnapshotVersion snapshot_version,
                  flashcacheSnapshotSaveType snapshot_save_type);

// Start the process of taking a point in time snapshot using Snapshot writer. All the data written before calling
// this method will be present in the snapshot.
// logStartStreamBasedSave consumes an rdb/fdb correlation secret and snapshot writer instance which contains bunch
// of callback APIs. The size of the correlation secret must be 0 to FC_SNAPSHOT_MAX_SECRET_SIZE bytes.
// The snapshot_version is used to select the algorithm version for snapshotting.
void logStartStreamBasedSave(flashcacheLog *log, flashcacheSnapshotSecret *snapshot_secret,
                             flashcacheSnapshotWriter *snapshot_writer,
                             flashcacheSnapshotVersion snapshot_version,
                             flashcacheSnapshotSaveType snapshot_save_type,
                             flashcacheLogIterationCallbackDetails *log_iteration_completion_callback_details);

// Cancel the ongoing snapshotting
void logCancelSave(struct flashcacheLog *log);

// Load the provided snapshot. This is a blocking call and returns only after the snapshot has been loaded. The
// previous data is deleted before loading the snapshot. The secret_response and checksum_comparison_result are
// output parameters to carry the fdb's secret and checksum validation results to the caller.
void logLoadSnapshot(struct flashcacheLog *log, char const *snapshot_filename,
                     flashcacheSnapshotSecret *secret_response,
                     int *checksum_comparison_result);

// Runs the cron tasks associated with the log like garbage collection, handling pending read request.
flashcacheReturnCode logRunCronTasks(struct flashcacheLog *log);

// Returns the value associated with the specified metric
size_t logGetCountBasedMetric(flashcacheLog *log, flashcacheCountBasedMetrics metric);

// Releases the log object. Caller should not use the log object after releasing it.
void logRelease(struct flashcacheLog *log);

// Dump the state of the log instance
void logDumpState(struct flashcacheLog *log, int level);

// Flush the index for the given db or all dbs if dbid = FC_FLUSH_ALL_DBIDS
void logFlush(struct flashcacheLog *log, uint64_t dbid);

// Returns 1 if there are pending cron tasks that needs to be processed immediately else returns 0
int logShouldRunCronTasksImmediately(struct flashcacheLog *log);

// Set the specified configuration
void logSetConfig(flashcacheLog *log, flashcacheConfig *config);

// Get the value for the specified configuration key
void logGetConfig(flashcacheLog *log, flashcacheConfig *config);

// Flush buffered writes to disk
void logFsyncBufferedWrites(flashcacheLog *log);

// Invokes the ASIO control message callback so that these messages are processed timely
// even during long-running operations in flashcache
void invokeAsioControlMsgCallback();

// Flush staging buffer.
void logFlushStagingBufferIfRequired(flashcacheLog *log, size_t threshold);

// Completes forkless save replication
void logCompleteForklessSaveReplication(flashcacheLog *log);

#endif  // __FLASHCACHE_LOG_H
