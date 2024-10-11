#ifndef __BIGKEYLOG_H__
#define __BIGKEYLOG_H__

#include "server.h"

/* This structure defines an entry inside the bigkey log bucket */
typedef struct bigkeylogEntry {
    robj *key;
    long long num_elements; /* number of elements of the bigkey */
    time_t time;            /* Unix time at which the bigkey was grown. */
} bigkeylogEntry;

/* Exported API */
void bigkeylogInit(void);
void bigkeylogPushEntryIfNeeded(robj *keyobj, long long num_elements);

#endif /* __BIGKEYLOG_H__ */
