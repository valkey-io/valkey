/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Commandlog implements a system that is able to remember the latest N
 * queries that took more than M microseconds to execute, or consumed
 * too much network bandwidth and memory for input/output buffers.
 *
 * The execution time to reach to be logged in the slow log is set
 * using the 'commandlog-execution-slower-than' config directive, that is also
 * readable and writable using the CONFIG SET/GET command.
 *
 * Other configurations such as `commandlog-request-larger-than` and
 * `commandlog-reply-larger-than` can be found with more detailed
 * explanations in the config file.
 *
 * The command log is actually not "logged" in the server log file
 * but is accessible thanks to the COMMANDLOG command.
 *
 * ----------------------------------------------------------------------------
 */

#include "commandlog.h"
#include "script.h"

/* Create a new commandlog entry.
 * Incrementing the ref count of all the objects retained is up to
 * this function. */
static commandlogEntry *commandlogCreateEntry(client *c, robj **argv, int argc, long long value, int type) {
    commandlogEntry *ce = zmalloc(sizeof(*ce));
    int j, ceargc = argc;

    if (ceargc > COMMANDLOG_ENTRY_MAX_ARGC) ceargc = COMMANDLOG_ENTRY_MAX_ARGC;
    ce->argc = ceargc;
    ce->argv = zmalloc(sizeof(robj *) * ceargc);
    for (j = 0; j < ceargc; j++) {
        /* Logging too many arguments is a useless memory waste, so we stop
         * at COMMANDLOG_ENTRY_MAX_ARGC, but use the last argument to specify
         * how many remaining arguments there were in the original command. */
        if (ceargc != argc && j == ceargc - 1) {
            ce->argv[j] =
                createObject(OBJ_STRING, sdscatprintf(sdsempty(), "... (%d more arguments)", argc - ceargc + 1));
        } else {
            if (clientCommandArgShouldBeRedacted(c, j)) {
                ce->argv[j] = shared.redacted;
                /* Trim too long strings as well... */
            } else if (argv[j]->type == OBJ_STRING && sdsEncodedObject(argv[j]) &&
                       sdslen(objectGetVal(argv[j])) > COMMANDLOG_ENTRY_MAX_STRING) {
                sds s = sdsnewlen(objectGetVal(argv[j]), COMMANDLOG_ENTRY_MAX_STRING);

                s = sdscatprintf(s, "... (%lu more bytes)",
                                 (unsigned long)sdslen(objectGetVal(argv[j])) - COMMANDLOG_ENTRY_MAX_STRING);
                ce->argv[j] = createObject(OBJ_STRING, s);
            } else if (argv[j]->refcount == OBJ_SHARED_REFCOUNT) {
                ce->argv[j] = argv[j];
            } else {
                /* Here we need to duplicate the string objects composing the
                 * argument vector of the command, because those may otherwise
                 * end shared with string objects stored into keys. Having
                 * shared objects between any part of the server, and the data
                 * structure holding the data, is a problem: FLUSHALL ASYNC
                 * may release the shared string object and create a race. */
                ce->argv[j] = dupStringObject(argv[j]);
            }
        }
    }
    ce->time = time(NULL);
    ce->value = value;
    ce->id = server.commandlog[type].entry_id++;
    ce->peerid = sdsnew(getClientPeerId(c));
    ce->cname = c->name ? sdsnew(objectGetVal(c->name)) : sdsempty();
    return ce;
}

/* Free a command log entry. The argument is void so that the prototype of this
 * function matches the one of the 'free' method of adlist.c.
 *
 * This function will take care to release all the retained object. */
static void commandlogFreeEntry(void *ceptr) {
    commandlogEntry *ce = ceptr;
    int j;

    for (j = 0; j < ce->argc; j++) decrRefCount(ce->argv[j]);
    zfree(ce->argv);
    sdsfree(ce->peerid);
    sdsfree(ce->cname);
    zfree(ce);
}

/* Initialize the command log. This function should be called a single time
 * at server startup. */
