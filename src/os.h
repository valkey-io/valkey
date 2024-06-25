#ifndef OS_H
#define OS_H

#define IO_THREADS_MAX_NUM 128
#define MAX_THREADS_NUM (IO_THREADS_MAX_NUM + 3 + 1)

#ifndef CACHE_LINE_SIZE
#if defined(__aarch64__) && defined(__APPLE__)
#define CACHE_LINE_SIZE 128
#else
#define CACHE_LINE_SIZE 64
#endif
#endif

#endif /* OS_H */
