#ifndef MAA_H
#define MAA_H

struct client;

void onMaxBatchSizeChange(void);
void prefetchCommandsBatchInit(void);
void processClientsCommandsBatch(void);
int addCommandToBatchAndProcessIfFull(struct client *c);
void removeClientFromPendingCommandsBatch(struct client *c);

#endif /* MAA_H */
