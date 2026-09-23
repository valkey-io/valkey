#include "include/snapshot_version_one.h"
#include "include/snapshot_version_two.h"
#include "include/snapshot_manager.h"
#include "include/snapshot_common.h"
#include "include/util.h"

static snapshotManagerInfo snapshot_manager_info = { 0 };
static snapshotInfoCreateParameters snapshot_info_create_params = { 0 };

// Not static for testing purposes
snapshotManagerInfo *getSnapshotManagerInfo() {
    return &snapshot_manager_info;
}

static void initializeSnapshotInfoCreateParams(char const *log_filename, uint32_t num_databases,
                                 size_t log_file_size_bytes,
                                 flashcacheIndex **index,
                                 struct logMetrics *log_metrics,
                                 flashcache_monotonic_clock_us monotonic_clock_us,
                                 flashcache_crc_function crc_function) {
    snapshot_info_create_params.log_filename = (char *) fcMalloc(strlen(log_filename) + 1);
    flashcacheAssert(snapshot_info_create_params.log_filename != NULL);
    memcpy(snapshot_info_create_params.log_filename, log_filename, strlen(log_filename) + 1);

    snapshot_info_create_params.num_databases = num_databases;
    snapshot_info_create_params.log_file_size_bytes = log_file_size_bytes;
    snapshot_info_create_params.index = index;
    snapshot_info_create_params.log_metrics = log_metrics;
    snapshot_info_create_params.monotonic_clock_us = monotonic_clock_us;
    snapshot_info_create_params.crc_function = crc_function;
}


void snapshotManagerInfoCreate(char const *log_filename, uint32_t num_databases,
                               size_t log_file_size_bytes,
                               flashcacheIndex **index,
                               struct logMetrics *log_metrics,
                               flashcache_monotonic_clock_us monotonic_clock_us,
                               flashcache_crc_function crc_function) {
    // Initialize the snapshot info params struct.
    initializeSnapshotInfoCreateParams(log_filename, num_databases, log_file_size_bytes, index, log_metrics,
                               monotonic_clock_us, crc_function);

    // Set the snapshot version default to 2.
    snapshot_manager_info.snapshot_version_two_info = snapshotV2InfoCreate(snapshot_info_create_params);
    snapshot_manager_info.snapshot_version = FC_SNAPSHOT_VERSION_TWO;
}

void snapshotManagerInfoRelease() {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            snapshotInfoRelease(snapshot_manager_info.snapshot_version_one_info);
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshotV2InfoRelease(snapshot_manager_info.snapshot_version_two_info);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in snapshotManagerInfoRelease", 0);
            break;
    }
    fcFree(snapshot_info_create_params.log_filename);
}

// Not static for unit tests.
void adjustSnapshotVersion(flashcacheSnapshotVersion new_snapshot_version) {
    if (snapshot_manager_info.snapshot_version == new_snapshot_version) return;
    switch (new_snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            snapshotV2InfoRelease(snapshot_manager_info.snapshot_version_two_info);
            snapshot_manager_info.snapshot_version_one_info = snapshotInfoCreate(snapshot_info_create_params);
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshotInfoRelease(snapshot_manager_info.snapshot_version_one_info);
            snapshot_manager_info.snapshot_version_two_info = snapshotV2InfoCreate(snapshot_info_create_params);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in adjustSnapshotVersion", 0);
            break;
    }
    snapshot_manager_info.snapshot_version = new_snapshot_version;
}

void snapshotManagerStartSave(flashcacheSnapshotSecret *snapshot_secret,
                              size_t log_file_tail_offset,
                              size_t active_log_file_size_bytes, size_t log_file_allocated_size_bytes,
                              size_t *log_file_allocated_size_bytes_per_db,
                              flashcache_monotonic_clock_us monotonic_clock_us,
                              flashcacheHasher *hasher,
                              flashcacheIndex *index,
                              size_t current_index_growth_dbid,
                              flashcacheSnapshotCallbackDetails *callback_details,
                              char const *snapshot_filename,
                              flashcacheSnapshotWriter *snapshot_writer,
                              int checksum_verification_enabled,
                              flashcacheSnapshotVersion snapshot_version,
                              flashcacheSnapshotSaveType snapshot_save_type,
                              uint8_t *can_do_log_compaction,
                              flashcacheLogIterationCallbackDetails *log_iteration_completion_callback_details) {
    adjustSnapshotVersion(snapshot_version);
    flashcacheLogger(FC_LL_WARNING, "Starting snapshot version `%d` saving.",
                                     snapshot_manager_info.snapshot_version);
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            if (snapshot_writer != NULL && snapshot_save_type == FC_SAVE_TYPE_THREADSAVE) {
                flashcacheAssertWithLogging(0, "Snapshot V1 does not support THREADSAVE replication.", 0);
            }
            indexPauseGrowth(index);  // If index growth is Running, Pause it.

            snapshotStartSave(snapshot_manager_info.snapshot_version_one_info,
                              snapshot_secret,
                              log_file_tail_offset,
                              active_log_file_size_bytes, log_file_allocated_size_bytes,
                              log_file_allocated_size_bytes_per_db,
                              monotonic_clock_us,
                              hasher,
                              current_index_growth_dbid,
                              index->growth_iterator->status,
                              index->growth_iterator->next_hash_bucket,
                              callback_details, snapshot_filename,
                              snapshot_writer);
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshotV2StartSave(snapshot_manager_info.snapshot_version_two_info,
                                snapshot_secret,
                                log_file_tail_offset,
                                active_log_file_size_bytes,
                                log_file_allocated_size_bytes,
                                callback_details,
                                snapshot_filename,
                                snapshot_writer,
                                checksum_verification_enabled,
                                snapshot_save_type,
                                can_do_log_compaction,
                                log_iteration_completion_callback_details);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in snapshotManagerStartSave", 0);
            break;
    }
}

