#include "server.h"
#include "bgiteration.h"
#include "hashtable.h"

void amzUnblockClientsOnKey(void *info, robj *key) {
    UNUSED(info);
    UNUSED(key);
}

int amzBlockClientOnKeys(void *info, client *c, robj *keys[], int nKeys) {
    UNUSED(info);
    UNUSED(c);
    UNUSED(keys);
    UNUSED(nKeys);
    return C_OK;
}
