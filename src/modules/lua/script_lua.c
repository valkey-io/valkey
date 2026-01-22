/*
 * Copyright (c) 2009-2021, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "../../valkeymodule.h"
#include "script_lua.h"
#include "debug_lua.h"
#include "engine_structs.h"
#include "../../sha1.h"
#include "../../rand.h"

#include <fpconv_dtoa.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#define LUA_CMD_OBJCACHE_SIZE 32
#define LUA_CMD_OBJCACHE_MAX_LEN 64

/* Command propagation flags, see propagateNow() function */
#define PROPAGATE_NONE 0
#define PROPAGATE_AOF 1
#define PROPAGATE_REPL 2

/* Log levels */
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3

typedef struct luaFuncCallCtx {
    ValkeyModuleCtx *module_ctx;
    ValkeyModuleScriptingEngineServerRuntimeCtx *run_ctx;
    ValkeyModuleScriptingEngineSubsystemType type;
    int replication_flags;
    int resp;
    int lua_enable_insecure_api;
} luaFuncCallCtx;

/* Globals that are added by the Lua libraries */
static char *libraries_allow_list[] = {
    "string",
    "cjson",
    "bit",
    "cmsgpack",
    "math",
    "table",
    "struct",
    "os",
    NULL,
};

/* Lua API globals */
static char *server_api_allow_list[] = {
    SERVER_API_NAME,
    REDIS_API_NAME,
    "__redis__err__handler",  /* Backwards compatible error handler */
    "__server__err__handler", /* error handler for eval, currently located on globals.
                                Should move to registry. */
    NULL,
};

/* Lua builtins */
static char *lua_builtins_allow_list[] = {
    "xpcall",
    "tostring",
    "setmetatable",
    "next",
    "assert",
    "tonumber",
    "rawequal",
    "collectgarbage",
    "getmetatable",
    "rawset",
    "pcall",
    "coroutine",
    "type",
    "_G",
    "select",
    "unpack",
    "gcinfo",
    "pairs",
    "rawget",
    "loadstring",
    "ipairs",
    "_VERSION",
    "load",
    "error",
    NULL,
};

/* Lua builtins which are deprecated for sandboxing concerns */
static char *lua_builtins_deprecated[] = {
    "newproxy",
    "setfenv",
    "getfenv",
    NULL,
};

/* Lua builtins which are allowed on initialization but will be removed right after */
static char *lua_builtins_removed_after_initialization_allow_list[] = {
    "debug", /* debug will be set to nil after the error handler will be created */
    NULL,
};

/* Those allow lists was created from the globals that was
 * available to the user when the allow lists was first introduce.
 * Because we do not want to break backward compatibility we keep
 * all the globals. The allow lists will prevent us from accidentally
 * creating unwanted globals in the future.
 *
 * Also notice that the allow list is only checked on start time,
 * after that the global table is locked so not need to check anything.*/
static char **allow_lists[] = {
    libraries_allow_list,
    server_api_allow_list,
    lua_builtins_allow_list,
    lua_builtins_removed_after_initialization_allow_list,
    NULL,
};

/* Deny list contains elements which we know we do not want to add to globals
 * and there is no need to print a warning message form them. We will print a
 * log message only if an element was added to the globals and the element is
 * neither on the allow list nor on the back list. */
static char *deny_list[] = {
    "dofile",
    "loadfile",
    "print",
    NULL,
};

static void _serverPanic(const char *file, int line, const char *msg, ...) {
    fprintf(stderr, "------------------------------------------------");
    fprintf(stderr, "!!! Software Failure. Press left mouse button to continue");
    fprintf(stderr, "Guru Meditation: %s #%s:%d", msg, file, line);
    abort();
}

#define serverPanic(...) _serverPanic(__FILE__, __LINE__, __VA_ARGS__)

typedef uint64_t monotime;

