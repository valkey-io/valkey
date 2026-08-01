#include "server.h"
#include "zmalloc.h"
#include "script.h"
#include <assert.h>
#include <stdatomic.h>

/* Forward declarations from module.h to avoid pulling in full module internals
 * which has header dependency issues when included before server.h */
int moduleClientIsBlockedOnKeys(client *c);

/*============================ Internal prototypes ========================= */
static void resetPreExecutionOffset(struct client *c);
static void trackCommandPreExecutionPosition(struct client *c);
static int unblockClientWaitingAck(struct client *c);
static bool clientEligibleForResponseTracking(client *c);
static void unblockFirstResponse(const struct client *c);
static int isBlockingNeededForOffset(const struct client *c, long long offset);
static void blockClientAndMonitorsOnReplOffset(struct client *c, long long blockingReplOffset);
static long long getSingleCommandBlockingOffsetForReplicatingCommand(client *c);
static long long getSingleCommandBlockingOffsetForNonReplicatingCommand(client *c);
static long long getSingleCommandBlockingOffsetForConsistentWrites(client *c);
static void replyBlockingResetPrimaryState(bool is_free_clients_needed);


/*================================= Utility functions ======================== */

/* Returns the replication offset acknowledged by the AOF fsync subsystem,
 * i.e. the most recent offset whose data has been persisted to disk.
 *
 * Uses fsynced_reploff_pending directly instead of fsynced_reploff so that
 * when async AOF flushing is used (BIO threads), the latest fsync progress
 * is observed immediately rather than waiting for the next beforeSleep()
 * iteration to copy it into fsynced_reploff. Falls back to fsynced_reploff
 * if pending is still 0 but a non-zero fsynced_reploff has been recorded
 * (e.g. after server load). */
static long long aofAckedOffset(void) {
    long long fsynced_offset = atomic_load_explicit(&server.fsynced_reploff_pending, memory_order_relaxed);
    if (fsynced_offset == 0 && server.fsynced_reploff > 0) {
        fsynced_offset = server.fsynced_reploff;
    }
    return fsynced_offset;
}

/* Utility function to determine whether AOF is configured for synchronous
 * reply-blocking — i.e. AOF is enabled and appendfsync is set to always. */
int isAofReplyBlockingEnabled(void) {
    return server.aof_state != AOF_OFF && server.aof_fsync == AOF_FSYNC_ALWAYS;
}

/* Returns the replication offset that has been durably committed locally.
 *
 * When AOF synchronous reply-blocking is enabled, this is the AOF-acknowledged
 * offset (or the snapshot captured at pause time, when paused via DEBUG).
 * When AOF synchronous reply-blocking is disabled, no local reply-blocking gate
 * is in effect, so the primary's current replication offset is returned
 * (i.e. nothing is reply-blocked). */
long long getDurablyCommittedOffset(void) {
    if (!isAofReplyBlockingEnabled()) {
        return server.primary_repl_offset;
    }
    if (server.reply_blocking.aof_paused) {
        return server.reply_blocking.aof_paused_offset;
    }
    return aofAckedOffset();
}

/* Pause AOF reply-blocking progress (via DEBUG command).
 * The current AOF-acked offset is captured and frozen — any writes after
 * the pause point will block until reply-blocking is resumed and catches up. */
void pauseAofReplyBlocking(void) {
    /* Snapshot the current acked offset before pausing so that writes
     * already acknowledged remain unblocked. */
    server.reply_blocking.aof_paused_offset = aofAckedOffset();
    server.reply_blocking.aof_paused = true;
    serverLog(LL_NOTICE, "Paused AOF reply-blocking (frozen at offset %lld)",
              server.reply_blocking.aof_paused_offset);
}

/* Resume AOF reply-blocking progress (via DEBUG command).
 * After resuming, triggers a reply-blocking progress check to unblock any
 * clients that can now proceed. */
void resumeAofReplyBlocking(void) {
    server.reply_blocking.aof_paused = false;
    // Trigger a reply-blocking check to unblock any clients that can now proceed
    notifyReplyBlockingProgress();
    serverLog(LL_NOTICE, "Resumed AOF reply-blocking");
}

/* Utility function to determine whether reply-blocking is enabled.
 * Reply-blocking is enabled when the BIO AOF offload path is active and the
 * AOF subsystem is configured for synchronous reply-blocking (appendonly +
 * appendfsync always). */
int isReplyBlockingEnabled(void) {
    return server.bio_aof_offload_enabled && isAofReplyBlockingEnabled();
}

