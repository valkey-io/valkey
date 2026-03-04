#include "reply_blocking.h"
#include "expire.h"
#include "module.h"
#include "server.h"
#include "zmalloc.h"
#include <assert.h>
#include <math.h>

#include "script.h"

/*============================ Internal prototypes ========================= */
static void resetPreExecutionOffset(struct client *c);
static void trackCommandPreExecutionPosition(struct client *c);
static int unblockClientWaitingReplicaAck(struct client *c);
static bool clientEligibleForResponseTracking(client *c);
static void unblockFirstResponse(const struct client *c);
static int isBlockingNeededForOffset(const struct client *c, long long offset);
static void blockClientAndMonitorsOnReplOffset(struct client *c, long long blockingReplOffset);
static void populateReplicaOffsets(long long *offsets, const size_t numReplicas);
static int offsetSorterDesc(const void *v1, const void *v2);
static long long getDurabilityConsensusOffset(void);
static void postDurabilityAck(void);
static bool replicaProviderIsEnabled(void);
static long long replicaProviderGetAckedOffset(void);
static bool aofProviderIsEnabled(void);
static long long aofProviderGetAckedOffset(void);
static unsigned long long getNumberOfUncommittedKeys(void);
static uint64_t uncommittedKeysHash(const void *key);
static int uncommittedKeysKeyCompare(const void *key1, const void *key2);
static const void *uncommittedKeyEntryGetKey(const void *entry);
static void uncommittedKeyEntryDestructor(void *entry);
static void addUncommittedKey(sds key,long long offset, hashtable *uncommittedKeys);
static void uncommittedKeysCleanupScanCallback(void *privdata, void *entry);
static void pendingUncommittedKeyDestructor(void *entry);
static void handleDatabaseModification(client *c);
static int isSingleCommandAccessingUncommittedKeys(const serverDb *db, struct serverCommand *cmd, robj **argv, int argc);
static int isAccessingUncommittedData(client *c);
static bool shouldRejectCommandWithUncommittedData(client *c);
static long long getSingleCommandBlockingOffsetForReplicatingCommand(client *c);
static long long getSingleCommandBlockingOffsetForNonReplicatingCommand(client *c);
static long long getSingleCommandBlockingOffsetForConsistentWrites(client *c);
static void syncReplicationResetPrimaryState(bool is_free_clients_needed);

/*================================= Internal Data structures ======================== */
typedef struct uncommittedKeyEntry {
    sds key;
    long long offset;
} uncommittedKeyEntry;

typedef struct uncommittedKeyCleanupCtx {
    hashtable *ht;
    long long acked_offset;
    unsigned long long *scan_count;
} uncommittedKeyCleanupCtx;

static hashtableType uncommittedKeysHashtableType = {
    .entryGetKey = uncommittedKeyEntryGetKey,
    .hashFunction = uncommittedKeysHash,
    .keyCompare = uncommittedKeysKeyCompare,
    .entryDestructor = uncommittedKeyEntryDestructor,
};


bool amzCommands_isFunctionRWCommand(client *c) {
    return (c->argc > 0 && (!strcasecmp(objectGetVal(c->argv[0]), "FUNCTION"))) && !(c->argc > 1 && !strcasecmp(objectGetVal(c->argv[1]), "HELP"));
}

bool amzCommands_isFunctionStoreRWCommand(client *c) {
    return amzCommands_isFunctionRWCommand(c) || c->cmd->proc == fcallCommand || c->cmd->proc == fcallroCommand;
}

// Track the number of commands awaiting propagation prior to executing a single command in call()
static int pre_call_num_ops_pending_propagation;

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
    taskWaitingAck* (*createTask)(va_list);
    void (*destroyTask)(void *);
    void (*executeTask)(const taskWaitingAck *);
    // Callback method when the client initiated this task is destroyed
    void (*onClientDestroy)(void *);
} taskWaitingAckType;

static taskWaitingAckType taskTypes[AMZ_TASK_TYPE_MAX];
void initTaskTypes(void);

/**
 * Below are the data structures used to buffer intermediate dirty keys/DBs for multi-command
 * blocks including MULTI/EXEC and Lua script. As we execute the individual commands in the
 * transaction, we don't know the final replication offset so we store the updated keys and DBs
 * in afterCommandTrackReplOffset(), and process them in postCommandExec() after the entire transaction is
 * propagated to the replication buffer.
 *
 * Note: here it is impossible to skip processing the buffered keys/DBs at the end of each command
 * because the command processing is single-threaded and atomic, so afterCommandTrackReplOffset() always
 * get invoked after call() even if call() fails with some error.
 * See processCommand() implementation in server.c.
 */
typedef struct pendingUncommittedKey {
    robj *key;
    hashtable *uncommitted_keys;
} pendingUncommittedKey;

// Track the list of pending uncommitted keys for an ongoing multi-command block
// such as a MULTI/EXEC or Lua.
static list *pending_uncommitted_keys;

// Track the list of pending uncommitted databases for an ongoing multi-command block
// such as MULTI/EXEC or Lua
static list *pending_uncommitted_dbs;

static bool all_dbs_dirty_in_current_cmd;

/*================================= Utility functions ======================== */
static void pendingUncommittedKeyDestructor(void *entry) {
    if (entry == NULL) return;
    pendingUncommittedKey *uk = entry;
    if (uk->key != NULL) decrRefCount(uk->key);
    zfree(uk);
}

/**
 * Utility function to determine whether the durability flag has been enabled.
 * return       1 if durability is enabled, 0 otherwise.
 */
int isSyncReplicationEnabled(void) {
    return server.durability.sync_replication_enabled;
}

/**
 * Utility function to determine whether the primary sync replication flag has been enabled.
 * return       1 if durability is enabled and we are a primary, 0 otherwise.
 */
int isPrimarySyncReplicationEnabled(void) {
    return isSyncReplicationEnabled() && iAmPrimary();
}

/**
 * TODO: this needs to be replaced by an interface w/ durable replication
 * to tell us when we've achieved consensus via raft.
 * Using 2 as default for POC.
 */
static unsigned replicaAcksForConsensus(void) {
    return 0;
}

/**
 * Utility function to sort offsets in descending order
 * @v1    Pointer to long long representing the first offset
 * @v2    Pointer to long long representing the second offset
 * returns    -ve  if first offset > second offset
 *            zero if both offsets are equal
 *            +ve  if first offset < second offset
 */
static int offsetSorterDesc(const void *v1, const void *v2) {
    const long long *a = v1;
    const long long *b = v2;

    return (*b - *a);
}

static uint64_t uncommittedKeysHash(const void *key) {
    const sds keystr = (const sds)key;
    return hashtableGenHashFunction(keystr, sdslen(keystr));
}

static int uncommittedKeysKeyCompare(const void *key1, const void *key2) {
    const sds s1 = (const sds)key1;
    const sds s2 = (const sds)key2;
    return sdslen(s1) != sdslen(s2) || memcmp(s1, s2, sdslen(s1));
}

static const void *uncommittedKeyEntryGetKey(const void *entry) {
    return ((const uncommittedKeyEntry *)entry)->key;
}

static void uncommittedKeyEntryDestructor(void *entry) {
    if (entry == NULL) return;
    uncommittedKeyEntry *uke = entry;
    sdsfree(uke->key);
    zfree(uke);
}

static unsigned long long getNumberOfUncommittedKeys(void) {
    unsigned long long num_uncommitted_keys = 0;
    for (int i = 0; i < server.dbnum; i++) {
        if (server.db[i] != NULL) {
            num_uncommitted_keys += hashtableSize(server.db[i]->uncommitted_keys);
        }
    }
    return num_uncommitted_keys;
}

unsigned long long getUncommittedKeysCleanupTimeLimit(unsigned long long num_uncommitted_keys) {
    // If we have uncommitted keys, then the time limit for the clean-up is proportional
    // to it up to the configured cleanup_time_limit_ms. The upper threshold is 1 million
    // dirty keys as that will occupy 30MB+ of memory for a typical key of 10-20 Bytes.
    unsigned long long time_limit_ms = 1;
    if (num_uncommitted_keys > 0) {
        time_limit_ms = ceil(server.durability.keys_cleanup_time_limit_ms * MIN(1, (double)(num_uncommitted_keys / 1000000.0)));
    }
    return time_limit_ms;
}

/*================================= Replica offset management =============== */

/**
 * Populates the offset of each replica. If the replica is offline,
 * then the function places a ZERO for its entry.
 * @offsets        The array that needs to be filled in. Function assumes that proper memory has been allocated for it.
 * @numReplicas    The size of the offsets array that needs to be filled in.
 */
//TODO:merge DONE
static void populateReplicaOffsets(long long *offsets, const size_t numReplicas) {
    memset(offsets, 0, sizeof(long long) * numReplicas);

    // iterate the replicas to get the offset they have reached.
    listIter li;
    listRewind(server.replicas, &li);

    listNode *ln = listNext(&li);
    for (unsigned i = 0; i < numReplicas && ln != NULL; ln = listNext(&li), i++) {
        const client *replica = listNodeValue(ln);
        serverAssert(replica->repl_data);
        if (replica->repl_data->repl_state == REPLICA_STATE_ONLINE) {
            offsets[i] = replica->repl_data->repl_ack_off;
        }
    }
}

