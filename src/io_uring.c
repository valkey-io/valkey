#include "io_uring.h"

#ifdef HAVE_LIBURING
#include <liburing.h>
#include <string.h>

#include "serverassert.h"
#include "zmalloc.h"

#define IO_URING_DEPTH 256 /* io_uring instance queue depth. */

static size_t io_uring_queue_len = 0;
static io_uring *_io_uring;

void initIOUring(void) {
    struct io_uring_params params;
    struct io_uring *ring = zmalloc(sizeof(struct io_uring));
    memset(&params, 0, sizeof(params));
    /* On success, io_uring_queue_init_params(3) returns 0 and ring will
     * point to the shared memory containing the io_uring queues.
     * On failure -errno is returned. */
    assert(io_uring_queue_init_params(IO_URING_DEPTH, ring, &params) == 0);
    _io_uring = ring;
}

io_uring *getIOUring() {
    if (_io_uring) {
        return _io_uring;
    } else {
        initIOUring();
        return _io_uring;
    }
}

/* Submit fdatasync request to io_uring. */
void ioUringPrepFsyncAndSubmit(io_uring *io_uring, int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(_io_uring);
    io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
    io_uring_queue_len++;
    io_uring_submit(_io_uring);
}

void ioUringWaitFsyncBarrier(io_uring *io_uring) {
    struct io_uring_cqe *cqe;
    while (io_uring_queue_len) {
        if (io_uring_wait_cqe(io_uring, &cqe) == 0) {
            if (cqe->res < 0) {
                char errormsg[1024];
                strerror_r(-cqe->res, errormsg, sizeof(errormsg));
                fprintf(stderr,
                        "Can't persist AOF for fsync error when the AOF fsync policy is 'always': %s. Exiting...",
                        errormsg);
                exit(1);
            }
            io_uring_cqe_seen(io_uring, cqe);
            io_uring_queue_len--;
        } else {
            char errormsg[1024];
            strerror_r(-cqe->res, errormsg, sizeof(errormsg));
            fprintf(stderr, "Can't persist AOF for fsync error when the AOF fsync policy is 'always': %s. Exiting...",
                    errormsg);
            exit(1);
        }
    }
}

void freeIOUring(void) {
    if (_io_uring) {
        io_uring_queue_exit(_io_uring);
        zfree(_io_uring);
        _io_uring = NULL;
    }
}
#else
void initIOUring(void) {
}

io_uring *getIOUring(void) {
    return NULL;
}

void ioUringPrepFsyncAndSubmit(io_uring *io_uring, int fd) {
    UNUSED(fd);
}

void ioUringWaitFsyncBarrier(io_uring *io_uring) {
}

void freeIOUring(void) {
}
#endif
