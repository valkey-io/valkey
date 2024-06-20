#ifndef IO_URING_H
#define IO_URING_H

typedef struct io_uring_context io_uring_context; /* opaque */

#define IO_URING_OK 0

/* Initialize io_uring at server startup if have io_uring configured,
 * setup io_uring submission and completion. */
io_uring_context *createIOUring(void);

/* Submit fsync to io_uring submission queue. */
int ioUringPrepFsyncAndSubmit(io_uring_context *uring_instance, int fd);

/* This function will wait for asynchronous fsync to complete. */
int ioUringWaitFsyncBarrier(io_uring_context *uring_instance);

/* Free io_uring. */
void freeIOUring(io_uring_context *uring_instance);

#endif /* IO_URING_H */
