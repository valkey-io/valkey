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

static void commandlogEnqueueDeferred(client *c, robj **argv, int argc, size_t plain_bytes);

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

    /* Large-reply check.
     *
     * With copy avoidance the reply buffer holds a reference to the value object
     * (BULK_STR_REF) instead of a copy, so the exact reply size is only known
     * once the IO thread writes it (trackBufReferences). At this point argv is
     * available but the size is not; by the time the reply is flushed, argv has
     * been freed. To avoid dereferencing the value's sds header on the main
     * thread (a guaranteed cache miss — the original regression), we stash argv
     * refs into a per-command FIFO and defer the threshold check until the reply
     * bytes have been attributed at buffer-release time.
     *
     * Non-copy-avoidance replies already have their size tracked synchronously
     * in net_output_bytes_curr_cmd, so they are checked immediately as before. */
    if (c->flag.buf_encoded && server.commandlog[COMMANDLOG_TYPE_LARGE_REPLY].threshold >= 0) {
        commandlogEnqueueDeferred(c, argv, argc, net_output_bytes_curr_cmd);
    } else {
        commandlogPushEntryIfNeeded(c, argv, argc, net_output_bytes_curr_cmd, COMMANDLOG_TYPE_LARGE_REPLY);
    }
}

/* Stash a copy-avoidance command's argv (and the plain-reply bytes already
 * counted on the main thread) into the per-client deferred FIFO. The exact
 * reply size is filled in later at buffer-release time. The FIFO is lazily
 * allocated and grown geometrically, then reused for the client's lifetime. */
static void commandlogEnqueueDeferred(client *c, robj **argv, int argc, size_t plain_bytes) {
    if (c->cmdlog_deferred_len == c->cmdlog_deferred_cap) {
        int newcap = c->cmdlog_deferred_cap ? c->cmdlog_deferred_cap * 2 : 8;
        c->cmdlog_deferred = zrealloc(c->cmdlog_deferred, (size_t)newcap * sizeof(cmdlogDeferredReply));
        c->cmdlog_deferred_cap = newcap;
    }

    cmdlogDeferredReply *e = &c->cmdlog_deferred[c->cmdlog_deferred_len++];
    if (argc <= CMDLOG_INLINE_ARGV_MAX) {
        e->argv = e->argv_inline;
    } else {
        e->argv = zmalloc((size_t)argc * sizeof(robj *));
    }
    for (int j = 0; j < argc; j++) {
        e->argv[j] = argv[j];
        incrRefCount(argv[j]);
    }
    e->argc = argc;
    e->plain_bytes = plain_bytes;
    e->bulk_bytes = 0;

    /* The next command's first BULK_STR_REF header starts a new command. */
    c->cmdlog_bulk_boundary = 1;
}

/* Attribute one BULK_STR_REF header's exact reply size (as computed by the IO
 * thread) to the correct deferred command. Called in buffer order from
 * releaseBufReferences() on the main thread; a cmd_start header advances to the
 * next queued command. */
void commandlogAccumulateDeferredBytes(client *c, size_t reply_len, int cmd_start) {
    if (cmd_start) c->cmdlog_deferred_cursor++;
    if (c->cmdlog_deferred_cursor < 0 || c->cmdlog_deferred_cursor >= c->cmdlog_deferred_len) return;
    c->cmdlog_deferred[c->cmdlog_deferred_cursor].bulk_bytes += reply_len;
}

/* Run the deferred large-reply threshold checks once all of a client's replies
 * have been flushed and their exact sizes attributed. Called from
 * postWriteToClient() on the main thread. */
void commandlogFinalizeDeferred(client *c) {
    for (int i = 0; i < c->cmdlog_deferred_len; i++) {
        cmdlogDeferredReply *e = &c->cmdlog_deferred[i];
        long long total = (long long)(e->plain_bytes + e->bulk_bytes);
        commandlogPushEntryIfNeeded(c, e->argv, e->argc, total, COMMANDLOG_TYPE_LARGE_REPLY);
        for (int j = 0; j < e->argc; j++) decrRefCount(e->argv[j]);
        if (e->argv != e->argv_inline) zfree(e->argv);
    }
    c->cmdlog_deferred_len = 0;
    c->cmdlog_deferred_cursor = -1;
    /* Ready the boundary marker for the next command's first reply. */
    c->cmdlog_bulk_boundary = 1;
}

/* Release any queued deferred entries without logging (e.g. on client free).
 * Frees stashed argv refs and the FIFO backing array. */
void commandlogFreeDeferred(client *c) {
    for (int i = 0; i < c->cmdlog_deferred_len; i++) {
        cmdlogDeferredReply *e = &c->cmdlog_deferred[i];
        for (int j = 0; j < e->argc; j++) decrRefCount(e->argv[j]);
        if (e->argv != e->argv_inline) zfree(e->argv);
    }
    zfree(c->cmdlog_deferred);
    c->cmdlog_deferred = NULL;
    c->cmdlog_deferred_len = 0;
    c->cmdlog_deferred_cap = 0;
    c->cmdlog_deferred_cursor = -1;
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