int snapshotManagerIsLogReadingInProgress() {
    int ret = 0;
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            ret = snapshotIsLogReadingInProgress(snapshot_manager_info.snapshot_version_one_info);
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            ret = snapshotV2IsLogReadingInProgress(snapshot_manager_info.snapshot_version_two_info);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in snapshotManagerIsLogReadingInProgress", 0);
            break;
    }
    return ret;
}

size_t snapshotManagerGetLogFileProcessedOffset() {
    size_t ret = 0;
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            ret = snapshot_manager_info.snapshot_version_one_info->snapshot_common.log_file_processed_offset;
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            ret = snapshot_manager_info.snapshot_version_two_info->snapshot_common.log_file_processed_offset;
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in snapshotManagerGetLogFileProcessedOffset", 0);
            break;
    }
    return ret;
}

void snapshotManagerCancelSave() {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            snapshotCancelSave(snapshot_manager_info.snapshot_version_one_info);
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshotV2CancelSave(snapshot_manager_info.snapshot_version_two_info);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in snapshotManagerCancelSave", 0);
            break;
    }
}

void snapshotManagerLoad(struct flashcacheLog *log,
                         char const *snapshot_filename,
                         flashcacheSnapshotSecret *secret_response,
                         int *checksum_comparison_result) {
    fioContext *snapshot_file_io_context =
            fioCreateContext(snapshot_filename,
                             FILE_READ_ONLY,
                             FC_SNAPSHOT_LOADING_QUEUE_DEPTH,
                             log->monotonic_clock_us);
    flashcacheAssertWithLogging(snapshot_file_io_context != NULL, "Snapshot FIO is NULL before loading.", 0);

    // Read the snapshot version.
    flashcacheSnapshotVersion current_snapshot_version;
    char *first_page_in_snapshot = readItemBlocking(snapshot_file_io_context, 0, FC_PAGESIZE);
    memcpy(&current_snapshot_version, first_page_in_snapshot, sizeof(uint32_t));

    flashcacheLogger(FC_LL_WARNING, "Starting snapshot version `%d` loading.", current_snapshot_version);
    switch (current_snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            snapshotLoad(log, snapshot_file_io_context, secret_response, first_page_in_snapshot);
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshotV2Load(log, snapshot_filename, snapshot_file_io_context, secret_response,
                           checksum_comparison_result, first_page_in_snapshot);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in snapshotManagerLoad", 0);
            break;
    }
}

void snapshotManagerCronTask() {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            snapshotCronTask(snapshot_manager_info.snapshot_version_one_info);
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshotV2CronTask(snapshot_manager_info.snapshot_version_two_info);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in snapshotManagerCronTask", 0);
            break;
    }
}

int snapshotManagerIsRunning() {
    int ret = 0;
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            ret = snapshot_manager_info.snapshot_version_one_info->snapshot_common.is_running;
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            ret = snapshot_manager_info.snapshot_version_two_info->snapshot_common.is_running;
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in snapshotManagerIsRunning", 0);
            break;
    }
    return ret;
}

int snapshotManagerIsSnapshotV1Running() {
    if (snapshotManagerIsRunning()) {
        return snapshot_manager_info.snapshot_version == FC_SNAPSHOT_VERSION_ONE;
    }
    return 0;
}

void snapshotManagerSetMaxSnapshotBufferSizeBytes(size_t value) {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            snapshot_manager_info.snapshot_version_one_info->max_snapshot_buffer_size_bytes = value;
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshot_manager_info.snapshot_version_two_info->max_snapshot_buffer_size_to_stop_read_bytes = value;
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in "
                                           "snapshotManagerSetMaxSnapshotBufferSizeBytes", 0);
            break;
    }
}

