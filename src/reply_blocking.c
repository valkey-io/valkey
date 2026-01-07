#include "reply_blocking.h"
#include "expire.h"
#include "server.h"
#include <assert.h>
#include <math.h>

// TODO: handle PSYNC
// TODO: handle failovers (clear durability state)
// TODO: remove debug logging
// TODO: handle lua & multi
// TODO: handle blocking commands
// TODO: handle DB level commands (swap flushall etc)
// TODO: handle monitors
// TODO: temetry

/*============================ Internal prototypes ========================= */

static void resetPreExecutionOffset(struct client *c);
static inline void trackCommandPreExecutionPosition(struct client *c);
static int unblockClientWaitingReplicaAck(struct client *c);
static bool clientEligibleForResponseTracking(client *c);
static inline void unblockFirstResponse(struct client *c);
static inline int isBlockingNeededForOffset(struct client *c, long long offset);
static void blockClientAndMonitorsOnReplOffset(struct client *c, long long blockingReplOffset);
static void populateReplicaOffsets(long long *offsets, const size_t numReplicas);
static inline int offsetSorterDesc(const void* v1, const void* v2);
static unsigned long long getNumberOfUncommittedKeys(void);
static inline int hasUncommittedKeys(void);
static inline void addUncommittedKey(const sds key, const long long offset, rax *uncommittedKeys);
static int isSingleCommandAccessingUncommittedKeys(serverDb *db, struct serverCommand *cmd, robj **argv, int argc);
static int isAccessingUncommittedData(client *c);
static bool shouldRejectCommandWithUncommittedData(client *c);
static long long getSingleCommandBlockingOffsetForReplicatingCommand(client *c);
static long long getSingleCommandBlockingOffsetForNonReplicatingCommand(client *c);
static long long getSingleCommandBlockingOffsetForConsistentWrites(struct client *c);

/*================================= Utility functions ======================== */

/**
 * Utility function to determine whether the durability flag has been enabled.
 * return       1 if durability is enabled, 0 otherwise.
 */
int isDurabilityEnabled(void) {
    // should this have its own flag?
    // or general 'durability flag'
    return true;
}

int isPrimaryDurabilityEnabled(void) {
    return isDurabilityEnabled() && iAmPrimary();
}

/**
 * TODO: this needs to be replaced by an interface w/ durable replication
 * to tell us when we've achieved consensus via raft.
 * Using 2 as default for POC. 
 */
static inline unsigned replicaAcksForConsensus(void) {
    return 1; 
}

/*
 * Utility function to sort offsets in descending order
 * @v1    Pointer to long long representing the first offset
 * @v2    Pointer to long long representing the second offset
 * returns    -ve  if first offset > second offset
 *            zero if both offsets are equal
 *            +ve  if first offset < second offset
 */
static inline int offsetSorterDesc(const void* v1, const void* v2) {
    const long long *a = v1;
    const long long *b = v2;
 
    return (*b - *a);
}

