#include "reply_blocking.h"
#include "durable_task.h"
#include "durability_provider.h"
#include "uncommitted_keys.h"
#include "expire.h"
#include "server.h"
#include "zmalloc.h"
#include "script.h"
#include <assert.h>

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
static void durabilityResetPrimaryState(bool is_free_clients_needed);


/*================================= Utility functions ======================== */

/**
 * Utility function to determine whether durability is enabled.
 * Durability is enabled when any registered durability provider reports
 * itself as enabled (e.g. the built-in AOF provider enables when
 * appendonly + appendfsync always).
 */
int isDurabilityEnabled(void) {
    return anyDurabilityProviderEnabled();
}

/**
 * Utility function to determine whether durability is enabled on a primary node.
 */
int isPrimaryDurabilityEnabled(void) {
    return isDurabilityEnabled() && iAmPrimary();
}

/*================================= Client management ======================== */

/**
 * Reset the pre-execution offset fields.
 */
static void resetPreExecutionOffset(struct client *c) {
    c->clientDurabilityInfo.offset.recorded = false;
    c->clientDurabilityInfo.offset.reply_block = NULL;
    c->clientDurabilityInfo.offset.byte_offset = 0;
}


/**
 * Track the pre-execution position in the client reply COB.
 */
static void trackCommandPreExecutionPosition(struct client *c) {
    resetPreExecutionOffset(c);
    list *reply = c->reply;
    int bufpos = c->bufpos;

    if (reply != NULL && listLength(reply) > 0) {
        listNode *last_reply_block = listLast(reply);
        c->clientDurabilityInfo.offset.reply_block = last_reply_block;
        c->clientDurabilityInfo.offset.byte_offset = ((clientReplyBlock *)listNodeValue(last_reply_block))->used;
    } else if (bufpos > 0) {
        c->clientDurabilityInfo.offset.reply_block = NULL;
        c->clientDurabilityInfo.offset.byte_offset = bufpos;
    }
    c->clientDurabilityInfo.offset.recorded = true;
}

/**
 * If the client is currently waiting for durability acknowledgement,
 * mark it unblocked and reset the client flags.
 */
static int unblockClientWaitingAck(struct client *c) {
    if (c->clientDurabilityInfo.durability_blocked) {
        listNode *node = listSearchKey(server.durability.clients_waiting_ack, c);
        if (node != NULL) {
            listDelNode(server.durability.clients_waiting_ack, node);
            c->clientDurabilityInfo.durability_blocked = 0;
            return 1;
        }
    }
    return 0;
}

/**
 * Initialize the durability client attributes when client is created.
 */
void durabilityClientInit(client *c) {
    if (!isDurabilityEnabled()) {
        return;
    }
    if (c->clientDurabilityInfo.blocked_responses == NULL) {
        c->clientDurabilityInfo.blocked_responses = listCreate();
        listSetFreeMethod(c->clientDurabilityInfo.blocked_responses, zfree);
        resetPreExecutionOffset(c);
        c->clientDurabilityInfo.current_command_repl_offset = -1;
        c->clientDurabilityInfo.module_cmd_blocking_offset = -1;
        c->clientDurabilityInfo.pending_notify_tasks = listCreate();
    }
}

/**
 * Reset the client durability attributes during a client clean-up.
 */
void durabilityClientReset(client *c) {
    if (unblockClientWaitingAck(c)) {
        server.durability.clients_disconnected_before_unblocking++;
    }

    if (c->clientDurabilityInfo.blocked_responses != NULL) {
        listRelease(c->clientDurabilityInfo.blocked_responses);
        c->clientDurabilityInfo.blocked_responses = NULL;
    }

    if (c->clientDurabilityInfo.pending_notify_tasks != NULL) {
        durableTaskNotifyClientDestroy(c->clientDurabilityInfo.pending_notify_tasks);
        listRelease(c->clientDurabilityInfo.pending_notify_tasks);
        c->clientDurabilityInfo.pending_notify_tasks = NULL;
    }

    resetPreExecutionOffset(c);
    c->clientDurabilityInfo.current_command_repl_offset = -1;
    c->clientDurabilityInfo.module_cmd_blocking_offset = -1;
}

/**
 * Determines if a client is doing a transaction or not.
 */