/**
 * This function loops through all the replica's offset, finding the max offset that the required replicas have acknowledged.
 * In case the required replicas exceeds the number of replicas connected, the function will return 0 indicating the offset
 * is not reached by sufficient amount of replicas.
 *
 * @param numAcksNeeded The number of replicas that need to acknowledge the offset.
 * @returns The offset that requested replicas have reached.
 *          In absence of required replicas, the primary offset is returned.
 *          If there is not enough number of replicas connected, return -1.
 */
 //TODO:merge DONE
long long getConsensusOffset(const unsigned long numAcksNeeded) {
    const unsigned long numReplicas = listLength(server.replicas);

    //TODO:aof i need to change this to look at our durabilityProviders
    if (numAcksNeeded == 0) {
        // If no ack is needed, then the consensus offset is the one primary is at.
        return server.primary_repl_offset;
    }

    // If the number of connected replicas is less than the number of required replicas,
    // return -1 because we don't have enough number of replicas for the ACK.
    if (numReplicas < numAcksNeeded) {
        return -1;
    }
    // TODO:aof size stuff?
    long long replica_offsets[numReplicas];

    populateReplicaOffsets(replica_offsets, numReplicas);

    // don't bother sorting if there is only one replica.
    if (numReplicas > 1) {
        qsort(replica_offsets, numReplicas, sizeof(long long), offsetSorterDesc);
    }

    // get the Kth element
    return replica_offsets[numAcksNeeded - 1];
}

/**
 * Returns true if local AOF fsync progress should be considered for durability.
 */
static bool isAofLocalAckRequired(void) {
    return server.aof_state != AOF_OFF && server.aof_fsync == AOF_FSYNC_ALWAYS;
}

/*================================= Durability Provider System =============== */

/* Provider registry: static array of registered providers */
static durabilityProvider *durability_providers[MAX_DURABILITY_PROVIDERS];
static int num_durability_providers = 0;

/**
 * Register a durability provider. Providers are checked in registration order.
 * The overall durability consensus is the MIN (AND) of all enabled providers.
 */
void registerDurabilityProvider(durabilityProvider *provider) {
    serverAssert(num_durability_providers < MAX_DURABILITY_PROVIDERS);
    durability_providers[num_durability_providers++] = provider;
    serverLog(LL_NOTICE, "Registered durability provider: %s", provider->name);
}

/**
 * Unregister a durability provider by pointer.
 */
void unregisterDurabilityProvider(durabilityProvider *provider) {
    for (int i = 0; i < num_durability_providers; i++) {
        if (durability_providers[i] == provider) {
            /* Shift remaining providers down */
            for (int j = i; j < num_durability_providers - 1; j++) {
                durability_providers[j] = durability_providers[j + 1];
            }
            num_durability_providers--;
            serverLog(LL_NOTICE, "Unregistered durability provider: %s", provider->name);
            return;
        }
    }
}


bool anyDurabilityProviderEnabled(void) {
    for (int i = 0; i < num_durability_providers; i++) {
        if (durability_providers[i]->isEnabled()) return true;
    }
    return false;
}

void notifyDurabilityProgress(void) {
    postDurabilityAck();
}

/* ---- Built-in Replica Provider ---- */

static bool replicaProviderIsEnabled(void) {
    return replicaAcksForConsensus() > 0;
}

static long long replicaProviderGetAckedOffset(void) {
    long long offset = getConsensusOffset(replicaAcksForConsensus());
    /* If there are no connected replicas, return -1 to indicate
     * this provider cannot make progress. */
    return offset;
}

durabilityProvider builtinReplicaProvider = {
    .name = "replica",
    .isEnabled = replicaProviderIsEnabled,
    .getAckedOffset = replicaProviderGetAckedOffset,
};

/* ---- Built-in AOF Provider ---- */

static bool aofProviderIsEnabled(void) {
    return isAofLocalAckRequired();
}

static long long aofProviderGetAckedOffset(void) {
    /* Use fsynced_reploff_pending directly instead of fsynced_reploff.
     * When async AOF flushing is used (IO threads), fsynced_reploff_pending
     * is updated by the IO thread upon fsync completion, but fsynced_reploff
     * is only updated in the next beforeSleep() iteration. Using the pending
     * value ensures we see the most up-to-date fsync progress immediately. */
    long long fsynced_offset = atomic_load_explicit(&server.fsynced_reploff_pending, memory_order_relaxed);
    /* Handle the case where AOF is enabled but no data has been fsynced yet
     * (fsynced_reploff_pending is 0 initially). In that case, use fsynced_reploff
     * if it's been properly initialized. */
    if (fsynced_offset == 0 && server.fsynced_reploff > 0) {
        fsynced_offset = server.fsynced_reploff;
    }
    return fsynced_offset;
}

durabilityProvider builtinAofProvider = {
    .name = "aof",
    .isEnabled = aofProviderIsEnabled,
    .getAckedOffset = aofProviderGetAckedOffset,
};

/**
 * Register the built-in durability providers. Called from syncReplicationInit().
 */
static void registerBuiltinDurabilityProviders(void) {
    /* Only register if not already registered (idempotent) */
    if (num_durability_providers == 0) {
        registerDurabilityProvider(&builtinReplicaProvider);
        registerDurabilityProvider(&builtinAofProvider);
    }
}

/**
 * Returns the durability consensus offset by iterating all registered
 * providers and returning the MIN of all enabled providers' acknowledged
 * offsets (AND semantics: all must acknowledge).
 */
static long long getDurabilityConsensusOffset(void) {
    long long consensus = server.primary_repl_offset;
    bool any_enabled = false;

    for (int i = 0; i < num_durability_providers; i++) {
        durabilityProvider *p = durability_providers[i];
        if (!p->isEnabled()) continue;
        any_enabled = true;
        long long offset = p->getAckedOffset();
        /* Skip providers that return -1 (e.g. replica provider with insufficient replicas)
         * only if there are no replicas connected at allallow other providers to drive progress. */
        if (offset == -1) {
            /* For backward compat: if this is the replica provider and there are
             * no replicas, skip it so other providers can drive durability. */
            if (p == &builtinReplicaProvider && listLength(server.replicas) == 0) {
                continue;
            }
            /* Otherwise, -1 means the provider can't make progress  use it as-is
             * to block consensus advancement. */
            return -1;
        }
        if (offset < consensus) consensus = offset;
    }

    return any_enabled ? consensus : server.primary_repl_offset;
}

/*================================= Client management ======================== */

/**
 * Reset the pre-execution offset fields.
 */
//TODO:merge done
static void resetPreExecutionOffset(struct client *c) {
    c->clientDurabilityInfo.offset.recorded = false;
    c->clientDurabilityInfo.offset.reply_block = NULL;
    c->clientDurabilityInfo.offset.byte_offset = 0;
}

/**
 * Utility function to track the pre-execution position in the client reply COB. The given client can be either
 * a normal client (or a monitor client)
 * For a normal client, this position is the byte position in the COB prior to command execution. The response
 * generated from executing the next valkey command comes after this position.
 * (For a monitor client, this position is the byte position in the COB prior to command replication. The command
 * will be replicated after this position.)
 */
//TODO:merge done
static void trackCommandPreExecutionPosition(struct client *c) {
    // There can be cases when the client gets blocked by other mechanisms such as slot migration
    // after we tracked the command's pre-execution position when the reply buffer is non-empty.
    // Later on when the client unblocks, the reply buffer can get flushed to the client so the
    // previously tracked pre-execution reply position is no longer valid. In order to address that,
    // here we reset the pre-execution position of the command unconditionally.
    resetPreExecutionOffset(c);
    list *reply = c->reply;
    int bufpos = c->bufpos;

    if (reply != NULL && listLength(reply) > 0) {
        listNode *last_reply_block = listLast(reply);
        c->clientDurabilityInfo.offset.reply_block = last_reply_block;
        c->clientDurabilityInfo.offset.byte_offset = ((clientReplyBlock *)listNodeValue(last_reply_block))->used;
    } else if (bufpos > 0) {
        // We are only tracking the client reply block and we don't need to
        // take ownership of the pointer, so there is no need to free it
        c->clientDurabilityInfo.offset.reply_block = NULL;
        c->clientDurabilityInfo.offset.byte_offset = bufpos;
    }
    c->clientDurabilityInfo.offset.recorded = true;
}

/**
 * If the client is currently waiting for replica acknowledgement,
 * mark it unblocked and reset the client flags.
 * This involves us removing the client from the clients_waiting_replica_ack list,
 * and mark the client as unblocked for sync replication.
 *
 * @param c The client
 * @return 1 if the client is successfully marked unblocked, 0 otherwise
 */
//TODO:merge done
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
 * Initialize the sync replication client attributes when client is created
 */
//TODO:merge done
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
 * This method is invoked when a client is freed.
 */
