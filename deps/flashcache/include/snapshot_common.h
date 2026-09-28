#ifndef __FLASHCACHE_SNAPSHOT_COMMON_H
#define __FLASHCACHE_SNAPSHOT_COMMON_H

#include "include/log.h"
#include "include/fio.h"
#include "include/staging_buffer.h"

// The log file processed offset updating logic depends on the fact that there can be a single read request to read
// log file at any point in time. Therefore the snapshot queue depth needs to be 1.
#define FC_SNAPSHOT_QUEUE_DEPTH (1)
#define FC_SNAPSHOT_LOADING_QUEUE_DEPTH (128)

// Num of fio data is the max of the fio data required for generating snapshot and the number of
// fio data required for loading snapshot. Number of fio data required for generating snapshot is
// equal to 2 * FC_SNAPSHOT_QUEUE_DEPTH because FC_SNAPSHOT_QUEUE_DEPTH number of FIO Request is
// used for reading from log file and FC_SNAPSHOT_QUEUE_DEPTH number of FIO Data is used for write
// to snapshot file
#define FC_SNAPSHOT_NUM_FIO_DATA (FC_MAX((2 * FC_SNAPSHOT_QUEUE_DEPTH), \
            FC_SNAPSHOT_LOADING_QUEUE_DEPTH))
#define FC_SNAPSHOT_READ_BUFFER_SIZE_BYTES (1024 * 1024)               // 1 MiB

struct logMetrics;

typedef struct snapshotCommon {
    // Set to 1 if snapshot is running else 0
    uint8_t is_running;

    // Set to 1 if the snapshotting process has failed and needs to be stopped else 0
    int has_failed;

    // Log file name
    char *log_filename;

    // Correlation rdb/fdb secret
    flashcacheSnapshotSecret snapshot_secret;

    // Number of databases
    uint32_t num_databases;

    // Used for capturing metrics related to the log for which snapshot is taken
    struct logMetrics *log_metrics;

    // IO context to write to the snapshot file
    fioContext *snapshot_file_io_context;

    // FIO Request used to write to snapshot file
    fioRequest fio_requests[FC_SNAPSHOT_NUM_FIO_DATA];

    // Offset in log file from where snapshot begins
    size_t log_file_tail_offset;

    // Offset in log file upto which data has been processed
    size_t log_file_processed_offset;

    // Size of log file in bytes
    size_t log_file_size_bytes;

    // Size of allocated log file in bytes
    size_t log_file_allocated_size_bytes;

    // Number of bytes processed by the snapshot in the current second.
    // Currently not used. Later on we can add this into INFO metrics for snapshot V2
    size_t log_file_processed_bytes_in_current_second;

    // Size of snapshot file in bytes
    size_t snapshot_file_size_bytes;

    // Size of snapshot file in bytes without snapshot metadata
    size_t snapshot_file_size_bytes_without_metadata;

    // Number of ongoing write to the snapshot file
    size_t num_inflight_snapshot_file_write;

    // Function used for writing snapshot data in stream based save
    flashcacheSnapshotWriter *snapshot_writer;

    // Contains details about the callback that is triggered when file based snapshot is completed or terminated.
    flashcacheSnapshotCallbackDetails callback_details;

    // Size of active log file that needs to be copied into the snapshot
    size_t active_page_aligned_log_data_size_bytes;

    // Set to 1 if snapshot generation (RDB) is done in the engine layer during replication else 0 in all other cases.
    // Needed for EOF marker in FDB
    uint8_t has_snapshotting_completed_in_engine_layer;
} snapshotCommon;

/*
 * Configurable options for flashcache snapshots
 */
typedef struct {
    size_t load_staging_buffer_max_size;  /* Max memory to be used by snapshot load staging buffer */
    size_t load_queue_depth;              /* Maximum depth of loading queue for snapshots */
} flashcacheSnapshotConfig;

extern flashcacheSnapshotConfig flashcache_snapshot_config;

typedef struct snapshotMetrics {
    size_t max_load_staging_buffer_size;
} snapshotMetrics;

typedef struct snapshotInfoCreateParameters {
    char *log_filename;
    uint32_t num_databases;
    size_t log_file_size_bytes;
    flashcacheIndex **index;
    struct logMetrics *log_metrics;
    flashcache_monotonic_clock_us monotonic_clock_us;
    flashcache_crc_function crc_function;
} snapshotInfoCreateParameters;

/**
 * Create FIO requests.
 * @param A list of available FIO
 * @return an FIO to use
 */
fioRequest *getFreeFioData(fioRequest *fio_requests);


/**
 * Flush an item from the staging buffer.
 * @param io_context IO context
 * @param fio_request FIO request
 * @param staging_buffer Staging buffer
 * @param entry entry to flush
 * @param offset offset of the entry to flush.
 * @return The number of bytes flushed.
 */
size_t flushItemFromStagingBuffer(fioContext *io_context, fioRequest *fio_request, stagingBuffer* staging_buffer,
                                  stagingBufferEntry *entry, size_t offset);

/**
 * Read and item from an FIO context in a blocking fashion.
 * @param io_context IO context.
 * @param offset offset of the item to read.
 * @param size of the item to read
 * @return item read.
 */
char *readItemBlocking(fioContext *io_context, size_t offset, size_t size);

/**
 * Call the snapshot completion callback.
 * @param snapshot_common instance of the common fields between all snapshot version.
 * @param completed status of completion 1 success 0 failure
 */
void invokeCompletionCallback(snapshotCommon *snapshot_common, int completed);

/**
 * In file based save create the snapshot file context and in Stream based initiate the stream.
 * @param snapshot_common instance of the common fields between all snapshot version.
 * @param snapshot_filename used to name the file in file based snapshot
 * @param monotonic_clock_us instance of the monotonic clock
 * @param snapshot_file_total_size_bytes number of bytes to allocate to the file or expect the stream to receive.
 */
ssize_t snapshotAllocateStorage(snapshotCommon *snapshot_common, char const *snapshot_filename,
                                flashcache_monotonic_clock_us monotonic_clock_us,
                                size_t snapshot_file_total_size_bytes);
#endif  // __FLASHCACHE_SNAPSHOT_COMMON_H