void commandlogInit(void) {
    for (int i = 0; i < COMMANDLOG_TYPE_NUM; i++) {
        server.commandlog[i].entries = listCreate();
        server.commandlog[i].entry_id = 0;
        listSetFreeMethod(server.commandlog[i].entries, commandlogFreeEntry);
    }
}

/* Push a new entry into the command log.
 * This function will make sure to trim the command log accordingly to the
 * configured max length. */
static void commandlogPushEntryIfNeeded(client *c, robj **argv, int argc, long long value, int type) {
    if (server.commandlog[type].threshold < 0 || server.commandlog[type].max_len == 0) return; /* The corresponding commandlog disabled */
    if (value >= server.commandlog[type].threshold)
        listAddNodeHead(server.commandlog[type].entries, commandlogCreateEntry(c, argv, argc, value, type));

    /* Remove old entries if needed. */
    while (listLength(server.commandlog[type].entries) > server.commandlog[type].max_len) listDelNode(server.commandlog[type].entries, listLast(server.commandlog[type].entries));
}

/* Remove all the entries from the current command log of the specified type. */
static void commandlogReset(int type) {
    while (listLength(server.commandlog[type].entries) > 0) listDelNode(server.commandlog[type].entries, listLast(server.commandlog[type].entries));
}

/* Reply command logs to client. */
static void commandlogGetReply(client *c, int type, long count) {
    listIter li;
    listNode *ln;
    commandlogEntry *ce;

    if (count > (long)listLength(server.commandlog[type].entries)) {
        count = listLength(server.commandlog[type].entries);
    }
    addReplyArrayLen(c, count);
    listRewind(server.commandlog[type].entries, &li);
    while (count--) {
        int j;

        ln = listNext(&li);
        ce = ln->value;
        addReplyArrayLen(c, 6);
        addReplyLongLong(c, ce->id);
        addReplyLongLong(c, ce->time);
        addReplyLongLong(c, ce->value);
        addReplyArrayLen(c, ce->argc);
        for (j = 0; j < ce->argc; j++) addReplyBulk(c, ce->argv[j]);
        addReplyBulkCBuffer(c, ce->peerid, sdslen(ce->peerid));
        addReplyBulkCBuffer(c, ce->cname, sdslen(ce->cname));
    }
}

