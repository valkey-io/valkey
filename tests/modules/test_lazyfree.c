/* This module emulates a linked list for lazyfree testing of modules, which
 is a simplified version of 'hellotype.c'
 */
#include "valkeymodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>

static ValkeyModuleType *LazyFreeLinkType;

struct LazyFreeLinkNode {
    int64_t value;
    struct LazyFreeLinkNode *next;
};

struct LazyFreeLinkObject {
    struct LazyFreeLinkNode *head;
    size_t len; /* Number of elements added. */
};

struct LazyFreeLinkObject *createLazyFreeLinkObject(void) {
    struct LazyFreeLinkObject *o;
    o = ValkeyModule_Alloc(sizeof(*o));
    o->head = NULL;
    o->len = 0;
    return o;
}

void LazyFreeLinkInsert(struct LazyFreeLinkObject *o, int64_t ele) {
    struct LazyFreeLinkNode *next = o->head, *newnode, *prev = NULL;

    while(next && next->value < ele) {
        prev = next;
        next = next->next;
    }
    newnode = ValkeyModule_Alloc(sizeof(*newnode));
    newnode->value = ele;
    newnode->next = next;
    if (prev) {
        prev->next = newnode;
    } else {
        o->head = newnode;
    }
    o->len++;
}

void LazyFreeLinkReleaseObject(struct LazyFreeLinkObject *o) {
    struct LazyFreeLinkNode *cur, *next;
    cur = o->head;
    while(cur) {
        next = cur->next;
        ValkeyModule_Free(cur);
        cur = next;
    }
    ValkeyModule_Free(o);
}

/* LAZYFREELINK.INSERT key value */
int LazyFreeLinkInsert_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    ValkeyModule_AutoMemory(ctx); /* Use automatic memory management. */

    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx,argv[1],
        VALKEYMODULE_READ|VALKEYMODULE_WRITE);
    int type = ValkeyModule_KeyType(key);
    if (type != VALKEYMODULE_KEYTYPE_EMPTY &&
        ValkeyModule_ModuleTypeGetType(key) != LazyFreeLinkType)
    {
        return ValkeyModule_ReplyWithError(ctx,VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }

    long long value;
    if ((ValkeyModule_StringToLongLong(argv[2],&value) != VALKEYMODULE_OK)) {
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid value: must be a signed 64 bit integer");
    }

    struct LazyFreeLinkObject *hto;
    if (type == VALKEYMODULE_KEYTYPE_EMPTY) {
        hto = createLazyFreeLinkObject();
        ValkeyModule_ModuleTypeSetValue(key,LazyFreeLinkType,hto);
    } else {
        hto = ValkeyModule_ModuleTypeGetValue(key);
    }

    LazyFreeLinkInsert(hto,value);
    ValkeyModule_SignalKeyAsReady(ctx,argv[1]);

    ValkeyModule_ReplyWithLongLong(ctx,hto->len);
    ValkeyModule_ReplicateVerbatim(ctx);
    return VALKEYMODULE_OK;
}

/* LAZYFREELINK.LEN key */
int LazyFreeLinkLen_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    ValkeyModule_AutoMemory(ctx); /* Use automatic memory management. */

    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx,argv[1],
                                              VALKEYMODULE_READ);
    int type = ValkeyModule_KeyType(key);
    if (type != VALKEYMODULE_KEYTYPE_EMPTY &&
        ValkeyModule_ModuleTypeGetType(key) != LazyFreeLinkType)
    {
        return ValkeyModule_ReplyWithError(ctx,VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }

    struct LazyFreeLinkObject *hto = ValkeyModule_ModuleTypeGetValue(key);
    ValkeyModule_ReplyWithLongLong(ctx,hto ? hto->len : 0);
    return VALKEYMODULE_OK;
}

void *LazyFreeLinkRdbLoad(ValkeyModuleIO *rdb, int encver) {
    if (encver != 0) {
        return NULL;
    }
    uint64_t elements = ValkeyModule_LoadUnsigned(rdb);
    struct LazyFreeLinkObject *hto = createLazyFreeLinkObject();
    while(elements--) {
        int64_t ele = ValkeyModule_LoadSigned(rdb);
        LazyFreeLinkInsert(hto,ele);
    }
    return hto;
}

void LazyFreeLinkRdbSave(ValkeyModuleIO *rdb, void *value) {
    struct LazyFreeLinkObject *hto = value;
    struct LazyFreeLinkNode *node = hto->head;
    ValkeyModule_SaveUnsigned(rdb,hto->len);
    while(node) {
        ValkeyModule_SaveSigned(rdb,node->value);
        node = node->next;
    }
}

void LazyFreeLinkAofRewrite(ValkeyModuleIO *aof, ValkeyModuleString *key, void *value) {
    struct LazyFreeLinkObject *hto = value;
    struct LazyFreeLinkNode *node = hto->head;
    while(node) {
        ValkeyModule_EmitAOF(aof,"LAZYFREELINK.INSERT","sl",key,node->value);
        node = node->next;
    }
}

void LazyFreeLinkFree(void *value) {
    LazyFreeLinkReleaseObject(value);
}

size_t LazyFreeLinkFreeEffort(ValkeyModuleString *key, const void *value) {
    VALKEYMODULE_NOT_USED(key);
    const struct LazyFreeLinkObject *hto = value;
    return hto->len;
}

void LazyFreeLinkUnlink(ValkeyModuleString *key, const void *value) {
    VALKEYMODULE_NOT_USED(key);
    VALKEYMODULE_NOT_USED(value);
    /* Here you can know which key and value is about to be freed. */
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx,"lazyfreetest",1,VALKEYMODULE_APIVER_1)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    /* We only allow our module to be loaded when the core version is greater than the version of my module */
    if (ValkeyModule_GetTypeMethodVersion() < VALKEYMODULE_TYPE_METHOD_VERSION) {
        return VALKEYMODULE_ERR;
    }

    ValkeyModuleTypeMethods tm = {
        .version = VALKEYMODULE_TYPE_METHOD_VERSION,
        .rdb_load = LazyFreeLinkRdbLoad,
        .rdb_save = LazyFreeLinkRdbSave,
        .aof_rewrite = LazyFreeLinkAofRewrite,
        .free = LazyFreeLinkFree,
        .free_effort = LazyFreeLinkFreeEffort,
        .unlink = LazyFreeLinkUnlink,
    };

    LazyFreeLinkType = ValkeyModule_CreateDataType(ctx,"test_lazy",0,&tm);
    if (LazyFreeLinkType == NULL) return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"lazyfreelink.insert",
        LazyFreeLinkInsert_RedisCommand,"write deny-oom",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"lazyfreelink.len",
        LazyFreeLinkLen_RedisCommand,"readonly",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
