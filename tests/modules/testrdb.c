#include "valkeymodule.h"

#include <string.h>
#include <assert.h>

/* Module configuration, save aux or not? */
#define CONF_AUX_OPTION_NO_AUX           0
#define CONF_AUX_OPTION_SAVE2            1 << 0
#define CONF_AUX_OPTION_BEFORE_KEYSPACE  1 << 1
#define CONF_AUX_OPTION_AFTER_KEYSPACE   1 << 2
#define CONF_AUX_OPTION_NO_DATA          1 << 3
long long conf_aux_count = 0;

/* Registered type */
ValkeyModuleType *testrdb_type = NULL;

/* Global values to store and persist to aux */
ValkeyModuleString *before_str = NULL;
ValkeyModuleString *after_str = NULL;

/* Global values used to keep aux from db being loaded (in case of async_loading) */
ValkeyModuleString *before_str_temp = NULL;
ValkeyModuleString *after_str_temp = NULL;

/* Indicates whether there is an async replication in progress.
 * We control this value from ValkeyModuleEvent_ReplAsyncLoad events. */
int async_loading = 0;

int n_aux_load_called = 0;

void replAsyncLoadCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(data);

    switch (sub) {
    case VALKEYMODULE_SUBEVENT_REPL_ASYNC_LOAD_STARTED:
        assert(async_loading == 0);
        async_loading = 1;
        break;
    case VALKEYMODULE_SUBEVENT_REPL_ASYNC_LOAD_ABORTED:
        /* Discard temp aux */
        if (before_str_temp)
            ValkeyModule_FreeString(ctx, before_str_temp);
        if (after_str_temp)
            ValkeyModule_FreeString(ctx, after_str_temp);
        before_str_temp = NULL;
        after_str_temp = NULL;

        async_loading = 0;
        break;
    case VALKEYMODULE_SUBEVENT_REPL_ASYNC_LOAD_COMPLETED:
        if (before_str)
            ValkeyModule_FreeString(ctx, before_str);
        if (after_str)
            ValkeyModule_FreeString(ctx, after_str);
        before_str = before_str_temp;
        after_str = after_str_temp;

        before_str_temp = NULL;
        after_str_temp = NULL;

        async_loading = 0;
        break;
    default:
        assert(0);
    }
}

void *testrdb_type_load(ValkeyModuleIO *rdb, int encver) {
    int count = ValkeyModule_LoadSigned(rdb);
    ValkeyModuleString *str = ValkeyModule_LoadString(rdb);
    float f = ValkeyModule_LoadFloat(rdb);
    long double ld = ValkeyModule_LoadLongDouble(rdb);
    if (ValkeyModule_IsIOError(rdb)) {
        ValkeyModuleCtx *ctx = ValkeyModule_GetContextFromIO(rdb);
        if (str)
            ValkeyModule_FreeString(ctx, str);
        return NULL;
    }
    /* Using the values only after checking for io errors. */
    assert(count==1);
    assert(encver==1);
    assert(f==1.5f);
    assert(ld==0.333333333333333333L);
    return str;
}

void testrdb_type_save(ValkeyModuleIO *rdb, void *value) {
    ValkeyModuleString *str = (ValkeyModuleString*)value;
    ValkeyModule_SaveSigned(rdb, 1);
    ValkeyModule_SaveString(rdb, str);
    ValkeyModule_SaveFloat(rdb, 1.5);
    ValkeyModule_SaveLongDouble(rdb, 0.333333333333333333L);
}

void testrdb_aux_save(ValkeyModuleIO *rdb, int when) {
    if (!(conf_aux_count & CONF_AUX_OPTION_BEFORE_KEYSPACE)) assert(when == VALKEYMODULE_AUX_AFTER_RDB);
    if (!(conf_aux_count & CONF_AUX_OPTION_AFTER_KEYSPACE)) assert(when == VALKEYMODULE_AUX_BEFORE_RDB);
    assert(conf_aux_count!=CONF_AUX_OPTION_NO_AUX);
    if (when == VALKEYMODULE_AUX_BEFORE_RDB) {
        if (before_str) {
            ValkeyModule_SaveSigned(rdb, 1);
            ValkeyModule_SaveString(rdb, before_str);
        } else {
            ValkeyModule_SaveSigned(rdb, 0);
        }
    } else {
        if (after_str) {
            ValkeyModule_SaveSigned(rdb, 1);
            ValkeyModule_SaveString(rdb, after_str);
        } else {
            ValkeyModule_SaveSigned(rdb, 0);
        }
    }
}

