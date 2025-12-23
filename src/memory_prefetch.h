#ifndef MEMORY_PREFETCH_H
#define MEMORY_PREFETCH_H

#include <stddef.h>

struct client;

void prefetchCommandsBatchInit(void);
void processClientsCommandsBatch(void);
int addCommandToBatchAndProcessIfFull(struct client *c);
void removeClientFromPendingCommandsBatch(struct client *c);
int onMaxBatchSizeChange(const char **err);
void processIOThreadClients(struct client **clients, size_t clients_count);
#endif /* MEMORY_PREFETCH_H */
