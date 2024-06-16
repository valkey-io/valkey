#ifndef IO_URING_H
#define IO_URING_H

typedef struct io_uring io_uring; /* opaque */

/* Initialize io_uring at server startup if have io_uring configured,
 * setup io_uring submission and completion. */
void initIOUring(void);

io_uring *getIOUring(void);

/* Submit fsync to io_uring submission queue. */
void ioUringPrepFsyncAndSubmit(io_uring *io_uring, int fd);

/* This function will wait for asynchronous fsync to complete. */
void ioUringWaitFsyncBarrier(io_uring *io_uring);

/* Free io_uring. */
void freeIOUring(void);

#endif /* IO_URING_H */
