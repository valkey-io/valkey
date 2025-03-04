#include "valkeymodule.h"
#include <strings.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED(V) ((void) V)

int shared_sds_cnt = 0;
ValkeyModuleSharedSDS *shared_sds_arr[4];

void *shared_sds_alloc(size_t len, size_t *alloc) {
    *alloc = 2 * len;
    return malloc(*alloc * sizeof(char));
}

void shared_sds_free_cb(void *ptr, size_t size) {
    UNUSED(size);
    UNUSED(ptr);
}

void check_hash_get_shared_sds(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int result) {
    if (result == 0) return;
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    for (int i = 0; i < shared_sds_cnt; ++i) {
        ValkeyModuleSharedSDS *get_value = NULL;
        ValkeyModule_HashGet(key, VALKEYMODULE_HASH_SHAREBLE_VALUES,
                                     argv[i*2 + 3], &get_value, NULL);
        if (get_value) {
            ValkeyModule_Assert(shared_sds_arr[i] == get_value);
        }
    }
}

/* If a string is ":deleted:", the special value for deleted hash fields is
 * returned; otherwise the input string is returned. */
static void *value_or_delete(ValkeyModuleCtx *ctx, ValkeyModuleString *s, int flags) {
    size_t str_len;
    const char *str = ValkeyModule_StringPtrLen(s, &str_len);
    if (flags & VALKEYMODULE_HASH_SHAREBLE_VALUES) {
        ValkeyModuleSharedSDS *shared_sds = ValkeyModule_CreateSharedSDS(ctx, str_len, shared_sds_alloc, shared_sds_free_cb);
        size_t sds_len;
        char *sds_str = ValkeyModule_SharedSDSPtrLen(shared_sds, &sds_len);
        ValkeyModule_Assert(sds_len == str_len);
        memcpy(sds_str, str, str_len);
        shared_sds_arr[shared_sds_cnt++] = shared_sds;
        return shared_sds;
    }
    shared_sds_arr[shared_sds_cnt++] = NULL;
    if (!strcasecmp(str, ":delete:"))
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

    shared_sds_cnt = 0;
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
        case 's': flags |= VALKEYMODULE_HASH_SHAREBLE_VALUES; break;
        }
    }

    /* Test some varargs. (In real-world, use a loop and set one at a time.) */
    int result;
    errno = 0;
    if (argc == 5) {
        result = ValkeyModule_HashSet(key, flags,
                                     argv[3], value_or_delete(ctx, argv[4], flags),
                                     NULL);
    } else if (argc == 7) {
        result = ValkeyModule_HashSet(key, flags,
                                     argv[3], value_or_delete(ctx, argv[4], flags),
                                     argv[5], value_or_delete(ctx, argv[6], flags),
                                     NULL);
    } else if (argc == 9) {
        result = ValkeyModule_HashSet(key, flags,
                                     argv[3], value_or_delete(ctx, argv[4], flags),
                                     argv[5], value_or_delete(ctx, argv[6], flags),
                                     argv[7], value_or_delete(ctx, argv[8], flags),
                                     NULL);
    } else if (argc == 11) {
        result = ValkeyModule_HashSet(key, flags,
                                     argv[3], value_or_delete(ctx, argv[4], flags),
                                     argv[5], value_or_delete(ctx, argv[6], flags),
                                     argv[7], value_or_delete(ctx, argv[8], flags),
                                     argv[9], value_or_delete(ctx, argv[10], flags),
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
    check_hash_get_shared_sds(ctx, argv, result);
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
