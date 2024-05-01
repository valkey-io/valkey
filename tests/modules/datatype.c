/* This module current tests a small subset but should be extended in the future
 * for general ModuleDataType coverage.
 */

/* define macros for having usleep */
#define _BSD_SOURCE
#define _DEFAULT_SOURCE
#include <unistd.h>

#include "valkeymodule.h"

static ValkeyModuleType *datatype = NULL;
static int load_encver = 0;

/* used to test processing events during slow loading */
static volatile int slow_loading = 0;
static volatile int is_in_slow_loading = 0;

#define DATATYPE_ENC_VER 1

typedef struct {
    long long intval;
    ValkeyModuleString *strval;
} DataType;

static void *datatype_load(ValkeyModuleIO *io, int encver) {
    load_encver = encver;
    int intval = ValkeyModule_LoadSigned(io);
    if (ValkeyModule_IsIOError(io)) return NULL;

    ValkeyModuleString *strval = ValkeyModule_LoadString(io);
    if (ValkeyModule_IsIOError(io)) return NULL;

    DataType *dt = (DataType *) ValkeyModule_Alloc(sizeof(DataType));
    dt->intval = intval;
    dt->strval = strval;

    if (slow_loading) {
        ValkeyModuleCtx *ctx = ValkeyModule_GetContextFromIO(io);
        is_in_slow_loading = 1;
        while (slow_loading) {
            ValkeyModule_Yield(ctx, VALKEYMODULE_YIELD_FLAG_CLIENTS, "Slow module operation");
            usleep(1000);
        }
        is_in_slow_loading = 0;
    }

    return dt;
}

static void datatype_save(ValkeyModuleIO *io, void *value) {
    DataType *dt = (DataType *) value;
    ValkeyModule_SaveSigned(io, dt->intval);
    ValkeyModule_SaveString(io, dt->strval);
}

static void datatype_free(void *value) {
    if (value) {
        DataType *dt = (DataType *) value;

        if (dt->strval) ValkeyModule_FreeString(NULL, dt->strval);
        ValkeyModule_Free(dt);
    }
}

static void *datatype_copy(ValkeyModuleString *fromkey, ValkeyModuleString *tokey, const void *value) {
    const DataType *old = value;

    /* Answers to ultimate questions cannot be copied! */
    if (old->intval == 42)
        return NULL;

    DataType *new = (DataType *) ValkeyModule_Alloc(sizeof(DataType));

    new->intval = old->intval;
    new->strval = ValkeyModule_CreateStringFromString(NULL, old->strval);

    /* Breaking the rules here! We return a copy that also includes traces
     * of fromkey/tokey to confirm we get what we expect.
     */
    size_t len;
    const char *str = ValkeyModule_StringPtrLen(fromkey, &len);
    ValkeyModule_StringAppendBuffer(NULL, new->strval, "/", 1);
    ValkeyModule_StringAppendBuffer(NULL, new->strval, str, len);
    ValkeyModule_StringAppendBuffer(NULL, new->strval, "/", 1);
    str = ValkeyModule_StringPtrLen(tokey, &len);
    ValkeyModule_StringAppendBuffer(NULL, new->strval, str, len);

    return new;
}

static int datatype_set(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    long long intval;

    if (ValkeyModule_StringToLongLong(argv[2], &intval) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "Invalid integer value");
        return VALKEYMODULE_OK;
    }

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    DataType *dt = ValkeyModule_Calloc(sizeof(DataType), 1);
    dt->intval = intval;
    dt->strval = argv[3];
    ValkeyModule_RetainString(ctx, dt->strval);

    ValkeyModule_ModuleTypeSetValue(key, datatype, dt);
    ValkeyModule_CloseKey(key);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");

    return VALKEYMODULE_OK;
}

static int datatype_restore(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    long long encver;
    if (ValkeyModule_StringToLongLong(argv[3], &encver) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "Invalid integer value");
        return VALKEYMODULE_OK;
    }

    DataType *dt = ValkeyModule_LoadDataTypeFromStringEncver(argv[2], datatype, encver);
    if (!dt) {
        ValkeyModule_ReplyWithError(ctx, "Invalid data");
        return VALKEYMODULE_OK;
    }

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    ValkeyModule_ModuleTypeSetValue(key, datatype, dt);
    ValkeyModule_CloseKey(key);
    ValkeyModule_ReplyWithLongLong(ctx, load_encver);

    return VALKEYMODULE_OK;
}

