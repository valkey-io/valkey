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

#include "server.h"
#include "script.h"
#include "cluster_slot_stats.h"

scriptFlag scripts_flags_def[] = {
    {.flag = VMSE_SCRIPT_FLAG_NO_WRITES, .str = "no-writes"},
    {.flag = VMSE_SCRIPT_FLAG_ALLOW_OOM, .str = "allow-oom"},
    {.flag = VMSE_SCRIPT_FLAG_ALLOW_STALE, .str = "allow-stale"},
    {.flag = VMSE_SCRIPT_FLAG_NO_CLUSTER, .str = "no-cluster"},
    {.flag = VMSE_SCRIPT_FLAG_ALLOW_CROSS_SLOT, .str = "allow-cross-slot-keys"},
    {.flag = 0, .str = NULL}, /* flags array end */
};

/* On script invocation, holding the current run context */
static scriptRunCtx *curr_run_ctx = NULL;

static void exitScriptTimedoutMode(scriptRunCtx *run_ctx) {
    serverAssert(run_ctx == curr_run_ctx);
    serverAssert(scriptIsTimedout());
    run_ctx->flags &= ~SCRIPT_TIMEDOUT;
    blockingOperationEnds();
    /* if we are a replica and we have an active primary, set it for continue processing */
    if (server.primary_host && server.primary) queueClientForReprocessing(server.primary);
}

static void enterScriptTimedoutMode(scriptRunCtx *run_ctx) {
    serverAssert(run_ctx == curr_run_ctx);
    serverAssert(!scriptIsTimedout());
    /* Mark script as timedout */
    run_ctx->flags |= SCRIPT_TIMEDOUT;
    blockingOperationStarts();
}

int scriptIsTimedout(void) {
    return scriptIsRunning() && (curr_run_ctx->flags & SCRIPT_TIMEDOUT);
}

client *scriptGetCaller(void) {
    serverAssert(scriptIsRunning());
    return curr_run_ctx->original_client;
}

/* interrupt function for scripts, should be call
 * from time to time to reply some special command (like ping)
 * and also check if the run should be terminated. */
int scriptInterrupt(scriptRunCtx *run_ctx) {
    if (run_ctx->flags & SCRIPT_TIMEDOUT) {
        /* script already timedout
           we just need to precess some events and return */
        processEventsWhileBlocked();
        return (run_ctx->flags & SCRIPT_KILLED) ? SCRIPT_KILL : SCRIPT_CONTINUE;
    }

    long long elapsed = elapsedMs(run_ctx->start_time);
    if (elapsed < server.busy_reply_threshold) {
        return SCRIPT_CONTINUE;
    }

    serverLog(LL_WARNING,
              "Slow script detected: still in execution after %lld milliseconds. "
              "You can try killing the script using the %s command. Script name is: %s.",
              elapsed, (run_ctx->flags & SCRIPT_EVAL_MODE) ? "SCRIPT KILL" : "FUNCTION KILL", run_ctx->funcname);

    enterScriptTimedoutMode(run_ctx);
    /* Once the script timeouts we reenter the event loop to permit others
     * some commands execution. For this reason
     * we need to mask the client executing the script from the event loop.
     * If we don't do that the client may disconnect and could no longer be
     * here when the EVAL command will return. */
    protectClient(run_ctx->original_client);

    processEventsWhileBlocked();

    return (run_ctx->flags & SCRIPT_KILLED) ? SCRIPT_KILL : SCRIPT_CONTINUE;
}

uint64_t scriptFlagsToCmdFlags(uint64_t cmd_flags, uint64_t script_flags) {
    /* If the script declared flags, clear the ones from the command and use the ones it declared.*/
    cmd_flags &= ~(CMD_STALE | CMD_DENYOOM | CMD_WRITE);

    /* NO_WRITES implies ALLOW_OOM */
    if (!(script_flags & (SCRIPT_FLAG_ALLOW_OOM | SCRIPT_FLAG_NO_WRITES))) cmd_flags |= CMD_DENYOOM;
    if (!(script_flags & SCRIPT_FLAG_NO_WRITES)) cmd_flags |= CMD_WRITE;
    if (script_flags & SCRIPT_FLAG_ALLOW_STALE) cmd_flags |= CMD_STALE;

    /* In addition the MAY_REPLICATE flag is set for these commands, but
     * if we have flags we know if it's gonna do any writes or not. */
    cmd_flags &= ~CMD_MAY_REPLICATE;

    return cmd_flags;
}