int testrdb_aux_load(ValkeyModuleIO *rdb, int encver, int when) {
    assert(encver == 1);
    if (!(conf_aux_count & CONF_AUX_OPTION_BEFORE_KEYSPACE)) assert(when == VALKEYMODULE_AUX_AFTER_RDB);
    if (!(conf_aux_count & CONF_AUX_OPTION_AFTER_KEYSPACE)) assert(when == VALKEYMODULE_AUX_BEFORE_RDB);
    assert(conf_aux_count!=CONF_AUX_OPTION_NO_AUX);
    ValkeyModuleCtx *ctx = ValkeyModule_GetContextFromIO(rdb);
    if (when == VALKEYMODULE_AUX_BEFORE_RDB) {
        if (async_loading == 0) {
            if (before_str)
                ValkeyModule_FreeString(ctx, before_str);
            before_str = NULL;
            int count = ValkeyModule_LoadSigned(rdb);
            if (ValkeyModule_IsIOError(rdb))
                return VALKEYMODULE_ERR;
            if (count)
                before_str = ValkeyModule_LoadString(rdb);
        } else {
            if (before_str_temp)
                ValkeyModule_FreeString(ctx, before_str_temp);
            before_str_temp = NULL;
            int count = ValkeyModule_LoadSigned(rdb);
            if (ValkeyModule_IsIOError(rdb))
                return VALKEYMODULE_ERR;
            if (count)
                before_str_temp = ValkeyModule_LoadString(rdb);
        }
    } else {
        if (async_loading == 0) {
            if (after_str)
                ValkeyModule_FreeString(ctx, after_str);
            after_str = NULL;
            int count = ValkeyModule_LoadSigned(rdb);
            if (ValkeyModule_IsIOError(rdb))
                return VALKEYMODULE_ERR;
            if (count)
                after_str = ValkeyModule_LoadString(rdb);
        } else {
            if (after_str_temp)
                ValkeyModule_FreeString(ctx, after_str_temp);
            after_str_temp = NULL;
            int count = ValkeyModule_LoadSigned(rdb);
            if (ValkeyModule_IsIOError(rdb))
                return VALKEYMODULE_ERR;
            if (count)
                after_str_temp = ValkeyModule_LoadString(rdb);
        }
    }

    if (ValkeyModule_IsIOError(rdb))
        return VALKEYMODULE_ERR;
    return VALKEYMODULE_OK;
}

void testrdb_type_free(void *value) {
    if (value)
        ValkeyModule_FreeString(NULL, (ValkeyModuleString*)value);
}

int testrdb_set_before(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    if (before_str)
        ValkeyModule_FreeString(ctx, before_str);
    before_str = argv[1];
    ValkeyModule_RetainString(ctx, argv[1]);
    ValkeyModule_ReplyWithLongLong(ctx, 1);
    return VALKEYMODULE_OK;
}

int testrdb_get_before(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1){
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    if (before_str)
        ValkeyModule_ReplyWithString(ctx, before_str);
    else
        ValkeyModule_ReplyWithStringBuffer(ctx, "", 0);
    return VALKEYMODULE_OK;
}

/* For purpose of testing module events, expose variable state during async_loading. */
int testrdb_async_loading_get_before(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1){
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    if (before_str_temp)
        ValkeyModule_ReplyWithString(ctx, before_str_temp);
    else
        ValkeyModule_ReplyWithStringBuffer(ctx, "", 0);
    return VALKEYMODULE_OK;
}

int testrdb_set_after(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 2){
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    if (after_str)
        ValkeyModule_FreeString(ctx, after_str);
    after_str = argv[1];
    ValkeyModule_RetainString(ctx, argv[1]);
    ValkeyModule_ReplyWithLongLong(ctx, 1);
    return VALKEYMODULE_OK;
}

int testrdb_get_after(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1){
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    if (after_str)
        ValkeyModule_ReplyWithString(ctx, after_str);
    else
        ValkeyModule_ReplyWithStringBuffer(ctx, "", 0);
    return VALKEYMODULE_OK;
}

int testrdb_set_key(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 3){
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    ValkeyModuleString *str = ValkeyModule_ModuleTypeGetValue(key);
    if (str)
        ValkeyModule_FreeString(ctx, str);
    ValkeyModule_ModuleTypeSetValue(key, testrdb_type, argv[2]);
    ValkeyModule_RetainString(ctx, argv[2]);
    ValkeyModule_CloseKey(key);
    ValkeyModule_ReplyWithLongLong(ctx, 1);
    return VALKEYMODULE_OK;
}

int testrdb_get_key(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 2){
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    ValkeyModuleString *str = ValkeyModule_ModuleTypeGetValue(key);
    ValkeyModule_CloseKey(key);
    ValkeyModule_ReplyWithString(ctx, str);
    return VALKEYMODULE_OK;
}

int testrdb_get_n_aux_load_called(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModule_ReplyWithLongLong(ctx, n_aux_load_called);
    return VALKEYMODULE_OK;
}

