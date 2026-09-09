#include "valkeymodule.h"

#define UNUSED(V) ((void) V)

/* This function implements all commands in this module. All we care about is
 * the COMMAND metadata anyway. */
int kspec_impl(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    /* Handle getkeys-api introspection (for "kspec.nonewithgetkeys")  */
    if (ValkeyModule_IsKeysPositionRequest(ctx)) {
        for (int i = 1; i < argc; i += 2)
            ValkeyModule_KeyAtPosWithFlags(ctx, i, VALKEYMODULE_CMD_KEY_RO | VALKEYMODULE_CMD_KEY_ACCESS);

        return VALKEYMODULE_OK;
    }

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int createKspecNone(ValkeyModuleCtx *ctx) {
    /* A command without keyspecs; only the legacy (first,last,step) triple (MSET like spec). */
    if (ValkeyModule_CreateCommand(ctx,"kspec.none",kspec_impl,"",1,-1,2) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    return VALKEYMODULE_OK;
}

int createKspecNoneWithGetkeys(ValkeyModuleCtx *ctx) {
    /* A command without keyspecs; only the legacy (first,last,step) triple (MSET like spec), but also has a getkeys callback */
    if (ValkeyModule_CreateCommand(ctx,"kspec.nonewithgetkeys",kspec_impl,"getkeys-api",1,-1,2) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    return VALKEYMODULE_OK;
}

int createKspecTwoRanges(ValkeyModuleCtx *ctx) {
    /* Test that two position/range-based key specs are combined to produce the
     * legacy (first,last,step) values representing both keys. */
    if (ValkeyModule_CreateCommand(ctx,"kspec.tworanges",kspec_impl,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModuleCommand *command = ValkeyModule_GetCommand(ctx,"kspec.tworanges");
    ValkeyModuleCommandInfo info = {
        .version = VALKEYMODULE_COMMAND_INFO_VERSION,
        .arity = -2,
        .key_specs = (ValkeyModuleCommandKeySpec[]){
            {
                .flags = VALKEYMODULE_CMD_KEY_RO | VALKEYMODULE_CMD_KEY_ACCESS,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {
                .flags = VALKEYMODULE_CMD_KEY_RW | VALKEYMODULE_CMD_KEY_UPDATE,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 2,
                /* Omitted find_keys_type is shorthand for RANGE {0,1,0} */
            },
            {0}
        }
    };
    if (ValkeyModule_SetCommandInfo(command, &info) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}

int createKspecTwoRangesWithGap(ValkeyModuleCtx *ctx) {
    /* Test that two position/range-based key specs are combined to produce the
     * legacy (first,last,step) values representing just one key. */
    if (ValkeyModule_CreateCommand(ctx,"kspec.tworangeswithgap",kspec_impl,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModuleCommand *command = ValkeyModule_GetCommand(ctx,"kspec.tworangeswithgap");
    ValkeyModuleCommandInfo info = {
        .version = VALKEYMODULE_COMMAND_INFO_VERSION,
        .arity = -2,
        .key_specs = (ValkeyModuleCommandKeySpec[]){
            {
                .flags = VALKEYMODULE_CMD_KEY_RO | VALKEYMODULE_CMD_KEY_ACCESS,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {
                .flags = VALKEYMODULE_CMD_KEY_RW | VALKEYMODULE_CMD_KEY_UPDATE,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 3,
                /* Omitted find_keys_type is shorthand for RANGE {0,1,0} */
            },
            {0}
        }
    };
    if (ValkeyModule_SetCommandInfo(command, &info) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}

int createKspecKeyword(ValkeyModuleCtx *ctx) {
    /* Only keyword-based specs. The legacy triple is wiped and set to (0,0,0). */
    if (ValkeyModule_CreateCommand(ctx,"kspec.keyword",kspec_impl,"",3,-1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModuleCommand *command = ValkeyModule_GetCommand(ctx,"kspec.keyword");
    ValkeyModuleCommandInfo info = {
        .version = VALKEYMODULE_COMMAND_INFO_VERSION,
        .key_specs = (ValkeyModuleCommandKeySpec[]){
            {
                .flags = VALKEYMODULE_CMD_KEY_RO | VALKEYMODULE_CMD_KEY_ACCESS,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_KEYWORD,
                .bs.keyword.keyword = "KEYS",
                .bs.keyword.startfrom = 1,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_RANGE,
                .fk.range = {-1,1,0}
            },
            {0}
        }
    };
    if (ValkeyModule_SetCommandInfo(command, &info) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}

int createKspecComplex1(ValkeyModuleCtx *ctx) {
    /* First is a range a single key. The rest are keyword-based specs. */
    if (ValkeyModule_CreateCommand(ctx,"kspec.complex1",kspec_impl,"",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModuleCommand *command = ValkeyModule_GetCommand(ctx,"kspec.complex1");
    ValkeyModuleCommandInfo info = {
        .version = VALKEYMODULE_COMMAND_INFO_VERSION,
        .key_specs = (ValkeyModuleCommandKeySpec[]){
            {
                .flags = VALKEYMODULE_CMD_KEY_RO,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
            },
            {
                .flags = VALKEYMODULE_CMD_KEY_RW | VALKEYMODULE_CMD_KEY_UPDATE,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_KEYWORD,
                .bs.keyword.keyword = "STORE",
                .bs.keyword.startfrom = 2,
            },
            {
                .flags = VALKEYMODULE_CMD_KEY_RO | VALKEYMODULE_CMD_KEY_ACCESS,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_KEYWORD,
                .bs.keyword.keyword = "KEYS",
                .bs.keyword.startfrom = 2,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_KEYNUM,
                .fk.keynum = {0,1,1}
            },
            {0}
        }
    };
    if (ValkeyModule_SetCommandInfo(command, &info) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}

int createKspecComplex2(ValkeyModuleCtx *ctx) {
    /* First is not legacy, more than STATIC_KEYS_SPECS_NUM specs */
    if (ValkeyModule_CreateCommand(ctx,"kspec.complex2",kspec_impl,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModuleCommand *command = ValkeyModule_GetCommand(ctx,"kspec.complex2");
    ValkeyModuleCommandInfo info = {
        .version = VALKEYMODULE_COMMAND_INFO_VERSION,
        .key_specs = (ValkeyModuleCommandKeySpec[]){
            {
                .flags = VALKEYMODULE_CMD_KEY_RW | VALKEYMODULE_CMD_KEY_UPDATE,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_KEYWORD,
                .bs.keyword.keyword = "STORE",
                .bs.keyword.startfrom = 5,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {
                .flags = VALKEYMODULE_CMD_KEY_RO | VALKEYMODULE_CMD_KEY_ACCESS,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {
                .flags = VALKEYMODULE_CMD_KEY_RO | VALKEYMODULE_CMD_KEY_ACCESS,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 2,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_RANGE,
                .fk.range = {0,1,0}
            },
            {
                .flags = VALKEYMODULE_CMD_KEY_RW | VALKEYMODULE_CMD_KEY_UPDATE,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 3,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_KEYNUM,
                .fk.keynum = {0,1,1}
            },
            {
                .flags = VALKEYMODULE_CMD_KEY_RW | VALKEYMODULE_CMD_KEY_UPDATE,
                .begin_search_type = VALKEYMODULE_KSPEC_BS_KEYWORD,
                .bs.keyword.keyword = "MOREKEYS",
                .bs.keyword.startfrom = 5,
                .find_keys_type = VALKEYMODULE_KSPEC_FK_RANGE,
                .fk.range = {-1,1,0}
            },
            {0}
        }
    };
    if (ValkeyModule_SetCommandInfo(command, &info) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "keyspecs", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (createKspecNone(ctx) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    if (createKspecNoneWithGetkeys(ctx) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    if (createKspecTwoRanges(ctx) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    if (createKspecTwoRangesWithGap(ctx) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    if (createKspecKeyword(ctx) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    if (createKspecComplex1(ctx) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    if (createKspecComplex2(ctx) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    return VALKEYMODULE_OK;
}
