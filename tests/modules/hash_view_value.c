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

int hashSetValueView(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);

    ValkeyModule_AutoMemory(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_WRITE);

    size_t buf_len;
    const char *buf = ValkeyModule_StringPtrLen(argv[3], &buf_len);
    bufferNode *node = addBuffer(buf, buf_len);

    int result = ValkeyModule_HashSetValueView(key, argv[2], node->buf, node->len);
    return ValkeyModule_ReplyWithLongLong(ctx, result);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "hash.set_view", 1, VALKEYMODULE_APIVER_1) ==
        VALKEYMODULE_OK &&
        ValkeyModule_CreateCommand(ctx, "hash.set_view", hashSetValueView, "write",
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
