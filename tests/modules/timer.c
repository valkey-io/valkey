
#include "redismodule.h"

typedef struct {
    RedisModuleTimerID id;
    RedisModuleString *keyname;
} selfstopTimerData;

static void timer_callback(RedisModuleCtx *ctx, void *data)
{
    RedisModuleString *keyname = data;
    RedisModuleCallReply *reply;

    reply = RedisModule_Call(ctx, "INCR", "s", keyname);
    if (reply != NULL)
        RedisModule_FreeCallReply(reply);
    RedisModule_FreeString(ctx, keyname);
}

/* Stops the currently firing timer from inside its own callback.
 * This used to double-free the ValkeyModuleTimer in moduleTimerHandler(). */
static void selfstop_timer_callback(RedisModuleCtx *ctx, void *data)
{
    selfstopTimerData *d = data;
    RedisModuleCallReply *reply;
    void *timer_data = NULL;

    RedisModule_Assert(RedisModule_StopTimer(ctx, d->id, &timer_data) == REDISMODULE_OK);
    RedisModule_Assert(timer_data == d);

    reply = RedisModule_Call(ctx, "INCR", "s", d->keyname);
    if (reply != NULL)
        RedisModule_FreeCallReply(reply);
    RedisModule_FreeString(ctx, d->keyname);
    RedisModule_Free(d);
}

int test_createtimer(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    long long period;
    if (RedisModule_StringToLongLong(argv[1], &period) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "Invalid time specified.");
        return REDISMODULE_OK;
    }

    RedisModuleString *keyname = argv[2];
    RedisModule_RetainString(ctx, keyname);

    RedisModuleTimerID id = RedisModule_CreateTimer(ctx, period, timer_callback, keyname);
    RedisModule_ReplyWithLongLong(ctx, id);

    return REDISMODULE_OK;
}

int test_gettimer(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    long long id;
    if (RedisModule_StringToLongLong(argv[1], &id) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "Invalid id specified.");
        return REDISMODULE_OK;
    }

    uint64_t remaining;
    RedisModuleString *keyname;
    if (RedisModule_GetTimerInfo(ctx, id, &remaining, (void **)&keyname) == REDISMODULE_ERR) {
        RedisModule_ReplyWithNull(ctx);
    } else {
        RedisModule_ReplyWithArray(ctx, 2);
        RedisModule_ReplyWithString(ctx, keyname);
        RedisModule_ReplyWithLongLong(ctx, remaining);
    }

    return REDISMODULE_OK;
}

int test_stoptimer(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    long long id;
    if (RedisModule_StringToLongLong(argv[1], &id) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "Invalid id specified.");
        return REDISMODULE_OK;
    }

    int ret = 0;
    RedisModuleString *keyname;
    if (RedisModule_StopTimer(ctx, id, (void **) &keyname) == REDISMODULE_OK) {
        RedisModule_FreeString(ctx, keyname);
        ret = 1;
    }

    RedisModule_ReplyWithLongLong(ctx, ret);
    return REDISMODULE_OK;
}

int test_selfstoptimer(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 3) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    long long period;
    if (RedisModule_StringToLongLong(argv[1], &period) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "Invalid time specified.");
        return REDISMODULE_OK;
    }

    RedisModuleString *keyname = argv[2];
    RedisModule_RetainString(ctx, keyname);

    selfstopTimerData *d = RedisModule_Alloc(sizeof(*d));
    d->keyname = keyname;
    d->id = RedisModule_CreateTimer(ctx, period, selfstop_timer_callback, d);
    RedisModule_ReplyWithLongLong(ctx, d->id);

    return REDISMODULE_OK;
}


int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);
    if (RedisModule_Init(ctx,"timer",1,REDISMODULE_APIVER_1)== REDISMODULE_ERR)
        return REDISMODULE_ERR;

    if (RedisModule_CreateCommand(ctx,"test.createtimer", test_createtimer,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.gettimer", test_gettimer,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.stoptimer", test_stoptimer,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    if (RedisModule_CreateCommand(ctx,"test.selfstoptimer", test_selfstoptimer,"",0,0,0) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    return REDISMODULE_OK;
}
