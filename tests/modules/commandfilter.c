#include "valkeymodule.h"

#include <string.h>
#include <strings.h>

static ValkeyModuleString *log_key_name;

static const char log_command_name[] = "commandfilter.log";
static const char ping_command_name[] = "commandfilter.ping";
static const char retained_command_name[] = "commandfilter.retained";
static const char unregister_command_name[] = "commandfilter.unregister";
static const char unfiltered_clientid_name[] = "unfilter_clientid";
static int in_log_command = 0;

unsigned long long unfiltered_clientid = 0;

static ValkeyModuleCommandFilter *filter, *filter1;
static ValkeyModuleString *retained;

int CommandFilter_UnregisterCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    (void) argc;
    (void) argv;

    ValkeyModule_ReplyWithLongLong(ctx,
            ValkeyModule_UnregisterCommandFilter(ctx, filter));

    return VALKEYMODULE_OK;
}

int CommandFilter_PingCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    (void) argc;
    (void) argv;

    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "ping", "c", "@log");
    if (reply) {
        ValkeyModule_ReplyWithCallReply(ctx, reply);
        ValkeyModule_FreeCallReply(reply);
    } else {
        ValkeyModule_ReplyWithSimpleString(ctx, "Unknown command or invalid arguments");
    }

    return VALKEYMODULE_OK;
}

int CommandFilter_Retained(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    (void) argc;
    (void) argv;

    if (retained) {
        ValkeyModule_ReplyWithString(ctx, retained);
    } else {
        ValkeyModule_ReplyWithNull(ctx);
    }

    return VALKEYMODULE_OK;
}

int CommandFilter_LogCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    ValkeyModuleString *s = ValkeyModule_CreateString(ctx, "", 0);

    int i;
    for (i = 1; i < argc; i++) {
        size_t arglen;
        const char *arg = ValkeyModule_StringPtrLen(argv[i], &arglen);

        if (i > 1) ValkeyModule_StringAppendBuffer(ctx, s, " ", 1);
        ValkeyModule_StringAppendBuffer(ctx, s, arg, arglen);
    }

    ValkeyModuleKey *log = ValkeyModule_OpenKey(ctx, log_key_name, VALKEYMODULE_WRITE|VALKEYMODULE_READ);
    ValkeyModule_ListPush(log, VALKEYMODULE_LIST_HEAD, s);
    ValkeyModule_CloseKey(log);
    ValkeyModule_FreeString(ctx, s);

    in_log_command = 1;

    size_t cmdlen;
    const char *cmdname = ValkeyModule_StringPtrLen(argv[1], &cmdlen);
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, cmdname, "v", &argv[2], argc - 2);
    if (reply) {
        ValkeyModule_ReplyWithCallReply(ctx, reply);
        ValkeyModule_FreeCallReply(reply);
    } else {
        ValkeyModule_ReplyWithSimpleString(ctx, "Unknown command or invalid arguments");
    }

    in_log_command = 0;

    return VALKEYMODULE_OK;
}

int CommandFilter_UnfilteredClientId(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc < 2)
        return ValkeyModule_WrongArity(ctx);

    long long id;
    if (ValkeyModule_StringToLongLong(argv[1], &id) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "invalid client id");
        return VALKEYMODULE_OK;
    }
    if (id < 0) {
        ValkeyModule_ReplyWithError(ctx, "invalid client id");
        return VALKEYMODULE_OK;
    }

    unfiltered_clientid = id;
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

/* Filter to protect against Bug #11894 reappearing
 *
 * ensures that the filter is only run the first time through, and not on reprocessing
 */
void CommandFilter_BlmoveSwap(ValkeyModuleCommandFilterCtx *filter)
{
    if (ValkeyModule_CommandFilterArgsCount(filter) != 6)
        return;

    ValkeyModuleString *arg = ValkeyModule_CommandFilterArgGet(filter, 0);
    size_t arg_len;
    const char *arg_str = ValkeyModule_StringPtrLen(arg, &arg_len);

    if (arg_len != 6 || strncmp(arg_str, "blmove", 6))
        return;

    /*
     * Swapping directional args (right/left) from source and destination.
     * need to hold here, can't push into the ArgReplace func, as it will cause other to freed -> use after free
     */
    ValkeyModuleString *dir1 = ValkeyModule_HoldString(NULL, ValkeyModule_CommandFilterArgGet(filter, 3));
    ValkeyModuleString *dir2 = ValkeyModule_HoldString(NULL, ValkeyModule_CommandFilterArgGet(filter, 4));
    ValkeyModule_CommandFilterArgReplace(filter, 3, dir2);
    ValkeyModule_CommandFilterArgReplace(filter, 4, dir1);
}