int test2rdb_aux_load(ValkeyModuleIO *rdb, int encver, int when) {
    VALKEYMODULE_NOT_USED(rdb);
    VALKEYMODULE_NOT_USED(encver);
    VALKEYMODULE_NOT_USED(when);
    n_aux_load_called++;
    return VALKEYMODULE_OK;
}

void test2rdb_aux_save(ValkeyModuleIO *rdb, int when) {
    VALKEYMODULE_NOT_USED(rdb);
    VALKEYMODULE_NOT_USED(when);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx,"testrdb",1,VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModule_SetModuleOptions(ctx, VALKEYMODULE_OPTIONS_HANDLE_IO_ERRORS | VALKEYMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD);

    if (argc > 0)
        ValkeyModule_StringToLongLong(argv[0], &conf_aux_count);

    if (conf_aux_count==CONF_AUX_OPTION_NO_AUX) {
        ValkeyModuleTypeMethods datatype_methods = {
            .version = 1,
            .rdb_load = testrdb_type_load,
            .rdb_save = testrdb_type_save,
            .aof_rewrite = NULL,
            .digest = NULL,
            .free = testrdb_type_free,
        };

        testrdb_type = ValkeyModule_CreateDataType(ctx, "test__rdb", 1, &datatype_methods);
        if (testrdb_type == NULL)
            return VALKEYMODULE_ERR;
    } else if (!(conf_aux_count & CONF_AUX_OPTION_NO_DATA)) {
        ValkeyModuleTypeMethods datatype_methods = {
            .version = VALKEYMODULE_TYPE_METHOD_VERSION,
            .rdb_load = testrdb_type_load,
            .rdb_save = testrdb_type_save,
            .aof_rewrite = NULL,
            .digest = NULL,
            .free = testrdb_type_free,
            .aux_load = testrdb_aux_load,
            .aux_save = testrdb_aux_save,
            .aux_save_triggers = ((conf_aux_count & CONF_AUX_OPTION_BEFORE_KEYSPACE) ? VALKEYMODULE_AUX_BEFORE_RDB : 0) |
                                 ((conf_aux_count & CONF_AUX_OPTION_AFTER_KEYSPACE)  ? VALKEYMODULE_AUX_AFTER_RDB : 0)
        };

        if (conf_aux_count & CONF_AUX_OPTION_SAVE2) {
            datatype_methods.aux_save2 = testrdb_aux_save;
        }

        testrdb_type = ValkeyModule_CreateDataType(ctx, "test__rdb", 1, &datatype_methods);
        if (testrdb_type == NULL)
            return VALKEYMODULE_ERR;
    } else {

        /* Used to verify that aux_save2 api without any data, saves nothing to the RDB. */
        ValkeyModuleTypeMethods datatype_methods = {
            .version = VALKEYMODULE_TYPE_METHOD_VERSION,
            .aux_load = test2rdb_aux_load,
            .aux_save = test2rdb_aux_save,
            .aux_save_triggers = ((conf_aux_count & CONF_AUX_OPTION_BEFORE_KEYSPACE) ? VALKEYMODULE_AUX_BEFORE_RDB : 0) |
                                 ((conf_aux_count & CONF_AUX_OPTION_AFTER_KEYSPACE)  ? VALKEYMODULE_AUX_AFTER_RDB : 0)
        };
        if (conf_aux_count & CONF_AUX_OPTION_SAVE2) {
            datatype_methods.aux_save2 = test2rdb_aux_save;
        }

        ValkeyModule_CreateDataType(ctx, "test__rdb", 1, &datatype_methods);
    }

    if (ValkeyModule_CreateCommand(ctx,"testrdb.set.before", testrdb_set_before,"deny-oom",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"testrdb.get.before", testrdb_get_before,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"testrdb.async_loading.get.before", testrdb_async_loading_get_before,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"testrdb.set.after", testrdb_set_after,"deny-oom",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"testrdb.get.after", testrdb_get_after,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"testrdb.set.key", testrdb_set_key,"deny-oom",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"testrdb.get.key", testrdb_get_key,"",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"testrdb.get.n_aux_load_called", testrdb_get_n_aux_load_called,"",1,1,1) == VALKEYMODULE_ERR)
            return VALKEYMODULE_ERR;

    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_ReplAsyncLoad, replAsyncLoadCallback);

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    if (before_str)
        ValkeyModule_FreeString(ctx, before_str);
    if (after_str)
        ValkeyModule_FreeString(ctx, after_str);
    if (before_str_temp)
        ValkeyModule_FreeString(ctx, before_str_temp);
    if (after_str_temp)
        ValkeyModule_FreeString(ctx, after_str_temp);
    return VALKEYMODULE_OK;
}
