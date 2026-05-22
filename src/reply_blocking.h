#ifndef REPLY_BLOCKING_H
#define REPLY_BLOCKING_H

/* Include feature-test macros early so _FILE_OFFSET_BITS=64 is defined
 * before any system headers, ensuring off_t is 64-bit on 32-bit builds. */
#include "fmacros.h"

#include <inttypes.h>
#include <sys/types.h>
#include <stdbool.h>
#include "expire.h"
#include "monotonic.h"
#include "sds.h"
#include "uncommitted_keys.h"
#include "durable_task.h"

/* Command filter codes that are used in pre execution stage of a command. */
#define CMD_FILTER_ALLOW 0
#define CMD_FILTER_REJECT 1
/* Returns true if the cmd is a script command that never replicates. */
#define IS_SCRIPT_CALL_READONLY_CMD(cmd) ((cmd) && (((cmd)->proc == fcallroCommand) || ((cmd)->proc == evalRoCommand) || ((cmd)->proc == evalShaRoCommand)))

/* Returns true if the cmd is a script command
 * (EVAL/EVAL_RO/EVALSHA/EVALSHA_RO/FCALL/FCALL_RO). */
#define IS_SCRIPT_CALL_CMD(cmd) ((cmd) && (((cmd)->proc == fcallCommand) || ((cmd)->proc == fcallroCommand) || ((cmd)->proc == evalCommand) || ((cmd)->proc == evalRoCommand) || ((cmd)->proc == evalShaCommand) || ((cmd)->proc == evalShaRoCommand)))

/* Returns true if the cmd is a keyspace informational command — a command that is
 * related to the keyspace (ACL_CATEGORY_KEYSPACE) but does not mutate it (not CMD_WRITE).
 * These commands provide information about the keyspace and need to be tracked for
 * durability response blocking even when they are admin or non-read/non-write commands. */
#define IS_KEYSPACE_INFORMATIONAL(cmd) ((cmd) && ((cmd)->acl_categories & ACL_CATEGORY_KEYSPACE) && !((cmd)->flags & CMD_WRITE))

/* Flags below help in correctly classifying transactions as
 * either read/write commands or non-keyspace commands. */
/* Indicates the client's last command was a mutative command. */
#define DURABILITY_CLIENT_LAST_CMD_WRITE (1ULL << 20)
/* Indicates the client's last command was read-only command. */
#define DURABILITY_CLIENT_LAST_CMD_READONLY (1ULL << 21)

struct client;
struct serverObject;
struct serverDb;
struct list;
struct listNode;

typedef long long mstime_t;

/* Indicate this type of notification is called inside of a durable task,
 * which is used by the durability feature to defer notifications. */
#define NOTIFY_IN_DURABLE_TASK (1 << 30)
/* Durability container to house all the durability related fields. */
typedef struct durable_t {
    /* Clients waiting for the durably-committed offset to advance */
    struct list *clients_waiting_ack;

    /* Deferred tasks waiting for the durably-committed offset to advance */
    struct list *tasks_waiting_ack[DURABLE_TASK_TYPE_MAX];

    /* Pending lists of tasks waiting for durability ack. This list is populated
     * when the current command is under execution but before we know about the
     * updated primary_repl_offset. After the command execution completes, the
     * server.primary_repl_offset would get incremented and we need to update
     * this list and move all the pending tasks to the official
     * tasks_waiting_ack list as part of the post-execution logic
     */
    struct list *pending_tasks_waiting_ack[DURABLE_TASK_TYPE_MAX];

    /* Previously acknowledged durably-committed replication offset */
    long long previous_acked_offset;

    /* Track the replication offset prior to executing a single command in call() */
    long long pre_call_replication_offset;

    /* Track the replication offset prior to executing a command block
     including single command and multi-command transactions */
    long long pre_command_replication_offset;

    /* Track the number of commands awaiting propagation prior to executing a single command in call() */
    int pre_call_num_ops_pending_propagation;

    /* Counters for stats / info */

    /* Counter of how many clients are blocked for durability */
    unsigned long long clients_blocked;
    /* Counter of how many clients are unblocked for durability */
    unsigned long long clients_unblocked;
    /* Counter of how many clients are disconnected before being unblocked for durability */
    unsigned long long clients_disconnected_before_unblocking;
    /* Counter of how many responses are blocked/unblocked by type */
    unsigned long long read_responses_blocked;
    unsigned long long write_responses_blocked;
    unsigned long long other_responses_blocked;
    unsigned long long read_responses_unblocked;
    unsigned long long write_responses_unblocked;
    unsigned long long other_responses_unblocked;

    /* Cumulative times for all the blocked responses */
    unsigned long long read_responses_blocked_cumulative_time_us;
    unsigned long long write_responses_blocked_cumulative_time_us;
    unsigned long long other_responses_blocked_cumulative_time_us;

    /* Tracks whether all databases were dirtied during the current command
     * within a multi-command block (MULTI/EXEC or Lua script). */
    bool all_dbs_dirty_in_current_cmd;

    /* Function store blocking offset: tracks the replication offset at which
     * the function store was last modified and needs durability acknowledgement. */
    long long func_store_blocking_offset;

    /* Flag indicating a function write occurred inside a transaction, so the
     * blocking offset should be updated when the transaction completes. */
    bool processed_func_write_in_transaction;

    /* When true (set via DEBUG reply-blocking-pause aof), the durably
     * committed offset is frozen at aof_paused_offset to halt durability
     * progress for testing. */
    bool aof_paused;

    /* Snapshot of the AOF-acked offset captured at pause time so that writes
     * already acknowledged remain unblocked while new writes block. */
    long long aof_paused_offset;
} durable_t;

