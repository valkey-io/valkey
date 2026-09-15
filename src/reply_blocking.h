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
#include "post_commit_task.h"

/* Command filter codes that are used in pre execution stage of a command. */
#define CMD_FILTER_ALLOW 0
#define CMD_FILTER_REJECT 1
/* Returns true if the cmd is a script command that never replicates. */
#define IS_SCRIPT_CALL_READONLY_CMD(cmd) ((cmd) && (((cmd)->proc == fcallroCommand) || ((cmd)->proc == evalRoCommand) || ((cmd)->proc == evalShaRoCommand)))

/* Returns true if the cmd is a script command
 * (EVAL/EVAL_RO/EVALSHA/EVALSHA_RO/FCALL/FCALL_RO). */
#define IS_SCRIPT_CALL_CMD(cmd) ((cmd) && (((cmd)->proc == fcallCommand) || ((cmd)->proc == fcallroCommand) || ((cmd)->proc == evalCommand) || ((cmd)->proc == evalRoCommand) || ((cmd)->proc == evalShaCommand) || ((cmd)->proc == evalShaRoCommand)))

/* Returns true if the cmd is a whole-keyspace command — one that reads the
 * entire keyspace with no key argument, so its result depends on any
 * uncommitted write and it must block on the global replication offset.
 * Membership is defined by the CMD_KEYSPACE_GLOBAL flag in the command JSON
 * specs (see its definition in commands.h for the criterion). */
#define IS_KEYSPACE_GLOBAL(cmd) ((cmd) && ((cmd)->flags & CMD_KEYSPACE_GLOBAL))

/* Flags below help in correctly classifying transactions as
 * either read/write commands or non-keyspace commands. */
#define REPLY_BLOCKING_CLIENT_LAST_CMD_WRITE (1ULL << 0)
#define REPLY_BLOCKING_CLIENT_LAST_CMD_READONLY (1ULL << 1)

struct client;
struct serverObject;
struct serverDb;
struct list;
struct listNode;

typedef long long mstime_t;

/* Reply-blocking state container. */
typedef struct reply_blocking_t {
    /* Clients waiting for the durably-committed offset to advance */
    struct list *clients_waiting_ack;

    /* Deferred tasks waiting for the durably-committed offset to advance */
    struct list *tasks_waiting_ack[POST_COMMIT_TASK_TYPE_MAX];

    /* Pending lists of tasks waiting for reply-blocking ack. This list is populated
     * when the current command is under execution but before we know about the
     * updated primary_repl_offset. After the command execution completes, the
     * server.primary_repl_offset would get incremented and we need to update
     * this list and move all the pending tasks to the official
     * tasks_waiting_ack list as part of the post-execution logic
     */
    struct list *pending_tasks_waiting_ack[POST_COMMIT_TASK_TYPE_MAX];

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

    /* Counter of how many clients are reply-blocked */
    unsigned long long clients_blocked;
    /* Counter of how many clients are reply-unblocked */
    unsigned long long clients_unblocked;
    /* Counter of how many clients are disconnected before being reply-unblocked */
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

    /* Function store blocking offset: tracks the replication offset at which
     * the function store was last modified and needs reply-blocking acknowledgement. */
    long long func_store_blocking_offset;

    /* Flag indicating a function write occurred inside a transaction, so the
     * blocking offset should be updated when the transaction completes. */
    bool processed_func_write_in_transaction;

    /* When true (set via DEBUG reply-blocking-pause aof), the durably
     * committed offset is frozen at aof_paused_offset to pause reply-blocking
     * progress for testing. */
    bool aof_paused;

    /* Snapshot of the AOF-acked offset captured at pause time so that writes
     * already acknowledged remain unblocked while new writes block. */
    long long aof_paused_offset;

    /* True while executeDeferredTasksForAck is running deferred post-commit
     * tasks. Used by notifyKeyspaceEvent to tell a first-pass notification
     * (notify modules inline + defer the client pub/sub message) apart from the
     * re-fired client notification driven by the deferred task at ack time. */
    bool in_post_commit_task_execution;
} reply_blocking_t;

/* Define the type of command being blocked */
typedef enum {
    REPLY_BLOCKED_CMD_OTHER = 0,
    REPLY_BLOCKED_CMD_WRITE,
    REPLY_BLOCKED_CMD_READ
} replyBlockedCmdType;

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
    replyBlockedCmdType cmd_type;
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
    /* Blocked client responses list for reply-blocking */
    struct list *blocked_responses;

    /* Pre-execution data recorded before a command is executed
     * to record the boundaries of the COB. */
    preExecutionOffsetPosition offset;

    /* Replication offset to block this current command response */
    long long current_command_repl_offset;

    /* Durability blocking offset for a reply parked in the deferred-reply buffer by a
     * module that blocked the client on its KSN callback; applied when the reply is
     * committed into the COB. -1 when not deferred. */
    long long deferred_block_offset;

    /* The list of async notification tasks that reference this client */
    struct list *pending_notify_tasks;

    /* This client is waiting for reply-blocking providers to acknowledge
     * the write before its response can be sent. */
    uint64_t reply_blocked : 1;
    /* Modules can set the blocking offset for read cmds */
    long long module_cmd_blocking_offset;

    uint64_t reply_blocking_flags;
} clientReplyBlockingState;

/* Init / Lifecycle */
void replyBlockingInit(void);
void replyBlockingCleanup(void);
void replyBlockingReset(void);
void replyBlockingClientInit(struct client *c);
void replyBlockingClientReset(struct client *c);
void replyBlockingClearPrimaryState(void);

/* Command processing hooks for offset and COB tracking */
void recordReplOffsetBaseline(client *c);
void computeCommandBlockingOffset(client *c);
int beginCommandReplyBlocking(client *c);
char *validateScriptForReplyBlocking(client *c);
void finalizeCommandReplyBlocking(client *c);
void notifyReplyBlockingProgress(void);

/* Response blocking */
void blockClientOnReplOffset(client *c, long long blockingReplOffset);
void unblockResponsesWithAckOffset(const reply_blocking_t *rb_state, long long consensus_ack_offset);

/* Deferred-reply (module client-blocking) durability hooks, called from
 * commitDeferredReplyBuffer around the move of a parked reply into the COB. */
void replyBlockingSnapshotBeforeDeferredReplyCommit(client *c);
void replyBlockingApplyDeferredReplyBoundary(client *c);

/* Utils */
int isPrimaryReplyBlockingEnabled(void);
int isReplyBlockingEnabled(void);
int isAofReplyBlockingEnabled(void);
long long getDurablyCommittedOffset(void);
void pauseAofReplyBlocking(void);
void resumeAofReplyBlocking(void);
bool isClientReplyBufferLimited(client *c);
sds genReplyBlockingInfoString(sds info);

/* Function store dirty tracking (reply-blocking for function store writes) */
bool isFunctionRWCommand(struct client *c);
bool isFunctionStoreRWCommand(struct client *c);
bool isUncommittedFunctionStore(void);
void handleUncommittedFunctionStore(void);
void updateFuncStoreBlockingOffsetForWrite(long long blocking_repl_offset);
long long getFuncStoreBlockingOffset(void);

#endif /* REPLY_BLOCKING_H */