static bool isClientDoingTransaction(client *c) {
    return c->cmd->proc == execCommand || IS_SCRIPT_CALL_CMD(c->cmd);
}

/**
 * Returns true if the client is eligible for keyspace tracking on a primary node.
 */
static bool clientEligibleForResponseTracking(client *c) {
    serverAssert(iAmPrimary());

    if (c->cmd == NULL) return false;

    bool is_keyspace_informational_cmd = IS_KEYSPACE_INFORMATIONAL(c->cmd);

    if ((c->cmd->flags & CMD_ADMIN) && !(c->cmd->flags & CMD_WRITE) && !is_keyspace_informational_cmd) {
        return false;
    }

    return ((c->cmd->flags & (CMD_WRITE | CMD_READONLY)) || isClientDoingTransaction(c) || is_keyspace_informational_cmd || isFunctionStoreRWCommand(c));
}

/**
 * Check if we only allow client to receive up to a certain
 * position in the client reply buffer.
 */
inline bool isClientReplyBufferLimited(client *c) {
    return c->clientDurabilityInfo.blocked_responses != NULL &&
           listLength(c->clientDurabilityInfo.blocked_responses) > 0;
}

/*================================= Response blocking ======================= */

/**
 * Store the metrics for a command when blocking
 * @param c The client that issued the command.
 * @param br The Node at which commands are blocked.
 */
static inline void initCmdMetrics(const client *c, struct blockedResponse *br) {
    if (!c->cmd) {
        // If client command is NULL, eg Monitor clients, we do not start the timer
        // because we do not emit metrics for this response.
        br->blocked_command_timer = 0;
        return;
    }

    elapsedStart(&br->blocked_command_timer);
    // For end-to-end latency measurement

    if (c->clientDurabilityInfo.durability_flags & DURABILITY_CLIENT_LAST_CMD_WRITE) {
        server.durability.write_responses_blocked++;
        br->cmd_type = DURABLE_BLOCKED_CMD_WRITE;
    } else if (c->clientDurabilityInfo.durability_flags & DURABILITY_CLIENT_LAST_CMD_READONLY) {
        server.durability.read_responses_blocked++;
        br->cmd_type = DURABLE_BLOCKED_CMD_READ;
    } else {
        server.durability.other_responses_blocked++;
        br->cmd_type = DURABLE_BLOCKED_CMD_OTHER;
    }
}


/**
 * Block the last response if it exists in the client output buffer.
 */
static void blockLastResponseIfExist(const client *c, const long long blocked_offset) {
    serverAssert(c->clientDurabilityInfo.offset.recorded);

    bool has_new_response = false;
    listNode *disallowed_reply_block =
        c->clientDurabilityInfo.offset.reply_block;
    size_t disallowed_byte_offset =
        c->clientDurabilityInfo.offset.byte_offset;

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
        listAddNodeTail(c->clientDurabilityInfo.blocked_responses, new_block);
    }
}


/**
 * Process the metrics of all commands blocked at a BlockedResponse while unblocking
 * @param br The Node at which commands are blocked.
 */
static inline void processCmdMetrics(struct blockedResponse *br) {
    if (br->blocked_command_timer == 0) return; // Do not count the response if timer is not started

    unsigned long long duration = elapsedUs(br->blocked_command_timer);

    if (br->cmd_type == DURABLE_BLOCKED_CMD_WRITE) {
        server.durability.write_responses_blocked_cumulative_time_us += duration;
        server.durability.write_responses_unblocked++;
    } else if (br->cmd_type == DURABLE_BLOCKED_CMD_READ) {
        server.durability.read_responses_blocked_cumulative_time_us += duration;
        server.durability.read_responses_unblocked++;
    } else {
        server.durability.other_responses_blocked_cumulative_time_us += duration;
        server.durability.other_responses_unblocked++;
    }
}
/**
 * Unblocks the first response in the client's blocked responses list.
 */
static void unblockFirstResponse(const client *c) {
    serverAssert(c->clientDurabilityInfo.blocked_responses != NULL);
    if (listLength(c->clientDurabilityInfo.blocked_responses) > 0) {
        listNode *first = listFirst(c->clientDurabilityInfo.blocked_responses);
        processCmdMetrics(listNodeValue(first));
        listDelNode(c->clientDurabilityInfo.blocked_responses, first);
    }
}