/* Prepare the given run ctx for execution */
int scriptPrepareForRun(scriptRunCtx *run_ctx,
                        scriptingEngine *engine,
                        client *caller,
                        const char *funcname,
                        uint64_t script_flags,
                        int ro) {
    serverAssert(!curr_run_ctx);
    int client_allow_oom = !!(caller->flag.allow_oom);

    int running_stale =
        server.primary_host && server.repl_state != REPL_STATE_CONNECTED && server.repl_serve_stale_data == 0;
    int obey_client = mustObeyClient(caller);

    if (!(script_flags & SCRIPT_FLAG_EVAL_COMPAT_MODE)) {
        if ((script_flags & SCRIPT_FLAG_NO_CLUSTER) && server.cluster_enabled) {
            addReplyError(caller, "Can not run script on cluster, 'no-cluster' flag is set.");
            return C_ERR;
        }

        if (running_stale && !(script_flags & SCRIPT_FLAG_ALLOW_STALE)) {
            addReplyError(caller, "-MASTERDOWN Link with MASTER is down, "
                                  "replica-serve-stale-data is set to 'no' "
                                  "and 'allow-stale' flag is not set on the script.");
            return C_ERR;
        }

        if (!(script_flags & SCRIPT_FLAG_NO_WRITES)) {
            /* Script may perform writes we need to verify:
             * 1. we are not a readonly replica
             * 2. no disk error detected
             * 3. command is not `fcall_ro`/`eval[sha]_ro` */
            if (server.primary_host && server.repl_replica_ro && !obey_client) {
                addReplyError(caller, "-READONLY Can not run script with write flag on readonly replica");
                return C_ERR;
            }

            /* Deny writes if we're unable to persist. */
            int deny_write_type = writeCommandsDeniedByDiskError();
            if (deny_write_type != DISK_ERROR_TYPE_NONE && !obey_client) {
                if (deny_write_type == DISK_ERROR_TYPE_RDB)
                    addReplyErrorFormat(caller,
                                        "-MISCONF %s is configured to save RDB snapshots, "
                                        "but it's currently unable to persist to disk. "
                                        "Writable scripts are blocked. Use 'no-writes' flag for read only scripts.",
                                        server.extended_redis_compat ? "Redis" : SERVER_TITLE);
                else
                    addReplyErrorFormat(caller,
                                        "-MISCONF %s is configured to persist data to AOF, "
                                        "but it's currently unable to persist to disk. "
                                        "Writable scripts are blocked. Use 'no-writes' flag for read only scripts. "
                                        "AOF error: %s",
                                        server.extended_redis_compat ? "Redis" : SERVER_TITLE,
                                        strerror(server.aof_last_write_errno));
                return C_ERR;
            }

            if (ro) {
                addReplyError(caller, "Can not execute a script with write flag using *_ro command.");
                return C_ERR;
            }

            /* Don't accept write commands if there are not enough good replicas and
             * user configured the min-replicas-to-write option. */
            if (!checkGoodReplicasStatus()) {
                addReplyErrorObject(caller, shared.noreplicaserr);
                return C_ERR;
            }
        }

        /* Check OOM state. the no-writes flag imply allow-oom. we tested it
         * after the no-write error, so no need to mention it in the error reply. */
        if (!client_allow_oom && server.pre_command_oom_state && server.maxmemory &&
            !(script_flags & (SCRIPT_FLAG_ALLOW_OOM | SCRIPT_FLAG_NO_WRITES))) {
            addReplyError(caller, "-OOM allow-oom flag is not set on the script, "
                                  "can not run it when used memory > 'maxmemory'");
            return C_ERR;
        }

    } else {
        /* Special handling for backwards compatibility (no shebang eval[sha]) mode */
        if (running_stale) {
            addReplyErrorObject(caller, shared.primarydownerr);
            return C_ERR;
        }
    }

    run_ctx->engine = engine;

    run_ctx->original_client = caller;
    run_ctx->funcname = funcname;
    run_ctx->slot = caller->slot;

    run_ctx->start_time = getMonotonicUs();

    run_ctx->flags = 0;
    run_ctx->repl_flags = PROPAGATE_AOF | PROPAGATE_REPL;

    if (ro || (!(script_flags & SCRIPT_FLAG_EVAL_COMPAT_MODE) && (script_flags & SCRIPT_FLAG_NO_WRITES))) {
        /* On fcall_ro or on functions that do not have the 'write'
         * flag, we will not allow write commands. */
        run_ctx->flags |= SCRIPT_READ_ONLY;
    }
    if (client_allow_oom ||
        (!(script_flags & SCRIPT_FLAG_EVAL_COMPAT_MODE) && (script_flags & SCRIPT_FLAG_ALLOW_OOM))) {
        /* Note: we don't need to test the no-writes flag here and set this run_ctx flag,
         * since only write commands can deny-oom. */
        run_ctx->flags |= SCRIPT_ALLOW_OOM;
    }

    if ((script_flags & SCRIPT_FLAG_EVAL_COMPAT_MODE) || (script_flags & SCRIPT_FLAG_ALLOW_CROSS_SLOT)) {
        run_ctx->flags |= SCRIPT_ALLOW_CROSS_SLOT;
    }

    /* set the curr_run_ctx so we can use it to kill the script if needed */
    curr_run_ctx = run_ctx;

    return C_OK;
}

