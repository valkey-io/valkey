#include "valkeymodule.h"

#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#define UNUSED(x) (void)(x)

static int n_events = 0;

static int KeySpace_NotificationModuleKeyMissExpired(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    UNUSED(ctx);
    UNUSED(type);
    UNUSED(event);
    UNUSED(key);
    n_events++;
    return VALKEYMODULE_OK;
}

int test_clear_n_events(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    n_events = 0;
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int test_get_n_events(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_ReplyWithLongLong(ctx, n_events);
    return VALKEYMODULE_OK;
}

int test_open_key_no_effects(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc<2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    int supportedMode = ValkeyModule_GetOpenKeyModesAll();
    if (!(supportedMode & VALKEYMODULE_READ) || !(supportedMode & VALKEYMODULE_OPEN_KEY_NOEFFECTS)) {
        ValkeyModule_ReplyWithError(ctx, "OpenKey modes are not supported");
        return VALKEYMODULE_OK;
    }

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_OPEN_KEY_NOEFFECTS);
    if (!key) {
        ValkeyModule_ReplyWithError(ctx, "key not found");
        return VALKEYMODULE_OK;
    }

    ValkeyModule_CloseKey(key);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int test_call_generic(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc<2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    const char* cmdname = ValkeyModule_StringPtrLen(argv[1], NULL);
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, cmdname, "v", argv+2, argc-2);
    if (reply) {
        ValkeyModule_ReplyWithCallReply(ctx, reply);
        ValkeyModule_FreeCallReply(reply);
    } else {
        ValkeyModule_ReplyWithError(ctx, strerror(errno));
    }
    return VALKEYMODULE_OK;
}

int test_call_info(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    ValkeyModuleCallReply *reply;
    if (argc>1)
        reply = ValkeyModule_Call(ctx, "info", "s", argv[1]);
    else
        reply = ValkeyModule_Call(ctx, "info", "");
    if (reply) {
        ValkeyModule_ReplyWithCallReply(ctx, reply);
        ValkeyModule_FreeCallReply(reply);
    } else {
        ValkeyModule_ReplyWithError(ctx, strerror(errno));
    }
    return VALKEYMODULE_OK;
}

int test_ld_conv(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    long double ld = 0.00000000000000001L;
    const char *ldstr = "0.00000000000000001";
    ValkeyModuleString *s1 = ValkeyModule_CreateStringFromLongDouble(ctx, ld, 1);
    ValkeyModuleString *s2 =
        ValkeyModule_CreateString(ctx, ldstr, strlen(ldstr));
    if (ValkeyModule_StringCompare(s1, s2) != 0) {
        char err[4096];
        snprintf(err, 4096,
            "Failed to convert long double to string ('%s' != '%s')",
            ValkeyModule_StringPtrLen(s1, NULL),
            ValkeyModule_StringPtrLen(s2, NULL));
        ValkeyModule_ReplyWithError(ctx, err);
        goto final;
    }
    long double ld2 = 0;
    if (ValkeyModule_StringToLongDouble(s2, &ld2) == VALKEYMODULE_ERR) {
        ValkeyModule_ReplyWithError(ctx,
            "Failed to convert string to long double");
        goto final;
    }
    if (ld2 != ld) {
        char err[4096];
        snprintf(err, 4096,
            "Failed to convert string to long double (%.40Lf != %.40Lf)",
            ld2,
            ld);
        ValkeyModule_ReplyWithError(ctx, err);
        goto final;
    }

    /* Make sure we can't convert a string that has \0 in it */
    char buf[4] = "123";
    buf[1] = '\0';
    ValkeyModuleString *s3 = ValkeyModule_CreateString(ctx, buf, 3);
    long double ld3;
    if (ValkeyModule_StringToLongDouble(s3, &ld3) == VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "Invalid string successfully converted to long double");
        ValkeyModule_FreeString(ctx, s3);
        goto final;
    }
    ValkeyModule_FreeString(ctx, s3);

    ValkeyModule_ReplyWithLongDouble(ctx, ld2);
final:
    ValkeyModule_FreeString(ctx, s1);
    ValkeyModule_FreeString(ctx, s2);
    return VALKEYMODULE_OK;
}

int test_flushall(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModule_ResetDataset(1, 0);
    ValkeyModule_ReplyWithCString(ctx, "Ok");
    return VALKEYMODULE_OK;
}

int test_dbsize(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    long long ll = ValkeyModule_DbSize(ctx);
    ValkeyModule_ReplyWithLongLong(ctx, ll);
    return VALKEYMODULE_OK;
}

int test_randomkey(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModuleString *str = ValkeyModule_RandomKey(ctx);
    ValkeyModule_ReplyWithString(ctx, str);
    ValkeyModule_FreeString(ctx, str);
    return VALKEYMODULE_OK;
}