static unsigned long long getNumberOfUncommittedKeys(void) {
    unsigned long long num_uncommitted_keys = 0;
    for(int i=0; i<server.dbnum; i++) {
        if (server.db[i] != NULL) { 
            num_uncommitted_keys += raxSize(server.db[i]->uncommitted_keys);
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

/**
 * Determine if there are uncommitted keys in the valkey server or not
 * Returns 1 if there are uncommitted keys, 0 otherwise.
 */
static inline int hasUncommittedKeys(void) {
    for (int i = 0; i < server.dbnum; i++) {
        if (server.db[i] != NULL) { 
            if(raxSize(server.db[i]->uncommitted_keys) > 0)
                return 1;
        }
    }
    return 0;
}

/*================================= Replica offset management =============== */

/*
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
    for (unsigned i=0; i < numReplicas && ln != NULL; ln = listNext(&li), i++) {
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
static inline void trackCommandPreExecutionPosition(struct client *c) {
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
        c->clientDurabilityInfo.offset.byte_offset = ((clientReplyBlock*)listNodeValue(last_reply_block))->used;
    } else if (bufpos > 0) {
        // We are only tracking the client reply block and we don't need to
        // take ownership of the pointer, so there is no need to free it
        c->clientDurabilityInfo.offset.reply_block = NULL;
        c->clientDurabilityInfo.offset.byte_offset = bufpos;
    }
    c->clientDurabilityInfo.offset.recorded = true;
}

/* If the client is currently waiting for replica acknowledgement,
 * mark it unblocked and reset the client flags. 
 * This involves us removing the client from the clients_waiting_replica_ack list,
 * and mark the client as unblocked for durability.
 *
 * @param c The client
 * @return 1 if the client is successfully marked unblocked, 0 otherwise 
 */
static int unblockClientWaitingReplicaAck(struct client *c) {
    if (c->clientDurabilityInfo.durable_blocked_client) {
        listNode *ln = listSearchKey(server.durability.clients_waiting_replica_ack, c);
        if(ln != NULL) {
            listDelNode(server.durability.clients_waiting_replica_ack, ln);
            c->clientDurabilityInfo.durable_blocked_client = 0;
            return 1;
        }
    }
    return 0;
}

/**
 * Initialize the durable write client attributes when client is created
 */
void durableClientInit(struct client *c) {
    if (!isDurabilityEnabled()) {
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
void durableClientReset(struct client *c) {
    // Free this client from the clients_waiting_replica_ack list and emit a metric on
    // how many clients are disconnected before the response gets flushed/unblocked.
    unblockClientWaitingReplicaAck(c);

    if(c->clientDurabilityInfo.blocked_responses != NULL) {
        listRelease(c->clientDurabilityInfo.blocked_responses);
        c->clientDurabilityInfo.blocked_responses = NULL;
    }

    resetPreExecutionOffset(c);
    c->clientDurabilityInfo.current_command_repl_offset = -1;
}

/*
 * Returns true if the client is eligible for keyspace tracking
 * on a primary node.
 */
static bool clientEligibleForResponseTracking(client *c) {
    serverAssert(iAmPrimary());

    if(c->cmd == NULL) return false;

    // should we do info?
    // i.e: keyspace, does it include dirty keys?
    // Administrative commands that are not keyspace informational nor
    // write commands are not eligible for response tracking/blocking.
    if ((c->cmd->flags & CMD_ADMIN) && !(c->cmd->flags & CMD_WRITE)) {
        return false;
    }

    return ((c->cmd->flags & (CMD_WRITE | CMD_READONLY))); // Read or write command
            // TODO: transactions, lua, scripts, etc
            // TODO: functions
}

/* Check if we only allow client to receive up to a certain
 * position in the client reply buffer 
 */
inline bool isClientReplyBufferLimited(struct client *c) {
    return c->clientDurabilityInfo.blocked_responses != NULL && 
              listLength(c->clientDurabilityInfo.blocked_responses) > 0;
}

/*================================= Response blocking ======================= */

/**
 * Block the last response if it exists in the client output buffer
 * @param c The client to block the last response in the COB
 * @param blocked_offset The replication offset to block on
 */
void blockLastResponseIfExist(struct client *c, long long blocked_offset) {
    // We must have called the pre-hook to track COB position
    serverAssert(c->clientDurabilityInfo.offset.recorded);

    // Flag to indicate whether there is new response added in client output 
    // buffer
    bool has_new_response = false;
    struct listNode *disallowed_reply_block = 
            c->clientDurabilityInfo.offset.reply_block;
    size_t disallowed_byte_offset = 
            c->clientDurabilityInfo.offset.byte_offset;

    // Track the starting position of the blocked response in the client COB
    if (disallowed_reply_block == NULL) {
        // The end of last response was in the initial 16KB buffer
        if((size_t)c->bufpos > disallowed_byte_offset) {
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
        clientReplyBlock *last_reply_block = (clientReplyBlock*)listNodeValue(disallowed_reply_block);
        if (last_reply_block->used > disallowed_byte_offset) {
            // More data comes after the last reply in the same reply block
            has_new_response = true;
        } else if(disallowed_reply_block->next != NULL) {
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

/* Unblock the first response for the client */ 
static inline void unblockFirstResponse(struct client *c) {
    serverAssert(c->clientDurabilityInfo.blocked_responses != NULL);
    if (listLength(c->clientDurabilityInfo.blocked_responses) > 0) {
        listNode *first = listFirst(c->clientDurabilityInfo.blocked_responses);
        listDelNode(c->clientDurabilityInfo.blocked_responses, first);
    }
}

/* Determines if we need to block on a given replication offset for a given client
 * @param c Client
 * @param offset The replication offset we are checking
 * @return 0 if we don't need to block at the specified offset, and 1 if we do.
 */
static inline int isBlockingNeededForOffset(struct client *c, long long offset) {
    // If the blocking offset is -1 or no replica is needed to ACK, don't block.
    if (offset == -1 || replicaAcksForConsensus() == 0) {
        return 0;
    }

    // If there are no blocked responses previously, we always want to block
    if (listLength(c->clientDurabilityInfo.blocked_responses) == 0)
        return 1;

    listNode *last_response = listLast(c->clientDurabilityInfo.blocked_responses);
    long long previous_offset = ((blockedResponse*)listNodeValue(last_response))->primary_repl_offset;
    return previous_offset < offset; 
}

/**
 * Block a given client on the specified replication offset if applicable.
 * And clears the client's pre-execution byte offset fields so it won't carry
 * forward to the next command.
 *
 * @param c Client
 * @param blockingReplOffset The replication offset to block the client on
 */
void blockClientOnReplOffset(struct client *c, long long blockingReplOffset) {
    serverAssert(isPrimaryDurabilityEnabled());

    /* If needed, we block the client and put it into our list of clients
     * waiting for ack from slaves. */
    if (isBlockingNeededForOffset(c, blockingReplOffset)) {
        serverLog(LOG_DEBUG, "client should be blocked at offset %lld,", blockingReplOffset);
        blockLastResponseIfExist(c, blockingReplOffset);
        if (!c->clientDurabilityInfo.durable_blocked_client) {
            listAddNodeTail(server.durability.clients_waiting_replica_ack,c);
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
static void blockClientAndMonitorsOnReplOffset(struct client *c, long long blockingReplOffset) {
    // Block the client that issues the command on the replication offset
    blockClientOnReplOffset(c, blockingReplOffset);

    //TODO: handle monitors
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
void unblockResponsesWithAckOffset(struct durable_t *durability, long long consensus_ack_offset) {
            
    serverLog(LOG_DEBUG, "unblocking clients for consensus offset %lld,", consensus_ack_offset);
    // Traverses through all the clients that wait for replica ack
    listIter li, li_response;
    listNode *ln, *ln_response;
    listRewind(durability->clients_waiting_replica_ack, &li);
    blockedResponse *br = NULL;
    while((ln = listNext(&li))) {
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
            }
        }
        // Put client in pending write queue so responses can be flushed
        // to client if we have unblocked at least 1 response objects.
        if (unblocked_responses) {
            putClientInPendingWriteQueue(c);
        }
    }
}

/* Check if there are clients blocked that can be unblocked since
 * we received enough ACKs from replicas */
void postReplicaAck(void) {
    serverLog(LOG_DEBUG, "postReplicaAck hook entered");
    if (!isPrimaryDurabilityEnabled()) {
        return;
    }
    
    struct durable_t *durability = &server.durability;
    long long consensus_ack_offset = getConsensusOffset(replicaAcksForConsensus());
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
static inline void addUncommittedKey(const sds key, const long long offset, rax *uncommittedKeys) {
    // The value in the uncomittedKeys is the replication offset in long long format
    int retval = raxInsert(uncommittedKeys, (unsigned char*)key, sdslen(key), (void *)offset, NULL);
    serverAssert(retval == 1 || errno == 0);
}

/**
 * Retrieve the uncommitted replication offset for a given key, purge the given
 * key from uncommitted keys set if the replication offset has been committed.
 * Pre-condition: valkey is currently a primary
 * @param key The key to retrieve the uncommitted replication offset
 * @param db The serverDB object
 * @return the ACK offset of the key if key is uncommitted, returns -1 otherwise.
 */
long long durablePurgeAndGetUncommittedKeyOffset(const sds key, serverDb *db) {
    serverAssert(iAmPrimary());
    void *result;
    if (!raxFind(db->uncommitted_keys, (unsigned char*)key, sdslen(key), &result)) {
        return -1;
    }

    long long key_offset = (long long) result;
    
    /**
     * If the replication offset of key has been properly acked by replicas, 
     * then purge the key from the uncommitted keys set, and return -1
     * indicating the key has been committed.
     */
    if (key_offset <= server.durability.previous_acked_offset) {
        raxRemove(db->uncommitted_keys, (unsigned char*)key, sdslen(key), NULL);
        return -1;
    }

    return key_offset;
}

/**
 * Utility method to handle a dirty key for a given client.
 * @param c The calling client. NULL if the key becomes dirty outside of a client command (i.e. expiry/eviction)
 * @param key
 * @param db
 */
void handleUncommittedKeyForClient(client *c, robj *key, serverDb *db) {
    // If we are in the context of a MULTI/EXEC transaction (or TODO: a script)
    // we mark the dirty key
    // pending so it can be properly recorded later on with the final replication offset.
    if ((c != NULL) && ((c->flag.multi))) {
       // TODO: handle multi
    } else {
        // Otherwise, the key is updated outside of a transaction or a script, simply mark the key
        // dirty at the current primary_repl_offset
        addUncommittedKey(objectGetVal(key), server.primary_repl_offset, db->uncommitted_keys);
    }
}

/**
 * Clear all uncommitted DBs and keys that are properly acknowledged by 
 * sufficient number of replicas and mark them no longer dirty. 
 *
 * This method iterates through all the valkey databases and checks the
 * DB and all items tracked by the uncommitted_keys set for each, and
 * removes keys that are acknowledged by sufficient number of replicas.
 * It is applicable only to primary.
 */
void clearUncommittedKeysAcknowledged(void) {
    if (!isPrimaryDurabilityEnabled()) {
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
    while(durability->curr_db_scan_idx < server.dbnum) {
        serverDb *db = server.db[durability->curr_db_scan_idx];
        if (db != NULL) {
            raxIterator *iter = &db->next_scan_iter;

            // Clear the database's dirty replication offset if it is acknowledged by replicas
            if (db->dirty_repl_offset <= server.durability.previous_acked_offset) {
                db->dirty_repl_offset = -1;
            }

            // In a time-bound fashion, clear the uncommitted keys if the required replication
            // offset has been acknowledged by replicas.
            if(raxSize(db->uncommitted_keys) > 0) {
                if (!db->scan_in_progress) {
                    raxStart(iter, db->uncommitted_keys);
                    raxSeek(iter, "^", NULL, 0);
                    db->scan_in_progress = 1;
                } else {
                    raxSeek(iter, ">=", iter->key, iter->key_len);
                }
        
                while (raxNext(iter)) {
                    // Use scan_count % TIME_CHECK_INTERVAL to reduce the number of calling mstime
                    // method, it can make sure to scan some keys if time_limit_ms 
                    // is very small. 
                    if ((time_limit_ms > 0) && (scan_count > 0) 
                            && (scan_count % TIME_CHECK_INTERVAL == 0)) {
                        unsigned long long cur_time_ms = mstime();
                        if (cur_time_ms - start_time_ms > time_limit_ms) {
                            // Stop the current scan, continue to do in the next run
                            return;
                        }
                    }
        
                    long long dirty_key_offset = (long long)iter->data;
                    if (dirty_key_offset <= server.durability.previous_acked_offset) {
                        raxRemove(db->uncommitted_keys, iter->key, iter->key_len, NULL);
                        raxSeek(iter, ">", iter->key, iter->key_len);
                    }
                    scan_count++;
                }
            }
            
            // Finish to DB scan.
            if(db->scan_in_progress) {
                db->scan_in_progress = 0;
                raxStop(iter);
            }
        }
        durability->curr_db_scan_idx++;
    }

    // If all databases have been scanned, reset curr_db_scan_idx to 0, and 
    // exit the keys cleanup procedure.
    if(durability->curr_db_scan_idx == server.dbnum) {
        durability->curr_db_scan_idx = 0;
    }
}

/*========================== Command access validation ====================== */

/**
 * Determines if a single valkey command is trying to access an uncommitted key. 
 * Returns 1 if so, 0 otherwise. 
 */
static int isSingleCommandAccessingUncommittedKeys(serverDb *db, struct serverCommand *cmd, robj **argv, int argc) {
    // If the database has no uncommitted keys, return 0
    if (raxSize(db->uncommitted_keys) == 0) return 0;

    getKeysResult keysResult;
    initGetKeysResult(&keysResult);
    int numkeys = getKeysFromCommand(cmd, argv, argc, &keysResult);
    keyReference *keys = keysResult.keys;

    for (int i = 0; i < numkeys; i++) {
        sds keystr = objectGetVal(argv[keys[i].pos]);
        // Check if we are trying to access an uncommitted key
        void *result;
        if (raxFind(db->uncommitted_keys, (unsigned char*)keystr, sdslen(keystr), &result)) {
            getKeysFreeResult(&keysResult);
            return 1;
        }
    }

    // Free the keys after done processing them
    getKeysFreeResult(&keysResult);
    return 0;
}

/**
 * Determine if a client is trying to access uncommitted keys or function store.
 * Returns 1 if so, 0 otherwise. 
 */
static int isAccessingUncommittedData(client *c) {
    if (hasUncommittedKeys()) {
        return 1;
    }

    // Single command handling
    if (isSingleCommandAccessingUncommittedKeys(c->db, c->cmd, c->argv, c->argc)) {
        return 1;
    }

    return 0;
}

static bool shouldRejectCommandWithUncommittedData(client *c) {
    if(c->cmd == NULL   // command is null
            || ((c->cmd->flags & CMD_ADMIN))
            || c->flag.primary) {
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

    if (!(c->cmd->flags & CMD_WRITE)) {
        return -1;
    }

    // If the command executed generated replication data, then this means the server data changed.
    // We need to mark the modified data as dirty and block the response to the client until the 
    // replica's replication offset is caught up to the current global offset.
    // todo handle functions
    getKeysResult result;
    initGetKeysResult(&result);
    int numkeys = getKeysFromCommand(c->cmd, c->argv, c->argc, &result);
    keyReference *keys = result.keys;
    if (numkeys > 0) {
        if (c->cmd->proc == moveCommand) {
            // TODO: support MOVE command: we need to mark the key as dirty in the destination DB
            // dont block for now
            return -1;
        } else if (c->cmd->proc == copyCommand) {
            //  TODO: handle copy command
            // handle the dirty keys in the destination db
            // dont block for now
            return -1;
        }

        // Mark all the keys updated by the current command as dirty in the current DB 
        for (int i = 0; i < numkeys; i++) {
            handleUncommittedKeyForClient(c, c->argv[keys[i].pos], c->db);
        }
    }
    getKeysFreeResult(&result);
    

    return server.primary_repl_offset;
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
    if (c->cmd->flags & (CMD_READONLY | CMD_WRITE)) {
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
            sds keystr = objectGetVal(c->argv[keys[i].pos]);
            // If we try to access an uncommitted key, then block the client
            // until all prior updates on this key have been acknowledged.
            // So here we essentially need to track the biggest offset amongst
            // all the uncommitted keys accessed by the command.
            long long offset = durablePurgeAndGetUncommittedKeyOffset(keystr, c->db);
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
static long long getSingleCommandBlockingOffsetForConsistentWrites(struct client *c) {
    serverAssert(isPrimaryDurabilityEnabled());

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
    serverLog(LOG_DEBUG, "preCall hook entered");
    if (!isPrimaryDurabilityEnabled()) return;

    server.durability.pre_call_replication_offset = server.primary_repl_offset;
    server.durability.pre_call_num_ops_pending_propagation = server.also_propagate.numops;
    serverLog(LOG_DEBUG, "preCall hook: pre_call_replication_offset=%lld, pre_call_num_ops_pending_propagation=%d", 
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
void afterCommandTrackReplOffset(struct client *c) {
    // log debug tracing
    serverLog(LOG_DEBUG, "Call hook entered for command '%s'", c->cmd->declared_name);
    if (!isPrimaryDurabilityEnabled() || (c->flag.blocked))
        return;

    // Determine the blocking replication offset for the current command
    long long current_cmd_blocking_offset = getSingleCommandBlockingOffsetForConsistentWrites(c);

    // Here we need to track the replication offset of the command executed
    // by the calling client somewhere. This is usually tracked in the calling
    // client itself. But for the case of scripts, the script caller client is
    // different from the fake client created to execute each script command
    struct client *tracking_client = server.current_client? server.current_client : c;

    if (current_cmd_blocking_offset > tracking_client->clientDurabilityInfo.current_command_repl_offset) {
        tracking_client->clientDurabilityInfo.current_command_repl_offset = current_cmd_blocking_offset;
    }

    // TODO: handle db level modifications like FLUSHALL, SWAPDB
}

/**
 * Perform pre-processing before command execution for a given client.
 *
 * For non-administrative commands that is either read or write, we will track the pre-execution positions
 * in the reply COB of the client and all the connected monitors.
 */
int preCommandExec(struct client *c) {
    serverLog(LOG_DEBUG, "preCommandExec hook entered for command '%s'", 
              c->cmd ? c->cmd->declared_name : "NULL");
    if (!isDurabilityEnabled()) {
        serverLog(LOG_DEBUG, "preCommandExec hook: durability not enabled, allowing");
        return CMD_FILTER_ALLOW;
    }

    // Reset the client current command replication offset
    c->clientDurabilityInfo.current_command_repl_offset = -1;

    if (shouldRejectCommandWithUncommittedData(c)) {
        serverAssert(!(c->cmd->flags & CMD_WRITE));
        flagTransaction(c);
        addReplyError(c, DURABLE_ACCESSED_DATA_UNAVAILABLE);
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
void postCommandExec(struct client *c) {
    if (!isPrimaryDurabilityEnabled()) {
        return;
    }
    serverLog(LOG_DEBUG, "postCommandExec hook entered for command '%s'", 
              c->cmd ? c->cmd->declared_name : "NULL");
    // If the command is NULL or is in a MULTI/EXEC block, then we skip
    // TODO: handle multi
    if(c->cmd == NULL || c->flag.multi) {
        return;
    }

    // Block the client (TODO:monitors) based on the required replication offset
    // for the current command.
    long long blocking_repl_offset = c->clientDurabilityInfo.current_command_repl_offset;
    
    // If the client needs to block, we need to enforce that it is eligible for response tracking.
    // Otherwise we will try to block the response without tracking the command's pre-execution
    // position in the client reply buffer, which wouldn't work. If this assert fails, then we
    // need to fix clientEligibleForResponseTracking() to handle the problematic command.
    if (blocking_repl_offset > server.durability.pre_command_replication_offset) {
        serverAssert(clientEligibleForResponseTracking(c));
    }

    blockClientAndMonitorsOnReplOffset(c, blocking_repl_offset);
}

/**
 * Function used to initialize the durability datastructures.
 * TODO: exit clean up?
 */ 
void durableInit(void) {
    // Initialize synchronous replication
    server.durability.previous_acked_offset = -1;
    server.durability.curr_db_scan_idx = 0;
    server.durability.clients_waiting_replica_ack = listCreate();
}

