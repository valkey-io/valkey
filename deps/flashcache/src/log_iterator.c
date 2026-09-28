#include <stdio.h>
#include "include/util.h"
#include "include/log_iterator.h"
#include "include/serialization.h"

/**
 * The log iterator provides a generic way to iterate through all items currently stored in the log.
 * It offers a cron job (logIteratorCron) that fetches items from the log as needed. The cron job performs
 * two actions: (a) place a read request from the log (readItem) and (b) when the item is read apply the
 * logic introduced by the owner via callbacks.
 *
 * To use the log iterator the owner needs to create 3 callback functions:
 * 1- flashcache_iterator_pre_processing_callback: Check if reading the next item should proceed and returns the
 *                block size to read. This callback is mandatory.
 * 2- flashcache_iterator_core_logic_processing_callback: execute's the caller's logic on the item fetched from
 *                the log. This callback is mandatory.
 * 3- flashcache_iterator_post_processing_callback: performs any post processing needed after the current cycle
 *                the log iterator completes. This callback is optional.
 */

static void processReadItem(flashcacheLogIterator *log_iterator) {
    if (!(*log_iterator->is_running && log_iterator->pending_log_data_processing)) return;

    // Steps to follow:
    // 1. Traverse the log data queue
    // 2. Parse the item and check if it is present in the index
    // 3. Call the owner's core logic callback for processing.
    flashcacheAssert(!fioRequestIsEmpty(&(log_iterator->log_file_fio_request)));
    char *buf = fioRequestGetBuffer(&(log_iterator->log_file_fio_request));
    size_t buf_size = fioRequestGetBufferSize(&(log_iterator->log_file_fio_request));
    uint64_t processing_start_time = log_iterator->monotonic_clock_us();
    size_t offset = log_iterator->log_data_buffer_offset;
    size_t collected_bytes = 0;

    while (offset < buf_size) {
        // Header is partially read so the last cannot be processed. Bailing out.
        if (buf_size - offset < FC_ITEM_HEADER_LEN) {
            log_iterator->partial_item_size_bytes = 2 * FC_PAGESIZE;
            break;
        }

        char *item = buf + offset;
        flashcacheAssert(validateHeaderInSerializedItem(item, log_iterator->crc_function));
        uint32_t dbid = extractDbidFromSerializedItem(item);
        uint32_t item_flag = getFlagInSerializedItem(item);

        size_t total_item_len = extractTotalLenFromSerializedItem(item);
        if (offset + total_item_len > buf_size) {
            log_iterator->partial_item_size_bytes =
                    getPartialItemReadSizeBytes(total_item_len, *log_iterator->log_processed_offset);
            break;
        }

        if (item_flag != FC_SKIP_SEGMENT) {
            flashcacheAssert(validateKeyInSerializedItem(item, log_iterator->crc_function));
            char *key = NULL;
            size_t key_len = 0;
            extractKeyFromSerializedItem(item, &key, &key_len);
            flashcacheAssert(key != NULL);

            indexEntry *index_entry = indexGetItem(log_iterator->index[dbid], key,
                                                   key_len,
                                                   *log_iterator->log_processed_offset);
            if (index_entry) {
                log_iterator->callbacks.core_logic_processing_callback(log_iterator->callbacks.parameter,
                                                                       item,
                                                                       index_entry);
            }
        }

        size_t last_offset = offset;
        offset += total_item_len;
        if (item_flag == FC_LAST_ITEM_BEFORE_NEXT_PAGE_BOUNDARY) {
            offset = getCeilPageAlignedOffset(offset);
        }

        size_t change_in_offset = (offset - last_offset);
        collected_bytes += change_in_offset;
        *log_iterator->log_processed_offset += change_in_offset;
        log_iterator->log_file_pending_processing_bytes -= change_in_offset;

        if ((log_iterator->monotonic_clock_us() - processing_start_time) >
             log_iterator->max_batch_processing_time_microseconds) {
            *log_iterator->log_processed_offset %= *log_iterator->log_file_size_bytes_ptr;
            log_iterator->log_data_buffer_offset += collected_bytes;
            *log_iterator->log_processed_bytes_in_current_second += collected_bytes;
            return;
        }
    }

    *log_iterator->log_processed_bytes_in_current_second += collected_bytes;
    *log_iterator->log_processed_offset %= *log_iterator->log_file_size_bytes_ptr;
    if (log_iterator->callbacks.post_processing_callback != NULL) {
        log_iterator->callbacks.post_processing_callback(log_iterator->callbacks.parameter, 0);
    }
    log_iterator->log_data_buffer_offset = 0;
    log_iterator->pending_log_data_processing = 0;

    fcFree(buf);
    fioRequestClear(&(log_iterator->log_file_fio_request));
}

static void readItem(flashcacheLogIterator *log_iterator) {
    size_t buf_size = log_iterator->callbacks.pre_processing_callback(log_iterator->callbacks.parameter);
    if (!buf_size) {
        return;
    }

    char *buf = createPageAlignedBuffer(buf_size);
    size_t read_offset = getFloorPageAlignedOffset(*log_iterator->log_processed_offset);
    fioRequestFill(&(log_iterator->log_file_fio_request), log_iterator->user_data,
                   buf, buf_size, read_offset, FC_FIO_READ);
    fioSubmit(log_iterator->log_file_io_context, &(log_iterator->log_file_fio_request));

    *log_iterator->is_running = 1;
    log_iterator->log_data_buffer_offset = (*log_iterator->log_processed_offset - read_offset);
    log_iterator->partial_item_size_bytes = 0;
}

void logIteratorCron(flashcacheLogIterator *log_iterator) {
    processReadItem(log_iterator);
    readItem(log_iterator);
}

void logIteratorRelease(flashcacheLogIterator *log_iterator) {
    fioRequestClear(&(log_iterator->log_file_fio_request));
    fcFree(log_iterator);
}

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
        flashcache_iterator_post_processing_callback post_processing_callback) {
    flashcacheAssert(parameter != NULL);
    flashcacheAssert(pre_processing_callback != NULL);
    flashcacheAssert(core_logic_processing_callback != NULL);

    flashcacheLogIterator *log_iterator = (flashcacheLogIterator *) fcMalloc(sizeof(flashcacheLogIterator));
    log_iterator->max_batch_processing_time_microseconds =
            max_batch_processing_time_microseconds;
    log_iterator->user_data = user_data;
    log_iterator->monotonic_clock_us = monotonic_clock_us;
    log_iterator->index = index;
    log_iterator->pending_log_data_processing = 0;
    log_iterator->crc_function = crc_function;
    log_iterator->log_file_size_bytes_ptr = log_file_size_bytes;
    log_iterator->log_processed_offset = log_file_tail_offset;
    log_iterator->partial_item_size_bytes = 0;
    log_iterator->log_file_io_context = log_file_io_context;
    log_iterator->is_running = is_running;
    log_iterator->log_file_pending_processing_bytes = 0;
    log_iterator->log_processed_bytes_in_current_second = log_file_processed_bytes_in_current_second;

    // Create callbacks.
    log_iterator->callbacks.parameter = parameter;
    log_iterator->callbacks.core_logic_processing_callback = core_logic_processing_callback;
    log_iterator->callbacks.pre_processing_callback = pre_processing_callback;
    log_iterator->callbacks.post_processing_callback = post_processing_callback;

    // Start with empty requests.
    fioRequestClear(&(log_iterator->log_file_fio_request));

    return log_iterator;
}
