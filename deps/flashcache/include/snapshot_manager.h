#ifndef __FLASHCACHE_SNAPSHOT_MANAGER_H
#define __FLASHCACHE_SNAPSHOT_MANAGER_H

#include "include/snapshot_common.h"
#include <stdio.h>

typedef struct snapshotManagerInfo {
    // The versions of the active snapshotting algorithm.
    flashcacheSnapshotVersion snapshot_version;

    // A union for the snapshot version structs. We should only have version active at a time.
    // The structs are forward declarated to keep the version specific implementation hidden in snapshot_manager.c.
    union {
        struct snapshotVersionOneInfo *snapshot_version_one_info;
        struct snapshotVersionTwoInfo *snapshot_version_two_info;
    };
} snapshotManagerInfo;

// Creates snapshot manager info that is used for snapshotting
void snapshotManagerInfoCreate(char const *log_filename,
                               uint32_t num_databases,
                               size_t log_file_size_bytes,
                               flashcacheIndex **index,
                               struct logMetrics *log_metrics,
                               flashcache_monotonic_clock_us monotonic_clock_us,
                               flashcache_crc_function crc_function);

// Frees the resources used by the snapshot manager.
void snapshotManagerInfoRelease();

// Start writing snapshot to the specified file or stream using the active snapshot version.
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
                              flashcacheLogIterationCallbackDetails *log_iteration_completion_callback_details);

// Snapshotting process is incremental. This task ensure that the next batch of work required for current ongoing
// snapshot save is done. This function ensures the cron job of the active snapshot version is the only one called.
// This function returns with no action if there is no snapshot save running at the time of invocation.
void snapshotManagerCronTask();

// Loads the snapshot into the supplied log instance
void snapshotManagerLoad(struct flashcacheLog *log,
                         char const *snapshot_filename,
                         flashcacheSnapshotSecret *secret_response,
                         int *checksum_comparison_result);

// Return 1 if there is a snapshot save in progress and it has not yet completed reading the log file else returns 0
int snapshotManagerIsLogReadingInProgress();

// Cancels the ongoing snapshot save
void snapshotManagerCancelSave();

// Returns the amount of data that has been copied from the log by the current active snapshotting algorithm.
size_t snapshotManagerGetLogFileProcessedOffset();

// Returns 1 if the active snapshot version is currently in progress.
int snapshotManagerIsRunning();

// Returns 1 if snapshot version 1 is running.
int snapshotManagerIsSnapshotV1Running();

// Set the maximum amount of data that can be buffered in memory while waiting to be written to the snapshot.
void snapshotManagerSetMaxSnapshotBufferSizeBytes(size_t value);

// Get the maximum amount of data that can be buffered in memory while waiting to be written to the snapshot.
size_t snapshotManagerGetMaxSnapshotBufferSizeBytes();

// Expedite adding an item to the snapshot.
void snapshotManagerAddExpeditedItem(size_t offset, char *item, size_t item_size);

// Checks if an item will be expedited to the FDB snapshot
int snapshotManagerShouldExpediteItem(size_t offset);

// Add replication commands to the snapshot.
void snapshotManagerAddReplicationCommandIfRequired(size_t offset, uint32_t dbid, char const *key,
                                                    size_t key_len, char const *value, size_t value_len,
                                                    flashcache_crc_function crc_function);

// Set the flag (has_snapshotting_completed_in_engine_layer) once engine layer snapshotting is completed
void snapshotManagerSetHasSnapshottingCompletedInEngineLayer(uint8_t value);

// Increments the snapshot manager's tracker for number of items added to the RDB
void snapshotManagerIncrementNumItemsAddedToRDB();

// Returns 1 if the item is in an active forkless save's snapshot range
int snapshotManagerIsItemInForklessSaveSnapshotRange(size_t offset);

// Get metric from the current snapshot
size_t snapshotManagerGetCountBasedMetric(flashcacheCountBasedMetrics metric);

// Set the time interval for keep alive messages which are sent to keep
// the replication link alive until engine layer snapshotting is completed
void snapshotManagerSetSnapshotKeepAliveMsgIntervalUs(uint64_t value);

// Set the timeout that the replication link will stay up to wait for engine layer
// snapshotting to complete
void snapshotManagerSetReplicationLinkTimeoutSecs(size_t value);

// Updates the Snapshotting range after eviction in flash during forkless save replication.
void snapshotManagerUpdateSnapshottingRangeTailOffset(size_t updated_log_tail_offset);

#endif  // __FLASHCACHE_SNAPSHOT_MANAGER_H