//TODO:merge done
void syncReplicationClientReset(client *c) {
    // Free this client from the clients_waiting_replica_ack list and emit a metric on
    // how many clients are disconnected before the response gets flushed/unblocked.
    if (unblockClientWaitingReplicaAck(c)) {
        server.durability.clients_disconnected_before_unblocking_on_sync_write++;
    }

    if (c->clientDurabilityInfo.blocked_responses != NULL) {
        listRelease(c->clientDurabilityInfo.blocked_responses);
        c->clientDurabilityInfo.blocked_responses = NULL;
    }

     // Notify all pending tasks about the client's destruction
    if (c->clientDurabilityInfo.pending_notify_tasks != NULL) {
        listIter li;
        listNode *ln;
        listRewind(c->clientDurabilityInfo.pending_notify_tasks, &li);
        while((ln = listNext(&li))) {
            taskWaitingAck *task = (taskWaitingAck*)listNodeValue(ln);
            if (task) {
                taskTypes[task->type].onClientDestroy(task);
            }
        }
        // Now we can safety release the entire list of pending notification tasks
        listRelease(c->clientDurabilityInfo.pending_notify_tasks);
        c->clientDurabilityInfo.pending_notify_tasks = NULL;
    }

    resetPreExecutionOffset(c);
    c->clientDurabilityInfo.current_command_repl_offset = -1;
    c->clientDurabilityInfo.module_cmd_blocking_offset = -1;
}

/**
 * Determines if a client is doing a transaction or not. This applies to either
 * MULTI/EXEC or scripts
 */
//TODO:merge done
static bool isClientDoingTransaction(client *c) {
    return c->cmd->proc == execCommand || IS_SCRIPT_CALL_CMD(c->cmd);
}

/**
 * Returns true if the client is eligible for keyspace tracking
 * on a primary node.
 */
//TODO:merge NOTDONE
static bool clientEligibleForResponseTracking(client *c) {
    serverAssert(iAmPrimary());

    if (c->cmd == NULL) return false;

    //TODO:aof deal with informational commands.
    bool is_keyspace_informational_cmd = false;

    // Administrative commands that are not keyspace informational nor
    // write commands are not eligible for response tracking/blocking.
    if ((c->cmd->flags & CMD_ADMIN) && !(c->cmd->flags & CMD_WRITE) && !is_keyspace_informational_cmd) {
        return false;
    }

    return ((c->cmd->flags & (CMD_WRITE | CMD_READONLY)) // Read or write command
        || isClientDoingTransaction(c) // Transaction command
        || is_keyspace_informational_cmd // Informational command
        || amzCommands_isFunctionStoreRWCommand(c));
}

/**
 * Check if we only allow client to receive up to a certain
 * position in the client reply buffer
 */
//TODO:merge done
inline bool isClientReplyBufferLimited(client *c) {
    return c->clientDurabilityInfo.blocked_responses != NULL &&
           listLength(c->clientDurabilityInfo.blocked_responses) > 0;
}

/*================================= Response blocking ======================= */

/**
 * Block the last response if it exists in the client output buffer
 * @param c The client to block the last response in the COB
 * @param blocked_offset The replication offset to block on
 */
//TODO:merge done
void blockLastResponseIfExist(const client *c, const long long blocked_offset) {
    // We must have called the pre-hook to track COB position
    serverAssert(c->clientDurabilityInfo.offset.recorded);

    // Flag to indicate whether there is new response added in client output
    // buffer
    bool has_new_response = false;
    listNode *disallowed_reply_block =
        c->clientDurabilityInfo.offset.reply_block;
    size_t disallowed_byte_offset =
        c->clientDurabilityInfo.offset.byte_offset;

    // Track the starting position of the blocked response in the client COB
    if (disallowed_reply_block == NULL) {
        // The end of last response was in the initial 16KB buffer
        if ((size_t)c->bufpos > disallowed_byte_offset) {
            // We are not at the end of the 16KB initial buffer
            has_new_response = true;
        } else if (listLength(c->reply) > 0) {
            // We were at the end of the 16KB initial buffer and need to spill
            // over to start our response from the first byte at the reply block
            has_new_response = true;
            disallowed_byte_offset = 0;
            disallowed_reply_block = listFirst(c->reply);
        }
    } else {
        // The end of the previous response is in the client reply list
        const clientReplyBlock *last_reply_block = listNodeValue(disallowed_reply_block);
        if (last_reply_block->used > disallowed_byte_offset) {
            // More data comes after the last reply in the same reply block
            has_new_response = true;
        } else if (disallowed_reply_block->next != NULL) {
            // No more data comes after the last reply and we start from the next reply block
            has_new_response = true;
            disallowed_byte_offset = 0;
            disallowed_reply_block = disallowed_reply_block->next;
        }
    }

    // If the command outputs new response, create blockedResponse object and
    // add it into linkedlist to block it.
    if (has_new_response) {
        blockedResponse *new_block = zcalloc(sizeof(blockedResponse));
        new_block->primary_repl_offset = blocked_offset;
        new_block->disallowed_byte_offset = disallowed_byte_offset;
        new_block->disallowed_reply_block = disallowed_reply_block;
        listAddNodeTail(c->clientDurabilityInfo.blocked_responses, new_block);
    }
}

/**
 * Process the metrics of all commands blocked at a BlockedResponse while unblocking
 * @param br The Node at which commands are blocked.
 */
static inline void processCmdMetrics(struct blockedResponse *br){
    // TODO:merge NOTDONE
}

/**
 * Unblocks the first response in the client's blocked responses list
 * @param c the client to unblock the first response for.
 */
//TODO:merge done
static void unblockFirstResponse(const client *c) {
    serverAssert(c->clientDurabilityInfo.blocked_responses != NULL);
    if (listLength(c->clientDurabilityInfo.blocked_responses) > 0) {
        listNode *first = listFirst(c->clientDurabilityInfo.blocked_responses);
        processCmdMetrics(listNodeValue(first));
        listDelNode(c->clientDurabilityInfo.blocked_responses, first);
    }
}

/**
 * Determines if we need to block on a given replication offset for a given client
 * @param c Client
 * @param offset The replication offset we are checking
 * @return 0 if we don't need to block at the specified offset, and 1 if we do.
 */
//TODO:merge done
static int isBlockingNeededForOffset(const client *c, const long long offset) {
    // If the blocking offset is -1, don't block.
    if (offset == -1 || anyDurabilityProviderEnabled() == 0) {
        return 0;
    }

    // If there are no blocked responses previously, we always want to block
    if (listLength(c->clientDurabilityInfo.blocked_responses) == 0)
        return 1;

    listNode *last_response = listLast(c->clientDurabilityInfo.blocked_responses);
    long long previous_offset = ((blockedResponse *)listNodeValue(last_response))->primary_repl_offset;
    return previous_offset < offset;
}

/**
 * Block a given client on the specified replication offset if applicable.
 * And clears the client's pre-execution byte offset fields so it won't carry
 * forward to the next command.
 *
 * @param c client whose response we will block.
 * @param blockingReplOffset The replication offset to block the client on
 */
//TODO:merge NOTDONE
void blockClientOnReplOffset(client *c, const long long blockingReplOffset) {
    serverAssert(isPrimarySyncReplicationEnabled());

    /* If needed, we block the client and put it into our list of clients
     * waiting for ack from slaves. */
    if (isBlockingNeededForOffset(c, blockingReplOffset)) {
        /* Track blocked command stats for debugging */
        serverLog(LL_DEBUG, "client should be blocked at offset %lld, cmd=%s, is_write=%d", 
                  blockingReplOffset, c->cmd->declared_name, (c->cmd->flags & CMD_WRITE) ? 1 : 0);
        blockLastResponseIfExist(c, blockingReplOffset);
        if (!c->clientDurabilityInfo.durable_blocked_client) {
            listAddNodeTail(server.durability.clients_waiting_replica_ack, c);
            c->clientDurabilityInfo.durable_blocked_client = 1;
            server.durability.clients_blocked_on_sync_write++;
        }
        //TODO:aof only request if we are waiting on replicas.
        replicationRequestAckFromReplicas();
    }

    // Now we have processed the client blocking information and tracked it,
    // we can reset the client durability attributes we are tracking for
    // the current command.
    resetPreExecutionOffset(c);
}

/**
 * Utility function to determine whether a command should be replicated to the list of monitors.
 * return       1 if the command is replicated, 0 otherwise.
 */
//TODO:merge done
static inline int isCommandReplicatedToMonitors(void) {
    return listLength(server.monitors) && !server.loading;
}

/**
 * Process the pending dirty keys/databases if needed, and block a give client as well as all
 * connected MONITOR clients on the specified replication offset.
 * Regarding the command issued by the given client, its response to the given client will be blocked,
 * and replication of such command to the monitors will be also blocked.
 *
 * @param c Client
 * @param blockingReplOffset The replication offset to block the client and the monitors on
 */
//TODO:merge done
static void blockClientAndMonitorsOnReplOffset(client *c, long long blockingReplOffset) {
    // Block the client that issues the command on the replication offset
    blockClientOnReplOffset(c, blockingReplOffset);

    if (isCommandReplicatedToMonitors()) {
        listNode *ln;
        listIter li;
        listRewind(server.monitors,&li);
        while((ln = listNext(&li))) {
            client *monitor = ln->value;
            // Block each monitor client based on the replication offset
            blockClientOnReplOffset(monitor, blockingReplOffset);
        }
    }
}