// Utility function to determine whether reply-blocking is enabled on a primary node.
int isPrimaryReplyBlockingEnabled(void) {
    return isReplyBlockingEnabled() && iAmPrimary();
}

/*================================= Client management ======================== */

// Reset the pre-execution offset fields.
static void resetPreExecutionOffset(struct client *c) {
    c->reply_blocking_state.offset.recorded = false;
    c->reply_blocking_state.offset.reply_block = NULL;
    c->reply_blocking_state.offset.byte_offset = 0;
}


// Track the pre-execution position in the client reply COB.
static void trackCommandPreExecutionPosition(struct client *c) {
    resetPreExecutionOffset(c);
    list *reply = c->reply;
    int bufpos = c->bufpos;

    if (reply != NULL && listLength(reply) > 0) {
        listNode *last_reply_block = listLast(reply);
        c->reply_blocking_state.offset.reply_block = last_reply_block;
        c->reply_blocking_state.offset.byte_offset = ((clientReplyBlock *)listNodeValue(last_reply_block))->used;
    } else if (bufpos > 0) {
        c->reply_blocking_state.offset.reply_block = NULL;
        c->reply_blocking_state.offset.byte_offset = bufpos;
    }
    c->reply_blocking_state.offset.recorded = true;
}

/* If the client is currently waiting for reply-blocking acknowledgement,
 * mark it unblocked and reset the client flags. */
static int unblockClientWaitingAck(struct client *c) {
    if (c->reply_blocking_state.reply_blocked) {
        listNode *node = listSearchKey(server.reply_blocking.clients_waiting_ack, c);
        if (node != NULL) {
            listDelNode(server.reply_blocking.clients_waiting_ack, node);
            c->reply_blocking_state.reply_blocked = 0;
            return 1;
        }
    }
    return 0;
}

// Initialize the reply-blocking client attributes when client is created.
void replyBlockingClientInit(client *c) {
    if (!isReplyBlockingEnabled()) {
        return;
    }
    if (c->reply_blocking_state.blocked_responses == NULL) {
        c->reply_blocking_state.blocked_responses = listCreate();
        listSetFreeMethod(c->reply_blocking_state.blocked_responses, zfree);
        resetPreExecutionOffset(c);
        c->reply_blocking_state.current_command_repl_offset = -1;
        c->reply_blocking_state.module_cmd_blocking_offset = -1;
        c->reply_blocking_state.deferred_block_offset = -1;
        c->reply_blocking_state.pending_notify_tasks = listCreate();
    }
}

// Reset the client reply-blocking attributes during a client clean-up.
void replyBlockingClientReset(client *c) {
    if (unblockClientWaitingAck(c)) {
        server.reply_blocking.clients_disconnected_before_unblocking++;
    }

    if (c->reply_blocking_state.blocked_responses != NULL) {
        listRelease(c->reply_blocking_state.blocked_responses);
        c->reply_blocking_state.blocked_responses = NULL;
    }

    if (c->reply_blocking_state.pending_notify_tasks != NULL) {
        postCommitTaskNotifyClientDestroy(c->reply_blocking_state.pending_notify_tasks);
        listRelease(c->reply_blocking_state.pending_notify_tasks);
        c->reply_blocking_state.pending_notify_tasks = NULL;
    }

    resetPreExecutionOffset(c);
    c->reply_blocking_state.current_command_repl_offset = -1;
    c->reply_blocking_state.module_cmd_blocking_offset = -1;
}

// Determines if a client is doing a transaction or not.
static bool isClientDoingTransaction(client *c) {
    return c->cmd->proc == execCommand || IS_SCRIPT_CALL_CMD(c->cmd);
}

// Returns true if the client is eligible for keyspace tracking on a primary node.
static bool clientEligibleForResponseTracking(client *c) {
    serverAssert(iAmPrimary());

    if (c->cmd == NULL) return false;

    bool is_keyspace_global_cmd = IS_KEYSPACE_GLOBAL(c->cmd);

    if ((c->cmd->flags & CMD_ADMIN) && !(c->cmd->flags & CMD_WRITE) && !is_keyspace_global_cmd) {
        return false;
    }

    return ((c->cmd->flags & (CMD_WRITE | CMD_READONLY)) || isClientDoingTransaction(c) || is_keyspace_global_cmd || isFunctionStoreRWCommand(c));
}

/* Check if we only allow client to receive up to a certain
 * position in the client reply buffer. */
inline bool isClientReplyBufferLimited(client *c) {
    return c->reply_blocking_state.blocked_responses != NULL &&
           listLength(c->reply_blocking_state.blocked_responses) > 0;
}

