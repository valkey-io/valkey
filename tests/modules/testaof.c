#include "valkeymodule.h"

#include <string.h>
#include <assert.h>

/* Configuration flags, passed as module argument. */
#define CONF_AOF_BEFORE_KEYSPACE (1 << 0)
#define CONF_AOF_AFTER_KEYSPACE  (1 << 1)

static long long conf_flags = 0;

static ValkeyModuleString *before_str = NULL;
static ValkeyModuleString *after_str = NULL;

static ValkeyModuleType *testaof_type = NULL;

/* --- commands to get/set the aux globals --- */

int testaof_set_before(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    if (before_str) ValkeyModule_FreeString(ctx, before_str);
    before_str = argv[1];
    ValkeyModule_RetainString(ctx, argv[1]);
    ValkeyModule_ReplyWithLongLong(ctx, 1);
    return VALKEYMODULE_OK;
}

int testaof_get_before(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    if (before_str)
        ValkeyModule_ReplyWithString(ctx, before_str);
    else
        ValkeyModule_ReplyWithStringBuffer(ctx, "", 0);
    return VALKEYMODULE_OK;
}

int testaof_set_after(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    if (after_str) ValkeyModule_FreeString(ctx, after_str);
    after_str = argv[1];
    ValkeyModule_RetainString(ctx, argv[1]);
    ValkeyModule_ReplyWithLongLong(ctx, 1);
    return VALKEYMODULE_OK;
}

int testaof_get_after(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    if (after_str)
        ValkeyModule_ReplyWithString(ctx, after_str);
    else
        ValkeyModule_ReplyWithStringBuffer(ctx, "", 0);
    return VALKEYMODULE_OK;
}

/* --- AOF aux callbacks --- */

void testaof_aux_save(ValkeyModuleIO *aof, int when) {
    if (when == VALKEYMODULE_AUX_BEFORE_AOF) {
        if (before_str) {
            ValkeyModule_SaveSigned(aof, 1);
            ValkeyModule_SaveString(aof, before_str);
        }
        /* Write nothing if no data; the entry will be skipped. */
    } else {
        if (after_str) {
            ValkeyModule_SaveSigned(aof, 1);
            ValkeyModule_SaveString(aof, after_str);
        }
    }
}

int testaof_aux_load(ValkeyModuleIO *aof, int encver, int when) {
    assert(encver == 1);
    ValkeyModuleCtx *ctx = ValkeyModule_GetContextFromIO(aof);
    if (when == VALKEYMODULE_AUX_BEFORE_AOF) {
        if (before_str) ValkeyModule_FreeString(ctx, before_str);
        before_str = NULL;
        int count = ValkeyModule_LoadSigned(aof);
        if (ValkeyModule_IsIOError(aof)) return VALKEYMODULE_ERR;
        if (count) before_str = ValkeyModule_LoadString(aof);
    } else {
        if (after_str) ValkeyModule_FreeString(ctx, after_str);
        after_str = NULL;
        int count = ValkeyModule_LoadSigned(aof);
        if (ValkeyModule_IsIOError(aof)) return VALKEYMODULE_ERR;
        if (count) after_str = ValkeyModule_LoadString(aof);
    }

    if (ValkeyModule_IsIOError(aof)) return VALKEYMODULE_ERR;
    return VALKEYMODULE_OK;
}

/* --- module load entry point --- */

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (ValkeyModule_Init(ctx, "testaof", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (argc >= 1)
        ValkeyModule_StringToLongLong(argv[0], &conf_flags);

    ValkeyModuleTypeMethods datatype_methods = {
        .version = VALKEYMODULE_TYPE_METHOD_VERSION,
        .aux_save_aof = testaof_aux_save,
        .aux_load_aof = testaof_aux_load,
        .aux_save_aof_triggers =
            ((conf_flags & CONF_AOF_BEFORE_KEYSPACE) ? VALKEYMODULE_AUX_BEFORE_AOF : 0) |
            ((conf_flags & CONF_AOF_AFTER_KEYSPACE) ? VALKEYMODULE_AUX_AFTER_AOF : 0),
    };

    testaof_type = ValkeyModule_CreateDataType(ctx, "test__aof", 1, &datatype_methods);
    if (testaof_type == NULL) return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "testaof.set.before", testaof_set_before, "deny-oom", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "testaof.get.before", testaof_get_before, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "testaof.set.after", testaof_set_after, "deny-oom", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "testaof.get.after", testaof_get_after, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