/**
 * Find and execute the tasks when 'consensus_ack_offset' is acked by number 
 * of replicas.
 */
//TODO:merge done
static void executeTaskForReplicaAck(const long long consensus_ack_offset) {
    listIter li;
    listNode *ln;
    struct durable_t *durability = &server.durability;

    for(int i=0 ;i<AMZ_TASK_TYPE_MAX; i++) {
        listRewind(durability->tasks_waiting_replica_ack[i], &li);
        while((ln = listNext(&li))) {
            taskWaitingAck *task = listNodeValue(ln);
            if (task->offset <= consensus_ack_offset) {
                // Execute the task and remove it from linkedlist.
                taskTypes[i].executeTask(task);
                listDelNode(durability->tasks_waiting_replica_ack[i], ln);
            } else {
                // Because the node in the linkedlist is sorted by offset
                // from low to high, if the offset of current node is not
                // acked, do not need to check the nodes after it.
                break;
            }
        }
    }
}

/**
 * Unblock responses and tasks of all blocked clients with a given consensus acked offset
 * This function traverses through all the clients that wait for replica ack, and unblock
 * all responses and tasks that has the required offset that is acknowledged by replicas.
 * If the max repl offset is acked, all blocked responses will be flushed.
 *
 * @param durability Durability object of the current primary
 * @param consensus_ack_offset Repl offset that have been acked by the required number of replicas
 */
//TODO:merge done
void unblockResponsesWithAckOffset(const durable_t *durability, const long long consensus_ack_offset) {
    serverLog(LL_DEBUG, "unblocking clients for consensus offset %lld,", consensus_ack_offset);
    // Traverses through all the clients that wait for replica ack
    listIter li, li_response;
    listNode *ln, *ln_response;
    listRewind(durability->clients_waiting_replica_ack, &li);
    blockedResponse *br = NULL;
    while ((ln = listNext(&li))) {
        client *c = ln->value;

        // For each client blocked, we go through all its blocked responses,
        // and unblock all the responses whose replication offset are
        // ACK'ed by the required number of replicas
        // If the max repl offset is acked, all blocked responses will be unblocked
        serverAssert(c->clientDurabilityInfo.blocked_responses != NULL);
        listRewind(c->clientDurabilityInfo.blocked_responses, &li_response);
        bool unblocked_responses = false;
        
        while((ln_response = listNext(&li_response))) {
            br = listNodeValue(ln_response);
            if(br->primary_repl_offset <= consensus_ack_offset) {
                unblockFirstResponse(c);
                if (unblocked_responses == false) {
                    unblocked_responses = true;
                }
            } else {
                // As soon as we encounter a client response that has the
                // required reply offset greater than the replicas ACK'ed offset,
                // we can break out of this loop because all replies that follows
                // has replication offset that is greater and can't be unblocked
                break;
            }
        }
        // If there are no more blocked responses for the client, we can safely
        // mark it unblocked entirely
        if (listLength(c->clientDurabilityInfo.blocked_responses) == 0) {
            if (unblockClientWaitingReplicaAck(c)) {
                server.durability.clients_unblocked_on_sync_write++;
            }
        }
        // Put client in pending write queue so responses can be flushed
        // to client if we have unblocked at least 1 response objects.
        if (unblocked_responses) {
            putClientInPendingWriteQueue(c);
        }
    }
    
    // Find and execute the registered tasks when offset is acked.
    executeTaskForReplicaAck(consensus_ack_offset);
}

//TODO:merge done
//TODO:merge amzDurableProcessClientsWaitingReplicasAck
//TODO:aof this looks weird
static void postDurabilityAck(void) {
    if (!isPrimarySyncReplicationEnabled()) {
        return;
    }

    durable_t *durability = &server.durability;
    const long long consensus_ack_offset = getDurabilityConsensusOffset();
    if (consensus_ack_offset <= durability->previous_acked_offset) {
        return;
    }

    // Update the previous acknowledged offset for durability
    durability->previous_acked_offset = consensus_ack_offset;

    // Unblock responses and keyspace notifications with consensus acked offset
    unblockResponsesWithAckOffset(durability, consensus_ack_offset);
}

/**
 * Checks if there are clients blocked that can be unblocked since
 * we received enough ACKs from replicas.
 */
//TODO:aof this looks weird
void postReplicaAck(void) {
    postDurabilityAck();
}

/**
 * Checks if there are clients blocked that can be unblocked since
 * local AOF fsync progressed.
 */
//TODO:aof this looks weird
void postAofFsync(void) {
    if (!isPrimarySyncReplicationEnabled()) {
        return;
    }
    postDurabilityAck();
}

/*================================= Key management ============================ */

/**
 * Mark a key as uncommitted at a particular replication offset for acknowledgement.
 *
 * @param key The name of the uncommitted key to mark
 * @param offset The replication offset to wait on the uncommitted key
 * @param uncommittedKeys The set of uncommitted keys
 */
//TODO:merge done
static void addUncommittedKey(const sds key, const long long offset, hashtable *uncommittedKeys) {
    uncommittedKeyEntry *entry = zmalloc(sizeof(*entry));
    entry->key = sdsdup(key);
    entry->offset = offset;

    void *existing = NULL;
    if (hashtableAddOrFind(uncommittedKeys, entry, &existing)) {
        return;
    }

    uncommittedKeyEntry *existing_entry = existing;
    existing_entry->offset = offset;
    sdsfree(entry->key);
    zfree(entry);
}

/**
 * function executed by hashtableScan for cleaning up uncommitted keys.
 * @param privdata
 * @param entry
 */
//TODO:merge done
static void uncommittedKeysCleanupScanCallback(void *privdata, void *entry) {
    uncommittedKeyCleanupCtx *ctx = privdata;
    uncommittedKeyEntry *uke = entry;
    if (uke->offset <= ctx->acked_offset) {
        hashtableDelete(ctx->ht, uke->key);
    }
    (*ctx->scan_count)++;
}

/**
 * Retrieve the uncommitted replication offset for a given key, purge the given
 * key from uncommitted keys set if the replication offset has been committed.
 * Pre-condition: valkey is currently a primary
 * @param key The key to retrieve the uncommitted replication offset
 * @param db The serverDB object
 * @return the ACK offset of the key if key is uncommitted, returns -1 otherwise.
 */
//TODO:merge done
long long syncReplicationPurgeAndGetUncommittedKeyOffset(const sds key, serverDb *db) {
    serverAssert(iAmPrimary());
    uncommittedKeyEntry *entry = NULL;
    if (!hashtableFind(db->uncommitted_keys, key, (void **)&entry)) {
        return -1;
    }

    long long key_offset = entry->offset;

    /**
     * If the replication offset of key has been properly acked by replicas,
     * then purge the key from the uncommitted keys set, and return -1
     * indicating the key has been committed.
     */
    if (key_offset <= server.durability.previous_acked_offset) {
        hashtableDelete(db->uncommitted_keys, key);
        return -1;
    }

    return key_offset;
}

/**
 * Utility method to handle a dirty key for a given client.
 * @param c The calling client. NULL if the key becomes dirty outside a client command (i.e. expiry/eviction)
 * @param key
 * @param db
 */
//TODO:merge done
void handleUncommittedKeyForClient(const client *c, robj *key, serverDb *db) {
    // If we are in the context of a MULTI/EXEC transaction or a script, mark the dirty key
    // pending so it can be properly recorded later on with the final replication offset.
    if ((c != NULL) && ((c->flag.multi) || scriptIsRunning())) {
        // If all DBs are already dirty, no need to track dirty keys
        if (all_dbs_dirty_in_current_cmd) return;
        if (pending_uncommitted_keys == NULL) {
            pending_uncommitted_keys = listCreate();
            listSetFreeMethod(pending_uncommitted_keys, pendingUncommittedKeyDestructor);
        }
        pendingUncommittedKey *dirty_key = (pendingUncommittedKey*)zmalloc(sizeof(pendingUncommittedKey));
        incrRefCount(key);
        dirty_key->key = key;
        dirty_key->uncommitted_keys = db->uncommitted_keys;
        listAddNodeTail(pending_uncommitted_keys, dirty_key);
    } else {
        // Otherwise, the key is updated outside of a transaction or a script, simply mark the key
        // dirty at the current primary_repl_offset
        addUncommittedKey(objectGetVal(key), server.primary_repl_offset, db->uncommitted_keys);
    }
}

static inline void handleDirtyDatabase(client *c, serverDb *db) {
    if ((c->flag.multi) || scriptIsRunning()) {
        // For multi-commands transaction, queue up the uncommitted DB for later.
        // If all the databases are already dirty, no need to do anything more
        if (all_dbs_dirty_in_current_cmd) return;
        if (db != NULL) {
            // Current database is dirty
            listAddNodeTail(pending_uncommitted_dbs, db);
        } else {
            // All databases are dirty.
            all_dbs_dirty_in_current_cmd = true;
            // Here we no longer need to track any dirty keys or databases as all DBs
            // will be dirty on the final offset of the command block
            listEmpty(pending_uncommitted_keys);
            listEmpty(pending_uncommitted_dbs);
        }
    } else {
        // For single command, simply mark the DB dirty at the current offset
        if (db != NULL) {
            db->dirty_repl_offset = server.primary_repl_offset;
        } else {
            // For FLUSHALL command, we track all the databases to be dirty
            for (int i = 0; i < server.dbnum; i++) {
                if (server.db[i] != NULL) {
                    server.db[i]->dirty_repl_offset = server.primary_repl_offset;
                }
            }
        }
    }
}

