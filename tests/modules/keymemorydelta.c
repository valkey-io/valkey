/* Test module for the key memory delta server event.
 *
 * The module keeps a shadow copy of the keyspace, mapping every key to its
 * memory usage, built only from the events it receives. Tests compare it with
 * the real keyspace and MEMORY USAGE. Every event is checked against the shadow
 * (its old state must match what the previous events reported) and must
 * describe an actual change.
 *
 * Like a real consumer, the module rebuilds the shadow from scratch when a
 * load starts, clears it on FLUSHDB/FLUSHALL, swaps it on SWAPDB, and stages a
 * separate shadow for the dataset being loaded by an asynchronous load, which
 * replaces the shadow if the load completes.
 *
 * Commands:
 * - KMD.RESET                - clear the shadow, the counters and the log
 * - KMD.STATS                - counters, as a flat name/value array
 * - KMD.DUMP <db>            - shadow of a database, as a flat key/bytes array
 * - KMD.LOG                  - recent events, newest last
 * - KMD.SUBSCRIBE            - subscribe to the event (done on load)
 * - KMD.UNSUBSCRIBE          - unsubscribe from the event
 * - KMD.SCAN                 - scan the keyspace, and every value through its key handle
 *
 * When "kmd:job" is reported, the callback registers a post notification job
 * setting "kmd:job:result". When "kmd:read" is reported, the callback reads
 * "kmd:expired" and the path hash "kmd:tree", which is tracked again since
 * reading it may resize it. When loaded from an RDB file, "kmd:load:del" is deleted and
 * "kmd:load:set" overwritten by a keyspace notification callback.
 *
 * Loaded with the "keyevent" argument, the module also subscribes to the Key
 * event, whose callbacks run while keys are deleted or overwritten. */

#include "valkeymodule.h"

#include <string.h>

#define MAX_DBS 16
#define LOG_SIZE 128

typedef struct {
    int dbnum;
    char key[64];
    long long old_exists, old_bytes, new_exists, new_bytes;
} LogEntry;

static ValkeyModuleDict *shadow[MAX_DBS];
static ValkeyModuleDict *staging[MAX_DBS]; /* Used during asynchronous loading. */
static int loading = 0, async_loading = 0;

static long long events, loading_events, async_events, async_deletions, chain_errors, noop_errors, replica_events;
static LogEntry event_log[LOG_SIZE];
static long long log_next;

static void freeShadow(ValkeyModuleDict **dicts) {
    for (int j = 0; j < MAX_DBS; j++) {
        if (dicts[j]) ValkeyModule_FreeDict(NULL, dicts[j]);
        dicts[j] = NULL;
    }
}

static void clearShadow(ValkeyModuleDict **dicts, int dbnum) {
    for (int j = 0; j < MAX_DBS; j++) {
        if (dbnum != -1 && j != dbnum) continue;
        if (dicts[j]) ValkeyModule_FreeDict(NULL, dicts[j]);
        dicts[j] = ValkeyModule_CreateDict(NULL);
    }
}

static void freeReply(ValkeyModuleCallReply *reply) {
    if (reply) ValkeyModule_FreeCallReply(reply);
}

static void setResultJob(ValkeyModuleCtx *ctx, void *pd) {
    VALKEYMODULE_NOT_USED(pd);
    freeReply(ValkeyModule_Call(ctx, "SET", "!cc", "kmd:job:result", "a value written by a post notification job"));
}

static void onKeyMemoryDelta(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data) {
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(sub);
    ValkeyModuleKeyMemoryDelta *info = data;
    if (info->version != VALKEYMODULE_KEYMEMORYDELTA_VERSION || info->dbnum < 0 || info->dbnum >= MAX_DBS ||
        (info->loading_dataset && !async_loading)) {
        chain_errors++;
        return;
    }

    size_t len;
    const char *key = ValkeyModule_StringPtrLen(info->key, &len);
    ValkeyModuleDict *d = (info->loading_dataset ? staging : shadow)[info->dbnum];

    events++;
    if (loading) loading_events++;
    if (info->loading_dataset) {
        async_events++;
        if (info->old_exists && !info->new_exists) async_deletions++;
    }
    if (ValkeyModule_GetContextFlags(ctx) & VALKEYMODULE_CTX_FLAGS_REPLICA) replica_events++;
    if (info->old_exists == info->new_exists && info->old_bytes == info->new_bytes) noop_errors++;

    int nokey;
    void *current = ValkeyModule_DictGetC(d, (void *)key, len, &nokey);
    if (nokey ? info->old_exists : (!info->old_exists || (uint64_t)(uintptr_t)current != info->old_bytes))
        chain_errors++;
    if (info->new_exists)
        ValkeyModule_DictReplaceC(d, (void *)key, len, (void *)(uintptr_t)info->new_bytes);
    else
        ValkeyModule_DictDelC(d, (void *)key, len, NULL);

    LogEntry *entry = &event_log[log_next++ % LOG_SIZE];
    entry->dbnum = info->dbnum;
    size_t copy = len < sizeof(entry->key) - 1 ? len : sizeof(entry->key) - 1;
    memcpy(entry->key, key, copy);
    entry->key[copy] = '\0';
    entry->old_exists = info->old_exists;
    entry->old_bytes = info->old_bytes;
    entry->new_exists = info->new_exists;
    entry->new_bytes = info->new_bytes;

    if (len == 7 && !memcmp(key, "kmd:job", len)) {
        ValkeyModule_AddPostNotificationJob(ctx, setResultJob, NULL, NULL);
    } else if (len == 8 && !memcmp(key, "kmd:read", len)) {
        freeReply(ValkeyModule_Call(ctx, "GET", "c", "kmd:expired"));
        freeReply(ValkeyModule_Call(ctx, "PHGET", "ccc", "kmd:tree", "path", "field"));
    }
}

