/*
 * Copyright (c) 2009-2012, Redis Ltd.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "debug_lua.h"
#include "script_lua.h"

#include <stdio.h>
#include <string.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

extern char *lm_asprintf(char const *fmt, ...);

/* ---------------------------------------------------------------------------
 * LDB: Lua debugging facilities
 * ------------------------------------------------------------------------- */

/* Debugger shared state is stored inside this global structure. */
#define LDB_BREAKPOINTS_MAX 64 /* Max number of breakpoints. */
struct ldbState {
    int active;                  /* Are we debugging EVAL right now? */
    int bp[LDB_BREAKPOINTS_MAX]; /* An array of breakpoints line numbers. */
    int bpcount;                 /* Number of valid entries inside bp. */
    int step;                    /* Stop at next line regardless of breakpoints. */
    int luabp;                   /* Stop at next line because server.breakpoint() was called. */
    char **src;                  /* Lua script source code split by line. */
    int lines;                   /* Number of lines in 'src'. */
    int currentline;             /* Current line number. */
} ldb;

/* Initialize Lua debugger data structures. */
void ldbInit(void) {
    ldb.active = 0;
    ldb.bpcount = 0;
    ldb.step = 0;
    ldb.luabp = 0;
    ldb.src = NULL;
    ldb.lines = 0;
    ldb.currentline = -1;
}

int ldbIsEnabled(void) {
    return ldb.active && ldb.step;
}

/* Enable debug mode of Lua scripts for this client. */
void ldbEnable(void) {
    ldb.active = 1;
    ldb.step = 1;
    ldb.bpcount = 0;
    ldb.luabp = 0;
}

/* Exit debugging mode from the POV of client. This function is not enough
 * to properly shut down a client debugging session, see ldbEndSession()
 * for more information. */
void ldbDisable(void) {
    ldb.step = 0;
    ldb.active = 0;
}

static char **split_text_by_lines(const char *text, size_t len, int *lines) {
    ValkeyModule_Assert(text != NULL && len > 0);

    int count = 1;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') count++;
    }

    char **result = ValkeyModule_Calloc(count, sizeof(char *));
    if (!result) {
        ValkeyModule_Log(NULL, "error", "Failed to allocate memory for Lua source code lines.");
        *lines = 0;
        return NULL;
    }

    size_t start = 0, idx = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            size_t linelen = i - start;
            char *line = ValkeyModule_Calloc(linelen + 1, 1);
            if (line) {
                memcpy(line, text + start, linelen);
                line[linelen] = '\0';
                result[idx++] = line;
            }
            start = i + 1;
        }
    }
    *lines = idx;
    return result;
}

void ldbStart(ValkeyModuleString *source) {
    ldb.active = 1;

    /* First argument of EVAL is the script itself. We split it into different
     * lines since this is the way the debugger accesses the source code. */
    size_t srclen;
    const char *src_raw = ValkeyModule_StringPtrLen(source, &srclen);
    while (srclen && (src_raw[srclen - 1] == '\n' || src_raw[srclen - 1] == '\r')) {
        --srclen;
    }
    ldb.src = split_text_by_lines(src_raw, srclen, &ldb.lines);
}

void ldbEnd(void) {
    for (int i = 0; i < ldb.lines; i++) {
        ValkeyModule_Free(ldb.src[i]);
    }
    ValkeyModule_Free(ldb.src);
    ldb.lines = 0;
    ldb.active = 0;
}

void ldbLog(ValkeyModuleString *entry) {
    ValkeyModule_ScriptingEngineDebuggerLog(entry, 0);
}

void ldbLogCString(const char *c_str) {
    ValkeyModuleString *entry = ValkeyModule_CreateString(NULL, c_str, strlen(c_str));
    ldbLog(entry);
}

void ldbSendLogs(void) {
    ValkeyModule_ScriptingEngineDebuggerFlushLogs();
}

/* Return a pointer to ldb.src source code line, considering line to be
 * one-based, and returning a special string for out of range lines. */
static char *ldbGetSourceLine(int line) {
    int idx = line - 1;
    if (idx < 0 || idx >= ldb.lines) return "<out of range source code line>";
    return ldb.src[idx];
}

