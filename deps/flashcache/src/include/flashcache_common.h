#ifndef __FLASHCACHE_COMMON_H
#define __FLASHCACHE_COMMON_H

#include <stdint.h>
#include <stddef.h>

/* rdb/fdb correlation secret size */
#define FC_SNAPSHOT_MAX_SECRET_SIZE (128)

/* Log levels */
#define FC_LL_DEBUG (0)
#define FC_LL_VERBOSE (1)
#define FC_LL_NOTICE (2)
#define FC_LL_WARNING (3)

typedef struct {
    long long interval_start;
    long long interval_end;
} flashcacheHistogramInterval;

typedef enum {
    FC_SAVE_TYPE_BGSAVE,
    FC_SAVE_TYPE_FORKLESS_SAVE
} flashcacheSnapshotSaveType;

typedef enum {
    // Snapshot version that includes index and log data. Checksum is calculated per item.
    FC_SNAPSHOT_VERSION_ONE = 1,
    // Snapshot version that includes only log data. Checksum per item and for the whole snapshot.
    FC_SNAPSHOT_VERSION_TWO
} flashcacheSnapshotVersion;

typedef enum {
    FC_OK,
    FC_ERR_SETUP_DB_FILE,
    FC_ERR_THROTTLED,
    FC_ERR_CATCH_ALL
} flashcacheReturnCode;

