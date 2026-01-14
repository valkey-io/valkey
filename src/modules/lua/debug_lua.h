#ifndef _LUA_DEBUG_H_
#define _LUA_DEBUG_H_

#include "../../valkeymodule.h"

typedef struct lua_State lua_State;
typedef struct client client;

void ldbInit(void);
int ldbIsEnabled(void);
void ldbDisable(void);
void ldbEnable(void);
int ldbIsActive(void);
void ldbStart(ValkeyModuleString *source);
void ldbEnd(void);
void ldbLog(ValkeyModuleString *entry);
void ldbLogCString(const char *c_str);
void ldbSendLogs(void);
void ldbLogRespReply(char *reply);

int ldbGetCurrentLine(void);
void ldbSetCurrentLine(int line);
void ldbLogSourceLine(int lnum);
ValkeyModuleString *ldbCatStackValue(ValkeyModuleString *s, lua_State *lua, int idx);
void ldbSetBreakpointOnNextLine(int enable);
int ldbIsBreakpointOnNextLineEnabled(void);
int ldbShouldBreak(void);
int ldbIsStepEnabled(void);
void ldbSetStepMode(int enable);

int ldbRepl(lua_State *lua);
void ldbGenerateDebuggerCommandsArray(lua_State *lua,
                                      const ValkeyModuleScriptingEngineDebuggerCommand **commands,
                                      size_t *commands_len);

#endif /* _LUA_DEBUG_H_ */
