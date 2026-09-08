#ifndef MEMORY_PREFETCH_H
#define MEMORY_PREFETCH_H

#include "hashtable.h"

struct client;
struct robj;

/* Value prefetch state machine for nested data structures (hashtables, etc.) */
typedef enum {
    HASHTABLE_PREFETCH_ENTRY, /* Initial state, prefetch hashtable pointer */
    HASHTABLE_PREFETCH_INIT,  /* Initialize incremental find in the hashtable */
    HASHTABLE_PREFETCH_VALUE, /* Step through incremental find in the hashtable */
} HashtablePrefetchState;

typedef struct {
    HashtablePrefetchState state;
    union {
        hashtableIncrementalFindState hashtab_state;
    } data;
} ValuePrefetchInfo;

void prefetchCommandsBatchInit(void);
void processClientsCommandsBatch(void);
int addCommandToBatchAndProcessIfFull(struct client *c);
void removeClientFromPendingCommandsBatch(struct client *c);
int onMaxBatchSizeChange(const char **err);

#endif /* MEMORY_PREFETCH_H */
