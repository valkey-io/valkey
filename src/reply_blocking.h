#ifndef REPLY_BLOCKING_H
#define REPLY_BLOCKING_H

#include <inttypes.h>
#include <sys/types.h>
#include <stdbool.h>
#include "expire.h"
#include "sds.h"
#include "durability_provider.h"
#include "uncommitted_keys.h"

/**
 * Define the supported task types
 */
/* Renamed from amzDurableTaskType / AMZ_* prefix */
typedef enum {
    DURABLE_KEYSPACE_NOTIFY_TASK = 0, /* KEYSPACE NOTIFY task */
    DURABLE_REPLCONF_ACK_NOTIFY_TASK, /* Replconf ack task */
    DURABLE_KEY_INVALIDATION_TASK,    /* Key invalidation task for client side caching */
    DURABLE_FLUSH_INVALIDATION_TASK,  /* FLUSH invalidation task for client side caching */
    DURABLE_TASK_TYPE_MAX             /* Max task type */
} durableTaskType;

#define SYNC_REPL_ACCESSED_DATA_UNAVAILABLE "Accessed data unavailable to be served"
/* Command filter codes that are used in pre execution stage of a command. */
#define CMD_FILTER_ALLOW 0
#define CMD_FILTER_REJECT 1
// Returns true if the cmd is a script command that never replicates.
#define IS_SCRIPT_CALL_READONLY_CMD(cmd) ((cmd) && (((cmd)->proc == fcallroCommand) || ((cmd)->proc == evalRoCommand) || ((cmd)->proc == evalShaRoCommand)))

// Returns true if the cmd is a script command
// (EVAL/EVAL_RO/EVALSHA/EVALSHA_RO/FCALL/FCALL_RO).
#define IS_SCRIPT_CALL_CMD(cmd) ((cmd) && (((cmd)->proc == fcallCommand) || ((cmd)->proc == fcallroCommand) || ((cmd)->proc == evalCommand) || ((cmd)->proc == evalRoCommand) || ((cmd)->proc == evalShaCommand) || ((cmd)->proc == evalShaRoCommand)))

struct client;
struct serverObject;
struct serverDb;
struct list;
struct listNode;

typedef long long mstime_t;

/**
 * Durability container to house all the durability related fields.
 */
typedef struct durable_t {
    /* Flag to enable/disable sync replication */
    int sync_replication_enabled;
    /* Uncommitted keys cleanup configuration time limit in milliseconds */
    unsigned int keys_cleanup_time_limit_ms;
    /* The current scanning database index, starting from 0 */
    int curr_db_scan_idx;

    /* DKT: clients waiting for offset
     * acknowledgement from a set of replicas */
    struct list *clients_waiting_replica_ack;

    /* Zero data loss: functions waiting for offset
     * acknowledgement from a set of replicas */
    struct list *tasks_waiting_replica_ack[DURABLE_TASK_TYPE_MAX];

    /* Pending lists of tasks waiting for replica ACK. This list is populated
     * when the current command is under execution but before we know about the
     * updated master_repl_offset. After the command execution completes, the
     * server.primary_repl_offset would get incremented and we need to update
     * this list and move all the pending tasks to the official
     * tasks_waiting_replica_ack list as part of the post-execution logic
     */
    struct list *pending_tasks_waiting_replica_ack[DURABLE_TASK_TYPE_MAX];


    /*  cached allocation of replica offsets to prevent allocation per cmd. */
    unsigned long replica_offsets_size;
    long long *replica_offsets;

    /* Previously acknowledged replication offset by replicas */
    long long previous_acked_offset;

    /* Track the replication offset prior to executing a single command in call() */
    long long pre_call_replication_offset;

    /* Track the replication offset prior to executing a command block
     including single command and multi-command transactions */
    long long pre_command_replication_offset;

    /* Track the number of commands awaiting propagation prior to executing a single command in call() */
    int pre_call_num_ops_pending_propagation;

    /* Counters for stats / info */

    /* Counter of how many clients are blocked for synchronous write */
    unsigned long long clients_blocked_on_sync_write;
    /* Counter of how many clients are unblocked for synchronous write */
    unsigned long long clients_unblocked_on_sync_write;
    /* Counter of how many clients are disconnected before being unblocked for sync write */
    unsigned long long clients_disconnected_before_unblocking_on_sync_write;
    /* Counter of how many commands are blocked/unblocked for synchronous write */
    unsigned long long read_responses_blocked_on_sync_write;
    unsigned long long write_responses_blocked_on_sync_write;
    unsigned long long other_responses_blocked_on_sync_write;
    unsigned long long read_responses_unblocked_on_sync_write;
    unsigned long long write_responses_unblocked_on_sync_write;
    unsigned long long other_responses_unblocked_on_sync_write;

    /* Cumulative Times for all the blocked commands */
    unsigned long long read_responses_blocked_on_sync_write_cumulative_time_us;
    unsigned long long write_responses_blocked_on_sync_write_cumulative_time_us;
    unsigned long long other_responses_blocked_on_sync_write_cumulative_time_us;

    /* Tracks whether all databases were dirtied during the current command
     * within a multi-command block (MULTI/EXEC or Lua script). */
    bool all_dbs_dirty_in_current_cmd;

    /* Function store blocking offset: tracks the replication offset at which
     * the function store was last modified and needs durability acknowledgement. */
    long long func_store_blocking_offset;

    /* Flag indicating a function write occurred inside a transaction, so the
     * blocking offset should be updated when the transaction completes. */
    bool processed_func_write_in_transaction;
} durable_t;

