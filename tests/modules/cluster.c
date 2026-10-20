#include "valkeymodule.h"
#include <string.h>

#define UNUSED(x) (void)(x)

void cluster_timer_handler(ValkeyModuleCtx *ctx, void *data) {
    VALKEYMODULE_NOT_USED(data);

    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, "CLUSTER", "c", "SLOTS");

    if (rep) {
        if (ValkeyModule_CallReplyType(rep) == VALKEYMODULE_REPLY_ARRAY) {
            ValkeyModule_Log(ctx, "notice", "Timer: CLUSTER SLOTS success");
        } else {
            ValkeyModule_Log(ctx, "notice",
                             "Timer: CLUSTER SLOTS unexpected reply type %d",
                             ValkeyModule_CallReplyType(rep));
        }
        ValkeyModule_FreeCallReply(rep);
    } else {
        ValkeyModule_Log(ctx, "warning", "Timer: CLUSTER SLOTS failed");
    }
}

int test_start_cluster_timer(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    ValkeyModule_CreateTimer(ctx, 1, cluster_timer_handler, NULL);

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}


int test_cluster_slots(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);

    if (argc != 1) return ValkeyModule_WrongArity(ctx);

    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, "CLUSTER", "c", "SLOTS");
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "ERR NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    return VALKEYMODULE_OK;
}

int test_cluster_shards(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);

    if (argc != 1) return ValkeyModule_WrongArity(ctx);

    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, "CLUSTER", "c", "SHARDS");
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "ERR NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }

    return VALKEYMODULE_OK;
}

#define MSGTYPE_DING 1
#define MSGTYPE_DONG 2
#define MSGTYPE_TEST_UAF 3
#define MSGTYPE_TEST_MAX 255

/* test.pingall */
int PingallCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    ValkeyModule_SendClusterMessage(ctx, NULL, MSGTYPE_DING, "Hey", 3);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

void DingReceiver(ValkeyModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len) {
    ValkeyModule_Log(ctx, "notice", "DING (type %d) RECEIVED from %.*s: '%.*s'", type, VALKEYMODULE_NODE_ID_LEN, sender_id, (int)len, payload);
    /* Ensure sender_id is null-terminated for cross-version compatibility */
    char null_terminated_sender_id[VALKEYMODULE_NODE_ID_LEN + 1];
    memcpy(null_terminated_sender_id, sender_id, VALKEYMODULE_NODE_ID_LEN);
    null_terminated_sender_id[VALKEYMODULE_NODE_ID_LEN] = '\0';
    ValkeyModule_SendClusterMessage(ctx, null_terminated_sender_id, MSGTYPE_DONG, "Message Received!", 17);
}

void DongReceiver(ValkeyModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len) {
    ValkeyModule_Log(ctx, "notice", "DONG (type %d) RECEIVED from %.*s: '%.*s'", type, VALKEYMODULE_NODE_ID_LEN, sender_id, (int)len, payload);
}

