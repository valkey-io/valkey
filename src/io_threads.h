#ifndef IO_THREADS_H
#define IO_THREADS_H

#include <stddef.h> /* For size_t */

struct client;
struct connection;
struct serverObject;

typedef enum {
    R_READ = 0,
    R_COMMAND = 1,
    R_WRITE = 2,
    R_JOBLIST = 3,
    R_LAST = 4,
} jobResponseType;

typedef void (*job_handler)(void *);

void initIOThreads(void);
void killIOThreads(void);
int inMainThread(void);
int trySendReadToIOThreads(struct client *c);
int trySendWriteToIOThreads(struct client *c);
int tryOffloadFreeObjToIOThreads(struct serverObject *o);
int tryOffloadFreeArgvToIOThreads(struct client *c, int argc, struct serverObject **argv);
void adjustIOThreadsByEventLoad(int numevents, int increase_only);
void drainIOThreadsQueue(void);
void trySendPollJobToIOThreads(void);
int trySendAcceptToIOThreads(struct connection *conn);
int trySendProcessCommandToIOThreads(struct client *c);
int processIOThreadsResponses(void);
void threadAddDelayedJob(int slot, job_handler handler, size_t len, void *data);
void threadRespond(struct client *c, jobResponseType r);
int clientIOInProgress(struct client *c);
int postponeClientCommand(struct client *c);
int isServerCronDelayed(void);
void ioThreadsOnUnlinkClient(struct client *c);
void pollIOThreadStats(void);
int isCommandOffloadingRunning(void);
int isCommandOffloadingPaused(void);
void updateLatencyStatsForIOThreads(struct client *c, unsigned long long duration);
void ioThreadUpdateCmdDuration(unsigned long long duration);
int trySendAcceptToIOThreads(struct connection *conn);
int updateIOThreads(const char **err);

#endif /* IO_THREADS_H */
