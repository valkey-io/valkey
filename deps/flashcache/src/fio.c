#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>

#include "include/util.h"
#include "include/fio.h"

#define FC_LOG_FILE_PERMISSION (0644)
#define IS_RETRYABLE_ERROR(err) ((err == -EAGAIN) || (err == -EINTR))
#define FIO_LATENCY_HISTOGRAM_SZ 9

flashcacheHistogramInterval fc_fio_latency_histogram_intervals[] = {
        {0, 100}, {100, 200}, {200, 400}, {400, 1000}, {1000, 2000}, {2000, 10000},
        {10000, 50000}, {50000, 100000}, {100000, LLONG_MAX}};

typedef struct {
    unsigned long long fio_read_latency_histogram[FIO_LATENCY_HISTOGRAM_SZ];
    unsigned long long fio_write_latency_histogram[FIO_LATENCY_HISTOGRAM_SZ];
    unsigned long long num_retryable_disk_error;
} fioStats;

static fioStats fio_stats = {0};

static inline void fioUpdateReadLatencyHistogram(unsigned long long current_value) {
    updateHistogram(current_value, fio_stats.fio_read_latency_histogram,
                    fc_fio_latency_histogram_intervals);
}

static inline void fioUpdateWriteLatencyHistogram(unsigned long long current_value) {
    updateHistogram(current_value, fio_stats.fio_write_latency_histogram,
                    fc_fio_latency_histogram_intervals);
}

static inline void fioUpdateLatencyHistogram(fioOp op, long long current_value) {
    switch (op) {
        case FC_FIO_WRITE:
            fioUpdateWriteLatencyHistogram(current_value);
            break;
        case FC_FIO_READ:
            fioUpdateReadLatencyHistogram(current_value);
            break;
    }
}

fioContext *fioCreateContext(char const *filename, int mode, size_t queue_depth,
                             flashcache_monotonic_clock_us monotonic_clock_us) {
    fioContext *io_context = (fioContext *) fcMalloc(sizeof(fioContext));
    flashcacheAssert(io_context != NULL);

    io_context->filename = (char *) fcMalloc(strlen(filename) + 1);
    flashcacheAssert(io_context->filename != NULL);
    memcpy(io_context->filename, filename, strlen(filename) + 1);

    switch (mode) {
        case FILE_READ_ONLY:
            io_context->fd = open(io_context->filename, O_RDONLY);
            break;
        case FILE_READ_WRITE:
            io_context->fd = open(io_context->filename,
                                O_RDWR | O_DIRECT | O_CREAT, FC_LOG_FILE_PERMISSION);
            break;
        default:
            flashcacheAssertWithLogging(0, "Cannot create fioContext with unknown mode: %d", mode);
    }
    if (io_context->fd == -1) {
        goto err;
    }

    memset(&(io_context->aio_context), 0, sizeof(io_context_t));
    int ret;
    // Retry up to a certain times to create the LinuxAIO context
    int retries = 0;
    while ((ret = io_setup(queue_depth, &(io_context->aio_context))) != 0) {
        flashcacheAssertHandledCrashWithLogging(IS_RETRYABLE_ERROR(ret),
                                    "IO failure with non retryable error [Error code: %d]", ret);
        fio_stats.num_retryable_disk_error++;
        if (++retries > FC_MAX_IO_OPERATION_RETRY) {
            flashcacheAssertWithLogging(0, "io_setup() failed with code %d after %d attempts", ret, retries);
        }
    }

    io_context->queue_depth = queue_depth;
    io_context->num_inflight_request = 0;
    io_context->monotonic_clock_us = monotonic_clock_us;
    io_context->completed_fio_requests = (fioRequest **) fcCalloc(sizeof(fioRequest *), queue_depth);
    flashcacheAssert(io_context->completed_fio_requests != NULL);

    io_context->io_events = (struct io_event *) fcCalloc(sizeof(struct io_event), queue_depth);
    flashcacheAssert(io_context->io_events != NULL);

    return io_context;
err:
    flashcacheLogger(FC_LL_NOTICE, "Failed to read file from '%s', errno:%d %s",
                     io_context->filename, errno, strerror(errno));
    fcFree(io_context->filename);
    fcFree(io_context);
    return NULL;
}

