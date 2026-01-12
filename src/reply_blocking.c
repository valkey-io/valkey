#include "reply_blocking.h"
#include "expire.h"
#include "server.h"
#include "zmalloc.h"
#include <assert.h>
#include <math.h>

#include "script.h"

// TODO: handle PSYNC
// TODO: remove debug logging
// TODO: handle lua & multi
// TODO: handle blocking commands
// TODO: handle DB level commands (swap flushall etc)
// TODO: handle monitors
// TODO: telemetry

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
    return 1;
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
long long getConsensusOffset(const unsigned long numAcksNeeded) {
    const unsigned long numReplicas = listLength(server.replicas);
    if (numAcksNeeded == 0) {
        // If no ack is needed, then the consensus offset is the one primary is at.
        return server.primary_repl_offset;
    }

    // If the number of connected replicas is less than the number of required replicas,
    // return -1 because we don't have enough number of replicas for the ACK.
    if (numReplicas < numAcksNeeded) {
        return -1;
    }

    long long replica_offsets[numReplicas];

    populateReplicaOffsets(replica_offsets, numReplicas);

    // don't bother sorting if there is only one replica.
    if (numReplicas > 1) {
        qsort(replica_offsets, numReplicas, sizeof(long long), offsetSorterDesc);
    }

    // get the Kth element
    return replica_offsets[numAcksNeeded - 1];
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
 * Utility function to track the pre-execution position in the client reply COB. The given client can be either
 * a normal client (or TODO: a monitor client)
 * For a normal client, this position is the byte position in the COB prior to command execution. The response
 * generated from executing the next valkey command comes after this position.
 * (For a monitor client, this position is the byte position in the COB prior to command replication. The command
 * will be replicated after this position.)
 */
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
void syncReplicationClientInit(client *c) {
    if (!isSyncReplicationEnabled()) {
        return;
    }
    if (c->clientDurabilityInfo.blocked_responses == NULL) {
        c->clientDurabilityInfo.blocked_responses = listCreate();
        listSetFreeMethod(c->clientDurabilityInfo.blocked_responses, zfree);
        resetPreExecutionOffset(c);
        c->clientDurabilityInfo.current_command_repl_offset = -1;
    }
}

/**
 * Reset the client durable write attributes during a client clean-up.
 * This method is invoked when a client is freed.
 */
void syncReplicationClientReset(client *c) {
    // Free this client from the clients_waiting_replica_ack list and emit a metric on
    // how many clients are disconnected before the response gets flushed/unblocked.
    unblockClientWaitingReplicaAck(c);

    if (c->clientDurabilityInfo.blocked_responses != NULL) {
        listRelease(c->clientDurabilityInfo.blocked_responses);
        c->clientDurabilityInfo.blocked_responses = NULL;
    }

    resetPreExecutionOffset(c);
    c->clientDurabilityInfo.current_command_repl_offset = -1;
}

/**
 * Determines if a client is doing a transaction or not. This applies to either
 * MULTI/EXEC or scripts
 */
static bool isClientDoingTransaction(client *c) {
    return c->cmd->proc == execCommand || IS_SCRIPT_CALL_CMD(c->cmd);
}

/**
 * Returns true if the client is eligible for keyspace tracking
 * on a primary node.
 */
static bool clientEligibleForResponseTracking(client *c) {
    serverAssert(iAmPrimary());

    if (c->cmd == NULL) return false;

    // should we do info?
    // i.e: keyspace, does it include dirty keys?
    // Administrative commands that are not keyspace informational nor
    // write commands are not eligible for response tracking/blocking.
    if (c->cmd->flags & CMD_ADMIN && !(c->cmd->flags & CMD_WRITE)) {
        return false;
    }

    return c->cmd->flags & (CMD_WRITE | CMD_READONLY)
        || isClientDoingTransaction(c);// Read or write command // TODO: functions
}

/**
 * Check if we only allow client to receive up to a certain
 * position in the client reply buffer
 */
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
 * Unblocks the first response in the client's blocked responses list
 * @param c the client to unblock the first response for.
 */
static void unblockFirstResponse(const client *c) {
    serverAssert(c->clientDurabilityInfo.blocked_responses != NULL);
    if (listLength(c->clientDurabilityInfo.blocked_responses) > 0) {
        listNode *first = listFirst(c->clientDurabilityInfo.blocked_responses);
        listDelNode(c->clientDurabilityInfo.blocked_responses, first);
    }
}

/**
 * Determines if we need to block on a given replication offset for a given client
 * @param c Client
 * @param offset The replication offset we are checking
 * @return 0 if we don't need to block at the specified offset, and 1 if we do.
 */
static int isBlockingNeededForOffset(const client *c, const long long offset) {
    // If the blocking offset is -1 or no replica is needed to ACK, don't block.
    if (offset == -1 || replicaAcksForConsensus() == 0) {
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
void blockClientOnReplOffset(client *c, const long long blockingReplOffset) {
    serverAssert(isPrimarySyncReplicationEnabled());

    /* If needed, we block the client and put it into our list of clients
     * waiting for ack from slaves. */
    if (isBlockingNeededForOffset(c, blockingReplOffset)) {
        serverLog(LL_DEBUG, "client should be blocked at offset %lld,", blockingReplOffset);
        blockLastResponseIfExist(c, blockingReplOffset);
        if (!c->clientDurabilityInfo.durable_blocked_client) {
            listAddNodeTail(server.durability.clients_waiting_replica_ack, c);
            c->clientDurabilityInfo.durable_blocked_client = 1;
        }
        replicationRequestAckFromReplicas();
    }

    // Now we have processed the client blocking information and tracked it,
    // we can reset the client durability attributes we are tracking for
    // the current command.
    resetPreExecutionOffset(c);
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
static void blockClientAndMonitorsOnReplOffset(client *c, long long blockingReplOffset) {
    // Block the client that issues the command on the replication offset
    blockClientOnReplOffset(c, blockingReplOffset);

    // TODO: handle monitors
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
void unblockResponsesWithAckOffset(const durable_t *durability, const long long consensus_ack_offset) {
    serverLog(LL_DEBUG, "unblocking clients for consensus offset %lld,", consensus_ack_offset);
    // Traverses through all the clients that wait for replica ack
    listIter li;
    listNode *ln;
    listRewind(durability->clients_waiting_replica_ack, &li);
    while ((ln = listNext(&li))) {
        client *c = ln->value;

        // For each client blocked, we go through all its blocked responses,
        // and unblock all the responses whose replication offset are
        // ACK'ed by the required number of replicas
        // If the max repl offset is acked, all blocked responses will be unblocked
        serverAssert(c->clientDurabilityInfo.blocked_responses != NULL);
        bool unblocked_responses = false;

        // Keep deleting from the front while the first response can be unblocked
        while (listLength(c->clientDurabilityInfo.blocked_responses) > 0) {
            const listNode *first = listFirst(c->clientDurabilityInfo.blocked_responses);
            const blockedResponse *br = listNodeValue(first);

            if (br->primary_repl_offset <= consensus_ack_offset) {
                unblockFirstResponse(c);
                unblocked_responses = true;
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
            unblockClientWaitingReplicaAck(c);
        }
        // Put client in pending write queue so responses can be flushed
        // to client if we have unblocked at least 1 response objects.
        if (unblocked_responses) {
            putClientInPendingWriteQueue(c);
        }
    }
}

/**
 * Checks if there are clients blocked that can be unblocked since
 * we received enough ACKs from replicas.
 */
void postReplicaAck(void) {
    serverLog(LL_DEBUG, "postReplicaAck hook entered");
    if (!isPrimarySyncReplicationEnabled()) {
        return;
    }

    durable_t *durability = &server.durability;
    const long long consensus_ack_offset = getConsensusOffset(replicaAcksForConsensus());
    if (consensus_ack_offset <= durability->previous_acked_offset) {
        return;
    }

    // Update the previous acknowledged offset for durability
    durability->previous_acked_offset = consensus_ack_offset;

    // Unblock responses and keyspace notifications with consensus acked offset
    unblockResponsesWithAckOffset(durability, consensus_ack_offset);
}

/*================================= Key management ============================ */

/**
 * Mark a key as uncommitted at a particular replication offset for acknowledgement.
 *
 * @param key The name of the uncommitted key to mark
 * @param offset The replication offset to wait on the uncommitted key
 * @param uncommittedKeys The set of uncommitted keys
 */
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
void handleUncommittedKeyForClient(const client *c, const robj *key, const serverDb *db) {
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

static void handleDatabaseModification(client *c) {
    UNUSED(c);
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
void syncReplicationInitDatabase(serverDb *db) {
    db->uncommitted_keys = hashtableCreate(&uncommittedKeysHashtableType);
    db->dirty_repl_offset = -1;
    db->uncommitted_keys_cursor = 0;
    db->scan_in_progress = 0;
}

/**
 * Utility function to clear all uncommitted keys for each database
 */
static void clearAllUncommittedKeys(void) {
    serverLog(LL_NOTICE, "Clearing all uncommitted keys for sync replication");
    for (int i = 0; i < server.dbnum; i++) {
        serverDb *db = server.db[i];
        if (db == NULL) continue;
        hashtableRelease(db->uncommitted_keys);
        syncReplicationInitDatabase(db);
    }
    server.durability.curr_db_scan_idx = 0;
}

/*========================== Command access validation ====================== */

/**
 * Determines if a single valkey command is trying to access an uncommitted key.
 * Returns 1 if so, 0 otherwise.
 */
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
 * Determine if a client is trying to access uncommitted keys.
 * Returns 1 if so, 0 otherwise.
 */
static int isAccessingUncommittedData(client *c) {
    if (isSingleCommandAccessingUncommittedKeys(c->db, c->cmd, c->argv, c->argc)) {
        return 1;
    }
    // TODO: handle other commands
    return 0;
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

/*========================== Command offset calculation ===================== */

/**
 * Process a single replicating command for consistent write blocking.
 *
 * @param c Client
 * @return The blocking replication offset or -1 if we're in a nested call and the replication
 *         offset has not been updated yet.
 */
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
    // TODO: handle functions
    getKeysResult result;
    initGetKeysResult(&result);
    int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
    keyReference *keys = result.keys;
    if (numkeys > 0) {
        if (c->cmd->proc == moveCommand) {
            // TODO: support MOVE command: we need to mark the key as dirty in the destination DB
            // dont block for now
            getKeysFreeResult(&result);
            return -1;
        } else if (c->cmd->proc == copyCommand) {
            //  TODO: handle copy command
            // handle the dirty keys in the destination db
            // dont block for now
            getKeysFreeResult(&result);
            return -1;
        }

        // Mark all the keys updated by the current command as dirty in the current DB
        for (int i = 0; i < numkeys; i++) {
            handleUncommittedKeyForClient(c, c->argv[keys[i].pos], c->db);
        }
    }
    getKeysFreeResult(&result);

    // If we're in a nested call, we do not update the blocking replication offset yet because
    // the replication data is not propagated until after the transaction completes.
    // Instead, the blocking repl offset will be finalized in postCommandTrackReplOffset
    if (!server.execution_nesting) {
        return server.primary_repl_offset;
    }

    return -1;
}

/**
 * Process a single non-replicating command for consistent write blocking.
 *
 * @param c Client
 * @return The blocking replication offset or -1 if no blocking need.
 */
static long long getSingleCommandBlockingOffsetForNonReplicatingCommand(client *c) {
    long long blocking_repl_offset = -1;
    // TODO: handle function, module, etc
    if (IS_SCRIPT_CALL_READONLY_CMD(c->cmd)) {
        // We don't need to indiscriminately block on dirty keys specified in read-only script run commands.
        // We only block on ones that are actually accessed which is handled during the nested calls.
        return -1;
    }
    if (c->cmd->flags & (CMD_READONLY | CMD_WRITE)) {
        serverLog(LL_DEBUG, "getSingleCommandBlockingOffsetForNonReplicatingCommand for command");
        // For read/write commands that didn't generate replication data, we would block
        // on the highest offset of all accessed uncommitted keys and the valkey DBs itself.
        // Note some commands categorized as writes can perform read only operations
        // therefore they should undergo the same checks as read-only commands.
        blocking_repl_offset = c->db->dirty_repl_offset;
        getKeysResult result;
        initGetKeysResult(&result);
        int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
        keyReference *keys = result.keys;

        for (int i = 0; i < numkeys; i++) {
            sds key = objectGetVal(c->argv[keys[i].pos]);
            // If we try to access an uncommitted key, then block the client
            // until all prior updates on this key have been acknowledged.
            // So here we essentially need to track the biggest offset amongst
            // all the uncommitted keys accessed by the command.
            const long long offset = syncReplicationPurgeAndGetUncommittedKeyOffset(key, c->db);
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
 *
 * @param c Client
 * @return The replication offset we need to use for blocking this client for replica ack.
 *         Returns -1 if blocking is not required. Replication offset of 0 can lead to blocking
 *         behavior because if the primary has no replicas, and it is configured to require replica
 *         to ACK write, then it needs to block writes.
 */
static long long getSingleCommandBlockingOffsetForConsistentWrites(struct client *c) {
    serverAssert(isPrimarySyncReplicationEnabled());

    // If no replicas are required for ACK, then return -1 (no need to block)
    if (replicaAcksForConsensus() == 0)
        return -1;

    long long blocking_repl_offset = -1;

    if ((server.primary_repl_offset > server.durability.pre_call_replication_offset) || (server.also_propagate.numops > server.durability.pre_call_num_ops_pending_propagation)) {
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
    serverLog(LL_DEBUG, "beforeCommandTrackReplOffset hook: pre_call_replication_offset=%lld, pre_call_num_ops_pending_propagation=%d",
              server.durability.pre_call_replication_offset, server.durability.pre_call_num_ops_pending_propagation);
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
void afterCommandTrackReplOffset(client *c) {
    serverLog(LL_DEBUG, "afterCommandTrackReplOffset entered for command '%s'", c->cmd->declared_name);
    //TODO: blocked by module
    if (!isPrimarySyncReplicationEnabled() || (c->flag.blocked))
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
int preCommandExec(client *c) {
    serverLog(LL_DEBUG, "preCommandExec hook entered for command '%s'",
              c->cmd ? c->cmd->declared_name : "NULL");
    if (!isSyncReplicationEnabled()) {
        serverLog(LL_DEBUG, "preCommandExec hook: durability not enabled, allowing");
        return CMD_FILTER_ALLOW;
    }

    // Reset the client current command replication offset
    c->clientDurabilityInfo.current_command_repl_offset = -1;

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

        // todo: handle monitors
    }

    server.durability.pre_command_replication_offset = server.primary_repl_offset;
    return CMD_FILTER_ALLOW;
}


/**
 * Marks keys, databases, and the function store dirty at the current
 * replication offset if they were updated during a transaction.
 */
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

    serverAssert(listLength(pending_uncommitted_keys) == 0);
    serverAssert(all_dbs_dirty_in_current_cmd == false);
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
void postCommandExec(client *c) {
    if (!isPrimarySyncReplicationEnabled()) {
        return;
    }
    serverLog(LL_DEBUG, "postCommandExec hook entered for command '%s'",
              c->cmd ? c->cmd->declared_name : "NULL");
    // If the command is NULL or is in a MULTI/EXEC block, then we skip
    if(c->cmd == NULL || c->flag.multi) {
        return;
    }

    // Block the client (TODO:monitors) based on the required replication offset
    // for the current command.
    long long blocking_repl_offset = c->clientDurabilityInfo.current_command_repl_offset;


    // TODO CMD_MAY_REPLICATE

    // If the client ran a transaction or a script that included write commands, the
    // blocking offset was not accurately set in amzPostCall() so we update it here.
    //
    // There are 2 exceptions to this rule:
    // 1. We exclude sync commands as the master replication offset will have increased
    // despite no change to the primary's state occurring.
    // 2. SHUTDOWN command may execute REPLCONF GETACK command if there is uncommitted data
    // and this increments the master replication offset. However, the client is not eligible
    // for blocking and the SHUTDOWN command should not be blocked.
    if (server.primary_repl_offset > server.durability.pre_command_replication_offset
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
}

/**
 * Function used to initialize the durability datastructures.
 */
void syncReplicationInit(void) {
    server.durability.previous_acked_offset = -1;
    server.durability.curr_db_scan_idx = 0;
    server.durability.clients_waiting_replica_ack = listCreate();
    if (pending_uncommitted_keys == NULL) {
        pending_uncommitted_keys = listCreate();
        listSetFreeMethod(pending_uncommitted_keys, pendingUncommittedKeyDestructor);
    }
}

/**
 * Function used to clean up the sync replication datastructures on server shutdown.
 */
void syncReplicationCleanup(void) {
    serverLog(LL_DEBUG, "Cleanup reply blocking structures");
    server.durability.replica_offsets_size = 0;
    zfree(server.durability.replica_offsets);

    if (server.durability.clients_waiting_replica_ack != NULL) {
        listRelease(server.durability.clients_waiting_replica_ack);
        server.durability.clients_waiting_replica_ack = NULL;
    }
    if (pending_uncommitted_keys != NULL) {
        listRelease(pending_uncommitted_keys);
        pending_uncommitted_keys = NULL;
    }

    clearAllUncommittedKeys();
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
 * Utility function to disconnect and free clients waiting for replica ACK
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
 * Function to reset primary state for synchronous replication
 * This function will be invoked at primary when sync replication is dynamically disabled or it becomes a replica.
 * Both cases require related resources to be reset to initial state. The only difference is how we handle the
 * clients waiting for replica ACK. If sync replication is disabled, all blocked responses and keyspace
 * notifications will be flushed for these clients, whose connections to primary will still be maintained.
 */
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
}

/**
 * Clear the sync replication attributes specific to the primary.
 * This method is invoked when a master node becomes a replica.
 */
void syncReplicationClearPrimaryState(void) {
    if (!isSyncReplicationEnabled()) return;

    // Clear all blocked responses and free the clients waiting for replica ACK
    syncReplicationResetPrimaryState(true);
}


/**
 * Reset related resources when disabling synchronous replication
 * This method is invoked when user turns off durability via config set command
 */
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
