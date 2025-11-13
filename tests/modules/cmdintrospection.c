#include "valkeymodule.h"

#define UNUSED(V) ((void) V)

int cmd_xadd(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "cmdintrospection", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"cmdintrospection.xadd",cmd_xadd,"write deny-oom random fast",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModuleCommand *xadd = ValkeyModule_GetCommand(ctx,"cmdintrospection.xadd");

    ValkeyModuleCommandInfo info = {
        .version = VALKEYMODULE_COMMAND_INFO_VERSION,
        .arity = -5,
        .summary = "Appends a new message to a stream. Creates the key if it doesn't exist.",
        .since = "5.0.0",
        .complexity = "O(1) when adding a new entry, O(N) when trimming where N being the number of entries evicted.",
        .tips = "nondeterministic_output",
        .history = (ValkeyModuleCommandHistoryEntry[]){
            /* NOTE: All versions specified should be the module's versions, not
             * the server's! We use server versions in this example for the purpose of
             * testing (comparing the output with the output of the vanilla
             * XADD). */
            {"6.2.0", "Added the `NOMKSTREAM` option, `MINID` trimming strategy and the `LIMIT` option."},
            {"7.0.0", "Added support for the `<ms>-*` explicit ID form."},
            {0}
        },
        .key_specs = (ValkeyModuleCommandKeySpec[]){
            {
                .notes = "UPDATE instead of INSERT because of the optional trimming feature",
                .flags = VALKEYMODULE_CMD_KEY_RW | VALKEYMODULE_CMD_KEY_UPDATE,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {0}
        },
        .args = (ValkeyModuleCommandArg[]){
            {
                .name = "key",
                .type = VALKEYMODULE_ARG_TYPE_KEY,
                .key_spec_index = 0
            },
            {
                .name = "nomkstream",
                .type = VALKEYMODULE_ARG_TYPE_PURE_TOKEN,
                .token = "NOMKSTREAM",
                .since = "6.2.0",
                .flags = VALKEYMODULE_CMD_ARG_OPTIONAL
            },
            {
                .name = "trim",
                .type = VALKEYMODULE_ARG_TYPE_BLOCK,
                .flags = VALKEYMODULE_CMD_ARG_OPTIONAL,
                .subargs = (ValkeyModuleCommandArg[]){
                    {
                        .name = "strategy",
                        .type = VALKEYMODULE_ARG_TYPE_ONEOF,
                        .subargs = (ValkeyModuleCommandArg[]){
                            {
                                .name = "maxlen",
                                .type = VALKEYMODULE_ARG_TYPE_PURE_TOKEN,
                                .token = "MAXLEN",
                            },
                            {
                                .name = "minid",
                                .type = VALKEYMODULE_ARG_TYPE_PURE_TOKEN,
                                .token = "MINID",
                                .since = "6.2.0",
                            },
                            {0}
                        }
                    },
                    {
                        .name = "operator",
                        .type = VALKEYMODULE_ARG_TYPE_ONEOF,
                        .flags = VALKEYMODULE_CMD_ARG_OPTIONAL,
                        .subargs = (ValkeyModuleCommandArg[]){
                            {
                                .name = "equal",
                                .type = VALKEYMODULE_ARG_TYPE_PURE_TOKEN,
                                .token = "="
                            },
                            {
                                .name = "approximately",
                                .type = VALKEYMODULE_ARG_TYPE_PURE_TOKEN,
                                .token = "~"
                            },
                            {0}
                        }
                    },
                    {
                        .name = "threshold",
                        .type = VALKEYMODULE_ARG_TYPE_STRING,
                        .display_text = "threshold" /* Just for coverage, doesn't have a visible effect */
                    },
                    {
                        .name = "count",
                        .type = VALKEYMODULE_ARG_TYPE_INTEGER,
                        .token = "LIMIT",
                        .since = "6.2.0",
                        .flags = VALKEYMODULE_CMD_ARG_OPTIONAL
                    },
                    {0}
                }
            },
            {
                .name = "id-selector",
                .type = VALKEYMODULE_ARG_TYPE_ONEOF,
                .subargs = (ValkeyModuleCommandArg[]){
                    {
                        .name = "auto-id",
                        .type = VALKEYMODULE_ARG_TYPE_PURE_TOKEN,
                        .token = "*"
                    },
                    {
                        .name = "id",
                        .type = VALKEYMODULE_ARG_TYPE_STRING,
                    },
                    {0}
                }
            },
            {
                .name = "data",
                .type = VALKEYMODULE_ARG_TYPE_BLOCK,
                .flags = VALKEYMODULE_CMD_ARG_MULTIPLE,
                .subargs = (ValkeyModuleCommandArg[]){
                    {
                        .name = "field",
                        .type = VALKEYMODULE_ARG_TYPE_STRING,
                    },
                    {
                        .name = "value",
                        .type = VALKEYMODULE_ARG_TYPE_STRING,
                    },
                    {0}
                }
            },
            {0}
        }
    };
    if (ValkeyModule_SetCommandInfo(xadd, &info) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
