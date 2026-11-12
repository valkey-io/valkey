#include "valkeymodule.h"

#include <stdint.h>
#include <string.h>

static size_t external_memory_used = 0;

void InfoFunc(ValkeyModuleInfoCtx *ctx, int for_crash_report) {
    ValkeyModule_InfoAddSection(ctx, "");
    ValkeyModule_InfoAddFieldLongLong(ctx, "global", -2);
    ValkeyModule_InfoAddFieldULongLong(ctx, "uglobal", (unsigned long long)-2);

    ValkeyModule_InfoAddSection(ctx, "Spanish");
    ValkeyModule_InfoAddFieldCString(ctx, "uno", "one");
    ValkeyModule_InfoAddFieldLongLong(ctx, "dos", 2);

    ValkeyModule_InfoAddSection(ctx, "Italian");
    ValkeyModule_InfoAddFieldLongLong(ctx, "due", 2);
    ValkeyModule_InfoAddFieldDouble(ctx, "tre", 3.3);

    ValkeyModule_InfoAddSection(ctx, "keyspace");
    ValkeyModule_InfoBeginDictField(ctx, "db0");
    ValkeyModule_InfoAddFieldLongLong(ctx, "keys", 3);
    ValkeyModule_InfoAddFieldLongLong(ctx, "expires", 1);
    ValkeyModule_InfoEndDictField(ctx);

    ValkeyModule_InfoAddSection(ctx, "unsafe");
    ValkeyModule_InfoBeginDictField(ctx, "unsafe:field");
    ValkeyModule_InfoAddFieldLongLong(ctx, "value", 1);
    ValkeyModule_InfoEndDictField(ctx);

    if (for_crash_report) {
        ValkeyModule_InfoAddSection(ctx, "Klingon");
        ValkeyModule_InfoAddFieldCString(ctx, "one", "wa'");
        ValkeyModule_InfoAddFieldCString(ctx, "two", "cha'");
        ValkeyModule_InfoAddFieldCString(ctx, "three", "wej");
    }

}

int info_get(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc, char field_type)
{
    if (argc != 3 && argc != 4) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    int err = VALKEYMODULE_OK;
    const char *section, *field;
    section = ValkeyModule_StringPtrLen(argv[1], NULL);
    field = ValkeyModule_StringPtrLen(argv[2], NULL);
    ValkeyModuleServerInfoData *info = ValkeyModule_GetServerInfo(ctx, section);
    if (field_type=='i') {
        long long ll = ValkeyModule_ServerInfoGetFieldSigned(info, field, &err);
        if (err==VALKEYMODULE_OK)
            ValkeyModule_ReplyWithLongLong(ctx, ll);
    } else if (field_type=='u') {
        unsigned long long ll = (unsigned long long)ValkeyModule_ServerInfoGetFieldUnsigned(info, field, &err);
        if (err==VALKEYMODULE_OK)
            ValkeyModule_ReplyWithLongLong(ctx, ll);
    } else if (field_type=='d') {
        double d = ValkeyModule_ServerInfoGetFieldDouble(info, field, &err);
        if (err==VALKEYMODULE_OK)
            ValkeyModule_ReplyWithDouble(ctx, d);
    } else if (field_type=='c') {
        const char *str = ValkeyModule_ServerInfoGetFieldC(info, field);
        if (str)
            ValkeyModule_ReplyWithCString(ctx, str);
    } else {
        ValkeyModuleString *str = ValkeyModule_ServerInfoGetField(ctx, info, field);
        if (str) {
            ValkeyModule_ReplyWithString(ctx, str);
            ValkeyModule_FreeString(ctx, str);
        } else
            err=VALKEYMODULE_ERR;
    }
    if (err!=VALKEYMODULE_OK)
        ValkeyModule_ReplyWithError(ctx, "not found");
    ValkeyModule_FreeServerInfo(ctx, info);
    return VALKEYMODULE_OK;
}

int info_gets(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return info_get(ctx, argv, argc, 's');
}

int info_getc(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return info_get(ctx, argv, argc, 'c');
}

int info_geti(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return info_get(ctx, argv, argc, 'i');
}

int info_getu(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return info_get(ctx, argv, argc, 'u');
}

int info_getd(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    return info_get(ctx, argv, argc, 'd');
}

int info_setexternal(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    long long amount_ll;
    size_t amount;

    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    if (ValkeyModule_StringToLongLong(argv[1], &amount_ll) != VALKEYMODULE_OK || amount_ll < 0 ||
        (unsigned long long)amount_ll > SIZE_MAX) {
        ValkeyModule_ReplyWithError(ctx, "ERR invalid external memory value");
        return VALKEYMODULE_OK;
    }

    amount = amount_ll;
    if (amount > external_memory_used) {
        if (ValkeyModule_IncrExternalMemory(amount - external_memory_used) != VALKEYMODULE_OK) {
            ValkeyModule_ReplyWithError(ctx, "ERR external memory increment failed");
            return VALKEYMODULE_OK;
        }
    } else if (amount < external_memory_used) {
        if (ValkeyModule_DecrExternalMemory(external_memory_used - amount) != VALKEYMODULE_OK) {
            ValkeyModule_ReplyWithError(ctx, "ERR external memory decrement failed");
            return VALKEYMODULE_OK;
        }
    }

    external_memory_used = amount;
    ValkeyModule_ReplyWithLongLong(ctx, amount_ll);
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    VALKEYMODULE_NOT_USED(ctx);

    if (external_memory_used != 0 &&
        ValkeyModule_DecrExternalMemory(external_memory_used) != VALKEYMODULE_OK) {
        return VALKEYMODULE_ERR;
    }

    external_memory_used = 0;
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx,"infotest",1,VALKEYMODULE_APIVER_1)
            == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_RegisterInfoFunc(ctx, InfoFunc) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"info.gets", info_gets,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"info.getc", info_getc,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"info.geti", info_geti,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"info.getu", info_getu,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"info.getd", info_getd,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"info.setexternal", info_setexternal,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
