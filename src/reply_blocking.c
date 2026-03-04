#include "reply_blocking.h"
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
void moduleNotifyKeyspaceEvent(int type, const char *event, robj *key, int dbid);

/*============================ Internal prototypes ========================= */
static void resetPreExecutionOffset(struct client *c);
static void trackCommandPreExecutionPosition(struct client *c);
static int unblockClientWaitingReplicaAck(struct client *c);
static bool clientEligibleForResponseTracking(client *c);
static void unblockFirstResponse(const struct client *c);
static int isBlockingNeededForOffset(const struct client *c, long long offset);
static void blockClientAndMonitorsOnReplOffset(struct client *c, long long blockingReplOffset);
static long long getSingleCommandBlockingOffsetForReplicatingCommand(client *c);
static long long getSingleCommandBlockingOffsetForNonReplicatingCommand(client *c);
static long long getSingleCommandBlockingOffsetForConsistentWrites(client *c);
static void syncReplicationResetPrimaryState(bool is_free_clients_needed);
static void postDurabilityAck(void);

// Track the number of commands awaiting propagation prior to executing a single command in call()
static int pre_call_num_ops_pending_propagation;

/*================================= Internal Data structures ======================== */

/**
 * Internal structure used to track replication offset and arguments needed in
 * executing task when offset has been acked by required number of replicas.
 */
typedef struct taskWaitingAck {
    int type; // Task type
    int64_t offset;
    void **argv;
} taskWaitingAck;

/**
 * Internal structure used to define all handlers for a task type
 */
typedef struct taskWaitingAckType {
    taskWaitingAck *(*createTask)(va_list);
    void (*destroyTask)(void *);
    void (*executeTask)(const taskWaitingAck *);
    void (*onClientDestroy)(void *);
} taskWaitingAckType;

static taskWaitingAckType taskTypes[DURABLE_TASK_TYPE_MAX];

/* Stub task type handlers  these will be replaced with real implementations
 * when the task notification system is fully wired up. */
static taskWaitingAck *stubCreateTask(va_list ap) { (void)ap; return NULL; }
static void stubDestroyTask(void *task) { if (task) zfree(task); }
static void stubExecuteTask(const taskWaitingAck *task) { (void)task; }
static void stubOnClientDestroy(void *task) { (void)task; }

void initTaskTypes(void) {
    for (int i = 0; i < DURABLE_TASK_TYPE_MAX; i++) {
        taskTypes[i].createTask = stubCreateTask;
        taskTypes[i].destroyTask = stubDestroyTask;
        taskTypes[i].executeTask = stubExecuteTask;
        taskTypes[i].onClientDestroy = stubOnClientDestroy;
    }
}

/*================================= Utility functions ======================== */

/**
 * Utility function to determine whether the durability flag has been enabled.
 */
int isSyncReplicationEnabled(void) {
    return server.durability.sync_replication_enabled;
}

/**
 * Utility function to determine whether the primary sync replication flag has been enabled.
 */
