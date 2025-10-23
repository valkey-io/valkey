#ifndef _ENGINE_LUA_
#define _ENGINE_LUA_

#include "../scripting_engine.h"
#include <lua.h>

typedef struct luaFunction {
    lua_State *lua;   /* Pointer to the lua context where this function was created. Only used in EVAL context. */
    int function_ref; /* Special ID that allows getting the Lua function object from the Lua registry */
} luaFunction;

int luaEngineInitEngine(void);

#endif /* _ENGINE_LUA_ */