/* Return true if there is a breakpoint in the specified line. */
static int ldbIsBreakpoint(int line) {
    int j;

    for (j = 0; j < ldb.bpcount; j++)
        if (ldb.bp[j] == line) return 1;
    return 0;
}

/* Add the specified breakpoint. Ignore it if we already reached the max.
 * Returns 1 if the breakpoint was added (or was already set). 0 if there is
 * no space for the breakpoint or if the line is invalid. */
static int ldbAddBreakpoint(int line) {
    if (line <= 0 || line > ldb.lines) return 0;
    if (!ldbIsBreakpoint(line) && ldb.bpcount != LDB_BREAKPOINTS_MAX) {
        ldb.bp[ldb.bpcount++] = line;
        return 1;
    }
    return 0;
}

/* Remove the specified breakpoint, returning 1 if the operation was
 * performed or 0 if there was no such breakpoint. */
static int ldbDelBreakpoint(int line) {
    int j;

    for (j = 0; j < ldb.bpcount; j++) {
        if (ldb.bp[j] == line) {
            ldb.bpcount--;
            memmove(ldb.bp + j, ldb.bp + j + 1, ldb.bpcount - j);
            return 1;
        }
    }
    return 0;
}

/* Log the specified line in the Lua debugger output. */
void ldbLogSourceLine(int lnum) {
    char *line = ldbGetSourceLine(lnum);
    char *prefix;
    int bp = ldbIsBreakpoint(lnum);
    int current = ldb.currentline == lnum;

    if (current && bp)
        prefix = "->#";
    else if (current)
        prefix = "-> ";
    else if (bp)
        prefix = "  #";
    else
        prefix = "   ";
    ValkeyModuleString *thisline = ValkeyModule_CreateStringPrintf(NULL, "%s%-3d %s", prefix, lnum, line);
    ldbLog(thisline);
}

/* Implement the "list" command of the Lua debugger. If around is 0
 * the whole file is listed, otherwise only a small portion of the file
 * around the specified line is shown. When a line number is specified
 * the amount of context (lines before/after) is specified via the
 * 'context' argument. */
static void ldbList(int around, int context) {
    int j;

    for (j = 1; j <= ldb.lines; j++) {
        if (around != 0 && abs(around - j) > context) continue;
        ldbLogSourceLine(j);
    }
}

/* Append a human readable representation of the Lua value at position 'idx'
 * on the stack of the 'lua' state, to the string passed as argument.
 * The new string with the represented value attached is returned.
 * Used in order to implement ldbLogStackValue().
 *
 * The element is neither automatically removed from the stack, nor
 * converted to a different type. */