int test_keyexists(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 2) return ValkeyModule_WrongArity(ctx);
    ValkeyModuleString *key = argv[1];
    int exists = ValkeyModule_KeyExists(ctx, key);
    return ValkeyModule_ReplyWithBool(ctx, exists);
}

ValkeyModuleKey *open_key_or_reply(ValkeyModuleCtx *ctx, ValkeyModuleString *keyname, int mode) {
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, keyname, mode);
    if (!key) {
        ValkeyModule_ReplyWithError(ctx, "key not found");
        return NULL;
    }
    return key;
}

int test_getlru(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc<2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    ValkeyModuleKey *key = open_key_or_reply(ctx, argv[1], VALKEYMODULE_READ|VALKEYMODULE_OPEN_KEY_NOTOUCH);
    mstime_t lru;
    ValkeyModule_GetLRU(key, &lru);
    ValkeyModule_ReplyWithLongLong(ctx, lru);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

int test_setlru(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc<3) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    ValkeyModuleKey *key = open_key_or_reply(ctx, argv[1], VALKEYMODULE_READ|VALKEYMODULE_OPEN_KEY_NOTOUCH);
    mstime_t lru;
    if (ValkeyModule_StringToLongLong(argv[2], &lru) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "invalid idle time");
        return VALKEYMODULE_OK;
    }
    int was_set = ValkeyModule_SetLRU(key, lru)==VALKEYMODULE_OK;
    ValkeyModule_ReplyWithLongLong(ctx, was_set);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

int test_getlfu(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc<2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    ValkeyModuleKey *key = open_key_or_reply(ctx, argv[1], VALKEYMODULE_READ|VALKEYMODULE_OPEN_KEY_NOTOUCH);
    mstime_t lfu;
    ValkeyModule_GetLFU(key, &lfu);
    ValkeyModule_ReplyWithLongLong(ctx, lfu);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

int test_setlfu(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc<3) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    ValkeyModuleKey *key = open_key_or_reply(ctx, argv[1], VALKEYMODULE_READ|VALKEYMODULE_OPEN_KEY_NOTOUCH);
    mstime_t lfu;
    if (ValkeyModule_StringToLongLong(argv[2], &lfu) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "invalid freq");
        return VALKEYMODULE_OK;
    }
    int was_set = ValkeyModule_SetLFU(key, lfu)==VALKEYMODULE_OK;
    ValkeyModule_ReplyWithLongLong(ctx, was_set);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

int test_serverversion(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    (void) argv;
    (void) argc;

    int version = ValkeyModule_GetServerVersion();
    int patch = version & 0x000000ff;
    int minor = (version & 0x0000ff00) >> 8;
    int major = (version & 0x00ff0000) >> 16;

    ValkeyModuleString* vStr = ValkeyModule_CreateStringPrintf(ctx, "%d.%d.%d", major, minor, patch);
    ValkeyModule_ReplyWithString(ctx, vStr);
    ValkeyModule_FreeString(ctx, vStr);
  
    return VALKEYMODULE_OK;
}

int test_getclientcert(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    (void) argv;
    (void) argc;

    ValkeyModuleString *cert = ValkeyModule_GetClientCertificate(ctx,
            ValkeyModule_GetClientId(ctx));
    if (!cert) {
        ValkeyModule_ReplyWithNull(ctx);
    } else {
        ValkeyModule_ReplyWithString(ctx, cert);
        ValkeyModule_FreeString(ctx, cert);
    }

    return VALKEYMODULE_OK;
}