/*================================= Response blocking ======================= */

/* Store the metrics for a command when blocking
 * @param c The client that issued the command.
 * @param br The Node at which commands are blocked. */
static inline void initCmdMetrics(const client *c, struct blockedResponse *br) {
    if (!c->cmd) {
        /* If client command is NULL, eg Monitor clients, we do not start the timer
         * because we do not emit metrics for this response. */
        br->blocked_command_timer = 0;
        return;
    }

    elapsedStart(&br->blocked_command_timer);
    // For end-to-end latency measurement

    if (c->reply_blocking_state.reply_blocking_flags & REPLY_BLOCKING_CLIENT_LAST_CMD_WRITE) {
        server.reply_blocking.write_responses_blocked++;
        br->cmd_type = REPLY_BLOCKED_CMD_WRITE;
    } else if (c->reply_blocking_state.reply_blocking_flags & REPLY_BLOCKING_CLIENT_LAST_CMD_READONLY) {
        server.reply_blocking.read_responses_blocked++;
        br->cmd_type = REPLY_BLOCKED_CMD_READ;
    } else {
        server.reply_blocking.other_responses_blocked++;
        br->cmd_type = REPLY_BLOCKED_CMD_OTHER;
    }
}


// Block the last response if it exists in the client output buffer.
static void blockLastResponseIfExist(const client *c, const long long blocked_offset) {
    serverAssert(c->reply_blocking_state.offset.recorded);

    bool has_new_response = false;
    listNode *disallowed_reply_block =
        c->reply_blocking_state.offset.reply_block;
    size_t disallowed_byte_offset =
        c->reply_blocking_state.offset.byte_offset;

    if (disallowed_reply_block == NULL) {
        if ((size_t)c->bufpos > disallowed_byte_offset) {
            has_new_response = true;
        } else if (listLength(c->reply) > 0) {
            has_new_response = true;
            disallowed_byte_offset = 0;
            disallowed_reply_block = listFirst(c->reply);
        }
    } else {
        const clientReplyBlock *last_reply_block = listNodeValue(disallowed_reply_block);
        if (last_reply_block->used > disallowed_byte_offset) {
            has_new_response = true;
        } else if (disallowed_reply_block->next != NULL) {
            has_new_response = true;
            disallowed_byte_offset = 0;
            disallowed_reply_block = disallowed_reply_block->next;
        }
    }

    if (has_new_response) {
        blockedResponse *new_block = zcalloc(sizeof(blockedResponse));
        new_block->primary_repl_offset = blocked_offset;
        new_block->disallowed_byte_offset = disallowed_byte_offset;
        new_block->disallowed_reply_block = disallowed_reply_block;
        initCmdMetrics(c, new_block);
        listAddNodeTail(c->reply_blocking_state.blocked_responses, new_block);
    }
}


/* Process the metrics of all commands blocked at a BlockedResponse while unblocking
 * @param br The Node at which commands are blocked. */
static inline void processCmdMetrics(struct blockedResponse *br) {
    if (br->blocked_command_timer == 0) return; /* Do not count the response if timer is not started */

    unsigned long long duration = elapsedUs(br->blocked_command_timer);

    if (br->cmd_type == REPLY_BLOCKED_CMD_WRITE) {
        server.reply_blocking.write_responses_blocked_cumulative_time_us += duration;
        server.reply_blocking.write_responses_unblocked++;
    } else if (br->cmd_type == REPLY_BLOCKED_CMD_READ) {
        server.reply_blocking.read_responses_blocked_cumulative_time_us += duration;
        server.reply_blocking.read_responses_unblocked++;
    } else {
        server.reply_blocking.other_responses_blocked_cumulative_time_us += duration;
        server.reply_blocking.other_responses_unblocked++;
    }
}
// Unblocks the first response in the client's blocked responses list.
static void unblockFirstResponse(const client *c) {
    serverAssert(c->reply_blocking_state.blocked_responses != NULL);
    if (listLength(c->reply_blocking_state.blocked_responses) > 0) {
        listNode *first = listFirst(c->reply_blocking_state.blocked_responses);
        processCmdMetrics(listNodeValue(first));
        listDelNode(c->reply_blocking_state.blocked_responses, first);
    }
}

