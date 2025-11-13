#include "valkeymodule.h"

#include <string.h>
#include <strings.h>
#include <assert.h>
#include <unistd.h>

#define UNUSED(V) ((void) V)

#define LIST_SIZE 1024

/* The FSL (Fixed-Size List) data type is a low-budget imitation of the
 * list data type, in order to test list-like commands implemented
 * by a module.
 * Examples: FSL.PUSH, FSL.BPOP, etc. */

typedef struct {
    long long list[LIST_SIZE];
    long long length;
} fsl_t; /* Fixed-size list */

static ValkeyModuleType *fsltype = NULL;

fsl_t *fsl_type_create(void) {
    fsl_t *o;
    o = ValkeyModule_Alloc(sizeof(*o));
    o->length = 0;
    return o;
}

void fsl_type_free(fsl_t *o) {
    ValkeyModule_Free(o);
}

/* ========================== "fsltype" type methods ======================= */

void *fsl_rdb_load(ValkeyModuleIO *rdb, int encver) {
    if (encver != 0) {
        return NULL;
    }
    fsl_t *fsl = fsl_type_create();
    fsl->length = ValkeyModule_LoadUnsigned(rdb);
    for (long long i = 0; i < fsl->length; i++)
        fsl->list[i] = ValkeyModule_LoadSigned(rdb);
    return fsl;
}

void fsl_rdb_save(ValkeyModuleIO *rdb, void *value) {
    fsl_t *fsl = value;
    ValkeyModule_SaveUnsigned(rdb,fsl->length);
    for (long long i = 0; i < fsl->length; i++)
        ValkeyModule_SaveSigned(rdb, fsl->list[i]);
}

void fsl_aofrw(ValkeyModuleIO *aof, ValkeyModuleString *key, void *value) {
    fsl_t *fsl = value;
    for (long long i = 0; i < fsl->length; i++)
        ValkeyModule_EmitAOF(aof, "FSL.PUSH","sl", key, fsl->list[i]);
}

void fsl_free(void *value) {
    fsl_type_free(value);
}

/* ========================== helper methods ======================= */

/* Wrapper to the boilerplate code of opening a key, checking its type, etc.
 * Returns 0 if `keyname` exists in the dataset, but it's of the wrong type (i.e. not FSL) */
int get_fsl(ValkeyModuleCtx *ctx, ValkeyModuleString *keyname, int mode, int create, fsl_t **fsl, int reply_on_failure) {
    *fsl = NULL;
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, keyname, mode);

    if (ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_EMPTY) {
        /* Key exists */
        if (ValkeyModule_ModuleTypeGetType(key) != fsltype) {
            /* Key is not FSL */
            ValkeyModule_CloseKey(key);
            if (reply_on_failure)
                ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
            ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "INCR", "c", "fsl_wrong_type");
            ValkeyModule_FreeCallReply(reply);
            return 0;
        }

        *fsl = ValkeyModule_ModuleTypeGetValue(key);
        if (*fsl && !(*fsl)->length && mode & VALKEYMODULE_WRITE) {
            /* Key exists, but it's logically empty */
            if (create) {
                create = 0; /* No need to create, key exists in its basic state */
            } else {
                ValkeyModule_DeleteKey(key);
                *fsl = NULL;
            }
        } else {
            /* Key exists, and has elements in it - no need to create anything */
            create = 0;
        }
    }

    if (create) {
        *fsl = fsl_type_create();
        ValkeyModule_ModuleTypeSetValue(key, fsltype, *fsl);
    }

    ValkeyModule_CloseKey(key);
    return 1;
}

/* ========================== commands ======================= */

/* FSL.PUSH <key> <int> - Push an integer to the fixed-size list (to the right).
 * It must be greater than the element in the head of the list. */