size_t snapshotManagerGetMaxSnapshotBufferSizeBytes() {
    size_t ret = 0;
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            ret = snapshot_manager_info.snapshot_version_one_info->max_snapshot_buffer_size_bytes;
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            ret = snapshot_manager_info.snapshot_version_two_info->max_snapshot_buffer_size_to_stop_read_bytes;
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in "
                                           "snapshotManagerGetMaxSnapshotBufferSizeBytes", 0);
            break;
    }
    return ret;
}

int snapshotManagerShouldExpediteItem(size_t offset) {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            return 0;
        case FC_SNAPSHOT_VERSION_TWO:
            return snapshotV2ShouldExpediteItem(snapshot_manager_info.snapshot_version_two_info, offset);
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in snapshotManagerShouldExpediteItem", 0);
    }
    return 0;
}

void snapshotManagerAddExpeditedItem(size_t offset, char *item, size_t item_size) {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            // nothing to do for this snapshotting version.
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshotV2AddExpeditedItem(snapshot_manager_info.snapshot_version_two_info, offset, item, item_size);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in snapshotManagerAddExpeditedItem", 0);
            break;
    }
}

void snapshotManagerAddReplicationCommandIfRequired(size_t offset, uint32_t dbid, char const *key,
                                                    size_t key_len, char const *value, size_t value_len,
                                                    flashcache_crc_function crc_function) {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            // nothing to do for this snapshotting version.
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshotV2AddReplicationCommandIfRequired(snapshot_manager_info.snapshot_version_two_info, offset,
                                                      dbid, key, key_len, value, value_len, crc_function);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
           flashcacheAssertWithLogging(0, "Unknown snapshot version %d in "
                                           "snapshotManagerAddReplicationCommandIfRequired",
                                        snapshot_manager_info.snapshot_version, 0);
            break;
    }
}

void snapshotManagerSetHasSnapshottingCompletedInRedisLayer(uint8_t value) {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            // nothing to do for this snapshotting version.
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshot_manager_info.snapshot_version_two_info->snapshot_common.has_snapshotting_completed_in_redis_layer
                    = value;
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version %d in "
                                           "snapshotManagerSetMaxSnapshotBufferSizeBytes",
                                        snapshot_manager_info.snapshot_version, 0);
            break;
    }
}

void snapshotManagerIncrementNumItemsAddedToRDB() {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            // nothing to do for this snapshotting version.
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshotV2IncrementNumItemsAddedToRDB(snapshot_manager_info.snapshot_version_two_info);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version %d in "
                                           "snapshotManagerIncrementNumItemsAddedToRDB",
                                        snapshot_manager_info.snapshot_version, 0);
            break;
    }
}

int snapshotManagerIsItemInThreadsaveSnapshotRange(size_t offset) {
    int ret = 0;
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            // nothing to do for this snapshotting version.
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            ret = snapshotV2IsItemInThreadsaveSnapshotRange(snapshot_manager_info.snapshot_version_two_info,
                    offset);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version %d in "
                                           "snapshotManagerIsItemInThreadsaveSnapshotRange",
                                        snapshot_manager_info.snapshot_version, 0);
            break;
    }
    return ret;
}

size_t snapshotManagerGetCountBasedMetric(flashcacheCountBasedMetrics metric) {
    size_t ret = 0;
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            // nothing to do for this snapshotting version.
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            ret = snapshotV2GetCountBasedMetric(snapshot_manager_info.snapshot_version_two_info, metric);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version %d in "
                                           "snapshotManagerGetCountBasedMetric",
                                        snapshot_manager_info.snapshot_version, 0);
            break;
    }
    return ret;
}

void snapshotManagerSetSnapshotKeepAliveMsgIntervalUs(uint64_t value) {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            // nothing to do for this snapshotting version.
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshot_manager_info.snapshot_version_two_info->snapshot_keep_alive_msg_interval_us = value;
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in "
                                           "snapshotManagerSetSnapshotKeepAliveMsgIntervalUs", 0);
            break;
    }
}

void snapshotManagerSetReplicationLinkTimeoutSecs(size_t value) {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            // nothing to do for this snapshotting version.
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshot_manager_info.snapshot_version_two_info->replication_link_timeout_secs = value;
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in "
                                           "snapshotManagerSetReplicationLinkTimeoutSecs", 0);
            break;
    }
}

void snapshotManagerUpdateSnapshottingRangeTailOffset(size_t updated_log_tail_offset) {
    switch (snapshot_manager_info.snapshot_version) {
        case FC_SNAPSHOT_VERSION_ONE:
            // nothing to do for this snapshotting version.
            break;
        case FC_SNAPSHOT_VERSION_TWO:
            snapshotV2UpdateSnapshottingRangeDuringThreadsave(snapshot_manager_info.snapshot_version_two_info,
                                                              updated_log_tail_offset);
            break;
        default:
            // Being here means we have an unsupported version of snapshotting.
            flashcacheAssertWithLogging(0, "Unknown snapshot version in "
                                           "snapshotManagerUpdateSnapshottingRangeTailOffset", 0);
            break;
    }
}
