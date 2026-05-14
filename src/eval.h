#ifndef _EVAL_H_
#define _EVAL_H_

#include <stdint.h>

typedef struct listNode listNode;
typedef struct scriptingEngine scriptingEngine;

typedef struct evalScript {
    void *script; /* compiledFunction* */
    void *engine; /* scriptingEngine* */
    void *body;   /* robj* */
    uint64_t flags;
    listNode *node; /* list node in scripts_lru_list list. */
    char sha[40];   /* SHA1 hex, no null terminator (use memcmp/memcpy with length 40) */
} evalScript;

void evalInit(void);
void evalReset(int async);
void evalRemoveScriptsFromEngine(scriptingEngine *engine);
void *evalActiveDefragScript(void *ptr);

#endif /* _EVAL_H_ */