int fsl_push(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3)
        return ValkeyModule_WrongArity(ctx);

    long long ele;
    if (ValkeyModule_StringToLongLong(argv[2],&ele) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid integer");

    fsl_t *fsl;
    if (!get_fsl(ctx, argv[1], VALKEYMODULE_WRITE, 1, &fsl, 1))
        return VALKEYMODULE_OK;

    if (fsl->length == LIST_SIZE)
        return ValkeyModule_ReplyWithError(ctx,"ERR list is full");

    if (fsl->length != 0 && fsl->list[fsl->length-1] >= ele)
        return ValkeyModule_ReplyWithError(ctx,"ERR new element has to be greater than the head element");

    fsl->list[fsl->length++] = ele;
    ValkeyModule_SignalKeyAsReady(ctx, argv[1]);

    ValkeyModule_ReplicateVerbatim(ctx);

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

typedef struct {
    ValkeyModuleString *keyname;
    long long ele;
} timer_data_t;

static void timer_callback(ValkeyModuleCtx *ctx, void *data)
{
    timer_data_t *td = data;

    fsl_t *fsl;
    if (!get_fsl(ctx, td->keyname, VALKEYMODULE_WRITE, 1, &fsl, 1))
        return;

    if (fsl->length == LIST_SIZE)
        return; /* list is full */

    if (fsl->length != 0 && fsl->list[fsl->length-1] >= td->ele)
        return; /* new element has to be greater than the head element */

    fsl->list[fsl->length++] = td->ele;
    ValkeyModule_SignalKeyAsReady(ctx, td->keyname);

    ValkeyModule_Replicate(ctx, "FSL.PUSH", "sl", td->keyname, td->ele);

    ValkeyModule_FreeString(ctx, td->keyname);
    ValkeyModule_Free(td);
}

/* FSL.PUSHTIMER <key> <int> <period-in-ms> - Push the number 9000 to the fixed-size list (to the right).
 * It must be greater than the element in the head of the list. */
int fsl_pushtimer(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 4)
        return ValkeyModule_WrongArity(ctx);

    long long ele;
    if (ValkeyModule_StringToLongLong(argv[2],&ele) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid integer");

    long long period;
    if (ValkeyModule_StringToLongLong(argv[3],&period) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid period");

    fsl_t *fsl;
    if (!get_fsl(ctx, argv[1], VALKEYMODULE_WRITE, 1, &fsl, 1))
        return VALKEYMODULE_OK;

    if (fsl->length == LIST_SIZE)
        return ValkeyModule_ReplyWithError(ctx,"ERR list is full");

    timer_data_t *td = ValkeyModule_Alloc(sizeof(*td));
    td->keyname = argv[1];
    ValkeyModule_RetainString(ctx, td->keyname);
    td->ele = ele;

    ValkeyModuleTimerID id = ValkeyModule_CreateTimer(ctx, period, timer_callback, td);
    ValkeyModule_ReplyWithLongLong(ctx, id);

    return VALKEYMODULE_OK;
}

int bpop_reply_callback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModuleString *keyname = ValkeyModule_GetBlockedClientReadyKey(ctx);

    fsl_t *fsl;
    if (!get_fsl(ctx, keyname, VALKEYMODULE_WRITE, 0, &fsl, 0) || !fsl)
        return VALKEYMODULE_ERR;

    ValkeyModule_Assert(fsl->length);
    ValkeyModule_ReplyWithLongLong(ctx, fsl->list[--fsl->length]);

    /* I'm lazy so i'll replicate a potentially blocking command, it shouldn't block in this flow. */
    ValkeyModule_ReplicateVerbatim(ctx);
    return VALKEYMODULE_OK;
}

int bpop_timeout_callback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    return ValkeyModule_ReplyWithSimpleString(ctx, "Request timedout");
}

/* FSL.BPOP <key> <timeout> [NO_TO_CB]- Block clients until list has two or more elements.
 * When that happens, unblock client and pop the last two elements (from the right). */