#define LDB_MAX_VALUES_DEPTH (LUA_MINSTACK / 2)
static ValkeyModuleString *ldbCatStackValueRec(ValkeyModuleString *s, lua_State *lua, int idx, int level) {
    int t = lua_type(lua, idx);

    if (level++ == LDB_MAX_VALUES_DEPTH) {
        const char *msg = "<max recursion level reached! Nested table?>";
        ValkeyModule_StringAppendBuffer(NULL, s, msg, strlen(msg));
        return s;
    }

    switch (t) {
    case LUA_TSTRING: {
        size_t strl;
        char *strp = (char *)lua_tolstring(lua, idx, &strl);
        ValkeyModule_StringAppendBuffer(NULL, s, strp, strl);
    } break;
    case LUA_TBOOLEAN: {
        const char *bool_str = lua_toboolean(lua, idx) ? "true" : "false";
        ValkeyModule_StringAppendBuffer(NULL, s, bool_str, strlen(bool_str));
        break;
    }
    case LUA_TNUMBER: {
        ValkeyModuleString *old_s = s;
        const char *prefix = ValkeyModule_StringPtrLen(s, NULL);
        s = ValkeyModule_CreateStringPrintf(NULL, "%s%g", prefix, (double)lua_tonumber(lua, idx));
        ValkeyModule_FreeString(NULL, old_s);
        break;
    }
    case LUA_TNIL: ValkeyModule_StringAppendBuffer(NULL, s, "nil", 3); break;
    case LUA_TTABLE: {
        int expected_index = 1; /* First index we expect in an array. */
        int is_array = 1;       /* Will be set to null if check fails. */
        /* Note: we create two representations at the same time, one
         * assuming the table is an array, one assuming it is not. At the
         * end we know what is true and select the right one. */
        ValkeyModuleString *repr1 = ValkeyModule_CreateString(NULL, "", 0);
        ValkeyModuleString *repr2 = ValkeyModule_CreateString(NULL, "", 0);
        lua_pushnil(lua); /* The first key to start the iteration is nil. */
        while (lua_next(lua, idx - 1)) {
            /* Test if so far the table looks like an array. */
            if (is_array && (lua_type(lua, -2) != LUA_TNUMBER || lua_tonumber(lua, -2) != expected_index)) is_array = 0;
            /* Stack now: table, key, value */
            /* Array repr. */
            repr1 = ldbCatStackValueRec(repr1, lua, -1, level);
            ValkeyModule_StringAppendBuffer(NULL, repr1, "; ", 2);
            /* Full repr. */
            ValkeyModule_StringAppendBuffer(NULL, repr2, "[", 1);
            repr2 = ldbCatStackValueRec(repr2, lua, -2, level);
            ValkeyModule_StringAppendBuffer(NULL, repr2, "]=", 2);
            repr2 = ldbCatStackValueRec(repr2, lua, -1, level);
            ValkeyModule_StringAppendBuffer(NULL, repr2, "; ", 2);
            lua_pop(lua, 1); /* Stack: table, key. Ready for next iteration. */
            expected_index++;
        }

        /* Select the right one and discard the other. */
        ValkeyModule_StringAppendBuffer(NULL, s, "{", 1);
        size_t repr1_len;
        const char *repr1_str = ValkeyModule_StringPtrLen(repr1, &repr1_len);
        size_t repr2_len;
        const char *repr2_str = ValkeyModule_StringPtrLen(repr2, &repr2_len);
        ValkeyModule_StringAppendBuffer(NULL, s, is_array ? repr1_str : repr2_str, is_array ? repr1_len : repr2_len);
        ValkeyModule_StringAppendBuffer(NULL, s, "}", 1);
        ValkeyModule_FreeString(NULL, repr1);
        ValkeyModule_FreeString(NULL, repr2);
    } break;
    case LUA_TFUNCTION:
    case LUA_TUSERDATA:
    case LUA_TTHREAD:
    case LUA_TLIGHTUSERDATA: {
        const void *p = lua_topointer(lua, idx);
        char *typename = "unknown";
        if (t == LUA_TFUNCTION)
            typename = "function";
        else if (t == LUA_TUSERDATA)
            typename = "userdata";
        else if (t == LUA_TTHREAD)
            typename = "thread";
        else if (t == LUA_TLIGHTUSERDATA)
            typename = "light-userdata";
        ValkeyModuleString *old_s = s;
        const char *prefix = ValkeyModule_StringPtrLen(s, NULL);
        s = ValkeyModule_CreateStringPrintf(NULL, "%s \"%s@%p\"", prefix, typename, p);
        ValkeyModule_FreeString(NULL, old_s);
    } break;
    default: {
        const char *unknown_str = "\"<unknown-lua-type>\"";
        ValkeyModule_StringAppendBuffer(NULL, s, unknown_str, strlen(unknown_str));
        break;
    }
    }

    return s;
}

/* Higher level wrapper for ldbCatStackValueRec() that just uses an initial
 * recursion level of '0'. */
ValkeyModuleString *ldbCatStackValue(ValkeyModuleString *s, lua_State *lua, int idx) {
    return ldbCatStackValueRec(s, lua, idx, 0);
}

/* Produce a debugger log entry representing the value of the Lua object
 * currently on the top of the stack. The element is neither popped nor modified.
 * Check ldbCatStackValue() for the actual implementation. */
static void ldbLogStackValue(lua_State *lua, const char *prefix) {
    ValkeyModuleString *p = ValkeyModule_CreateString(NULL, prefix, strlen(prefix));
    ValkeyModuleString *s = ldbCatStackValue(p, lua, -1);
    ValkeyModule_ScriptingEngineDebuggerLog(s, 1);
    ValkeyModule_FreeString(NULL, s);
}

/* Log a RESP reply as debugger output, in a human readable format.
 * If the resulting string is longer than 'len' plus a few more chars
 * used as prefix, it gets truncated. */
void ldbLogRespReply(char *reply) {
    ValkeyModule_ScriptingEngineDebuggerLogRespReplyStr(reply);
}

