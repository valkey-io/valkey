#include "valkeymodule.h"
#include <strings.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

/* A sample with declarable channels, that are used to validate against ACLs */
int getChannels_subscribe(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if ((argc - 1) % 3 != 0) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    char *err = NULL;
    
    /* getchannels.command [[subscribe|unsubscribe|publish] [pattern|literal] <channel> ...]
     * This command marks the given channel is accessed based on the
     * provided modifiers. */
    for (int i = 1; i < argc; i += 3) {
        const char *operation = ValkeyModule_StringPtrLen(argv[i], NULL);
        const char *type = ValkeyModule_StringPtrLen(argv[i+1], NULL);
        int flags = 0;

        if (!strcasecmp(operation, "subscribe")) {
            flags |= VALKEYMODULE_CMD_CHANNEL_SUBSCRIBE;
        } else if (!strcasecmp(operation, "unsubscribe")) {
            flags |= VALKEYMODULE_CMD_CHANNEL_UNSUBSCRIBE;
        } else if (!strcasecmp(operation, "publish")) {
            flags |= VALKEYMODULE_CMD_CHANNEL_PUBLISH;
        } else {
            err = "Invalid channel operation";
            break;
        }

        if (!strcasecmp(type, "literal")) {
            /* No op */
        } else if (!strcasecmp(type, "pattern")) {
            flags |= VALKEYMODULE_CMD_CHANNEL_PATTERN;
        } else {
            err = "Invalid channel type";
            break;
        }
        if (ValkeyModule_IsChannelsPositionRequest(ctx)) {
            ValkeyModule_ChannelAtPosWithFlags(ctx, i+2, flags);
        }
    }

    if (!ValkeyModule_IsChannelsPositionRequest(ctx)) {
        if (err) {
            ValkeyModule_ReplyWithError(ctx, err);
        } else {
            /* Normal implementation would go here, but for tests just return okay */
            ValkeyModule_ReplyWithSimpleString(ctx, "OK");
        }
    }

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "getchannels", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "getchannels.command", getChannels_subscribe, "getchannels-api", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
