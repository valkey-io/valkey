#ifndef _FUNCTION_LUA_H_
#define _FUNCTION_LUA_H_

#include "../../valkeymodule.h"
#include "engine_structs.h"

void luaFunctionInitializeLuaState(luaEngineCtx *ctx, lua_State *lua);

ValkeyModuleScriptingEngineCompiledFunction **luaFunctionLibraryCreate(lua_State *lua,
                                                                       const char *code,
                                                                       size_t timeout,
                                                                       size_t *out_num_compiled_functions,
                                                                       ValkeyModuleString **err);

void luaFunctionFreeFunction(lua_State *lua, void *function);

#endif /* _FUNCTION_LUA_H_ */