/* Implements the "print <var>" command of the Lua debugger. It scans for Lua
 * var "varname" starting from the current stack frame up to the top stack
 * frame. The first matching variable is printed. */
static void ldbPrint(lua_State *lua, const char *varname) {
    lua_Debug ar;

    int l = 0; /* Stack level. */
    while (lua_getstack(lua, l, &ar) != 0) {
        l++;
        const char *name;
        int i = 1; /* Variable index. */
        while ((name = lua_getlocal(lua, &ar, i)) != NULL) {
            i++;
            if (strcmp(varname, name) == 0) {
                ldbLogStackValue(lua, "<value> ");
                lua_pop(lua, 1);
                return;
            } else {
                lua_pop(lua, 1); /* Discard the var name on the stack. */
            }
        }
    }

    /* Let's try with global vars in two selected cases */
    if (!strcmp(varname, "ARGV") || !strcmp(varname, "KEYS")) {
        lua_getglobal(lua, varname);
        ldbLogStackValue(lua, "<value> ");
        lua_pop(lua, 1);
    } else {
        ldbLogCString("No such variable.");
    }
}

/* Implements the "print" command (without arguments) of the Lua debugger.
 * Prints all the variables in the current stack frame. */
static void ldbPrintAll(lua_State *lua) {
    lua_Debug ar;
    int vars = 0;

    if (lua_getstack(lua, 0, &ar) != 0) {
        const char *name;
        int i = 1; /* Variable index. */
        while ((name = lua_getlocal(lua, &ar, i)) != NULL) {
            i++;
            if (!strstr(name, "(*temporary)")) {
                char *prefix = lm_asprintf("<value> %s = ", name);
                ldbLogStackValue(lua, prefix);
                ValkeyModule_Free(prefix);
                vars++;
            }
            lua_pop(lua, 1);
        }
    }

    if (vars == 0) {
        ldbLogCString("No local variables in the current context.");
    }
}

/* Implements the break command to list, add and remove breakpoints. */
static void ldbBreak(ValkeyModuleString **argv, int argc) {
    if (argc == 1) {
        if (ldb.bpcount == 0) {
            ldbLogCString("No breakpoints set. Use 'b <line>' to add one.");
            return;
        } else {
            char *msg = lm_asprintf("%i breakpoints set:", ldb.bpcount);
            ldbLogCString(msg);
            ValkeyModule_Free(msg);
            int j;
            for (j = 0; j < ldb.bpcount; j++) ldbLogSourceLine(ldb.bp[j]);
        }
    } else {
        int j;
        for (j = 1; j < argc; j++) {
            long long line;
            int res = ValkeyModule_StringToLongLong(argv[j], &line);
            if (res != VALKEYMODULE_OK) {
                const char *arg = ValkeyModule_StringPtrLen(argv[j], NULL);
                char *msg = lm_asprintf("Invalid argument:'%s'", arg);
                ldbLogCString(msg);
                ValkeyModule_Free(msg);
            } else {
                if (line == 0) {
                    ldb.bpcount = 0;
                    ldbLogCString("All breakpoints removed.");
                } else if (line > 0) {
                    if (ldb.bpcount == LDB_BREAKPOINTS_MAX) {
                        ldbLogCString("Too many breakpoints set.");
                    } else if (ldbAddBreakpoint(line)) {
                        ldbList(line, 1);
                    } else {
                        ldbLogCString("Wrong line number.");
                    }
                } else if (line < 0) {
                    if (ldbDelBreakpoint(-line))
                        ldbLogCString("Breakpoint removed.");
                    else
                        ldbLogCString("No breakpoint in the specified line.");
                }
            }
        }
    }
}

/* Implements the Lua debugger "eval" command. It just compiles the user
 * passed fragment of code and executes it, showing the result left on
 * the stack. */
