#include "valkeymodule.h"

#include <string.h>
#include <assert.h>
#include <unistd.h>

typedef struct {
    size_t nkeys;
} scan_strings_pd;

void scan_strings_callback(ValkeyModuleCtx *ctx, ValkeyModuleString* keyname, ValkeyModuleKey* key, void *privdata) {
    scan_strings_pd* pd = privdata;
    int was_opened = 0;
    if (!key) {
        key = ValkeyModule_OpenKey(ctx, keyname, VALKEYMODULE_READ);
        was_opened = 1;
    }

    if (ValkeyModule_KeyType(key) == VALKEYMODULE_KEYTYPE_STRING) {
        size_t len;
        char * data = ValkeyModule_StringDMA(key, &len, VALKEYMODULE_READ);
        ValkeyModule_ReplyWithArray(ctx, 2);
        ValkeyModule_ReplyWithString(ctx, keyname);
        ValkeyModule_ReplyWithStringBuffer(ctx, data, len);
        pd->nkeys++;
    }
    if (was_opened)
        ValkeyModule_CloseKey(key);
}

int scan_strings(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    scan_strings_pd pd = {
        .nkeys = 0,
    };

    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);

    ValkeyModuleScanCursor* cursor = ValkeyModule_ScanCursorCreate();
    while(ValkeyModule_Scan(ctx, cursor, scan_strings_callback, &pd));
    ValkeyModule_ScanCursorDestroy(cursor);

    ValkeyModule_ReplySetArrayLength(ctx, pd.nkeys);
    return VALKEYMODULE_OK;
}

typedef struct {
    ValkeyModuleCtx *ctx;
    size_t nreplies;
} scan_key_pd;

void scan_key_callback(ValkeyModuleKey *key, ValkeyModuleString* field, ValkeyModuleString* value, void *privdata) {
    VALKEYMODULE_NOT_USED(key);
    scan_key_pd* pd = privdata;
    ValkeyModule_ReplyWithArray(pd->ctx, 2);
    size_t fieldCStrLen;

    // The implementation of ValkeyModuleString is robj with lots of encodings.
    // We want to make sure the robj that passes to this callback in
    // String encoded, this is why we use ValkeyModule_StringPtrLen and
    // ValkeyModule_ReplyWithStringBuffer instead of directly use
    // ValkeyModule_ReplyWithString.
    const char* fieldCStr = ValkeyModule_StringPtrLen(field, &fieldCStrLen);
    ValkeyModule_ReplyWithStringBuffer(pd->ctx, fieldCStr, fieldCStrLen);
    if(value){
        size_t valueCStrLen;
        const char* valueCStr = ValkeyModule_StringPtrLen(value, &valueCStrLen);
        ValkeyModule_ReplyWithStringBuffer(pd->ctx, valueCStr, valueCStrLen);
    } else {
        ValkeyModule_ReplyWithNull(pd->ctx);
    }

    pd->nreplies++;
}

int scan_key(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    scan_key_pd pd = {
        .ctx = ctx,
        .nreplies = 0,
    };

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    if (!key) {
        ValkeyModule_ReplyWithError(ctx, "not found");
        return VALKEYMODULE_OK;
    }

    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_ARRAY_LEN);

    ValkeyModuleScanCursor* cursor = ValkeyModule_ScanCursorCreate();
    while(ValkeyModule_ScanKey(key, cursor, scan_key_callback, &pd));
    ValkeyModule_ScanCursorDestroy(cursor);

    ValkeyModule_ReplySetArrayLength(ctx, pd.nreplies);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "scan", 1, VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "scan.scan_strings", scan_strings, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "scan.scan_key", scan_key, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}


