/* Test module for command result event API
 *
 * This module tests the VALKEYMODULE_EVENT_COMMAND_RESULT server event
 * using ValkeyModule_SubscribeToServerEvent().
 *
 * Commands provided:
 * - CMDRESULT.REGISTER <mode> - Register event subscription (success/failure/all)
 * - CMDRESULT.UNSUBSCRIBE - Unsubscribe from the event
 * - CMDRESULT.STATS - Get statistics about event invocations
 * - CMDRESULT.RESET - Reset statistics
 * - CMDRESULT.GETLOG [count] - Get the last N logged command results
 * - CMDRESULT.SUCCESS - A command that always succeeds
 * - CMDRESULT.FAIL - A command that always fails
 * - CMDRESULT.RMCALL <command> [args...] - Call a command via RM_Call
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
#define MAX_ARGV_LOG 10
#define MAX_ARG_LEN 128

typedef struct {
    char command_name[64];
    int status;  /* 0 = success, 1 = failure */
    long long duration;
    long long dirty;
    unsigned long long client_id;
    int is_module_client;
    int argc;
    char argv[MAX_ARGV_LOG][MAX_ARG_LEN];
} ResultLogEntry;

static ResultLogEntry result_log[MAX_LOG_ENTRIES];
static int log_head = 0;
static int log_count = 0;

/* Track subscription mode: 0 = unsubscribed, 1 = success only, 2 = failure only, 3 = all */
static int subscription_mode = 0;

/* Add entry to circular log */
void LogResult(const char *cmd_name, int status, long long duration,
               long long dirty, unsigned long long client_id, int is_module_client,
               ValkeyModuleString **argv, int argc) {
    ResultLogEntry *entry = &result_log[log_head];

    strncpy(entry->command_name, cmd_name, sizeof(entry->command_name) - 1);
    entry->command_name[sizeof(entry->command_name) - 1] = '\0';
    entry->status = status;
    entry->duration = duration;
    entry->dirty = dirty;
    entry->client_id = client_id;
    entry->is_module_client = is_module_client;

    /* Store argv */
    if (argv && argc > 0) {
        entry->argc = (argc < MAX_ARGV_LOG) ? argc : MAX_ARGV_LOG;
        for (int i = 0; i < entry->argc; i++) {
            if (argv[i] == NULL) {
                strcpy(entry->argv[i], "(null)");
                continue;
            }
            size_t len;
            const char *arg = ValkeyModule_StringPtrLen(argv[i], &len);
            if (arg == NULL) {
                strcpy(entry->argv[i], "(empty)");
                continue;
            }
            size_t copy_len = (len < MAX_ARG_LEN - 1) ? len : MAX_ARG_LEN - 1;
            memcpy(entry->argv[i], arg, copy_len);
            entry->argv[i][copy_len] = '\0';
        }
    } else {
        entry->argc = 0;
    }

    log_head = (log_head + 1) % MAX_LOG_ENTRIES;
    if (log_count < MAX_LOG_ENTRIES) log_count++;
}

/* Command result event callback */
void CommandResultEventCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent eid,
                                 uint64_t subevent, void *data) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(eid);

    ValkeyModuleCommandResultInfo *info = (ValkeyModuleCommandResultInfo *)data;

    /* Verify version */
    if (info->version != VALKEYMODULE_COMMANDRESULTINFO_VERSION) {
        return;
    }

    stats.total_callbacks++;

    int status = (subevent == VALKEYMODULE_SUBEVENT_COMMAND_RESULT_FAILURE) ? 1 : 0;

    if (status == 0) {
        stats.success_count++;
    } else {
        stats.failure_count++;
    }

    stats.total_duration_us += info->duration_us;
    stats.total_dirty += info->dirty;

    /* Log the result using direct field access */
    LogResult(info->command_name ? info->command_name : "unknown",
              status, info->duration_us, info->dirty, info->client_id,
              info->is_module_client, info->argv, info->argc);
}

/* CMDRESULT.REGISTER <mode>
 * Mode can be: "all", "success", "failure"
 */
