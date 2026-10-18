/* Test module for ValkeyModule_UnsubscribeFromKeyspaceEvents.
 *
 * Subscriptions made on load, in this order:
 *   - countEvents on STRING|HASH, counting events per type.
 *   - countStringOther on STRING, a second callback on the same type.
 *   - unsubscribeSelf on LIST, which unsubscribes itself on its first call.
 *   - unsubscribeNext on SET, followed by countVictim on SET. The former
 *     unsubscribes the latter, which is the next entry the dispatcher visits.
 *   - nestedOuter on ZSET, followed by nestedVictim on ZSET. On a ZSET event,
 *     nestedOuter runs XADD, whose STREAM event is dispatched while the ZSET
 *     dispatch is still in progress. nestedInner on STREAM then unsubscribes
 *     nestedVictim from within that inner dispatch.
 *
 * Commands:
 *   KSU.SUB                 - subscribe countEvents to STRING|HASH again.
 *   KSU.UNSUB string|hash   - unsubscribe countEvents from the given type.
 *   KSU.COUNT name          - reply with the named counter.
 */

#include "valkeymodule.h"
#include <strings.h>

static long long string_events = 0;
static long long hash_events = 0;
static long long other_events = 0;
static long long self_events = 0;
static long long victim_events = 0;
static long long nested_victim_events = 0;

static int countEvents(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);
    if (type == VALKEYMODULE_NOTIFY_STRING) string_events++;
    if (type == VALKEYMODULE_NOTIFY_HASH) hash_events++;
    return VALKEYMODULE_OK;
}

static int countStringOther(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);
    other_events++;
    return VALKEYMODULE_OK;
}

static int unsubscribeSelf(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);
    self_events++;
    ValkeyModule_UnsubscribeFromKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_LIST, unsubscribeSelf);
    return VALKEYMODULE_OK;
}

static int countVictim(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);
    victim_events++;
    return VALKEYMODULE_OK;
}

static int unsubscribeNext(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);
    ValkeyModule_UnsubscribeFromKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_SET, countVictim);
    return VALKEYMODULE_OK;
}

static int nestedVictim(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);
    nested_victim_events++;
    return VALKEYMODULE_OK;
}

static int nestedOuter(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);
    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, "XADD", "!cccc", "nested_stream", "*", "f", "v");
    ValkeyModule_FreeCallReply(rep);
    return VALKEYMODULE_OK;
}

static int nestedInner(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);
    ValkeyModule_UnsubscribeFromKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_ZSET, nestedVictim);
    return VALKEYMODULE_OK;
}

static int cmdSub(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_STRING | VALKEYMODULE_NOTIFY_HASH, countEvents);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int cmdUnsub(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    const char *arg = ValkeyModule_StringPtrLen(argv[1], NULL);
    int types;
    if (!strcasecmp(arg, "string")) {
        types = VALKEYMODULE_NOTIFY_STRING;
    } else if (!strcasecmp(arg, "hash")) {
        types = VALKEYMODULE_NOTIFY_HASH;
    } else {
        return ValkeyModule_ReplyWithError(ctx, "ERR unknown type");
    }
    if (ValkeyModule_UnsubscribeFromKeyspaceEvents(ctx, types, countEvents) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx, "ERR not subscribed");
    }
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int cmdCount(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    const char *name = ValkeyModule_StringPtrLen(argv[1], NULL);
    long long count;
    if (!strcasecmp(name, "string")) {
        count = string_events;
    } else if (!strcasecmp(name, "hash")) {
        count = hash_events;
    } else if (!strcasecmp(name, "other")) {
        count = other_events;
    } else if (!strcasecmp(name, "self")) {
        count = self_events;
    } else if (!strcasecmp(name, "victim")) {
        count = victim_events;
    } else if (!strcasecmp(name, "nested_victim")) {
        count = nested_victim_events;
    } else {
        return ValkeyModule_ReplyWithError(ctx, "ERR unknown counter");
    }
    return ValkeyModule_ReplyWithLongLong(ctx, count);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "keyspace_unsubscribe", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_STRING | VALKEYMODULE_NOTIFY_HASH,
                                               countEvents) != VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_STRING, countStringOther) != VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_LIST, unsubscribeSelf) != VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_SET, unsubscribeNext) != VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_SET, countVictim) != VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_ZSET, nestedOuter) != VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_ZSET, nestedVictim) != VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_STREAM, nestedInner) != VALKEYMODULE_OK) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "ksu.sub", cmdSub, "", 0, 0, 0) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "ksu.unsub", cmdUnsub, "", 0, 0, 0) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "ksu.count", cmdCount, "", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    return VALKEYMODULE_OK;
}
