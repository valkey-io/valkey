/* Test module for command result callbacks API
 *
 * This module tests the ValkeyModule_RegisterCommandResult() API and related
 * functionality for tracking command execution results.
 *
 * Commands provided:
 * - CMDRESULT.REGISTER <flags> - Register a result callback with specified flags
 * - CMDRESULT.UNREGISTER - Unregister the callback
 * - CMDRESULT.STATS - Get statistics about callback invocations
 * - CMDRESULT.RESET - Reset statistics
 * - CMDRESULT.GETLOG [count] - Get the last N logged command results
 * - CMDRESULT.SUCCESS - A command that always succeeds
 * - CMDRESULT.FAIL - A command that always fails
 */

#include "valkeymodule.h"
#include <string.h>
#include <stdlib.h>

/* Statistics tracking */
static struct {
    long long total_callbacks;
    long long success_count;
    long long failure_count;
    long long total_duration_us;
    long long total_dirty;
} stats = {0};

/* Command result log entry */
#define MAX_LOG_ENTRIES 100
typedef struct {
    char command_name[64];
    int status;
    long long duration;
    long long dirty;
    unsigned long long client_id;
} ResultLogEntry;

static ResultLogEntry result_log[MAX_LOG_ENTRIES];
static int log_head = 0;
static int log_count = 0;

/* Registered callback handle */
static ValkeyModuleCommandResult *registered_callback = NULL;

/* Add entry to circular log */
void LogResult(const char *cmd_name, int status, long long duration,
               long long dirty, unsigned long long client_id) {
    ResultLogEntry *entry = &result_log[log_head];

    strncpy(entry->command_name, cmd_name, sizeof(entry->command_name) - 1);
    entry->command_name[sizeof(entry->command_name) - 1] = '\0';
    entry->status = status;
    entry->duration = duration;
    entry->dirty = dirty;
    entry->client_id = client_id;

    log_head = (log_head + 1) % MAX_LOG_ENTRIES;
    if (log_count < MAX_LOG_ENTRIES) log_count++;
}

/* Command result callback function */
void CommandResultCallback(ValkeyModuleCommandResultCtx *ctx) {
    stats.total_callbacks++;

    int status = ValkeyModule_CommandResultStatus(ctx);
    const char *cmd_name = ValkeyModule_CommandResultCommandName(ctx);
    long long duration = ValkeyModule_CommandResultDuration(ctx);
    long long dirty = ValkeyModule_CommandResultDirty(ctx);
    unsigned long long client_id = ValkeyModule_CommandResultClientId(ctx);

    if (status == VALKEYMODULE_CMDRESULT_SUCCESS) {
        stats.success_count++;
    } else {
        stats.failure_count++;
    }

    stats.total_duration_us += duration;
    stats.total_dirty += dirty;

    /* Log the result */
    LogResult(cmd_name ? cmd_name : "unknown", status, duration, dirty, client_id);
}

/* CMDRESULT.REGISTER <flags>
 * Flags can be: "all", "failures", "noself", "failures+noself"
 */
int CmdResultRegister_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        return ValkeyModule_WrongArity(ctx);
    }

    if (registered_callback) {
        return ValkeyModule_ReplyWithError(ctx, "ERR callback already registered");
    }

    size_t len;
    const char *flags_str = ValkeyModule_StringPtrLen(argv[1], &len);

    int flags = 0;
    if (strcmp(flags_str, "failures") == 0) {
        flags = VALKEYMODULE_CMDRESULT_FAILURES_ONLY;
    } else if (strcmp(flags_str, "noself") == 0) {
        flags = VALKEYMODULE_CMDRESULT_NOSELF;
    } else if (strcmp(flags_str, "failures+noself") == 0) {
        flags = VALKEYMODULE_CMDRESULT_FAILURES_ONLY | VALKEYMODULE_CMDRESULT_NOSELF;
    } else if (strcmp(flags_str, "all") != 0) {
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid flags. Use: all, failures, noself, or failures+noself");
    }

    registered_callback = ValkeyModule_RegisterCommandResult(ctx, CommandResultCallback, flags);

    if (!registered_callback) {
        return ValkeyModule_ReplyWithError(ctx, "ERR failed to register callback");
    }

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* CMDRESULT.UNREGISTER */
int CmdResultUnregister_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);

    if (argc != 1) {
        return ValkeyModule_WrongArity(ctx);
    }

    if (!registered_callback) {
        return ValkeyModule_ReplyWithError(ctx, "ERR no callback registered");
    }

    int result = ValkeyModule_UnregisterCommandResult(ctx, registered_callback);
    registered_callback = NULL;

    if (result != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx, "ERR failed to unregister callback");
    }

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* CMDRESULT.STATS
 * Returns: total_callbacks, success_count, failure_count, total_duration_us, total_dirty
 */