typedef enum {
    // Amount of free allocatable space in the database
    FC_FREE_ALLOCATABLE_DB_SIZE_BYTES,

    // Amount of space used over the max allocatable space in the database
    FC_DB_USED_OVER_ALLOCATABLE_DB_SIZE_BYTES,

    // Number of bytes used by items in the database
    FC_ALLOCATED_DB_SIZE_BYTES,

    // Number of bytes used by alive and deleted items (which have not been garbage collected)
    // in the database.
    FC_ACTIVE_DB_SIZE_BYTES,

    // Total size of the database. This include the alive items, deleted items (which have not been
    // garbage collected) and current free space in the database.
    FC_TOTAL_DB_SIZE_BYTES,

    // Size of items pending to be written to disk
    FC_ITEM_PENDING_FLUSH_SIZE_BYTES,

    // Current required garbage collection rate
    FC_GARBAGE_COLLECTION_CURR_RATE_BYTES_PER_SECOND,

    // The minimum flashcache garbage collection rate (bytes per second)
    FC_MIN_GARBAGE_COLLECTION_RATE,

    // Number of disk read caused due to garbage collection
    FC_GARBAGE_COLLECTION_NUM_DISK_READ,

    // Number of bytes read for garbage collection
    FC_GARBAGE_COLLECTION_READ_BYTES,

    // Number of bytes written for garbage collection when we move active items from the tail to the
    // head of the log
    FC_GARBAGE_COLLECTION_WRITE_BYTES,

    // Number of items moved during garbage collection
    FC_GARBAGE_COLLECTION_NUM_ITEMS_MOVED,

    // Number of disk reads that were not used due to false positive caused by hash collision
    FC_NUM_UNUSED_DISK_READ_HASH_COLLISION,

    // Number of bytes read from disk that were not used due to false positive caused by hash collision
    FC_UNUSED_DISK_READ_BYTES_HASH_COLLISION,

    // Number of disk reads that were not used as did not contain the entire item
    FC_NUM_PARTIAL_ITEM_READ,

    // Number of bytes read from disk that were not used as did not contain the entire item
    FC_PARTIAL_ITEM_READ_BYTES,

    // Number of times SAVE was started
    FC_SAVE_NUM_START_REQUEST,

    // Number of times SAVE was cancelled
    FC_SAVE_NUM_CANCEL_REQUEST,

    // Number of times SAVE completed
    FC_SAVE_NUM_COMPLETED,

    // Number of times SAVE was stopped before completion
    FC_SAVE_NUM_CANCELLED,

    // Number of times LOAD snapshot was requested
    FC_NUM_LOAD_REQUEST,

    // Number of times READ item was requested
    FC_NUM_READ_REQUEST,

    // Number of READ request currently being processed
    FC_NUM_READ_IN_FLIGHT,

    // Number of times WRITE item was requested
    FC_NUM_WRITE_REQUEST,

    // Number of times DELETE item was requested
    FC_NUM_DELETE_REQUEST,

    // Number of times an optimized deletion was used
    FC_NUM_OPTIMIZED_DELETES,

    // Number of bytes written to disk
    FC_TOTAL_DISK_WRITE_BYTES,

    // Number of bytes read from disk
    FC_TOTAL_DISK_READ_BYTES,

    // Number of disk writes
    FC_NUM_DISK_WRITE,

    // Number of disk reads
    FC_NUM_DISK_READ,

    // Number of items currently stored in log
    FC_NUM_ITEMS,

    // Number of items evicted from log
    FC_NUM_ITEMS_EVICTED,

    // Total size of items evicted from log
    FC_TOTAL_EVICTED_ITEMS_SIZE_BYTES,

    // Current Memory Usage
    FC_ACTIVE_MEMORY_SIZE,

    // Index Growth operation running or not
    FC_IS_INDEX_GROWING,

    // Number of index growth run
    FC_NUM_INDEX_GROWTH_RUN,

    // Number of retryable disk error
    FC_NUM_RETRYABLE_DISK_ERROR,

    // Flashcache is waiting for engine snapshotting to finish or not
    FC_IS_WAITING_FOR_ENGINE_SNAPSHOTTING_COMPLETION,

    // Number of DELETE replication commands sent to FDB
    FC_CURR_NUM_DELETE_REPL_CMD,

    // Number of bytes of DELETE replication commands sent to FDB
    FC_CURR_DELETE_REPL_CMD_BYTES,

    // Number of items read in pending snapshotting range and moved back to the engine
    // during forkless save replication
    FC_CURR_NUM_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE,

    // Number of bytes read in pending snapshotting range and moved back to the engine
    // during forkless save replication
    FC_CURR_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE_BYTES,

    // Number of items that needs to be added to RDB while doing forkless save replication
    FC_CURR_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG,

    // Number of DELETE replication commands sent to FDB in the previous snapshot
    FC_LAST_NUM_DELETE_REPL_CMD,

    // Number of bytes of DELETE replication commands sent to FDB in the previous snapshot
    FC_LAST_DELETE_REPL_CMD_BYTES,

    // Number of items read in pending snapshotting range and moved back to the engine
    // during forkless save replication in the previous snapshot
    FC_LAST_NUM_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE,

    // Number of bytes read in pending snapshotting range and moved back to the engine
    // during forkless save replication in the previous snapshot
    FC_LAST_ITEMS_DELETED_FROM_PENDING_SNAPSHOT_RANGE_BYTES,

    // Number of items that needs to be added to RDB while doing forkless save replication
    // in the previous snapshot
    FC_LAST_NUM_ITEMS_WITH_ADD_TO_RDB_FLAG,

    // Latest keep alive message time in us
    FC_LATEST_KEEP_ALIVE_MSG_TIME_US,

    // Size bytes of item which has been moved out of disk during forkless save replication
    FC_ITEM_BYTES_MOVED_FROM_DISK,

    // Size bytes of item which has been deleted from disk during forkless save replication
    FC_ITEM_BYTES_DELETED_FROM_DISK,

    // Is log iterator evicting before flashcache log has reached max size or not
    FC_IS_EVICTING_UNDER_MAX_LOGSIZE,

    // Number of items evicted from flashcache before log reached max size
    FC_NUM_ITEMS_EVICTED_UNDER_MAX_LOGSIZE
} flashcacheCountBasedMetrics;

typedef enum {
    // FIO Read Latency Histogram. It consists of 6 intervals range in microseconds.
    FC_DISK_READ_LATENCY_HISTOGRAM,

    // FIO Write Latency Histogram. It consists of 6 intervals range in microseconds.
    FC_DISK_WRITE_LATENCY_HISTOGRAM
} flashcacheHistogramMetrics;

typedef enum {
    // In this algorithm, the garbage collection rate is proportional to (log_size - free_space_in_log)
    FC_GARBAGE_COLLECTION_LINEAR_WITH_FREE_SPACE,

    // In this algorithm, the garbage collection rate is proportional to (1 / free_space_in_log)
    FC_GARBAGE_COLLECTION_INVERSE_WITH_FREE_SPACE
} flashcacheGarbageCollectionAlgorithm;

