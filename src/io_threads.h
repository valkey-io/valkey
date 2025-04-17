#ifndef IO_THREADS_H
#define IO_THREADS_H

#include "server.h"

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

#endif /* IO_THREADS_H */