// Determines if we need to block on a given replication offset for a given client.
static int isBlockingNeededForOffset(const client *c, const long long offset) {
    if (offset == -1 || isAofReplyBlockingEnabled() == 0) {
        return 0;
    }

    if (listLength(c->reply_blocking_state.blocked_responses) == 0)
        return 1;

    listNode *last_response = listLast(c->reply_blocking_state.blocked_responses);
    long long previous_offset = ((blockedResponse *)listNodeValue(last_response))->primary_repl_offset;
    return previous_offset < offset;
}

// Block a given client on the specified replication offset if applicable.
void blockClientOnReplOffset(client *c, const long long blockingReplOffset) {
    serverAssert(isPrimaryReplyBlockingEnabled());

    if (isBlockingNeededForOffset(c, blockingReplOffset)) {
        serverLog(LL_DEBUG, "client should be blocked at offset %lld, cmd=%s, is_write=%d",
                  blockingReplOffset, c->cmd->declared_name, (c->cmd->flags & CMD_WRITE) ? 1 : 0);
        blockLastResponseIfExist(c, blockingReplOffset);
        if (!c->reply_blocking_state.reply_blocked) {
            listAddNodeTail(server.reply_blocking.clients_waiting_ack, c);
            c->reply_blocking_state.reply_blocked = 1;
            server.reply_blocking.clients_blocked++;
        }
    }

    resetPreExecutionOffset(c);
}

// Utility function to determine whether a command should be replicated to monitors.
static inline int isCommandReplicatedToMonitors(void) {
    return listLength(server.monitors) && !server.loading;
}

// Block all connected MONITOR clients on the specified replication offset.
static void blockMonitorsOnReplOffset(long long blockingReplOffset) {
    if (isCommandReplicatedToMonitors()) {
        listNode *ln;
        listIter li;
        listRewind(server.monitors, &li);
        while ((ln = listNext(&li))) {
            client *monitor = ln->value;
            blockClientOnReplOffset(monitor, blockingReplOffset);
        }
    }
}

// Block a client and all connected MONITOR clients on the specified replication offset.
static void blockClientAndMonitorsOnReplOffset(client *c, long long blockingReplOffset) {
    blockClientOnReplOffset(c, blockingReplOffset);
    blockMonitorsOnReplOffset(blockingReplOffset);
}

/* Snapshot the COB tail just before a parked deferred reply is committed, so the
 * durability boundary lands on the bytes about to be appended. Snapshotting here
 * (rather than reusing the pre-execution snapshot) keeps the raw reply-list pointer
 * from dangling across the module-blocked window. */
void replyBlockingSnapshotBeforeDeferredReplyCommit(client *c) {
    if (!isPrimaryReplyBlockingEnabled()) return;
    if (c->reply_blocking_state.deferred_block_offset == -1) return;
    trackCommandPreExecutionPosition(c);
}

/* Apply the stashed durability boundary over the deferred reply just committed
 * into the COB (paired with the snapshot above). */
void replyBlockingApplyDeferredReplyBoundary(client *c) {
    if (c->reply_blocking_state.deferred_block_offset == -1) return;
    long long offset = c->reply_blocking_state.deferred_block_offset;
    c->reply_blocking_state.deferred_block_offset = -1;
    if (!isPrimaryReplyBlockingEnabled()) {
        resetPreExecutionOffset(c);
        return;
    }
    /* Already durable (module held the client past the ack): send now, no boundary.
     * Otherwise hold the reply until the ack advances the consensus offset. */
    if (getDurablyCommittedOffset() >= offset) {
        resetPreExecutionOffset(c);
    } else {
        blockClientOnReplOffset(c, offset);
    }
}

/*================================= Unblocking ============================== */

// Unblock responses and tasks of all blocked clients with a given consensus acked offset.
void unblockResponsesWithAckOffset(const reply_blocking_t *rb_state, const long long consensus_ack_offset) {
    serverLog(LL_DEBUG, "unblocking clients for consensus offset %lld,", consensus_ack_offset);
    listIter li, li_response;
    listNode *ln, *ln_response;
    listRewind(rb_state->clients_waiting_ack, &li);
    blockedResponse *br = NULL;
    while ((ln = listNext(&li))) {
        client *c = ln->value;

        serverAssert(c->reply_blocking_state.blocked_responses != NULL);
        listRewind(c->reply_blocking_state.blocked_responses, &li_response);
        bool unblocked_responses = false;

        while ((ln_response = listNext(&li_response))) {
            br = listNodeValue(ln_response);
            if (br->primary_repl_offset <= consensus_ack_offset) {
                unblockFirstResponse(c);
                if (unblocked_responses == false) {
                    unblocked_responses = true;
                }
            } else {
                break;
            }
        }
        if (listLength(c->reply_blocking_state.blocked_responses) == 0) {
            if (unblockClientWaitingAck(c)) {
                server.reply_blocking.clients_unblocked++;
            }
        }
        if (unblocked_responses) {
            putClientInPendingWriteQueue(c);
        }
    }

    executeDeferredTasksForAck(consensus_ack_offset);
}

