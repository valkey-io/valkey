#include "include/snapshot_common.h"


/* Default configuration parameters */
#define FC_SNAPSHOT_LOADING_STAGING_BUFFER_MAXIMUM_SIZE_THRESHOLD \
    ((128 * 1024 * 1024))  // 128 MiB

flashcacheSnapshotConfig flashcache_snapshot_config = {
    FC_SNAPSHOT_LOADING_STAGING_BUFFER_MAXIMUM_SIZE_THRESHOLD,
    FC_SNAPSHOT_LOADING_QUEUE_DEPTH};

fioRequest *getFreeFioData(fioRequest *fio_requests) {
    for (int i = 0; i < FC_SNAPSHOT_NUM_FIO_DATA; ++i) {
        fioRequest *fio_request = &(fio_requests[i]);
        if (!fioRequestGetBuffer(fio_request)) {
            return fio_request;
        }
    }
    // Expectation is to have at least one free FIO data when this function is called
    flashcacheAssertWithLogging(0, "No FIO data and trying to free it", 0);
    return NULL;
}

size_t flushItemFromStagingBuffer(fioContext *io_context, fioRequest *fio_request, stagingBuffer* staging_buffer,
                                  stagingBufferEntry *entry, size_t offset) {
    size_t flushed_item_size = entry->item_len;
    fioRequestFill(fio_request, 0, entry->item, flushed_item_size, offset, FC_FIO_WRITE);
    fioSubmit(io_context, fio_request);

    stagingBufferDeleteEntry(staging_buffer, entry, 0);
    return flushed_item_size;
}

char *readItemBlocking(fioContext *io_context, size_t offset, size_t size) {
    char *buf = createPageAlignedBuffer(size);

    fioRequest fio_request = { 0 };
    fioRequestFill(&fio_request, 0, buf, size, offset, FC_FIO_READ);
    fioSubmit(io_context, &fio_request);

    fioRequest **completed_fio_requests = NULL;
    // Wait for the read request to complete
    while (fioGetCompletedRequest(io_context, &completed_fio_requests) != 1) {
        // Busy spin
    }

    flashcacheAssert((&fio_request) == completed_fio_requests[0]);
    flashcacheAssertHandledCrashWithLogging(fio_request.err_no == 0,
                                "IO failure with non retryable error [Invalid res: %lld]", fio_request.err_no);
    return buf;
}

// Invokes Completion Callback for Snapshot
void invokeCompletionCallback(snapshotCommon *snapshot_common, int completed) {
    if (snapshot_common->snapshot_writer == NULL) {  // invoke callback incase of file based snapshot
        if (snapshot_common->snapshot_file_io_context != NULL) {
            fioReleaseContext(snapshot_common->snapshot_file_io_context);
            snapshot_common->snapshot_file_io_context = NULL;
        }

        // Bail if the the callback is NULL. Callbacks is a parameter from startSave
        // and needs to be checked on every attempt.
        if (snapshot_common->callback_details.callback == NULL) return;

        flashcacheSnapshotCallbackDetails completion_callback_details =
                snapshot_common->callback_details;
        memset(&(snapshot_common->callback_details), 0, sizeof(flashcacheSnapshotCallbackDetails));
        completion_callback_details.callback(completion_callback_details.context, completed);
    } else {  // invoke callback incase of stream based snapshot
        if (snapshot_common->snapshot_writer->complete != NULL) {
            snapshot_common->snapshot_writer->complete(
                    snapshot_common->snapshot_writer->callback_context, completed);
        }
        snapshot_common->snapshot_writer = NULL;
    }
}

// Allocates memory for file/stream based snapshot
ssize_t snapshotAllocateStorage(snapshotCommon *snapshot_common, char const *snapshot_filename,
                                flashcache_monotonic_clock_us monotonic_clock_us,
                                size_t snapshot_file_total_size_bytes) {
    snapshot_common->snapshot_file_io_context = NULL;
    flashcacheSnapshotWriter *snapshot_writer = snapshot_common->snapshot_writer;
    if (snapshot_writer == NULL) {  // Allocates memory incase of file based snapshot
        snapshot_common->snapshot_file_io_context =
                fioCreateContext(snapshot_filename, FILE_READ_WRITE, FC_SNAPSHOT_QUEUE_DEPTH, monotonic_clock_us);
        if (snapshot_common->snapshot_file_io_context == NULL ||
            fioAllocate(snapshot_common->snapshot_file_io_context, snapshot_file_total_size_bytes) != 0) {
            return 1;
        }

    } else {  // Allocates memory incase of stream based snapshot
        snapshot_writer->set_snapshot_size(snapshot_writer->callback_context, snapshot_file_total_size_bytes);
    }
    return 0;
}

