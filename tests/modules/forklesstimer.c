/* A module that writes keys from timers and callbacks, outside
 * ValkeyModule_Call, and replicates each write with ValkeyModule_Replicate. It
 * declares HANDLE_FORKLESS, so a full sync with it loaded can be forkless. */

#include "valkeymodule.h"

#include <string.h>

static long long done = 0; /* the last SETALL/MULTISETALL timer finished */
static long long set = 0;  /* keys (or key pairs) written by the last timer */
static long long skipped =
    0;                      /* keys the key API refused (in use by the sync) */
static long long bumps = 0; /* keyless bumps applied on this server */
static long long remaining = 0;

typedef struct {
    ValkeyModuleString *prefix;
    ValkeyModuleString *value;
    long long count;
    int multi;
} setAllArgs;

static void setAllTimer(ValkeyModuleCtx *ctx, void *data) {
    setAllArgs *args = data;
    size_t plen;
    const char *prefix = ValkeyModule_StringPtrLen(args->prefix, &plen);
    ValkeyModule_SelectDb(ctx, 0);
    long long step = args->multi ? 2 : 1;
    for (long long i = 0; i < args->count; i += step) {
        if (args->multi && i + 1 >= args->count)
            break;
        ValkeyModuleString *k1 =
            ValkeyModule_CreateStringPrintf(ctx, "%.*s%lld", (int)plen, prefix, i);
        if (!args->multi) {
            ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, k1, VALKEYMODULE_WRITE);
            if (key == NULL) {
                skipped++;
            } else {
                ValkeyModule_StringSet(key, args->value);
                ValkeyModule_CloseKey(key);
                ValkeyModule_Replicate(ctx, "SET", "ss", k1, args->value);
                set++;
            }
        } else {
            ValkeyModuleString *k2 = ValkeyModule_CreateStringPrintf(
                ctx, "%.*s%lld", (int)plen, prefix, i + 1);
            ValkeyModuleKey *key1 = ValkeyModule_OpenKey(ctx, k1, VALKEYMODULE_WRITE);
            ValkeyModuleKey *key2 =
                key1 ? ValkeyModule_OpenKey(ctx, k2, VALKEYMODULE_WRITE) : NULL;
            if (key1 == NULL || key2 == NULL) {
                if (key1)
                    ValkeyModule_CloseKey(key1);
                skipped++;
            } else {
                ValkeyModule_StringSet(key1, args->value);
                ValkeyModule_StringSet(key2, args->value);
                ValkeyModule_CloseKey(key1);
                ValkeyModule_CloseKey(key2);
                ValkeyModule_Replicate(ctx, "MSET", "ssss", k1, args->value, k2,
                                       args->value);
                set++;
            }
            ValkeyModule_FreeString(ctx, k2);
        }
        ValkeyModule_FreeString(ctx, k1);
    }
    ValkeyModule_FreeString(NULL, args->prefix);
    ValkeyModule_FreeString(NULL, args->value);
    ValkeyModule_Free(args);
    done = 1;
}

static int setAllCommandImpl(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc, int multi) {
    if (argc != 4)
        return ValkeyModule_WrongArity(ctx);
    long long count;
    if (ValkeyModule_StringToLongLong(argv[2], &count) != VALKEYMODULE_OK ||
        count < 0)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid count");
    setAllArgs *args = ValkeyModule_Alloc(sizeof(*args));
    args->prefix = ValkeyModule_CreateStringFromString(NULL, argv[1]);
    args->value = ValkeyModule_CreateStringFromString(NULL, argv[3]);
    args->count = count;
    args->multi = multi;
    done = set = skipped = 0;
    ValkeyModule_CreateTimer(ctx, 1, setAllTimer, args);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* FORKLESSTIMER.SETALL <prefix> <count> <value> */
static int setAllCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return setAllCommandImpl(ctx, argv, argc, 0);
}

/* FORKLESSTIMER.MULTISETALL <prefix> <count> <value>: pairs of keys, one MSET
 * each */
static int multiSetAllCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return setAllCommandImpl(ctx, argv, argc, 1);
}

/* FORKLESSTIMER.STATS -> [done, set, skipped] */
static int statsCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1)
        return ValkeyModule_WrongArity(ctx);
    ValkeyModule_ReplyWithArray(ctx, 3);
    ValkeyModule_ReplyWithLongLong(ctx, done);
    ValkeyModule_ReplyWithLongLong(ctx, set);
    ValkeyModule_ReplyWithLongLong(ctx, skipped);
    return VALKEYMODULE_OK;
}

/* forklesstimer.bump: keyless write, replicated by the timer and the callback
 */
static int bumpCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1)
        return ValkeyModule_WrongArity(ctx);
    bumps++;
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static void bumpAllTimer(ValkeyModuleCtx *ctx, void *data) {
    VALKEYMODULE_NOT_USED(data);
    bumps++;
    ValkeyModule_Replicate(ctx, "forklesstimer.bump", "");
    remaining--;
    if (remaining > 0)
        ValkeyModule_CreateTimer(ctx, 1, bumpAllTimer, NULL);
}

/* FORKLESSTIMER.BUMPALL <n> */
static int bumpAllCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2)
        return ValkeyModule_WrongArity(ctx);
    long long n;
    if (ValkeyModule_StringToLongLong(argv[1], &n) != VALKEYMODULE_OK || n <= 0)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid count");
    remaining = n;
    ValkeyModule_CreateTimer(ctx, 1, bumpAllTimer, NULL);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* FORKLESSTIMER.BUMPSTATS -> bumps */
static int bumpStatsCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1)
        return ValkeyModule_WrongArity(ctx);
    return ValkeyModule_ReplyWithLongLong(ctx, bumps);
}

/* Runs after the expiry that triggered it, outside call(): counts the key and
 * replicates a keyless bump. */
static void expiredJob(ValkeyModuleCtx *ctx, void *pd) {
    VALKEYMODULE_NOT_USED(pd);
    bumps++;
    ValkeyModule_Replicate(ctx, "forklesstimer.bump", "");
}

/* Active expiry of expire:* keys runs this callback. Writing from inside a
 * keyspace notification is discouraged, so it defers the write to a
 * post-notification job. */
static int expiredCallback(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    size_t len;
    const char *name = ValkeyModule_StringPtrLen(key, &len);
    if (len >= 7 && !strncmp(name, "expire:", 7))
        ValkeyModule_AddPostNotificationJob(ctx, expiredJob, NULL, NULL);
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "forklesstimer", 1, VALKEYMODULE_APIVER_1) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    ValkeyModule_SetModuleOptions(ctx, VALKEYMODULE_OPTIONS_HANDLE_FORKLESS);
    if (ValkeyModule_CreateCommand(ctx, "forklesstimer.setall", setAllCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "forklesstimer.multisetall",
                                   multiSetAllCommand, "readonly", 0, 0,
                                   0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "forklesstimer.stats", statsCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "forklesstimer.bump", bumpCommand,
                                   "write", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "forklesstimer.bumpall", bumpAllCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "forklesstimer.bumpstats",
                                   bumpStatsCommand, "readonly", 0, 0,
                                   0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_EXPIRED,
                                               expiredCallback) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    return VALKEYMODULE_OK;
}