int fsl_bpop(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 3)
        return ValkeyModule_WrongArity(ctx);

    long long timeout;
    if (ValkeyModule_StringToLongLong(argv[2],&timeout) != VALKEYMODULE_OK || timeout < 0)
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid timeout");

    int to_cb = 1;
    if (argc == 4) {
        if (strcasecmp("NO_TO_CB", ValkeyModule_StringPtrLen(argv[3], NULL)))
            return ValkeyModule_ReplyWithError(ctx,"ERR invalid argument");
        to_cb = 0;
    }

    fsl_t *fsl;
    if (!get_fsl(ctx, argv[1], VALKEYMODULE_WRITE, 0, &fsl, 1))
        return VALKEYMODULE_OK;

    if (!fsl) {
        ValkeyModule_BlockClientOnKeys(ctx, bpop_reply_callback, to_cb ? bpop_timeout_callback : NULL,
                                      NULL, timeout, &argv[1], 1, NULL);
    } else {
        ValkeyModule_Assert(fsl->length);
        ValkeyModule_ReplyWithLongLong(ctx, fsl->list[--fsl->length]);
        /* I'm lazy so i'll replicate a potentially blocking command, it shouldn't block in this flow. */
        ValkeyModule_ReplicateVerbatim(ctx);
    }

    return VALKEYMODULE_OK;
}

int bpopgt_reply_callback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModuleString *keyname = ValkeyModule_GetBlockedClientReadyKey(ctx);
    long long *pgt = ValkeyModule_GetBlockedClientPrivateData(ctx);

    fsl_t *fsl;
    if (!get_fsl(ctx, keyname, VALKEYMODULE_WRITE, 0, &fsl, 0) || !fsl)
        return ValkeyModule_ReplyWithError(ctx,"UNBLOCKED key no longer exists");

    if (fsl->list[fsl->length-1] <= *pgt)
        return VALKEYMODULE_ERR;

    ValkeyModule_Assert(fsl->length);
    ValkeyModule_ReplyWithLongLong(ctx, fsl->list[--fsl->length]);
    /* I'm lazy so i'll replicate a potentially blocking command, it shouldn't block in this flow. */
    ValkeyModule_ReplicateVerbatim(ctx);
    return VALKEYMODULE_OK;
}

int bpopgt_timeout_callback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    return ValkeyModule_ReplyWithSimpleString(ctx, "Request timedout");
}

void bpopgt_free_privdata(ValkeyModuleCtx *ctx, void *privdata) {
    VALKEYMODULE_NOT_USED(ctx);
    ValkeyModule_Free(privdata);
}

/* FSL.BPOPGT <key> <gt> <timeout> - Block clients until list has an element greater than <gt>.
 * When that happens, unblock client and pop the last element (from the right). */
int fsl_bpopgt(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4)
        return ValkeyModule_WrongArity(ctx);

    long long gt;
    if (ValkeyModule_StringToLongLong(argv[2],&gt) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid integer");

    long long timeout;
    if (ValkeyModule_StringToLongLong(argv[3],&timeout) != VALKEYMODULE_OK || timeout < 0)
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid timeout");

    fsl_t *fsl;
    if (!get_fsl(ctx, argv[1], VALKEYMODULE_WRITE, 0, &fsl, 1))
        return VALKEYMODULE_OK;

    if (!fsl)
        return ValkeyModule_ReplyWithError(ctx,"ERR key must exist");

    if (fsl->list[fsl->length-1] <= gt) {
        /* We use malloc so the tests in blockedonkeys.tcl can check for memory leaks */
        long long *pgt = ValkeyModule_Alloc(sizeof(long long));
        *pgt = gt;
        ValkeyModule_BlockClientOnKeysWithFlags(
            ctx, bpopgt_reply_callback, bpopgt_timeout_callback,
            bpopgt_free_privdata, timeout, &argv[1], 1, pgt,
            VALKEYMODULE_BLOCK_UNBLOCK_DELETED);
    } else {
        ValkeyModule_Assert(fsl->length);
        ValkeyModule_ReplyWithLongLong(ctx, fsl->list[--fsl->length]);
        /* I'm lazy so i'll replicate a potentially blocking command, it shouldn't block in this flow. */
        ValkeyModule_ReplicateVerbatim(ctx);
    }

    return VALKEYMODULE_OK;
}

