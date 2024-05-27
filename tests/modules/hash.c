#include "valkeymodule.h"
#include <strings.h>
#include <errno.h>
#include <stdlib.h>

/* If a string is ":deleted:", the special value for deleted hash fields is
 * returned; otherwise the input string is returned. */
static ValkeyModuleString *value_or_delete(ValkeyModuleString *s) {
    if (!strcasecmp(ValkeyModule_StringPtrLen(s, NULL), ":delete:"))
        return VALKEYMODULE_HASH_DELETE;
    else
        return s;
}

/* HASH.SET key flags field1 value1 [field2 value2 ..]
 *
 * Sets 1-4 fields. Returns the same as ValkeyModule_HashSet().
 * Flags is a string of "nxa" where n = NX, x = XX, a = COUNT_ALL.
 * To delete a field, use the value ":delete:".
 */
int hash_set(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 5 || argc % 2 == 0 || argc > 11)
        return ValkeyModule_WrongArity(ctx);

    ValkeyModule_AutoMemory(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);

    size_t flags_len;
    const char *flags_str = ValkeyModule_StringPtrLen(argv[2], &flags_len);
    int flags = VALKEYMODULE_HASH_NONE;
    for (size_t i = 0; i < flags_len; i++) {
        switch (flags_str[i]) {
        case 'n': flags |= VALKEYMODULE_HASH_NX; break;
        case 'x': flags |= VALKEYMODULE_HASH_XX; break;
        case 'a': flags |= VALKEYMODULE_HASH_COUNT_ALL; break;
        }
    }

    /* Test some varargs. (In real-world, use a loop and set one at a time.) */
    int result;
    errno = 0;
    if (argc == 5) {
        result = ValkeyModule_HashSet(key, flags,
                                     argv[3], value_or_delete(argv[4]),
                                     NULL);
    } else if (argc == 7) {
        result = ValkeyModule_HashSet(key, flags,
                                     argv[3], value_or_delete(argv[4]),
                                     argv[5], value_or_delete(argv[6]),
                                     NULL);
    } else if (argc == 9) {
        result = ValkeyModule_HashSet(key, flags,
                                     argv[3], value_or_delete(argv[4]),
                                     argv[5], value_or_delete(argv[6]),
                                     argv[7], value_or_delete(argv[8]),
                                     NULL);
    } else if (argc == 11) {
        result = ValkeyModule_HashSet(key, flags,
                                     argv[3], value_or_delete(argv[4]),
                                     argv[5], value_or_delete(argv[6]),
                                     argv[7], value_or_delete(argv[8]),
                                     argv[9], value_or_delete(argv[10]),
                                     NULL);
    } else {
        return ValkeyModule_ReplyWithError(ctx, "ERR too many fields");
    }

    /* Check errno */
    if (result == 0) {
        if (errno == ENOTSUP)
            return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
        else
            ValkeyModule_Assert(errno == ENOENT);
    }

    return ValkeyModule_ReplyWithLongLong(ctx, result);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "hash", 1, VALKEYMODULE_APIVER_1) ==
        VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "hash.set", hash_set, "write",
                                  1, 1, 1) == VALKEYMODULE_OK) {
        return VALKEYMODULE_OK;
    } else {
        return VALKEYMODULE_ERR;
    }
}