void CommandFilter_CommandFilter(ValkeyModuleCommandFilterCtx *filter)
{
    unsigned long long id = ValkeyModule_CommandFilterGetClientId(filter);
    if (id == unfiltered_clientid) return;

    if (in_log_command) return;  /* don't process our own RM_Call() from CommandFilter_LogCommand() */

    /* Fun manipulations:
     * - Remove @delme
     * - Replace @replaceme
     * - Append @insertbefore or @insertafter
     * - Prefix with Log command if @log encountered
     */
    int log = 0;
    int pos = 0;
    while (pos < ValkeyModule_CommandFilterArgsCount(filter)) {
        const ValkeyModuleString *arg = ValkeyModule_CommandFilterArgGet(filter, pos);
        size_t arg_len;
        const char *arg_str = ValkeyModule_StringPtrLen(arg, &arg_len);

        if (arg_len == 6 && !memcmp(arg_str, "@delme", 6)) {
            ValkeyModule_CommandFilterArgDelete(filter, pos);
            continue;
        } 
        if (arg_len == 10 && !memcmp(arg_str, "@replaceme", 10)) {
            ValkeyModule_CommandFilterArgReplace(filter, pos,
                    ValkeyModule_CreateString(NULL, "--replaced--", 12));
        } else if (arg_len == 13 && !memcmp(arg_str, "@insertbefore", 13)) {
            ValkeyModule_CommandFilterArgInsert(filter, pos,
                    ValkeyModule_CreateString(NULL, "--inserted-before--", 19));
            pos++;
        } else if (arg_len == 12 && !memcmp(arg_str, "@insertafter", 12)) {
            ValkeyModule_CommandFilterArgInsert(filter, pos + 1,
                    ValkeyModule_CreateString(NULL, "--inserted-after--", 18));
            pos++;
        } else if (arg_len == 7 && !memcmp(arg_str, "@retain", 7)) {
            if (retained) ValkeyModule_FreeString(NULL, retained);
            retained = ValkeyModule_CommandFilterArgGet(filter, pos + 1);
            ValkeyModule_RetainString(NULL, retained);
            pos++;
        } else if (arg_len == 4 && !memcmp(arg_str, "@log", 4)) {
            log = 1;
        }
        pos++;
    }

    if (log) ValkeyModule_CommandFilterArgInsert(filter, 0,
            ValkeyModule_CreateString(NULL, log_command_name, sizeof(log_command_name)-1));
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (ValkeyModule_Init(ctx,"commandfilter",1,VALKEYMODULE_APIVER_1)
            == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (argc != 2 && argc != 3) {
        ValkeyModule_Log(ctx, "warning", "Log key name not specified");
        return VALKEYMODULE_ERR;
    }

    long long noself = 0;
    log_key_name = ValkeyModule_CreateStringFromString(ctx, argv[0]);
    ValkeyModule_StringToLongLong(argv[1], &noself);
    retained = NULL;

    if (ValkeyModule_CreateCommand(ctx,log_command_name,
                CommandFilter_LogCommand,"write deny-oom",1,1,1) == VALKEYMODULE_ERR)
            return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,ping_command_name,
                CommandFilter_PingCommand,"deny-oom",1,1,1) == VALKEYMODULE_ERR)
            return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,retained_command_name,
                CommandFilter_Retained,"readonly",1,1,1) == VALKEYMODULE_ERR)
            return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,unregister_command_name,
                CommandFilter_UnregisterCommand,"write deny-oom",1,1,1) == VALKEYMODULE_ERR)
            return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, unfiltered_clientid_name,
                CommandFilter_UnfilteredClientId, "admin", 1,1,1) == VALKEYMODULE_ERR)
            return VALKEYMODULE_ERR;

    if ((filter = ValkeyModule_RegisterCommandFilter(ctx, CommandFilter_CommandFilter, 
                    noself ? VALKEYMODULE_CMDFILTER_NOSELF : 0))
            == NULL) return VALKEYMODULE_ERR;

    if ((filter1 = ValkeyModule_RegisterCommandFilter(ctx, CommandFilter_BlmoveSwap, 0)) == NULL)
        return VALKEYMODULE_ERR;

    if (argc == 3) {
        const char *ptr = ValkeyModule_StringPtrLen(argv[2], NULL);
        if (!strcasecmp(ptr, "noload")) {
            /* This is a hint that we return ERR at the last moment of OnLoad. */
            ValkeyModule_FreeString(ctx, log_key_name);
            if (retained) ValkeyModule_FreeString(NULL, retained);
            return VALKEYMODULE_ERR;
        }
    }

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    ValkeyModule_FreeString(ctx, log_key_name);
    if (retained) ValkeyModule_FreeString(NULL, retained);

    return VALKEYMODULE_OK;
}