int isPrimarySyncReplicationEnabled(void) {
    return isSyncReplicationEnabled() && iAmPrimary();
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
 * If the client is currently waiting for replica acknowledgement,
 * mark it unblocked and reset the client flags.
 */
static int unblockClientWaitingReplicaAck(struct client *c) {
    if (c->clientDurabilityInfo.durable_blocked_client) {
        listNode *node = listSearchKey(server.durability.clients_waiting_replica_ack, c);
        if (node != NULL) {
            listDelNode(server.durability.clients_waiting_replica_ack, node);
            c->clientDurabilityInfo.durable_blocked_client = 0;
            return 1;
        }
    }
    return 0;
}

/**
 * Initialize the sync replication client attributes when client is created.
 */
void syncReplicationClientInit(client *c) {
    if (!isSyncReplicationEnabled()) {
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
 * Reset the client durable write attributes during a client clean-up.
 */
void syncReplicationClientReset(client *c) {
    if (unblockClientWaitingReplicaAck(c)) {
        server.durability.clients_disconnected_before_unblocking_on_sync_write++;
    }

    if (c->clientDurabilityInfo.blocked_responses != NULL) {
        listRelease(c->clientDurabilityInfo.blocked_responses);
        c->clientDurabilityInfo.blocked_responses = NULL;
    }

    if (c->clientDurabilityInfo.pending_notify_tasks != NULL) {
        listIter li;
        listNode *ln;
        listRewind(c->clientDurabilityInfo.pending_notify_tasks, &li);
        while ((ln = listNext(&li))) {
            taskWaitingAck *task = (taskWaitingAck *)listNodeValue(ln);
            if (task) {
                taskTypes[task->type].onClientDestroy(task);
            }
        }
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

    bool is_keyspace_informational_cmd = false;

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
 * Block the last response if it exists in the client output buffer.
 */
void blockLastResponseIfExist(const client *c, const long long blocked_offset) {
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
        listAddNodeTail(c->clientDurabilityInfo.blocked_responses, new_block);
    }
}

/**
 * Process the metrics of all commands blocked at a BlockedResponse while unblocking.
 */
static inline void processCmdMetrics(struct blockedResponse *br) {
    // TODO:merge NOTDONE
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
    serverAssert(isPrimarySyncReplicationEnabled());

    if (isBlockingNeededForOffset(c, blockingReplOffset)) {
        serverLog(LL_DEBUG, "client should be blocked at offset %lld, cmd=%s, is_write=%d",
                  blockingReplOffset, c->cmd->declared_name, (c->cmd->flags & CMD_WRITE) ? 1 : 0);
        blockLastResponseIfExist(c, blockingReplOffset);
        if (!c->clientDurabilityInfo.durable_blocked_client) {
            listAddNodeTail(server.durability.clients_waiting_replica_ack, c);
            c->clientDurabilityInfo.durable_blocked_client = 1;
            server.durability.clients_blocked_on_sync_write++;
        }
        replicationRequestAckFromReplicas();
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

/*================================= Task ACK system ========================= */

/**
 * Find and execute the tasks when 'consensus_ack_offset' is acked.
 */
static void executeTaskForReplicaAck(const long long consensus_ack_offset) {
    listIter li;
    listNode *ln;
    struct durable_t *durability = &server.durability;

    for (int i = 0; i < DURABLE_TASK_TYPE_MAX; i++) {
        listRewind(durability->tasks_waiting_replica_ack[i], &li);
        while ((ln = listNext(&li))) {
            taskWaitingAck *task = listNodeValue(ln);
            if (task->offset <= consensus_ack_offset) {
                taskTypes[i].executeTask(task);
                listDelNode(durability->tasks_waiting_replica_ack[i], ln);
            } else {
                break;
            }
        }
    }
}

/**
 * Helper method to update all pending tasks for replicas ACK.
 */
void updateAndCertifyPendingTasksForReplicasAck(void) {
    listIter li;
    listNode *ln;
    for (int i = 0; i < DURABLE_TASK_TYPE_MAX; i++) {
        listRewind(server.durability.pending_tasks_waiting_replica_ack[i], &li);
        while ((ln = listNext(&li))) {
            taskWaitingAck *task = listNodeValue(ln);
            serverAssert(task->offset == 0);
            task->offset = server.primary_repl_offset;
            if (task->type == DURABLE_KEYSPACE_NOTIFY_TASK) {
                moduleNotifyKeyspaceEvent(
                    /*type*/ (intptr_t)task->argv[0],
                    /*event*/ (char *)task->argv[1],
                    /*key*/ (robj *)task->argv[2],
                    /*dbid*/ (intptr_t)task->argv[3]);
            }
        }
        if (listLength(server.durability.pending_tasks_waiting_replica_ack[i]) > 0) {
            listJoin(server.durability.tasks_waiting_replica_ack[i], server.durability.pending_tasks_waiting_replica_ack[i]);
        }
        serverAssert(listLength(server.durability.pending_tasks_waiting_replica_ack[i]) == 0);
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
    listRewind(durability->clients_waiting_replica_ack, &li);
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
            if (unblockClientWaitingReplicaAck(c)) {
                server.durability.clients_unblocked_on_sync_write++;
            }
        }
        if (unblocked_responses) {
            putClientInPendingWriteQueue(c);
        }
    }

    executeTaskForReplicaAck(consensus_ack_offset);
}

/*================================= Post-ack handlers ======================= */

static void postDurabilityAck(void) {
    if (!isPrimarySyncReplicationEnabled()) {
        return;
    }

    durable_t *durability = &server.durability;
    const long long consensus_ack_offset = getDurabilityConsensusOffset();
    if (consensus_ack_offset <= durability->previous_acked_offset) {
        return;
    }

    durability->previous_acked_offset = consensus_ack_offset;
    unblockResponsesWithAckOffset(durability, consensus_ack_offset);
}

void notifyDurabilityProgress(void) {
    postDurabilityAck();
}

void postReplicaAck(void) {
    postDurabilityAck();
}

void postAofFsync(void) {
    if (!isPrimarySyncReplicationEnabled()) {
        return;
    }
    postDurabilityAck();
}

/*================================= Function Store Tracking ================== */

/* Renamed from amzCommands_isFunctionRWCommand */
bool isFunctionRWCommand(client *c) {
    return (c->argc > 0 && (!strcasecmp(objectGetVal(c->argv[0]), "FUNCTION"))) && !(c->argc > 1 && !strcasecmp(objectGetVal(c->argv[1]), "HELP"));
}

/* Renamed from amzCommands_isFunctionStoreRWCommand */
bool isFunctionStoreRWCommand(client *c) {
    return isFunctionRWCommand(c) || c->cmd->proc == fcallCommand || c->cmd->proc == fcallroCommand;
}

/* Renamed from amzDurableFunctions_isFunctionStoreUncommitted */
bool isDurableFunctionStoreUncommitted(void) {
    return server.durability.func_store_blocking_offset > server.durability.previous_acked_offset;
}

/* Renamed from amzDurableFunctions_handleUncommittedFunctionStore */
void handleUncommittedFunctionStore(void) {
    if (server.execution_nesting) {
        server.durability.processed_func_write_in_transaction = true;
    } else {
        server.durability.func_store_blocking_offset = server.primary_repl_offset;
    }
}

/* Renamed from amzDurableFunctions_getBlockingOffset */
long long getFuncStoreBlockingOffset(void) {
    return server.durability.func_store_blocking_offset;
}

/* Renamed from amzDurableFunctions_updateBlockingOffsetForWrite */
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
    } else if (commandModifiesFirstKeyOnly(c->cmd)) {
        int first = c->cmd->legacy_range_key_spec.bs.index.pos;
        handleUncommittedKeyForClient(c, c->argv[first], c->db);
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
            long long offset = syncReplicationPurgeAndGetUncommittedKeyOffset(keystr, c->db);
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
    serverAssert(isPrimarySyncReplicationEnabled());

    if (!anyDurabilityProviderEnabled())
        return -1;

    long long blocking_repl_offset = -1;

    if ((listLength(server.durability.clients_waiting_replica_ack) > 0 || hasUncommittedKeys() || isDurableFunctionStoreUncommitted())) {
        blocking_repl_offset = server.primary_repl_offset;
    } else if ((server.primary_repl_offset > server.durability.pre_call_replication_offset) || (server.also_propagate.numops > pre_call_num_ops_pending_propagation)) {
        blocking_repl_offset = getSingleCommandBlockingOffsetForReplicatingCommand(c);
    } else {
        blocking_repl_offset = getSingleCommandBlockingOffsetForNonReplicatingCommand(c);
    }

    if (blocking_repl_offset <= server.durability.previous_acked_offset) {
        blocking_repl_offset = -1;
    }

    return blocking_repl_offset;
}

/*=========================== Command hook functions ======================= */

/**
 * Record the starting replication offset of the command about to be executed.
 */
void beforeCommandTrackReplOffset(void) {
    if (!isPrimarySyncReplicationEnabled()) return;

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
    if (!isPrimarySyncReplicationEnabled() || (c->flag.blocked && !isClientBlockedByModule(c)))
        return;

    long long current_cmd_blocking_offset = getSingleCommandBlockingOffsetForConsistentWrites(c);

    client *tracking_client = server.current_client ? server.current_client : c;

    if (current_cmd_blocking_offset > tracking_client->clientDurabilityInfo.current_command_repl_offset) {
        tracking_client->clientDurabilityInfo.current_command_repl_offset = current_cmd_blocking_offset;
    }

    handleDatabaseModification(c);
}

char *preScriptCmd(client *c) {
    if (!isSyncReplicationEnabled()) {
        return NULL;
    }

    if (shouldRejectCommandWithUncommittedData(c)) {
        return SYNC_REPL_ACCESSED_DATA_UNAVAILABLE;
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
        addReplyError(c, SYNC_REPL_ACCESSED_DATA_UNAVAILABLE);
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
    if (!isPrimarySyncReplicationEnabled() || c->cmd == NULL || c->flag.multi) {
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

    updateAndCertifyPendingTasksForReplicasAck();
}

/*================================= Lifecycle =============================== */

/**
 * Initialize the durability datastructures.
 */
void syncReplicationInit(void) {
    serverLog(LL_DEBUG, "Initializing DKT");

    /* Initialize uncommitted keys pending data */
    uncommittedKeysInitPending();

    /* Have to init the handlers before using them. */
    initTaskTypes();
    server.durability.replica_offsets_size = 0;
    server.durability.replica_offsets = NULL;
    server.durability.previous_acked_offset = -1;
    server.durability.curr_db_scan_idx = 0;
    server.durability.clients_waiting_replica_ack = listCreate();
    for (int i = 0; i < DURABLE_TASK_TYPE_MAX; i++) {
        server.durability.tasks_waiting_replica_ack[i] = listCreate();
        server.durability.pending_tasks_waiting_replica_ack[i] = listCreate();
        listSetFreeMethod(server.durability.tasks_waiting_replica_ack[i],
                          taskTypes[i].destroyTask);
        listSetFreeMethod(server.durability.pending_tasks_waiting_replica_ack[i],
                          taskTypes[i].destroyTask);
    }
    server.durability.clients_blocked_on_sync_write = 0;
    server.durability.clients_unblocked_on_sync_write = 0;
    server.durability.clients_disconnected_before_unblocking_on_sync_write = 0;
    server.durability.read_responses_blocked_on_sync_write = 0;
    server.durability.write_responses_blocked_on_sync_write = 0;
    server.durability.other_responses_blocked_on_sync_write = 0;
    server.durability.read_responses_unblocked_on_sync_write = 0;
    server.durability.write_responses_unblocked_on_sync_write = 0;
    server.durability.other_responses_unblocked_on_sync_write = 0;
    server.durability.read_responses_blocked_on_sync_write_cumulative_time_us = 0;
    server.durability.write_responses_blocked_on_sync_write_cumulative_time_us = 0;
    server.durability.other_responses_blocked_on_sync_write_cumulative_time_us = 0;

    /* Initialize function store blocking state */
    server.durability.all_dbs_dirty_in_current_cmd = false;
    server.durability.func_store_blocking_offset = -1;
    server.durability.processed_func_write_in_transaction = false;

    /* Register built-in durability providers (replica + AOF) */
    registerBuiltinDurabilityProviders();
}

/**
 * Utility function to release buffer used for replica offsets.
 */
static void releaseReplicaOffsetBuffer(void) {
    server.durability.replica_offsets_size = 0;
    zfree(server.durability.replica_offsets);
    server.durability.replica_offsets = NULL;
}

/**
 * Clean up the sync replication datastructures on server shutdown.
 */
void syncReplicationCleanup(void) {
    releaseReplicaOffsetBuffer();

    if (server.durability.clients_waiting_replica_ack != NULL) {
        listRelease(server.durability.clients_waiting_replica_ack);
        server.durability.clients_waiting_replica_ack = NULL;
    }

    uncommittedKeysCleanupPending();

    // cleanup tasks waiting for replica ACK
    for (int i = 0; i < DURABLE_TASK_TYPE_MAX; i++) {
        listRelease(server.durability.tasks_waiting_replica_ack[i]);
        server.durability.tasks_waiting_replica_ack[i] = NULL;
        listRelease(server.durability.pending_tasks_waiting_replica_ack[i]);
        server.durability.pending_tasks_waiting_replica_ack[i] = NULL;
    }

    /* Reset the durability provider registry so it can be re-initialized */
    resetDurabilityProviders();

    clearAllUncommittedKeys();
}

/**
 * Utility function to disconnect and free clients waiting for replica ACK.
 */
static void freeClientsWaitingAck(const durable_t *durability) {
    listIter li;
    listNode *ln;
    listRewind(durability->clients_waiting_replica_ack, &li);
    while ((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        freeClient(c);
    }
    listEmpty(durability->clients_waiting_replica_ack);
}

/**
 * Reset primary state for synchronous replication.
 */
static void syncReplicationResetPrimaryState(bool is_free_clients_needed) {
    releaseReplicaOffsetBuffer();

    if (listLength(server.durability.clients_waiting_replica_ack) > 0) {
        if (is_free_clients_needed) {
            freeClientsWaitingAck(&server.durability);
        } else {
            unblockResponsesWithAckOffset(&server.durability, LLONG_MAX);
        }
        serverAssert(listLength(server.durability.clients_waiting_replica_ack) == 0);
    }
    for (int i = 0; i < DURABLE_TASK_TYPE_MAX; i++) {
        listEmpty(server.durability.tasks_waiting_replica_ack[i]);
        listEmpty(server.durability.pending_tasks_waiting_replica_ack[i]);
    }
}

/**
 * Clear the sync replication attributes specific to the primary.
 * Invoked when a master node becomes a replica.
 */
void syncReplicationClearPrimaryState(void) {
    if (!isSyncReplicationEnabled()) return;
    syncReplicationResetPrimaryState(true);
}

/**
 * Generate INFO string for sync replication stats.
 */
sds genSyncReplicationInfoString(sds info) {
    if (!isSyncReplicationEnabled()) {
        info = sdscatprintf(info, "sync_replication_enabled:0\r\n");
        return info;
    }

    info = sdscatprintf(info,
                        "sync_replication_enabled:1\r\n"
                        "sync_repl_read_blocked_count:%lld\r\n"
                        "sync_repl_write_blocked_count:%lld\r\n"
                        "sync_repl_clients_waiting_ack:%lu\r\n"
                        "sync_repl_uncommitted_keys:%llu\r\n"
                        "sync_repl_previous_acked_offset:%lld\r\n"
                        "sync_repl_primary_repl_offset:%lld\r\n",
                        server.durability.read_responses_blocked_on_sync_write,
                        server.durability.write_responses_blocked_on_sync_write,
                        listLength(server.durability.clients_waiting_replica_ack),
                        getNumberOfUncommittedKeys(),
                        server.durability.previous_acked_offset,
                        server.primary_repl_offset);

    return info;
}

/**
 * Reset related resources when enabling/disabling synchronous replication.
 */
void syncReplicationReset(void) {
    if (isSyncReplicationEnabled()) {
        server.durability.pre_command_replication_offset = server.primary_repl_offset;
        listIter li;
        listNode *ln;
        listRewind(server.clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            syncReplicationClientInit(c);
        }
    } else {
        if (iAmPrimary()) {
            syncReplicationResetPrimaryState(false);
        }
        clearAllUncommittedKeys();
    }
}
