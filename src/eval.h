#ifndef _EVAL_H_
#define _EVAL_H_

typedef struct scriptingEngine scriptingEngine;

void evalInit(void);
void evalReset(int async);
void evalRemoveScriptsOfEngine(scriptingEngine *engine, const char *engine_name);
void *evalActiveDefragScript(void *ptr);

#endif /* _EVAL_H_ */