struct serverCommand *lookupCommandOrOriginalBySds(sds s) {
    struct serverCommand *cmd = lookupCommandBySdsLogic(server.commands, s);
    if (!cmd) cmd = lookupCommandBySdsLogic(server.orig_commands, s);

    return cmd;
}
/**
 * Get parameters for the SWAPDB command.
 * The optional permission_client allows for checking of a client's permission for swapdb.
 * Returns true if command would be executed.
 */
//TODO:merge NOTDONE ACL stuff do i need it?
bool amzSwapdbGetParams(robj **argv, int argc, int *id1_p, int *id2_p) {
    long long dbid1, dbid2;
    if (argc != 3) return false;
    if (server.cluster_enabled) return false;
    if (getLongLongFromObject(argv[1], &dbid1) != C_OK) return false;
    if (getLongLongFromObject(argv[2], &dbid2) != C_OK) return false;
    if (dbid1 < 0 || dbid1 >= server.dbnum) return false;
    if (dbid2 < 0 || dbid2 >= server.dbnum) return false;
    if (dbid1 == dbid2) return false;

    *id1_p = (int)dbid1;
    *id2_p = (int)dbid2;
    return true;
}

/**
 * Get parameters for the SELECT command.
 * The optional permission_client allows for checking of a client's permission for select.
 * Returns true if command would be executed.
 */
//TODO:merge NOTDONE ACL stuff do i need it?
bool amzSelectGetParams(robj **argv, int argc, client *permission_client, int *dbid_p) {
    int dbid;
    if (argc != 2) return false;
    if (getIntFromObject(argv[1], &dbid) != C_OK) return false;
    if (dbid < 0 || dbid >= server.dbnum) return false;

    *dbid_p = dbid;
    return true;
}

/**
 * Handle the client command which modifies entire redis databases by
 * tracking the uncommitted replication offset on a serverDb level.
 * This includes commands FLUSHDB, FLUSHALL, and SWAPDB
 * @param c Client
 */
//TODO:merge done
//TODO:aof ACL?
static void handleDatabaseModification(client *c) {
    if (c->cmd->proc == swapdbCommand && server.cluster_enabled == 0) {
        // For SWAPDB command, we track both the databases to be dirty
        int id1, id2;
        if (amzSwapdbGetParams(c->argv, c->argc, &id1, &id2)) {
            handleDirtyDatabase(c, server.db[id1]);
            handleDirtyDatabase(c, server.db[id2]);
        }
    } else if (c->cmd->proc == flushdbCommand) {
        // For FLUSHDB command, we track the current database to be dirty
        handleDirtyDatabase(c, c->db);
    } else if (c->cmd->proc == flushallCommand) {
        // For FLUSHALL command, we track all the databases to be dirty
        handleDirtyDatabase(c, NULL);
    }
}

/**
 * Clears all uncommitted DBs and keys that are properly acknowledged by
 * sufficient number of replicas and mark them no longer dirty.
 *
 * This method iterates through all the valkey databases and checks the
 * DB and all items tracked by the uncommitted_keys set for each, and
 * removes keys that are acknowledged by sufficient number of replicas.
 * It is applicable only to primary.
 */
//TODO:merge done
void clearUncommittedKeysAcknowledged(void) {
    if (!isPrimarySyncReplicationEnabled()) {
        return;
    }

    durable_t *durability = &server.durability;
    const int TIME_CHECK_INTERVAL = 100;
    unsigned long long scan_count = 0;

    // Determine the number of uncommitted keys. Return if there is none
    unsigned long long num_uncommitted_keys = getNumberOfUncommittedKeys();
    if (num_uncommitted_keys == 0) return;

    unsigned long long time_limit_ms = getUncommittedKeysCleanupTimeLimit(num_uncommitted_keys);
    unsigned long long start_time_ms = mstime();
    unsigned long long next_time_check = TIME_CHECK_INTERVAL;
    while (durability->curr_db_scan_idx < server.dbnum) {
        serverDb *db = server.db[durability->curr_db_scan_idx];
        if (db != NULL) {
            // Clear the database's dirty replication offset if it is acknowledged by replicas
            if (db->dirty_repl_offset <= server.durability.previous_acked_offset) {
                db->dirty_repl_offset = -1;
            }

            // In a time-bound fashion, clear the uncommitted keys if the required replication
            // offset has been acknowledged by replicas.
            if (hashtableSize(db->uncommitted_keys) > 0) {
                uncommittedKeyCleanupCtx ctx = {
                    .ht = db->uncommitted_keys,
                    .acked_offset = server.durability.previous_acked_offset,
                    .scan_count = &scan_count,
                };

                if (!db->scan_in_progress) {
                    db->uncommitted_keys_cursor = 0;
                    db->scan_in_progress = 1;
                }

                do {
                    db->uncommitted_keys_cursor =
                        hashtableScan(db->uncommitted_keys, db->uncommitted_keys_cursor, uncommittedKeysCleanupScanCallback, &ctx);

                    if (time_limit_ms > 0 && scan_count >= next_time_check) {
                        const unsigned long long cur_time_ms = mstime();
                        if (cur_time_ms - start_time_ms > time_limit_ms) {
                            // Stop the current scan, continue to do in the next run
                            return;
                        }
                        next_time_check += TIME_CHECK_INTERVAL;
                    }
                } while (db->uncommitted_keys_cursor != 0);
            }

            // Finish to DB scan.
            if (db->scan_in_progress) {
                db->scan_in_progress = 0;
            }
        }
        durability->curr_db_scan_idx++;
    }

    // If all databases have been scanned, reset curr_db_scan_idx to 0, and
    // exit the keys cleanup procedure.
    if (durability->curr_db_scan_idx == server.dbnum) {
        durability->curr_db_scan_idx = 0;
    }
}

/**
 * Initialize sync replication related fields for a database.
 * @param db database to initialize.
 */
//TODO:merge done
void syncReplicationInitDatabase(serverDb *db) {
    db->uncommitted_keys = hashtableCreate(&uncommittedKeysHashtableType);
    db->dirty_repl_offset = -1;
    db->uncommitted_keys_cursor = 0; //TODO:merge diverge
    db->scan_in_progress = 0;
}

/**
 * Utility function to clear all uncommitted keys for each database
 */
//TODO:merge NOTDONE
static void clearAllUncommittedKeys(void) {
    serverLog(LL_NOTICE, "Clearing all uncommitted keys for sync replication");
    for (int i = 0; i < server.dbnum; i++) {
        serverDb *db = server.db[i];
        if (db == NULL) continue;
        hashtableRelease(db->uncommitted_keys);
        //TODO:stop iter?
        syncReplicationInitDatabase(db);
    }
    server.durability.curr_db_scan_idx = 0;
}

/*========================== Command access validation ====================== */

/**
 * Determines if a single valkey command is trying to access an uncommitted key.
 * Returns 1 if so, 0 otherwise.
 */
//TODO:merge done
static int isSingleCommandAccessingUncommittedKeys(const serverDb *db, struct serverCommand *cmd, robj **argv, int argc) {
    // If the database has no uncommitted keys, return 0
    if (hashtableSize(db->uncommitted_keys) == 0) return 0;

    getKeysResult keysResult;
    initGetKeysResult(&keysResult);
    const int numKeys = getKeysFromCommand(cmd, argv, argc, &keysResult);
    const keyReference *keys = keysResult.keys;

    for (int i = 0; i < numKeys; i++) {
        const sds keyStr = objectGetVal(argv[keys[i].pos]);
        // Check if we are trying to access an uncommitted key
        if (hashtableFind(db->uncommitted_keys, keyStr, NULL)) {
            getKeysFreeResult(&keysResult);
            return 1;
        }
    }

    // Free the keys after done processing them
    getKeysFreeResult(&keysResult);
    return 0;
}

/**
 * Determine if there are uncommitted keys in the redis server or not
 * Returns 1 if there are uncommitted keys, 0 otherwise.
 */
//TODO:merge done
static inline int hasUncommittedKeys(void) {
    for (int i = 0; i < server.dbnum; i++) {
        if(server.db[i] && (hashtableSize(server.db[i]->uncommitted_keys) > 0))
            return 1;
    }
    return 0;
}

//TODO:aof MOVE TO SERVER
static long long func_store_blocking_offset = -1;

//TODO:merge NOTDONE
//TODO:merge RENAME
bool amzDurableFunctions_isFunctionStoreUncommitted(void) {
    return func_store_blocking_offset > server.durability.previous_acked_offset;
}

/**
 * Determine if a client is trying to access uncommitted keys.
 * Returns 1 if so, 0 otherwise.
 */