/* Reset the given run ctx after execution */
void scriptResetRun(scriptRunCtx *run_ctx) {
    serverAssert(curr_run_ctx);

    if (scriptIsTimedout()) {
        exitScriptTimedoutMode(run_ctx);
        /* Restore the client that was protected when the script timeout
         * was detected. */
        unprotectClient(run_ctx->original_client);
    }

    run_ctx->slot = -1;

    preventCommandPropagation(run_ctx->original_client);

    /*  unset curr_run_ctx so we will know there is no running script */
    curr_run_ctx = NULL;
}

/* return true if a script is currently running */
int scriptIsRunning(void) {
    return curr_run_ctx != NULL;
}

const char *scriptCurrFunction(void) {
    serverAssert(scriptIsRunning());
    return curr_run_ctx->funcname;
}

int scriptIsEval(void) {
    serverAssert(scriptIsRunning());
    return curr_run_ctx->flags & SCRIPT_EVAL_MODE;
}

/* Kill the current running script */
void scriptKill(client *c, int is_eval) {
    if (!curr_run_ctx) {
        addReplyError(c, "-NOTBUSY No scripts in execution right now.");
        return;
    }
    if (mustObeyClient(curr_run_ctx->original_client)) {
        addReplyError(c, "-UNKILLABLE The busy script was sent by a master instance in the context of replication and "
                         "cannot be killed.");
        return;
    }
    if (curr_run_ctx->flags & SCRIPT_WRITE_DIRTY) {
        addReplyError(c, "-UNKILLABLE Sorry the script already executed write "
                         "commands against the dataset. You can either wait the "
                         "script termination or kill the server in a hard way "
                         "using the SHUTDOWN NOSAVE command.");
        return;
    }
    if (is_eval && !(curr_run_ctx->flags & SCRIPT_EVAL_MODE)) {
        /* Kill a function with 'SCRIPT KILL' is not allow */
        addReplyErrorObject(c, shared.slowscripterr);
        return;
    }
    if (!is_eval && (curr_run_ctx->flags & SCRIPT_EVAL_MODE)) {
        /* Kill an eval with 'FUNCTION KILL' is not allow */
        addReplyErrorObject(c, shared.slowevalerr);
        return;
    }
    curr_run_ctx->flags |= SCRIPT_KILLED;
    addReply(c, shared.ok);
}

long long scriptRunDuration(void) {
    serverAssert(scriptIsRunning());
    return elapsedMs(curr_run_ctx->start_time);
}

int scriptAllowsOOM(void) {
    serverAssert(scriptIsRunning());
    return curr_run_ctx->flags & SCRIPT_ALLOW_OOM;
}

int scriptIsReadOnly(void) {
    serverAssert(scriptIsRunning());
    return curr_run_ctx->flags & SCRIPT_READ_ONLY;
}

int scriptIsWriteDirty(void) {
    serverAssert(scriptIsRunning());
    return curr_run_ctx->flags & SCRIPT_WRITE_DIRTY;
}

void scriptSetWriteDirtyFlag(void) {
    serverAssert(scriptIsRunning());
    curr_run_ctx->flags |= SCRIPT_WRITE_DIRTY;
}

int scriptAllowsCrossSlot(void) {
    serverAssert(scriptIsRunning());
    return curr_run_ctx->flags & SCRIPT_ALLOW_CROSS_SLOT;
}

int scriptGetSlot(void) {
    serverAssert(scriptIsRunning());
    return curr_run_ctx->slot;
}

void scriptSetSlot(int slot) {
    serverAssert(scriptIsRunning());
    curr_run_ctx->slot = slot;
}

void scriptSetOriginalClientSlot(int slot) {
    serverAssert(scriptIsRunning());
    curr_run_ctx->original_client->slot = slot;
}

sds scriptGetRunningEngineName(void) {
    serverAssert(scriptIsRunning());
    return scriptingEngineGetName(curr_run_ctx->engine);
}

/* For cross-slot scripting, its caller client's slot must be invalidated,
 * such that its slot-stats aggregation is bypassed. */
void scriptClusterSlotStatsInvalidateSlotIfApplicable(void) {
    if (!scriptAllowsCrossSlot()) return;
    curr_run_ctx->original_client->slot = -1;
}