ssize_t fioAllocate(fioContext *io_context, size_t size) {
    return ftruncate(io_context->fd, size);
}

void fioSubmit(fioContext *io_context, fioRequest *data) {
    flashcacheAssert(io_context->num_inflight_request < io_context->queue_depth);

    struct iocb *control_buffers[1];
    struct iocb *control_buffer = &(data->iocb);

    data->start_time_us = io_context->monotonic_clock_us();
    switch (data->op) {
        case FC_FIO_WRITE:
            io_prep_pwrite(control_buffer, io_context->fd, data->data_buffer, data->data_buffer_size,
                    data->offset);
            break;
        case FC_FIO_READ:
            io_prep_pread(control_buffer, io_context->fd, data->data_buffer, data->data_buffer_size,
                    data->offset);
            break;
        default:
            flashcacheAssert(0);
    }

    control_buffer->data = (void *) data;
    control_buffers[0] = control_buffer;

    int retries = 0;
    while (1) {
        flashcacheLogger(FC_LL_DEBUG, "Submitted IO request for file: [%s], fd: [%u], opcode: [%d], offset: [%lld],"
               " size: [%lld]", io_context->filename, control_buffer->aio_fildes, control_buffer->aio_lio_opcode,
               control_buffer->u.c.offset, control_buffer->u.c.nbytes);
        int return_code = io_submit(io_context->aio_context, 1, control_buffers);
        if (return_code == 1) {
            // the IO request was submitted successfully
            io_context->num_inflight_request++;
            return;
        }
        flashcacheAssertHandledCrashWithLogging(IS_RETRYABLE_ERROR(return_code),
                                    "IO failure with non retryable error [Error code: %d]", return_code);
        fio_stats.num_retryable_disk_error++;
        if (++retries > FC_MAX_IO_OPERATION_RETRY) {
            flashcacheAssertWithLogging(0, "io_submit() failed with code %d after %d attempts", return_code, retries);
        }
    }
}

size_t fioGetCompletedRequest(fioContext *io_context, fioRequest ***completed_fio_requests) {
    // Return if there are no request in flight
    if (!io_context->num_inflight_request) {
        return 0;
    }

    int num_events = io_getevents(io_context->aio_context, 0, io_context->queue_depth, io_context->io_events, NULL);
    if (num_events < 0) {
        flashcacheAssertHandledCrashWithLogging(IS_RETRYABLE_ERROR(num_events),
                                    "IO failure with non retryable error [Error code: %d]", num_events);
        fio_stats.num_retryable_disk_error++;
        return 0;
    }

    size_t num_completed_fio_request = 0;
    for (int i = 0; i < num_events; ++i) {
        struct io_event event = io_context->io_events[i];
        struct iocb *control_buffer = event.obj;
        flashcacheLogger(FC_LL_DEBUG, "Received IO event for file: [%s], fd: [%u], opcode: [%d], offset: [%lld],"
               " size: [%llu], res: [%lld], res2: [%llu]", io_context->filename, control_buffer->aio_fildes,
               control_buffer->aio_lio_opcode, control_buffer->u.c.offset, control_buffer->u.c.nbytes,
               (int64_t) event.res, event.res2);
        fioRequest *data = (fioRequest *) event.data;
        data->err_no = 0;

        int64_t ret = (int64_t) event.res;
        flashcacheAssertWithLogging((ret <= (int64_t) data->data_buffer_size),
                "IO failure with non retryable error [Invalid transferred bytes: %lld]", ret);
        flashcacheAssertWithLogging((control_buffer->u.c.nbytes == data->data_buffer_size),
                "IO failure with non retryable error [Invalid buffer size in control buffer, received buffer size: "
                "%llu, expected buffer size: %llu]", control_buffer->u.c.nbytes, data->data_buffer_size);
        flashcacheAssertWithLogging((control_buffer->u.c.offset == (int64_t) data->offset),
                "IO failure with non retryable error [Invalid offset in control buffer, received offset: %lld, "
                "expected offset: %llu]", control_buffer->u.c.offset, data->offset);

        // Handle non-retryable error
        // Res2 errors are treated as non-retryable error
        if (event.res2 != 0) {
            data->err_no = event.res2;
        }
        if (ret < 0 && !IS_RETRYABLE_ERROR(ret)) {
            data->err_no = ret;
        }
        // Retry on partial read/write or retryable error
        if (data->err_no == 0 && ret < (int64_t) data->data_buffer_size) {
            fio_stats.num_retryable_disk_error++;
            io_context->num_inflight_request--;
            fioSubmit(io_context, data);
            continue;
        }

        fioUpdateLatencyHistogram(data->op, io_context->monotonic_clock_us() - data->start_time_us);
        flashcacheAssert(num_completed_fio_request < io_context->queue_depth);
        io_context->completed_fio_requests[num_completed_fio_request++] = data;
    }
    (*completed_fio_requests) = io_context->completed_fio_requests;
    flashcacheAssert(io_context->num_inflight_request >= num_completed_fio_request);
    io_context->num_inflight_request -= num_completed_fio_request;
    return num_completed_fio_request;
}

