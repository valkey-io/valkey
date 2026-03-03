#ifndef REPLY_BLOCKING_H
#define REPLY_BLOCKING_H

#include <inttypes.h>
#include <sys/types.h>
#include <stdbool.h>
#include "expire.h"
#include "sds.h"

/*================================= Durability Provider Interface ============ */

/**
 * Maximum number of durability providers that can be registered.
 * Built-in providers: replica, aof. Future: raft, quorum, etc.
 */
#define MAX_DURABILITY_PROVIDERS 4

/**
 * A durability provider represents a source of durability acknowledgment.
 * Each provider tracks progress independently and the overall durability
 * consensus is the MIN (AND) of all enabled providers' acknowledged offsets.
 *
 * Examples: replica acknowledgments, AOF fsync, raft consensus.
 */
typedef struct durabilityProvider {
    const char *name;                    /* Human-readable name, e.g. "replica", "aof" */
    bool (*isEnabled)(void);             /* Is this provider currently active? */
    long long (*getAckedOffset)(void);   /* What offset has this provider acknowledged? */
} durabilityProvider;

void registerDurabilityProvider(durabilityProvider *provider);
void unregisterDurabilityProvider(durabilityProvider *provider);
void notifyDurabilityProgress(void);
bool anyDurabilityProviderEnabled(void);

#define SYNC_REPL_ACCESSED_DATA_UNAVAILABLE "Accessed data unavailable to be served"
/* Command filter codes that are used in pre execution stage of a command. */
#define CMD_FILTER_ALLOW 0
#define CMD_FILTER_REJECT 1
// Returns true if the cmd is a script command that never replicates.
#define IS_SCRIPT_CALL_READONLY_CMD(cmd) ((cmd) && (        \
((cmd)->proc == fcallroCommand)  \
|| ((cmd)->proc == evalRoCommand)   \
|| ((cmd)->proc == evalShaRoCommand)))

// Returns true if the cmd is a script command
// (EVAL/EVAL_RO/EVALSHA/EVALSHA_RO/FCALL/FCALL_RO).
#define IS_SCRIPT_CALL_CMD(cmd) ((cmd) && (                 \
((cmd)->proc == fcallCommand)    \
|| ((cmd)->proc == fcallroCommand)  \
|| ((cmd)->proc == evalCommand)     \
|| ((cmd)->proc == evalRoCommand)   \
|| ((cmd)->proc == evalShaCommand)  \
|| ((cmd)->proc == evalShaRoCommand)))

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
    /* Flag to enable/disable sync replication (durability) */
    int sync_replication_enabled;
    /* Uncommitted keys cleanup configuration time limit in milliseconds */
    unsigned int keys_cleanup_time_limit_ms;
    /* The current scanning database index, starting from 0 */
    int curr_db_scan_idx;

    /* Number of replicas to ack for an update to be considered committed */
    long long num_replicas_to_ack;

    /* clients waiting for offset ack/quorum*/
    struct list *clients_waiting_replica_ack;

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
    
    /* Counters for debugging/monitoring */
    long long stat_read_blocked_count;   /* Number of read commands that got blocked */
    long long stat_write_blocked_count;  /* Number of write commands that got blocked */
} durable_t;

// Blocked response structure used by client to mark
// the blocking information associated with each response
typedef struct blockedResponse {
    // Pointer to the client's reply node where the blocked response starts.
    // NULL if the blocked response starts from the 16KB initial buffer
    // Here we don't take ownership of this pointer so we never
    // release the memory pointed to by this block.
    struct listNode *disallowed_reply_block;
    // The boundary in the reply buffer where the blocked response starts.
    // We don't write data from this point onwards to the client socket
    size_t disallowed_byte_offset;
    // The replication offset to wait for ACK from replicas
    long long primary_repl_offset;
} blockedResponse;

// Describes a pre-execution COB offset for a client
typedef struct preExecutionOffsetPosition {
    // True if the pre execution offset/reply block are initialized
    bool recorded;
    // Track initial client COB position for client blocking
    // Pointer to the pre-execution reply node, NULL for initial buffer
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

    uint64_t durable_blocked_client : 1; /* This is a durable blocked client that is waiting for the server to
                                          * acknowledge the write of the command that caused it to be blocked. */
} clientDurableInfo;

/**
 * Init
 */
void syncReplicationInit(void);
void syncReplicationCleanup(void);
void syncReplicationReset(void);
void syncReplicationClientInit(struct client *c);
void syncReplicationClientReset(struct client *c);
void syncReplicationClearPrimaryState(void);

/**
  Command processing hooks for offset and cob tracking
*/
void beforeCommandTrackReplOffset(void);
void afterCommandTrackReplOffset(client *c);
int preCommandExec(client *c);
char *preScriptCmd(client *c);
void postCommandExec(client *c);
void postReplicaAck(void);
void postAofFsync(void);
/**
    Utils
*/
int isPrimarySyncReplicationEnabled(void);
bool isClientReplyBufferLimited(client *c);
long long syncReplicationPurgeAndGetUncommittedKeyOffset(sds key, serverDb *db);

int isSyncReplicationEnabled(void);
void clearUncommittedKeysAcknowledged(void);
sds genSyncReplicationInfoString(sds info);
void syncReplicationInitDatabase(serverDb *db);
void handleUncommittedKeyForClient(const client *c, struct serverObject *key, serverDb *db);
// TODO:
//  preReplyToBlockedClient
// for streams and timeounts, when a blocked client is being unblocked
// before a reply is added, the command will not be reprocessed via processCommand()
// we should hook this to get pre-execution offsets


#endif /* REPLY_BLOCKING_H */