typedef enum {
    // Config to enable/disable eviction
    FC_CONFIG_KEY_EVICTION_ENABLED,
    // Config to change the allocatable db size on flash
    FC_CONFIG_KEY_OVERRIDEN_ALLOCATABLE_DB_SIZE_BYTES,
    // Config for the amount of data that can be bufered when written to the store. When the amount of buffered writes
    // exceeds this limit, the writes are throttled.
    FC_CONFIG_KEY_MAX_BUFFERED_WRITE_SIZE_BYTES,
    // Config for the threshold used to flush buffered write
    FC_CONFIG_KEY_BUFFERED_WRITE_FLUSH_THRESHOLD_BYTES,
    // Config for max size of snapshot that be buffered in memory before being written to snapshot file
    FC_CONFIG_KEY_MAX_SNAPSHOT_BUFFER_SIZE_BYTES,
    // Config to update the status of engine layer snapshotting completion
    FC_CONFIG_KEY_ENGINE_LAYER_SNAPSHOT_COMPLETION_STATUS,
    // Config to change the time interval keep alive messages are sent in us
    FC_CONFIG_KEY_SNAPSHOT_KEEP_ALIVE_MSG_INTERVAL_US,
    // Config to change the max amount of time FC waits for engine snapshotting to complete in seconds
    FC_CONFIG_KEY_REPLICATION_LINK_TIMEOUT_SECS,
    // Config for the minumum flashcache garbage collection rate (bytes per second)
    FC_CONFIG_KEY_MIN_GARBAGE_COLLECTION_RATE,
    // Config for the maximum dynamic flashcache garbage collection rate (bytes per second)
    FC_CONFIG_KEY_MAX_DYNAMIC_GARBAGE_COLLECTION_RATE,
    // Rate at which log iterator will evict when log size has not yet reached the maximum (bytes per second)
    // Rate of 0 means eviction under max logsize is disabled
    FC_CONFIG_KEY_EVICT_UNDER_MAX_LOGSIZE_RATE,
    // Config to change the amount of time that log iterator can evict under max logsize
    // without a signal from the engine
    FC_CONFIG_KEY_EVICT_UNDER_MAX_LOGSIZE_TIME_LIMIT,
    // Config used to enable/disable optimized deletes in flashcache. Optimized deletes minimize latency
    // by requesting fewer pages.
    FC_CONFIG_KEY_OPTIMIZED_DELETE_ENABLED,
} flashcacheConfigKey;

typedef enum {
    FC_READ,
    FC_DELETE
} flashcacheReadTypes;

typedef enum {
    FC_ITEM_WITH_VALUE,
    FC_ITEM_WITHOUT_VALUE
} flashcacheItemRequest;

typedef struct {
    // Config key
    flashcacheConfigKey key;

    // Union for value. Depending on the key, the appropriate member is set.
    union {
        int64_t numeric_value;
    };
} flashcacheConfig;

// Function for logging information
typedef void (*flashcache_logger)(int level, const char *fmt, ...);

// Clock function that return time in microseconds. This clock is used for computing elapsed
// time.
typedef uint64_t (*flashcache_monotonic_clock_us)(void);

/* This callback is provided for reading a value from the store. This callback
 * is called when the value against the key is retrieved. The value is freed
 * by the store after the callback call returns. This callback can be called
 * immediately during the flashcacheGetData call or handled at a later point
 * in time during the flashcacheRunCronTasks call.
 */
typedef void (*flashcache_get_item_callback)(void *request_context, char *value,
        size_t value_len, int add_item_to_rdb);

/* This callback is triggered when a key is evicted as the allocated database size exceeds
 * the allowed database size
 */
typedef void (*flashcache_eviction_callback)(void *context, uint32_t dbid, char *key, size_t key_len);

// Store the details related to callback called when a key is evicted
typedef struct {
    // The context that is passed to the eviction callback in every call
    void *context;

    // The callback that is triggered when a key is evicted from database
    flashcache_eviction_callback callback;
} flashcacheEvictionDetails;