int bpoppush_reply_callback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModuleString *src_keyname = ValkeyModule_GetBlockedClientReadyKey(ctx);
    ValkeyModuleString *dst_keyname = ValkeyModule_GetBlockedClientPrivateData(ctx);

    fsl_t *src;
    if (!get_fsl(ctx, src_keyname, VALKEYMODULE_WRITE, 0, &src, 0) || !src)
        return VALKEYMODULE_ERR;

    fsl_t *dst;
    if (!get_fsl(ctx, dst_keyname, VALKEYMODULE_WRITE, 1, &dst, 0) || !dst)
        return VALKEYMODULE_ERR;

    ValkeyModule_Assert(src->length);
    long long ele = src->list[--src->length];
    dst->list[dst->length++] = ele;
    ValkeyModule_SignalKeyAsReady(ctx, dst_keyname);
    /* I'm lazy so i'll replicate a potentially blocking command, it shouldn't block in this flow. */
    ValkeyModule_ReplicateVerbatim(ctx);
    return ValkeyModule_ReplyWithLongLong(ctx, ele);
}

int bpoppush_timeout_callback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    return ValkeyModule_ReplyWithSimpleString(ctx, "Request timedout");
}

void bpoppush_free_privdata(ValkeyModuleCtx *ctx, void *privdata) {
    ValkeyModule_FreeString(ctx, privdata);
}

/* FSL.BPOPPUSH <src> <dst> <timeout> - Block clients until <src> has an element.
 * When that happens, unblock client, pop the last element from <src> and push it to <dst>
 * (from the right). */
int fsl_bpoppush(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4)
        return ValkeyModule_WrongArity(ctx);

    long long timeout;
    if (ValkeyModule_StringToLongLong(argv[3],&timeout) != VALKEYMODULE_OK || timeout < 0)
        return ValkeyModule_ReplyWithError(ctx,"ERR invalid timeout");

    fsl_t *src;
    if (!get_fsl(ctx, argv[1], VALKEYMODULE_WRITE, 0, &src, 1))
        return VALKEYMODULE_OK;

    if (!src) {
        /* Retain string for reply callback */
        ValkeyModule_RetainString(ctx, argv[2]);
        /* Key is empty, we must block */
        ValkeyModule_BlockClientOnKeys(ctx, bpoppush_reply_callback, bpoppush_timeout_callback,
                                      bpoppush_free_privdata, timeout, &argv[1], 1, argv[2]);
    } else {
        fsl_t *dst;
        if (!get_fsl(ctx, argv[2], VALKEYMODULE_WRITE, 1, &dst, 1))
            return VALKEYMODULE_OK;

        ValkeyModule_Assert(src->length);
        long long ele = src->list[--src->length];
        dst->list[dst->length++] = ele;
        ValkeyModule_SignalKeyAsReady(ctx, argv[2]);
        ValkeyModule_ReplyWithLongLong(ctx, ele);
        /* I'm lazy so i'll replicate a potentially blocking command, it shouldn't block in this flow. */
        ValkeyModule_ReplicateVerbatim(ctx);
    }

    return VALKEYMODULE_OK;
}

/* FSL.GETALL <key> - Reply with an array containing all elements. */
int fsl_getall(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2)
        return ValkeyModule_WrongArity(ctx);

    fsl_t *fsl;
    if (!get_fsl(ctx, argv[1], VALKEYMODULE_READ, 0, &fsl, 1))
        return VALKEYMODULE_OK;

    if (!fsl)
        return ValkeyModule_ReplyWithArray(ctx, 0);

    ValkeyModule_ReplyWithArray(ctx, fsl->length);
    for (int i = 0; i < fsl->length; i++)
        ValkeyModule_ReplyWithLongLong(ctx, fsl->list[i]);
    return VALKEYMODULE_OK;
}