static void onFlush(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(e);
    ValkeyModuleFlushInfo *fi = data;
    if (sub == VALKEYMODULE_SUBEVENT_FLUSHDB_END) clearShadow(shadow, fi->dbnum);
}

static void onSwapDb(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(sub);
    ValkeyModuleSwapDbInfo *si = data;
    if (si->dbnum_first >= MAX_DBS || si->dbnum_second >= MAX_DBS) return;
    ValkeyModuleDict *tmp = shadow[si->dbnum_first];
    shadow[si->dbnum_first] = shadow[si->dbnum_second];
    shadow[si->dbnum_second] = tmp;
}

static void onLoading(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(data);
    loading = sub == VALKEYMODULE_SUBEVENT_LOADING_RDB_START || sub == VALKEYMODULE_SUBEVENT_LOADING_AOF_START ||
              sub == VALKEYMODULE_SUBEVENT_LOADING_REPL_START;
    /* Keys are reported again as they are loaded. */
    if (loading && !async_loading) clearShadow(shadow, -1);
}

static void onReplAsyncLoad(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(data);
    if (sub == VALKEYMODULE_SUBEVENT_REPL_ASYNC_LOAD_STARTED) {
        clearShadow(staging, -1);
        async_loading = 1;
    } else if (sub == VALKEYMODULE_SUBEVENT_REPL_ASYNC_LOAD_COMPLETED) {
        freeShadow(shadow);
        memcpy(shadow, staging, sizeof(shadow));
        memset(staging, 0, sizeof(staging));
        async_loading = 0;
    } else if (sub == VALKEYMODULE_SUBEVENT_REPL_ASYNC_LOAD_ABORTED) {
        freeShadow(staging);
        async_loading = 0;
    }
}

static int onKeyspaceLoaded(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    size_t len;
    const char *name = ValkeyModule_StringPtrLen(key, &len);
    if (len == 12 && !memcmp(name, "kmd:load:del", len)) {
        freeReply(ValkeyModule_Call(ctx, "DEL", "c", "kmd:load:del"));
    } else if (len == 12 && !memcmp(name, "kmd:load:set", len)) {
        freeReply(
            ValkeyModule_Call(ctx, "SET", "cc", "kmd:load:set", "a longer value written while the key was being loaded"));
    }
    return VALKEYMODULE_OK;
}

static int resetCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    clearShadow(shadow, -1);
    events = loading_events = async_events = async_deletions = chain_errors = noop_errors = replica_events = 0;
    log_next = 0;
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int statsCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    const char *names[] = {"events", "loading_events", "async_events", "async_deletions",
                           "chain_errors", "noop_errors", "replica_events"};
    long long values[] = {events, loading_events, async_events, async_deletions, chain_errors, noop_errors,
                          replica_events};
    int count = sizeof(values) / sizeof(values[0]);
    ValkeyModule_ReplyWithArray(ctx, count * 2);
    for (int j = 0; j < count; j++) {
        ValkeyModule_ReplyWithSimpleString(ctx, names[j]);
        ValkeyModule_ReplyWithLongLong(ctx, values[j]);
    }
    return VALKEYMODULE_OK;
}

static int dumpCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    long long dbnum;
    if (ValkeyModule_StringToLongLong(argv[1], &dbnum) != VALKEYMODULE_OK || dbnum < 0 || dbnum >= MAX_DBS)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid db");

    ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(shadow[dbnum], "^", NULL, 0);
    char *key;
    size_t len;
    void *bytes;
    long long count = 0;
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    while ((key = ValkeyModule_DictNextC(iter, &len, &bytes)) != NULL) {
        ValkeyModule_ReplyWithStringBuffer(ctx, key, len);
        ValkeyModule_ReplyWithLongLong(ctx, (long long)(uintptr_t)bytes);
        count += 2;
    }
    ValkeyModule_DictIteratorStop(iter);
    ValkeyModule_ReplySetArrayLength(ctx, count);
    return VALKEYMODULE_OK;
}

