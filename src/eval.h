#ifndef _EVAL_H_
#define _EVAL_H_

/* Note: this header requires server.h to be included first (provides uint64_t,
 * struct listNode, and sets _FILE_OFFSET_BITS=64 before system headers). */

/* Forward declarations */
typedef struct scriptingEngine scriptingEngine;

typedef struct evalScript {
    void *script; /* compiledFunction* */
    void *engine; /* scriptingEngine* */
    void *body;   /* robj* */
    uint64_t flags;
    struct listNode *node; /* list node in scripts_lru_list list. */
    char sha[40];          /* SHA1 hex, no null terminator (use memcmp/memcpy with length 40) */
} evalScript;

void evalInit(void);
void evalReset(int async);
void evalRemoveScriptsFromEngine(scriptingEngine *engine);
void evalDefragScripts(void *(*defragfn)(void *));

#endif /* _EVAL_H_ */
