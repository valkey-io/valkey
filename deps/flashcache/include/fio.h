#ifndef __FLASHCACHE_FIO_H
#define __FLASHCACHE_FIO_H

#include <string.h>

#include "include/util.h"
#include "include/libaio.h"
#include "include/flashcache_common.h"

#define FC_MAX_IO_OPERATION_RETRY 100

typedef enum {
    FILE_READ_WRITE,
    FILE_READ_ONLY
} fileOpenMode;

typedef enum {
    FC_FIO_READ,
    FC_FIO_WRITE
} fioOp;

struct fioContext;

typedef struct fioRequest {
    // User data used associated with the data. This data can be used by caller to keep some state associated with the
    // IO request
    size_t user_data;

    // Set to READ for read operation and WRITE for write operation
    fioOp op;

    // The data buffer used for reading/writing data from disk
    char *data_buffer;

    // Size of data buffer used for reading/writing data from disk
    size_t data_buffer_size;

    // Offset in the log file from where the data is being read or written
    size_t offset;

    // IO context buffer used for the reading/writing data from disk
    struct iocb iocb;

    // Time at which fio request gets submitted
    unsigned long long start_time_us;

    // err no indicating whether the request was processed successfully. A value of 0 indicate success anything else
    // indicates failure
    ssize_t err_no;
} fioRequest;

typedef struct fioContext {
    // AIO context used for reading and writing from disk
    io_context_t aio_context;

    // Fd of file used to read/write data
    int fd;

    // Depth of IO queue
    size_t queue_depth;

    // Name of the file used to read/write data
    char *filename;

    // Array containing the completed io datas used to share completed io datas with the caller
    // of fioGetCompletedRequest
    fioRequest **completed_fio_requests;

    // IO events array used for retrieving completed io_event
    struct io_event *io_events;

    // Number of request in flight
    size_t num_inflight_request;

    // Clock used for computing elapsed time
    flashcache_monotonic_clock_us monotonic_clock_us;
} fioContext;

static inline int fioRequestIsEmpty(fioRequest *data) {
    return (data->data_buffer == NULL);
}

static inline char *fioRequestGetBuffer(fioRequest *data) {
    return data->data_buffer;
}

static inline int fioRequestGetBufferSize(fioRequest *data) {
    return data->data_buffer_size;
}

static inline void fioRequestFill(fioRequest *data, size_t user_data, char *buf, size_t buf_len,
        size_t offset, fioOp op) {
    flashcacheAssert(buf_len > 0);
    data->user_data = user_data;
    data->offset = offset;
    data->op = op;
    data->data_buffer = buf;
    data->data_buffer_size = buf_len;
}

static inline void fioRequestClear(fioRequest *data) {
    memset(data, 0, sizeof(fioRequest));
}

// Setup IO context used for reading and writing from the specified file
fioContext *fioCreateContext(char const *filename, int mode, size_t queue_depth,
                             flashcache_monotonic_clock_us monotonic_clock_us);

// Set the size of file to the specified size. Returns 0 on success else returns a non-zero number
ssize_t fioAllocate(fioContext *io_context, size_t size);

// Submits IO requests for reading and writing from disk
void fioSubmit(fioContext *io_context, struct fioRequest *data);

// Retrieve the File IO data of the completed IO request, returns the number of completed requests. The
// completed_fio_requests contains pointer to a structure that is reused in the next fioGetCompletedRequest call so it
// should be consumed before calling fioGetCompletedRequest API again.
size_t fioGetCompletedRequest(fioContext *io_context, fioRequest ***completed_fio_requests);

// Releases the IO context and all the memory allocated while creating it like filename etc
void fioReleaseContext(fioContext *io_context);

// Fetches count based metric
size_t fioGetCountBasedMetric(flashcacheCountBasedMetrics metric);

// Fetches histogram for a given metric.
void fioGetHistogramMetrics(flashcacheHistogramMetrics metric,
                            unsigned long long histogram[], const size_t histogram_size);

// Fetches histogram interval.
void fioGetHistogramInterval(flashcacheHistogramInterval histogram_interval[],
                             const size_t interval_size);

#endif  // __FLASHCACHE_FIO_H