int test_clientinfo(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    (void) argv;
    (void) argc;

    ValkeyModuleClientInfoV1 ci = VALKEYMODULE_CLIENTINFO_INITIALIZER_V1;
    uint64_t client_id = ValkeyModule_GetClientId(ctx);

    /* Check expected result from the V1 initializer. */
    assert(ci.version == 1);
    /* Trying to populate a future version of the struct should fail. */
    ci.version = VALKEYMODULE_CLIENTINFO_VERSION + 1;
    assert(ValkeyModule_GetClientInfoById(&ci, client_id) == VALKEYMODULE_ERR);

    ci.version = 1;
    if (ValkeyModule_GetClientInfoById(&ci, client_id) == VALKEYMODULE_ERR) {
            ValkeyModule_ReplyWithError(ctx, "failed to get client info");
            return VALKEYMODULE_OK;
    }

    ValkeyModule_ReplyWithArray(ctx, 10);
    char flags[512];
    snprintf(flags, sizeof(flags) - 1, "%s:%s:%s:%s:%s:%s",
        ci.flags & VALKEYMODULE_CLIENTINFO_FLAG_SSL ? "ssl" : "",
        ci.flags & VALKEYMODULE_CLIENTINFO_FLAG_PUBSUB ? "pubsub" : "",
        ci.flags & VALKEYMODULE_CLIENTINFO_FLAG_BLOCKED ? "blocked" : "",
        ci.flags & VALKEYMODULE_CLIENTINFO_FLAG_TRACKING ? "tracking" : "",
        ci.flags & VALKEYMODULE_CLIENTINFO_FLAG_UNIXSOCKET ? "unixsocket" : "",
        ci.flags & VALKEYMODULE_CLIENTINFO_FLAG_MULTI ? "multi" : "");

    ValkeyModule_ReplyWithCString(ctx, "flags");
    ValkeyModule_ReplyWithCString(ctx, flags);
    ValkeyModule_ReplyWithCString(ctx, "id");
    ValkeyModule_ReplyWithLongLong(ctx, ci.id);
    ValkeyModule_ReplyWithCString(ctx, "addr");
    ValkeyModule_ReplyWithCString(ctx, ci.addr);
    ValkeyModule_ReplyWithCString(ctx, "port");
    ValkeyModule_ReplyWithLongLong(ctx, ci.port);
    ValkeyModule_ReplyWithCString(ctx, "db");
    ValkeyModule_ReplyWithLongLong(ctx, ci.db);

    return VALKEYMODULE_OK;
}

int test_getname(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    (void)argv;
    if (argc != 1) return ValkeyModule_WrongArity(ctx);
    unsigned long long id = ValkeyModule_GetClientId(ctx);
    ValkeyModuleString *name = ValkeyModule_GetClientNameById(ctx, id);
    if (name == NULL)
        return ValkeyModule_ReplyWithError(ctx, "-ERR No name");
    ValkeyModule_ReplyWithString(ctx, name);
    ValkeyModule_FreeString(ctx, name);
    return VALKEYMODULE_OK;
}

int test_setname(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    unsigned long long id = ValkeyModule_GetClientId(ctx);
    if (ValkeyModule_SetClientNameById(id, argv[1]) == VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    else
        return ValkeyModule_ReplyWithError(ctx, strerror(errno));
}

int test_log_tsctx(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    ValkeyModuleCtx *tsctx = ValkeyModule_GetDetachedThreadSafeContext(ctx);

    if (argc != 3) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    char level[50];
    size_t level_len;
    const char *level_str = ValkeyModule_StringPtrLen(argv[1], &level_len);
    snprintf(level, sizeof(level) - 1, "%.*s", (int) level_len, level_str);

    size_t msg_len;
    const char *msg_str = ValkeyModule_StringPtrLen(argv[2], &msg_len);

    ValkeyModule_Log(tsctx, level, "%.*s", (int) msg_len, msg_str);
    ValkeyModule_FreeThreadSafeContext(tsctx);

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int test_weird_cmd(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int test_monotonic_time(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    ValkeyModule_ReplyWithLongLong(ctx, ValkeyModule_MonotonicMicroseconds());
    return VALKEYMODULE_OK;
}

/* wrapper for RM_Call */
int test_rm_call(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    if(argc < 2){
        return ValkeyModule_WrongArity(ctx);
    }

    const char* cmd = ValkeyModule_StringPtrLen(argv[1], NULL);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, cmd, "Ev", argv + 2, argc - 2);
    if(!rep){
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    }else{
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    return VALKEYMODULE_OK;
}

/* wrapper for RM_Call which also replicates the module command */
int test_rm_call_replicate(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    test_rm_call(ctx, argv, argc);
    ValkeyModule_ReplicateVerbatim(ctx);

    return VALKEYMODULE_OK;
}

/* wrapper for RM_Call with flags */
int test_rm_call_flags(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    if(argc < 3){
        return ValkeyModule_WrongArity(ctx);
    }

    /* Append Ev to the provided flags. */
    ValkeyModuleString *flags = ValkeyModule_CreateStringFromString(ctx, argv[1]);
    ValkeyModule_StringAppendBuffer(ctx, flags, "Ev", 2);

    const char* flg = ValkeyModule_StringPtrLen(flags, NULL);
    const char* cmd = ValkeyModule_StringPtrLen(argv[2], NULL);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, cmd, flg, argv + 3, argc - 3);
    if(!rep){
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    }else{
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }
    ValkeyModule_FreeString(ctx, flags);

    return VALKEYMODULE_OK;
}

int test_ull_conv(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    unsigned long long ull = 18446744073709551615ULL;
    const char *ullstr = "18446744073709551615";

    ValkeyModuleString *s1 = ValkeyModule_CreateStringFromULongLong(ctx, ull);
    ValkeyModuleString *s2 =
        ValkeyModule_CreateString(ctx, ullstr, strlen(ullstr));
    if (ValkeyModule_StringCompare(s1, s2) != 0) {
        char err[4096];
        snprintf(err, 4096,
            "Failed to convert unsigned long long to string ('%s' != '%s')",
            ValkeyModule_StringPtrLen(s1, NULL),
            ValkeyModule_StringPtrLen(s2, NULL));
        ValkeyModule_ReplyWithError(ctx, err);
        goto final;
    }
    unsigned long long ull2 = 0;
    if (ValkeyModule_StringToULongLong(s2, &ull2) == VALKEYMODULE_ERR) {
        ValkeyModule_ReplyWithError(ctx,
            "Failed to convert string to unsigned long long");
        goto final;
    }
    if (ull2 != ull) {
        char err[4096];
        snprintf(err, 4096,
            "Failed to convert string to unsigned long long (%llu != %llu)",
            ull2,
            ull);
        ValkeyModule_ReplyWithError(ctx, err);
        goto final;
    }
    
    /* Make sure we can't convert a string more than ULLONG_MAX or less than 0 */
    ullstr = "18446744073709551616";
    ValkeyModuleString *s3 = ValkeyModule_CreateString(ctx, ullstr, strlen(ullstr));
    unsigned long long ull3;
    if (ValkeyModule_StringToULongLong(s3, &ull3) == VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "Invalid string successfully converted to unsigned long long");
        ValkeyModule_FreeString(ctx, s3);
        goto final;
    }
    ValkeyModule_FreeString(ctx, s3);
    ullstr = "-1";
    ValkeyModuleString *s4 = ValkeyModule_CreateString(ctx, ullstr, strlen(ullstr));
    unsigned long long ull4;
    if (ValkeyModule_StringToULongLong(s4, &ull4) == VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "Invalid string successfully converted to unsigned long long");
        ValkeyModule_FreeString(ctx, s4);
        goto final;
    }
    ValkeyModule_FreeString(ctx, s4);
   
    ValkeyModule_ReplyWithSimpleString(ctx, "ok");

final:
    ValkeyModule_FreeString(ctx, s1);
    ValkeyModule_FreeString(ctx, s2);
    return VALKEYMODULE_OK;
}

