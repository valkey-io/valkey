/* Test module to verify that addReplyDeferredLen / setDeferredReply work
 * correctly when the deferred reply buffer is active.
 *
 * The module subscribes to NOTIFY_STRING keyspace notifications.  When armed,
 * the notification callback blocks the client with a reply callback that
 * builds a nested array using VALKEYMODULE_POSTPONED_LEN (which internally
 * calls addReplyDeferredLen).
 *
 * Without the fix in networking.c the placeholder node for the inner array
 * ends up in c->reply while the outer array header and elements live in
 * c->deferred_reply, producing a malformed response after
 * commitDeferredReplyBuffer joins the two lists. */

#include "valkeymodule.h"
#include <pthread.h>
#include <unistd.h>

static int armed = 0;

static void *UnblockThread(void *arg) {
    ValkeyModuleBlockedClient *bc = arg;
    usleep(100000); /* 100 ms */
    ValkeyModule_UnblockClient(bc, NULL);
    return NULL;
}

/* Reply callback – builds a two-element array where the second element is
 * itself an array whose length is set via POSTPONED_LEN.
 * Expected response: ["first", ["a", "b"]] */
static int ReplyCallback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    ValkeyModule_ReplyWithArray(ctx, 2);
    ValkeyModule_ReplyWithSimpleString(ctx, "first");
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    ValkeyModule_ReplyWithSimpleString(ctx, "a");
    ValkeyModule_ReplyWithSimpleString(ctx, "b");
    ValkeyModule_ReplySetArrayLength(ctx, 2);
    return VALKEYMODULE_OK;
}

static int OnKeyspaceNotification(ValkeyModuleCtx *ctx, int type,
                                  const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);

    if (!armed) return VALKEYMODULE_OK;
    armed = 0;

    ValkeyModuleBlockedClient *bc =
        ValkeyModule_BlockClient(ctx, ReplyCallback, NULL, NULL, 0);
    if (!bc) return VALKEYMODULE_OK;

    pthread_t tid;
    if (pthread_create(&tid, NULL, UnblockThread, bc) != 0) {
        ValkeyModule_UnblockClient(bc, NULL);
    } else {
        pthread_detach(tid);
    }
    return VALKEYMODULE_OK;
}

/* DEFERRED_REPLY.ARM – arms the notification handler so the next
 * NOTIFY_STRING event blocks the client. */
static int CmdArm(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    armed = 1;
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "deferred_reply", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_STRING,
                                               OnKeyspaceNotification) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "deferred_reply.arm", CmdArm, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
