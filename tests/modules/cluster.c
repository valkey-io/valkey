#include "valkeymodule.h"

#define UNUSED(x) (void)(x)
#define MSGTYPE_TEST_UAF 3
#define MSGTYPE_TEST_MAX 254

static void testReceiver(ValkeyModuleCtx *ctx,
                         const char *sender_id,
                         uint8_t type,
                         const unsigned char *payload,
                         uint32_t len) {
    ValkeyModule_Log(ctx, "notice", "DING (type %d) RECEIVED from %.*s: '%.*s'",
                    type, VALKEYMODULE_NODE_ID_LEN, sender_id, (int)len, (const char *)payload);
}

static int testRegisterReceiver(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_RegisterClusterMessageReceiver(ctx, MSGTYPE_TEST_UAF, testReceiver);
    ValkeyModule_RegisterClusterMessageReceiver(ctx, MSGTYPE_TEST_MAX, testReceiver);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int testUnregisterReceiver(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_RegisterClusterMessageReceiver(ctx, MSGTYPE_TEST_UAF, NULL);
    ValkeyModule_RegisterClusterMessageReceiver(ctx, MSGTYPE_TEST_MAX, NULL);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int testSendMessage(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_SendClusterMessage(ctx, NULL, MSGTYPE_TEST_UAF, "TestUAF", 7);
    ValkeyModule_SendClusterMessage(ctx, NULL, MSGTYPE_TEST_MAX, "TestMAX", 7);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "cluster", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.register_receiver", testRegisterReceiver, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.unregister_receiver", testUnregisterReceiver, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx, "test.send_msg_uaf", testSendMessage, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