static int logCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    long long first = log_next > LOG_SIZE ? log_next - LOG_SIZE : 0;
    ValkeyModule_ReplyWithArray(ctx, log_next - first);
    for (long long j = first; j < log_next; j++) {
        LogEntry *entry = &event_log[j % LOG_SIZE];
        ValkeyModule_ReplyWithArray(ctx, 6);
        ValkeyModule_ReplyWithLongLong(ctx, entry->dbnum);
        ValkeyModule_ReplyWithCString(ctx, entry->key);
        ValkeyModule_ReplyWithLongLong(ctx, entry->old_exists);
        ValkeyModule_ReplyWithLongLong(ctx, entry->old_bytes);
        ValkeyModule_ReplyWithLongLong(ctx, entry->new_exists);
        ValkeyModule_ReplyWithLongLong(ctx, entry->new_bytes);
    }
    return VALKEYMODULE_OK;
}

static void scanValueCallback(ValkeyModuleKey *key, ValkeyModuleString *field, ValkeyModuleString *value, void *pd) {
    VALKEYMODULE_NOT_USED(key);
    VALKEYMODULE_NOT_USED(field);
    VALKEYMODULE_NOT_USED(value);
    VALKEYMODULE_NOT_USED(pd);
}

static void scanKeyCallback(ValkeyModuleCtx *ctx, ValkeyModuleString *keyname, ValkeyModuleKey *key, void *pd) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(keyname);
    VALKEYMODULE_NOT_USED(pd);
    int type = ValkeyModule_KeyType(key);
    if (type != VALKEYMODULE_KEYTYPE_SET && type != VALKEYMODULE_KEYTYPE_HASH && type != VALKEYMODULE_KEYTYPE_ZSET) return;
    ValkeyModuleScanCursor *cursor = ValkeyModule_ScanCursorCreate();
    while (ValkeyModule_ScanKey(key, cursor, scanValueCallback, NULL));
    ValkeyModule_ScanCursorDestroy(cursor);
}

static int scanCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    ValkeyModuleScanCursor *cursor = ValkeyModule_ScanCursorCreate();
    while (ValkeyModule_Scan(ctx, cursor, scanKeyCallback, NULL));
    ValkeyModule_ScanCursorDestroy(cursor);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int subscribeCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_KeyMemoryDelta, onKeyMemoryDelta);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int unsubscribeCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_KeyMemoryDelta, NULL);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static void onKey(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(sub);
    VALKEYMODULE_NOT_USED(data);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (ValkeyModule_Init(ctx, "keymemorydelta", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    ValkeyModule_SetModuleOptions(ctx, VALKEYMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD |
                                           VALKEYMODULE_OPTIONS_HANDLE_ATOMIC_SLOT_MIGRATION);

    clearShadow(shadow, -1);
    for (int j = 0; j < argc; j++) {
        if (!strcmp(ValkeyModule_StringPtrLen(argv[j], NULL), "keyevent") &&
            ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_Key, onKey) == VALKEYMODULE_ERR)
            return VALKEYMODULE_ERR;
    }
    if (ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_KeyMemoryDelta, onKeyMemoryDelta) ==
            VALKEYMODULE_ERR ||
        ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_FlushDB, onFlush) == VALKEYMODULE_ERR ||
        ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_SwapDB, onSwapDb) == VALKEYMODULE_ERR ||
        ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_Loading, onLoading) == VALKEYMODULE_ERR ||
        ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_ReplAsyncLoad, onReplAsyncLoad) ==
            VALKEYMODULE_ERR ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_LOADED, onKeyspaceLoaded) ==
            VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    struct {
        const char *name;
        ValkeyModuleCmdFunc func;
    } commands[] = {
        {"kmd.reset", resetCommand},
        {"kmd.stats", statsCommand},
        {"kmd.dump", dumpCommand},
        {"kmd.log", logCommand},
        {"kmd.subscribe", subscribeCommand},
        {"kmd.unsubscribe", unsubscribeCommand},
        {"kmd.scan", scanCommand},
    };
    for (size_t j = 0; j < sizeof(commands) / sizeof(commands[0]); j++) {
        if (ValkeyModule_CreateCommand(ctx, commands[j].name, commands[j].func, "", 0, 0, 0) == VALKEYMODULE_ERR)
            return VALKEYMODULE_ERR;
    }
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    VALKEYMODULE_NOT_USED(ctx);
    freeShadow(shadow);
    freeShadow(staging);
    return VALKEYMODULE_OK;
}
