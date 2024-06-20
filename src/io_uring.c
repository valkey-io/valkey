#include "io_uring.h"

#ifdef HAVE_LIBURING
#include <liburing.h>
#include <string.h>

#include "serverassert.h"
#include "zmalloc.h"

#define IO_URING_DEPTH 256 /* io_uring instance queue depth. */

struct io_uring_context {
    struct io_uring *ring;
    size_t queue_len;
};

io_uring_context *createIOUring(void) {
    struct io_uring_context *uring_context = zmalloc(sizeof(struct io_uring_context));
    struct io_uring_params params;
    struct io_uring *ring = zmalloc(sizeof(struct io_uring));
    memset(&params, 0, sizeof(params));
    /* On success, io_uring_queue_init_params(3) returns 0 and ring will
     * point to the shared memory containing the io_uring queues.
     * On failure -errno is returned. */
    assert(io_uring_queue_init_params(IO_URING_DEPTH, ring, &params) == 0);
    uring_context->ring = ring;
    uring_context->queue_len = 0;
    return uring_context;
}

/* Submit fdatasync request to io_uring. */
int ioUringPrepFsyncAndSubmit(io_uring_context *ring_context, int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring_context->ring);
    io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
    ring_context->queue_len++;
    io_uring_submit(ring_context->ring);
    return IO_URING_OK;
}

int ioUringWaitFsyncBarrier(io_uring_context *ring_context) {
    struct io_uring_cqe *cqe;
    while (ring_context->queue_len) {
        int ret = io_uring_wait_cqe(ring_context->ring, &cqe);
        if (ret == 0) {
            if (cqe->res < 0) {
                return cqe->res;
            }
            io_uring_cqe_seen(ring_context->ring, cqe);
            ring_context->queue_len--;
        } else {
            return ret;
        }
    }
    return IO_URING_OK;
}

void freeIOUring(io_uring_context *ring_context) {
    if (ring_context) {
        io_uring_queue_exit(ring_context->ring);
        zfree(ring_context);
    }
}
#else
#ifndef UNUSED
#define UNUSED(V) ((void)V)
#endif

io_uring_context *createIOUring(void) {
    return 0;
}

int ioUringPrepFsyncAndSubmit(io_uring_context *ring_context, int fd) {
    UNUSED(ring_context);
    UNUSED(fd);
    return 0;
}

int ioUringWaitFsyncBarrier(io_uring_context *ring_context) {
    UNUSED(ring_context);
    return 0;
}

void freeIOUring(io_uring_context *ring_context) {
    UNUSED(ring_context);
}
#endif