//TODO:merge NOTDONE
static int isAccessingUncommittedData(client *c) {
     // Informational command handling
    //TODO: handle INFORMATIONAL

    // Single command handling
    if (isSingleCommandAccessingUncommittedKeys(c->db, c->cmd, c->argv, c->argc)
        || (amzCommands_isFunctionStoreRWCommand(c) && amzDurableFunctions_isFunctionStoreUncommitted())) {
        return 1;
    }

    int ret_val = 0;
    // MULTI/EXEC transaction handling
    if ((c->flag.multi) && c->cmd->proc == execCommand) {
        // We need to track the current database the client is on
        serverDb *cur_db = c->db;
        // Check if the keys accessed are dirty or not
        for (int i = 0; i < c->mstate->count; i++) {
            multiCmd mc = c->mstate->commands[i];
            // If the current command is SELECT, then we need to switch
            // the database referenced by the client
            if (mc.cmd->proc == selectCommand) {
                int db_id;
                if (amzSelectGetParams(mc.argv, mc.argc, c, &db_id)) {
                    c->db = server.db[db_id];
                    continue;
                } else {
                    discardTransaction(c);
                    ret_val = 1;
                    break;
                }
            }
            if (isSingleCommandAccessingUncommittedKeys(c->db, mc.cmd, mc.argv, mc.argc)
                || (amzCommands_isFunctionStoreRWCommand(c) && amzDurableFunctions_isFunctionStoreUncommitted())) {
                discardTransaction(c);
                ret_val = 1;
                break;
            }
        }
        // At the end of pre-processing the MULTI/EXEC, we need to
        // restore the current database referenced by the client.
        c->db = cur_db;
    }
    return ret_val;
}

/**
 * Checks if we should reject a command that is accessing uncommitted data.
 * The command is allowed if:
 * 1. The command is an administrative command
 * 2. The command is issued by the primary (but the response might be blocked)
 * 3. The command is a read-only command
 * 4. The command is a write command but the primary is not operating as a primary
 * @param c
 * @return
 */
//TODO:merge NOTDONE
//TODO:keyspace informational
//TODO:module replication?
static bool shouldRejectCommandWithUncommittedData(client *c) {
    if (c->cmd == NULL // command is null
        || ((c->cmd->flags & CMD_ADMIN)) || c->flag.primary) {
        return false;
    }

    // If we are operating as a replica (after a failover)
    // trying to access dirty items.
    if ((!iAmPrimary()) && isAccessingUncommittedData(c)) {
        return true;
    }

    return false;
}

//TODO:move to SERVER
static bool processed_func_write_in_transaction = false;

void amzDurableFunctions_handleUncommittedFunctionStore(void) {
    if (server.execution_nesting) {
        // The master replication offset isn't updated until the transaction completes
        processed_func_write_in_transaction = true;
    } else {
        func_store_blocking_offset = server.primary_repl_offset;
    }
}

/*
 * Return true if the given command only modifies the first key.
 */
//TODO:MERGE NOT DONE
bool commandModifiesFirstKeyOnly(struct serverCommand *cmd) {
  //  return cmd->amz_info != NULL && cmd->amz_info->flags & REDIS_AMZ_CMD_FIRSTKEY_MOD && cmd->getkeys_proc == NULL;
  return false;
}

bool amzGetDbIdFromRobj(robj *obj, int *db_id) {
    if ((getIntFromObject(obj, db_id) != C_OK) || (*db_id < 0) || (*db_id >= server.dbnum)) {
        return false;
    }
    return true;
}

bool amzGetTargetDbIdForCopyCommand(int argc, robj **argv, int selected_dbid, int *target_dbid) {
    const int copy_command_optional_arg_start_index = 3;

    *target_dbid = selected_dbid;

    for (int j = copy_command_optional_arg_start_index; j < argc; j++) {
        if (!strcasecmp(objectGetVal(argv[j]), "replace")) {
            continue;
        } else if (!strcasecmp(objectGetVal(argv[j]), "db") && (argc > j + 1)) {
            /* Note the parsing here needs to perfectly match what we have in redis OSS for COPY.
             * The following command is considered OK by Redis OSS 6.2 so we can't return here, but
             * must continue to parse till the last db which is the one they effectivelly use.
             * COPY key1 key2 db 1 db 2 db 3 (This will use db 3)
             */
            if (!amzGetDbIdFromRobj(argv[j + 1], target_dbid)) {
                return false;   // parse failure
            }
            j++; // Consume additional argument
        } else {
            return false;   // parse failure
        }
    }
    return true;
}

/*========================== Command offset calculation ===================== */

/**
 * Process a single replicating command for consistent write blocking.
 *
 * @param c Client
 * @return The blocking replication offset or -1 if we're in a nested call and the replication
 *         offset has not been updated yet.
 */
//TODO:merge done
static long long getSingleCommandBlockingOffsetForReplicatingCommand(client *c) {
    /* We check the CMD_WRITE flag because there are three cases where the post-call replication offset can
    * be greater than the pre-call replication offset, but we don't consider the command to be replicating:
    *
    * 1. Top-level commands of transactions (e.g. EVAL, FCALL, EXEC). The necessary keys will already be
    *    added to the pending_uncommitted_keys array when the nested write commands are processed.
    * 2. Read commands that cause a write as a side-effect. The only case currently is passive expiration.
    *    The mutated keys are marked dirty by a separate hook and the keys accessed by the read command
    *    don't need to be marked dirty here.
    * 3. The pre-call hook was skipped so we have an outdated pre-call replication offset. This can happen
    *    when sync-replication is dynamically enabled on a primary with CONFIG SET or when a replica becomes
    *    a primary via REPLICAOF NO ONE.
    */
    if (!(c->cmd->flags & CMD_WRITE)) {
        return -1;
    }

    // If the command executed generated replication data, then this means the server data changed.
    // We need to mark the modified data as dirty and block the response to the client until the 
    // replica's replication offset is caught up to the current global offset.
    if (amzCommands_isFunctionRWCommand(c)) {
        // We mark the entire function store dirty if any FUNCTION write command occurs.
        amzDurableFunctions_handleUncommittedFunctionStore();
    } else if (commandModifiesFirstKeyOnly(c->cmd)) {
        int first = c->cmd->legacy_range_key_spec.bs.index.pos;
        handleUncommittedKeyForClient(c, c->argv[first], c->db);
    } else {
        getKeysResult result;
        initGetKeysResult(&result);
        int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
        keyReference *keys = result.keys;
        if (numkeys > 0) {
            // Support MOVE command: we need to mark the key as dirty in the destination DB
            if (c->cmd->proc == moveCommand) {
                // Track the destination DB ID for MOVE. Here we assume the command is properly
                // formed and there is no need to validate destination database ID argument
                // because it should have already been checked in moveCommand() method and
                // made modifications to the redis data set before reaching this point in code
                int dest_dbid = -1;
                if (getIntFromObject(c->argv[2],&dest_dbid) == C_ERR) {
                    // Unable to parse or invalid destination DB, simply don't block
                    getKeysFreeResult(&result);
                    return -1;
                }
                // handle MOVE command as it also need to mark the key in the destination DB as dirty
                handleUncommittedKeyForClient(c, c->argv[keys[0].pos], server.db[dest_dbid]);
            } else if (c->cmd->proc == copyCommand) {
                int dest_dbid;
                if (!amzGetTargetDbIdForCopyCommand(c->argc, c->argv, c->db->id, &dest_dbid)) {
                    // Unable to parse or invalid destination DB, simply don't block
                    getKeysFreeResult(&result);
                    return -1;
                }
                if (dest_dbid != c->db->id) {
                    handleUncommittedKeyForClient(c, c->argv[2], server.db[dest_dbid]);
                }
            }

            // Mark all the keys updated by the current command as dirty in the current DB 
            for (int i = 0; i < numkeys; i++) {
                handleUncommittedKeyForClient(c, c->argv[keys[i].pos], c->db);
            }
        }
        getKeysFreeResult(&result);
    }

    // If we're in a nested call, we do not update the blocking replication offset yet because
    // the replication data is not propagated until after the transaction completes.
    // Instead, the blocking repl offset will be finalized in amzDurablePostProcessCommand().
    if (!server.execution_nesting) {
        return server.primary_repl_offset;
    }

    return -1;
}

long long amzDurableFunctions_getBlockingOffset(void) {
    return func_store_blocking_offset;
}

/**
 * Process a single non-replicating command for consistent write blocking.
 *
 * @param c Client
 * @return The blocking replication offset or -1 if no blocking need.
 */
