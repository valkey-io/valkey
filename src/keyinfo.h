#ifndef __KEYINFO_H__
#define __KEYINFO_H__

#include "server.h"

/* This structure defines an entry inside the bigkey log bucket */
typedef struct keyinfoEntry {
    robj *key;
    long long value;
    time_t time;
} keyinfoEntry;

/* Exported API */
void keyinfoInit(void);

#endif /* __KEYINFO_H__ */