/* Callback for blockonkeys_popall */
int blockonkeys_popall_reply_callback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    if (ValkeyModule_KeyType(key) == VALKEYMODULE_KEYTYPE_LIST) {
        ValkeyModuleString *elem;
        long len = 0;
        ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_ARRAY_LEN);
        while ((elem = ValkeyModule_ListPop(key, VALKEYMODULE_LIST_HEAD)) != NULL) {
            len++;
            ValkeyModule_ReplyWithString(ctx, elem);
            ValkeyModule_FreeString(ctx, elem);
        }
        /* I'm lazy so i'll replicate a potentially blocking command, it shouldn't block in this flow. */
        ValkeyModule_ReplicateVerbatim(ctx);
        ValkeyModule_ReplySetArrayLength(ctx, len);
    } else {
        ValkeyModule_ReplyWithError(ctx, "ERR Not a list");
    }
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

int blockonkeys_popall_timeout_callback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    return ValkeyModule_ReplyWithError(ctx, "ERR Timeout");
}

/* BLOCKONKEYS.POPALL key
 *
 * Blocks on an empty key for up to 3 seconds. When unblocked by a list
 * operation like LPUSH, all the elements are popped and returned. Fails with an
 * error on timeout. */
int blockonkeys_popall(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2)
        return ValkeyModule_WrongArity(ctx);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ);
    if (ValkeyModule_KeyType(key) == VALKEYMODULE_KEYTYPE_EMPTY) {
        ValkeyModule_BlockClientOnKeys(ctx, blockonkeys_popall_reply_callback,
                                      blockonkeys_popall_timeout_callback,
                                      NULL, 3000, &argv[1], 1, NULL);
    } else {
        ValkeyModule_ReplyWithError(ctx, "ERR Key not empty");
    }
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* BLOCKONKEYS.LPUSH key val [val ..]
 * BLOCKONKEYS.LPUSH_UNBLOCK key val [val ..]
 *
 * A module equivalent of LPUSH. If the name LPUSH_UNBLOCK is used,
 * RM_SignalKeyAsReady() is also called. */
int blockonkeys_lpush(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 3)
        return ValkeyModule_WrongArity(ctx);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    if (ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_EMPTY &&
        ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_LIST) {
        ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    } else {
        for (int i = 2; i < argc; i++) {
            if (ValkeyModule_ListPush(key, VALKEYMODULE_LIST_HEAD,
                                     argv[i]) != VALKEYMODULE_OK) {
                ValkeyModule_CloseKey(key);
                return ValkeyModule_ReplyWithError(ctx, "ERR Push failed");
            }
        }
    }
    ValkeyModule_CloseKey(key);

    /* signal key as ready if the command is lpush_unblock */
    size_t len;
    const char *str = ValkeyModule_StringPtrLen(argv[0], &len);
    if (!strncasecmp(str, "blockonkeys.lpush_unblock", len)) {
        ValkeyModule_SignalKeyAsReady(ctx, argv[1]);
    }
    ValkeyModule_ReplicateVerbatim(ctx);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* Callback for the BLOCKONKEYS.BLPOPN command */
int blockonkeys_blpopn_reply_callback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argc);
    long long n;
    ValkeyModule_StringToLongLong(argv[2], &n);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    int result;
    if (ValkeyModule_KeyType(key) == VALKEYMODULE_KEYTYPE_LIST &&
        ValkeyModule_ValueLength(key) >= (size_t)n) {
        ValkeyModule_ReplyWithArray(ctx, n);
        for (long i = 0; i < n; i++) {
            ValkeyModuleString *elem = ValkeyModule_ListPop(key, VALKEYMODULE_LIST_HEAD);
            ValkeyModule_ReplyWithString(ctx, elem);
            ValkeyModule_FreeString(ctx, elem);
        }
        /* I'm lazy so i'll replicate a potentially blocking command, it shouldn't block in this flow. */
        ValkeyModule_ReplicateVerbatim(ctx);
        result = VALKEYMODULE_OK;
    } else if (ValkeyModule_KeyType(key) == VALKEYMODULE_KEYTYPE_LIST ||
               ValkeyModule_KeyType(key) == VALKEYMODULE_KEYTYPE_EMPTY) {
        const char *module_cmd = ValkeyModule_StringPtrLen(argv[0], NULL);
        if (!strcasecmp(module_cmd, "blockonkeys.blpopn_or_unblock"))
            ValkeyModule_UnblockClient(ValkeyModule_GetBlockedClientHandle(ctx), NULL);

        /* continue blocking */
        result = VALKEYMODULE_ERR;
    } else {
        result = ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }
    ValkeyModule_CloseKey(key);
    return result;
}

