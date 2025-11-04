#ifndef __KEYINFO_H__
#define __KEYINFO_H__

#include "server.h"

typedef struct bigkeyEntry {
    long long value;
    robj *key;
} bigkeyEntry;

/* Exported API */
void bigkeyListInit(void);


#endif /* __KEYINFO_H__ */