/* Define the type of command being blocked */
typedef enum {
    DURABLE_BLOCKED_CMD_OTHER = 0,
    DURABLE_BLOCKED_CMD_WRITE,
    DURABLE_BLOCKED_CMD_READ
} durableBlockedCmdType;

/* Blocked response structure used by client to mark
 * the blocking information associated with each response */
typedef struct blockedResponse {
    /* Pointer to the client's reply node where the blocked response starts.
     * NULL if the blocked response starts from the 16KB initial buffer */
    struct listNode *disallowed_reply_block;
    /* The boundary in the reply buffer where the blocked response starts. */
    size_t disallowed_byte_offset;
    /* The replication offset to wait for the durably-committed offset to reach */
    long long primary_repl_offset;

    /* Enum to store the type of blocked command */
    durableBlockedCmdType cmd_type;
    /* Timer for blocked command */
    monotime blocked_command_timer;
} blockedResponse;

/* Describes a pre-execution COB offset for a client */
typedef struct preExecutionOffsetPosition {
    /* True if the pre execution offset/reply block are initialized */
    bool recorded;
    /* Track initial client COB position for client blocking */
    struct listNode *reply_block;
    /* Byte position boundary within the pre-execution reply block */
    size_t byte_offset;
} preExecutionOffsetPosition;

typedef struct clientReplyBlockingState {
    /* Blocked client responses list for durability */
    struct list *blocked_responses;

    /* Pre-execution data recorded before a command is executed
     * to record the boundaries of the COB. */
    preExecutionOffsetPosition offset;

    /* Replication offset to block this current command response */
    long long current_command_repl_offset;

    /* The list of async notification tasks that reference this client */
    struct list *pending_notify_tasks;

    /* This client is waiting for durability providers to acknowledge
     * the write before its response can be sent. */
    uint64_t durability_blocked : 1;
    /* Modules can set the blocking offset for read cmds */
    long long module_cmd_blocking_offset;

    uint64_t durability_flags;
} clientReplyBlockingState;

/* Init / Lifecycle */
void replyBlockingInit(void);
void replyBlockingCleanup(void);
void replyBlockingReset(void);
void durabilityClientInit(struct client *c);
void durabilityClientReset(struct client *c);
void durabilityClearPrimaryState(void);

/* Command processing hooks for offset and COB tracking */
void beforeCommandTrackReplOffset(client *c);
void afterCommandTrackReplOffset(client *c);
int preCommandExec(client *c);
char *preScriptCmd(client *c);
void postCommandExec(client *c);
void notifyDurabilityProgress(void);

/* Response blocking */
void blockClientOnReplOffset(client *c, long long blockingReplOffset);
void unblockResponsesWithAckOffset(const durable_t *durability, long long consensus_ack_offset);

/* Utils */
int isPrimaryDurabilityEnabled(void);
int isDurabilityEnabled(void);
int isAofDurabilityEnabled(void);
long long getDurablyCommittedOffset(void);
void pauseAofDurability(void);
void resumeAofDurability(void);
bool isClientReplyBufferLimited(client *c);
sds genDurabilityInfoString(sds info);

/* Function store dirty tracking (durability blocking for function store writes) */
bool isFunctionRWCommand(struct client *c);
bool isFunctionStoreRWCommand(struct client *c);
bool isDurableFunctionStoreUncommitted(void);
void handleUncommittedFunctionStore(void);
void updateFuncStoreBlockingOffsetForWrite(long long blocking_repl_offset);
long long getFuncStoreBlockingOffset(void);

#endif /* REPLY_BLOCKING_H */