/*================================= Post-ack handlers ======================= */

void notifyReplyBlockingProgress(void) {
    if (!isPrimaryReplyBlockingEnabled()) {
        return;
    }

    reply_blocking_t *rb_state = &server.reply_blocking;
    const long long consensus_ack_offset = getDurablyCommittedOffset();
    if (consensus_ack_offset <= rb_state->previous_acked_offset) {
        return;
    }

    rb_state->previous_acked_offset = consensus_ack_offset;
    drainCommittedKeys(consensus_ack_offset);
    unblockResponsesWithAckOffset(rb_state, consensus_ack_offset);
}

/*================================= Function Store Tracking ================== */

bool isFunctionRWCommand(client *c) {
    return (c->argc > 0 && (!strcasecmp(objectGetVal(c->argv[0]), "FUNCTION"))) && !(c->argc > 1 && !strcasecmp(objectGetVal(c->argv[1]), "HELP"));
}

bool isFunctionStoreRWCommand(client *c) {
    return isFunctionRWCommand(c) || c->cmd->proc == fcallCommand || c->cmd->proc == fcallroCommand;
}

bool isUncommittedFunctionStore(void) {
    return server.reply_blocking.func_store_blocking_offset > server.reply_blocking.previous_acked_offset;
}

void handleUncommittedFunctionStore(void) {
    if (server.execution_nesting) {
        server.reply_blocking.processed_func_write_in_transaction = true;
    } else {
        server.reply_blocking.func_store_blocking_offset = server.primary_repl_offset;
    }
}

long long getFuncStoreBlockingOffset(void) {
    return server.reply_blocking.func_store_blocking_offset;
}

void updateFuncStoreBlockingOffsetForWrite(long long blocking_repl_offset) {
    if (server.reply_blocking.processed_func_write_in_transaction) {
        server.reply_blocking.func_store_blocking_offset = blocking_repl_offset;
        server.reply_blocking.processed_func_write_in_transaction = false;
    }
}

/*========================== Command offset calculation ===================== */

// Process a single replicating command for consistent write blocking.
static long long getSingleCommandBlockingOffsetForReplicatingCommand(client *c) {
    if (!(c->cmd->flags & CMD_WRITE)) {
        return -1;
    }

    if (isFunctionRWCommand(c)) {
        handleUncommittedFunctionStore();
    } else {
        getKeysResult result;
        initGetKeysResult(&result);
        /* Use key specs directly to extract key positions. We avoid
         * getKeysFromCommand / getkeys_proc because some commands (e.g. SET)
         * rewrite argv during execution (EX→PXAT) and the custom getkeys_proc
         * may crash on the rewritten embedded-string robj. We only need key
         * positions here, not per-key flags, so key specs are sufficient. */
        int numkeys = getKeysUsingKeySpecs(c->cmd, c->argv, c->argc, GET_KEYSPEC_DEFAULT, &result);
        keyReference *keys = result.keys;
        if (numkeys > 0) {
            if (c->cmd->proc == moveCommand) {
                int dest_dbid = -1;
                if (getIntFromObject(c->argv[2], &dest_dbid) == C_ERR || dest_dbid < 0 || dest_dbid >= server.dbnum) {
                    getKeysFreeResult(&result);
                    return -1;
                }
                handleUncommittedKeyForClient(c, c->argv[keys[0].pos], server.db[dest_dbid]);
            } else if (c->cmd->proc == copyCommand) {
                int dest_dbid;
                if (!getTargetDbIdForCopyCommand(c->argc, c->argv, c->db->id, &dest_dbid)) {
                    getKeysFreeResult(&result);
                    return -1;
                }
                if (dest_dbid != c->db->id) {
                    handleUncommittedKeyForClient(c, c->argv[2], server.db[dest_dbid]);
                }
            }

            for (int i = 0; i < numkeys; i++) {
                handleUncommittedKeyForClient(c, c->argv[keys[i].pos], c->db);
            }
        }
        getKeysFreeResult(&result);
    }

    if (!server.execution_nesting) {
        return server.primary_repl_offset;
    }

    return -1;
}

