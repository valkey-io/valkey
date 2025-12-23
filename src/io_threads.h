#ifndef IO_THREADS_H
#define IO_THREADS_H

#include "server.h"

typedef enum {
    JOB_REQ_READ_CLIENT = 0,
    JOB_REQ_WRITE_CLIENT,
    JOB_REQ_FREE_ARGV,
    JOB_REQ_FREE_OBJ,
    JOB_REQ_POLL,
    JOB_REQ_ACCEPT,
    JOB_REQ_COMMAND,
    JOB_REQ_COUNT
} JobRequest;
_Static_assert(JOB_REQ_COUNT <= 7, "JOB_REQ_COUNT must not exceed 7 for pointer arithmetic");

typedef enum {
    JOB_RES_READ_CLIENT = 0,
    JOB_RES_WRITE_CLIENT,
    JOB_RES_COMMAND,
    JOB_RES_JOBLIST,
    JOB_RES_COUNT
} JobResult;
_Static_assert(JOB_RES_COUNT <= 7, "JOB_RES_COUNT must not exceed 7 for pointer arithmetic");

typedef void (*job_handler)(void *);

/* Per IO thread stats (index = thread ID) */
extern atomic_int io_threads_stat_cmd_cpu[IO_THREADS_MAX_NUM];
extern atomic_int io_threads_stat_io_cpu[IO_THREADS_MAX_NUM];

void initIOThreads(int prev_threads_num);
void killIOThreads(void);
int inMainThread(void);
int trySendReadToIOThreads(client *c);
int trySendWriteToIOThreads(client *c);
int tryOffloadCommandToIOThreads(client *c);
int tryOffloadFreeObjToIOThreads(robj *o);
int tryOffloadFreeArgvToIOThreads(client *c, int argc, robj **argv);
void IOThreadsAfterSleep(int numevents);
void IOThreadsBeforeSleep(long long current_time);
void drainIOThreadsQueue(void);
void trySendPollJobToIOThreads(void);
int trySendAcceptToIOThreads(connection *conn);
int updateIOThreads(const char **err);
int clientHasPendingIO(struct client *c);
int processIOThreadsResponses(void);
int getCurTid(void);
void sendToMainThread(void *data, int type);
int getAverageThreadStat(_Atomic int *stats_array, int active_threads);

#endif /* IO_THREADS_H */
