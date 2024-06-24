#include "valkeymodule.h"
#include <assert.h>
#include <errno.h>
#include <strings.h>

/* LIST.GETALL key [REVERSE] */
int list_getall(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 2 || argc > 3) return ValkeyModule_WrongArity(ctx);
    int reverse = (argc == 3 &&
                   !strcasecmp(ValkeyModule_StringPtrLen(argv[2], NULL),
                               "REVERSE"));
    ValkeyModule_AutoMemory(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    if (ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_LIST) {
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }
    long n = ValkeyModule_ValueLength(key);
    ValkeyModule_ReplyWithArray(ctx, n);
    if (!reverse) {
        for (long i = 0; i < n; i++) {
            ValkeyModuleString *elem = ValkeyModule_ListGet(key, i);
            ValkeyModule_ReplyWithString(ctx, elem);
            ValkeyModule_FreeString(ctx, elem);
        }
    } else {
        for (long i = -1; i >= -n; i--) {
            ValkeyModuleString *elem = ValkeyModule_ListGet(key, i);
            ValkeyModule_ReplyWithString(ctx, elem);
            ValkeyModule_FreeString(ctx, elem);
        }
    }

    /* Test error condition: index out of bounds */
    assert(ValkeyModule_ListGet(key, n) == NULL);
    assert(errno == EDOM); /* no more elements in list */

    /* ValkeyModule_CloseKey(key); //implicit, done by auto memory */
    return VALKEYMODULE_OK;
}

/* LIST.EDIT key [REVERSE] cmdstr [value ..]
 *
 * cmdstr is a string of the following characters:
 *
 *     k -- keep
 *     d -- delete
 *     i -- insert value from args
 *     r -- replace with value from args
 *
 * The number of occurrences of "i" and "r" in cmdstr) should correspond to the
 * number of args after cmdstr.
 *
 * Reply with a RESP3 Map, containing the number of edits (inserts, replaces, deletes)
 * performed, as well as the last index and the entry it points to.
 */
int list_edit(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 3) return ValkeyModule_WrongArity(ctx);
    ValkeyModule_AutoMemory(ctx);
    int argpos = 1; /* the next arg */

    /* key */
    int keymode = VALKEYMODULE_READ | VALKEYMODULE_WRITE;
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[argpos++], keymode);
    if (ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_LIST) {
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }

    /* REVERSE */
    int reverse = 0;
    if (argc >= 4 &&
        !strcasecmp(ValkeyModule_StringPtrLen(argv[argpos], NULL), "REVERSE")) {
        reverse = 1;
        argpos++;
    }

    /* cmdstr */
    size_t cmdstr_len;
    const char *cmdstr = ValkeyModule_StringPtrLen(argv[argpos++], &cmdstr_len);

    /* validate cmdstr vs. argc */
    long num_req_args = 0;
    long min_list_length = 0;
    for (size_t cmdpos = 0; cmdpos < cmdstr_len; cmdpos++) {
        char c = cmdstr[cmdpos];
        if (c == 'i' || c == 'r') num_req_args++;
        if (c == 'd' || c == 'r' || c == 'k') min_list_length++;
    }
    if (argc < argpos + num_req_args) {
        return ValkeyModule_ReplyWithError(ctx, "ERR too few args");
    }
    if ((long)ValkeyModule_ValueLength(key) < min_list_length) {
        return ValkeyModule_ReplyWithError(ctx, "ERR list too short");
    }

    /* Iterate over the chars in cmdstr (edit instructions) */
    long long num_inserts = 0, num_deletes = 0, num_replaces = 0;
    long index = reverse ? -1 : 0;
    ValkeyModuleString *value;

    for (size_t cmdpos = 0; cmdpos < cmdstr_len; cmdpos++) {
        switch (cmdstr[cmdpos]) {
        case 'i': /* insert */
            value = argv[argpos++];
            assert(ValkeyModule_ListInsert(key, index, value) == VALKEYMODULE_OK);
            index += reverse ? -1 : 1;
            num_inserts++;
            break;
        case 'd': /* delete */
            assert(ValkeyModule_ListDelete(key, index) == VALKEYMODULE_OK);
            num_deletes++;
            break;
        case 'r': /* replace */
            value = argv[argpos++];
            assert(ValkeyModule_ListSet(key, index, value) == VALKEYMODULE_OK);
            index += reverse ? -1 : 1;
            num_replaces++;
            break;
        case 'k': /* keep */
            index += reverse ? -1 : 1;
            break;
        }
    }

    ValkeyModuleString *v = ValkeyModule_ListGet(key, index);
    ValkeyModule_ReplyWithMap(ctx, v ? 5 : 4);
    ValkeyModule_ReplyWithCString(ctx, "i");
    ValkeyModule_ReplyWithLongLong(ctx, num_inserts);
    ValkeyModule_ReplyWithCString(ctx, "d");
    ValkeyModule_ReplyWithLongLong(ctx, num_deletes);
    ValkeyModule_ReplyWithCString(ctx, "r");
    ValkeyModule_ReplyWithLongLong(ctx, num_replaces);
    ValkeyModule_ReplyWithCString(ctx, "index");
    ValkeyModule_ReplyWithLongLong(ctx, index);
    if (v) {
        ValkeyModule_ReplyWithCString(ctx, "entry");
        ValkeyModule_ReplyWithString(ctx, v);
        ValkeyModule_FreeString(ctx, v);
    } 

    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* Reply based on errno as set by the List API functions. */