static void ldbEval(lua_State *lua, ValkeyModuleString **argv, int argc) {
    /* Glue the script together if it is composed of multiple arguments. */
    ValkeyModuleString *code = ValkeyModule_CreateString(NULL, "", 0);
    for (int j = 1; j < argc; j++) {
        size_t arglen;
        const char *arg = ValkeyModule_StringPtrLen(argv[j], &arglen);
        ValkeyModule_StringAppendBuffer(NULL, code, arg, arglen);
        if (j != argc - 1) {
            ValkeyModule_StringAppendBuffer(NULL, code, " ", 1);
        }
    }

    ValkeyModuleString *expr = ValkeyModule_CreateStringPrintf(NULL, "return %s", ValkeyModule_StringPtrLen(code, NULL));

    size_t code_len;
    const char *code_str = ValkeyModule_StringPtrLen(code, &code_len);

    size_t expr_len;
    const char *expr_str = ValkeyModule_StringPtrLen(expr, &expr_len);

    /* Try to compile it as an expression, prepending "return ". */
    if (luaL_loadbuffer(lua, expr_str, expr_len, "@ldb_eval")) {
        lua_pop(lua, 1);
        /* Failed? Try as a statement. */
        if (luaL_loadbuffer(lua, code_str, code_len, "@ldb_eval")) {
            char *err_msg = lm_asprintf("Error compiling code: %s", lua_tostring(lua, -1));
            ldbLogCString(err_msg);
            ValkeyModule_Free(err_msg);
            ValkeyModule_FreeString(NULL, code);
            ValkeyModule_FreeString(NULL, expr);
            return;
        }
    }

    /* Call it. */
    ValkeyModule_FreeString(NULL, code);
    ValkeyModule_FreeString(NULL, expr);
    if (lua_pcall(lua, 0, 1, 0)) {
        char *err_msg = lm_asprintf("<error> %s", lua_tostring(lua, -1));
        ldbLogCString(err_msg);
        ValkeyModule_Free(err_msg);
        lua_pop(lua, 1);
        return;
    }
    ldbLogStackValue(lua, "<retval> ");
    lua_pop(lua, 1);
}

/* Implement the debugger "server" command. We use a trick in order to make
 * the implementation very simple: we just call the Lua server.call() command
 * implementation, with ldb.step enabled, so as a side effect the command
 * and its reply are logged. */
static void ldbServer(lua_State *lua, ValkeyModuleString **argv, int argc) {
    int j;

    if (!lua_checkstack(lua, argc + 1)) {
        /* Increase the Lua stack if needed to make sure there is enough room
         * to push 'argc + 1' elements to the stack. On failure, return error.
         * Notice that we need, in worst case, 'argc + 1' elements because we push all the arguments
         * given by the user (without the first argument) and we also push the 'server' global table and
         * 'server.call' function so:
         * (1 (server table)) + (1 (server.call function)) + (argc - 1 (all arguments without the first)) = argc + 1*/
        ldbLogRespReply("max lua stack reached");
        return;
    }

    lua_getglobal(lua, "server");
    lua_pushstring(lua, "call");
    lua_gettable(lua, -2); /* Stack: server, server.call */
    for (j = 1; j < argc; j++) {
        size_t arg_len;
        const char *arg = ValkeyModule_StringPtrLen(argv[j], &arg_len);
        lua_pushlstring(lua, arg, arg_len);
    }
    ldb.step = 1;                   /* Force server.call() to log. */
    lua_pcall(lua, argc - 1, 1, 0); /* Stack: server, result */
    ldb.step = 0;                   /* Disable logging. */
    lua_pop(lua, 2);                /* Discard the result and clean the stack. */
}

/* Implements "trace" command of the Lua debugger. It just prints a backtrace
 * querying Lua starting from the current callframe back to the outer one. */
static void ldbTrace(lua_State *lua) {
    lua_Debug ar;
    int level = 0;

    while (lua_getstack(lua, level, &ar)) {
        lua_getinfo(lua, "Snl", &ar);
        if (strstr(ar.short_src, "user_script") != NULL) {
            char *msg = lm_asprintf("%s %s:", (level == 0) ? "In" : "From", ar.name ? ar.name : "top level");
            ldbLogCString(msg);
            ValkeyModule_Free(msg);
            ldbLogSourceLine(ar.currentline);
        }
        level++;
    }
    if (level == 0) {
        ldbLogCString("<error> Can't retrieve Lua stack.");
    }
}

#define CONTINUE_SCRIPT_EXECUTION 0
#define CONTINUE_READ_NEXT_COMMAND 1

static int stepCommandHandler(ValkeyModuleString **argv, size_t argc, void *context) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    VALKEYMODULE_NOT_USED(context);
    ldb.step = 1;
    return CONTINUE_SCRIPT_EXECUTION;
}

