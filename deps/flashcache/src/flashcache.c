#include "include/flashcache.h"
#include "include/log.h"
#include "include/util.h"
#include "include/fio.h"
#include "include/hash.h"
#include "include/index.h"

typedef struct flashcacheContext {
    flashcacheLog *log;
} flashcacheContext;

flashcacheContext flashcache_context = { NULL };
extern flashcache_logger logger;
flashcacheAsioControlMsgCallbackDetails asio_control_msg_callback = { NULL, NULL };

flashcacheReturnCode flashcacheInit(char const *db_filename,
        size_t db_size_bytes,
        size_t initial_index_size_per_db,
        uint32_t num_databases,
        uint32_t max_allocated_db_size_percent,
        uint32_t max_num_in_flight_read_requests,
        uint32_t min_garbage_collection_rate,
        uint32_t evict_under_max_logsize_time_limit,
        uint8_t optimized_delete_enabled,
        flashcache_monotonic_clock_us monotonic_clock_us,
        flashcacheEvictionDetails *eviction_details,
        flashcache_logger logger_,
        flashcacheAsioControlMsgCallbackDetails *asio_control_msg_callback_details) {
    flashcacheAssert(!(flashcache_context.log));
    flashcacheAssert(db_filename != NULL);
    flashcacheAssert(monotonic_clock_us != NULL);
    flashcacheAssert(eviction_details != NULL);
    flashcacheAssert(eviction_details->context != NULL);
    flashcacheAssert(eviction_details->callback != NULL);
    flashcacheAssert(max_allocated_db_size_percent > 0 && max_allocated_db_size_percent <= 100);
    flashcacheAssert(max_num_in_flight_read_requests > 0);
    flashcacheAssert(asio_control_msg_callback_details != NULL);
    flashcacheAssert(asio_control_msg_callback_details->callback != NULL);
    flashcacheAssert(asio_control_msg_callback_details->context != NULL);
    asio_control_msg_callback = *asio_control_msg_callback_details;

    logger = logger_;
    flashcacheLogger(FC_LL_NOTICE, "Starting flashcache, DB file: [%s], Num database: [%u], DB size: [%lu], "
            "Initial Index size: [%lu], Max allocated DB size percent: [%u]", db_filename, num_databases,
            db_size_bytes, initial_index_size_per_db, max_allocated_db_size_percent);

    size_t actual_db_size_bytes = getFileSize(db_filename);
    flashcacheAssertWithLogging(actual_db_size_bytes >= db_size_bytes,
                            "Configured DB file size (%lu [bytes]) is larger than the actual size (%lu [bytes])",
                            db_size_bytes, actual_db_size_bytes);

    return logCreate(&(flashcache_context.log), db_filename, db_size_bytes,
            initial_index_size_per_db, num_databases, FC_DEFAULT_STAGING_BUFFER_FLUSH_SIZE_THRESHOLD,
            max_allocated_db_size_percent, max_num_in_flight_read_requests, min_garbage_collection_rate,
            evict_under_max_logsize_time_limit, optimized_delete_enabled,
            flashcacheHasherGetByType(FLASHCACHE_SIPHASH_HASHER), monotonic_clock_us, eviction_details);
}

flashcacheReturnCode flashcachePutItem(uint32_t dbid, char const *key, size_t key_len,
        char const *value, size_t value_len) {
    flashcacheAssert(flashcache_context.log != NULL);
    flashcacheAssert(key != NULL);
    flashcacheAssert(value != NULL);

    return logWrite(flashcache_context.log, dbid, key, key_len, value,
            value_len);
}

flashcacheReturnCode flashcacheGetItem(uint32_t dbid, char const *key, size_t key_len,
        flashcacheReadTypes read_type, void *request_context, flashcache_get_item_callback completion_callback) {
    flashcacheAssert(flashcache_context.log != NULL);
    flashcacheAssert(key != NULL);
    flashcacheAssert(completion_callback != NULL);

    return logRead(flashcache_context.log, dbid, key, key_len, read_type, request_context,
        completion_callback);
}

/*!\brief Check if a key exists in the FlashCache index.
 *
 * This is a synchronous, O(1) check against the in-memory index.
 * No disk I/O is performed. Returns 1 if the key may exist, 0 if
 * it definitely does not exist.
 *
 * Note: Due to hash collisions, this may return 1 for keys that
 * don't actually exist (false positive), but will never return 0
 * for keys that do exist (no false negatives).
 */
int flashcacheKeyExists(uint32_t dbid, char const *key, size_t key_len) {
    if (flashcache_context.log == NULL) return 0;
    if (dbid >= flashcache_context.log->num_databases) return 0;
    indexEntry *entry = indexGetHeadEntry(flashcache_context.log->index_list[dbid], key, key_len);
    return (entry != NULL) ? 1 : 0;
}