/* Log the last command a client executed into the commandlog. */
void commandlogPushCurrentCommand(client *c, struct serverCommand *cmd) {
    /* Some commands may contain sensitive data that should not be available in the commandlog.
     */
    if (cmd->flags & CMD_SKIP_COMMANDLOG) return;

    /* If command argument vector was rewritten, use the original
     * arguments. */
    robj **argv = c->original_argv ? c->original_argv : c->argv;
    int argc = c->original_argv ? c->original_argc : c->argc;

    /* In script, client will be replaced with its caller, so commandlog needs to use the metrics
     * of the client that currently executing the command. */
    long duration = c->duration;
    unsigned long long net_input_bytes_curr_cmd = c->net_input_bytes_curr_cmd;
    unsigned long long net_output_bytes_curr_cmd = c->net_output_bytes_curr_cmd;

    /* If a script is currently running, the client passed in is a
     * fake client. Or the client passed in is the original client
     * if this is a EVAL or alike, doesn't matter. In this case,
     * use the original client to get the client information. */
    c = scriptIsRunning() ? scriptGetCaller() : c;

    commandlogPushEntryIfNeeded(c, argv, argc, duration, COMMANDLOG_TYPE_SLOW);
    commandlogPushEntryIfNeeded(c, argv, argc, net_input_bytes_curr_cmd, COMMANDLOG_TYPE_LARGE_REQUEST);

    /* Deferred large-reply check for copy-avoidance replies.
     *
     * Problem: with copy avoidance, the reply buffer holds a reference to the
     * value object (BULK_STR_REF) rather than a copy. The IO thread resolves
     * the actual byte count when writing to the socket (trackBufReferences).
     * At this point in command processing, argv is available but reply size is
     * not. By the time postWriteToClient fires (reply size known), argv has
     * been freed by resetClient/freeClientArgv.
     *
     * Solution: stash argv refs for the FIRST command in a pipeline batch and
     * defer its check to postWriteToClient. For subsequent pipelined commands,
     * fall back to immediate sdslen (which is cheap — values are cache-hot from
     * sequential processing). This avoids the cold cache miss that only affects
     * the first GET in isolation, while preserving per-command commandlog entries
     * for pipelined workloads.
     *
     * For large-argc commands (MGET, etc.), skip the deferred path entirely.
     *
     * Cost: 2 refcount increments + 2 pointer stores for the first command only
     * (zero allocation, inline array). */
    if (c->flag.buf_encoded && server.commandlog[COMMANDLOG_TYPE_LARGE_REPLY].threshold >= 0 &&
        argc <= CMDLOG_INLINE_ARGV_MAX) {
        if (c->cmdlog_argc > 0) {
            /* Pipelining: a previous command's stash hasn't been consumed yet.
             * Log it now with whatever bytes have been tracked so far (may be 0
             * if the IO thread hasn't run yet — accepted as under-report for the
             * first command). Then do the immediate check for this command since
             * addReplyBulk computed net_output_bytes_curr_cmd for it (values are
             * cache-hot from sequential pipeline processing). */
            size_t io_bytes = atomic_load_explicit(&c->io_reply_len_cmdlog, memory_order_relaxed);
            commandlogPushEntryIfNeeded(c, c->cmdlog_argv, c->cmdlog_argc,
                                        (long long)io_bytes, COMMANDLOG_TYPE_LARGE_REPLY);
            for (int j = 0; j < c->cmdlog_argc; j++)
                decrRefCount(c->cmdlog_argv[j]);
            c->cmdlog_argc = 0;
            /* Check this command immediately — net_output_bytes_curr_cmd was
             * populated by addReplyBulk since it detected pipelining (cmdlog_argc > 0). */
            commandlogPushEntryIfNeeded(c, argv, argc, net_output_bytes_curr_cmd, COMMANDLOG_TYPE_LARGE_REPLY);
        } else {
            /* First (or only) command — stash argv for deferred check. */
            c->cmdlog_argv = c->cmdlog_argv_inline;
            for (int j = 0; j < argc; j++) {
                c->cmdlog_argv[j] = argv[j];
                incrRefCount(argv[j]);
            }
            c->cmdlog_argc = argc;
        }
    } else {
        /* For commands with argc > CMDLOG_INLINE_ARGV_MAX (e.g., MGET with many keys),
         * we skip the deferred stash to avoid per-command zmalloc overhead. Use the
         * immediately-known net_output_bytes_curr_cmd for the check. For copy-avoidance
         * replies this may undercount (BULK_STR_REF bytes tracked by IO thread aren't
         * included yet), but multi-key commands with all values exceeding the large-reply
         * threshold are rare in practice, and the values are cache-hot anyway. */
        commandlogPushEntryIfNeeded(c, argv, argc, net_output_bytes_curr_cmd, COMMANDLOG_TYPE_LARGE_REPLY);
    }
}

/* Check deferred large-reply commandlog entry after IO thread has written the
 * copy-avoidance reply. Called from postWriteToClient() on the main thread once
 * all pending replies are flushed. Uses the stashed argv refs from
 * commandlogPushCurrentCommand() for full command attribution. */
void commandlogCheckDeferredLargeReply(client *c) {
    if (c->cmdlog_argc == 0) return;

    size_t io_bytes = atomic_load_explicit(&c->io_reply_len_cmdlog, memory_order_relaxed);
    commandlogPushEntryIfNeeded(c, c->cmdlog_argv, c->cmdlog_argc,
                                (long long)io_bytes, COMMANDLOG_TYPE_LARGE_REPLY);
    for (int j = 0; j < c->cmdlog_argc; j++)
        decrRefCount(c->cmdlog_argv[j]);
    c->cmdlog_argv = NULL;
    c->cmdlog_argc = 0;
}

/* The SLOWLOG command. Implements all the subcommands needed to handle the
 * slow log. */
void slowlogCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(objectGetVal(c->argv[1]), "help")) {
        const char *help[] = {
            "GET [<count>]",
            "    Return top <count> entries from the slowlog (default: 10, -1 mean all).",
            "    Entries are made of:",
            "    id, timestamp, time in microseconds, arguments array, client IP and port,",
            "    client name",
            "LEN",
            "    Return the length of the slowlog.",
            "RESET",
            "    Reset the slowlog.",
            NULL,
        };
        addReplyHelp(c, help);
    } else if (c->argc == 2 && !strcasecmp(objectGetVal(c->argv[1]), "reset")) {
        commandlogReset(COMMANDLOG_TYPE_SLOW);
        addReply(c, shared.ok);
    } else if (c->argc == 2 && !strcasecmp(objectGetVal(c->argv[1]), "len")) {
        addReplyLongLong(c, listLength(server.commandlog[COMMANDLOG_TYPE_SLOW].entries));
    } else if ((c->argc == 2 || c->argc == 3) && !strcasecmp(objectGetVal(c->argv[1]), "get")) {
        long count = 10;

        if (c->argc == 3) {
            /* Consume count arg. */
            if (getRangeLongFromObjectOrReply(c, c->argv[2], -1, LONG_MAX, &count,
                                              "count should be greater than or equal to -1") != C_OK)
                return;

            if (count == -1) {
                /* We treat -1 as a special value, which means to get all slow logs.
                 * Simply set count to the length of server.commandlog. */
                count = listLength(server.commandlog[COMMANDLOG_TYPE_SLOW].entries);
            }
        }

        commandlogGetReply(c, COMMANDLOG_TYPE_SLOW, count);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

static int commandlogGetTypeOrReply(client *c, robj *o) {
    if (!strcasecmp(objectGetVal(o), "slow")) return COMMANDLOG_TYPE_SLOW;
    if (!strcasecmp(objectGetVal(o), "large-request")) return COMMANDLOG_TYPE_LARGE_REQUEST;
    if (!strcasecmp(objectGetVal(o), "large-reply")) return COMMANDLOG_TYPE_LARGE_REPLY;
    addReplyError(c, "type should be one of the following: slow, large-request, large-reply");
    return -1;
}

/* The COMMANDLOG command. Implements all the subcommands needed to handle the
 * command log. */
void commandlogCommand(client *c) {
    int type;
    if (c->argc == 2 && !strcasecmp(objectGetVal(c->argv[1]), "help")) {
        const char *help[] = {
            "GET <count> <type>",
            "    Return top <count> entries of the specified <type> from the commandlog (-1 mean all).",
            "    Entries are made of:",
            "    id, timestamp,",
            "        time in microseconds for type of slow,",
            "        or size in bytes for type of large-request,",
            "        or size in bytes for type of large-reply",
            "    arguments array, client IP and port,",
            "    client name",
            "LEN <type>",
            "    Return the length of the specified type of commandlog.",
            "RESET <type>",
            "    Reset the specified type of commandlog.",
            NULL,
        };
        addReplyHelp(c, help);
    } else if (c->argc == 3 && !strcasecmp(objectGetVal(c->argv[1]), "reset")) {
        if ((type = commandlogGetTypeOrReply(c, c->argv[2])) == -1) return;
        commandlogReset(type);
        addReply(c, shared.ok);
    } else if (c->argc == 3 && !strcasecmp(objectGetVal(c->argv[1]), "len")) {
        if ((type = commandlogGetTypeOrReply(c, c->argv[2])) == -1) return;
        addReplyLongLong(c, listLength(server.commandlog[type].entries));
    } else if (c->argc == 4 && !strcasecmp(objectGetVal(c->argv[1]), "get")) {
        long count;

        /* Consume count arg. */
        if (getRangeLongFromObjectOrReply(c, c->argv[2], -1, LONG_MAX, &count,
                                          "count should be greater than or equal to -1") != C_OK)
            return;

        if ((type = commandlogGetTypeOrReply(c, c->argv[3])) == -1) return;

        if (count == -1) {
            /* We treat -1 as a special value, which means to get all command logs.
             * Simply set count to the length of server.commandlog. */
            count = listLength(server.commandlog[type].entries);
        }

        commandlogGetReply(c, type, count);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
