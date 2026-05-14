#ifndef _EVAL_H_
#define _EVAL_H_

typedef struct scriptingEngine scriptingEngine;
typedef struct serverObject robj;
typedef struct listNode listNode;
typedef struct ValkeyModuleScriptingEngineCompiledFunction compiledFunction;

typedef struct evalScript {
    char sha[41];               /* SHA1 hex string (null-terminated key) */
    compiledFunction *script;
    scriptingEngine *engine;
    robj *body;
    uint64_t flags;
    listNode *node; /* list node in scripts_lru_list list. */
} evalScript;

void evalInit(void);
void evalReset(int async);
void evalRemoveScriptsFromEngine(scriptingEngine *engine);
void *evalActiveDefragScript(void *ptr);

#endif /* _EVAL_H_ */