void fioReleaseContext(fioContext *io_context) {
    if (io_context == NULL) return;

    while (io_context->num_inflight_request > 0) {
        fioRequest **completed_fio_requests;
        size_t num_events = fioGetCompletedRequest(io_context, &completed_fio_requests);

        for (size_t i = 0; i < num_events; ++i) {
            fioRequest *fio_request = completed_fio_requests[i];
            fcFree(fioRequestGetBuffer(fio_request));
            fioRequestClear(fio_request);
        }
    }

    io_destroy(io_context->aio_context);
    close(io_context->fd);
    fcFree(io_context->filename);
    fcFree(io_context->completed_fio_requests);
    fcFree(io_context->io_events);
    fcFree(io_context);
}

size_t fioGetCountBasedMetric(flashcacheCountBasedMetrics metric) {
    switch (metric) {
        case FC_NUM_RETRYABLE_DISK_ERROR:
            return fio_stats.num_retryable_disk_error;
        default:
            flashcacheAssertWithLogging(0, "Unknown metric: [%d]", metric);
    }
    return 0;
}

void fioGetHistogramMetrics(flashcacheHistogramMetrics metric,
                            unsigned long long histogram[], const size_t histogram_size) {
    flashcacheAssert(histogram_size == FIO_LATENCY_HISTOGRAM_SZ);
    switch (metric) {
        case FC_DISK_READ_LATENCY_HISTOGRAM:
            memcpy(histogram, fio_stats.fio_read_latency_histogram,
                   sizeof(unsigned long long) * FIO_LATENCY_HISTOGRAM_SZ);
            break;
        case FC_DISK_WRITE_LATENCY_HISTOGRAM:
            memcpy(histogram, fio_stats.fio_write_latency_histogram,
                   sizeof(unsigned long long) * FIO_LATENCY_HISTOGRAM_SZ);
            break;
        default:
            flashcacheAssertWithLogging(0, "Unknown metric: [%d]", metric);
    }
}

void fioGetHistogramInterval(flashcacheHistogramInterval histogram_interval[],
                             const size_t interval_size) {
    flashcacheAssert(interval_size == FIO_LATENCY_HISTOGRAM_SZ);
    for (unsigned int i = 0; i < interval_size; i++) {
        histogram_interval[i].interval_start = fc_fio_latency_histogram_intervals[i].interval_start;
        histogram_interval[i].interval_end = fc_fio_latency_histogram_intervals[i].interval_end;
    }
}