int CmdResultStats_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);

    if (argc != 1) {
        return ValkeyModule_WrongArity(ctx);
    }

    ValkeyModule_ReplyWithArray(ctx, 10);
    ValkeyModule_ReplyWithSimpleString(ctx, "total_callbacks");
    ValkeyModule_ReplyWithLongLong(ctx, stats.total_callbacks);
    ValkeyModule_ReplyWithSimpleString(ctx, "success_count");
    ValkeyModule_ReplyWithLongLong(ctx, stats.success_count);
    ValkeyModule_ReplyWithSimpleString(ctx, "failure_count");
    ValkeyModule_ReplyWithLongLong(ctx, stats.failure_count);
    ValkeyModule_ReplyWithSimpleString(ctx, "total_duration_us");
    ValkeyModule_ReplyWithLongLong(ctx, stats.total_duration_us);
    ValkeyModule_ReplyWithSimpleString(ctx, "total_dirty");
    ValkeyModule_ReplyWithLongLong(ctx, stats.total_dirty);

    return VALKEYMODULE_OK;
}

/* CMDRESULT.RESET */
int CmdResultReset_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);

    if (argc != 1) {
        return ValkeyModule_WrongArity(ctx);
    }

    stats.total_callbacks = 0;
    stats.success_count = 0;
    stats.failure_count = 0;
    stats.total_duration_us = 0;
    stats.total_dirty = 0;

    log_head = 0;
    log_count = 0;

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* CMDRESULT.GETLOG [count]
 * Returns the last N command results from the log
 */
int CmdResultGetLog_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc > 2) {
        return ValkeyModule_WrongArity(ctx);
    }

    long long count = log_count;
    if (argc == 2) {
        if (ValkeyModule_StringToLongLong(argv[1], &count) != VALKEYMODULE_OK) {
            return ValkeyModule_ReplyWithError(ctx, "ERR invalid count");
        }
        if (count < 0) count = 0;
        if (count > log_count) count = log_count;
    }

    ValkeyModule_ReplyWithArray(ctx, count);

    /* Get entries from newest to oldest */
    for (int i = 0; i < count; i++) {
        int idx = (log_head - 1 - i + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
        ResultLogEntry *entry = &result_log[idx];

        ValkeyModule_ReplyWithArray(ctx, 10);
        ValkeyModule_ReplyWithSimpleString(ctx, "command");
        ValkeyModule_ReplyWithCString(ctx, entry->command_name);
        ValkeyModule_ReplyWithSimpleString(ctx, "status");
        ValkeyModule_ReplyWithCString(ctx,
            entry->status == VALKEYMODULE_CMDRESULT_SUCCESS ? "success" : "failure");
        ValkeyModule_ReplyWithSimpleString(ctx, "duration_us");
        ValkeyModule_ReplyWithLongLong(ctx, entry->duration);
        ValkeyModule_ReplyWithSimpleString(ctx, "dirty");
        ValkeyModule_ReplyWithLongLong(ctx, entry->dirty);
        ValkeyModule_ReplyWithSimpleString(ctx, "client_id");
        ValkeyModule_ReplyWithLongLong(ctx, entry->client_id);
    }

    return VALKEYMODULE_OK;
}

/* CMDRESULT.SUCCESS
 * A command that always succeeds
 */
int CmdResultSuccess_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* CMDRESULT.FAIL
 * A command that always fails
 */
int CmdResultFail_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    return ValkeyModule_ReplyWithError(ctx, "ERR intentional failure");
}

/* CMDRESULT.DIRTY <key>
 * A command that modifies a key (increments dirty count)
 */
int CmdResultDirty_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        return ValkeyModule_WrongArity(ctx);
    }

    /* Use RM_Call to execute SET, which will properly increment dirty count */
    ValkeyModuleString *value = ValkeyModule_CreateString(ctx, "modified", 8);
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "SET", "ss", argv[1], value);

    if (!reply) {
        ValkeyModule_FreeString(ctx, value);
        return ValkeyModule_ReplyWithError(ctx, "ERR failed to set key");
    }

    ValkeyModule_FreeCallReply(reply);
    ValkeyModule_FreeString(ctx, value);
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* CMDRESULT.RMCALL <command> [args...]
 * Test that NOSELF flag works - this calls a command via RM_Call
 */
int CmdResultRMCall_RedisCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 2) {
        return ValkeyModule_WrongArity(ctx);
    }

    /* Call the command via RM_Call */
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx,
        ValkeyModule_StringPtrLen(argv[1], NULL), "v", argv + 2, argc - 2);

    if (!reply) {
        return ValkeyModule_ReplyWithError(ctx, "ERR call failed");
    }

    /* Forward the reply */
    ValkeyModule_ReplyWithCallReply(ctx, reply);
    ValkeyModule_FreeCallReply(reply);

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "commandresult", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.register", CmdResultRegister_RedisCommand,
                                   "admin", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.unregister", CmdResultUnregister_RedisCommand,
                                   "admin", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.stats", CmdResultStats_RedisCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.reset", CmdResultReset_RedisCommand,
                                   "admin", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.getlog", CmdResultGetLog_RedisCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.success", CmdResultSuccess_RedisCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.fail", CmdResultFail_RedisCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.dirty", CmdResultDirty_RedisCommand,
                                   "write", 1, 1, 1) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.rmcall", CmdResultRMCall_RedisCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    return VALKEYMODULE_OK;
}
