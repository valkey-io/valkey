#include "io_uring.h"

#ifdef HAVE_LIBURING
#include <liburing.h>

#define IO_URING_DEPTH 256 /* io_uring instance queue depth. */

static size_t io_uring_queue_len = 0;

void initIOUring(void) {
    if (server.io_uring_enabled) {
        struct io_uring_params params;
        struct io_uring *ring = zmalloc(sizeof(struct io_uring));
        memset(&params, 0, sizeof(params));
        /* On success, io_uring_queue_init_params(3) returns 0 and ring will
         * point to the shared memory containing the io_uring queues.
         * On failure -errno is returned. */
        int ret = io_uring_queue_init_params(IO_URING_DEPTH, ring, &params);
        if (ret != 0) {
            /* Warning if user enable the io_uring in config but system doesn't support yet. */
            serverLog(LL_WARNING, "Failed to initialize io_uring: %s", strerror(-ret));
            zfree(ring);
            server.io_uring = NULL;
        } else {
            serverLog(LL_NOTICE, "io_uring enabled.");
            server.io_uring = ring;
        }
    }
}

int canFsyncUsingIOUring(void) {
    return server.aof_state == AOF_ON && server.aof_fsync == AOF_FSYNC_ALWAYS && server.io_uring_enabled &&
           server.io_uring;
}

/* Submit fdatasync request to io_uring. */
void ioUringPrepFsyncAndSubmit(int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(server.io_uring);
    io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
    io_uring_queue_len++;
    io_uring_submit(server.io_uring);
}

void ioUringWaitFsyncBarrier(void) {
    struct io_uring_cqe *cqe;
    while (io_uring_queue_len) {
        if (io_uring_wait_cqe(server.io_uring, &cqe) == 0) {
            if (cqe->res < 0) {
                serverLog(LL_WARNING,
                          "Can't persist AOF for fsync error when the "
                          "AOF fsync policy is 'always': %s. Exiting...",
                          strerror(-cqe->res));
                exit(1);
            }
            io_uring_cqe_seen(server.io_uring, cqe);
            io_uring_queue_len--;
        } else {
            serverLog(LL_WARNING,
                      "Can't persist AOF for fsync error when the "
                      "AOF fsync policy is 'always': %s. Exiting...",
                      strerror(-cqe->res));
            exit(1);
        }
    }
}

void freeIOUring(void) {
    if (server.io_uring_enabled && server.io_uring) {
        io_uring_queue_exit(server.io_uring);
        zfree(server.io_uring);
        server.io_uring = NULL;
    }
}

#else
void initIOUring(void) {
}

int canFsyncUsingIOUring(void) {
    return 0;
}

void ioUringPrepFsyncAndSubmit(int fd) {
    UNUSED(fd);
}

void ioUringWaitFsyncBarrier(void) {
}

void freeIOUring(void) {
}
#endif
