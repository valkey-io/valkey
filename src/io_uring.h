#ifndef IO_URING_H
#define IO_URING_H
#include <stddef.h>

#define IO_URING_OK 0
#define IO_URING_ERR -1

typedef void (*io_uring_cqe_handler)(void *, int);

int initIOUring(void);
int ioUringPrepWrite(void *data, int fd, const void *buf, size_t len);
int ioUringWaitWriteBarrier(io_uring_cqe_handler cqe_handler);
void freeIOUring(void);

#endif /* IO_URING_H */