/**
 * Determines if we need to block on a given replication offset for a given client.
 */
static int isBlockingNeededForOffset(const client *c, const long long offset) {
    if (offset == -1 || anyDurabilityProviderEnabled() == 0) {
        return 0;
    }

    if (listLength(c->clientDurabilityInfo.blocked_responses) == 0)
        return 1;

    listNode *last_response = listLast(c->clientDurabilityInfo.blocked_responses);
    long long previous_offset = ((blockedResponse *)listNodeValue(last_response))->primary_repl_offset;
    return previous_offset < offset;
}

/**
 * Block a given client on the specified replication offset if applicable.
 */
void blockClientOnReplOffset(client *c, const long long blockingReplOffset) {
    serverAssert(isPrimaryDurabilityEnabled());

    if (isBlockingNeededForOffset(c, blockingReplOffset)) {
        serverLog(LL_DEBUG, "client should be blocked at offset %lld, cmd=%s, is_write=%d",
                  blockingReplOffset, c->cmd->declared_name, (c->cmd->flags & CMD_WRITE) ? 1 : 0);
        blockLastResponseIfExist(c, blockingReplOffset);
        if (!c->clientDurabilityInfo.durability_blocked) {
            listAddNodeTail(server.durability.clients_waiting_ack, c);
            c->clientDurabilityInfo.durability_blocked = 1;
            server.durability.clients_blocked++;
        }
    }

    resetPreExecutionOffset(c);
}

/**
 * Utility function to determine whether a command should be replicated to monitors.
 */
static inline int isCommandReplicatedToMonitors(void) {
    return listLength(server.monitors) && !server.loading;
}

/**
 * Block a client and all connected MONITOR clients on the specified replication offset.
 */
