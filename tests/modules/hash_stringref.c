/* Module Test: Verifies the module's capability to share an owned buffer with the core, 
 * which is then stored in a hash key field using a non-owning string reference (stringRef). */
#include "valkeymodule.h"
#include <string.h>

typedef struct bufferNode {
    char *buf;
    size_t len;
    struct bufferNode *next;
} bufferNode;

bufferNode *head = NULL;

bufferNode *addBuffer(const char *buf, size_t len) {
    if (!buf || len == 0) return NULL;

    bufferNode *node = malloc(sizeof(bufferNode));
    node->buf = malloc(len);
    memcpy(node->buf, buf, len);
    node->len = len;
    node->next = head;
    head = node;
    return node;
}

void freeBufferList(void) {
    bufferNode *current = head;
    while (current) {
        bufferNode *next = current->next;
        free(current->buf);
        free(current);
        current = next;
    }
}

/* HASH.HAS_STRINGREF key field
 *
 * Returns 1 if all of the following conditions are met for the hash field:
 * 1. The key exists.
 * 2. The key's value is a HASH type.
 * 3. The field's value is a string reference (stringRef) type.
 * Otherwise, returns 0.
 *
 * Parameters:
 * 1. The hash entry key.
 * 2. The hahs entry field.
 */
int hashHasStringRef(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);

    ValkeyModule_AutoMemory(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);

    int result = ValkeyModule_HashHasStringRef(key, argv[2]);
    return ValkeyModule_ReplyWithLongLong(ctx, result);
}

/* HASH.SET_STRINGREF key field buffer
 *
 * Sets hash entry value of a given key and field to an external owned buffer.
 * Parameters:
 * 1. The hash entry key.
 * 2. The hahs entry field.
 * 3. The buffer to share with the core.
 */
int hashSetStringRef(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);

    ValkeyModule_AutoMemory(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);

    size_t buf_len;
    const char *buf = ValkeyModule_StringPtrLen(argv[3], &buf_len);
    bufferNode *node = addBuffer(buf, buf_len);

    int result = ValkeyModule_HashSetStringRef(key, argv[2], node->buf, node->len);
    if (result == 0) return ValkeyModule_ReplyWithLongLong(ctx, result);
    return ValkeyModule_ReplyWithError(ctx, "Err");
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "hash.stringref", 1, VALKEYMODULE_APIVER_1) ==
        VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "hash.set_stringref", hashSetStringRef, "write",
                                  1, 1, 1) == VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "hash.has_stringref", hashHasStringRef, "readonly",
                                  1, 1, 1) == VALKEYMODULE_OK) {
        return VALKEYMODULE_OK;
    }
    return VALKEYMODULE_ERR;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    VALKEYMODULE_NOT_USED(ctx);
    freeBufferList();
    return VALKEYMODULE_OK;
}
