#ifndef IO_THREADS_H
#define IO_THREADS_H

#include "server.h"

void initIOThreads(void);
void killIOThreads(void);
int inMainThread(void);
int trySendReadToIOThreads(client *c);
int trySendWriteToIOThreads(client *c);
void adjustIOThreadsByEventLoad(int numevents, int increase_only);
void drainIOThreadsQueue(void);

#endif /* IO_THREADS_H */