static int replyByErrno(ValkeyModuleCtx *ctx) {
    switch (errno) {
    case EDOM:
        return ValkeyModule_ReplyWithError(ctx, "ERR index out of bounds");
    case ENOTSUP:
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    default: assert(0); /* Can't happen */
    }
}

/* LIST.GET key index */
int list_get(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    long long index;
    if (ValkeyModule_StringToLongLong(argv[2], &index) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx, "ERR index must be a number");
    }
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    ValkeyModuleString *value = ValkeyModule_ListGet(key, index);
    if (value) {
        ValkeyModule_ReplyWithString(ctx, value);
        ValkeyModule_FreeString(ctx, value);
    } else {
        replyByErrno(ctx);
    }
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* LIST.SET key index value */
int list_set(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    long long index;
    if (ValkeyModule_StringToLongLong(argv[2], &index) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "ERR index must be a number");
        return VALKEYMODULE_OK;
    }
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    if (ValkeyModule_ListSet(key, index, argv[3]) == VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        replyByErrno(ctx);
    }
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* LIST.INSERT key index value
 *
 * If index is negative, value is inserted after, otherwise before the element
 * at index.
 */
int list_insert(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    long long index;
    if (ValkeyModule_StringToLongLong(argv[2], &index) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "ERR index must be a number");
        return VALKEYMODULE_OK;
    }
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    if (ValkeyModule_ListInsert(key, index, argv[3]) == VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        replyByErrno(ctx);
    }
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* LIST.DELETE key index */
int list_delete(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);
    long long index;
    if (ValkeyModule_StringToLongLong(argv[2], &index) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "ERR index must be a number");
        return VALKEYMODULE_OK;
    }
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    if (ValkeyModule_ListDelete(key, index) == VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    } else {
        replyByErrno(ctx);
    }
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "list", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "list.getall", list_getall, "",
                                  1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "list.edit", list_edit, "write",
                                  1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "list.get", list_get, "write",
                                  1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "list.set", list_set, "write",
                                  1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "list.insert", list_insert, "write",
                                  1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "list.delete", list_delete, "write",
                                  1, 1, 1) == VALKEYMODULE_OK) {
        return VALKEYMODULE_OK;
    } else {
        return VALKEYMODULE_ERR;
    }
}