// Process a single non-replicating command for consistent write blocking.
static long long getSingleCommandBlockingOffsetForNonReplicatingCommand(client *c) {
    long long blocking_repl_offset = -1;

    if (isFunctionStoreRWCommand(c)) {
        blocking_repl_offset = getFuncStoreBlockingOffset();
    } else if (IS_SCRIPT_CALL_READONLY_CMD(c->cmd)) {
        return -1;
    } else if ((c->cmd->flags & CMD_MODULE) && (c->reply_blocking_state.module_cmd_blocking_offset != -1)) {
        blocking_repl_offset = c->reply_blocking_state.module_cmd_blocking_offset;
    } else if (c->cmd->flags & (CMD_READONLY | CMD_WRITE)) {
        blocking_repl_offset = c->db->dirty_repl_offset;
        getKeysResult result;
        initGetKeysResult(&result);
        int numkeys = getKeysUsingKeySpecs(c->cmd, c->argv, c->argc, GET_KEYSPEC_DEFAULT, &result);
        keyReference *keys = result.keys;

        for (int i = 0; i < numkeys; i++) {
            sds keystr = objectGetVal(c->argv[keys[i].pos]);
            long long offset = getUncommittedKeyOffset(keystr, c->db, server.reply_blocking.previous_acked_offset);
            if (offset > blocking_repl_offset) {
                blocking_repl_offset = offset;
            }
        }

        // COPY/MOVE may target a different DB; check the destination key there too.
        if (c->cmd->proc == moveCommand) {
            int dest_dbid = -1;
            if (getIntFromObject(c->argv[2], &dest_dbid) == C_OK && dest_dbid >= 0 && dest_dbid < server.dbnum &&
                dest_dbid != c->db->id) {
                long long offset = getUncommittedKeyOffset(objectGetVal(c->argv[1]), server.db[dest_dbid], server.reply_blocking.previous_acked_offset);
                if (offset > blocking_repl_offset) blocking_repl_offset = offset;
            }
        } else if (c->cmd->proc == copyCommand) {
            int dest_dbid;
            if (getTargetDbIdForCopyCommand(c->argc, c->argv, c->db->id, &dest_dbid) && dest_dbid != c->db->id) {
                long long offset = getUncommittedKeyOffset(objectGetVal(c->argv[2]), server.db[dest_dbid], server.reply_blocking.previous_acked_offset);
                if (offset > blocking_repl_offset) blocking_repl_offset = offset;
            }
        }

        getKeysFreeResult(&result);
    }

    return blocking_repl_offset;
}

// Process a single command for consistent write blocking.
static long long getSingleCommandBlockingOffsetForConsistentWrites(struct client *c) {
    serverAssert(isPrimaryReplyBlockingEnabled());

    if (!isAofReplyBlockingEnabled())
        return -1;

    long long blocking_repl_offset = -1;

    // Whole-keyspace commands have no key argument, so their result depends on
    // any uncommitted write anywhere. Block them on the newest offset whenever
    // any data is dirty.
    if (IS_KEYSPACE_GLOBAL(c->cmd) &&
        (listLength(server.reply_blocking.clients_waiting_ack) > 0 || hasUncommittedKeys() || isUncommittedFunctionStore())) {
        blocking_repl_offset = server.primary_repl_offset;
    } else if ((server.primary_repl_offset > server.reply_blocking.pre_call_replication_offset) || (server.also_propagate.numops > server.reply_blocking.pre_call_num_ops_pending_propagation)) {
        blocking_repl_offset = getSingleCommandBlockingOffsetForReplicatingCommand(c);
    } else {
        blocking_repl_offset = getSingleCommandBlockingOffsetForNonReplicatingCommand(c);
    }

    if (blocking_repl_offset <= server.reply_blocking.previous_acked_offset) {
        blocking_repl_offset = -1;
    }

    return blocking_repl_offset;
}

static void replyBlockingSetClientCmdFlags(client *c) {
    /* Transaction wrapper commands, e.g., eval, exec, fcall, should not interfere with the
     * final classification of the transaction itself as read or write. Rather the commands
     * executed inside the transaction will define if it is read or write or none. */
    if (isClientDoingTransaction(c)) return;
    if (c->cmd->flags & CMD_WRITE)
        c->reply_blocking_state.reply_blocking_flags |= REPLY_BLOCKING_CLIENT_LAST_CMD_WRITE;
    else if (c->cmd->flags & CMD_READONLY)
        c->reply_blocking_state.reply_blocking_flags |= REPLY_BLOCKING_CLIENT_LAST_CMD_READONLY;
}

/*=========================== Command hook functions ======================= */