int CmdResultRegister_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        return ValkeyModule_WrongArity(ctx);
    }

    if (subscription_mode != 0) {
        return ValkeyModule_ReplyWithError(ctx, "ERR already subscribed to command result events");
    }

    size_t len;
    const char *mode_str = ValkeyModule_StringPtrLen(argv[1], &len);

    if (strcmp(mode_str, "all") == 0) {
        subscription_mode = 3;
    } else if (strcmp(mode_str, "success") == 0) {
        subscription_mode = 1;
    } else if (strcmp(mode_str, "failure") == 0) {
        subscription_mode = 2;
    } else {
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid mode. Use: all, success, or failure");
    }

    /* Subscribe to the command result event */
    if (ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_CommandResult,
                                            CommandResultEventCallback) == VALKEYMODULE_ERR) {
        subscription_mode = 0;
        return ValkeyModule_ReplyWithError(ctx, "ERR failed to subscribe to event");
    }

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* CMDRESULT.UNSUBSCRIBE */
int CmdResultUnsubscribe_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);

    if (argc != 1) {
        return ValkeyModule_WrongArity(ctx);
    }

    if (subscription_mode == 0) {
        return ValkeyModule_ReplyWithError(ctx, "ERR not subscribed to command result events");
    }

    /* Unsubscribe by passing NULL callback */
    if (ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_CommandResult,
                                            NULL) == VALKEYMODULE_ERR) {
        return ValkeyModule_ReplyWithError(ctx, "ERR failed to unsubscribe from event");
    }

    subscription_mode = 0;
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* CMDRESULT.STATS
 * Returns: total_callbacks, success_count, failure_count, total_duration_us, total_dirty
 */
int CmdResultStats_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
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
int CmdResultReset_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
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
int CmdResultGetLog_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
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

        ValkeyModule_ReplyWithArray(ctx, 14);
        ValkeyModule_ReplyWithSimpleString(ctx, "command");
        ValkeyModule_ReplyWithCString(ctx, entry->command_name);
        ValkeyModule_ReplyWithSimpleString(ctx, "status");
        ValkeyModule_ReplyWithCString(ctx, entry->status == 0 ? "success" : "failure");
        ValkeyModule_ReplyWithSimpleString(ctx, "duration_us");
        ValkeyModule_ReplyWithLongLong(ctx, entry->duration);
        ValkeyModule_ReplyWithSimpleString(ctx, "dirty");
        ValkeyModule_ReplyWithLongLong(ctx, entry->dirty);
        ValkeyModule_ReplyWithSimpleString(ctx, "client_id");
        ValkeyModule_ReplyWithLongLong(ctx, entry->client_id);
        ValkeyModule_ReplyWithSimpleString(ctx, "is_module_client");
        ValkeyModule_ReplyWithLongLong(ctx, entry->is_module_client);
        ValkeyModule_ReplyWithSimpleString(ctx, "argv");
        ValkeyModule_ReplyWithArray(ctx, entry->argc);
        for (int j = 0; j < entry->argc; j++) {
            ValkeyModule_ReplyWithCString(ctx, entry->argv[j]);
        }
    }

    return VALKEYMODULE_OK;
}

/* CMDRESULT.SUCCESS
 * A command that always succeeds
 */
int CmdResultSuccess_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* CMDRESULT.FAIL
 * A command that always fails
 */
int CmdResultFail_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    return ValkeyModule_ReplyWithError(ctx, "ERR intentional failure");
}

/* CMDRESULT.RMCALL <command> [args...]
 * Test calling a command via RM_Call - allows testing is_module_client detection
 */
int CmdResultRMCall_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
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

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.register", CmdResultRegister_ValkeyCommand,
                                   "admin", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.unsubscribe", CmdResultUnsubscribe_ValkeyCommand,
                                   "admin", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.stats", CmdResultStats_ValkeyCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.reset", CmdResultReset_ValkeyCommand,
                                   "admin", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.getlog", CmdResultGetLog_ValkeyCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.success", CmdResultSuccess_ValkeyCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.fail", CmdResultFail_ValkeyCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.rmcall", CmdResultRMCall_ValkeyCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    return VALKEYMODULE_OK;
}
