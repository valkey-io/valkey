#include "io_uring.h"

#ifdef HAVE_LIBURING
#include <liburing.h>
#include <string.h>
#include "zmalloc.h"

/* io_uring instance queue depth. */
#define IO_URING_DEPTH 256

static struct io_uring *_io_uring;
static size_t io_uring_write_queue_len = 0;

/* Initialize io_uring at server startup if io_uring enabled,
 * setup io_uring submission and completion. */
int initIOUring(void) {
    struct io_uring_params params;
    _io_uring = zmalloc(sizeof(struct io_uring));
    memset(&params, 0, sizeof(params));
    /* On success, io_uring_queue_init_params(3) returns 0 and _io_uring will
     * point to the shared memory containing the io_uring queues.
     * On failure -errno is returned. */
    if (io_uring_queue_init_params(IO_URING_DEPTH, _io_uring, &params) < 0) return IO_URING_ERR;
    return IO_URING_OK;
}

/* Use io_uring to handle the client write request. */
int ioUringPrepWrite(void *data, int fd, const void *buf, size_t len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(_io_uring);
    if (sqe == NULL) return IO_URING_ERR;
    io_uring_prep_send(sqe, fd, buf, len, MSG_DONTWAIT);
    io_uring_sqe_set_data(sqe, data);
    io_uring_write_queue_len++;
    return IO_URING_OK;
}

/* Submit requests to the submission queue and wait for completion. */
int ioUringWaitWriteBarrier(io_uring_cqe_handler cqe_handler) {
    if (io_uring_submit(_io_uring) < 0) return IO_URING_ERR;
    while (io_uring_write_queue_len) {
        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(_io_uring, &cqe);
        if (ret == 0) {
            if (cqe_handler) {
                void *data = io_uring_cqe_get_data(cqe);
                cqe_handler(data, cqe->res);
            }
            io_uring_cqe_seen(_io_uring, cqe);
            io_uring_write_queue_len--;
        } else {
            return IO_URING_ERR;
        }
    }
    return IO_URING_OK;
}

/* Free io_uring. */
void freeIOUring(void) {
    io_uring_queue_exit(_io_uring);
    zfree(_io_uring);
    _io_uring = NULL;
}
#else
#ifndef UNUSED
#define UNUSED(V) ((void)V)
#endif

int initIOUring(void) {
    return 0;
}

int ioUringPrepWrite(void *data, int fd, const void *buf, size_t len) {
    UNUSED(data);
    UNUSED(fd);
    UNUSED(buf);
    UNUSED(len);
    return 0;
}

int ioUringWaitWriteBarrier(io_uring_cqe_handler cqe_handler) {
    UNUSED(cqe_handler);
    return 0;
}

void freeIOUring(void) {
}

#endif