// Record the starting replication offset of the command about to be executed.
void recordReplOffsetBaseline(struct client *c) {
    if (!isPrimaryReplyBlockingEnabled()) return;

    replyBlockingSetClientCmdFlags(c);


    server.reply_blocking.pre_call_replication_offset = server.primary_repl_offset;
    server.reply_blocking.pre_call_num_ops_pending_propagation = server.also_propagate.numops;
}

static bool isClientBlockedByModule(struct client *c) {
    return c->flag.blocked &&
           c->bstate &&
           c->bstate->btype == BLOCKED_MODULE &&
           !moduleClientIsBlockedOnKeys(c);
}

/* After processing a command, track the replication offset and update
 * the blocking offset for the command block. */
void computeCommandBlockingOffset(client *c) {
    serverLog(LL_DEBUG, "computeCommandBlockingOffset entered for command '%s'", c->cmd->declared_name);
    if (!isPrimaryReplyBlockingEnabled() || (c->flag.blocked && !isClientBlockedByModule(c)))
        return;

    long long current_cmd_blocking_offset = getSingleCommandBlockingOffsetForConsistentWrites(c);

    client *tracking_client = server.current_client ? server.current_client : c;

    if (current_cmd_blocking_offset > tracking_client->reply_blocking_state.current_command_repl_offset) {
        tracking_client->reply_blocking_state.current_command_repl_offset = current_cmd_blocking_offset;
    }

    handleDatabaseModification(c);
}

char *validateScriptForReplyBlocking(client *c) {
    UNUSED(c);
    return NULL;
}

// Perform pre-processing before command execution for a given client.
int beginCommandReplyBlocking(client *c) {
    c->reply_blocking_state.reply_blocking_flags = 0;
    c->reply_blocking_state.current_command_repl_offset = -1;
    c->reply_blocking_state.module_cmd_blocking_offset = -1;
    c->reply_blocking_state.deferred_block_offset = -1;

    if (iAmPrimary() && clientEligibleForResponseTracking(c)) {
        trackCommandPreExecutionPosition(c);

        if (isCommandReplicatedToMonitors()) {
            listNode *ln;
            listIter li;
            listRewind(server.monitors, &li);
            while ((ln = listNext(&li))) {
                client *monitor = ln->value;
                trackCommandPreExecutionPosition(monitor);
            }
        }
    }

    server.reply_blocking.pre_command_replication_offset = server.primary_repl_offset;
    return CMD_FILTER_ALLOW;
}

// Perform post-processing after command execution for a given client.
void finalizeCommandReplyBlocking(client *c) {
    if (!isPrimaryReplyBlockingEnabled() || c->cmd == NULL || c->flag.multi) {
        return;
    }

    long long blocking_repl_offset = c->reply_blocking_state.current_command_repl_offset;

    if (server.primary_repl_offset > server.reply_blocking.pre_command_replication_offset && (c->cmd->flags & CMD_WRITE || isClientDoingTransaction(c)) && c->cmd->proc != syncCommand && c->cmd->proc != clusterCommand && c->cmd->proc != shutdownCommand) {
        blocking_repl_offset = server.primary_repl_offset;
    }

    if (blocking_repl_offset > server.reply_blocking.pre_command_replication_offset) {
        serverAssert(clientEligibleForResponseTracking(c));
    }

    processPendingUncommittedData(server.primary_repl_offset);
    updateFuncStoreBlockingOffsetForWrite(server.primary_repl_offset);

    if (c->flag.blocked && isDeferredReplyEnabled(c) && listLength(c->deferred_reply) > 0) {
        /* A module blocked the client on its KSN callback, so the reply is parked in the
         * deferred buffer, not the COB. Stash the offset and apply the boundary when the
         * reply is committed (commitDeferredReplyBuffer); block monitors now (not deferred). */
        resetPreExecutionOffset(c);
        c->reply_blocking_state.deferred_block_offset = blocking_repl_offset;
        blockMonitorsOnReplOffset(blocking_repl_offset);
    } else {
        blockClientAndMonitorsOnReplOffset(c, blocking_repl_offset);
    }

    certifyPendingDeferredTasks();
}

/*================================= Lifecycle =============================== */