//TODO:merge done
static long long getSingleCommandBlockingOffsetForNonReplicatingCommand(client *c) {
        long long blocking_repl_offset = -1;

    if (amzCommands_isFunctionStoreRWCommand(c)) {
        // We block any FUNCTION/FCALL commands accessing a dirty function store.
        blocking_repl_offset = amzDurableFunctions_getBlockingOffset();
    } else if (IS_SCRIPT_CALL_READONLY_CMD(c->cmd)) {
        // We don't need to indiscriminately block on dirty keys specified in read-only script run commands.
        // We only block on ones that are actually accessed which is handled during the nested calls.
        return -1;
    } else if ((c->cmd->flags & CMD_MODULE) && (c->clientDurabilityInfo.module_cmd_blocking_offset != -1)) {
        // If it is a module command that set a blocking offset, we use it.
        blocking_repl_offset = c->clientDurabilityInfo.module_cmd_blocking_offset;
    } else if (c->cmd->flags & (CMD_READONLY | CMD_WRITE)) {
        // For read/write commands that didn't generate replication data, we would block
        // on the highest offset of all accessed uncommitted keys and the redis DBs itself.
        // Note some commands categorized as writes can perform read only operations
        // therefore they should undergo the same checks as read-only commands.
        blocking_repl_offset = c->db->dirty_repl_offset;
        getKeysResult result;
        initGetKeysResult(&result);
        int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
        keyReference *keys = result.keys;

        for (int i = 0; i < numkeys; i++) {
            sds keystr = objectGetVal(c->argv[keys[i].pos]);
            // If we try to access an uncommitted key, then block the client
            // until all prior updates on this key have been acknowledged.
            // So here we essentially need to track the biggest offset amongst
            // all the uncommitted keys accessed by the command.
            long long offset = syncReplicationPurgeAndGetUncommittedKeyOffset(keystr, c->db);
            if(offset > blocking_repl_offset) {
                blocking_repl_offset = offset;
            }
        }
        getKeysFreeResult(&result);
    }

    return blocking_repl_offset;
}

/**
 * Process a single command for consistent write blocking.
 *
 * @param c Client
 * @return The replication offset we need to use for blocking this client for replica ack.
 *         Returns -1 if blocking is not required. Replication offset of 0 can lead to blocking
 *         behavior because if the primary has no replicas, and it is configured to require replica
 *         to ACK write, then it needs to block writes.
 */
//TODO:merge getSingleCommandBlockingOffsetForZeroDataLoss
//TODO:merge NOTDONE
//TODO:merge handle keyspace
static long long getSingleCommandBlockingOffsetForConsistentWrites(struct client *c) {
    serverAssert(isPrimarySyncReplicationEnabled());

    // If no durability provider is enabled, return -1 (no need to block)
    if (!anyDurabilityProviderEnabled())
        return -1;

    long long blocking_repl_offset = -1;

    if (//isAmazonKeySpaceInformationalCommand(c->cmd)
        // && !isAdminClient(c)
        (listLength(server.durability.clients_waiting_replica_ack) > 0 || hasUncommittedKeys() || amzDurableFunctions_isFunctionStoreUncommitted())) {
        // Block keyspace informational command for non-admin client if
        // has inflight commands
        blocking_repl_offset = server.primary_repl_offset;
    } else if ((server.primary_repl_offset > server.durability.pre_call_replication_offset) || (server.also_propagate.numops > pre_call_num_ops_pending_propagation)) {
        blocking_repl_offset = getSingleCommandBlockingOffsetForReplicatingCommand(c);
    } else {
        blocking_repl_offset = getSingleCommandBlockingOffsetForNonReplicatingCommand(c);
    }

    // If the blocking offset is already acknowledged by replicas,
    // then we don't need to block the response at all.
    if (blocking_repl_offset <= server.durability.previous_acked_offset) {
        blocking_repl_offset = -1;
    }

    return blocking_repl_offset;
}

/*=========================== Command hook functions ======================= */

/**
 * For synchronous replication, we need to record the starting replication offset of the command
 * about to be executed.
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
 * For synchronous replication, after we finish processing a valkey command which can either be a stand-alone
 * command, or in a multi-command block such as MULTI/EXEC transaction or a Lua script,  we need to
 * track the replication offset for the command and update the replication offset post-execution
 * for the entire command block. Later on, after the command block execution completes, we can determine
 * whether to block the client response for replica acknowledgement or not.
 *
 * Note: we need to track the final replication offset on the primary for all the keys and databases
 * that become dirty due to the command or transaction/script.
 *
 * @param c The client executing the valkey command
 */
//TODO:merge done
void afterCommandTrackReplOffset(client *c) {
    serverLog(LL_DEBUG, "afterCommandTrackReplOffset entered for command '%s'", c->cmd->declared_name);
    //TODO: blocked by module
    if (!isPrimarySyncReplicationEnabled() || (c->flag.blocked && !isClientBlockedByModule(c)))
        return;

    // Determine the blocking replication offset for the current command
    long long current_cmd_blocking_offset = getSingleCommandBlockingOffsetForConsistentWrites(c);

    // Here we need to track the replication offset of the command executed
    // by the calling client somewhere. This is usually tracked in the calling
    // client itself. But for the case of scripts, the script caller client is
    // different from the fake client created to execute each script command
    client *tracking_client = server.current_client ? server.current_client : c;

    if (current_cmd_blocking_offset > tracking_client->clientDurabilityInfo.current_command_repl_offset) {
        tracking_client->clientDurabilityInfo.current_command_repl_offset = current_cmd_blocking_offset;
    }

    // Handle database level modifications done by the current command
    handleDatabaseModification(c);
}

//TODO:merge NOTDONE
//TODO:merge diff between ALLOW and recover
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
 *
 * For non-administrative commands that is either read or write, we will track the pre-execution positions
 * in the reply COB of the client and all the connected monitors.
 */
 //TODO:merge NOTDONE zeroDataLossPreCommandExec
 //TODO:Mmerge READONLYMASTER
 //TODO:MERGE ADMINMONITORS
int preCommandExec(client *c) {
    // Reset the client current command replication offset
    c->clientDurabilityInfo.current_command_repl_offset = -1;
     // Reset the client module cmd blocking offset
    c->clientDurabilityInfo.module_cmd_blocking_offset = -1;

    // If a read-only master, no writes should have been allowed to reach this point
    //bool is_readonly_master = amzModuleReplication_getAndUpdateReadonlyMaster();
    // serverAssert(!(is_readonly_master && (c->cmd->flags & CMD_WRITE)));


    if (shouldRejectCommandWithUncommittedData(c)) {
        serverAssert(!(c->cmd->flags & CMD_WRITE));
        flagTransaction(c);
        addReplyError(c, SYNC_REPL_ACCESSED_DATA_UNAVAILABLE);
        return CMD_FILTER_REJECT;
    }

    // If we are operating as a primary, then apply the regular synchronous replication
    // logic of blocking command response post execution if needed.
    if (iAmPrimary() && clientEligibleForResponseTracking(c)) {
        // Track the pre-execution position in the client reply COB
        trackCommandPreExecutionPosition(c);

       
        if (isCommandReplicatedToMonitors()) {
            // Track the byte position in each monitor's reply COB prior to command replication
            listNode *ln;
            listIter li;
            listRewind(server.monitors,&li);
            while((ln = listNext(&li))) {
                client *monitor = ln->value;
                trackCommandPreExecutionPosition(monitor);
            }
        }
    }

    server.durability.pre_command_replication_offset = server.primary_repl_offset;
    return CMD_FILTER_ALLOW;
}


void amzDurableFunctions_updateBlockingOffsetForWrite(long long blocking_repl_offset) {
    if (processed_func_write_in_transaction) {
        func_store_blocking_offset = blocking_repl_offset;
        processed_func_write_in_transaction = false;
    }
}

/**
 * Marks keys, databases, and the function store dirty at the current
 * replication offset if they were updated during a transaction.
 */
//TODO:merge DONE
static void processPendingUncommittedData(long long blocking_repl_offset) {
    // Process the dirty keys in the current command block if needed
    if (listLength(pending_uncommitted_keys) > 0) {
        listIter li;
        listNode *key_node;
        listRewind(pending_uncommitted_keys, &li);
        while ((key_node = listNext(&li)) != NULL) {
            const pendingUncommittedKey *uk = listNodeValue(key_node);
            addUncommittedKey(objectGetVal(uk->key), blocking_repl_offset, uk->uncommitted_keys);
            listDelNode(pending_uncommitted_keys, key_node);
        }
    }

    // Process the dirty databases for the current command block if needed
    if (all_dbs_dirty_in_current_cmd) {
        for (int i = 0; i < server.dbnum; i++) {
            if (server.db[i] != NULL) {
                server.db[i]->dirty_repl_offset = blocking_repl_offset;
            }
        }
        all_dbs_dirty_in_current_cmd = false;
    } else if (listLength(pending_uncommitted_dbs) > 0) {
        listIter li;
        listNode *db_node;
        listRewind(pending_uncommitted_dbs, &li);
        while ((db_node = listNext(&li)) != NULL) {
            serverDb *db = listNodeValue(db_node);
            db->dirty_repl_offset = blocking_repl_offset;
            listDelNode(pending_uncommitted_dbs, db_node);
        }
    }

    serverAssert(listLength(pending_uncommitted_keys) == 0);
    serverAssert(listLength(pending_uncommitted_dbs) == 0);
    serverAssert(all_dbs_dirty_in_current_cmd == false);

    // Mark the function store dirty at the current replication offset if a 
    // Redis Functions write command occurred.
    amzDurableFunctions_updateBlockingOffsetForWrite(blocking_repl_offset);
}

/**
 * Helper method to update all pending tasks for replicas ACK. This will update each
 * pending task with the current server.primary_repl_offset and putting them into
 * the official tasks_waiting_replica_ack list.
 */