static int continueCommandHandler(ValkeyModuleString **argv, size_t argc, void *context) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    VALKEYMODULE_NOT_USED(context);
    return CONTINUE_SCRIPT_EXECUTION;
}

static int listCommandHandler(ValkeyModuleString **argv, size_t argc, void *context) {
    VALKEYMODULE_NOT_USED(context);
    int around = ldb.currentline, ctx = 5;
    if (argc > 1) {
        int num = atoi(ValkeyModule_StringPtrLen(argv[1], NULL));
        if (num > 0) around = num;
    }
    if (argc > 2) ctx = atoi(ValkeyModule_StringPtrLen(argv[2], NULL));
    ldbList(around, ctx);
    ValkeyModule_ScriptingEngineDebuggerFlushLogs();
    return CONTINUE_READ_NEXT_COMMAND;
}

static int wholeCommandHandler(ValkeyModuleString **argv, size_t argc, void *context) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    VALKEYMODULE_NOT_USED(context);
    ldbList(1, 1000000);
    ValkeyModule_ScriptingEngineDebuggerFlushLogs();
    return CONTINUE_READ_NEXT_COMMAND;
}

static int printCommandHandler(ValkeyModuleString **argv, size_t argc, void *context) {
    ValkeyModule_Assert(context != NULL);
    lua_State *lua = context;
    if (argc == 2) {
        ldbPrint(lua, ValkeyModule_StringPtrLen(argv[1], NULL));
    } else {
        ldbPrintAll(lua);
    }
    ValkeyModule_ScriptingEngineDebuggerFlushLogs();
    return CONTINUE_READ_NEXT_COMMAND;
}

static int breakCommandHandler(ValkeyModuleString **argv, size_t argc, void *context) {
    VALKEYMODULE_NOT_USED(context);
    ldbBreak(argv, argc);
    ValkeyModule_ScriptingEngineDebuggerFlushLogs();
    return CONTINUE_READ_NEXT_COMMAND;
}

static int traceCommandHandler(ValkeyModuleString **argv, size_t argc, void *context) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    VALKEYMODULE_NOT_USED(context);
    lua_State *lua = context;
    ldbTrace(lua);
    ValkeyModule_ScriptingEngineDebuggerFlushLogs();
    return CONTINUE_READ_NEXT_COMMAND;
}

static int evalCommandHandler(ValkeyModuleString **argv, size_t argc, void *context) {
    ValkeyModule_Assert(context != NULL);
    lua_State *lua = context;
    ldbEval(lua, argv, argc);
    ValkeyModule_ScriptingEngineDebuggerFlushLogs();
    return CONTINUE_READ_NEXT_COMMAND;
}

static int valkeyCommandHandler(ValkeyModuleString **argv, size_t argc, void *context) {
    ValkeyModule_Assert(context != NULL);
    lua_State *lua = context;
    ldbServer(lua, argv, argc);
    ValkeyModule_ScriptingEngineDebuggerFlushLogs();
    return CONTINUE_READ_NEXT_COMMAND;
}

static int abortCommandHandler(ValkeyModuleString **argv, size_t argc, void *context) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    VALKEYMODULE_NOT_USED(context);
    ValkeyModule_Assert(context != NULL);
    lua_State *lua = context;
    luaPushError(lua, "script aborted for user request");
    luaError(lua);
    return CONTINUE_READ_NEXT_COMMAND;
}

static ValkeyModuleScriptingEngineDebuggerCommand *commands_array_cache = NULL;
static size_t commands_array_len = 0;

