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
#define MAX_ARGV_LOG 10
#define MAX_ARG_LEN 128
#define MAX_REPLY_LEN 256

typedef struct {
    char command_name[64];
    int status;
    long long duration;
    long long dirty;
    unsigned long long client_id;
    /* New fields for argv and reply */
    int argc;
    char argv[MAX_ARGV_LOG][MAX_ARG_LEN];
    size_t reply_size;
    char reply_proto[MAX_REPLY_LEN];
    size_t reply_proto_len;
} ResultLogEntry;

static ResultLogEntry result_log[MAX_LOG_ENTRIES];
static int log_head = 0;
static int log_count = 0;

/* Registered callback handle */
static ValkeyModuleCommandResult *registered_callback = NULL;

/* For testing VM_CommandResultCreateReply */
static int test_create_reply_enabled = 0;
static char last_reply_type[32] = "";
static char last_reply_string[MAX_REPLY_LEN] = "";
static long long last_reply_integer = 0;

/* Add entry to circular log */
void LogResult(const char *cmd_name, int status, long long duration,
               long long dirty, unsigned long long client_id,
               ValkeyModuleString **argv, int argc,
               size_t reply_size, const char *reply_proto, size_t reply_proto_len) {
    ResultLogEntry *entry = &result_log[log_head];

    strncpy(entry->command_name, cmd_name, sizeof(entry->command_name) - 1);
    entry->command_name[sizeof(entry->command_name) - 1] = '\0';
    entry->status = status;
    entry->duration = duration;
    entry->dirty = dirty;
    entry->client_id = client_id;

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

    /* Store reply info */
    entry->reply_size = reply_size;
    if (reply_proto && reply_proto_len > 0) {
        size_t copy_len = (reply_proto_len < MAX_REPLY_LEN - 1) ? reply_proto_len : MAX_REPLY_LEN - 1;
        memcpy(entry->reply_proto, reply_proto, copy_len);
        entry->reply_proto[copy_len] = '\0';
        entry->reply_proto_len = copy_len;
    } else {
        entry->reply_proto[0] = '\0';
        entry->reply_proto_len = 0;
    }

    log_head = (log_head + 1) % MAX_LOG_ENTRIES;
    if (log_count < MAX_LOG_ENTRIES) log_count++;
}

/* Command result callback function */
void CommandResultCallback(ValkeyModuleCommandResultCtx *ctx) {
    stats.total_callbacks++;

    int status = ValkeyModule_CommandResultGetStatus(ctx);
    const char *cmd_name = ValkeyModule_CommandResultGetCommandName(ctx);
    long long duration = ValkeyModule_CommandResultGetDuration(ctx);
    long long dirty = ValkeyModule_CommandResultGetDirty(ctx);
    unsigned long long client_id = ValkeyModule_CommandResultGetClientId(ctx);

    /* Get command arguments using new accessor */
    ValkeyModuleString **argv = NULL;
    int argc = 0;
    int argv_result = ValkeyModule_CommandResultGetArgv(ctx, &argv, &argc);

    /* Get reply info using new accessors */
    size_t reply_size = ValkeyModule_CommandResultGetReplySize(ctx);
    size_t reply_proto_len = 0;
    const char *reply_proto = ValkeyModule_CommandResultGetReplyProto(ctx, &reply_proto_len);

    if (status == VALKEYMODULE_CMDRESULT_SUCCESS) {
        stats.success_count++;
    } else {
        stats.failure_count++;
    }

    stats.total_duration_us += duration;
    stats.total_dirty += dirty;

    /* Optionally test VM_CommandResultCreateReply */
    if (test_create_reply_enabled && reply_size > 0) {
        /* Skip large replies to avoid issues */
        ValkeyModuleCallReply *reply = ValkeyModule_CommandResultCreateReply(ctx);
        if (reply) {
            int type = ValkeyModule_CallReplyType(reply);
            switch (type) {
                case VALKEYMODULE_REPLY_STRING:
                case VALKEYMODULE_REPLY_SIMPLE_STRING:
                    strcpy(last_reply_type, type == VALKEYMODULE_REPLY_STRING ? "string" : "simple_string");
                    {
                        size_t len;
                        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
                        if (str && len < MAX_REPLY_LEN) {
                            memcpy(last_reply_string, str, len);
                            last_reply_string[len] = '\0';
                        }
                    }
                    break;
                case VALKEYMODULE_REPLY_INTEGER:
                    strcpy(last_reply_type, "integer");
                    /* Get the integer value */
                    last_reply_integer = ValkeyModule_CallReplyInteger(reply);
                    /* Clear the string */
                    last_reply_string[0] = '\0';
                    break;
                case VALKEYMODULE_REPLY_ARRAY:
                    strcpy(last_reply_type, "array");
                    break;
                case VALKEYMODULE_REPLY_ERROR:
                    strcpy(last_reply_type, "error");
                    {
                        size_t len;
                        const char *str = ValkeyModule_CallReplyStringPtr(reply, &len);
                        if (str && len < MAX_REPLY_LEN) {
                            memcpy(last_reply_string, str, len);
                            last_reply_string[len] = '\0';
                        }
                    }
                    break;
                case VALKEYMODULE_REPLY_NULL:
                    strcpy(last_reply_type, "null");
                    break;
                default:
                    strcpy(last_reply_type, "other");
                    break;
            }
            ValkeyModule_FreeCallReply(reply);
        } else {
            strcpy(last_reply_type, "none");
        }
    }

    /* Log the result - skip if we couldn't get argv */
    if (argv_result == VALKEYMODULE_OK) {
        LogResult(cmd_name ? cmd_name : "unknown", status, duration, dirty, client_id,
                  argv, argc, reply_size, reply_proto, reply_proto_len);
    } else {
        LogResult(cmd_name ? cmd_name : "unknown", status, duration, dirty, client_id,
                  NULL, 0, reply_size, reply_proto, reply_proto_len);
    }
}

