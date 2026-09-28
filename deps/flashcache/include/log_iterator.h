#ifndef __FLASHCACHE_LOG_ITERATOR_H
#define __FLASHCACHE_LOG_ITERATOR_H

#include "include/serialization.h"
#include "include/index.h"
#include "include/fio.h"

/**
 * A callback function for executing the logic needed on the fetched item.
 * @param context: A generic parameter defined by the owner while creating the iterator.
 * @param *item: The location of item read by the iterator.
 * @param *index_entry: the current entry as read from the index.
 */
typedef void (*flashcache_iterator_core_logic_processing_callback)(void *context, void *item, void *index_entry);

/**
 * A callback function to be executed before fetching an item. It should return the block size of the item to fetch.
 * @param context: A generic parameter defined by the owner while creating the iterator.
*/
typedef size_t (*flashcache_iterator_pre_processing_callback)(void *context);

/**
 * A callback function to be executed after fetching an item.
 * @param context: A generic parameter defined by the owner while creating the iterator.
 * @param is_running: the state of the iterator.
*/
typedef void (*flashcache_iterator_post_processing_callback)(void *context, size_t is_running);

typedef struct flashcacheLogIteratorCallbacks {
    // The parameter used by the callbacks below.
    void *parameter;

    // The callback responsible for checking if the iterator can read data from the log.
    flashcache_iterator_pre_processing_callback pre_processing_callback;

    // The callback responsible for executing the business logic once an item is fetched from the log.
    flashcache_iterator_core_logic_processing_callback core_logic_processing_callback;

    // The callback to update the status of the iterator after processing an item.
    flashcache_iterator_post_processing_callback post_processing_callback;
} flashcacheLogIteratorCallbacks;

typedef struct flashcacheLogIterator {
    // A reference to the state of the parent job
    uint8_t *is_running;

    // FIO Request to read from log file
    fioRequest log_file_fio_request;

    // IO context to read from log file
    fioContext *log_file_io_context;

    // Number of bytes pending processing from log file
    size_t log_file_pending_processing_bytes;

    // A pointer to the tail offset for the owner's view of the log.
    // This is the offset of last processed item.
    size_t *log_processed_offset;

    // The offset upto which log data has been processed in the log data already read from log file
    size_t log_data_buffer_offset;

    // Set to 1 if there is pending log data (already read from log file) to process else set to 0
    uint8_t pending_log_data_processing;

    // Size (in bytes) of last item that was partially read
    size_t partial_item_size_bytes;

    // A pointer the log size in bytes.
    size_t *log_file_size_bytes_ptr;

    // A pointer to the processed bytes in the current second
    size_t *log_processed_bytes_in_current_second;

    // Maximum amount of time for processing a batch of items.
    size_t max_batch_processing_time_microseconds;

    // Clock
    flashcache_monotonic_clock_us monotonic_clock_us;

    // CRC function used for computing the checksum of data stored in the log.
    flashcache_crc_function crc_function;

    // Reference to the indices for which iterator needs to be read.
    flashcacheIndex **index;

    // A flag to correlate request/completed request to disk.
    size_t user_data;

    // Iterator callbacks.
    flashcacheLogIteratorCallbacks callbacks;
} flashcacheLogIterator;

// The function to trigger the log iterator work.
void logIteratorCron(flashcacheLogIterator *log_iterator);

// Release all the resources created by the log iterator.
void logIteratorRelease(flashcacheLogIterator *log_iterator);

// A function to create an instance of the log iterator.
flashcacheLogIterator *logIteratorCreate(
        flashcacheIndex **index,
        flashcache_monotonic_clock_us monotonic_clock_us,
        flashcache_crc_function crc_function,
        size_t max_batch_processing_time_microseconds,
        size_t user_data,
        size_t *log_file_size_bytes,
        size_t *log_file_tail_offset,
        size_t *log_file_processed_bytes_in_current_second,
        uint8_t *is_running,
        fioContext *log_file_io_context,
        void *parameter,
        flashcache_iterator_pre_processing_callback pre_processing_callback,
        flashcache_iterator_core_logic_processing_callback core_logic_processing_callback,
        flashcache_iterator_post_processing_callback post_processing_callback);
#endif  // __FLASHCACHE_LOG_ITERATOR_H