static void blockClientAndMonitorsOnReplOffset(client *c, long long blockingReplOffset) {
    blockClientOnReplOffset(c, blockingReplOffset);

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

/*================================= Unblocking ============================== */

/**
 * Unblock responses and tasks of all blocked clients with a given consensus acked offset.
 */
void unblockResponsesWithAckOffset(const durable_t *durability, const long long consensus_ack_offset) {
    serverLog(LL_DEBUG, "unblocking clients for consensus offset %lld,", consensus_ack_offset);
    listIter li, li_response;
    listNode *ln, *ln_response;
    listRewind(durability->clients_waiting_ack, &li);
    blockedResponse *br = NULL;
    while ((ln = listNext(&li))) {
        client *c = ln->value;

        serverAssert(c->clientDurabilityInfo.blocked_responses != NULL);
        listRewind(c->clientDurabilityInfo.blocked_responses, &li_response);
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
        if (listLength(c->clientDurabilityInfo.blocked_responses) == 0) {
            if (unblockClientWaitingAck(c)) {
                server.durability.clients_unblocked++;
            }
        }
        if (unblocked_responses) {
            putClientInPendingWriteQueue(c);
        }
    }

    executeDeferredTasksForAck(consensus_ack_offset);
}

/*================================= Post-ack handlers ======================= */

void notifyDurabilityProgress(void) {
    if (!isPrimaryDurabilityEnabled()) {
        return;
    }

    durable_t *durability = &server.durability;
    const long long consensus_ack_offset = getDurabilityConsensusOffset();
    if (consensus_ack_offset <= durability->previous_acked_offset) {
        return;
    }

    durability->previous_acked_offset = consensus_ack_offset;
    drainCommittedKeys(consensus_ack_offset);
    unblockResponsesWithAckOffset(durability, consensus_ack_offset);
}

/*================================= Function Store Tracking ================== */

bool isFunctionRWCommand(client *c) {
    return (c->argc > 0 && (!strcasecmp(objectGetVal(c->argv[0]), "FUNCTION"))) && !(c->argc > 1 && !strcasecmp(objectGetVal(c->argv[1]), "HELP"));
}

bool isFunctionStoreRWCommand(client *c) {
    return isFunctionRWCommand(c) || c->cmd->proc == fcallCommand || c->cmd->proc == fcallroCommand;
}

bool isDurableFunctionStoreUncommitted(void) {
    return server.durability.func_store_blocking_offset > server.durability.previous_acked_offset;
}

void handleUncommittedFunctionStore(void) {
    if (server.execution_nesting) {
        server.durability.processed_func_write_in_transaction = true;
    } else {
        server.durability.func_store_blocking_offset = server.primary_repl_offset;
    }
}

long long getFuncStoreBlockingOffset(void) {
    return server.durability.func_store_blocking_offset;
}

void updateFuncStoreBlockingOffsetForWrite(long long blocking_repl_offset) {
    if (server.durability.processed_func_write_in_transaction) {
        server.durability.func_store_blocking_offset = blocking_repl_offset;
        server.durability.processed_func_write_in_transaction = false;
    }
}

/*========================== Command offset calculation ===================== */

/**
 * Process a single replicating command for consistent write blocking.
 */
static long long getSingleCommandBlockingOffsetForReplicatingCommand(client *c) {
    if (!(c->cmd->flags & CMD_WRITE)) {
        return -1;
    }

    if (isFunctionRWCommand(c)) {
        handleUncommittedFunctionStore();
    } else {
        getKeysResult result;
        initGetKeysResult(&result);
        int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
        keyReference *keys = result.keys;
        if (numkeys > 0) {
            if (c->cmd->proc == moveCommand) {
                int dest_dbid = -1;
                if (getIntFromObject(c->argv[2], &dest_dbid) == C_ERR) {
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

/**
 * Process a single non-replicating command for consistent write blocking.
 */
static long long getSingleCommandBlockingOffsetForNonReplicatingCommand(client *c) {
    long long blocking_repl_offset = -1;

    if (isFunctionStoreRWCommand(c)) {
        blocking_repl_offset = getFuncStoreBlockingOffset();
    } else if (IS_SCRIPT_CALL_READONLY_CMD(c->cmd)) {
        return -1;
    } else if ((c->cmd->flags & CMD_MODULE) && (c->clientDurabilityInfo.module_cmd_blocking_offset != -1)) {
        blocking_repl_offset = c->clientDurabilityInfo.module_cmd_blocking_offset;
    } else if (c->cmd->flags & (CMD_READONLY | CMD_WRITE)) {
        blocking_repl_offset = c->db->dirty_repl_offset;
        getKeysResult result;
        initGetKeysResult(&result);
        int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
        keyReference *keys = result.keys;

        for (int i = 0; i < numkeys; i++) {
            sds keystr = objectGetVal(c->argv[keys[i].pos]);
            long long offset = durabilityPurgeAndGetUncommittedKeyOffset(keystr, c->db);
            if (offset > blocking_repl_offset) {
                blocking_repl_offset = offset;
            }
        }
        getKeysFreeResult(&result);
    }

    return blocking_repl_offset;
}

/**
 * Process a single command for consistent write blocking.
 */
static long long getSingleCommandBlockingOffsetForConsistentWrites(struct client *c) {
    serverAssert(isPrimaryDurabilityEnabled());

    if (!anyDurabilityProviderEnabled())
        return -1;

    long long blocking_repl_offset = -1;

    // we can't trust keyspace info if we have any dirty data
    if (IS_KEYSPACE_INFORMATIONAL(c->cmd) &&
        (listLength(server.durability.clients_waiting_ack) > 0 || hasUncommittedKeys() || isDurableFunctionStoreUncommitted())) {
        blocking_repl_offset = server.primary_repl_offset;
    } else if ((server.primary_repl_offset > server.durability.pre_call_replication_offset) || (server.also_propagate.numops > server.durability.pre_call_num_ops_pending_propagation)) {
        blocking_repl_offset = getSingleCommandBlockingOffsetForReplicatingCommand(c);
    } else {
        blocking_repl_offset = getSingleCommandBlockingOffsetForNonReplicatingCommand(c);
    }

    if (blocking_repl_offset <= server.durability.previous_acked_offset) {
        blocking_repl_offset = -1;
    }

    return blocking_repl_offset;
}

static void durabilitySetClientCmdFlags(client *c) {
    // Transaction wrapper commands, e.g., eval, exec, fcall, should not interfere with the
    // final classification of the transaction itself as read or write. Rather the commands
    // executed inside the transaction will define if it is read or write or none.
    if (isClientDoingTransaction(c)) return;
    if (c->cmd->flags & CMD_WRITE)
        c->clientDurabilityInfo.durability_flags |= DURABILITY_CLIENT_LAST_CMD_WRITE;
    else if (c->cmd->flags & CMD_READONLY)
        c->clientDurabilityInfo.durability_flags |= DURABILITY_CLIENT_LAST_CMD_READONLY;
}

/*=========================== Command hook functions ======================= */

/**
 * Record the starting replication offset of the command about to be executed.
 */
void beforeCommandTrackReplOffset(struct client *c) {
    if (!isPrimaryDurabilityEnabled()) return;

    durabilitySetClientCmdFlags(c);


    server.durability.pre_call_replication_offset = server.primary_repl_offset;
    server.durability.pre_call_num_ops_pending_propagation = server.also_propagate.numops;
}

static bool isClientBlockedByModule(struct client *c) {
    return c->flag.blocked &&
           c->bstate &&
           c->bstate->btype == BLOCKED_MODULE &&
           !moduleClientIsBlockedOnKeys(c);
}

/**
 * After processing a command, track the replication offset and update
 * the blocking offset for the command block.
 */
void afterCommandTrackReplOffset(client *c) {
    serverLog(LL_DEBUG, "afterCommandTrackReplOffset entered for command '%s'", c->cmd->declared_name);
    if (!isPrimaryDurabilityEnabled() || (c->flag.blocked && !isClientBlockedByModule(c)))
        return;

    long long current_cmd_blocking_offset = getSingleCommandBlockingOffsetForConsistentWrites(c);

    client *tracking_client = server.current_client ? server.current_client : c;

    if (current_cmd_blocking_offset > tracking_client->clientDurabilityInfo.current_command_repl_offset) {
        tracking_client->clientDurabilityInfo.current_command_repl_offset = current_cmd_blocking_offset;
    }

    handleDatabaseModification(c);
}

char *preScriptCmd(client *c) {
    if (!isDurabilityEnabled()) {
        return NULL;
    }

    if (shouldRejectCommandWithUncommittedData(c)) {
        return DURABILITY_DATA_UNAVAILABLE;
    }

    return NULL;
}

/**
 * Perform pre-processing before command execution for a given client.
 */
int preCommandExec(client *c) {
    c->clientDurabilityInfo.current_command_repl_offset = -1;
    c->clientDurabilityInfo.module_cmd_blocking_offset = -1;

    if (shouldRejectCommandWithUncommittedData(c)) {
        serverAssert(!(c->cmd->flags & CMD_WRITE));
        flagTransaction(c);
        addReplyError(c, DURABILITY_DATA_UNAVAILABLE);
        return CMD_FILTER_REJECT;
    }

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

    server.durability.pre_command_replication_offset = server.primary_repl_offset;
    return CMD_FILTER_ALLOW;
}

/**
 * Perform post-processing after command execution for a given client.
 */
void postCommandExec(client *c) {
    if (!isPrimaryDurabilityEnabled() || c->cmd == NULL || c->flag.multi) {
        return;
    }

    long long blocking_repl_offset = c->clientDurabilityInfo.current_command_repl_offset;

    if (server.primary_repl_offset > server.durability.pre_command_replication_offset && (c->cmd->flags & CMD_WRITE || isClientDoingTransaction(c)) && c->cmd->proc != syncCommand && c->cmd->proc != clusterCommand && c->cmd->proc != shutdownCommand) {
        blocking_repl_offset = server.primary_repl_offset;
    }

    if (blocking_repl_offset > server.durability.pre_command_replication_offset) {
        serverAssert(clientEligibleForResponseTracking(c));
    }

    processPendingUncommittedData(server.primary_repl_offset);

    blockClientAndMonitorsOnReplOffset(c, blocking_repl_offset);

    certifyPendingDeferredTasks();
}

/*================================= Lifecycle =============================== */

/**
 * Initialize the durability subsystem.
 */
void durabilityInit(void) {
    serverLog(LL_DEBUG, "Initializing durability subsystem");

    /* Initialize uncommitted keys pending data */
    uncommittedKeysInitPending();

    /* Have to init the handlers before using them. */
    initTaskTypes();
    server.durability.previous_acked_offset = -1;
    server.durability.clients_waiting_ack = listCreate();
    durableTaskInitLists();
    server.durability.clients_blocked = 0;
    server.durability.clients_unblocked = 0;
    server.durability.clients_disconnected_before_unblocking = 0;
    server.durability.read_responses_blocked = 0;
    server.durability.write_responses_blocked = 0;
    server.durability.other_responses_blocked = 0;
    server.durability.read_responses_unblocked = 0;
    server.durability.write_responses_unblocked = 0;
    server.durability.other_responses_unblocked = 0;
    server.durability.read_responses_blocked_cumulative_time_us = 0;
    server.durability.write_responses_blocked_cumulative_time_us = 0;
    server.durability.other_responses_blocked_cumulative_time_us = 0;

    /* Initialize function store blocking state */
    server.durability.all_dbs_dirty_in_current_cmd = false;
    server.durability.func_store_blocking_offset = -1;
    server.durability.processed_func_write_in_transaction = false;

    /* Register built-in durability providers (AOF) */
    registerBuiltinDurabilityProviders();
}

/**
 * Clean up the durability subsystem on server shutdown.
 */
void durabilityCleanup(void) {
    if (server.durability.clients_waiting_ack != NULL) {
        listRelease(server.durability.clients_waiting_ack);
        server.durability.clients_waiting_ack = NULL;
    }

    uncommittedKeysCleanupPending();

    /* Cleanup deferred tasks waiting for durability ack */
    durableTaskCleanupLists();

    /* Reset the durability provider registry so it can be re-initialized */
    resetDurabilityProviders();

    clearAllUncommittedKeys();
}

/**
 * Disconnect and free clients waiting for durability ack.
 */
static void freeClientsWaitingAck(const durable_t *durability) {
    listIter li;
    listNode *ln;
    listRewind(durability->clients_waiting_ack, &li);
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        freeClient(c);
    }
    listEmpty(durability->clients_waiting_ack);
}

/**
 * Reset primary state for the durability subsystem.
 */
static void durabilityResetPrimaryState(bool is_free_clients_needed) {
    if (listLength(server.durability.clients_waiting_ack) > 0) {
        if (is_free_clients_needed) {
            freeClientsWaitingAck(&server.durability);
        } else {
            unblockResponsesWithAckOffset(&server.durability, LLONG_MAX);
        }
        serverAssert(listLength(server.durability.clients_waiting_ack) == 0);
    }
    durableTaskEmptyLists();
}

/**
 * Clear the durability attributes specific to the primary.
 * Invoked when a primary node becomes a replica.
 */
void durabilityClearPrimaryState(void) {
    if (!isDurabilityEnabled()) return;
    durabilityResetPrimaryState(true);
}

/**
 * Generate INFO string for durability stats.
 */
sds genDurabilityInfoString(sds info) {
    if (!isDurabilityEnabled()) {
        info = sdscatprintf(info, "durability_enabled:0\r\n");
        return info;
    }

    info = sdscatprintf(info,
                        "durability_enabled:1\r\n"
                        "durability_read_blocked_count:%lld\r\n"
                        "durability_write_blocked_count:%lld\r\n"
                        "durability_clients_waiting_ack:%lu\r\n"
                        "durability_uncommitted_keys:%llu\r\n"
                        "durability_previous_acked_offset:%lld\r\n"
                        "durability_primary_repl_offset:%lld\r\n",
                        server.durability.read_responses_blocked,
                        server.durability.write_responses_blocked,
                        listLength(server.durability.clients_waiting_ack),
                        getNumberOfUncommittedKeys(),
                        server.durability.previous_acked_offset,
                        server.primary_repl_offset);

    return info;
}

/**
 * Reset related resources when enabling/disabling durability.
 */
void durabilityReset(void) {
    if (isDurabilityEnabled()) {
        server.durability.pre_command_replication_offset = server.primary_repl_offset;
        listIter li;
        listNode *ln;
        listRewind(server.clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            durabilityClientInit(c);
        }
    } else {
        if (iAmPrimary()) {
            durabilityResetPrimaryState(false);
        }
        clearAllUncommittedKeys();
    }
}