void flashcacheStartFileBasedSave(char const *snapshot_filename,
        flashcacheSnapshotSecret *snapshot_secret,
        flashcacheSnapshotCallbackDetails *completion_callback_details,
        int checksum_verification_enabled,
        flashcacheSnapshotVersion snapshot_version,
        flashcacheSnapshotSaveType snapshot_save_type) {
    flashcacheAssert(flashcache_context.log != NULL);

    logStartFileBasedSave(flashcache_context.log,
                          snapshot_filename,
                          snapshot_secret,
                          completion_callback_details,
                          checksum_verification_enabled,
                          snapshot_version,
                          snapshot_save_type);
}

void flashcacheStartStreamBasedSave(flashcacheSnapshotSecret *snapshot_secret,
                                    flashcacheSnapshotWriter *snapshot_writer,
                                    flashcacheSnapshotVersion snapshot_version,
                                    flashcacheSnapshotSaveType snapshot_save_type,
                                    flashcacheLogIterationCallbackDetails *log_iteration_completion_callback_details) {
    flashcacheAssert(flashcache_context.log != NULL);

    logStartStreamBasedSave(flashcache_context.log, snapshot_secret, snapshot_writer, snapshot_version,
                            snapshot_save_type, log_iteration_completion_callback_details);
}

void flashcacheCancelSave() {
    logCancelSave(flashcache_context.log);
}

void flashcacheLoadSnapshot(char const *snapshot_filename,
                            flashcacheSnapshotSecret *shared_secret,
                            int *checksum_comparison_result) {
    flashcacheAssertWithLogging(shared_secret != NULL, "shared_secret is NULL in flashcacheLoadSnapshot", 0);
    flashcacheAssertWithLogging(checksum_comparison_result != NULL, "checksum_comparison_result is "
                                                                    "NULL in flashcacheLoadSnapshot", 0);
    logLoadSnapshot(flashcache_context.log, snapshot_filename, shared_secret, checksum_comparison_result);
}

/* Snapshot support: pause/resume GC so a fork-based snapshot child can read
 * frozen on-flash offsets safely. See logForkChildReadItem. */
void flashcacheSetGcPaused(int paused) {
    logSetGcPaused(paused);
}

int flashcacheGetGcPaused(void) {
    return logGetGcPaused();
}

/* Snapshot support: synchronous fork-child-safe read. Returns FC_OK and a
 * malloc'd serialized item (caller frees) or FC_ERR_CATCH_ALL. */
flashcacheReturnCode flashcacheForkChildReadItem(uint32_t dbid, char const *key,
        size_t key_len, char **out_item, size_t *out_len) {
    flashcacheAssert(flashcache_context.log != NULL);
    return logForkChildReadItem(flashcache_context.log, dbid, key, key_len,
                                out_item, out_len);
}

flashcacheReturnCode flashcacheRunCronTasks() {
    flashcacheAssert(flashcache_context.log != NULL);

    return logRunCronTasks(flashcache_context.log);
}

size_t flashcacheGetCountBasedMetric(flashcacheCountBasedMetrics metric) {
    switch (metric) {
        case FC_ACTIVE_MEMORY_SIZE:
            return getCurrentMemoryUsage();
        default:
            return logGetCountBasedMetric(flashcache_context.log, metric);
    }
}

void flashcacheGetHistogramMetrics(flashcacheHistogramMetrics metric,
             unsigned long long histogram[], const size_t histogram_size) {
    fioGetHistogramMetrics(metric, histogram, histogram_size);
}

void flashcacheGetHistogramIntervals(flashcacheHistogramMetrics metric,
             flashcacheHistogramInterval histogram_interval[], const size_t interval_size) {
    switch (metric) {
        case FC_DISK_READ_LATENCY_HISTOGRAM:
        case FC_DISK_WRITE_LATENCY_HISTOGRAM:
            fioGetHistogramInterval(histogram_interval, interval_size);
            break;
        default:
            flashcacheAssertWithLogging(0, "Unknown histogram metric: [%d]", metric);
    }
}

flashcacheReturnCode flashcacheTearDown() {
    flashcacheAssert(flashcache_context.log != NULL);

    logRelease(flashcache_context.log);
    flashcache_context.log = NULL;
    return FC_OK;
}

void flashcacheLogState(int level) {
    logDumpState(flashcache_context.log, level);
}

flashcacheReturnCode flashcacheFlushDB(uint32_t dbid) {
    logFlush(flashcache_context.log, (uint64_t) dbid);
    return FC_OK;
}

flashcacheReturnCode flashcacheFlushAllDBs() {
    logFlush(flashcache_context.log, FC_FLUSH_ALL_DBIDS);
    return FC_OK;
}

int flashcacheShouldRunCronTasksImmediately() {
    return logShouldRunCronTasksImmediately(flashcache_context.log);
}

void flashcacheSetConfig(flashcacheConfig *config) {
    logSetConfig(flashcache_context.log, config);
}

void flashcacheGetConfig(flashcacheConfig *config) {
    logGetConfig(flashcache_context.log, config);
}

void flashcacheFsyncBufferedWrites() {
    logFsyncBufferedWrites(flashcache_context.log);
}

void flashcacheNotifyRedisLayerSnapshotCompletion() {
    logCompleteThreadsaveReplication(flashcache_context.log);
}