/* CMDRESULT.REGISTER <flags>
 * Flags can be: "all", "failures", "noself", "failures+noself"
 */
int CmdResultRegister_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
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
int CmdResultUnregister_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
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

        ValkeyModule_ReplyWithArray(ctx, 16);
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
        /* New fields for argv and reply */
        ValkeyModule_ReplyWithSimpleString(ctx, "argv");
        ValkeyModule_ReplyWithArray(ctx, entry->argc);
        for (int j = 0; j < entry->argc; j++) {
            ValkeyModule_ReplyWithCString(ctx, entry->argv[j]);
        }
        ValkeyModule_ReplyWithSimpleString(ctx, "reply_size");
        ValkeyModule_ReplyWithLongLong(ctx, entry->reply_size);
        ValkeyModule_ReplyWithSimpleString(ctx, "reply_proto");
        ValkeyModule_ReplyWithStringBuffer(ctx, entry->reply_proto, entry->reply_proto_len);
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
 * Test that NOSELF flag works - this calls a command via RM_Call
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

/* CMDRESULT.TESTREPLY <on|off>
 * Enable or disable testing of VM_CommandResultCreateReply in the callback
 */
int CmdResultTestReply_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        return ValkeyModule_WrongArity(ctx);
    }

    size_t len;
    const char *arg = ValkeyModule_StringPtrLen(argv[1], &len);

    if (strcmp(arg, "on") == 0) {
        test_create_reply_enabled = 1;
    } else if (strcmp(arg, "off") == 0) {
        test_create_reply_enabled = 0;
    } else {
        return ValkeyModule_ReplyWithError(ctx, "ERR argument must be 'on' or 'off'");
    }

    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* CMDRESULT.GETLASTREPLY
 * Get the last parsed reply from VM_CommandResultCreateReply
 */
int CmdResultGetLastReply_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);

    if (argc != 1) {
        return ValkeyModule_WrongArity(ctx);
    }

    ValkeyModule_ReplyWithArray(ctx, 6);
    ValkeyModule_ReplyWithSimpleString(ctx, "type");
    ValkeyModule_ReplyWithCString(ctx, last_reply_type);
    ValkeyModule_ReplyWithSimpleString(ctx, "string");
    ValkeyModule_ReplyWithCString(ctx, last_reply_string);
    ValkeyModule_ReplyWithSimpleString(ctx, "integer");
    ValkeyModule_ReplyWithLongLong(ctx, last_reply_integer);

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

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.unregister", CmdResultUnregister_ValkeyCommand,
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

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.testreply", CmdResultTestReply_ValkeyCommand,
                                   "admin", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "cmdresult.getlastreply", CmdResultGetLastReply_ValkeyCommand,
                                   "readonly", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }

    return VALKEYMODULE_OK;
}
