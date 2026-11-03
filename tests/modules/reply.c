/* 
 * A module the tests RM_ReplyWith family of commands
 */

#include "valkeymodule.h"
#include <math.h>

int rw_string(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    return ValkeyModule_ReplyWithString(ctx, argv[1]);
}

int rw_cstring(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);

    return ValkeyModule_ReplyWithSimpleString(ctx, "A simple string");
}

int rw_int(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    long long integer;
    if (ValkeyModule_StringToLongLong(argv[1], &integer) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "Arg cannot be parsed as an integer");

    return ValkeyModule_ReplyWithLongLong(ctx, integer);
}

/* When one argument is given, it is returned as a double,
 * when two arguments are given, it returns a/b. */
int rw_double(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc==1)
        return ValkeyModule_ReplyWithDouble(ctx, NAN);

    if (argc != 2 && argc != 3) return ValkeyModule_WrongArity(ctx);

    double dbl, dbl2;
    if (ValkeyModule_StringToDouble(argv[1], &dbl) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "Arg cannot be parsed as a double");
    if (argc == 3) {
        if (ValkeyModule_StringToDouble(argv[2], &dbl2) != VALKEYMODULE_OK)
            return ValkeyModule_ReplyWithError(ctx, "Arg cannot be parsed as a double");
        dbl /= dbl2;
    }

    return ValkeyModule_ReplyWithDouble(ctx, dbl);
}

int rw_longdouble(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    long double longdbl;
    if (ValkeyModule_StringToLongDouble(argv[1], &longdbl) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "Arg cannot be parsed as a double");

    return ValkeyModule_ReplyWithLongDouble(ctx, longdbl);
}

int rw_bignumber(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    size_t bignum_len;
    const char *bignum_str = ValkeyModule_StringPtrLen(argv[1], &bignum_len);

    return ValkeyModule_ReplyWithBigNumber(ctx, bignum_str, bignum_len);
}

int rw_array(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    long long integer;
    if (ValkeyModule_StringToLongLong(argv[1], &integer) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    ValkeyModule_ReplyWithArray(ctx, integer);
    for (int i = 0; i < integer; ++i) {
        ValkeyModule_ReplyWithLongLong(ctx, i);
    }

    return VALKEYMODULE_OK;
}

int rw_map(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    long long integer;
    if (ValkeyModule_StringToLongLong(argv[1], &integer) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    ValkeyModule_ReplyWithMap(ctx, integer);
    for (int i = 0; i < integer; ++i) {
        ValkeyModule_ReplyWithLongLong(ctx, i);
        ValkeyModule_ReplyWithDouble(ctx, i * 1.5);
    }

    return VALKEYMODULE_OK;
}

int rw_set(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    long long integer;
    if (ValkeyModule_StringToLongLong(argv[1], &integer) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    ValkeyModule_ReplyWithSet(ctx, integer);
    for (int i = 0; i < integer; ++i) {
        ValkeyModule_ReplyWithLongLong(ctx, i);
    }

    return VALKEYMODULE_OK;
}

int rw_attribute(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    long long integer;
    if (ValkeyModule_StringToLongLong(argv[1], &integer) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "Arg cannot be parsed as a integer");

    if (ValkeyModule_ReplyWithAttribute(ctx, integer) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx, "Attributes aren't supported by RESP 2");
    }

    for (int i = 0; i < integer; ++i) {
        ValkeyModule_ReplyWithLongLong(ctx, i);
        ValkeyModule_ReplyWithDouble(ctx, i * 1.5);
    }

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int rw_bool(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);

    ValkeyModule_ReplyWithArray(ctx, 2);
    ValkeyModule_ReplyWithBool(ctx, 0);
    return ValkeyModule_ReplyWithBool(ctx, 1);
}

int rw_null(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);

    return ValkeyModule_ReplyWithNull(ctx);
}

int rw_error(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) return ValkeyModule_WrongArity(ctx);

    return ValkeyModule_ReplyWithError(ctx, "An error");
}

int rw_error_format(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);

    return ValkeyModule_ReplyWithErrorFormat(ctx,
                                            ValkeyModule_StringPtrLen(argv[1], NULL),
                                            ValkeyModule_StringPtrLen(argv[2], NULL));
}

int rw_verbatim(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    size_t verbatim_len;
    const char *verbatim_str = ValkeyModule_StringPtrLen(argv[1], &verbatim_len);

    return ValkeyModule_ReplyWithVerbatimString(ctx, verbatim_str, verbatim_len);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "replywith", 1, VALKEYMODULE_APIVER_1) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"rw.string",rw_string,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.cstring",rw_cstring,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.bignumber",rw_bignumber,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.int",rw_int,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.double",rw_double,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.longdouble",rw_longdouble,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.array",rw_array,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.map",rw_map,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.attribute",rw_attribute,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.set",rw_set,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.bool",rw_bool,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.null",rw_null,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.error",rw_error,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.error_format",rw_error_format,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"rw.verbatim",rw_verbatim,"",0,0,0) != VALKEYMODULE_OK)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