monotime getMonotonicUs(void) {
    /* clock_gettime() is specified in POSIX.1b (1993).  Even so, some systems
     * did not support this until much later.  CLOCK_MONOTONIC is technically
     * optional and may not be supported - but it appears to be universal.
     * If this is not supported, provide a system-specific alternate version.  */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

inline uint64_t elapsedUs(monotime start_time) {
    return getMonotonicUs() - start_time;
}

inline uint64_t elapsedMs(monotime start_time) {
    return elapsedUs(start_time) / 1000;
}

static int server_math_random(lua_State *L);
static int server_math_randomseed(lua_State *L);

static void luaReplyToServerReply(ValkeyModuleCtx *ctx, int resp_version, lua_State *lua);

/*
 * Save the give pointer on Lua registry, used to save the Lua context and
 * function context so we can retrieve them from lua_State.
 */
void luaSaveOnRegistry(lua_State *lua, const char *name, void *ptr) {
    lua_pushstring(lua, name);
    if (ptr) {
        lua_pushlightuserdata(lua, ptr);
    } else {
        lua_pushnil(lua);
    }
    lua_settable(lua, LUA_REGISTRYINDEX);
}

/*
 * Get a saved pointer from registry
 */
void *luaGetFromRegistry(lua_State *lua, const char *name) {
    lua_pushstring(lua, name);
    lua_gettable(lua, LUA_REGISTRYINDEX);

    if (lua_isnil(lua, -1)) {
        lua_pop(lua, 1); /* pops the value */
        return NULL;
    }
    /* must be light user data */
    ValkeyModule_Assert(lua_islightuserdata(lua, -1));

    void *ptr = (void *)lua_topointer(lua, -1);
    ValkeyModule_Assert(ptr);

    /* pops the value */
    lua_pop(lua, 1);

    return ptr;
}

char *lm_asprintf(char const *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    size_t str_len = vsnprintf(NULL, 0, fmt, args) + 1;
    va_end(args);

    char *str = ValkeyModule_Alloc(str_len);

    va_start(args, fmt);
    vsnprintf(str, str_len, fmt, args);
    va_end(args);

    return str;
}

char *lm_strcpy(const char *str) {
    size_t len = strlen(str);
    char *res = ValkeyModule_Alloc(len + 1);
    memcpy(res, str, len + 1);
    return res;
}

char *lm_strtrim(char *s, const char *cset) {
    char *end, *sp, *ep;
    size_t len;

    sp = s;
    ep = end = s + strlen(s) - 1;
    while (sp <= end && strchr(cset, *sp)) sp++;
    while (ep > sp && strchr(cset, *ep)) ep--;
    len = (ep - sp) + 1;
    if (s != sp) memmove(s, sp, len);
    s[len] = '\0';
    return s;
}

/* This function is used in order to push an error on the Lua stack in the
 * format used by server.pcall to return errors, which is a lua table
 * with an "err" field set to the error string including the error code.
 * Note that this table is never a valid reply by proper commands,
 * since the returned tables are otherwise always indexed by integers, never by strings.
 *
 * The function takes ownership on the given err_buffer. */
static void luaPushErrorBuff(lua_State *lua, const char *err_buffer) {
    char *msg;

    /* If debugging is active and in step mode, log errors resulting from
     * server commands. */
    if (ldbIsEnabled()) {
        char *msg = lm_asprintf("<error> %s", err_buffer);
        ldbLogCString(msg);
        ValkeyModule_Free(msg);
    }

    char *final_msg = NULL;
    /* There are two possible formats for the received `error` string:
     * 1) "-CODE msg": in this case we remove the leading '-' since we don't store it as part of the lua error format.
     * 2) "msg": in this case we prepend a generic 'ERR' code since all error statuses need some error code.
     * We support format (1) so this function can reuse the error messages used in other places.
     * We support format (2) so it'll be easy to pass descriptive errors to this function without worrying about format.
     */
    if (err_buffer[0] == '-') {
        /* derive error code from the message */
        char *err_msg = strstr(err_buffer, " ");
        if (!err_msg) {
            msg = lm_strcpy(err_buffer + 1);
            final_msg = lm_asprintf("ERR %s", msg);
        } else {
            *err_msg = '\0';
            msg = lm_strcpy(err_msg + 1);
            msg = lm_strtrim(msg, "\r\n");
            final_msg = lm_asprintf("%s %s", err_buffer + 1, msg);
        }
    } else {
        msg = lm_strcpy(err_buffer);
        msg = lm_strtrim(msg, "\r\n");
        final_msg = lm_asprintf("%s", msg);
    }
    /* Trim newline at end of string. If we reuse the ready-made error objects (case 1 above) then we might
     * have a newline that needs to be trimmed. In any case the lua server error table shouldn't end with a newline. */

    lua_newtable(lua);
    lua_pushstring(lua, "err");
    lua_pushstring(lua, final_msg);
    lua_settable(lua, -3);

    ValkeyModule_Free(msg);
    ValkeyModule_Free(final_msg);
}

void luaPushError(lua_State *lua, const char *error) {
    luaPushErrorBuff(lua, error);
}

/* In case the error set into the Lua stack by luaPushError() was generated
 * by the non-error-trapping version of server.pcall(), which is server.call(),
 * this function will raise the Lua error so that the execution of the
 * script will be halted. */
int luaError(lua_State *lua) {
    return lua_error(lua);
}

/* ---------------------------------------------------------------------------
 * Server reply to Lua type conversion functions.
 * ------------------------------------------------------------------------- */

static void callReplyToLuaType(lua_State *lua, ValkeyModuleCallReply *reply, int resp) {
    int type = ValkeyModule_CallReplyType(reply);
    switch (type) {
    case VALKEYMODULE_REPLY_STRING: {
        if (!lua_checkstack(lua, 1)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
        lua_pushlstring(lua, str, len);
        break;
    }
    case VALKEYMODULE_REPLY_SIMPLE_STRING: {
        if (!lua_checkstack(lua, 3)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
        lua_newtable(lua);
        lua_pushstring(lua, "ok");
        lua_pushlstring(lua, str, len);
        lua_settable(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_INTEGER: {
        if (!lua_checkstack(lua, 1)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }
        long long val = ValkeyModule_CallReplyInteger(reply);
        lua_pushnumber(lua, (lua_Number)val);
        break;
    }
    case VALKEYMODULE_REPLY_ARRAY: {
        if (!lua_checkstack(lua, 2)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }
        size_t items = ValkeyModule_CallReplyLength(reply);
        lua_createtable(lua, items, 0);

        for (size_t i = 0; i < items; i++) {
            ValkeyModuleCallReply *val = ValkeyModule_CallReplyArrayElement(reply, i);

            lua_pushnumber(lua, i + 1);
            callReplyToLuaType(lua, val, resp);
            lua_settable(lua, -3);
        }
        break;
    }
    case VALKEYMODULE_REPLY_NULL:
    case VALKEYMODULE_REPLY_ARRAY_NULL:
        if (!lua_checkstack(lua, 1)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }
        if (resp == 2) {
            lua_pushboolean(lua, 0);
        } else {
            lua_pushnil(lua);
        }
        break;
    case VALKEYMODULE_REPLY_MAP: {
        if (!lua_checkstack(lua, 3)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }

        size_t items = ValkeyModule_CallReplyLength(reply);
        lua_newtable(lua);
        lua_pushstring(lua, "map");
        lua_createtable(lua, 0, items);

        for (size_t i = 0; i < items; i++) {
            ValkeyModuleCallReply *key = NULL;
            ValkeyModuleCallReply *val = NULL;
            ValkeyModule_CallReplyMapElement(reply, i, &key, &val);

            callReplyToLuaType(lua, key, resp);
            callReplyToLuaType(lua, val, resp);
            lua_settable(lua, -3);
        }
        lua_settable(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_SET: {
        if (!lua_checkstack(lua, 3)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }

        size_t items = ValkeyModule_CallReplyLength(reply);
        lua_newtable(lua);
        lua_pushstring(lua, "set");
        lua_createtable(lua, 0, items);

        for (size_t i = 0; i < items; i++) {
            ValkeyModuleCallReply *val = ValkeyModule_CallReplySetElement(reply, i);

            callReplyToLuaType(lua, val, resp);
            lua_pushboolean(lua, 1);
            lua_settable(lua, -3);
        }
        lua_settable(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_BOOL: {
        if (!lua_checkstack(lua, 1)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }
        int b = ValkeyModule_CallReplyBool(reply);
        lua_pushboolean(lua, b);
        break;
    }
    case VALKEYMODULE_REPLY_DOUBLE: {
        if (!lua_checkstack(lua, 3)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }
        double d = ValkeyModule_CallReplyDouble(reply);
        lua_newtable(lua);
        lua_pushstring(lua, "double");
        lua_pushnumber(lua, d);
        lua_settable(lua, -3);
        break;
    }

    case VALKEYMODULE_REPLY_BIG_NUMBER: {
        if (!lua_checkstack(lua, 3)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }
        size_t len = 0;
        const char *str = ValkeyModule_CallReplyBigNumber(reply, &len);
        lua_newtable(lua);
        lua_pushstring(lua, "big_number");
        lua_pushlstring(lua, str, len);
        lua_settable(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_VERBATIM_STRING: {
        if (!lua_checkstack(lua, 5)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }
        size_t len = 0;
        const char *format = NULL;
        const char *str = ValkeyModule_CallReplyVerbatim(reply, &len, &format);
        lua_newtable(lua);
        lua_pushstring(lua, "verbatim_string");
        lua_newtable(lua);
        lua_pushstring(lua, "string");
        lua_pushlstring(lua, str, len);
        lua_settable(lua, -3);
        lua_pushstring(lua, "format");
        lua_pushlstring(lua, format, 3);
        lua_settable(lua, -3);
        lua_settable(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_ERROR: {
        if (!lua_checkstack(lua, 3)) {
            /* Increase the Lua stack if needed, to make sure there is enough room
             * to push elements to the stack. On failure, exit with panic. */
            serverPanic("lua stack limit reach when parsing server.call reply");
        }
        const char *err = ValkeyModule_CallReplyStringPtr(reply, NULL);
        luaPushErrorBuff(lua, err);
        /* push a field indicate to ignore updating the stats on this error
         * because it was already updated when executing the command. */
        lua_pushstring(lua, "ignore_error_stats_update");
        lua_pushboolean(lua, 1);
        lua_settable(lua, -3);
        break;
    }
    case VALKEYMODULE_REPLY_ATTRIBUTE: {
        /* Currently, we do not expose the attribute to the Lua script. */
        break;
    }
    case VALKEYMODULE_REPLY_PROMISE:
    case VALKEYMODULE_REPLY_UNKNOWN:
    default:
        ValkeyModule_Assert(0);
    }
}

/* ---------------------------------------------------------------------------
 * Lua reply to server reply conversion functions.
 * ------------------------------------------------------------------------- */

char *strmapchars(char *s, const char *from, const char *to, size_t setlen) {
    size_t j, i, l = strlen(s);

    for (j = 0; j < l; j++) {
        for (i = 0; i < setlen; i++) {
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

char *copy_string_from_lua_stack(lua_State *lua) {
    const char *str = lua_tostring(lua, -1);
    size_t len = lua_strlen(lua, -1);
    char *res = ValkeyModule_Alloc(len + 1);
    strncpy(res, str, len);
    res[len] = 0;
    return res;
}

/* Reply to client 'c' converting the top element in the Lua stack to a
 * server reply. As a side effect the element is consumed from the stack.  */
static void luaReplyToServerReply(ValkeyModuleCtx *ctx, int resp_version, lua_State *lua) {
    int t = lua_type(lua, -1);

    if (!lua_checkstack(lua, 4)) {
        /* Increase the Lua stack if needed to make sure there is enough room
         * to push 4 elements to the stack. On failure, return error.
         * Notice that we need, in the worst case, 4 elements because returning a map might
         * require push 4 elements to the Lua stack.*/
        ValkeyModule_ReplyWithError(ctx, "ERR reached lua stack limit");
        lua_pop(lua, 1); /* pop the element from the stack */
        return;
    }

    switch (t) {
    case LUA_TSTRING:
        ValkeyModule_ReplyWithStringBuffer(ctx, lua_tostring(lua, -1), lua_strlen(lua, -1));
        break;
    case LUA_TBOOLEAN:
        if (resp_version == 2) {
            int b = lua_toboolean(lua, -1);
            if (b) {
                ValkeyModule_ReplyWithLongLong(ctx, 1);
            } else {
                ValkeyModule_ReplyWithNull(ctx);
            }
        } else {
            ValkeyModule_ReplyWithBool(ctx, lua_toboolean(lua, -1));
        }
        break;
    case LUA_TNUMBER: ValkeyModule_ReplyWithLongLong(ctx, (long long)lua_tonumber(lua, -1)); break;
    case LUA_TTABLE:
        /* We need to check if it is an array, an error, or a status reply.
         * Error are returned as a single element table with 'err' field.
         * Status replies are returned as single element table with 'ok'
         * field. */

        /* Handle error reply. */
        /* we took care of the stack size on function start */
        lua_pushstring(lua, "err");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TSTRING) {
            lua_pop(lua,
                    1); /* pop the error message, we will use luaExtractErrorInformation to get error information */
            errorInfo err_info = {0};
            luaExtractErrorInformation(lua, &err_info);
            ValkeyModule_ReplyWithCustomErrorFormat(ctx, !err_info.ignore_err_stats_update, "%s", err_info.msg);
            luaErrorInformationDiscard(&err_info);
            lua_pop(lua, 1); /* pop the result table */
            return;
        }
        lua_pop(lua, 1); /* Discard field name pushed before. */

        /* Handle status reply. */
        lua_pushstring(lua, "ok");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TSTRING) {
            char *ok = copy_string_from_lua_stack(lua);
            strmapchars(ok, "\r\n", "  ", 2);
            ValkeyModule_ReplyWithSimpleString(ctx, ok);
            ValkeyModule_Free(ok);
            lua_pop(lua, 2);
            return;
        }
        lua_pop(lua, 1); /* Discard field name pushed before. */

        /* Handle double reply. */
        lua_pushstring(lua, "double");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TNUMBER) {
            ValkeyModule_ReplyWithDouble(ctx, lua_tonumber(lua, -1));
            lua_pop(lua, 2);
            return;
        }
        lua_pop(lua, 1); /* Discard field name pushed before. */

        /* Handle big number reply. */
        lua_pushstring(lua, "big_number");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TSTRING) {
            char *big_num = copy_string_from_lua_stack(lua);
            strmapchars(big_num, "\r\n", "  ", 2);
            ValkeyModule_ReplyWithBigNumber(ctx, big_num, strlen(big_num));
            ValkeyModule_Free(big_num);
            lua_pop(lua, 2);
            return;
        }
        lua_pop(lua, 1); /* Discard field name pushed before. */

        /* Handle verbatim reply. */
        lua_pushstring(lua, "verbatim_string");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TTABLE) {
            lua_pushstring(lua, "format");
            lua_rawget(lua, -2);
            t = lua_type(lua, -1);
            if (t == LUA_TSTRING) {
                char *format = (char *)lua_tostring(lua, -1);
                lua_pushstring(lua, "string");
                lua_rawget(lua, -3);
                t = lua_type(lua, -1);
                if (t == LUA_TSTRING) {
                    size_t len;
                    char *str = (char *)lua_tolstring(lua, -1, &len);
                    ValkeyModule_ReplyWithVerbatimStringType(ctx, str, len, format);
                    lua_pop(lua, 4);
                    return;
                }
                lua_pop(lua, 1);
            }
            lua_pop(lua, 1);
        }
        lua_pop(lua, 1); /* Discard field name pushed before. */

        /* Handle map reply. */
        lua_pushstring(lua, "map");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TTABLE) {
            int maplen = 0;
            ValkeyModule_ReplyWithMap(ctx, VALKEYMODULE_POSTPONED_LEN);
            /* we took care of the stack size on function start */
            lua_pushnil(lua); /* Use nil to start iteration. */
            while (lua_next(lua, -2)) {
                /* Stack now: table, key, value */
                lua_pushvalue(lua, -2);                        /* Dup key before consuming. */
                luaReplyToServerReply(ctx, resp_version, lua); /* Return key. */
                luaReplyToServerReply(ctx, resp_version, lua); /* Return value. */
                /* Stack now: table, key. */
                maplen++;
            }
            ValkeyModule_ReplySetMapLength(ctx, maplen);
            lua_pop(lua, 2);
            return;
        }
        lua_pop(lua, 1); /* Discard field name pushed before. */

        /* Handle set reply. */
        lua_pushstring(lua, "set");
        lua_rawget(lua, -2);
        t = lua_type(lua, -1);
        if (t == LUA_TTABLE) {
            int setlen = 0;
            ValkeyModule_ReplyWithSet(ctx, VALKEYMODULE_POSTPONED_LEN);
            /* we took care of the stack size on function start */
            lua_pushnil(lua); /* Use nil to start iteration. */
            while (lua_next(lua, -2)) {
                /* Stack now: table, key, true */
                lua_pop(lua, 1);                               /* Discard the boolean value. */
                lua_pushvalue(lua, -1);                        /* Dup key before consuming. */
                luaReplyToServerReply(ctx, resp_version, lua); /* Return key. */
                /* Stack now: table, key. */
                setlen++;
            }
            ValkeyModule_ReplySetSetLength(ctx, setlen);
            lua_pop(lua, 2);
            return;
        }
        lua_pop(lua, 1); /* Discard field name pushed before. */

        /* Handle the array reply. */
        ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
        int j = 1, mbulklen = 0;
        while (1) {
            /* we took care of the stack size on function start */
            lua_pushnumber(lua, j++);
            lua_rawget(lua, -2);
            t = lua_type(lua, -1);
            if (t == LUA_TNIL) {
                lua_pop(lua, 1);
                break;
            }
            luaReplyToServerReply(ctx, resp_version, lua);
            mbulklen++;
        }
        ValkeyModule_ReplySetArrayLength(ctx, mbulklen);
        break;
    default: ValkeyModule_ReplyWithNull(ctx);
    }
    lua_pop(lua, 1);
}

/* ---------------------------------------------------------------------------
 * Lua server.* functions implementations.
 * ------------------------------------------------------------------------- */
void freeLuaServerArgv(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc);

/* Return the number of digits of 'v' when converted to string in radix 10.
 * See ll2string() for more information. */
static uint32_t digits10(uint64_t v) {
    if (v < 10) return 1;
    if (v < 100) return 2;
    if (v < 1000) return 3;
    if (v < 1000000000000UL) {
        if (v < 100000000UL) {
            if (v < 1000000) {
                if (v < 10000) return 4;
                return 5 + (v >= 100000);
            }
            return 7 + (v >= 10000000UL);
        }
        if (v < 10000000000UL) {
            return 9 + (v >= 1000000000UL);
        }
        return 11 + (v >= 100000000000UL);
    }
    return 12 + digits10(v / 1000000000000UL);
}

/* Convert a unsigned long long into a string. Returns the number of
 * characters needed to represent the number.
 * If the buffer is not big enough to store the string, 0 is returned.
 *
 * Based on the following article (that apparently does not provide a
 * novel approach but only publicizes an already used technique):
 *
 * https://web.archive.org/web/20150427221229/https://www.facebook.com/notes/facebook-engineering/three-optimization-tips-for-c/10151361643253920 */
static int ull2string(char *dst, size_t dstlen, unsigned long long value) {
    static const char digits[201] = "0001020304050607080910111213141516171819"
                                    "2021222324252627282930313233343536373839"
                                    "4041424344454647484950515253545556575859"
                                    "6061626364656667686970717273747576777879"
                                    "8081828384858687888990919293949596979899";

    /* Check length. */
    uint32_t length = digits10(value);
    if (length >= dstlen) goto err;
    ;

    /* Null term. */
    uint32_t next = length - 1;
    dst[next + 1] = '\0';
    while (value >= 100) {
        int const i = (value % 100) * 2;
        value /= 100;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
        next -= 2;
    }

    /* Handle last 1-2 digits. */
    if (value < 10) {
        dst[next] = '0' + (uint32_t)value;
    } else {
        int i = (uint32_t)value * 2;
        dst[next] = digits[i + 1];
        dst[next - 1] = digits[i];
    }
    return length;
err:
    /* force add Null termination */
    if (dstlen > 0) dst[0] = '\0';
    return 0;
}

/* Convert a long long into a string. Returns the number of
 * characters needed to represent the number.
 * If the buffer is not big enough to store the string, 0 is returned. */
static int ll2string(char *dst, size_t dstlen, long long svalue) {
    unsigned long long value;
    int negative = 0;

    /* The ull2string function with 64bit unsigned integers for simplicity, so
     * we convert the number here and remember if it is negative. */
    if (svalue < 0) {
        if (svalue != LLONG_MIN) {
            value = -svalue;
        } else {
            value = ((unsigned long long)LLONG_MAX) + 1;
        }
        if (dstlen < 2) goto err;
        negative = 1;
        dst[0] = '-';
        dst++;
        dstlen--;
    } else {
        value = svalue;
    }

    /* Converts the unsigned long long value to string*/
    int length = ull2string(dst, dstlen, value);
    if (length == 0) return 0;
    return length + negative;

err:
    /* force add Null termination */
    if (dstlen > 0) dst[0] = '\0';
    return 0;
}

/* Returns 1 if the double value can safely be represented in long long without
 * precision loss, in which case the corresponding long long is stored in the out variable. */
static int double2ll(double d, long long *out) {
#if (__DBL_MANT_DIG__ >= 52) && (__DBL_MANT_DIG__ <= 63) && (LLONG_MAX == 0x7fffffffffffffffLL)
    /* Check if the float is in a safe range to be casted into a
     * long long. We are assuming that long long is 64 bit here.
     * Also we are assuming that there are no implementations around where
     * double has precision < 52 bit.
     *
     * Under this assumptions we test if a double is inside a range
     * where casting to long long is safe. Then using two castings we
     * make sure the decimal part is zero. If all this is true we can use
     * integer without precision loss.
     *
     * Note that numbers above 2^52 and below 2^63 use all the fraction bits as real part,
     * and the exponent bits are positive, which means the "decimal" part must be 0.
     * i.e. all double values in that range are representable as a long without precision loss,
     * but not all long values in that range can be represented as a double.
     * we only care about the first part here. */
    if (d < (double)(-LLONG_MAX / 2) || d > (double)(LLONG_MAX / 2)) return 0;
    long long ll = d;
    if (ll == d) {
        *out = ll;
        return 1;
    }
#else
    VALKEYMODULE_NOT_USED(d);
    VALKEYMODULE_NOT_USED(out);
#endif
    return 0;
}

static ValkeyModuleString **luaArgsToServerArgv(ValkeyModuleCtx *ctx, lua_State *lua, int *argc) {
    int j;
    /* Require at least one argument */
    *argc = lua_gettop(lua);
    if (*argc == 0) {
        luaPushError(lua, "Please specify at least one argument for this call");
        return NULL;
    }

    ValkeyModuleString **lua_argv = ValkeyModule_Alloc(sizeof(ValkeyModuleString *) * *argc);

    for (j = 0; j < *argc; j++) {
        char *obj_s;
        size_t obj_len;
        char dbuf[64];

        if (lua_type(lua, j + 1) == LUA_TNUMBER) {
            /* We can't use lua_tolstring() for number -> string conversion
             * since Lua uses a format specifier that loses precision. */
            lua_Number num = lua_tonumber(lua, j + 1);
            /* Integer printing function is much faster, check if we can safely use it.
             * Since lua_Number is not explicitly an integer or a double, we need to make an effort
             * to convert it as an integer when that's possible, since the string could later be used
             * in a context that doesn't support scientific notation (e.g. 1e9 instead of 100000000). */
            long long lvalue;
            if (double2ll((double)num, &lvalue)) {
                obj_len = ll2string(dbuf, sizeof(dbuf), lvalue);
            } else {
                obj_len = fpconv_dtoa((double)num, dbuf);
                dbuf[obj_len] = '\0';
            }
            obj_s = dbuf;
        } else {
            obj_s = (char *)lua_tolstring(lua, j + 1, &obj_len);
            if (obj_s == NULL) break; /* Not a string. */
        }

        lua_argv[j] = ValkeyModule_CreateString(ctx, obj_s, obj_len);
    }

    /* Pop all arguments from the stack, we do not need them anymore
     * and this way we guaranty we will have room on the stack for the result. */
    lua_pop(lua, *argc);

    /* Check if one of the arguments passed by the Lua script
     * is not a string or an integer (lua_isstring() return true for
     * integers as well). */
    if (j != *argc) {
        freeLuaServerArgv(ctx, lua_argv, j);
        luaPushError(lua, "ERR Command arguments must be strings or integers");
        return NULL;
    }

    return lua_argv;
}

void freeLuaServerArgv(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    int j;
    for (j = 0; j < argc; j++) {
        ValkeyModuleString *o = argv[j];
        ValkeyModule_FreeString(ctx, o);
    }
    ValkeyModule_Free(argv);
}

static void luaProcessReplyError(ValkeyModuleCallReply *reply, lua_State *lua) {
    const char *err = ValkeyModule_CallReplyStringPtr(reply, NULL);
    int push_error = 1;

    /* The following error messages rewrites are required to keep the backward compatibility
     * with the previous Lua engine that was implemented in Valkey core. */
    if (errno == ESPIPE) {
        if (strncmp(err, "ERR command ", strlen("ERR command ")) == 0) {
            luaPushError(lua, "ERR This Valkey command is not allowed from script");
            push_error = 0;
        }
    } else if (errno == EINVAL) {
        if (strncmp(err, "ERR wrong number of arguments for ", strlen("ERR wrong number of arguments for ")) == 0) {
            luaPushError(lua, "ERR Wrong number of args calling command from script");
            push_error = 0;
        }
    } else if (errno == ENOENT) {
        if (strncmp(err, "ERR unknown command '", strlen("ERR unknown command '")) == 0) {
            luaPushError(lua, "ERR Unknown command called from script");
            push_error = 0;
        }
    } else if (errno == EACCES) {
        if (strncmp(err, "NOPERM ", strlen("NOPERM ")) == 0) {
            const char *err_prefix = "ERR ACL failure in script: ";
            size_t err_len = strlen(err_prefix) + strlen(err + strlen("NOPERM ")) + 1;
            char *err_msg = ValkeyModule_Alloc(err_len * sizeof(char));
            bzero(err_msg, err_len);
            strcpy(err_msg, err_prefix);
            strcat(err_msg, err + strlen("NOPERM "));
            luaPushError(lua, err_msg);
            ValkeyModule_Free(err_msg);
            push_error = 0;
        }
    }

    if (push_error) {
        luaPushError(lua, err);
    }
    /* push a field indicate to ignore updating the stats on this error
     * because it was already updated when executing the command. */
    lua_pushstring(lua, "ignore_error_stats_update");
    lua_pushboolean(lua, 1);
    lua_settable(lua, -3);
}

static int luaServerGenericCommand(lua_State *lua, int raise_error) {
    luaFuncCallCtx *rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    ValkeyModule_Assert(rctx); /* Only supported inside script invocation */
    ValkeyModuleCallReply *reply;

    int argc = 0;
    ValkeyModuleString **argv = luaArgsToServerArgv(rctx->module_ctx, lua, &argc);
    if (argv == NULL) {
        return raise_error ? luaError(lua) : 1;
    }

    static int inuse = 0; /* Recursive calls detection. */

    /* By using Lua debug hooks it is possible to trigger a recursive call
     * to luaServerGenericCommand(), which normally should never happen.
     * To make this function reentrant is futile and makes it slower, but
     * we should at least detect such a misuse, and abort. */
    if (inuse) {
        char *recursion_warning = "luaRedisGenericCommand() recursive call detected. "
                                  "Are you doing funny stuff with Lua debug hooks?";
        ValkeyModule_Log(rctx->module_ctx, "warning", "%s", recursion_warning);
        luaPushError(lua, recursion_warning);
        return 1;
    }
    inuse++;

    /* Log the command if debugging is active. */
    if (ldbIsEnabled()) {
        const char *cmd_prefix = "<command>";
        char *cmdlog = ValkeyModule_Calloc(strlen(cmd_prefix) + 1, sizeof(char));
        strcpy(cmdlog, cmd_prefix);
        for (int i = 0; i < argc; i++) {
            if (i == 10) {
                char *new_cmdlog = lm_asprintf("%s ... (%d more)", cmdlog, argc - i - 1);
                ValkeyModule_Free(cmdlog);
                cmdlog = new_cmdlog;
                break;
            } else {
                const char *argv_cstr = ValkeyModule_StringPtrLen(argv[i], NULL);
                char *new_cmdlog = lm_asprintf("%s %s", cmdlog, argv_cstr);
                ValkeyModule_Free(cmdlog);
                cmdlog = new_cmdlog;
            }
        }
        ldbLogCString(cmdlog);
        ValkeyModule_Free(cmdlog);
    }

    char fmt[13] = "v!EMSX";
    int fmt_idx = 6; /* Index of the last char in fmt[] */

    ValkeyModuleString *username = ValkeyModule_GetCurrentUserName(rctx->module_ctx);
    if (username != NULL) {
        fmt[fmt_idx++] = 'C';
        ValkeyModule_FreeString(rctx->module_ctx, username);
    }

    if (!(rctx->replication_flags & PROPAGATE_AOF)) {
        fmt[fmt_idx++] = 'A';
    }
    if (!(rctx->replication_flags & PROPAGATE_REPL)) {
        fmt[fmt_idx++] = 'R';
    }
    if (!rctx->replication_flags) {
        /* PROPAGATE_NONE case */
        fmt[fmt_idx++] = 'A';
        fmt[fmt_idx++] = 'R';
    }
    if (rctx->resp == 3) {
        fmt[fmt_idx++] = '3';
    }
    fmt[fmt_idx] = '\0';

    const char *cmdname = ValkeyModule_StringPtrLen(argv[0], NULL);

    errno = 0;
    reply = ValkeyModule_Call(rctx->module_ctx, cmdname, fmt, argv + 1, argc - 1);
    freeLuaServerArgv(rctx->module_ctx, argv, argc);
    int reply_type = ValkeyModule_CallReplyType(reply);
    if (errno != 0) {
        ValkeyModule_Assert(reply_type == VALKEYMODULE_REPLY_ERROR);

        const char *err = ValkeyModule_CallReplyStringPtr(reply, NULL);
        ValkeyModule_Log(rctx->module_ctx, "debug", "command returned an error: %s errno=%d", err, errno);

        luaProcessReplyError(reply, lua);
        goto cleanup;
    } else if (raise_error && reply_type != VALKEYMODULE_REPLY_ERROR) {
        raise_error = 0;
    }

    callReplyToLuaType(lua, reply, rctx->resp);

    /* If the debugger is active, log the reply from the server. */
    if (ldbIsEnabled()) {
        ValkeyModule_ScriptingEngineDebuggerLogRespReply(reply);
    }

cleanup:
    /* Clean up. Command code may have changed argv/argc so we use the
     * argv/argc of the client instead of the local variables. */
    ValkeyModule_FreeCallReply(reply);

    inuse--;

    if (raise_error) {
        /* If we are here we should have an error in the stack, in the
         * form of a table with an "err" field. Extract the string to
         * return the plain error. */
        return luaError(lua);
    }
    return 1;
}

/* Our implementation to lua pcall.
 * We need this implementation for backward
 * comparability with older Redis OSS versions.
 *
 * On Redis OSS 7, the error object is a table,
 * compare to older version where the error
 * object is a string. To keep backward
 * comparability we catch the table object
 * and just return the error message. */
static int luaRedisPcall(lua_State *lua) {
    int argc = lua_gettop(lua);
    lua_pushboolean(lua, 1); /* result place holder */
    lua_insert(lua, 1);
    if (lua_pcall(lua, argc - 1, LUA_MULTRET, 0)) {
        /* Error */
        lua_remove(lua, 1); /* remove the result place holder, now we have room for at least one element */
        if (lua_istable(lua, -1)) {
            lua_getfield(lua, -1, "err");
            if (lua_isstring(lua, -1)) {
                lua_replace(lua, -2); /* replace the error message with the table */
            }
        }
        lua_pushboolean(lua, 0); /* push result */
        lua_insert(lua, 1);
    }
    return lua_gettop(lua);
}

/* server.call() */
static int luaRedisCallCommand(lua_State *lua) {
    return luaServerGenericCommand(lua, 1);
}

/* server.pcall() */
static int luaRedisPCallCommand(lua_State *lua) {
    return luaServerGenericCommand(lua, 0);
}

/* Perform the SHA1 of the input string. We use this both for hashing script
 * bodies in order to obtain the Lua function name, and in the implementation
 * of server.sha1().
 *
 * 'digest' should point to a 41 bytes buffer: 40 for SHA1 converted into an
 * hexadecimal number, plus 1 byte for null term. */
void sha1hex(char *digest, char *script, size_t len) {
    SHA1_CTX ctx;
    unsigned char hash[20];
    char *cset = "0123456789abcdef";
    int j;

    SHA1Init(&ctx);
    SHA1Update(&ctx, (unsigned char *)script, len);
    SHA1Final(hash, &ctx);

    for (j = 0; j < 20; j++) {
        digest[j * 2] = cset[((hash[j] & 0xF0) >> 4)];
        digest[j * 2 + 1] = cset[(hash[j] & 0xF)];
    }
    digest[40] = '\0';
}

/* This adds server.sha1hex(string) to Lua scripts using the same hashing
 * function used for sha1ing lua scripts. */
static int luaRedisSha1hexCommand(lua_State *lua) {
    int argc = lua_gettop(lua);
    char digest[41];
    size_t len;
    char *s;

    if (argc != 1) {
        luaPushError(lua, "wrong number of arguments");
        return luaError(lua);
    }

    s = (char *)lua_tolstring(lua, 1, &len);
    sha1hex(digest, s, len);
    lua_pushstring(lua, digest);
    return 1;
}

/* Returns a table with a single field 'field' set to the string value
 * passed as argument. This helper function is handy when returning
 * a RESP error or status reply from Lua:
 *
 * return server.error_reply("ERR Some Error")
 * return server.status_reply("ERR Some Error")
 */
static int luaRedisReturnSingleFieldTable(lua_State *lua, char *field) {
    if (lua_gettop(lua) != 1 || lua_type(lua, -1) != LUA_TSTRING) {
        luaPushError(lua, "wrong number or type of arguments");
        return 1;
    }

    lua_newtable(lua);
    lua_pushstring(lua, field);
    lua_pushvalue(lua, -3);
    lua_settable(lua, -3);
    return 1;
}

/* server.error_reply() */
static int luaRedisErrorReplyCommand(lua_State *lua) {
    if (lua_gettop(lua) != 1 || lua_type(lua, -1) != LUA_TSTRING) {
        luaPushError(lua, "wrong number or type of arguments");
        return 1;
    }

    /* add '-' if not exists */
    const char *err = lua_tostring(lua, -1);
    char *err_buff = NULL;
    if (err[0] != '-') {
        err_buff = lm_asprintf("-%s", err);
    } else {
        err_buff = lm_strcpy(err);
    }
    luaPushErrorBuff(lua, err_buff);
    ValkeyModule_Free(err_buff);
    return 1;
}

/* server.status_reply() */
static int luaRedisStatusReplyCommand(lua_State *lua) {
    return luaRedisReturnSingleFieldTable(lua, "ok");
}

/* server.set_repl()
 *
 * Set the propagation of write commands executed in the context of the
 * script to on/off for AOF and replicas. */
static int luaRedisSetReplCommand(lua_State *lua) {
    int flags, argc = lua_gettop(lua);

    luaFuncCallCtx *rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    ValkeyModule_Assert(rctx); /* Only supported inside script invocation */

    if (argc != 1) {
        luaPushError(lua, "server.set_repl() requires one argument.");
        return luaError(lua);
    }

    flags = lua_tonumber(lua, -1);
    if ((flags & ~(PROPAGATE_AOF | PROPAGATE_REPL)) != 0) {
        luaPushError(lua, "Invalid replication flags. Use REPL_AOF, REPL_REPLICA, REPL_ALL or REPL_NONE.");
        return luaError(lua);
    }

    rctx->replication_flags = flags;

    return 0;
}

/* server.acl_check_cmd()
 *
 * Checks ACL permissions for given command for the current user. */
static int luaRedisAclCheckCmdPermissionsCommand(lua_State *lua) {
    luaFuncCallCtx *rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    ValkeyModule_Assert(rctx); /* Only supported inside script invocation */

    int raise_error = 0;

    int argc = 0;
    ValkeyModuleString **argv = luaArgsToServerArgv(rctx->module_ctx, lua, &argc);

    /* Require at least one argument */
    if (argv == NULL) return luaError(lua);

    ValkeyModuleString *username = ValkeyModule_GetCurrentUserName(rctx->module_ctx);
    ValkeyModuleUser *user = ValkeyModule_GetModuleUserFromUserName(username);
    int dbid = ValkeyModule_GetSelectedDb(rctx->module_ctx);
    ValkeyModule_FreeString(rctx->module_ctx, username);

    if (ValkeyModule_ACLCheckPermissions(user, argv, argc, dbid, NULL) != VALKEYMODULE_OK) {
        if (errno == EINVAL) {
            luaPushError(lua, "ERR Invalid command passed to server.acl_check_cmd()");
            raise_error = 1;
        } else {
            ValkeyModule_Assert(errno == EACCES);
            lua_pushboolean(lua, 0);
        }
    } else {
        lua_pushboolean(lua, 1);
    }

    ValkeyModule_FreeModuleUser(user);
    freeLuaServerArgv(rctx->module_ctx, argv, argc);
    if (raise_error)
        return luaError(lua);
    else
        return 1;
}


/* server.log() */
static int luaLogCommand(lua_State *lua) {
    luaFuncCallCtx *rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    ValkeyModule_Assert(rctx); /* Only supported inside script invocation */

    int j, argc = lua_gettop(lua);
    int level;

    if (argc < 2) {
        luaPushError(lua, "server.log() requires two arguments or more.");
        return luaError(lua);
    } else if (!lua_isnumber(lua, -argc)) {
        luaPushError(lua, "First argument must be a number (log level).");
        return luaError(lua);
    }
    level = lua_tonumber(lua, -argc);
    if (level < LL_DEBUG || level > LL_WARNING) {
        luaPushError(lua, "Invalid log level.");
        return luaError(lua);
    }

    /* Glue together all the arguments */
    char *log = NULL;
    for (j = 1; j < argc; j++) {
        size_t len;
        char *s;

        s = (char *)lua_tolstring(lua, (-argc) + j, &len);
        if (s) {
            if (j != 1) {
                char *next_log = lm_asprintf("%s %s", log, s);
                ValkeyModule_Free(log);
                log = next_log;
            } else {
                log = lm_asprintf("%s", s);
            }
        }
    }

    const char *level_str = NULL;
    switch (level) {
    case LL_DEBUG: level_str = "debug"; break;
    case LL_VERBOSE: level_str = "verbose"; break;
    case LL_NOTICE: level_str = "notice"; break;
    case LL_WARNING: level_str = "warning"; break;
    default: ValkeyModule_Assert(0);
    }

    ValkeyModule_Log(rctx->module_ctx, level_str, "%s", log);
    ValkeyModule_Free(log);
    return 0;
}

/* server.setresp() */
static int luaSetResp(lua_State *lua) {
    luaFuncCallCtx *rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    ValkeyModule_Assert(rctx); /* Only supported inside script invocation */

    int argc = lua_gettop(lua);

    if (argc != 1) {
        luaPushError(lua, "server.setresp() requires one argument.");
        return luaError(lua);
    }

    int resp = lua_tonumber(lua, -argc);
    if (resp != 2 && resp != 3) {
        luaPushError(lua, "RESP version must be 2 or 3.");
        return luaError(lua);
    }

    rctx->resp = resp;

    return 0;
}

/* ---------------------------------------------------------------------------
 * Lua engine initialization and reset.
 * ------------------------------------------------------------------------- */

static void luaLoadLib(lua_State *lua, const char *libname, lua_CFunction luafunc) {
    lua_pushcfunction(lua, luafunc);
    lua_pushstring(lua, libname);
    lua_call(lua, 1, 0);
}

LUALIB_API int(luaopen_cjson)(lua_State *L);
LUALIB_API int(luaopen_struct)(lua_State *L);
LUALIB_API int(luaopen_cmsgpack)(lua_State *L);
LUALIB_API int(luaopen_bit)(lua_State *L);

static void luaLoadLibraries(lua_State *lua) {
    luaLoadLib(lua, "", luaopen_base);
    luaLoadLib(lua, LUA_TABLIBNAME, luaopen_table);
    luaLoadLib(lua, LUA_STRLIBNAME, luaopen_string);
    luaLoadLib(lua, LUA_MATHLIBNAME, luaopen_math);
    luaLoadLib(lua, LUA_DBLIBNAME, luaopen_debug);
    luaLoadLib(lua, LUA_OSLIBNAME, luaopen_os);
    luaLoadLib(lua, "cjson", luaopen_cjson);
    luaLoadLib(lua, "struct", luaopen_struct);
    luaLoadLib(lua, "cmsgpack", luaopen_cmsgpack);
    luaLoadLib(lua, "bit", luaopen_bit);

#if 0 /* Stuff that we don't load currently, for sandboxing concerns. */
    luaLoadLib(lua, LUA_LOADLIBNAME, luaopen_package);
#endif
}

static int luaProtectedTableError(lua_State *lua) {
    luaFuncCallCtx *rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    int argc = lua_gettop(lua);
    if (argc != 2) {
        ValkeyModule_Log(rctx->module_ctx, "warning", "malicious code trying to call luaProtectedTableError with wrong arguments");
        luaL_error(lua, "Wrong number of arguments to luaProtectedTableError");
    }
    if (!lua_isstring(lua, -1) && !lua_isnumber(lua, -1)) {
        luaL_error(lua, "Second argument to luaProtectedTableError must be a string or number");
    }
    const char *variable_name = lua_tostring(lua, -1);
    luaL_error(lua, "Script attempted to access nonexistent global variable '%s'", variable_name);
    return 0;
}

/* Set a special metatable on the table on the top of the stack.
 * The metatable will raise an error if the user tries to fetch
 * an un-existing value.
 *
 * The function assumes the Lua stack have a least enough
 * space to push 2 element, its up to the caller to verify
 * this before calling this function. */
void luaSetErrorMetatable(lua_State *lua) {
    lua_newtable(lua);                              /* push metatable */
    lua_pushcfunction(lua, luaProtectedTableError); /* push get error handler */
    lua_setfield(lua, -2, "__index");
    lua_setmetatable(lua, -2);
}

static int luaNewIndexAllowList(lua_State *lua) {
    luaFuncCallCtx *rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    int argc = lua_gettop(lua);
    if (argc != 3) {
        ValkeyModule_Log(rctx->module_ctx, "warning", "malicious code trying to call luaNewIndexAllowList with wrong arguments");
        luaL_error(lua, "Wrong number of arguments to luaNewIndexAllowList");
    }
    if (!lua_istable(lua, -3)) {
        luaL_error(lua, "first argument to luaNewIndexAllowList must be a table");
    }
    if (!lua_isstring(lua, -2) && !lua_isnumber(lua, -2)) {
        luaL_error(lua, "Second argument to luaNewIndexAllowList must be a string or number");
    }
    const char *variable_name = lua_tostring(lua, -2);
    /* check if the key is in our allow list */

    char ***allow_l = allow_lists;
    for (; *allow_l; ++allow_l) {
        char **c = *allow_l;
        for (; *c; ++c) {
            if (strcmp(*c, variable_name) == 0) {
                break;
            }
        }
        if (*c) {
            break;
        }
    }
    int allowed = (*allow_l != NULL);
    /* If not explicitly allowed, check if it's a deprecated function. If so,
     * allow it only if 'lua_enable_insecure_api' config is enabled. */
    int deprecated = 0;
    if (!allowed) {
        char **c = lua_builtins_deprecated;
        for (; *c; ++c) {
            if (strcmp(*c, variable_name) == 0) {
                deprecated = 1;
                allowed = rctx->lua_enable_insecure_api ? 1 : 0;
                break;
            }
        }
    }
    if (!allowed) {
        /* Search the value on the back list, if its there we know that it was removed
         * on purpose and there is no need to print a warning. */
        char **c = deny_list;
        for (; *c; ++c) {
            if (strcmp(*c, variable_name) == 0) {
                break;
            }
        }
        if (!*c && !deprecated) {
            ValkeyModule_Log(rctx->module_ctx, "warning",
                             "A key '%s' was added to Lua globals which is neither on the globals allow list nor listed on the "
                             "deny list.",
                             variable_name);
        }
    } else {
        lua_rawset(lua, -3);
    }
    return 0;
}

/* Set a metatable with '__newindex' function that verify that
 * the new index appears on our globals while list.
 *
 * The metatable is set on the table which located on the top
 * of the stack.
 */
static void luaSetAllowListProtection(lua_State *lua) {
    lua_newtable(lua);                            /* push metatable */
    lua_pushcfunction(lua, luaNewIndexAllowList); /* push get error handler */
    lua_setfield(lua, -2, "__newindex");
    lua_setmetatable(lua, -2);
}

/* Set the readonly flag on the table located on the top of the stack
 * and recursively call this function on each table located on the original
 * table.  Also, recursively call this function on the metatables.*/
void luaSetTableProtectionRecursively(lua_State *lua) {
    /* This protect us from a loop in case we already visited the table
     * For example, globals has '_G' key which is pointing back to globals. */
    if (lua_isreadonlytable(lua, -1)) {
        return;
    }

    /* protect the current table */
    lua_enablereadonlytable(lua, -1, 1);

    lua_checkstack(lua, 2);
    lua_pushnil(lua); /* Use nil to start iteration. */
    while (lua_next(lua, -2)) {
        /* Stack now: table, key, value */
        if (lua_istable(lua, -1)) {
            luaSetTableProtectionRecursively(lua);
        }
        lua_pop(lua, 1);
    }

    /* protect the metatable if exists */
    if (lua_getmetatable(lua, -1)) {
        luaSetTableProtectionRecursively(lua);
        lua_pop(lua, 1); /* pop the metatable */
    }
}

/* Set the readonly flag on the metatable of basic types (string, nil etc.) */
void luaSetTableProtectionForBasicTypes(lua_State *lua) {
    static const int types[] = {
        LUA_TSTRING,
        LUA_TNUMBER,
        LUA_TBOOLEAN,
        LUA_TNIL,
        LUA_TFUNCTION,
        LUA_TTHREAD,
        LUA_TLIGHTUSERDATA};

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        /* Push a dummy value of the type to get its metatable */
        switch (types[i]) {
        case LUA_TSTRING: lua_pushstring(lua, ""); break;
        case LUA_TNUMBER: lua_pushnumber(lua, 0); break;
        case LUA_TBOOLEAN: lua_pushboolean(lua, 0); break;
        case LUA_TNIL: lua_pushnil(lua); break;
        case LUA_TFUNCTION: lua_pushcfunction(lua, NULL); break;
        case LUA_TTHREAD: lua_newthread(lua); break;
        case LUA_TLIGHTUSERDATA: lua_pushlightuserdata(lua, (void *)lua); break;
        }
        if (lua_getmetatable(lua, -1)) {
            luaSetTableProtectionRecursively(lua);
            lua_pop(lua, 1); /* pop metatable */
        }
        lua_pop(lua, 1); /* pop dummy value */
    }
}

void luaRegisterVersion(luaEngineCtx *ctx, lua_State *lua) {
    /* For legacy compatibility reasons include Redis versions. */
    lua_pushstring(lua, "REDIS_VERSION_NUM");
    lua_pushnumber(lua, ctx->redis_version_num);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REDIS_VERSION");
    lua_pushstring(lua, ctx->redis_version);
    lua_settable(lua, -3);

    /* Now push the Valkey version information. */
    lua_pushstring(lua, "VALKEY_VERSION_NUM");
    lua_pushnumber(lua, ctx->valkey_version_num);
    lua_settable(lua, -3);

    lua_pushstring(lua, "VALKEY_VERSION");
    lua_pushstring(lua, ctx->valkey_version);
    lua_settable(lua, -3);

    lua_pushstring(lua, "SERVER_NAME");
    lua_pushstring(lua, ctx->server_name);
    lua_settable(lua, -3);
}

void luaRegisterLogFunction(lua_State *lua) {
    /* server.log and log levels. */
    lua_pushstring(lua, "log");
    lua_pushcfunction(lua, luaLogCommand);
    lua_settable(lua, -3);

    lua_pushstring(lua, "LOG_DEBUG");
    lua_pushnumber(lua, LL_DEBUG);
    lua_settable(lua, -3);

    lua_pushstring(lua, "LOG_VERBOSE");
    lua_pushnumber(lua, LL_VERBOSE);
    lua_settable(lua, -3);

    lua_pushstring(lua, "LOG_NOTICE");
    lua_pushnumber(lua, LL_NOTICE);
    lua_settable(lua, -3);

    lua_pushstring(lua, "LOG_WARNING");
    lua_pushnumber(lua, LL_WARNING);
    lua_settable(lua, -3);
}

/*
 * Adds server.* functions/fields to lua such as server.call etc.
 * This function only handles fields common between Functions and LUA scripting.
 * scriptingInit() and functionsInit() may add additional fields specific to each.
 */
void luaRegisterServerAPI(luaEngineCtx *ctx, lua_State *lua) {
    /* In addition to registering server.call/pcall API, we will throw a custom message when a script accesses
     * undefined global variable. LUA stores global variables in the global table, accessible to us on stack at virtual
     * index = LUA_GLOBALSINDEX. We will set __index handler in global table's metatable to a custom C function to
     * achieve this - handled by luaSetAllowListProtection. Refer to https://www.lua.org/pil/13.4.1.html for
     * documentation on __index and https://www.lua.org/pil/contents.html#13 for documentation on metatables. We need to
     * pass global table to lua invocations as parameters. To achieve this, lua_pushvalue invocation brings global
     * variable table to the top of the stack by pushing value from global index onto the stack. And lua_pop invocation
     * after luaSetAllowListProtection removes it - resetting the stack to its original state. */
    lua_pushvalue(lua, LUA_GLOBALSINDEX);
    luaSetAllowListProtection(lua);
    lua_pop(lua, 1);

    luaFuncCallCtx call_ctx = {
        .lua_enable_insecure_api = ctx->lua_enable_insecure_api,
    };
    luaSaveOnRegistry(lua, REGISTRY_RUN_CTX_NAME, &call_ctx);

    /* Add default C functions provided in deps/lua codebase to handle basic data types such as table, string etc. */
    luaLoadLibraries(lua);

    luaSaveOnRegistry(lua, REGISTRY_RUN_CTX_NAME, NULL);

    /* Before Redis OSS 7, Lua used to return error messages as strings from pcall function. With Valkey (or Redis OSS 7), Lua now returns
     * error messages as tables. To keep backwards compatibility, we wrap the Lua pcall function with our own
     * implementation of C function that converts table to string. */
    lua_pushcfunction(lua, luaRedisPcall);
    lua_setglobal(lua, "pcall");

    /* Create a top level table object on the stack to temporarily hold fields for 'server' table. We will name it as
     * 'server' and send it to LUA at the very end. Also add 'call' and 'pcall' functions to the table. */
    lua_newtable(lua);
    lua_pushstring(lua, "call");
    lua_pushcfunction(lua, luaRedisCallCommand);
    lua_settable(lua, -3);
    lua_pushstring(lua, "pcall");
    lua_pushcfunction(lua, luaRedisPCallCommand);
    lua_settable(lua, -3);

    /* Add server.log function and debug level constants. LUA scripts use it to print messages to server log. */
    luaRegisterLogFunction(lua);

    /* Add SERVER_VERSION_NUM, SERVER_VERSION and SERVER_NAME fields with appropriate values. */
    luaRegisterVersion(ctx, lua);

    /* Add server.setresp function to allow LUA scripts to change the RESP version for server.call and server.pcall
     * invocations. */
    lua_pushstring(lua, "setresp");
    lua_pushcfunction(lua, luaSetResp);
    lua_settable(lua, -3);

    /* Add server.sha1hex function.  */
    lua_pushstring(lua, "sha1hex");
    lua_pushcfunction(lua, luaRedisSha1hexCommand);
    lua_settable(lua, -3);

    /* Add server.error_reply and server.status_reply functions. */
    lua_pushstring(lua, "error_reply");
    lua_pushcfunction(lua, luaRedisErrorReplyCommand);
    lua_settable(lua, -3);
    lua_pushstring(lua, "status_reply");
    lua_pushcfunction(lua, luaRedisStatusReplyCommand);
    lua_settable(lua, -3);

    /* Add server.set_repl function and associated flags. */
    lua_pushstring(lua, "set_repl");
    lua_pushcfunction(lua, luaRedisSetReplCommand);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REPL_NONE");
    lua_pushnumber(lua, PROPAGATE_NONE);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REPL_AOF");
    lua_pushnumber(lua, PROPAGATE_AOF);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REPL_SLAVE");
    lua_pushnumber(lua, PROPAGATE_REPL);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REPL_REPLICA");
    lua_pushnumber(lua, PROPAGATE_REPL);
    lua_settable(lua, -3);

    lua_pushstring(lua, "REPL_ALL");
    lua_pushnumber(lua, PROPAGATE_AOF | PROPAGATE_REPL);
    lua_settable(lua, -3);

    /* Add server.acl_check_cmd function. */
    lua_pushstring(lua, "acl_check_cmd");
    lua_pushcfunction(lua, luaRedisAclCheckCmdPermissionsCommand);
    lua_settable(lua, -3);

    /* Finally set the table as 'server' global var.
     * We will also alias it to 'redis' global var for backwards compatibility. */
    lua_setglobal(lua, SERVER_API_NAME);
    /* lua_getglobal invocation retrieves the 'server' variable value to the stack.
     * lua_setglobal invocation uses the value from stack to set 'redis' global variable.
     * This is not a deep copy but is enough for our purpose here. */
    lua_getglobal(lua, SERVER_API_NAME);
    lua_setglobal(lua, REDIS_API_NAME);

    /* Replace math.random and math.randomseed with custom implementations. */
    lua_getglobal(lua, "math");
    lua_pushstring(lua, "random");
    lua_pushcfunction(lua, server_math_random);
    lua_settable(lua, -3);
    lua_pushstring(lua, "randomseed");
    lua_pushcfunction(lua, server_math_randomseed);
    lua_settable(lua, -3);
    /* overwrite value of global variable 'math' with this modified math table present on top of stack. */
    lua_setglobal(lua, "math");
}

/* Set an array of String Objects as a Lua array (table) stored into a
 * global variable. */
static void luaCreateArray(lua_State *lua, ValkeyModuleString **elev, int elec) {
    int j;

    lua_createtable(lua, elec, 0);
    for (j = 0; j < elec; j++) {
        size_t len = 0;
        const char *str = ValkeyModule_StringPtrLen(elev[j], &len);
        lua_pushlstring(lua, str, len);
        lua_rawseti(lua, -2, j + 1);
    }
}

/* ---------------------------------------------------------------------------
 * Custom provided math.random
 * ------------------------------------------------------------------------- */

/* We replace math.random() with our implementation that is not affected
 * by specific libc random() implementations and will output the same sequence
 * (for the same seed) in every arch. */

/* The following implementation is the one shipped with Lua itself but with
 * rand() replaced by serverLrand48(). */
static int server_math_random(lua_State *L) {
    /* the `%' avoids the (rare) case of r==1, and is needed also because on
       some systems (SunOS!) `rand()' may return a value larger than RAND_MAX */
    lua_Number r = (lua_Number)(serverLrand48() % SERVER_LRAND48_MAX) /
                   (lua_Number)SERVER_LRAND48_MAX;
    switch (lua_gettop(L)) {  /* check number of arguments */
    case 0: {                 /* no arguments */
        lua_pushnumber(L, r); /* Number between 0 and 1 */
        break;
    }
    case 1: { /* only upper limit */
        int u = luaL_checkint(L, 1);
        luaL_argcheck(L, 1 <= u, 1, "interval is empty");
        lua_pushnumber(L, floor(r * u) + 1); /* int between 1 and `u' */
        break;
    }
    case 2: { /* lower and upper limits */
        int l = luaL_checkint(L, 1);
        int u = luaL_checkint(L, 2);
        luaL_argcheck(L, l <= u, 2, "interval is empty");
        lua_pushnumber(L, floor(r * (u - l + 1)) + l); /* int between `l' and `u' */
        break;
    }
    default: return luaL_error(L, "wrong number of arguments");
    }
    return 1;
}

static int server_math_randomseed(lua_State *L) {
    serverSrand48(luaL_checkint(L, 1));
    return 0;
}

/* This is the Lua script "count" hook that we use to detect scripts timeout. */
static void luaMaskCountHook(lua_State *lua, lua_Debug *ar) {
    VALKEYMODULE_NOT_USED(ar);

    luaFuncCallCtx *rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    ValkeyModule_Assert(rctx); /* Only supported inside script invocation */

    ValkeyModuleScriptingEngineExecutionState state = ValkeyModule_GetFunctionExecutionState(rctx->run_ctx);
    if (state == VMSE_STATE_KILLED) {
        char *err = NULL;
        if (rctx->type == VMSE_EVAL) {
            err = "ERR Script killed by user with SCRIPT KILL.";
        } else {
            err = "ERR Script killed by user with FUNCTION KILL.";
        }
        ValkeyModule_Log(NULL, "notice", "%s", err);

        /*
         * Set the hook to invoke all the time so the user
         * will not be able to catch the error with pcall and invoke
         * pcall again which will prevent the script from ever been killed
         */
        lua_sethook(lua, luaMaskCountHook, LUA_MASKLINE, 0);

        luaPushError(lua, err);
        luaError(lua);
    }
}

void luaErrorInformationDiscard(errorInfo *err_info) {
    if (err_info->msg) ValkeyModule_Free(err_info->msg);
    if (err_info->source) ValkeyModule_Free(err_info->source);
    if (err_info->line) ValkeyModule_Free(err_info->line);
}

void luaExtractErrorInformation(lua_State *lua, errorInfo *err_info) {
    if (lua_isstring(lua, -1)) {
        err_info->msg = lm_asprintf("ERR %s", lua_tostring(lua, -1));
        err_info->line = NULL;
        err_info->source = NULL;
        err_info->ignore_err_stats_update = 0;
        return;
    }

    lua_getfield(lua, -1, "err");
    if (lua_isstring(lua, -1)) {
        err_info->msg = lm_strcpy(lua_tostring(lua, -1));
    }
    lua_pop(lua, 1);

    lua_getfield(lua, -1, "source");
    if (lua_isstring(lua, -1)) {
        err_info->source = lm_strcpy(lua_tostring(lua, -1));
    }
    lua_pop(lua, 1);

    lua_getfield(lua, -1, "line");
    if (lua_isstring(lua, -1)) {
        err_info->line = lm_strcpy(lua_tostring(lua, -1));
    }
    lua_pop(lua, 1);

    lua_getfield(lua, -1, "ignore_error_stats_update");
    if (lua_isboolean(lua, -1)) {
        err_info->ignore_err_stats_update = lua_toboolean(lua, -1);
    }
    lua_pop(lua, 1);

    if (err_info->msg == NULL) {
        /* Ensure we never return a NULL msg. */
        err_info->msg = lm_strcpy("ERR unknown error");
    }
}

/* This is the core of our Lua debugger, called each time Lua is about
 * to start executing a new line. */
void luaLdbLineHook(lua_State *lua, lua_Debug *ar) {
    ValkeyModuleScriptingEngineServerRuntimeCtx *rctx = luaGetFromRegistry(lua, REGISTRY_RUN_CTX_NAME);
    ValkeyModule_Assert(rctx); /* Only supported inside script invocation */
    lua_getstack(lua, 0, ar);
    lua_getinfo(lua, "Sl", ar);
    ldbSetCurrentLine(ar->currentline);

    int bp = ldbShouldBreak();
    int timeout = 0;

    /* Events outside our script are not interesting. */
    if (strstr(ar->short_src, "user_script") == NULL) return;

    /* Check if a timeout occurred. */
    if (ar->event == LUA_HOOKCOUNT && !ldbIsStepEnabled() && bp == 0) {
        // mstime_t elapsed = elapsedMs(rctx->start_time);
        // mstime_t timelimit = server.busy_reply_threshold ? server.busy_reply_threshold : 5000;
        // if (elapsed >= timelimit) {
        //     timeout = 1;
        //     ldbSetStepMode(1);
        // } else {
        return; /* No timeout, ignore the COUNT event. */
        // }
    }

    if (ldbIsStepEnabled() || bp) {
        char *reason = "step over";
        if (bp)
            reason = ldbIsBreakpointOnNextLineEnabled() ? "server.breakpoint() called" : "break point";
        else if (timeout)
            reason = "timeout reached, infinite loop?";
        ldbSetStepMode(0);
        ldbSetBreakpointOnNextLine(0);
        ValkeyModuleString *msg = ValkeyModule_CreateStringPrintf(NULL, "* Stopped at %d, stop reason = %s", ldbGetCurrentLine(), reason);
        ldbLog(msg);
        ldbLogSourceLine(ldbGetCurrentLine());
        ldbSendLogs();
        if (ldbRepl(lua) == C_ERR && timeout) {
            /* If the client closed the connection and we have a timeout
             * connection, let's kill the script otherwise the process
             * will remain blocked indefinitely. */
            luaPushError(lua, "timeout during Lua debugging with client closing connection");
            luaError(lua);
        }
        // rctx->start_time = getMonotonicUs();
    }
}

void luaCallFunction(ValkeyModuleCtx *ctx,
                     ValkeyModuleScriptingEngineServerRuntimeCtx *run_ctx,
                     ValkeyModuleScriptingEngineSubsystemType type,
                     lua_State *lua,
                     ValkeyModuleString **keys,
                     size_t nkeys,
                     ValkeyModuleString **args,
                     size_t nargs,
                     int debug_enabled,
                     int lua_enable_insecure_api) {
    int delhook = 0;

    /* We must set it before we set the Lua hook, theoretically the
     * Lua hook might be called wheneven we run any Lua instruction
     * such as 'luaSetGlobalArray' and we want the run_ctx to be available
     * each time the Lua hook is invoked. */

    luaFuncCallCtx call_ctx = {
        .module_ctx = ctx,
        .run_ctx = run_ctx,
        .type = type,
        .replication_flags = PROPAGATE_AOF | PROPAGATE_REPL,
        .resp = 2,
        .lua_enable_insecure_api = lua_enable_insecure_api,
    };

    luaSaveOnRegistry(lua, REGISTRY_RUN_CTX_NAME, &call_ctx);

    if (!debug_enabled) {
        lua_sethook(lua, luaMaskCountHook, LUA_MASKCOUNT, 100000);
        delhook = 1;
    } else if (debug_enabled) {
        lua_sethook(lua, luaLdbLineHook, LUA_MASKLINE | LUA_MASKCOUNT, 100000);
        delhook = 1;
    }

    /* Populate the argv and keys table accordingly to the arguments that
     * EVAL received. */
    luaCreateArray(lua, keys, nkeys);
    /* On eval, keys and arguments are globals. */
    if (type == VMSE_EVAL) {
        /* open global protection to set KEYS */
        lua_enablereadonlytable(lua, LUA_GLOBALSINDEX, 0);
        lua_setglobal(lua, "KEYS");
        lua_enablereadonlytable(lua, LUA_GLOBALSINDEX, 1);
    }
    luaCreateArray(lua, args, nargs);
    if (type == VMSE_EVAL) {
        /* open global protection to set ARGV */
        lua_enablereadonlytable(lua, LUA_GLOBALSINDEX, 0);
        lua_setglobal(lua, "ARGV");
        lua_enablereadonlytable(lua, LUA_GLOBALSINDEX, 1);
    }

    /* At this point whether this script was never seen before or if it was
     * already defined, we can call it.
     * On eval mode, we have zero arguments and expect a single return value.
     * In addition the error handler is located on position -2 on the Lua stack.
     * On function mode, we pass 2 arguments (the keys and args tables),
     * and the error handler is located on position -4 (stack: error_handler, callback, keys, args) */
    int err;
    if (type == VMSE_EVAL) {
        err = lua_pcall(lua, 0, 1, -2);
    } else {
        err = lua_pcall(lua, 2, 1, -4);
    }

/* Call the Lua garbage collector from time to time to avoid a
 * full cycle performed by Lua, which adds too latency.
 *
 * The call is performed every LUA_GC_CYCLE_PERIOD executed commands
 * (and for LUA_GC_CYCLE_PERIOD collection steps) because calling it
 * for every command uses too much CPU. */
#define LUA_GC_CYCLE_PERIOD 50
    {
        static long gc_count = 0;

        gc_count++;
        if (gc_count == LUA_GC_CYCLE_PERIOD) {
            lua_gc(lua, LUA_GCSTEP, LUA_GC_CYCLE_PERIOD);
            gc_count = 0;
        }
    }

    if (err) {
        /* Error object is a table of the following format:
         * {err='<error msg>', source='<source file>', line=<line>}
         * We can construct the error message from this information */
        if (!lua_istable(lua, -1)) {
            const char *msg = "execution failure";
            if (lua_isstring(lua, -1)) {
                msg = lua_tostring(lua, -1);
            }
            ValkeyModule_ReplyWithErrorFormat(ctx, "ERR Error running script, %.100s\n", msg);
        } else {
            errorInfo err_info = {0};
            luaExtractErrorInformation(lua, &err_info);
            if (err_info.line && err_info.source) {
                ValkeyModule_ReplyWithCustomErrorFormat(
                    ctx,
                    !err_info.ignore_err_stats_update,
                    "%s script: on %s:%s.",
                    err_info.msg,
                    err_info.source,
                    err_info.line);
            } else {
                ValkeyModule_ReplyWithCustomErrorFormat(
                    ctx,
                    !err_info.ignore_err_stats_update,
                    "%s",
                    err_info.msg);
            }
            luaErrorInformationDiscard(&err_info);
        }
        lua_pop(lua, 1); /* Consume the Lua error */
    } else {
        /* On success convert the Lua return value into RESP, and
         * send it to * the client. */

        luaReplyToServerReply(ctx, call_ctx.resp, lua); /* Convert and consume the reply. */
    }

    /* Perform some cleanup that we need to do both on error and success. */
    if (delhook) lua_sethook(lua, NULL, 0, 0); /* Disable hook */

    /* remove run_ctx from registry, its only applicable for the current script. */
    luaSaveOnRegistry(lua, REGISTRY_RUN_CTX_NAME, NULL);
}

unsigned long luaMemory(lua_State *lua) {
    return lua_gc(lua, LUA_GCCOUNT, 0) * 1024LL;
}
