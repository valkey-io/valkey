#include "valkeymodule.h"

#define UNUSED(V) ((void) V)

int cmd_set(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int cmd_get(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);

    if (argc > 4) /* For testing */
        return ValkeyModule_WrongArity(ctx);

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int cmd_get_fullname(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    const char *command_name = ValkeyModule_GetCurrentCommandName(ctx);
    ValkeyModule_ReplyWithSimpleString(ctx, command_name);
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "subcommands", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Module command names cannot contain special characters. */
    ValkeyModule_Assert(ValkeyModule_CreateCommand(ctx,"subcommands.char\r",NULL,"",0,0,0) == VALKEYMODULE_ERR);
    ValkeyModule_Assert(ValkeyModule_CreateCommand(ctx,"subcommands.char\n",NULL,"",0,0,0) == VALKEYMODULE_ERR);
    ValkeyModule_Assert(ValkeyModule_CreateCommand(ctx,"subcommands.char ",NULL,"",0,0,0) == VALKEYMODULE_ERR);

    if (ValkeyModule_CreateCommand(ctx,"subcommands.bitarray",NULL,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    ValkeyModuleCommand *parent = ValkeyModule_GetCommand(ctx,"subcommands.bitarray");

    if (ValkeyModule_CreateSubcommand(parent,"set",cmd_set,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Module subcommand names cannot contain special characters. */
    ValkeyModule_Assert(ValkeyModule_CreateSubcommand(parent,"char|",cmd_set,"",0,0,0) == VALKEYMODULE_ERR);
    ValkeyModule_Assert(ValkeyModule_CreateSubcommand(parent,"char@",cmd_set,"",0,0,0) == VALKEYMODULE_ERR);
    ValkeyModule_Assert(ValkeyModule_CreateSubcommand(parent,"char=",cmd_set,"",0,0,0) == VALKEYMODULE_ERR);

    ValkeyModuleCommand *subcmd = ValkeyModule_GetCommand(ctx,"subcommands.bitarray|set");
    ValkeyModuleCommandInfo cmd_set_info = {
        .version = VALKEYMODULE_COMMAND_INFO_VERSION,
        .key_specs = (ValkeyModuleCommandKeySpec[]){
            {
                .flags = VALKEYMODULE_CMD_KEY_RW | VALKEYMODULE_CMD_KEY_UPDATE,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {0}
        }
    };
    if (ValkeyModule_SetCommandInfo(subcmd, &cmd_set_info) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateSubcommand(parent,"get",cmd_get,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    subcmd = ValkeyModule_GetCommand(ctx,"subcommands.bitarray|get");
    ValkeyModuleCommandInfo cmd_get_info = {
        .version = VALKEYMODULE_COMMAND_INFO_VERSION,
        .key_specs = (ValkeyModuleCommandKeySpec[]){
            {
                .flags = VALKEYMODULE_CMD_KEY_RO | VALKEYMODULE_CMD_KEY_ACCESS,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {0}
        }
    };
    if (ValkeyModule_SetCommandInfo(subcmd, &cmd_get_info) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Get the name of the command currently running. */
    if (ValkeyModule_CreateCommand(ctx,"subcommands.parent_get_fullname",cmd_get_fullname,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Get the name of the subcommand currently running. */
    if (ValkeyModule_CreateCommand(ctx,"subcommands.sub",NULL,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModuleCommand *fullname_parent = ValkeyModule_GetCommand(ctx,"subcommands.sub");
    if (ValkeyModule_CreateSubcommand(fullname_parent,"get_fullname",cmd_get_fullname,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Sanity */

    /* Trying to create the same subcommand fails */
    ValkeyModule_Assert(ValkeyModule_CreateSubcommand(parent,"get",NULL,"",0,0,0) == VALKEYMODULE_ERR);

    /* Trying to create a sub-subcommand fails */
    ValkeyModule_Assert(ValkeyModule_CreateSubcommand(subcmd,"get",NULL,"",0,0,0) == VALKEYMODULE_ERR);

    return VALKEYMODULE_OK;
}