int test_register_receiver(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_RegisterClusterMessageReceiver(ctx, MSGTYPE_TEST_UAF, DingReceiver);
    ValkeyModule_RegisterClusterMessageReceiver(ctx, MSGTYPE_TEST_MAX, DingReceiver);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

int test_unregister_receiver(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_RegisterClusterMessageReceiver(ctx, MSGTYPE_TEST_UAF, NULL);
    ValkeyModule_RegisterClusterMessageReceiver(ctx, MSGTYPE_TEST_MAX, NULL);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

int test_send_msg_uaf(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_SendClusterMessage(ctx, NULL, MSGTYPE_TEST_UAF, "TestUAF", 7);
    ValkeyModule_SendClusterMessage(ctx, NULL, MSGTYPE_TEST_MAX, "TestMAX", 7);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* Commands for the per-slot memory-bytes tests. Each one changes a string in
 * place with StringTruncate and then makes a second change on the same open
 * key, so the in-place change is still pending when the second change runs. */

/* test.slotmem_truncate_expire <key> <len> <ms> */
int test_slotmem_truncate_expire(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    long long len, ms;
    if (ValkeyModule_StringToLongLong(argv[2], &len) != VALKEYMODULE_OK ||
        ValkeyModule_StringToLongLong(argv[3], &ms) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid arguments");
    ValkeyModuleKey *k = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    ValkeyModule_StringTruncate(k, len);
    ValkeyModule_SetExpire(k, ms);
    ValkeyModule_CloseKey(k);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* test.slotmem_truncate_set <key> <len> <value> */
int test_slotmem_truncate_set(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    long long len;
    if (ValkeyModule_StringToLongLong(argv[2], &len) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid arguments");
    ValkeyModuleKey *k = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    ValkeyModule_StringTruncate(k, len);
    ValkeyModule_StringSet(k, argv[3]);
    ValkeyModule_CloseKey(k);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* test.slotmem_truncate_two <key1> <key2> <len>
 * Declares no keys, so the two keys may be in different slots. */
int test_slotmem_truncate_two(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    long long len;
    if (ValkeyModule_StringToLongLong(argv[3], &len) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid arguments");
    ValkeyModuleKey *k1 = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    ValkeyModuleKey *k2 = ValkeyModule_OpenKey(ctx, argv[2], VALKEYMODULE_WRITE);
    ValkeyModule_StringTruncate(k1, len);
    ValkeyModule_StringTruncate(k2, len);
    ValkeyModule_CloseKey(k1);
    ValkeyModule_CloseKey(k2);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* test.slotmem_call_append <key> <suffix> [<len>]
 * Opens the key for write, runs APPEND on it with VM_Call, and then, if <len>
 * is given, truncates the open key to <len> before closing it. */
int test_slotmem_call_append(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3 && argc != 4) return ValkeyModule_WrongArity(ctx);
    long long len = -1;
    if (argc == 4 && ValkeyModule_StringToLongLong(argv[3], &len) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid arguments");
    ValkeyModuleKey *k = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    ValkeyModuleCallReply *rep = ValkeyModule_Call(ctx, "APPEND", "ss", argv[1], argv[2]);
    if (rep) ValkeyModule_FreeCallReply(rep);
    if (len >= 0) ValkeyModule_StringTruncate(k, len);
    ValkeyModule_CloseKey(k);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

typedef struct {
    ValkeyModuleString *key;
    long long len;
} slotmemTimerData;

static void slotmemTimerTruncate(ValkeyModuleCtx *ctx, void *data) {
    slotmemTimerData *d = data;
    ValkeyModuleKey *k = ValkeyModule_OpenKey(ctx, d->key, VALKEYMODULE_WRITE);
    ValkeyModule_StringTruncate(k, d->len);
    ValkeyModule_CloseKey(k);
    ValkeyModule_FreeString(NULL, d->key);
    ValkeyModule_Free(d);
}

/* test.slotmem_timer_truncate <key> <len>
 * Truncates the key from a timer callback, where no client is executing. */
int test_slotmem_timer_truncate(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    long long len;
    if (ValkeyModule_StringToLongLong(argv[2], &len) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid arguments");
    slotmemTimerData *d = ValkeyModule_Alloc(sizeof(*d));
    d->key = ValkeyModule_CreateStringFromString(NULL, argv[1]);
    d->len = len;
    ValkeyModule_CreateTimer(ctx, 1, slotmemTimerTruncate, d);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "cluster", 1, VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.pingall", PingallCommand, "readonly", 0, 0, 0) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.cluster_slots", test_cluster_slots, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.cluster_shards", test_cluster_shards, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.start_cluster_timer", test_start_cluster_timer, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.register_receiver", test_register_receiver, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.unregister_receiver", test_unregister_receiver, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.send_msg_uaf", test_send_msg_uaf, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.slotmem_truncate_expire", test_slotmem_truncate_expire, "write", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.slotmem_truncate_set", test_slotmem_truncate_set, "write", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.slotmem_truncate_two", test_slotmem_truncate_two, "write", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.slotmem_call_append", test_slotmem_call_append, "write", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.slotmem_timer_truncate", test_slotmem_timer_truncate, "write", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Register our handlers for different message types. */
    ValkeyModule_RegisterClusterMessageReceiver(ctx, MSGTYPE_DING, DingReceiver);
    ValkeyModule_RegisterClusterMessageReceiver(ctx, MSGTYPE_DONG, DongReceiver);

    return VALKEYMODULE_OK;
}
