#ifndef _EVAL_H_
#define _EVAL_H_

typedef struct scriptingEngine scriptingEngine;

void evalInit(void);
void evalReset(int async);
void evalRemoveScriptsFromEngine(scriptingEngine *engine);
void *evalActiveDefragScript(void *ptr);

#endif /* _EVAL_H_ */
