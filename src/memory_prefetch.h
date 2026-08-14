#ifndef MEMORY_PREFETCH_H
#define MEMORY_PREFETCH_H

struct client;
struct serverDb;
struct serverObject;

void prefetchCommandsBatchInit(void);
void processClientsCommandsBatch(void);
int addCommandToBatchAndProcessIfFull(struct client *c);
void removeClientFromPendingCommandsBatch(struct client *c);
int onMaxBatchSizeChange(const char **err);
void prefetchStringKey(struct serverDb *db, struct serverObject *keyobj);
void prefetchKeyBucketRange(struct serverDb *db, struct serverObject **argv, int start, int end, int stride, int offset);

#endif /* MEMORY_PREFETCH_H */