int test_malloc_api(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    void *p;

    p = ValkeyModule_TryAlloc(1024);
    memset(p, 0, 1024);
    ValkeyModule_Free(p);

    p = ValkeyModule_TryCalloc(1, 1024);
    memset(p, 1, 1024);

    p = ValkeyModule_TryRealloc(p, 5 * 1024);
    memset(p, 1, 5 * 1024);
    ValkeyModule_Free(p);

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int test_keyslot(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    /* Static check of the ClusterKeySlot + ClusterCanonicalKeyNameInSlot
     * round-trip for all slots. */
    for (unsigned int slot = 0; slot < 16384; slot++) {
        const char *tag = ValkeyModule_ClusterCanonicalKeyNameInSlot(slot);
        ValkeyModuleString *key = ValkeyModule_CreateStringPrintf(ctx, "x{%s}y", tag);
        assert(slot == ValkeyModule_ClusterKeySlot(key));
        ValkeyModule_FreeString(ctx, key);
    }
    if (argc != 2){
        return ValkeyModule_WrongArity(ctx);
    }
    unsigned int slot = ValkeyModule_ClusterKeySlot(argv[1]);
    return ValkeyModule_ReplyWithLongLong(ctx, slot);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx,"misc",1,VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_KEY_MISS | VALKEYMODULE_NOTIFY_EXPIRED, KeySpace_NotificationModuleKeyMissExpired) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx,"test.call_generic", test_call_generic,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.call_info", test_call_info,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.ld_conversion", test_ld_conv, "",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.ull_conversion", test_ull_conv, "",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.flushall", test_flushall,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.dbsize", test_dbsize,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.randomkey", test_randomkey,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.keyexists", test_keyexists,"",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.setlru", test_setlru,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.getlru", test_getlru,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.setlfu", test_setlfu,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.getlfu", test_getlfu,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.clientinfo", test_clientinfo,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.getname", test_getname,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.setname", test_setname,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.serverversion", test_serverversion,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.getclientcert", test_getclientcert,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.log_tsctx", test_log_tsctx,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    /* Add a command with ':' in it's name, so that we can check commandstats sanitization. */
    if (ValkeyModule_CreateCommand(ctx,"test.weird:cmd", test_weird_cmd,"readonly",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"test.monotonic_time", test_monotonic_time,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.rm_call", test_rm_call,"allow-stale", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.rm_call_flags", test_rm_call_flags,"allow-stale", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.rm_call_replicate", test_rm_call_replicate,"allow-stale", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.silent_open_key", test_open_key_no_effects,"", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.get_n_events", test_get_n_events,"", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.clear_n_events", test_clear_n_events,"", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.malloc_api", test_malloc_api,"", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.keyslot", test_keyslot, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