// Initialize the reply-blocking subsystem.
void replyBlockingInit(void) {
    serverLog(LL_DEBUG, "Initializing reply-blocking subsystem");

    // Initialize uncommitted keys pending data
    uncommittedKeysInitPending();

    // Have to init the handlers before using them.
    initTaskTypes();
    server.reply_blocking.previous_acked_offset = -1;
    server.reply_blocking.clients_waiting_ack = listCreate();
    postCommitTaskInitLists();
    server.reply_blocking.clients_blocked = 0;
    server.reply_blocking.clients_unblocked = 0;
    server.reply_blocking.clients_disconnected_before_unblocking = 0;
    server.reply_blocking.read_responses_blocked = 0;
    server.reply_blocking.write_responses_blocked = 0;
    server.reply_blocking.other_responses_blocked = 0;
    server.reply_blocking.read_responses_unblocked = 0;
    server.reply_blocking.write_responses_unblocked = 0;
    server.reply_blocking.other_responses_unblocked = 0;
    server.reply_blocking.read_responses_blocked_cumulative_time_us = 0;
    server.reply_blocking.write_responses_blocked_cumulative_time_us = 0;
    server.reply_blocking.other_responses_blocked_cumulative_time_us = 0;

    // Initialize function store blocking state
    server.reply_blocking.func_store_blocking_offset = -1;
    server.reply_blocking.processed_func_write_in_transaction = false;

    // Initialize AOF reply-blocking pause state (used by DEBUG for testing)
    server.reply_blocking.aof_paused = false;
    server.reply_blocking.aof_paused_offset = 0;

    // Not executing deferred post-commit tasks at startup.
    server.reply_blocking.in_post_commit_task_execution = false;
}

// Clean up the reply-blocking subsystem on server shutdown.
void replyBlockingCleanup(void) {
    if (server.reply_blocking.clients_waiting_ack != NULL) {
        listRelease(server.reply_blocking.clients_waiting_ack);
        server.reply_blocking.clients_waiting_ack = NULL;
    }

    uncommittedKeysCleanupPending();

    // Cleanup deferred tasks waiting for reply-blocking ack
    postCommitTaskCleanupLists();

    clearAllUncommittedKeys();
}

// Disconnect and free clients waiting for reply-blocking ack.
static void freeClientsWaitingAck(const reply_blocking_t *rb_state) {
    listIter li;
    listNode *ln;
    listRewind(rb_state->clients_waiting_ack, &li);
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        freeClient(c);
    }
    listEmpty(rb_state->clients_waiting_ack);
}

// Reset primary state for the reply-blocking subsystem.
static void replyBlockingResetPrimaryState(bool is_free_clients_needed) {
    if (listLength(server.reply_blocking.clients_waiting_ack) > 0) {
        if (is_free_clients_needed) {
            freeClientsWaitingAck(&server.reply_blocking);
        } else {
            unblockResponsesWithAckOffset(&server.reply_blocking, LLONG_MAX);
        }
        serverAssert(listLength(server.reply_blocking.clients_waiting_ack) == 0);
    }
    postCommitTaskEmptyLists();
}

/* Clear the reply-blocking attributes specific to the primary.
 * Invoked when a primary node becomes a replica. */
void replyBlockingClearPrimaryState(void) {
    if (!isReplyBlockingEnabled()) return;
    replyBlockingResetPrimaryState(true);
}

// Generate INFO string for reply-blocking stats.
sds genReplyBlockingInfoString(sds info) {
    if (!isReplyBlockingEnabled()) {
        info = sdscatprintf(info, "reply_blocking_enabled:0\r\n");
        return info;
    }

    info = sdscatprintf(info,
                        "reply_blocking_enabled:1\r\n"
                        "reply_blocking_read_blocked_count:%lld\r\n"
                        "reply_blocking_write_blocked_count:%lld\r\n"
                        "reply_blocking_clients_waiting_ack:%lu\r\n"
                        "reply_blocking_uncommitted_keys:%llu\r\n"
                        "reply_blocking_previous_acked_offset:%lld\r\n"
                        "reply_blocking_primary_repl_offset:%lld\r\n",
                        server.reply_blocking.read_responses_blocked,
                        server.reply_blocking.write_responses_blocked,
                        listLength(server.reply_blocking.clients_waiting_ack),
                        getNumberOfUncommittedKeys(),
                        server.reply_blocking.previous_acked_offset,
                        server.primary_repl_offset);

    return info;
}

// Reset related resources when enabling/disabling reply-blocking.
void replyBlockingReset(void) {
    if (isReplyBlockingEnabled()) {
        server.reply_blocking.pre_command_replication_offset = server.primary_repl_offset;
        listIter li;
        listNode *ln;
        listRewind(server.clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            replyBlockingClientInit(c);
        }
    } else {
        if (iAmPrimary()) {
            replyBlockingResetPrimaryState(false);
        }
        clearAllUncommittedKeys();
    }
}