static int datatype_get(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    DataType *dt = ValkeyModule_ModuleTypeGetValue(key);
    ValkeyModule_CloseKey(key);

    if (!dt) {
        ValkeyModule_ReplyWithNullArray(ctx);
    } else {
        ValkeyModule_ReplyWithArray(ctx, 2);
        ValkeyModule_ReplyWithLongLong(ctx, dt->intval);
        ValkeyModule_ReplyWithString(ctx, dt->strval);
    }
    return VALKEYMODULE_OK;
}

static int datatype_dump(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    DataType *dt = ValkeyModule_ModuleTypeGetValue(key);
    ValkeyModule_CloseKey(key);

    ValkeyModuleString *reply = ValkeyModule_SaveDataTypeToString(ctx, dt, datatype);
    if (!reply) {
        ValkeyModule_ReplyWithError(ctx, "Failed to save");
        return VALKEYMODULE_OK;
    }

    ValkeyModule_ReplyWithString(ctx, reply);
    ValkeyModule_FreeString(ctx, reply);
    return VALKEYMODULE_OK;
}

static int datatype_swap(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    ValkeyModuleKey *a = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    ValkeyModuleKey *b = ValkeyModule_OpenKey(ctx, argv[2], VALKEYMODULE_WRITE);
    void *val = ValkeyModule_ModuleTypeGetValue(a);

    int error = (ValkeyModule_ModuleTypeReplaceValue(b, datatype, val, &val) == VALKEYMODULE_ERR ||
                 ValkeyModule_ModuleTypeReplaceValue(a, datatype, val, NULL) == VALKEYMODULE_ERR);
    if (!error)
        ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    else
        ValkeyModule_ReplyWithError(ctx, "ERR failed");

    ValkeyModule_CloseKey(a);
    ValkeyModule_CloseKey(b);

    return VALKEYMODULE_OK;
}

/* used to enable or disable slow loading */
static int datatype_slow_loading(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    long long ll;
    if (ValkeyModule_StringToLongLong(argv[1], &ll) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "Invalid integer value");
        return VALKEYMODULE_OK;
    }
    slow_loading = ll;
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

/* used to test if we reached the slow loading code */
static int datatype_is_in_slow_loading(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    ValkeyModule_ReplyWithLongLong(ctx, is_in_slow_loading);
    return VALKEYMODULE_OK;
}

int createDataTypeBlockCheck(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    static ValkeyModuleType *datatype_outside_onload = NULL;

    ValkeyModuleTypeMethods datatype_methods = {
        .version = VALKEYMODULE_TYPE_METHOD_VERSION,
        .rdb_load = datatype_load,
        .rdb_save = datatype_save,
        .free = datatype_free,
        .copy = datatype_copy
    };

    datatype_outside_onload = ValkeyModule_CreateDataType(ctx, "test_dt_outside_onload", 1, &datatype_methods);

    /* This validates that it's not possible to create datatype outside OnLoad,
     * thus returns an error if it succeeds. */
    if (datatype_outside_onload == NULL) {
        ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        ValkeyModule_ReplyWithError(ctx, "UNEXPECTEDOK");
    }
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx,"datatype",DATATYPE_ENC_VER,VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Creates a command which creates a datatype outside OnLoad() function. */
    if (ValkeyModule_CreateCommand(ctx,"block.create.datatype.outside.onload", createDataTypeBlockCheck, "write", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModule_SetModuleOptions(ctx, VALKEYMODULE_OPTIONS_HANDLE_IO_ERRORS);

    ValkeyModuleTypeMethods datatype_methods = {
        .version = VALKEYMODULE_TYPE_METHOD_VERSION,
        .rdb_load = datatype_load,
        .rdb_save = datatype_save,
        .free = datatype_free,
        .copy = datatype_copy
    };

    datatype = ValkeyModule_CreateDataType(ctx, "test___dt", 1, &datatype_methods);
    if (datatype == NULL)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"datatype.set", datatype_set,
                                  "write deny-oom", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"datatype.get", datatype_get,"",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"datatype.restore", datatype_restore,
                                  "write deny-oom", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"datatype.dump", datatype_dump,"",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "datatype.swap", datatype_swap,
                                  "write", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "datatype.slow_loading", datatype_slow_loading,
                                  "allow-loading", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "datatype.is_in_slow_loading", datatype_is_in_slow_loading,
                                  "allow-loading", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
