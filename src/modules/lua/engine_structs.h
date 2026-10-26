#ifndef _ENGINE_STRUCTS_H_
#define _ENGINE_STRUCTS_H_

#include <lua.h>
#include <stdint.h>

typedef struct luaEngineCtx {
    lua_State *eval_lua;     /* The Lua interpreter for EVAL commands. We use just one for all EVAL calls */
    lua_State *function_lua; /* The Lua interpreter for FCALL commands. We use just one for all FCALL calls */

    char *redis_version;
    uint32_t redis_version_num;
    char *server_name;
    char *valkey_version;
    uint32_t valkey_version_num;

    int lua_enable_insecure_api;
} luaEngineCtx;

typedef struct luaFunction {
    lua_State *lua;   /* Pointer to the lua context where this function was created. Only used in EVAL context. */
    int function_ref; /* Special ID that allows getting the Lua function object from the Lua registry */
} luaFunction;

#endif /* _ENGINE_STRUCTS_H_ */