//TODO:merge not done, why delay?
void updateAndCertifyPendingTasksForReplicasAck(void) {
    listIter li;
    listNode *ln;
    for (int i = 0; i < AMZ_TASK_TYPE_MAX; i++) {
        // Traverse the entire list of pending tasks and update them
        listRewind(server.durability.pending_tasks_waiting_replica_ack[i], &li);
        while ((ln = listNext(&li))) {
            taskWaitingAck *task = listNodeValue(ln);
            // Assert that the offset value is not initialized yet
            serverAssert(task->offset == 0);
            task->offset = server.primary_repl_offset;
            // Send the module notifications for those modules that opted-in for notifications
            // in ZDL via module option AMZN_VALKEYMODULE_OPTIONS_DELAY_KEYSPACE_NOTIFICATION_FOR_ZDL
            if (task->type == AMZ_KEYSPACE_NOTIFY_TASK) {
                moduleNotifyKeyspaceEvent(
                  /*type*/  (intptr_t) task->argv[0],
                  /*event*/ (char *)task->argv[1],
                  /*key*/   (robj *)task->argv[2],
                  /*dbid*/  (intptr_t) task->argv[3]);
            }
        }
        // Append all elements in the pending tasks list at the tail of the regular
        // tasks list, and empty all items in the pending tasks list
        if (listLength(server.durability.pending_tasks_waiting_replica_ack[i]) > 0) {
            listJoin(server.durability.tasks_waiting_replica_ack[i], server.durability.pending_tasks_waiting_replica_ack[i]);
        }
        serverAssert(listLength(server.durability.pending_tasks_waiting_replica_ack[i]) == 0);
    }
}

/**
 * Perform post-processing after command execution for a given client.
 *
 * For write operation, we insert the updated keys into the uncommitted keys
 * map and wait for replica acknowledgement at that particular replication
 * byte offset.
 *
 * For read operation that need to access uncommitted keys, we need to block the client
 * response until all the dependent keys are properly acknowledged by replicas.
 *
 * @param c client
 */
//TODO:merge DONE
void postCommandExec(client *c) {
    if(c->cmd == NULL || c->flag.multi) {
        return;
    }

    // Block the client (TODO:monitors) based on the required replication offset
    // for the current command.
    long long blocking_repl_offset = c->clientDurabilityInfo.current_command_repl_offset;

    // If the client ran a transaction or a script that included write commands, the
    // blocking offset was not accurately set in afterCommandTrackReplOffset() so we update it here.
    //
    // We only do this for:
    // 1. Write commands (CMD_WRITE flag)
    // 2. Transactions (EXEC command) or scripts that may contain writes
    //
    // We exclude certain commands that can increase the replication offset without
    // actually modifying data:
    // - syncCommand: replication protocol commands increase offset
    // - clusterCommand: cluster management may increase offset
    // - shutdownCommand: may execute REPLCONF GETACK which increases offset
    //
    // This check prevents read-only commands from being incorrectly blocked when
    // the replication offset increases due to unrelated activity (like AOF fsync
    // completing for a prior write).
    if (server.primary_repl_offset > server.durability.pre_command_replication_offset
            && (c->cmd->flags & CMD_WRITE || isClientDoingTransaction(c))
            && c->cmd->proc != syncCommand && c->cmd->proc != clusterCommand && c->cmd->proc != shutdownCommand) {
        blocking_repl_offset = server.primary_repl_offset;
    }


    // If the client needs to block, we need to enforce that it is eligible for response tracking.
    // Otherwise we will try to block the response without tracking the command's pre-execution
    // position in the client reply buffer, which wouldn't work. If this assert fails, then we
    // need to fix clientEligibleForResponseTracking() to handle the problematic command.
    if (blocking_repl_offset > server.durability.pre_command_replication_offset) {
        serverAssert(clientEligibleForResponseTracking(c));
    }


    processPendingUncommittedData(server.primary_repl_offset);

    blockClientAndMonitorsOnReplOffset(c, blocking_repl_offset);

    // Update the pending replication ACK tasks and certify them
    updateAndCertifyPendingTasksForReplicasAck();
}

/**
 * Function used to initialize the durability datastructures.
 */
//TODO:merge DONE amzDurableInit
void syncReplicationInit(void) {
    serverLog(LL_DEBUG, "Initializing DKT");

    // Initialize synchronous replication
    pending_uncommitted_keys = listCreate();
    listSetFreeMethod(pending_uncommitted_keys, pendingUncommittedKeyDestructor);
    pending_uncommitted_dbs = listCreate();
    all_dbs_dirty_in_current_cmd = false;

    // Have to init the handlers before using them.
    initTaskTypes();
    server.durability.replica_offsets_size = 0;
    server.durability.replica_offsets = NULL;
    server.durability.previous_acked_offset = -1;
    server.durability.curr_db_scan_idx = 0;
    server.durability.clients_waiting_replica_ack = listCreate();
    for (int i=0; i<AMZ_TASK_TYPE_MAX; i++) {
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

    /* Register built-in durability providers (replica + AOF) */
    registerBuiltinDurabilityProviders();
}

/**
 * Utility function to release buffer used for replica offsets
 */
static void releaseReplicaOffsetBuffer(void) {
    server.durability.replica_offsets_size = 0;
    zfree(server.durability.replica_offsets);
    server.durability.replica_offsets = NULL;
}

/**
 * Function used to clean up the sync replication datastructures on server shutdown.
 */
//TODO:merge NOTDONE
//TODO: make sure the clean up is complete compare amzDurableCleanup
void syncReplicationCleanup(void) { 
    releaseReplicaOffsetBuffer();

    if (server.durability.clients_waiting_replica_ack != NULL) {
        listRelease(server.durability.clients_waiting_replica_ack);
        server.durability.clients_waiting_replica_ack = NULL;
    }
    if (pending_uncommitted_keys != NULL) {
        listRelease(pending_uncommitted_keys);
        pending_uncommitted_keys = NULL;
    }

    // cleanup tasks waiting for replica ACK
    for (int i=0; i<AMZ_TASK_TYPE_MAX; i++) {
        listRelease(server.durability.tasks_waiting_replica_ack[i]);
        server.durability.tasks_waiting_replica_ack[i] = NULL;
        listRelease(server.durability.pending_tasks_waiting_replica_ack[i]);
        server.durability.pending_tasks_waiting_replica_ack[i] = NULL;
    }

    
    /* Reset the durability provider registry so it can be re-initialized */
    num_durability_providers = 0;

    clearAllUncommittedKeys();
}



/**
 * Utility function to disconnect and free clients waiting for replica ACK
 */
//TODO:merge done
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
 * Function to reset primary state for synchronous replication
 * This function will be invoked at primary when sync replication is dynamically disabled or it becomes a replica.
 * Both cases require related resources to be reset to initial state. The only difference is how we handle the
 * clients waiting for replica ACK. If sync replication is disabled, all blocked responses and keyspace
 * notifications will be flushed for these clients, whose connections to primary will still be maintained.
 */
//TODO:merge done
static void syncReplicationResetPrimaryState(bool is_free_clients_needed) {
    // Release buffer we use for replica offsets
    releaseReplicaOffsetBuffer();

    if (listLength(server.durability.clients_waiting_replica_ack) > 0) {
        if (is_free_clients_needed) {
            freeClientsWaitingAck(&server.durability);
        } else {
            // Flush all blocked response and keyspace notifications to clients waiting for replica ACK
            unblockResponsesWithAckOffset(&server.durability, LLONG_MAX);
        }
        // Make sure there is no clients waiting for replica ACK
        serverAssert(listLength(server.durability.clients_waiting_replica_ack) == 0);
    }
     // Empty all the lists for blocked tasks
    for (int i=0; i<AMZ_TASK_TYPE_MAX; i++) {
        listEmpty(server.durability.tasks_waiting_replica_ack[i]);
        listEmpty(server.durability.pending_tasks_waiting_replica_ack[i]);
    }
}

/**
 * Clear the sync replication attributes specific to the primary.
 * This method is invoked when a master node becomes a replica.
 */
//TODO:merge done
void syncReplicationClearPrimaryState(void) {
    if (!isSyncReplicationEnabled()) return;

    // Clear all blocked responses and free the clients waiting for replica ACK
    syncReplicationResetPrimaryState(true);
}


/**
 * Generate INFO string for sync replication stats.
 */
//TODO:aof clean up
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
 * Reset related resources when disabling synchronous replication
 * This method is invoked when user turns off durability via config set command
 */
//TODO:merge DONE
void syncReplicationReset(void) {
    if (isSyncReplicationEnabled()) {
        // To enable sync replication, we update the pre-command offset so that the CONFIG SET command
        // itself doesn't get inadvertently blocked because the primary replication offset is greater
        // than the stale pre_command_replication_offset.
        server.durability.pre_command_replication_offset = server.primary_repl_offset;
        listIter li;
        listNode *ln;
        listRewind(server.clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            syncReplicationClientInit(c);
        }
    } else {
        // To disable durable write, we need to flush all blocked responses and keyspace
        // notifications, and then reset all durability related resources to initial state at the primary node.
        if (iAmPrimary()) {
            syncReplicationResetPrimaryState(false);
        }
        clearAllUncommittedKeys();
    }
}
