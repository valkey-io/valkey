#include "io_uring.h"

#ifdef HAVE_LIBURING
#include <liburing.h>
#include <string.h>

#include "serverassert.h"
#include "zmalloc.h"

#define IO_URING_DEPTH 256 /* io_uring instance queue depth. */

static size_t io_uring_queue_len = 0;

io_uring *createIOUring(void) {
    struct io_uring_params params;
    struct io_uring *ring = zmalloc(sizeof(struct io_uring));
    memset(&params, 0, sizeof(params));
    /* On success, io_uring_queue_init_params(3) returns 0 and ring will
     * point to the shared memory containing the io_uring queues.
     * On failure -errno is returned. */
    assert(io_uring_queue_init_params(IO_URING_DEPTH, ring, &params) == 0);
    return ring;
}

/* Submit fdatasync request to io_uring. */
int ioUringPrepFsyncAndSubmit(io_uring *ring, int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
    io_uring_queue_len++;
    io_uring_submit(ring);
    return IO_URING_OK;
}

int ioUringWaitFsyncBarrier(io_uring *ring) {
    struct io_uring_cqe *cqe;
    while (io_uring_queue_len) {
        int ret = io_uring_wait_cqe(ring, &cqe);
        if (ret == 0) {
            if (cqe->res < 0) {
                return cqe->res;
            }
            io_uring_cqe_seen(ring, cqe);
            io_uring_queue_len--;
        } else {
            return ret;
        }
    }
    return IO_URING_OK;
}

void freeIOUring(io_uring *ring) {
    if (ring) {
        io_uring_queue_exit(ring);
        zfree(ring);
        ring = NULL;
    }
}
#else
#ifndef UNUSED
#define UNUSED(V) ((void)V)
#endif

io_uring *createIOUring(void) {
    return 0;
}

int ioUringPrepFsyncAndSubmit(io_uring *ring, int fd) {
    UNUSED(ring);
    UNUSED(fd);
    return 0;
}

int ioUringWaitFsyncBarrier(io_uring *ring) {
    UNUSED(ring);
    return 0;
}

void freeIOUring(io_uring *ring) {
    UNUSED(ring);
}
#endif
