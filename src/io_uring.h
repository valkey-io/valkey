#ifndef IO_URING_H
#define IO_URING_H

#include "server.h"

/* Initialize io_uring at server startup if have io_uring configured,
 * setup io_uring submission and completion. */
void initIOUring(void);

/* To check if server is sutiable for io_uring to do the fsync work. */
int canFsyncUsingIOUring(void);

/* Submit fsync to io_uring submission queue. */
void ioUringPrepFsyncAndSubmit(int fd);

/* This function will wait for asynchronous fsync to complete. */
void ioUringWaitFsyncBarrier(void);

/* Free io_uring. */
void freeIOUring(void);

#endif /* IO_URING_H */
