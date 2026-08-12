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
#include "sorted_array.h"

/* Create a new commandlog entry.
 * Incrementing the ref count of all the objects retained is up to
 * this function. */
static commandlogEntry *commandlogCreateEntry(client *c, robj **argv, int argc, long long value, int type, commandlog_store store) {
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
    ce->id = server.commandlog[type].entry_id[store]++;
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

/* Order two log entries by their value, smallest first. */
static int commandlogEntryCompareByValue(const void *a, const void *b) {
    const commandlogEntry *ea = a;
    const commandlogEntry *eb = b;
    if (ea->value < eb->value) return -1;
    if (ea->value > eb->value) return 1;
    return 0;
}

/* Initialize the command log. This function should be called a single time
 * at server startup. */
void commandlogInit(void) {
    for (int i = 0; i < COMMANDLOG_TYPE_NUM; i++) {
        server.commandlog[i].entries = listCreate();
        for (int j = 0; j < COMMANDLOG_STORE_NUM; j++) server.commandlog[i].entry_id[j] = 0;
        server.commandlog[i].magnitude = sortedArrayCreate(commandlogEntryCompareByValue);
        listSetFreeMethod(server.commandlog[i].entries, commandlogFreeEntry);
        sortedArraySetFreeMethod(server.commandlog[i].magnitude, commandlogFreeEntry);
    }
}

/* Push a new entry into the command log.
 * This function will make sure to trim the command log accordingly to the
 * configured max length. */
static void commandlogPushEntryIfNeeded(client *c, robj **argv, int argc, long long value, int type) {
    commandlog *cl = &server.commandlog[type];

    if (cl->threshold >= 0 && cl->max_len > 0 && value >= cl->threshold) {
        listAddNodeHead(cl->entries, commandlogCreateEntry(c, argv, argc, value, type, COMMANDLOG_STORE_RECENCY));
        /* Remove old entries if needed. */
        while (listLength(cl->entries) > cl->max_len) listDelNode(cl->entries, listLast(cl->entries));
    }

    if (cl->magnitude_max_len > 0) {
        sortedArray *magnitude = cl->magnitude;
        if (sortedArrayLen(magnitude) < cl->magnitude_max_len) {
            sortedArrayInsert(magnitude, commandlogCreateEntry(c, argv, argc, value, type, COMMANDLOG_STORE_MAGNITUDE));
        } else {
            /* Full, compare against the least significant entry. */
            if (value > ((const commandlogEntry *)sortedArrayPeekMin(magnitude))->value) {
                commandlogFreeEntry(sortedArrayExtractMin(magnitude));
                sortedArrayInsert(magnitude, commandlogCreateEntry(c, argv, argc, value, type, COMMANDLOG_STORE_MAGNITUDE));
            }
        }
    }
}

/* Remove all the entries from the given store of the specified command log type. */
static void commandlogResetStore(int type, commandlog_store store) {
    commandlog *cl = &server.commandlog[type];
    if (store == COMMANDLOG_STORE_MAGNITUDE) {
        sortedArrayEmpty(cl->magnitude);
    } else {
        while (listLength(cl->entries) > 0) listDelNode(cl->entries, listLast(cl->entries));
    }
}

/* Remove all the entries from both stores of the specified command log type. */
static void commandlogReset(int type) {
    commandlogResetStore(type, COMMANDLOG_STORE_RECENCY);
    commandlogResetStore(type, COMMANDLOG_STORE_MAGNITUDE);
}

/* Trim every command log store down to its currently configured max length.
 * The magnitude store drops the smallest-value entries first, the recency store
 * drops the oldest. */
void commandlogTrimToMaxLen(void) {
    for (int type = 0; type < COMMANDLOG_TYPE_NUM; type++) {
        commandlog *cl = &server.commandlog[type];
        while (sortedArrayLen(cl->magnitude) > cl->magnitude_max_len) {
            commandlogFreeEntry(sortedArrayExtractMin(cl->magnitude));
        }
        while (listLength(cl->entries) > cl->max_len) {
            listDelNode(cl->entries, listLast(cl->entries));
        }
    }
}

/* Return 1 if the given command log type is enabled in any of its backing stores. */
int commandlogTypeEnabled(int type) {
    commandlog *cl = &server.commandlog[type];
    return (cl->threshold >= 0 && cl->max_len > 0) || cl->magnitude_max_len > 0;
}

static unsigned long commandlogLength(int type, commandlog_store store) {
    commandlog *cl = &server.commandlog[type];
    if (store == COMMANDLOG_STORE_MAGNITUDE) return sortedArrayLen(cl->magnitude);
    return listLength(cl->entries);
}

static void commandlogReplyWithEntry(client *c, commandlogEntry *ce) {
    addReplyArrayLen(c, 6);
    addReplyLongLong(c, ce->id);
    addReplyLongLong(c, ce->time);
    addReplyLongLong(c, ce->value);
    addReplyArrayLen(c, ce->argc);
    for (int j = 0; j < ce->argc; j++) addReplyBulk(c, ce->argv[j]);
    addReplyBulkCBuffer(c, ce->peerid, sdslen(ce->peerid));
    addReplyBulkCBuffer(c, ce->cname, sdslen(ce->cname));
}

/* Reply command logs to client. Recency entries are returned newest first,
 * magnitude entries highest-value first. */
static void commandlogGetReply(client *c, int type, commandlog_store store, long count) {
    commandlog *cl = &server.commandlog[type];
    if (store == COMMANDLOG_STORE_MAGNITUDE) {
        if (count > (long)sortedArrayLen(cl->magnitude)) {
            count = sortedArrayLen(cl->magnitude);
        }
        addReplyArrayLen(c, count);
        for (long i = 0; i < count; i++) {
            commandlogReplyWithEntry(c, sortedArrayGet(cl->magnitude, i));
        }
    } else {
        listIter li;
        listNode *ln;

        if (count > (long)listLength(cl->entries)) {
            count = listLength(cl->entries);
        }
        addReplyArrayLen(c, count);
        listRewind(cl->entries, &li);
        while (count--) {
            ln = listNext(&li);
            commandlogReplyWithEntry(c, ln->value);
        }
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
    commandlogPushEntryIfNeeded(c, argv, argc, net_output_bytes_curr_cmd, COMMANDLOG_TYPE_LARGE_REPLY);
}

/* Parse the optional store selector. Returns C_OK and sets *store on success,
 * or replies with an error and returns C_ERR. */
static int commandlogGetStoreOrReply(client *c, robj *o, commandlog_store *store) {
    if (!strcasecmp(objectGetVal(o), "recency")) {
        *store = COMMANDLOG_STORE_RECENCY;
        return C_OK;
    }
    if (!strcasecmp(objectGetVal(o), "magnitude")) {
        *store = COMMANDLOG_STORE_MAGNITUDE;
        return C_OK;
    }
    addReplyError(c, "store should be one of the following: recency, magnitude");
    return C_ERR;
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
        commandlogResetStore(COMMANDLOG_TYPE_SLOW, COMMANDLOG_STORE_RECENCY);
        addReply(c, shared.ok);
    } else if (c->argc == 2 && !strcasecmp(objectGetVal(c->argv[1]), "len")) {
        addReplyLongLong(c, commandlogLength(COMMANDLOG_TYPE_SLOW, COMMANDLOG_STORE_RECENCY));
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
                count = commandlogLength(COMMANDLOG_TYPE_SLOW, COMMANDLOG_STORE_RECENCY);
            }
        }

        commandlogGetReply(c, COMMANDLOG_TYPE_SLOW, COMMANDLOG_STORE_RECENCY, count);
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
            "GET <count> <type> [RECENCY|MAGNITUDE]",
            "    Return top <count> entries of the specified <type> from the commandlog (-1 mean all).",
            "    RECENCY (default) returns the most recent entries, MAGNITUDE the highest-value ones.",
            "    Entries are made of:",
            "    id, timestamp,",
            "        time in microseconds for type of slow,",
            "        or size in bytes for type of large-request,",
            "        or size in bytes for type of large-reply",
            "    arguments array, client IP and port,",
            "    client name",
            "LEN <type> [RECENCY|MAGNITUDE]",
            "    Return the length of the specified type of commandlog (default: RECENCY).",
            "RESET <type> [RECENCY|MAGNITUDE]",
            "    Reset the specified type of commandlog. Without the last argument both sets of entries are cleared.",
            NULL,
        };
        addReplyHelp(c, help);
    } else if ((c->argc == 3 || c->argc == 4) && !strcasecmp(objectGetVal(c->argv[1]), "reset")) {
        if ((type = commandlogGetTypeOrReply(c, c->argv[2])) == -1) return;
        if (c->argc == 4) {
            commandlog_store store;
            if (commandlogGetStoreOrReply(c, c->argv[3], &store) != C_OK) return;
            commandlogResetStore(type, store);
        } else {
            commandlogReset(type);
        }
        addReply(c, shared.ok);
    } else if ((c->argc == 3 || c->argc == 4) && !strcasecmp(objectGetVal(c->argv[1]), "len")) {
        commandlog_store store = COMMANDLOG_STORE_RECENCY;
        if ((type = commandlogGetTypeOrReply(c, c->argv[2])) == -1) return;
        if (c->argc == 4 && commandlogGetStoreOrReply(c, c->argv[3], &store) != C_OK) return;
        addReplyLongLong(c, commandlogLength(type, store));
    } else if ((c->argc == 4 || c->argc == 5) && !strcasecmp(objectGetVal(c->argv[1]), "get")) {
        long count;
        commandlog_store store = COMMANDLOG_STORE_RECENCY;

        /* Consume count arg. */
        if (getRangeLongFromObjectOrReply(c, c->argv[2], -1, LONG_MAX, &count,
                                          "count should be greater than or equal to -1") != C_OK)
            return;

        if ((type = commandlogGetTypeOrReply(c, c->argv[3])) == -1) return;

        if (c->argc == 5 && commandlogGetStoreOrReply(c, c->argv[4], &store) != C_OK) return;

        if (count == -1) {
            count = commandlogLength(type, store);
        }

        commandlogGetReply(c, type, store, count);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
