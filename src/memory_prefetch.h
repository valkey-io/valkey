#ifndef MEMORY_PREFETCH_H
#define MEMORY_PREFETCH_H

struct client;
struct aeEventLoop;

void prefetchCommandsBatchInit(void);
void processClientsCommandsBatch(void);
int addCommandToBatchAndProcessIfFull(struct client *c);
void removeClientFromPendingCommandsBatch(struct client *c);
void prefetchEvents(struct aeEventLoop *eventLoop, int cur_idx, int numevents);

#endif /* MEMORY_PREFETCH_H */