/**
 * Flashcache has several code paths that may take significant time including saving or
 * loading a snapshot. This callback is to be invoked periodically during these long running
 * operations so that ASIO can respond in a timely manner to control messages from the Valkey main
 * thread in a timely fashion.
 *
 * (Valkey main thread) --> (control message: i.e. pull_metrics) --> ASIO thread
 *                                                                   (executing long-running flashcache function)
 * (Valkey main thread) <-- (timely control message reply) <-------- Flashcache(ASIO control msg callback)
 */

typedef void (*flashcache_asio_control_msg_callback)(void *context);

typedef struct {
    /* The context that is passed to the asio control msg callback */
    void *context;

    /* The callback that is invoked periodically during long running operations
     * in FlashCache
     */
    flashcache_asio_control_msg_callback callback;
} flashcacheAsioControlMsgCallbackDetails;

/* This callback is triggered when snapshotting is complete or is cancelled
 */
typedef void (*flashcache_snapshot_completion_callback)(void *context, int success);

// Store the details related to callback called when snapshot is completed or cancelled
typedef struct {
    // The context that is passed to the snapshot completion callback
    void *context;

    // The callback that is triggered when snapshot is completed or cancelled
    flashcache_snapshot_completion_callback callback;
} flashcacheSnapshotCallbackDetails;

/* This callback is triggered when log iteration is completed during forkless save replication */
typedef void (*flashcache_log_iteration_completion_callback)(void *context);

typedef struct {
    /* The context that is passed to the asio control msg callback */
    void *context;

    /* The callback that is invoked when lot iteration is completed
     * during forkless save replication
     */
    flashcache_log_iteration_completion_callback callback;
} flashcacheLogIterationCallbackDetails;

/*
 * Structure to store and retrieve a secret associated with each snapshot.
 */
typedef struct {
    size_t size; /* The snapshot secret size */
    unsigned char secret[FC_SNAPSHOT_MAX_SECRET_SIZE]; /* The snapshot secret */
} flashcacheSnapshotSecret;

/*
 * Structure for snapshot writer. This contains APIs required for stream based snapshot.
 */
typedef struct {
    // The callback context that is passed when the callbacks are invoked.
    void *callback_context;

    /**
     * This API is called at the beginning of the snapshotting operation. This helps
     * the writer know about the size of the snapshot to be generated.
     * @Returns : Void
     * @param :
     * callback_context : The context that was passed from the caller when
     * flashcacheStartStreamBasedSave was called.
     * size : Size of a snapshot which is about to generate.
     */
    void (*set_snapshot_size)(void *callback_context, size_t size);

    /**
     * This API is used to write chunks to the writer. The sum of buf_len
     * from all the write calls will sum up to the snapshot size.
     * @Returns : Void
     * @param :
     * callback_context : The context that was passed from the caller when
     * flashcacheStartStreamBasedSave was called.
     * offset : Offset where we want to write the snapshot chuck.
     * buf : Chunk of Data which needs to be written in Stream based snapshot.
     * buf_len : length of chunk of data.
     */
    void (*write)(void *callback_context, size_t offset, char *buf, size_t buf_len);

    /**
     * This API is used to check if the writer is writable before calling
     * the write API.
     * @Returns : 1 if writer is writable else 0.
     * @Params :
     * callback_context : The context that was passed from the caller when
     * flashcacheStartStreamBasedSave was called.
     */
    int (*is_writable)(void *callback_context);

    /**
     * This API is used to write a keep alive message to the writer. 
     * This is to keep the replication link alive during forkless save.
     *
     * @Returns : Void
     * @param :
     * callback_context : The context that was passed from the caller when
     * flashcacheStartStreamBasedSave was called.
     */
    void (*keep_alive)(void *callback_context);

    /**
     * This API is used to indicate that the snapshotting has completed. if the
     * snapshotting was terminated before completion, completed is set to 0 else its 1.
     * @Returns : void
     * @Params :
     * callback_context : The context that was passed from the caller when
     * flashcacheStartStreamBasedSave was called.
     * completed : 0 if snapshotting was terminated before completion else 1.
     */
    void (*complete)(void *callback_context, int completed);
} flashcacheSnapshotWriter;

#endif  // __FLASHCACHE_COMMON_H