int blockonkeys_blpopn_timeout_callback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    return ValkeyModule_ReplyWithError(ctx, "ERR Timeout");
}

int blockonkeys_blpopn_abort_callback(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    return ValkeyModule_ReplyWithSimpleString(ctx, "Action aborted");
}

/* BLOCKONKEYS.BLPOPN key N
 *
 * Blocks until key has N elements and then pops them or fails after 3 seconds.
 */
int blockonkeys_blpopn(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 3) return ValkeyModule_WrongArity(ctx);

    long long n, timeout = 3000LL;
    if (ValkeyModule_StringToLongLong(argv[2], &n) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx, "ERR Invalid N");
    }

    if (argc > 3 ) {
        if (ValkeyModule_StringToLongLong(argv[3], &timeout) != VALKEYMODULE_OK) {
            return ValkeyModule_ReplyWithError(ctx, "ERR Invalid timeout value");
        }
    }
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);
    int keytype = ValkeyModule_KeyType(key);
    if (keytype != VALKEYMODULE_KEYTYPE_EMPTY &&
        keytype != VALKEYMODULE_KEYTYPE_LIST) {
        ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    } else if (keytype == VALKEYMODULE_KEYTYPE_LIST &&
               ValkeyModule_ValueLength(key) >= (size_t)n) {
        ValkeyModule_ReplyWithArray(ctx, n);
        for (long i = 0; i < n; i++) {
            ValkeyModuleString *elem = ValkeyModule_ListPop(key, VALKEYMODULE_LIST_HEAD);
            ValkeyModule_ReplyWithString(ctx, elem);
            ValkeyModule_FreeString(ctx, elem);
        }
        /* I'm lazy so i'll replicate a potentially blocking command, it shouldn't block in this flow. */
        ValkeyModule_ReplicateVerbatim(ctx);
    } else {
        ValkeyModule_BlockClientOnKeys(ctx, blockonkeys_blpopn_reply_callback,
                                      timeout ? blockonkeys_blpopn_timeout_callback : blockonkeys_blpopn_abort_callback,
                                      NULL, timeout, &argv[1], 1, NULL);
    }
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "blockonkeys", 1, VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModuleTypeMethods tm = {
        .version = VALKEYMODULE_TYPE_METHOD_VERSION,
        .rdb_load = fsl_rdb_load,
        .rdb_save = fsl_rdb_save,
        .aof_rewrite = fsl_aofrw,
        .mem_usage = NULL,
        .free = fsl_free,
        .digest = NULL,
    };

    fsltype = ValkeyModule_CreateDataType(ctx, "fsltype_t", 0, &tm);
    if (fsltype == NULL)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"fsl.push",fsl_push,"write",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"fsl.pushtimer",fsl_pushtimer,"write",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"fsl.bpop",fsl_bpop,"write",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"fsl.bpopgt",fsl_bpopgt,"write",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"fsl.bpoppush",fsl_bpoppush,"write",1,2,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"fsl.getall",fsl_getall,"",1,1,1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "blockonkeys.popall", blockonkeys_popall,
                                  "write", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "blockonkeys.lpush", blockonkeys_lpush,
                                  "write", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "blockonkeys.lpush_unblock", blockonkeys_lpush,
                                  "write", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "blockonkeys.blpopn", blockonkeys_blpopn,
                                  "write", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "blockonkeys.blpopn_or_unblock", blockonkeys_blpopn,
                                      "write", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    return VALKEYMODULE_OK;
}
