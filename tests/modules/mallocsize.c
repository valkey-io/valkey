#include "valkeymodule.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define UNUSED(V) ((void) V)

/* Registered type */
ValkeyModuleType *mallocsize_type = NULL;

typedef enum {
    UDT_RAW,
    UDT_STRING,
    UDT_DICT
} udt_type_t;

typedef struct {
    void *ptr;
    size_t len;
} raw_t;

typedef struct {
    udt_type_t type;
    union {
        raw_t raw;
        ValkeyModuleString *str;
        ValkeyModuleDict *dict;
    } data;
} udt_t;

void udt_free(void *value) {
    udt_t *udt = value;
    switch (udt->type) {
        case (UDT_RAW): {
            ValkeyModule_Free(udt->data.raw.ptr);
            break;
        }
        case (UDT_STRING): {
            ValkeyModule_FreeString(NULL, udt->data.str);
            break;
        }
        case (UDT_DICT): {
            ValkeyModuleString *dk, *dv;
            ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(udt->data.dict, "^", NULL, 0);
            while((dk = ValkeyModule_DictNext(NULL, iter, (void **)&dv)) != NULL) {
                ValkeyModule_FreeString(NULL, dk);
                ValkeyModule_FreeString(NULL, dv);
            }
            ValkeyModule_DictIteratorStop(iter);
            ValkeyModule_FreeDict(NULL, udt->data.dict);
            break;
        }
    }
    ValkeyModule_Free(udt);
}

void udt_rdb_save(ValkeyModuleIO *rdb, void *value) {
    udt_t *udt = value;
    ValkeyModule_SaveUnsigned(rdb, udt->type);
    switch (udt->type) {
        case (UDT_RAW): {
            ValkeyModule_SaveStringBuffer(rdb, udt->data.raw.ptr, udt->data.raw.len);
            break;
        }
        case (UDT_STRING): {
            ValkeyModule_SaveString(rdb, udt->data.str);
            break;
        }
        case (UDT_DICT): {
            ValkeyModule_SaveUnsigned(rdb, ValkeyModule_DictSize(udt->data.dict));
            ValkeyModuleString *dk, *dv;
            ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(udt->data.dict, "^", NULL, 0);
            while((dk = ValkeyModule_DictNext(NULL, iter, (void **)&dv)) != NULL) {
                ValkeyModule_SaveString(rdb, dk);
                ValkeyModule_SaveString(rdb, dv);
                ValkeyModule_FreeString(NULL, dk); /* Allocated by ValkeyModule_DictNext */
            }
            ValkeyModule_DictIteratorStop(iter);
            break;
        }
    }
}

void *udt_rdb_load(ValkeyModuleIO *rdb, int encver) {
    if (encver != 0)
        return NULL;
    udt_t *udt = ValkeyModule_Alloc(sizeof(*udt));
    udt->type = ValkeyModule_LoadUnsigned(rdb);
    switch (udt->type) {
        case (UDT_RAW): {
            udt->data.raw.ptr = ValkeyModule_LoadStringBuffer(rdb, &udt->data.raw.len);
            break;
        }
        case (UDT_STRING): {
            udt->data.str = ValkeyModule_LoadString(rdb);
            break;
        }
        case (UDT_DICT): {
            long long dict_len = ValkeyModule_LoadUnsigned(rdb);
            udt->data.dict = ValkeyModule_CreateDict(NULL);
            for (int i = 0; i < dict_len; i += 2) {
                ValkeyModuleString *key = ValkeyModule_LoadString(rdb);
                ValkeyModuleString *val = ValkeyModule_LoadString(rdb);
                ValkeyModule_DictSet(udt->data.dict, key, val);
            }
            break;
        }
    }

    return udt;
}

size_t udt_mem_usage(ValkeyModuleKeyOptCtx *ctx, const void *value, size_t sample_size) {
    UNUSED(ctx);
    UNUSED(sample_size);
    
    const udt_t *udt = value;
    size_t size = sizeof(*udt);
    
    switch (udt->type) {
        case (UDT_RAW): {
            size += ValkeyModule_MallocSize(udt->data.raw.ptr);
            break;
        }
        case (UDT_STRING): {
            size += ValkeyModule_MallocSizeString(udt->data.str);
            break;
        }
        case (UDT_DICT): {
            void *dk;
            size_t keylen;
            ValkeyModuleString *dv;
            ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(udt->data.dict, "^", NULL, 0);
            while((dk = ValkeyModule_DictNextC(iter, &keylen, (void **)&dv)) != NULL) {
                size += keylen;
                size += ValkeyModule_MallocSizeString(dv);
            }
            ValkeyModule_DictIteratorStop(iter);
            break;
        }
    }
    
    return size;
}

/* MALLOCSIZE.SETRAW key len */
int cmd_setraw(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3)
        return ValkeyModule_WrongArity(ctx);
        
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);

    udt_t *udt = ValkeyModule_Alloc(sizeof(*udt));
    udt->type = UDT_RAW;
    
    long long raw_len;
    ValkeyModule_StringToLongLong(argv[2], &raw_len);
    udt->data.raw.ptr = ValkeyModule_Alloc(raw_len);
    udt->data.raw.len = raw_len;
    
    ValkeyModule_ModuleTypeSetValue(key, mallocsize_type, udt);
    ValkeyModule_CloseKey(key);

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* MALLOCSIZE.SETSTR key string */
int cmd_setstr(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3)
        return ValkeyModule_WrongArity(ctx);
        
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);

    udt_t *udt = ValkeyModule_Alloc(sizeof(*udt));
    udt->type = UDT_STRING;
    
    udt->data.str = argv[2];
    ValkeyModule_RetainString(ctx, argv[2]);
    
    ValkeyModule_ModuleTypeSetValue(key, mallocsize_type, udt);
    ValkeyModule_CloseKey(key);

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* MALLOCSIZE.SETDICT key field value [field value ...] */
int cmd_setdict(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 4 || argc % 2)
        return ValkeyModule_WrongArity(ctx);
        
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);

    udt_t *udt = ValkeyModule_Alloc(sizeof(*udt));
    udt->type = UDT_DICT;
    
    udt->data.dict = ValkeyModule_CreateDict(ctx);
    for (int i = 2; i < argc; i += 2) {
        ValkeyModule_DictSet(udt->data.dict, argv[i], argv[i+1]);
        /* No need to retain argv[i], it is copied as the rax key */
        ValkeyModule_RetainString(ctx, argv[i+1]);   
    }
    
    ValkeyModule_ModuleTypeSetValue(key, mallocsize_type, udt);
    ValkeyModule_CloseKey(key);

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (ValkeyModule_Init(ctx,"mallocsize",1,VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
        
    ValkeyModuleTypeMethods tm = {
        .version = VALKEYMODULE_TYPE_METHOD_VERSION,
        .rdb_load = udt_rdb_load,
        .rdb_save = udt_rdb_save,
        .free = udt_free,
        .mem_usage2 = udt_mem_usage,
    };

    mallocsize_type = ValkeyModule_CreateDataType(ctx, "allocsize", 0, &tm);
    if (mallocsize_type == NULL)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "mallocsize.setraw", cmd_setraw, "", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
        
    if (ValkeyModule_CreateCommand(ctx, "mallocsize.setstr", cmd_setstr, "", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
        
    if (ValkeyModule_CreateCommand(ctx, "mallocsize.setdict", cmd_setdict, "", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
