#ifndef EVAL_H
#define EVAL_H

typedef struct scriptingEngine scriptingEngine;

void evalInit(void);
void evalReset(int async);
void evalRemoveScriptsFromEngine(scriptingEngine *engine);
void *evalActiveDefragScript(void *ptr);

#endif /* EVAL_H */
