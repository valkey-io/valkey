#ifndef IO_THREADS_H
#define IO_THREADS_H

#include "server.h"

static inline long long timespec_diff_us(struct timespec start_time, struct timespec end_time) {
    return ((long long)end_time.tv_sec - (long long)start_time.tv_sec) * 1000000LL +
           ((long long)end_time.tv_nsec - (long long)start_time.tv_nsec) / 1000LL;
}

void initIOThreads(void);
void killIOThreads(void);
int inMainThread(void);
int trySendReadToIOThreads(client *c);
int trySendWriteToIOThreads(client *c);
int tryOffloadFreeObjToIOThreads(robj *o);
int tryOffloadFreeArgvToIOThreads(client *c, int argc, robj **argv);
void adjustIOThreadsByEventLoad(int numevents, int increase_only);
void drainIOThreadsQueue(void);
void trySendPollJobToIOThreads(void);
int trySendAcceptToIOThreads(connection *conn);
int updateIOThreads(const char **err);
long long getIOThreadUsefulTimeMicroseconds(int id);

#endif /* IO_THREADS_H */