void ldbGenerateDebuggerCommandsArray(lua_State *lua,
                                      const ValkeyModuleScriptingEngineDebuggerCommand **commands,
                                      size_t *commands_len) {
    static ValkeyModuleScriptingEngineDebuggerCommandParam list_params[] = {
        {.name = "line", .optional = 1},
        {.name = "ctx", .optional = 1},
    };

    static ValkeyModuleScriptingEngineDebuggerCommandParam print_params[] = {
        {.name = "var", .optional = 1},
    };

    static ValkeyModuleScriptingEngineDebuggerCommandParam break_params[] = {
        {.name = "line|-line", .optional = 1},
    };

    static ValkeyModuleScriptingEngineDebuggerCommandParam eval_params[] = {
        {.name = "code", .optional = 0, .variadic = 1},
    };

    static ValkeyModuleScriptingEngineDebuggerCommandParam valkey_params[] = {
        {.name = "cmd", .optional = 0, .variadic = 1},
    };

    if (commands_array_cache == NULL) {
        ValkeyModuleScriptingEngineDebuggerCommand commands_array[] = {
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND("step", 1, NULL, 0, "Run current line and stop again.", 0, stepCommandHandler),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND("next", 1, NULL, 0, "Alias for step.", 0, stepCommandHandler),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND("continue", 1, NULL, 0, "Run till next breakpoint.", 0, continueCommandHandler),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND("list", 1, list_params, 2, "List source code around a specific line. If no line is specified the list is printed around the current line. [ctx] specifies how many lines to show before/after [line].", 0, listCommandHandler),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND("whole", 1, NULL, 0, "List all source code. Alias for 'list 1 1000000'.", 0, wholeCommandHandler),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND_WITH_CTX("print", 1, print_params, 1, "Show the value of the specified variable [var]. Can also show global vars KEYS and ARGV. If no [var] is specidied, shows the value of all local variables.", 0, printCommandHandler, lua),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND("break", 1, break_params, 1, "Add/Remove a breakpoint to the specified line. If no [line] is specified, it shows all breakpoints. When line = 0, it removes all breakpoints.", 0, breakCommandHandler),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND_WITH_CTX("trace", 1, NULL, 0, "Show a backtrace.", 0, traceCommandHandler, lua),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND_WITH_CTX("eval", 1, eval_params, 1, "Execute some Lua code (in a different callframe).", 0, evalCommandHandler, lua),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND_WITH_CTX("valkey", 1, valkey_params, 1, "Execute a command.", 0, valkeyCommandHandler, lua),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND_WITH_CTX("redis", 1, valkey_params, 1, NULL, 1, valkeyCommandHandler, lua),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND_WITH_CTX(SERVER_API_NAME, 0, valkey_params, 1, NULL, 1, valkeyCommandHandler, lua),
            VALKEYMODULE_SCRIPTING_ENGINE_DEBUGGER_COMMAND_WITH_CTX("abort", 1, NULL, 0, "Stop the execution of the script. In sync mode dataset changes will be retained.", 0, abortCommandHandler, lua),
        };

        commands_array_len = sizeof(commands_array) / sizeof(ValkeyModuleScriptingEngineDebuggerCommand);

        commands_array_cache = ValkeyModule_Calloc(commands_array_len, sizeof(ValkeyModuleScriptingEngineDebuggerCommand));
        memcpy(commands_array_cache, &commands_array, sizeof(commands_array));
    }

    *commands = commands_array_cache;
    *commands_len = commands_array_len;
}

/* Read debugging commands from client.
 * Return C_OK if the debugging session is continuing, otherwise
 * C_ERR if the client closed the connection or is timing out. */
int ldbRepl(lua_State *lua) {
    int client_disconnected = 0;
    ValkeyModuleString *err = NULL;

    ValkeyModule_ScriptingEngineDebuggerProcessCommands(&client_disconnected, &err);

    if (err) {
        const char *err_msg = ValkeyModule_StringPtrLen(err, NULL);
        luaPushError(lua, err_msg);
        ValkeyModule_Free(err);
        luaError(lua);
    } else if (client_disconnected) {
        /* Make sure the script runs without user input since the
         * client is no longer connected. */
        ldb.step = 0;
        ldb.bpcount = 0;
        return C_ERR;
    }

    return C_OK;
}

int ldbIsActive(void) {
    return ldb.active;
}

int ldbGetCurrentLine(void) {
    return ldb.currentline;
}

void ldbSetCurrentLine(int line) {
    ldb.currentline = line;
}

void ldbSetBreakpointOnNextLine(int enable) {
    ldb.luabp = enable;
}

int ldbIsBreakpointOnNextLineEnabled(void) {
    return ldb.luabp;
}

int ldbShouldBreak(void) {
    return ldbIsBreakpoint(ldb.currentline) || ldb.luabp;
}

int ldbIsStepEnabled(void) {
    return ldb.step;
}

void ldbSetStepMode(int enable) {
    ldb.step = enable;
}
