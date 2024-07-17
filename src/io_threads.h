#ifndef IO_THREADS_H
#define IO_THREADS_H

#include "server.h"

void initIOThreads(void);
void killIOThreads(void);
int inMainThread(void);
int trySendReadToIOThreads(client *c);
int trySendWriteToIOThreads(client *c);
void adjustIOThreadsByEventLoad(int numevents, int increase_only);

#endif /* IO_THREADS_H */