// Blocked response structure used by client to mark
// the blocking information associated with each response
typedef struct blockedResponse {
    // Pointer to the client's reply node where the blocked response starts.
    // NULL if the blocked response starts from the 16KB initial buffer
    struct listNode *disallowed_reply_block;
    // The boundary in the reply buffer where the blocked response starts.
    size_t disallowed_byte_offset;
    // The replication offset to wait for ACK from replicas
    long long primary_repl_offset;
} blockedResponse;

// Describes a pre-execution COB offset for a client
typedef struct preExecutionOffsetPosition {
    // True if the pre execution offset/reply block are initialized
    bool recorded;
    // Track initial client COB position for client blocking
    struct listNode *reply_block;
    // Byte position boundary within the pre-execution reply block
    size_t byte_offset;
} preExecutionOffsetPosition;

typedef struct clientDurabilityInfo {
    // Blocked client responses list for consistency
    struct list *blocked_responses;

    /* Pre-execution data recorded before a command is executed
     * to record the boundaries of the COB. */
    preExecutionOffsetPosition offset;

    // Replication offset to block this current command response
    long long current_command_repl_offset;

    // The list of async notification tasks that reference this client
    struct list *pending_notify_tasks;

    // This is a durable blocked client that is waiting for the server to
    // acknowledge the write of the command that caused it to be blocked.
    uint64_t durable_blocked_client : 1;
    // Modules can set the blocking offset for read cmds
    long long module_cmd_blocking_offset;
} clientDurableInfo;

/**
 * Init / Lifecycle
 */
void syncReplicationInit(void);
void syncReplicationCleanup(void);
void syncReplicationReset(void);
void syncReplicationClientInit(struct client *c);
void syncReplicationClientReset(struct client *c);
void syncReplicationClearPrimaryState(void);

/**
 * Command processing hooks for offset and COB tracking
 */
void beforeCommandTrackReplOffset(void);
void afterCommandTrackReplOffset(client *c);
int preCommandExec(client *c);
char *preScriptCmd(client *c);
void postCommandExec(client *c);
void postReplicaAck(void);
void postAofFsync(void);
void notifyDurabilityProgress(void);

/**
 * Response blocking
 */
void blockClientOnReplOffset(client *c, long long blockingReplOffset);
void blockLastResponseIfExist(const client *c, long long blocked_offset);
void unblockResponsesWithAckOffset(const durable_t *durability, long long consensus_ack_offset);

/**
 * Utils
 */
int isPrimarySyncReplicationEnabled(void);
int isSyncReplicationEnabled(void);
bool isClientReplyBufferLimited(client *c);
sds genSyncReplicationInfoString(sds info);

/**
 * Function store dirty tracking (durability blocking for function store writes)
 * Renamed from amzCommands_* / amzDurableFunctions_* prefix
 */
bool isFunctionRWCommand(struct client *c);
bool isFunctionStoreRWCommand(struct client *c);
bool isDurableFunctionStoreUncommitted(void);
void handleUncommittedFunctionStore(void);
void updateFuncStoreBlockingOffsetForWrite(long long blocking_repl_offset);
long long getFuncStoreBlockingOffset(void);

#endif /* REPLY_BLOCKING_H */
