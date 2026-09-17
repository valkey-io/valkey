#include "valkeymodule.h"
#include <string.h>
#include <stdio.h>

/* When armed, the dump callback attaches 'armed_value' as this module's
 * per-key metadata for every key. The restore callback records the last
 * metadata it received so the test can read it back, and rejects the special
 * value "REJECT" to exercise the restore-failure path. */
static int armed = 0;
static char armed_value[256];
static char last_meta[256];
static char last_key[256];

static ValkeyModuleString *dumpMeta(ValkeyModuleCtx *ctx, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(key);
    if (!armed) return NULL;
    return ValkeyModule_CreateString(ctx, armed_value, strlen(armed_value));
}

static int restoreMeta(ValkeyModuleCtx *ctx, ValkeyModuleString *key, ValkeyModuleString *metadata) {
    size_t klen, mlen;
    const char *k = ValkeyModule_StringPtrLen(key, &klen);
    const char *m = ValkeyModule_StringPtrLen(metadata, &mlen);
    snprintf(last_key, sizeof(last_key), "%.*s", (int)klen, k);
    snprintf(last_meta, sizeof(last_meta), "%.*s", (int)mlen, m);
    int reject = (mlen == 6 && !memcmp(m, "REJECT", 6));
    ValkeyModule_FreeString(ctx, metadata);
    return reject ? VALKEYMODULE_ERR : VALKEYMODULE_OK;
}

static int Arm(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    size_t len;
    const char *v = ValkeyModule_StringPtrLen(argv[1], &len);
    snprintf(armed_value, sizeof(armed_value), "%.*s", (int)len, v);
    armed = 1;
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int Disarm(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    armed = 0;
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int LastMeta(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    return ValkeyModule_ReplyWithCString(ctx, last_meta);
}

static int LastKey(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    return ValkeyModule_ReplyWithCString(ctx, last_key);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "keymetadata", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_RegisterKeyMetadata(ctx, dumpMeta, restoreMeta) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "keymetadata.arm", Arm, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "keymetadata.disarm", Disarm, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "keymetadata.lastmeta", LastMeta, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "keymetadata.lastkey", LastKey, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    return VALKEYMODULE_OK;
}
