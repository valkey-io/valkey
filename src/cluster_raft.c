/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* cluster_raft.c — Raft-based cluster bus implementation.
 *
 * All cluster metadata changes (node membership, slot ownership, replication
 * topology) are replicated through a Raft consensus log. The Raft leader is
 * independent of the data primary/replica role.
 *
 * Invariants:
 * - server.cluster->size counts committed voting members. It is initialized
 *   to 0, incremented by NODE_JOIN/ADD_VOTER apply, and decremented by
 *   NODE_FORGET/DEL_VOTER apply when the affected node is a voter. (This is
 *   used for computing the quorum.)
 * - The first log entry referencing any node is its NODE_JOIN. Other
 *   entry types (SLOT_CHANGE, SET_REPLICA_OF, etc.) are only proposed
 *   after the node's NODE_JOIN is in the log.
 * - CLUSTER_NODE_JOINING means "not yet in the raft log" — set on all
 *   nodes (including myself) at creation, cleared on NODE_JOIN apply. It's
 *   never persisted in nodes.conf or sent in snapshots.
 *
 * Constraints:
 * - AE, PRE_VOTE_REQ, and VOTE_REQ are never sent to nodes with CLUSTER_NODE_JOINING.
 *   This prevents AE from arriving before MEET on the same link. Learners receive
 *   AE but are excluded from PRE_VOTE_REQ, VOTE_REQ, and quorum accounting.
 * - Configuration (quorum) takes effect on apply, not on append.
 *   This allows multiple NODE_JOINs in flight simultaneously.
 * - A singleton leader keeps its log empty until it proposes its first
 *   real entry (inviting another node). This prevents log conflicts
 *   when two singletons at the same term merge via MEET.
 *
 * See design-docs/cluster-raft.md for wire protocol and message details.
 */

#include "server.h"
#include "hotkeys.h"
#include "cluster.h"
#include "cluster_bus.h"
#include "cluster_state.h"
#include "cluster_link.h"
#include "cluster_nodes.h"

#include <arpa/inet.h>
#include <stdbool.h>

/* Result codes for proposal pre-validation and apply-time validation.
 * Used for control flow instead of comparing human-readable error strings. */
typedef enum {
    RAFT_RESULT_OK = 0,      /* Proposal is valid / applied successfully. */
    RAFT_RESULT_STALE_EPOCH, /* Rejected due to stale shard epoch (retryable). */
    RAFT_RESULT_REJECTED,    /* Rejected for other reasons (terminal). */
} RaftProposalResult;

/* Human-readable error messages returned to clients via callbacks. */
#define GENERIC_PROPOSAL_REJECTION_MSG "proposal rejected by raft leader"
#define STALE_SHARD_EPOCH_REJECTION_MSG "proposal rejected due to stale shard epoch"

/* Wire-protocol tokens used in REJECT proposal between leader -> follower. */
#define REJECT_WIRE_CONFLICT "conflict" /* for stale shard epoch validation fail. */
#define REJECT_WIRE_REJECTED "rejected" /* for any other validation fail. */

/* Map a RaftProposalResult to a human-readable error string for client replies.
 * Returns NULL for RAFT_RESULT_OK (no error). */
static inline const char *raftProposalResultMsg(RaftProposalResult result) {
    switch (result) {
    case RAFT_RESULT_OK: return NULL;
    case RAFT_RESULT_STALE_EPOCH: return STALE_SHARD_EPOCH_REJECTION_MSG;
    case RAFT_RESULT_REJECTED: return GENERIC_PROPOSAL_REJECTION_MSG;
    }
    return GENERIC_PROPOSAL_REJECTION_MSG;
}

/* Map a RaftProposalResult to a wire-protocol token for REJECT messages. */
static inline const char *raftProposalResultWireReason(RaftProposalResult result) {
    return (result == RAFT_RESULT_STALE_EPOCH) ? REJECT_WIRE_CONFLICT : REJECT_WIRE_REJECTED;
}

/* Parse a wire-protocol REJECT reason token back to a RaftProposalResult. */
static inline RaftProposalResult raftProposalResultFromWire(const char *reason) {
    return (!strcasecmp(reason, REJECT_WIRE_CONFLICT)) ? RAFT_RESULT_STALE_EPOCH : RAFT_RESULT_REJECTED;
}

/* From module.c */
void moduleCallClusterReceivers(const char *sender_id, uint64_t module_id, uint8_t type, const unsigned char *payload, uint32_t len);

/* Shard epoch dict type, mapping shard_id to epoch. */
static dictType raftShardEpochDictType = {
    .entryGetKey = dictEntryGetKey,
    .hashFunction = dictSdsHash,
    .keyCompare = dictSdsKeyCompare,
    .entryDestructor = dictEntryDestructorSdsKey,
};

/* --------------------------------------------------------------------------
 * Raft log entry types — what gets replicated
 * -------------------------------------------------------------------------- */

enum raftEntryType {
    RAFT_ENTRY_NODE_JOIN = 1,      /* Add a node */
    RAFT_ENTRY_NODE_FORGET = 2,    /* Remove a node */
    RAFT_ENTRY_SLOT_CHANGE = 3,    /* Slot ownership */
    RAFT_ENTRY_SET_REPLICA_OF = 4, /* Replication topology */
    RAFT_ENTRY_FAILOVER = 5,       /* Failover (manual or automatic) */
    RAFT_ENTRY_NODE_INFO = 6,      /* IP, port, hostname, etc. */
    RAFT_ENTRY_NODE_FAIL = 7,      /* Node failure detected */
    RAFT_ENTRY_NODE_RECOVER = 8,   /* Node recovery detected */
    RAFT_ENTRY_ADD_VOTER = 9,      /* Promote learner to voting member */
    RAFT_ENTRY_DEL_VOTER = 10,     /* Demote voting member to learner */
    RAFT_ENTRY_NOOP = 11,          /* No-op (leader term establishment) */
};

typedef struct {
    uint64_t term;
    uint64_t index;
    uint8_t type; /* enum raftEntryType */
    sds data;     /* Space-separated command arguments */
} raftLogEntry;

/* --------------------------------------------------------------------------
 * Raft roles
 * -------------------------------------------------------------------------- */

enum raftRole {
    RAFT_ROLE_JOINER = 0, /* Stepped down singleton waiting to be added to a cluster */
    RAFT_ROLE_LEARNER = 1,
    RAFT_ROLE_FOLLOWER = 2,
    RAFT_ROLE_PRE_CANDIDATE = 3,
    RAFT_ROLE_CANDIDATE = 4,
    RAFT_ROLE_LEADER = 5,
};

/* --------------------------------------------------------------------------
 * Helper types for pending operations
 * -------------------------------------------------------------------------- */

/* A pending proposal tracks a client waiting for a Raft entry to be
 * committed. Stored on the node that originated the proposal. */
typedef struct {
    uint8_t type; /* Expected entry type */
    sds data;     /* Expected entry data (for matching) */
    void *ctx;    /* Client context */
    void (*callback)(void *ctx, const char *error);
    mstime_t ctime;        /* Creation time, for the overall expiry cap */
    mstime_t propose_time; /* Last time it was sent to the leader, for expiry */
    int retries;           /* Remaining epoch-retry attempts */
    int deferred;          /* Waiting for epoch advance before re-propose */
} raftPendingProposal;

/* A pending meet tracks a CLUSTER MEET client waiting for the NODE_JOIN
 * to be committed. Matched by target ip:cport. */
typedef struct {
    sds addr; /* "ip:cport" */
    void *ctx;
    void (*callback)(void *ctx, const char *error);
} raftPendingMeet;

/* --------------------------------------------------------------------------
 * Protocol-specific state (stored in clusterState.protocol_data)
 * -------------------------------------------------------------------------- */

typedef struct {
    /* Persistent Raft state */
    uint64_t current_term;
    uint64_t last_applied;
    uint64_t snapshot_term;          /* Term of the entry at last_applied */
    char voted_for[CLUSTER_NAMELEN]; /* All zeros = none */

    /* Volatile Raft state */
    enum raftRole role;
    uint64_t commit_index;

    /* Log */
    raftLogEntry **log;
    uint64_t log_count;
    uint64_t log_alloc;
    uint64_t log_trim_index; /* Index of the last entry trimmed from the log, 0 = none */
    uint64_t log_trim_term;  /* Term of that entry */

    /* Pending proposals waiting for commit. */
    list *pending_proposals; /* list of raftPendingProposal */

    /* Pending MEET callbacks waiting for NODE_JOIN commit. */
    list *pending_meets;  /* CLUSTER MEET commands waiting for OK reply */
    list *deferred_meets; /* Names (sds) of nodes whose MEET we answer once we're in a multi-member cluster */

    clusterLink *processing_link; /* Link whose message is being processed, or NULL */

    /* Deferred work for beforeSleep. */
    unsigned int todo_update_slot_coverage : 1;
    unsigned int todo_connect_nodes : 1;
    unsigned int todo_broadcast_ae : 1;
    unsigned int todo_retry_proposals : 1;
    unsigned int todo_retry_deferred : 1; /* For proposals that need rebasing of new epoch */
    unsigned int todo_schedule_failover : 1;
    unsigned int todo_update_replication : 1;
    unsigned int todo_persist_log : 1;
    unsigned int todo_save_config : 1;
    unsigned int todo_send_ae_ack : 1;
    unsigned int todo_send_vote_response : 1;
    unsigned int todo_broadcast_vote_request : 1;
    uint64_t persist_log_from;     /* First index to persist in next batch */
    uint64_t last_rewrite_applied; /* last_applied at last full rewrite */

    /* NODE_INFO divergence detection. */
    sds my_last_committed_info;
    mstime_t last_node_info_check;

    /* Election */
    int votes_received;
    int pre_votes_received;
    mstime_t election_timeout;            /* Randomized timeout */
    mstime_t last_heartbeat;              /* Last time we heard from leader */
    mstime_t pre_vote_started;            /* Last time we started a pre-vote round */
    mstime_t last_repl_offsets_broadcast; /* Last REPL_OFFSETS broadcast */
    mstime_t joiner_since;                /* When we became a joiner (for timeout) */

    /* Leader identity */
    char leader[CLUSTER_NAMELEN]; /* All zeros = unknown */

    /* Manual failover state (on the replica side) */
    mstime_t mf_end;             /* Timeout for manual failover, 0 = not in progress */
    long long mf_primary_offset; /* Primary's offset after pausing, -1 = not received yet */
    void *mf_ctx;                /* blockedAsyncHandle for the CLUSTER FAILOVER client */
    void (*mf_callback)(void *ctx, const char *error);

    /* Automatic failover state (on the replica side) */
    mstime_t failover_time; /* When to propose FAILOVER based on rank, 0 = inactive */

    /* Message stats for CLUSTER INFO */
    long long stats_module_messages_sent;
    long long stats_module_messages_received;
    long long stats_publish_messages_sent;
    long long stats_publish_messages_received;
    uint64_t stats_bytes_sent;
    uint64_t stats_bytes_received;
    uint64_t stats_pubsub_bytes_sent;
    uint64_t stats_pubsub_bytes_received;
    uint64_t stats_module_bytes_sent;
    uint64_t stats_module_bytes_received;

    /* Shard epoch tracking (per-shard monotonic counter for stale proposal detection). */
    dict *shard_epochs;
} clusterRaftState;

#define RAFT_STATE() ((clusterRaftState *)server.cluster->protocol_data)

/* --------------------------------------------------------------------------
 * Per-node protocol data (stored in clusterNode.protocol_data)
 * -------------------------------------------------------------------------- */

typedef struct {
    uint64_t next_index;
    uint64_t match_index;
    mstime_t last_ack_time;               /* Last time we received AE_ACK from this peer */
    mstime_t last_snapshot_time;          /* Last time we sent INSTALL_SNAPSHOT to this peer */
    unsigned int pending_fail_change : 1; /* NODE_FAIL or NODE_RECOVER in flight */
} clusterNodeRaftData;

#define RAFT_NODE(n) ((clusterNodeRaftData *)(n)->protocol_data)

static bool nodeIsLearner(clusterNode *n) {
    return n->flags & CLUSTER_NODE_LEARNER;
}
static bool nodeIsVoter(clusterNode *n) {
    return !(n->flags & (CLUSTER_NODE_JOINING | CLUSTER_NODE_LEARNER));
}

/* --------------------------------------------------------------------------
 * Wire format helpers
 * -------------------------------------------------------------------------- */

#define RAFT_HDR_SIZE 8
#define REPL_OFFSETS_BROADCAST_PERIOD_MS 10000
#define RAFT_LOG_REWRITE_THRESHOLD 100
#define PROPOSAL_MAX_RETRIES 10
#define RAFT_TARGET_VOTERS 5

/* Monotonic millisecond clock for timeouts and failure detection.
 * Unlike gettimeofday(), this is not affected by system clock adjustments. */
static mstime_t monotonicMs(void) {
    return (mstime_t)(getMonotonicUs() / 1000);
}

/* Start building a message. Reserves space for the binary header and
 * appends the message type as the first text field. */
static sds wireNewMsg(const char *type) {
    sds buf = sdsnewlen(NULL, RAFT_HDR_SIZE);
    memcpy(buf, "RAFT", 4);
    /* totlen patched by wireFinishMsg */
    return sdscatfmt(buf, "%s", type);
}

/* Patch the totlen in the header after the message is fully built. */
static sds wireFinishMsg(sds buf) {
    uint32_t totlen = htonl(sdslen(buf));
    memcpy(buf + 4, &totlen, 4);
    return buf;
}

/* Send an sds message on a link. Frees the sds. */
static void clusterRaftSendMsg(clusterLink *link, sds msg) {
    if (!link || !link->conn) {
        sdsfree(msg);
        return;
    }
    size_t len = sdslen(msg);
    clusterMsgSendBlock *block = clusterAllocMsgSendBlock(len);
    memcpy(block->data, msg, len);
    sdsfree(msg);
    clusterLinkSendBlock(link, block);
    clusterMsgSendBlockDecrRefCount(block);
    RAFT_STATE()->stats_bytes_sent += len;
}

/* Broadcast a single node's replication offset to all connected peers. */
/* Broadcast a message to all connected peers (excluding myself).
 * If shard_filter is non-NULL, only send to nodes in that shard. */
static int clusterRaftBroadcast(sds msg, const char *shard_filter) {
    size_t msg_len = sdslen(msg);
    clusterMsgSendBlock *block = clusterAllocMsgSendBlock(msg_len);
    memcpy(block->data, msg, msg_len);
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    int sent = 0;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *n = dictGetVal(de);
        if (n == myself || !n->link) continue;
        if (shard_filter && memcmp(n->shard_id, shard_filter, CLUSTER_NAMELEN) != 0) continue;
        clusterLinkSendBlock(n->link, block);
        sent++;
    }
    dictReleaseIterator(di);
    clusterMsgSendBlockDecrRefCount(block);
    return sent;
}

static void clusterRaftBroadcastNodeOffset(clusterNode *node, long long offset) {
    sds msg = wireNewMsg("REPL_OFFSETS");
    msg = sdscatlen(msg, " ", 1);
    msg = sdscatlen(msg, node->name, CLUSTER_NAMELEN);
    msg = sdscatfmt(msg, " %I", offset);
    msg = wireFinishMsg(msg);
    clusterRaftBroadcast(msg, NULL);
    sdsfree(msg);
}

/* Build a REPL_OFFSETS message containing all known non-zero offsets.
 * Returns a refcounted send block, or NULL if there's nothing to send.
 * Caller must call clusterMsgSendBlockDecrRefCount when done. */
static clusterMsgSendBlock *clusterRaftBuildAllOffsetsMsg(void) {
    sds msg = wireNewMsg("REPL_OFFSETS");
    int count = 0;
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *n = dictGetVal(de);
        if (n->flags & CLUSTER_NODE_JOINING) continue;
        long long off = (n == myself) ? getNodeReplicationOffset(myself)
                                      : n->repl_offset;
        msg = sdscatlen(msg, " ", 1);
        msg = sdscatlen(msg, n->name, CLUSTER_NAMELEN);
        msg = sdscatfmt(msg, " %I", off);
        count++;
    }
    dictReleaseIterator(di);
    if (count == 0) {
        sdsfree(msg);
        return NULL;
    }
    msg = wireFinishMsg(msg);
    size_t msg_len = sdslen(msg);
    clusterMsgSendBlock *block = clusterAllocMsgSendBlock(msg_len);
    memcpy(block->data, msg, msg_len);
    sdsfree(msg);
    return block;
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Identity of a quorum-changing transition is (entry type, target node-id =
 * the first CLUSTER_NAMELEN bytes of the entry data). Used to enforce
 * "at most one membership transition in flight". */
typedef enum {
    RAFT_QC_NONE = 0, /* No quorum-changing entry in flight. */
    RAFT_QC_SAME,     /* The same transition is already in flight. */
    RAFT_QC_CONFLICT, /* A different transition is in flight. */
} raftQuorumChangeStatus;

static void clusterRaftPropose(sds entry, void *ctx, void (*callback)(void *ctx, const char *error));
static void clusterRaftDeferPendingProposals(void);
static void clusterRaftCompletePendingProposal(int type, sds data, RaftProposalResult result);
static RaftProposalResult clusterRaftPreValidate(int type, sds data);
static void clusterRaftAutoFailoverCallback(void *ctx, const char *error);
static void clusterRaftSlotChange(slotRange *ranges, int numranges, clusterNode *target, void *ctx, void (*callback)(void *ctx, const char *error));
static void clusterRaftUpdateMyself(int old_flags);
static sds clusterRaftBuildMyNodeInfo(void);
static void clusterRaftCheckSlotCoverage(void);
static void clusterRaftBroadcastAppendEntries(void);
static void clusterRaftSendAppendEntries(clusterLink *link, clusterNode *node);
static void clusterRaftSendPreVoteRequest(clusterLink *link, uint64_t term);
static void clusterRaftUnblockMeet(clusterNode *node);
static RaftProposalResult clusterRaftApplySlotChange(sds data, int validate_only);
static void clusterRaftProcessDeferredMeets(void);
static void clusterRaftUpdateLocalSlotState(int slot, clusterNode *old_owner, clusterNode *new_owner);
static RaftProposalResult clusterRaftApplySetReplica(sds data, int validate_only);
static RaftProposalResult clusterRaftApplyFailover(sds data, int validate_only);
static RaftProposalResult clusterRaftApplyNodeForget(sds data, int validate_only);
static RaftProposalResult clusterRaftApplyAddVoter(sds data, int validate_only);
static RaftProposalResult clusterRaftApplyDelVoter(sds data, int validate_only);
static void raftLogApply(raftLogEntry *e);
static raftLogEntry *raftLogCreate(uint64_t term, uint64_t index, uint8_t type, sds data);
static void raftLogAppend(raftLogEntry *e);
static raftLogEntry *raftLogGet(uint64_t index);
static uint64_t raftLogLastIndex(void);
static uint64_t raftLogTermAt(uint64_t index);
static void raftLogTruncateFrom(uint64_t index);
static int clusterRaftPersistNewLogEntries(uint64_t from);
static uint64_t raftLogLastTerm(void);
static int clusterRaftCanGrantVote(clusterRaftState *rs, uint64_t candidate_last_index, uint64_t candidate_last_term);
static void clusterRaftStartElection(void);
static bool raftEntryAffectsQuorum(int type, sds data);
static raftQuorumChangeStatus clusterRaftQuorumChangeStatus(int type, sds data);
static clusterNode *clusterRaftSelectPromotionCandidate(void);
static void clusterRaftAutoPromoteLearner(void);

static void clusterRaftRandomizeElectionTimeout(void) {
    mstime_t base = server.cluster_node_timeout;
    if (base < 1000) base = 1000;
    RAFT_STATE()->election_timeout = base + (rand() % base);
}

static int clusterRaftQuorum(void) {
    return server.cluster->size / 2 + 1;
}

/* True if there are any nodes other than myself without the JOINING flag. */
static bool clusterRaftHasPeerMembers(void) {
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node != myself && !(node->flags & CLUSTER_NODE_JOINING)) {
            dictReleaseIterator(di);
            return true;
        }
    }
    dictReleaseIterator(di);
    return false;
}

static bool clusterRaftIsSingleton(void) {
    return server.cluster->size <= 1 && !clusterRaftHasPeerMembers();
}

/* Step down on higher term / lost quorum.
 * Voters become followers; learners and joiners keep their role. */
static void clusterRaftStepDown(mstime_t now, const char *reason) {
    clusterRaftState *rs = RAFT_STATE();
    enum raftRole old_role = rs->role;
    clusterRaftDeferPendingProposals();
    if (rs->role != RAFT_ROLE_LEARNER && rs->role != RAFT_ROLE_JOINER)
        rs->role = RAFT_ROLE_FOLLOWER;
    rs->votes_received = 0;
    memset(rs->voted_for, 0, CLUSTER_NAMELEN);
    memset(rs->leader, 0, CLUSTER_NAMELEN);
    clusterRaftRandomizeElectionTimeout();
    rs->last_heartbeat = now;
    rs->todo_save_config = 1;
    if (rs->role != old_role)
        serverLog(LL_NOTICE, "Stepping down: %s.", reason);
}

/* Return non-zero if the leader still has a recently responsive voting
 * quorum based on follower AE_ACK timing. */
static int clusterRaftLeaderHasFreshQuorum(mstime_t now) {
    clusterRaftState *rs = RAFT_STATE();
    if (rs->role != RAFT_ROLE_LEADER || server.cluster->size <= 1) return 1;

    int fresh = 1; /* Self */
    int quorum = clusterRaftQuorum();
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node == myself) continue;
        if (!nodeIsVoter(node)) continue;
        clusterNodeRaftData *rd = RAFT_NODE(node);
        if (rd->last_ack_time > 0 &&
            now - rd->last_ack_time <= server.cluster_node_timeout) {
            fresh++;
            if (fresh >= quorum) break;
        }
    }
    dictReleaseIterator(di);
    return fresh >= quorum;
}

/* Singleton leader steps down to joiner when joining another cluster. */
static void clusterRaftSingletonStepDown(void) {
    clusterRaftState *rs = RAFT_STATE();
    clusterRaftDeferPendingProposals();
    rs->role = RAFT_ROLE_JOINER;
    rs->joiner_since = monotonicMs();
    memset(rs->leader, 0, CLUSTER_NAMELEN);
    memset(rs->voted_for, 0, CLUSTER_NAMELEN);
    clusterRaftRandomizeElectionTimeout();
    rs->last_heartbeat = monotonicMs();
    /* Clear singleton log — the leader's log is authoritative. */
    raftLogTruncateFrom(1);
    rs->last_applied = 0;
    rs->commit_index = 0;
}

/* Propose NODE_JOIN + SLOT_CHANGE for ourselves. Called once, before the
 * leader appends its first real entry. Ensures the leader's NODE_JOIN is
 * first in the log so followers can look up the leader on apply. */
static void clusterRaftSelfJoin(void) {
    clusterRaftState *rs = RAFT_STATE();
    serverAssert(rs->role == RAFT_ROLE_LEADER && raftLogLastIndex() == 0);

    sds entry = sdsnew("NODE_JOIN ");
    entry = sdscatlen(entry, myself->name, CLUSTER_NAMELEN);
    entry = sdscat(entry, " ");
    entry = clusterNodeAppendAddressString(entry, myself, server.tls_cluster);
    entry = sdscat(entry, " voter");
    clusterRaftPropose(entry, NULL, NULL);
    sdsfree(entry);

    /* Propose SLOT_CHANGE for slots assigned before any node joined. */
    sds slots = sdsempty();
    for (int j = 0; j < CLUSTER_SLOTS; j++) {
        if (server.cluster->slots[j] != myself) continue;
        int start = j;
        while (j + 1 < CLUSTER_SLOTS && server.cluster->slots[j + 1] == myself) j++;
        if (j == start)
            slots = sdscatfmt(slots, " %i", start);
        else
            slots = sdscatfmt(slots, " %i-%i", start, j);
    }
    if (sdslen(slots) > 0) {
        entry = sdsnew("SLOT_CHANGE - 0 ");
        entry = sdscatlen(entry, myself->name, CLUSTER_NAMELEN);
        entry = sdscat(entry, " 0");
        entry = sdscatsds(entry, slots);
        clusterRaftPropose(entry, NULL, NULL);
        sdsfree(entry);
    }
    sdsfree(slots);
}

/* Propose NODE_JOIN if sender is new to the cluster and I'm not new. */
static void clusterRaftInvitePeer(clusterNode *node) {
    clusterRaftState *rs = RAFT_STATE();

    /* Avoid duplicate NODE_JOIN in the log. */
    for (uint64_t i = 0; i < rs->log_count; i++) {
        if (rs->log[i]->type == RAFT_ENTRY_NODE_JOIN &&
            sdslen(rs->log[i]->data) >= CLUSTER_NAMELEN &&
            memcmp(rs->log[i]->data, node->name, CLUSTER_NAMELEN) == 0) {
            return;
        }
    }

    sds entry = sdsnew("NODE_JOIN ");
    entry = sdscatlen(entry, node->name, CLUSTER_NAMELEN);
    entry = sdscatlen(entry, " ", 1);
    entry = clusterNodeAppendAddressString(entry, node, server.tls_cluster);
    entry = sdscat(entry, " learner");

    clusterRaftPropose(entry, NULL, NULL);
    sdsfree(entry);
}

/* --------------------------------------------------------------------------
 * HELLO message: HELLO <node-id> <address-string>
 *
 * The address string uses the nodes.conf format:
 *   ip:port@cport[,hostname][,aux=val]*
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * Cluster bus greeting messages
 *
 * HELLO <node-id> <address>
 *   First message on every new outbound connection. Identifies the sender
 *   so the receiver can bind the inbound link. On MEET-initiated connections,
 *   HELLO is immediately followed by MEET.
 *
 * MEET singleton|cluster
 *   Sent after HELLO on MEET-initiated connections. Declares the sender's
 *   cluster status. The receiver decides based on its own status:
 *   - Receiver is non-singleton: invites sender (proposes NODE_JOIN),
 *     sends WELCOME after commit.
 *   - Receiver is singleton: steps down, replies ADDME.
 *
 * ADDME
 *   Reply to MEET: "I stepped down, please add me to your cluster."
 *   The MEET sender (now the leader) invites the peer.
 *
 * WELCOME
 *   Sent after NODE_JOIN is committed. Unblocks the CLUSTER MEET client
 *   on the joined node.
 * -------------------------------------------------------------------------- */

/* Send a greeting (HELLO or HI): verb + node-id + address. */
static void clusterRaftSendGreeting(clusterLink *link, const char *verb) {
    sds msg = wireNewMsg(verb);
    msg = sdscatlen(msg, " ", 1);
    msg = sdscatlen(msg, myself->name, CLUSTER_NAMELEN);
    msg = sdscatlen(msg, " ", 1);
    msg = clusterNodeAppendAddressString(msg, myself, server.tls_cluster);
    msg = wireFinishMsg(msg);
    clusterRaftSendMsg(link, msg);
}

/* Send MEET with singleton|cluster flag. */
static void clusterRaftSendMeet(clusterLink *link) {
    const char *flag = clusterRaftIsSingleton() ? "singleton" : "cluster";
    sds msg = wireNewMsg("MEET");
    msg = sdscatfmt(msg, " %s", flag);
    msg = wireFinishMsg(msg);
    clusterRaftSendMsg(link, msg);
}

/* Send a bare message (no payload): ADD_ME, WELCOME or MEET_REJECTED. */
static void clusterRaftSendBare(clusterLink *link, const char *verb) {
    sds msg = wireNewMsg(verb);
    msg = wireFinishMsg(msg);
    serverLog(LL_NOTICE, "Sending %s on %s link (conn=%p).", verb,
              link->inbound ? "inbound" : "outbound", (void *)link->conn);
    clusterRaftSendMsg(link, msg);
}

/* HELLO handler: received on an inbound link. Binds the link to the sender. */
static int clusterRaftProcessHello(clusterLink *link, int argc, sds *argv) {
    if (argc < 3 || sdslen(argv[1]) != CLUSTER_NAMELEN) return 1;
    char *sender_name = argv[1];

    serverLog(LL_DEBUG, "Received HELLO from %.40s.", sender_name);

    clusterNode *sender = clusterLookupNode(sender_name, CLUSTER_NAMELEN);
    if (!sender) {
        sender = createClusterNode(sender_name, CLUSTER_NODE_JOINING | CLUSTER_NODE_PRIMARY);
        clusterAddNode(sender);
    }

    /* Update sender address. */
    if (clusterNodeParseAddressString(sender, argv[2]) == C_ERR) {
        serverLog(LL_WARNING, "Bad address in HELLO from %.40s", sender_name);
        return 1;
    }
    if (sender->ip[0] == '\0') {
        connAddrPeerName(link->conn, sender->ip, sizeof(sender->ip), NULL);
    }

    /* Learn our own IP from the inbound connection if not yet known. */
    if (myself->ip[0] == '\0' && server.cluster_announce_ip == NULL) {
        char ip[NET_IP_STR_LEN];
        if (connAddrSockName(link->conn, ip, sizeof(ip), NULL) != -1 && strcmp(ip, myself->ip)) {
            memcpy(myself->ip, ip, NET_IP_STR_LEN);
            serverLog(LL_NOTICE, "IP address for this node updated to %s", myself->ip);
        }
    }

    /* Associate inbound link with sender. */
    if (sender->inbound_link && sender->inbound_link != link) {
        freeClusterLink(sender->inbound_link);
    }
    if (!link->node) {
        setClusterNodeToInboundClusterLink(sender, link);
    }

    clusterRaftSendGreeting(link, "HI");
    return 1;
}

/* HI handler: received on an outbound link as reply to HELLO.
 * Completes the handshake (renames handshake node to real ID). */
static int clusterRaftProcessHi(clusterLink *link, int argc, sds *argv) {
    if (argc < 3 || sdslen(argv[1]) != CLUSTER_NAMELEN) return 1;
    char *sender_name = argv[1];

    clusterNode *sender = clusterLookupNode(sender_name, CLUSTER_NAMELEN);
    if (link->node && nodeInHandshake(link->node)) {
        if (!sender) {
            clusterRenameNode(link->node, sender_name);
            link->node->flags &= ~CLUSTER_NODE_HANDSHAKE;
            clusterNodeParseAddressString(link->node, argv[2]);
            sender = link->node;
        } else if (sender != link->node) {
            clusterNode *handshake = link->node;
            link->node = sender;
            sender->link = link;
            handshake->link = NULL;
            clusterDelNode(handshake);
        }
        serverLog(LL_NOTICE, "Handshake with node %.40s completed.", sender_name);
    }

    clusterRaftState *rs = RAFT_STATE();

    /* If we just established a link to the leader and have deferred
     * proposals, trigger retry in the next beforeSleep. */
    if ((rs->role == RAFT_ROLE_FOLLOWER || rs->role == RAFT_ROLE_LEARNER) &&
        listLength(rs->pending_proposals) > 0 &&
        link->node && memcmp(link->node->name, rs->leader, CLUSTER_NAMELEN) == 0) {
        rs->todo_retry_proposals = 1;
    }

    /* If we're the leader, send AE immediately to minimize commit latency
     * after a reconnection (especially in 2-node clusters). */
    if (rs->role == RAFT_ROLE_LEADER && link->node &&
        !(link->node->flags & CLUSTER_NODE_JOINING)) {
        clusterRaftSendAppendEntries(link, link->node);
    }

    return 1;
}

/* MEET handler: received after HELLO on a MEET-initiated connection.
 * The link is already bound to the sender by the HELLO handler. */
static int clusterRaftProcessMeet(clusterLink *link, int argc, sds *argv) {
    if (argc < 2) return 1;
    if (!link->node) return 1; /* HELLO must come first. */

    clusterNode *sender = link->node;
    const char *flag = argv[1];
    int sender_is_singleton = !strcasecmp(flag, "singleton");

    serverLog(LL_NOTICE, "Received MEET %s from %.40s.", flag, sender->name);

    clusterRaftState *rs = RAFT_STATE();

    if (!clusterRaftIsSingleton() && !sender_is_singleton) {
        /* Both sides are in a cluster — reject. Merging non-singleton
         * clusters is not supported. */
        serverLog(LL_WARNING, "Rejecting MEET from %.40s: both sides are in a cluster.", sender->name);
        clusterRaftSendBare(link, "MEET_REJECTED");
    } else if (!clusterRaftIsSingleton()) {
        /* I'm in a cluster, sender is a singleton — reply WELCOME and
         * invite the sender. WELCOME tells the sender to step down. */
        clusterRaftSendBare(link, "WELCOME");
        clusterRaftInvitePeer(sender);
    } else if (rs->role == RAFT_ROLE_LEADER) {
        /* I'm a singleton leader — step down, reply ADD_ME. */
        clusterRaftSingletonStepDown();
        serverLog(LL_NOTICE, "Singleton stepping down on MEET from %.40s.", sender->name);
        clusterRaftSendBare(link, "ADD_ME");
        /* Unblock any pending CLUSTER MEET client — we've stepped down to
         * joiner, so we can't form a competing cluster. */
        clusterRaftUnblockMeet(sender);
    } else {
        /* Already a joiner (stepped down for another node). Defer until
         * we join a cluster and can invite the sender. */
        listAddNodeTail(rs->deferred_meets, sdsnewlen(sender->name, CLUSTER_NAMELEN));
        serverLog(LL_NOTICE, "Deferring MEET from %.40s (waiting to join cluster).", sender->name);
    }

    return 1;
}

/* Answer the MEETs we deferred while we were a joiner, now that we're in a
 * cluster. A node that has disconnected or been removed since is skipped. */
static void clusterRaftProcessDeferredMeets(void) {
    clusterRaftState *rs = RAFT_STATE();
    if (listLength(rs->deferred_meets) == 0 || !clusterRaftHasPeerMembers()) return;
    listIter li;
    listNode *ln;
    listRewind(rs->deferred_meets, &li);
    while ((ln = listNext(&li)) != NULL) {
        sds name = listNodeValue(ln);
        clusterNode *node = clusterLookupNode(name, CLUSTER_NAMELEN);
        if (node && node->inbound_link) {
            clusterRaftSendBare(node->inbound_link, "WELCOME");
            clusterRaftInvitePeer(node);
        }
    }
    listEmpty(rs->deferred_meets);
}

/* ADDME handler: the peer stepped down and wants us to add them. */
static int clusterRaftProcessAddme(clusterLink *link, int argc, sds *argv) {
    UNUSED(argc);
    UNUSED(argv);
    if (!link->node) return 1;

    clusterNode *sender = link->node;
    serverLog(LL_NOTICE, "Received ADDME from %.40s.", sender->name);

    /* Unblock CLUSTER MEET client — the peer has stepped down to joiner,
     * so it can't form a competing cluster. */
    clusterRaftUnblockMeet(sender);

    clusterRaftInvitePeer(sender);
    return 1;
}

/* WELCOME: reply to MEET from a cluster member.
 * The sender will add us — step down to joiner and unblock CLUSTER MEET. */
static int clusterRaftProcessWelcome(clusterLink *link, int argc, sds *argv) {
    UNUSED(argc);
    UNUSED(argv);
    if (!link->node) return 1;

    clusterNode *sender = link->node;
    serverLog(LL_NOTICE, "Received WELCOME from %.40s, stepping down.", sender->name);

    clusterRaftState *rs = RAFT_STATE();
    if (rs->role == RAFT_ROLE_LEADER && clusterRaftIsSingleton()) {
        clusterRaftSingletonStepDown();
    }

    clusterRaftUnblockMeet(sender);

    return 1;
}

/* MEET_REJECTED: the peer refused to merge because both sides are
 * already in a cluster. Unblock CLUSTER MEET with an error. */
static int clusterRaftProcessMeetRejected(clusterLink *link, int argc, sds *argv) {
    UNUSED(argc);
    UNUSED(argv);
    if (!link->node) return 1;

    clusterNode *sender = link->node;
    serverLog(LL_WARNING, "MEET rejected by %.40s: cannot merge two clusters.", sender->name);

    clusterRaftState *rs = RAFT_STATE();
    if (listLength(rs->pending_meets) == 0) return 1;
    /* Unblock with error. Find by address. */
    sds addr = sdscatfmt(sdsempty(), "%s:%i", sender->ip, sender->cport);
    listIter li;
    listNode *ln;
    listRewind(rs->pending_meets, &li);
    while ((ln = listNext(&li)) != NULL) {
        raftPendingMeet *pm = listNodeValue(ln);
        if (!sdscmp(pm->addr, addr)) {
            sdsfree(addr);
            listDelNode(rs->pending_meets, ln);
            pm->callback(pm->ctx, "Cannot merge two existing clusters");
            sdsfree(pm->addr);
            zfree(pm);
            return 1;
        }
    }
    sdsfree(addr);
    return 1;
}


/* --------------------------------------------------------------------------
 * PROPOSE message: PROPOSE <entry-type-name> <data...>
 *
 * Sent by a follower to the leader to propose a log entry.
 * The leader appends it to the Raft log and replicates via AE.
 * The payload after PROPOSE is the same as an AE entry line without
 * the term prefix: "<type-name> <data...>"
 *
 * Examples:
 *   PROPOSE NODE_JOIN <node-id> <address> <learner|voter>
 *   PROPOSE SLOT_CHANGE <source-id-or-dash> <source-epoch> <target-id-or-dash> <target-epoch> <range> [<range> ...]
 * -------------------------------------------------------------------------- */
static int raftEntryTypeByName(const char *name) {
    if (!strcasecmp(name, "NODE_JOIN")) return RAFT_ENTRY_NODE_JOIN;
    if (!strcasecmp(name, "NODE_FORGET")) return RAFT_ENTRY_NODE_FORGET;
    if (!strcasecmp(name, "SLOT_CHANGE")) return RAFT_ENTRY_SLOT_CHANGE;
    if (!strcasecmp(name, "SET_REPLICA_OF")) return RAFT_ENTRY_SET_REPLICA_OF;
    if (!strcasecmp(name, "FAILOVER")) return RAFT_ENTRY_FAILOVER;
    if (!strcasecmp(name, "NODE_INFO")) return RAFT_ENTRY_NODE_INFO;
    if (!strcasecmp(name, "NODE_FAIL")) return RAFT_ENTRY_NODE_FAIL;
    if (!strcasecmp(name, "NODE_RECOVER")) return RAFT_ENTRY_NODE_RECOVER;
    if (!strcasecmp(name, "ADD_VOTER")) return RAFT_ENTRY_ADD_VOTER;
    if (!strcasecmp(name, "DEL_VOTER")) return RAFT_ENTRY_DEL_VOTER;
    if (!strcasecmp(name, "NOOP")) return RAFT_ENTRY_NOOP;
    return -1;
}

static const char *raftEntryTypeName(uint8_t type) {
    switch (type) {
    case RAFT_ENTRY_NODE_JOIN: return "NODE_JOIN";
    case RAFT_ENTRY_NODE_FORGET: return "NODE_FORGET";
    case RAFT_ENTRY_SLOT_CHANGE: return "SLOT_CHANGE";
    case RAFT_ENTRY_SET_REPLICA_OF: return "SET_REPLICA_OF";
    case RAFT_ENTRY_FAILOVER: return "FAILOVER";
    case RAFT_ENTRY_NODE_INFO: return "NODE_INFO";
    case RAFT_ENTRY_NODE_FAIL: return "NODE_FAIL";
    case RAFT_ENTRY_NODE_RECOVER: return "NODE_RECOVER";
    case RAFT_ENTRY_ADD_VOTER: return "ADD_VOTER";
    case RAFT_ENTRY_DEL_VOTER: return "DEL_VOTER";
    case RAFT_ENTRY_NOOP: return "NOOP";
    default: return "UNKNOWN";
    }
}

/* Propose a log entry. The entry is an sds containing the type name
 * followed by the data, e.g. "NODE_JOIN <id> <addr>" or
 * "SLOT_CHANGE <src-id> <src-epoch> <target-id> <target-epoch> 0-5460". On the leader, it's appended directly
 * to the log. On followers, it's forwarded to the leader. */
static void clusterRaftPropose(sds entry, void *ctx, void (*callback)(void *ctx, const char *error)) {
    clusterRaftState *rs = RAFT_STATE();

    /* Parse type from the entry. */
    char *sp = strchr(entry, ' ');
    sds type_str = sp ? sdsnewlen(entry, sp - entry) : sdsdup(entry);
    int type = raftEntryTypeByName(type_str);
    sdsfree(type_str);
    if (type < 0) {
        if (callback) callback(ctx, "invalid entry type");
        return;
    }

    sds data = sp ? sdsnew(sp + 1) : sdsempty();

    if (rs->role == RAFT_ROLE_LEADER) {
        /* Pre-validate on the leader to reject obviously stale/invalid proposals. */
        RaftProposalResult pre_result = clusterRaftPreValidate(type, data);
        if (pre_result != RAFT_RESULT_OK) {
            if (callback) callback(ctx, raftProposalResultMsg(pre_result));
            sdsfree(data);
            return;
        }
    }

    /* Track pending proposal for retry on leader change. */
    {
        raftPendingProposal *pp = zmalloc(sizeof(*pp));
        pp->type = type;
        pp->data = sdsdup(data);
        pp->ctx = ctx;
        pp->callback = callback;
        pp->ctime = monotonicMs();
        pp->propose_time = pp->ctime;
        pp->deferred = 0;
        /* Entry types carrying shard epochs get automatic retry. */
        switch (type) {
        case RAFT_ENTRY_FAILOVER:
        case RAFT_ENTRY_SET_REPLICA_OF:
        case RAFT_ENTRY_SLOT_CHANGE:
        case RAFT_ENTRY_NODE_FORGET:
            pp->retries = PROPOSAL_MAX_RETRIES;
            break;
        default:
            pp->retries = 0;
            break;
        }
        listAddNodeTail(rs->pending_proposals, pp);
    }

    if (rs->role == RAFT_ROLE_LEADER) {
        /* If we're a singleton leader with an empty log that hasn't committed
         * its own NODE_JOIN yet, do it now before appending any other entry. */
        if (raftLogLastIndex() == 0 &&
            !(type == RAFT_ENTRY_NODE_JOIN &&
              sdslen(data) >= CLUSTER_NAMELEN &&
              memcmp(data, myself->name, CLUSTER_NAMELEN) == 0)) {
            clusterRaftSelfJoin();
        }
        uint64_t idx = raftLogLastIndex() + 1;
        raftLogAppend(raftLogCreate(rs->current_term, idx, type, data));
        serverLog(LL_NOTICE, "Leader appended %s (index %llu).",
                  raftEntryTypeName(type), (unsigned long long)idx);

        /* Multi-node: replicate to followers. Single-node: commit is
         * deferred to beforeSleep after persistence. */
        if (clusterRaftHasPeerMembers()) {
            rs->todo_broadcast_ae = 1;
        }
    } else {
        sdsfree(data);
        clusterNode *leader = clusterLookupNode(rs->leader, CLUSTER_NAMELEN);
        if (!leader || !leader->link) {
            /* Can't reach leader yet — defer for retry in beforeSleep. */
            rs->todo_retry_proposals = 1;
            rs->todo_connect_nodes = 1;
            serverLog(LL_NOTICE, "PROPOSE deferred: no outbound link to leader.");
            return;
        }
        sds msg = wireNewMsg("PROPOSE");
        msg = sdscatlen(msg, " ", 1);
        msg = sdscatlen(msg, entry, sdslen(entry));
        msg = wireFinishMsg(msg);
        clusterRaftSendMsg(leader->link, msg);
    }
}

/* --------------------------------------------------------------------------
 * Shard epoch helpers
 * -------------------------------------------------------------------------- */

static uint64_t clusterGetShardEpoch(const char *shard_id) {
    clusterRaftState *rs = RAFT_STATE();
    sds s = sdsnewlen(shard_id, CLUSTER_NAMELEN);
    dictEntry *de = dictFind(rs->shard_epochs, s);
    sdsfree(s);
    return de ? dictGetUnsignedIntegerVal(de) : 0;
}

static void clusterSetShardEpoch(const char *shard_id, uint64_t epoch) {
    clusterRaftState *rs = RAFT_STATE();
    sds s = sdsnewlen(shard_id, CLUSTER_NAMELEN);
    dictEntry *de = dictAddOrFind(rs->shard_epochs, s);
    if (dictGetKey(de) != s) {
        sdsfree(s);
    }
    dictSetUnsignedIntegerVal(de, epoch);
    /* Wake up any deferred proposals waiting on epoch advance. */
    rs->todo_retry_deferred = 1;
}

/* Validate a shard epoch.
 * Returns 1 if it's current, 0 if it's stale.
 * Shard_id may be NULL (skipped). Does not bump. */
static int clusterValidateShardEpoch(const char *shard_id, uint64_t epoch) {
    if (!shard_id) return 1;
    uint64_t current = clusterGetShardEpoch(shard_id);
    if (current > 0 && epoch != current) return 0;
    return 1;
}

/* Check if the shard epoch has advanced past what's in the proposal's data.
 * If so, update the epoch fields in-place and return 1. Otherwise return 0. */
static int raftRefreshEpochInData(int type, sds *data) {
    int argc;
    sds *argv = sdssplitlen(*data, sdslen(*data), " ", 1, &argc);
    if (!argv) return 0;

    int advanced = 0;
    switch (type) {
    case RAFT_ENTRY_FAILOVER: {
        /* Format: <replica-id> <primary-id> <shard-id> <shard-epoch> */
        if (argc >= 4) {
            uint64_t entry_epoch = strtoull(argv[3], NULL, 10);
            uint64_t current = clusterGetShardEpoch(argv[2]);
            if (current > entry_epoch) {
                advanced = 1;
                sdsfree(argv[3]);
                argv[3] = sdsfromlonglong(current);
            }
        }
        break;
    }
    case RAFT_ENTRY_SET_REPLICA_OF: {
        /* Format: <replica-id> <source-shard> <source-epoch> <primary-id-or-dash> <target-shard> <target-epoch> */
        if (argc >= 6) {
            uint64_t src_epoch = strtoull(argv[2], NULL, 10);
            uint64_t src_current = clusterGetShardEpoch(argv[1]);
            uint64_t tgt_epoch = strtoull(argv[5], NULL, 10);
            uint64_t tgt_current = clusterGetShardEpoch(argv[4]);
            if (src_current > src_epoch || tgt_current > tgt_epoch) {
                advanced = 1;
                sdsfree(argv[2]);
                argv[2] = sdsfromlonglong(src_current);
                sdsfree(argv[5]);
                argv[5] = sdsfromlonglong(tgt_current);
            }
        }
        break;
    }
    case RAFT_ENTRY_SLOT_CHANGE: {
        /* Format: <source-id-or-dash> <source-epoch> <target-id-or-dash> <target-epoch> <ranges...> */
        if (argc >= 5) {
            clusterNode *source = (sdslen(argv[0]) == CLUSTER_NAMELEN)
                                      ? clusterLookupNode(argv[0], CLUSTER_NAMELEN)
                                      : NULL;
            clusterNode *target = (sdslen(argv[2]) == CLUSTER_NAMELEN)
                                      ? clusterLookupNode(argv[2], CLUSTER_NAMELEN)
                                      : NULL;
            if (source && clusterGetShardEpoch(source->shard_id) > strtoull(argv[1], NULL, 10))
                advanced = 1;
            if (target && clusterGetShardEpoch(target->shard_id) > strtoull(argv[3], NULL, 10))
                advanced = 1;
            if (advanced) {
                if (source) {
                    sdsfree(argv[1]);
                    argv[1] = sdsfromlonglong(clusterGetShardEpoch(source->shard_id));
                }
                if (target) {
                    sdsfree(argv[3]);
                    argv[3] = sdsfromlonglong(clusterGetShardEpoch(target->shard_id));
                }
            }
        }
        break;
    }
    case RAFT_ENTRY_NODE_FORGET: {
        /* Format: <node-id> <epoch> */
        if (argc >= 2) {
            clusterNode *node = clusterLookupNode(argv[0], sdslen(argv[0]));
            if (node) {
                uint64_t entry_epoch = strtoull(argv[1], NULL, 10);
                uint64_t current = clusterGetShardEpoch(node->shard_id);
                if (current > entry_epoch) {
                    advanced = 1;
                    sdsfree(argv[1]);
                    argv[1] = sdsfromlonglong(current);
                }
            }
        }
        break;
    }
    default:
        break;
    }

    if (advanced) {
        sdsfree(*data);
        *data = sdsjoinsds(argv, argc, " ", 1);
    }
    sdsfreesplitres(argv, argc);
    return advanced;
}

/* Parsed fields from a SLOT_CHANGE entry's epoch-related data.
 * Format: "<source-id-or-dash> <source-epoch> <target-id-or-dash> <target-epoch> <ranges...>" */
typedef struct {
    clusterNode *target_node;    /* NULL if dash */
    clusterNode *source_node;    /* NULL if dash */
    const char *source_shard_id; /* source_node->shard_id or NULL. */
    const char *target_shard_id; /* target_node->shard_id or NULL. */
    uint64_t source_epoch;
    uint64_t target_epoch;
    int range_start_arg; /* Index in argv where ranges begin (first range element). */
} slotChangeEpochInfo;

/* Parse fields from a SLOT_CHANGE entry's split argv.
 * Format: "<source-id-or-dash> <source-epoch> <target-id-or-dash> <target-epoch> <ranges...>"
 * Caller must have verified argc >= 5.
 * Returns true if parsing succeeded, false otherwise. */
static bool parseSlotChangeEpochs(sds *argv, int argc, slotChangeEpochInfo *info) {
    memset(info, 0, sizeof(*info));

    /* argv[0] = source-id-or-dash */
    info->source_node = (sdslen(argv[0]) == CLUSTER_NAMELEN)
                            ? clusterLookupNode(argv[0], CLUSTER_NAMELEN)
                            : NULL;
    /* argv[1] = source-epoch */
    info->source_epoch = strtoull(argv[1], NULL, 10);
    /* argv[2] = target-id-or-dash */
    info->target_node = (sdslen(argv[2]) == CLUSTER_NAMELEN)
                            ? clusterLookupNode(argv[2], CLUSTER_NAMELEN)
                            : NULL;
    /* argv[3] = target-epoch */
    info->target_epoch = strtoull(argv[3], NULL, 10);
    /* argv[4..argc-1] = ranges */
    info->range_start_arg = 4;

    info->source_shard_id = info->source_node ? info->source_node->shard_id : NULL;
    info->target_shard_id = info->target_node ? info->target_node->shard_id : NULL;
    return (argc > 4);
}

/* Pre-validate a proposal on the leader before appending to the log.
 * Reuses the apply functions in validate_only mode.
 * Returns RAFT_RESULT_OK if valid, or RAFT_RESULT_STALE_EPOCH /
 * RAFT_RESULT_REJECTED describing why it failed. */
static RaftProposalResult clusterRaftPreValidate(int type, sds data) {
    RaftProposalResult result = RAFT_RESULT_OK;
    /* Enforce at most one quorum-changing transition in flight. A different
     * membership transition already in the log is rejected here so it never
     * consumes log space. */
    if (clusterRaftQuorumChangeStatus(type, data) == RAFT_QC_CONFLICT) {
        return RAFT_RESULT_REJECTED;
    }
    switch (type) {
    case RAFT_ENTRY_FAILOVER:
        result = clusterRaftApplyFailover(data, 1);
        break;
    case RAFT_ENTRY_SET_REPLICA_OF:
        result = clusterRaftApplySetReplica(data, 1);
        break;
    case RAFT_ENTRY_SLOT_CHANGE:
        result = clusterRaftApplySlotChange(data, 1);
        break;
    case RAFT_ENTRY_NODE_FORGET:
        result = clusterRaftApplyNodeForget(data, 1);
        break;
    case RAFT_ENTRY_ADD_VOTER:
        result = clusterRaftApplyAddVoter(data, 1);
        break;
    case RAFT_ENTRY_DEL_VOTER:
        result = clusterRaftApplyDelVoter(data, 1);
        break;
    default:
        break;
    }
    if (result != RAFT_RESULT_OK) {
        serverLog(LL_NOTICE, "Leader pre-validation: rejecting %s proposal (%s). data: %s",
                  raftEntryTypeName(type), raftProposalResultMsg(result), data);
    }
    return result;
}

static int clusterRaftProcessPropose(clusterLink *link, int argc, sds *argv) {
    clusterRaftState *rs = RAFT_STATE();

    /* argv[0]="PROPOSE", argv[1..] is the entry (type + data). */
    if (argc < 2) return 1;
    if (rs->role != RAFT_ROLE_LEADER) return 1;

    /* Reconstruct the entry and parse the type. */
    sds entry = sdsjoinsds(argv + 1, argc - 1, " ", 1);
    int type = raftEntryTypeByName(argv[1]);
    if (type < 0) {
        sdsfree(entry);
        return 1;
    }

    /* Data is everything after the type name. */
    sds data = (argc >= 3) ? sdsjoinsds(argv + 2, argc - 2, " ", 1) : sdsempty();

    /* Pre-validate on the leader to reject stale/invalid proposals. */
    RaftProposalResult pre_result = clusterRaftPreValidate(type, data);
    if (pre_result != RAFT_RESULT_OK) {
        /* Send REJECT back to the proposing follower so it can unblock the client. */
        if (link) {
            sds msg = wireNewMsg("REJECT");
            msg = sdscatfmt(msg, " %s ", raftProposalResultWireReason(pre_result));
            msg = sdscatlen(msg, entry, sdslen(entry));
            msg = wireFinishMsg(msg);
            clusterRaftSendMsg(link, msg);
        }
        sdsfree(data);
        sdsfree(entry);
        return 1;
    }

    uint64_t idx = raftLogLastIndex() + 1;
    raftLogAppend(raftLogCreate(rs->current_term, idx, type, data));
    serverLog(LL_NOTICE, "Leader appended proposed %s (index %llu).",
              raftEntryTypeName(type), (unsigned long long)idx);

    /* Multi-node: replicate to followers. Single-node: commit is
     * deferred to beforeSleep after persistence. */
    if (clusterRaftHasPeerMembers()) {
        rs->todo_broadcast_ae = 1;
    }

    sdsfree(entry);
    return 1;
}

/* Handle a REJECT message from the leader. The leader sends this when it
 * rejects a forwarded PROPOSE. We match it against our
 * pending_proposals and fire the callback with an error so the client
 * gets an immediate reply instead of hanging until timeout.
 * Format: "REJECT <reason> <type> <data...>"
 * <reason> is a single word: "conflict" (retryable epoch), "rejected" (terminal), etc. */
static int clusterRaftProcessReject(clusterLink *link, int argc, sds *argv) {
    UNUSED(link);

    /* argv[0]="REJECT", argv[1]=reason, argv[2]=type, argv[3..]=data */
    if (argc < 3) return 1;

    const char *reason = argv[1];
    int type = raftEntryTypeByName(argv[2]);
    if (type < 0) return 1;

    int data_argc = argc - 3;
    sds data = (data_argc > 0) ? sdsjoinsds(argv + 3, data_argc, " ", 1) : sdsempty();

    RaftProposalResult result = raftProposalResultFromWire(reason);

    clusterRaftCompletePendingProposal(type, data, result);
    sdsfree(data);
    return 1;
}

/* --------------------------------------------------------------------------
 * Raft election and heartbeat
 * -------------------------------------------------------------------------- */

/* Forward declarations for pre-vote handlers. */
static int clusterRaftProcessPreVoteRequest(clusterLink *link, int argc, sds *argv);
static int clusterRaftProcessPreVoteResponse(clusterLink *link, int argc, sds *argv);
static int clusterRaftProcessInstallSnapshot(clusterLink *link, int argc, sds *argv, sds *config_lines, int config_line_count);
static void clusterRaftStartPreVote(void);

/* Defer pending proposals for retry after a leader change. The proposals
 * are idempotent, so retrying is safe. They will be resent to the new
 * leader (or appended locally if we become leader) in the next cron. */
static void clusterRaftDeferPendingProposals(void) {
    clusterRaftState *rs = RAFT_STATE();
    if (listLength(rs->pending_proposals) > 0) {
        rs->todo_retry_proposals = 1;
        serverLog(LL_NOTICE, "Deferring %lu pending proposals for retry.",
                  listLength(rs->pending_proposals));
    }
    /* Note: pending_meets are NOT flushed here. A MEET remains valid
     * across leader changes — the WELCOME will arrive eventually. */
}

/* Step down to follower if we see a higher term. Returns 1 if stepped down. */
static int clusterRaftMaybeStepDown(uint64_t term) {
    clusterRaftState *rs = RAFT_STATE();
    if (term > rs->current_term) {
        rs->current_term = term;
        clusterRaftStepDown(monotonicMs(), "observed higher term");
        return 1;
    }
    return 0;
}

static int clusterRaftIsVotedForNone(void) {
    char zero[CLUSTER_NAMELEN] = {0};
    return memcmp(RAFT_STATE()->voted_for, zero, CLUSTER_NAMELEN) == 0;
}

/* --------------------------------------------------------------------------
 * Raft log helpers
 * -------------------------------------------------------------------------- */

static raftLogEntry *raftLogCreate(uint64_t term, uint64_t index, uint8_t type, sds data) {
    raftLogEntry *e = zmalloc(sizeof(*e));
    e->term = term;
    e->index = index;
    e->type = type;
    e->data = data;
    return e;
}

static void raftLogFree(raftLogEntry *e) {
    sdsfree(e->data);
    zfree(e);
}

static void raftLogAppend(raftLogEntry *e) {
    clusterRaftState *rs = RAFT_STATE();
    if (rs->log_count == rs->log_alloc) {
        rs->log_alloc = rs->log_alloc ? rs->log_alloc * 2 : 16;
        rs->log = zrealloc(rs->log, rs->log_alloc * sizeof(raftLogEntry *));
    }
    rs->log[rs->log_count++] = e;
    /* Schedule persistence in next beforeSleep. */
    if (!rs->todo_persist_log) {
        rs->todo_persist_log = 1;
        rs->persist_log_from = e->index;
    }
}

/* O(1) lookup by index. Returns NULL if out of range. Indices start at 1. */
static raftLogEntry *raftLogGet(uint64_t index) {
    clusterRaftState *rs = RAFT_STATE();
    if (rs->log_count == 0 || index == 0 || index > raftLogLastIndex()) return NULL;
    uint64_t base = rs->log[0]->index;
    if (index < base || index - base >= rs->log_count) return NULL;
    raftLogEntry *e = rs->log[index - base];
    return e->index == index ? e : NULL;
}

/* Truncate the log from the given index onwards (inclusive). */
static void raftLogTruncateFrom(uint64_t index) {
    clusterRaftState *rs = RAFT_STATE();
    while (rs->log_count > 0 && rs->log[rs->log_count - 1]->index >= index) {
        raftLogFree(rs->log[--rs->log_count]);
    }
    /* Truncated entries may already be on disk; a full rewrite is needed
     * to remove them. */
    rs->todo_save_config = 1;
}

/* Trim applied entries from the front of the log. Called after applying
 * entries; trims only when the number of applied entries in the log exceeds
 * twice the keep threshold, so trimming happens in infrequent batches. */
#define RAFT_LOG_TRIM_KEEP 64
static void raftLogTrimApplied(void) {
    clusterRaftState *rs = RAFT_STATE();
    if (rs->log_count == 0) return;
    uint64_t base = rs->log[0]->index;
    if (rs->last_applied < base) return;
    uint64_t applied_in_log = rs->last_applied - base + 1;
    if (applied_in_log <= RAFT_LOG_TRIM_KEEP * 2) return;
    uint64_t trim_count = applied_in_log - RAFT_LOG_TRIM_KEEP;
    rs->log_trim_index = rs->log[trim_count - 1]->index;
    rs->log_trim_term = rs->log[trim_count - 1]->term;
    for (uint64_t i = 0; i < trim_count; i++) {
        raftLogFree(rs->log[i]);
    }
    rs->log_count -= trim_count;
    memmove(rs->log, rs->log + trim_count, rs->log_count * sizeof(raftLogEntry *));
}

static uint64_t raftLogLastIndex(void) {
    clusterRaftState *rs = RAFT_STATE();
    return rs->log_count > 0 ? rs->log[rs->log_count - 1]->index : rs->last_applied;
}

static uint64_t raftLogLastTerm(void) {
    clusterRaftState *rs = RAFT_STATE();
    return rs->log_count > 0 ? rs->log[rs->log_count - 1]->term : rs->snapshot_term;
}

static uint64_t raftLogTermAt(uint64_t index) {
    raftLogEntry *e = raftLogGet(index);
    if (e) return e->term;
    /* Entry not in log — if it's at the snapshot boundary, use snapshot_term. */
    clusterRaftState *rs = RAFT_STATE();
    if (index == rs->last_applied) return rs->snapshot_term;
    if (index == rs->log_trim_index) return rs->log_trim_term;
    return 0;
}

/* Reset the optional fields of a node's address string, so that the ones
 * missing from the next address string we load end up cleared. */
static void clusterRaftClearOptionalNodeInfo(clusterNode *node) {
    node->announce_client_tcp_port = 0;
    node->announce_client_tls_port = 0;
    sdsclear(node->hostname);
    sdsclear(node->human_nodename);
    sdsclear(node->announce_client_ipv4);
    sdsclear(node->announce_client_ipv6);
    sdsclear(node->availability_zone);
    node->replica_priority = 0;
}

/* True if the entry changes the voting set (server.cluster->size). Safe on
 * malformed/too-short data: never reads past the buffer. */
static bool raftEntryAffectsQuorum(int type, sds data) {
    if (sdslen(data) < CLUSTER_NAMELEN) return false;

    switch (type) {
    case RAFT_ENTRY_ADD_VOTER:
    case RAFT_ENTRY_DEL_VOTER:
        return true;
    case RAFT_ENTRY_NODE_JOIN: {
        /* data: "<node-id> <address> <learner|voter>" */
        int argc;
        sds *argv = sdssplitlen(data, sdslen(data), " ", 1, &argc);
        bool changes = (argv && argc >= 3 && !strcasecmp(argv[2], "voter"));
        if (argv) sdsfreesplitres(argv, argc);
        return changes;
    }
    case RAFT_ENTRY_NODE_FORGET: {
        clusterNode *node = clusterLookupNode(data, CLUSTER_NAMELEN);
        return node != NULL && nodeIsVoter(node);
    }
    default:
        return false;
    }
}

/* Classify the in-flight (committed but not yet applied) quorum-changing
 * entries relative to a candidate transition. Scans (last_applied,
 * raftLogLastIndex()] once. Returns:
 *   RAFT_QC_NONE     — no quorum-changing entry in flight.
 *   RAFT_QC_SAME     — the identical (type, target) transition is in flight.
 *   RAFT_QC_CONFLICT — a different transition is in flight. */
static raftQuorumChangeStatus clusterRaftQuorumChangeStatus(int type, sds data) {
    if (!raftEntryAffectsQuorum(type, data)) return RAFT_QC_NONE;

    clusterRaftState *rs = RAFT_STATE();
    int found_same = 0;
    int found_conflict = 0;
    for (uint64_t idx = rs->last_applied + 1; idx <= raftLogLastIndex(); idx++) {
        raftLogEntry *e = raftLogGet(idx);
        if (!e || !raftEntryAffectsQuorum(e->type, e->data)) continue;
        if (e->type == type && memcmp(e->data, data, CLUSTER_NAMELEN) == 0) {
            found_same = 1;
        } else {
            found_conflict = 1;
        }
    }
    if (found_conflict) return RAFT_QC_CONFLICT;
    if (found_same) return RAFT_QC_SAME;
    return RAFT_QC_NONE;
}

/* Find a pending proposal matching type+data, fire its callback, and remove it.
 * Called both when an entry is applied (from raftLogApply) and when the leader
 * sends a REJECT for a forwarded proposal. */
static void clusterRaftCompletePendingProposal(int type, sds data, RaftProposalResult result) {
    clusterRaftState *rs = RAFT_STATE();
    if (listLength(rs->pending_proposals) == 0) return;

    listIter li;
    listNode *ln;
    listRewind(rs->pending_proposals, &li);
    while ((ln = listNext(&li)) != NULL) {
        raftPendingProposal *pp = listNodeValue(ln);
        if (pp->type == type && !sdscmp(pp->data, data)) {
            /* On stale epoch with retries remaining: mark deferred.
             * The epoch update happens at retry time when the epoch has
             * actually advanced (checked by raftRefreshEpochInData). */
            if (result == RAFT_RESULT_STALE_EPOCH && pp->retries > 0) {
                pp->retries--;
                pp->deferred = 1;
                /* The epoch may already have advanced before this rejection
                 * arrived. Retry in case it did; raftRefreshEpochInData()
                 * skips it otherwise. */
                rs->todo_retry_deferred = 1;
                return;
            }
            if (pp->callback) pp->callback(pp->ctx, raftProposalResultMsg(result));
            sdsfree(pp->data);
            zfree(pp);
            listDelNode(rs->pending_proposals, ln);
            break;
        }
    }
}

/* Apply a committed log entry. */
static void raftLogApply(raftLogEntry *e) {
    clusterRaftState *rs = RAFT_STATE();
    RaftProposalResult entry_result = RAFT_RESULT_OK;
    switch (e->type) {
    case RAFT_ENTRY_NODE_JOIN: {
        /* data: "<node-id> <address> <learner|voter>" */
        int argc;
        sds *argv = sdssplitlen(e->data, sdslen(e->data), " ", 1, &argc);
        if (argv && argc >= 3 && sdslen(argv[0]) == CLUSTER_NAMELEN) {
            bool learner;
            if (!strcasecmp(argv[2], "learner")) {
                learner = true;
            } else if (!strcasecmp(argv[2], "voter")) {
                learner = false;
            } else {
                serverLog(LL_WARNING, "NODE_JOIN rejected: unknown membership mode '%s'.", argv[2]);
                sdsfreesplitres(argv, argc);
                break;
            }
            clusterNode *existing = clusterLookupNode(argv[0], CLUSTER_NAMELEN);
            if (!existing) {
                clusterNode *n = createClusterNode(argv[0], CLUSTER_NODE_PRIMARY);
                if (clusterNodeParseAddressString(n, argv[1]) == C_OK) {
                    clusterAddNode(n);
                } else {
                    freeClusterNode(n);
                    sdsfreesplitres(argv, argc);
                    break;
                }
            } else if (existing != myself) {
                /* Update address for existing node. */
                clusterNodeParseAddressString(existing, argv[1]);
            }
            /* On first official join, clear JOINING and mark voter/learner. */
            clusterNode *joined = existing ? existing : clusterLookupNode(argv[0], CLUSTER_NAMELEN);
            bool first_join = false;
            if (joined && (!existing || (joined->flags & CLUSTER_NODE_JOINING))) {
                first_join = true;
                joined->flags &= ~CLUSTER_NODE_JOINING;
                if (learner) {
                    joined->flags |= CLUSTER_NODE_LEARNER;
                } else {
                    joined->flags &= ~CLUSTER_NODE_LEARNER;
                    server.cluster->size++;
                }
                clusterAddNodeToShard(joined->shard_id, joined);
                rs->todo_save_config = 1;

                clusterRaftProcessDeferredMeets();
            }
            clearCachedClusterSlotsResponse();
            rs->todo_connect_nodes = 1;
            serverLog(LL_NOTICE, "Applied NODE_JOIN for %.40s addr=%s mode=%s (voters=%d).",
                      argv[0], argc >= 2 ? argv[1] : "?", learner ? "learner" : "voter", server.cluster->size);

            /* Leader: initialize replication state for the new peer. */
            if (rs->role == RAFT_ROLE_LEADER) {
                clusterNode *peer = clusterLookupNode(argv[0], CLUSTER_NAMELEN);
                if (peer && peer != myself) {
                    RAFT_NODE(peer)->next_index = 1;
                    RAFT_NODE(peer)->match_index = 0;
                    RAFT_NODE(peer)->last_ack_time = monotonicMs();
                }
            }

            /* Ourselves: keep rs->role aligned with the membership flag above.
             *
             * learner → LEARNER (even if AE already made us FOLLOWER)
             * voter + JOINER → FOLLOWER
             * voter + LEADER → keep LEADER (singleton self-join) */
            if (memcmp(argv[0], myself->name, CLUSTER_NAMELEN) == 0) {
                sdsfree(rs->my_last_committed_info);
                rs->my_last_committed_info = clusterRaftBuildMyNodeInfo();

                int was_joiner = (rs->role == RAFT_ROLE_JOINER);
                if (first_join) {
                    if (learner)
                        rs->role = RAFT_ROLE_LEARNER;
                    else if (was_joiner)
                        rs->role = RAFT_ROLE_FOLLOWER;
                }

                if (was_joiner) {
                    serverLog(LL_NOTICE, "Promoted from joiner to %s.", learner ? "learner" : "follower");

                    /* Propose SLOT_CHANGE for slots assigned before joining
                     * the cluster (from our singleton state). */
                    int count = 0;
                    sds slot_range = sdsempty();
                    for (int j = 0; j < CLUSTER_SLOTS; j++) {
                        if (server.cluster->slots[j] != myself) continue;
                        int start = j;
                        while (j + 1 < CLUSTER_SLOTS && server.cluster->slots[j + 1] == myself) j++;
                        if (j == start)
                            slot_range = sdscatfmt(slot_range, " %i", start);
                        else
                            slot_range = sdscatfmt(slot_range, " %i-%i", start, j);
                        count++;
                    }
                    if (count > 0) {
                        sds entry = sdsnew("SLOT_CHANGE - 0 ");
                        entry = sdscatlen(entry, myself->name, CLUSTER_NAMELEN);
                        entry = sdscat(entry, " 0");
                        entry = sdscatsds(entry, slot_range);
                        clusterRaftPropose(entry, NULL, NULL);
                        sdsfree(entry);
                    }
                    sdsfree(slot_range);
                }
            }

            /* If we have a pending CLUSTER MEET for this node, fire its
             * callback now that the NODE_JOIN is committed. */
            {
                clusterNode *node = clusterLookupNode(argv[0], CLUSTER_NAMELEN);
                if (node) {
                    clusterRaftUnblockMeet(node);
                }
            }
        }
        if (argv) sdsfreesplitres(argv, argc);
        break;
    }
    case RAFT_ENTRY_SLOT_CHANGE: {
        entry_result = clusterRaftApplySlotChange(e->data, 0);
        if (entry_result == RAFT_RESULT_OK) {
            rs->todo_update_slot_coverage = 1;
            clearCachedClusterSlotsResponse();
        }
        serverLog(LL_NOTICE, "%s SLOT_CHANGE (index %llu).",
                  entry_result != RAFT_RESULT_OK ? "Skipped" : "Applied", (unsigned long long)e->index);
        break;
    }
    case RAFT_ENTRY_SET_REPLICA_OF: {
        entry_result = clusterRaftApplySetReplica(e->data, 0);
        if (entry_result == RAFT_RESULT_OK) {
            clearCachedClusterSlotsResponse();
        }
        serverLog(LL_NOTICE, "%s SET_REPLICA_OF (index %llu).",
                  entry_result != RAFT_RESULT_OK ? "Skipped" : "Applied", (unsigned long long)e->index);
        break;
    }
    case RAFT_ENTRY_FAILOVER: {
        entry_result = clusterRaftApplyFailover(e->data, 0);
        if (entry_result == RAFT_RESULT_OK) {
            rs->todo_update_slot_coverage = 1;
            clearCachedClusterSlotsResponse();
        }
        serverLog(LL_NOTICE, "%s FAILOVER (index %llu).",
                  entry_result != RAFT_RESULT_OK ? "Skipped" : "Applied", (unsigned long long)e->index);
        break;
    }
    case RAFT_ENTRY_NODE_FORGET: {
        entry_result = clusterRaftApplyNodeForget(e->data, 0);
        serverLog(LL_NOTICE, "%s NODE_FORGET (index %llu).",
                  entry_result != RAFT_RESULT_OK ? "Skipped" : "Applied", (unsigned long long)e->index);
        break;
    }
    case RAFT_ENTRY_ADD_VOTER: {
        entry_result = clusterRaftApplyAddVoter(e->data, 0);
        serverLog(LL_NOTICE, "%s ADD_VOTER (index %llu).",
                  entry_result != RAFT_RESULT_OK ? "Skipped" : "Applied", (unsigned long long)e->index);
        break;
    }
    case RAFT_ENTRY_DEL_VOTER: {
        entry_result = clusterRaftApplyDelVoter(e->data, 0);
        serverLog(LL_NOTICE, "%s DEL_VOTER (index %llu).",
                  entry_result != RAFT_RESULT_OK ? "Skipped" : "Applied", (unsigned long long)e->index);
        break;
    }
    case RAFT_ENTRY_NODE_FAIL: {
        clusterNode *node = clusterLookupNode(e->data, sdslen(e->data));
        if (node && node != myself) {
            node->flags |= CLUSTER_NODE_FAIL;
            RAFT_NODE(node)->pending_fail_change = 0;
            rs->todo_update_slot_coverage = 1;
            clearCachedClusterSlotsResponse();
        }
        serverLog(LL_NOTICE, "Applied NODE_FAIL %.40s (index %llu).",
                  e->data, (unsigned long long)e->index);

        /* If the failed node is my primary, defer failover scheduling
         * to beforeSleep. This avoids acting on stale NODE_FAIL entries
         * when catching up on old log entries — a subsequent NODE_RECOVER
         * in the same batch will clear the flag. */
        if (node && nodeIsReplica(myself) && myself->replicaof == node &&
            !nodeCantFailover(myself)) {
            rs->todo_schedule_failover = 1;
        }
        break;
    }
    case RAFT_ENTRY_NODE_RECOVER: {
        clusterNode *node = clusterLookupNode(e->data, sdslen(e->data));
        if (node) {
            node->flags &= ~CLUSTER_NODE_FAIL;
            RAFT_NODE(node)->pending_fail_change = 0;
            rs->todo_update_slot_coverage = 1;
            clearCachedClusterSlotsResponse();
            /* Cancel deferred failover if the recovered node is my primary. */
            if (nodeIsReplica(myself) && myself->replicaof == node) {
                rs->todo_schedule_failover = 0;
            }
        }
        serverLog(LL_NOTICE, "Applied NODE_RECOVER %.40s (index %llu).",
                  e->data, (unsigned long long)e->index);
        break;
    }
    case RAFT_ENTRY_NODE_INFO: {
        /* Format: "<node-id> <address-string> <flags>"
         * Propagates address/hostname/port changes. Shard-id is intentionally
         * excluded from the address string because it is authoritatively
         * managed by NODE_JOIN and SET_REPLICA_OF entries. */
        int argc;
        sds *argv = sdssplitlen(e->data, sdslen(e->data), " ", 1, &argc);
        if (argv && argc >= 2 && sdslen(argv[0]) == CLUSTER_NAMELEN) {
            clusterNode *node = clusterLookupNode(argv[0], CLUSTER_NAMELEN);

            if (node && node != myself) {
                clusterRaftClearOptionalNodeInfo(node);
                clusterNodeParseAddressString(node, argv[1]);
                /* Apply self-set flags. TODO: split on comma and compare
                 * each part individually when more flags are added. */
                if (argc >= 3) {
                    node->flags &= ~CLUSTER_NODE_NOFAILOVER;
                    if (strstr(argv[2], "nofailover")) {
                        node->flags |= CLUSTER_NODE_NOFAILOVER;
                    }
                }
            }
            if (node == myself) {
                sdsfree(rs->my_last_committed_info);
                rs->my_last_committed_info = sdsdup(e->data);
            }
        }
        if (argv) sdsfreesplitres(argv, argc);
        /* Invalidate immediately — address changes make the cached
         * CLUSTER SLOTS response invalid and the verify assert fires
         * if a client queries before beforeSleep runs. */
        clearCachedClusterSlotsResponse();
        serverLog(LL_NOTICE, "Applied NODE_INFO (index %llu).", (unsigned long long)e->index);
        break;
    }
    case RAFT_ENTRY_NOOP:
        /* No-op: nothing to apply. */
        break;
    default:
        serverLog(LL_NOTICE, "Applied log entry type %d (index %llu).", e->type,
                  (unsigned long long)e->index);
        break;
    }

    if (entry_result != RAFT_RESULT_OK) {
        serverLog(LL_DEBUG, "Proposal rejected at apply: %s (type %s, index %llu).",
                  raftProposalResultMsg(entry_result), raftEntryTypeName(e->type), (unsigned long long)e->index);
    }

    /* Check pending proposals for a match and remove it. */
    clusterRaftCompletePendingProposal(e->type, e->data, entry_result);
}

/* Apply all committed but unapplied log entries. */
static void raftApplyCommitted(void) {
    clusterRaftState *rs = RAFT_STATE();
    while (rs->last_applied < rs->commit_index) {
        raftLogEntry *e = raftLogGet(rs->last_applied + 1);
        if (!e) {
            serverLog(LL_WARNING, "Committed entry %llu is missing from the log.",
                      (unsigned long long)(rs->last_applied + 1));
            break;
        }
        rs->last_applied++;
        rs->snapshot_term = e->term;
        raftLogApply(e);
    }
}

/* --------------------------------------------------------------------------
 * AE (AppendEntries) message
 *
 * Header line: AE <leader-id> <term> <prev-log-idx> <prev-log-term> <commit> <count>
 * Entry lines: <term> <type> <data>
 * -------------------------------------------------------------------------- */

/* Send a snapshot to a follower whose next_index is behind the trimmed log.
 * Format: INSTALL_SNAPSHOT <leader-id> <term> <last-index> <last-term>\n<config> */
static void clusterRaftSendInstallSnapshot(clusterLink *link, clusterNode *node) {
    clusterRaftState *rs = RAFT_STATE();
    /* Strip MYSELF flag so the receiver doesn't adopt our identity.
     * Filter JOINING nodes (NODE_JOIN not yet committed). */
    myself->flags &= ~CLUSTER_NODE_MYSELF;
    sds config = clusterGenNodesDescription(NULL, CLUSTER_NODE_JOINING | CLUSTER_NODE_HANDSHAKE, 0);
    myself->flags |= CLUSTER_NODE_MYSELF;
    sds msg = wireNewMsg("INSTALL_SNAPSHOT");
    msg = sdscatlen(msg, " ", 1);
    msg = sdscatlen(msg, myself->name, CLUSTER_NAMELEN);
    msg = sdscatfmt(msg, " %U %U %U\n",
                    (unsigned long long)rs->current_term,
                    (unsigned long long)rs->last_applied,
                    (unsigned long long)rs->snapshot_term);
    msg = sdscatsds(msg, config);
    sdsfree(config);
    msg = wireFinishMsg(msg);
    clusterRaftSendMsg(link, msg);
    serverLog(LL_NOTICE, "Sent INSTALL_SNAPSHOT to %.40s (last_applied=%llu, term=%llu).",
              node->name, (unsigned long long)rs->last_applied,
              (unsigned long long)rs->snapshot_term);
    /* next_index is updated by the AE_ACK sent after installing it. */
    RAFT_NODE(node)->last_snapshot_time = monotonicMs();
}

static void clusterRaftSendAppendEntries(clusterLink *link, clusterNode *node) {
    clusterRaftState *rs = RAFT_STATE();
    clusterNodeRaftData *rd = RAFT_NODE(node);

    uint64_t next = rd->next_index;

    /* If the follower needs entries we've already trimmed, send a snapshot. */
    uint64_t first_available = rs->log_count > 0 ? rs->log[0]->index : rs->last_applied + 1;
    if (next < first_available) {
        /* Don't resend it on every heartbeat while the follower installs it,
         * but resend it if it's lost. */
        if (monotonicMs() - rd->last_snapshot_time > server.cluster_node_timeout / 2)
            clusterRaftSendInstallSnapshot(link, node);
        return;
    }

    uint64_t prev_index = next > 0 ? next - 1 : 0;
    uint64_t prev_term = raftLogTermAt(prev_index);

    /* Collect entries to send starting from next_index. */
    int count = 0;
    sds entries = sdsempty();
    for (uint64_t idx = next; idx <= raftLogLastIndex(); idx++) {
        raftLogEntry *e = raftLogGet(idx);
        if (!e) break;
        entries = sdscatfmt(entries, "\n%U %s %S",
                            (unsigned long long)e->term, raftEntryTypeName(e->type), e->data);
        count++;
    }

    sds msg = wireNewMsg("AE");
    msg = sdscatlen(msg, " ", 1);
    msg = sdscatlen(msg, myself->name, CLUSTER_NAMELEN);
    msg = sdscatfmt(msg, " %U %U %U %U %i",
                    (unsigned long long)rs->current_term,
                    (unsigned long long)prev_index,
                    (unsigned long long)prev_term,
                    (unsigned long long)rs->commit_index, count);
    msg = sdscatsds(msg, entries);
    sdsfree(entries);
    msg = wireFinishMsg(msg);
    serverLog(LL_DEBUG, "Sending AE to %.40s: next=%llu prev=%llu commit=%llu count=%d",
              node->name, (unsigned long long)next, (unsigned long long)prev_index,
              (unsigned long long)rs->commit_index, count);
    clusterRaftSendMsg(link, msg);
}

static void clusterRaftBroadcastAppendEntries(void) {
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node == myself) continue;
        if (node->flags & CLUSTER_NODE_JOINING) continue;
        if (!node->link) continue;
        clusterRaftSendAppendEntries(node->link, node);
    }
    dictReleaseIterator(di);
}

/* AE_ACK <term> <success> <last-log-index> */
static void clusterRaftSendAppendEntriesResponse(clusterLink *link, uint64_t term, int success) {
    long long offset = nodeIsReplica(myself) ? replicationGetReplicaOffset() : server.primary_repl_offset;
    sds msg = wireNewMsg("AE_ACK");
    msg = sdscatfmt(msg, " %U %i %U %I", (unsigned long long)term, success,
                    (unsigned long long)raftLogLastIndex(), (long long)offset);
    msg = wireFinishMsg(msg);
    clusterRaftSendMsg(link, msg);
}

/* Record the leader we heard from. On a leader change, resend the pending
 * proposals, since a node that isn't the leader drops them. */
static void clusterRaftSetLeader(const char *leader_id) {
    clusterRaftState *rs = RAFT_STATE();
    if (memcmp(rs->leader, leader_id, CLUSTER_NAMELEN) == 0) return;
    memcpy(rs->leader, leader_id, CLUSTER_NAMELEN);
    if (listLength(rs->pending_proposals) > 0) rs->todo_retry_proposals = 1;
}

static int clusterRaftProcessAppendEntries(clusterLink *link, int argc, sds *argv, sds *entry_lines, int entry_line_count) {
    /* argv: AE <leader-id> <term> <prev-log-idx> <prev-log-term> <commit> <count> */
    if (argc < 7) return 1;
    if (sdslen(argv[1]) != CLUSTER_NAMELEN) return 1;
    serverLog(LL_DEBUG, "Received AE from %.40s: term=%s prev=%s commit=%s count=%s",
              argv[1], argv[2], argv[3], argv[5], argv[6]);

    clusterRaftState *rs = RAFT_STATE();
    uint64_t msg_term = strtoull(argv[2], NULL, 10);
    uint64_t prev_log_index = strtoull(argv[3], NULL, 10);
    uint64_t prev_log_term = strtoull(argv[4], NULL, 10);
    uint64_t leader_commit = strtoull(argv[5], NULL, 10);
    int entry_count = atoi(argv[6]);

    if (msg_term < rs->current_term) {
        clusterRaftSendAppendEntriesResponse(link, rs->current_term, 0);
        return 1;
    }

    clusterRaftMaybeStepDown(msg_term);

    /* Accept heartbeat. */
    if (rs->role != RAFT_ROLE_JOINER && rs->role != RAFT_ROLE_LEARNER) rs->role = RAFT_ROLE_FOLLOWER;
    rs->last_heartbeat = monotonicMs();
    clusterRaftSetLeader(argv[1]);

    /* Log consistency check: verify prev_log_index/term match. Entries have
     * term >= 1, so 0 means we don't know the entry and never matches. */
    uint64_t local_prev_term = raftLogTermAt(prev_log_index);
    if (prev_log_index > 0 && (local_prev_term == 0 || local_prev_term != prev_log_term)) {
        clusterRaftSendAppendEntriesResponse(link, rs->current_term, 0);
        return 1;
    }

    /* Append new entries, handling conflicts. */
    uint64_t new_index = prev_log_index + 1;
    for (int i = 0; i < entry_count && i < entry_line_count; i++, new_index++) {
        int eargc;
        sds *eargv = sdssplitlen(entry_lines[i], sdslen(entry_lines[i]), " ", 1, &eargc);
        if (!eargv || eargc < 3) {
            if (eargv) sdsfreesplitres(eargv, eargc);
            break;
        }
        uint64_t e_term = strtoull(eargv[0], NULL, 10);
        int e_type_int = raftEntryTypeByName(eargv[1]);
        if (e_term == 0 || e_type_int < 0) {
            sdsfreesplitres(eargv, eargc);
            break;
        }
        uint8_t e_type = e_type_int;
        sds e_data = sdsjoinsds(eargv + 2, eargc - 2, " ", 1);
        sdsfreesplitres(eargv, eargc);

        raftLogEntry *existing = raftLogGet(new_index);
        if (existing && existing->term != e_term) {
            /* Conflict: truncate from here and append. */
            raftLogTruncateFrom(new_index);
            existing = NULL;
        }
        if (existing || new_index <= rs->last_applied) {
            sdsfree(e_data); /* Already have this entry, or applied and trimmed. */
            continue;
        }
        if (new_index != raftLogLastIndex() + 1) {
            /* Not contiguous with our log; don't leave a hole. */
            serverLog(LL_WARNING, "AE entry %llu doesn't follow our last log index %llu.",
                      (unsigned long long)new_index, (unsigned long long)raftLogLastIndex());
            sdsfree(e_data);
            break;
        }
        raftLogAppend(raftLogCreate(e_term, new_index, e_type, e_data));
    }

    /* Update commit index and apply. */
    if (leader_commit > rs->commit_index) {
        rs->commit_index = leader_commit;
        if (rs->commit_index > raftLogLastIndex()) rs->commit_index = raftLogLastIndex();
    }
    raftApplyCommitted();

    /* Defer AE_ACK until after persistence in beforeSleep. This ensures
     * entries are on stable storage before the leader counts our ACK. */
    rs->todo_send_ae_ack = 1;
    return 1;
}

static int clusterRaftProcessAppendEntriesResponse(clusterLink *link, int argc, sds *argv) {
    /* argv: AE_ACK <term> <success> <last-log-index> [<repl-offset>] */
    if (argc < 4) return 1;
    serverLog(LL_DEBUG, "Received AE_ACK: term=%s success=%s last_index=%s from %.40s",
              argv[1], argv[2], argv[3], link->node ? link->node->name : "?");

    clusterRaftState *rs = RAFT_STATE();
    uint64_t msg_term = strtoull(argv[1], NULL, 10);
    int success = atoi(argv[2]);
    uint64_t follower_last_index = strtoull(argv[3], NULL, 10);
    long long follower_repl_offset = (argc >= 5) ? strtoll(argv[4], NULL, 10) : 0;

    clusterRaftMaybeStepDown(msg_term);
    if (rs->role != RAFT_ROLE_LEADER) return 1;

    clusterNode *node = link->node;
    if (!node) return 1;
    clusterNodeRaftData *rd = RAFT_NODE(node);
    rd->last_ack_time = monotonicMs();
    /* Keep node->repl_offset in sync for CLUSTER SLOTS/SHARDS on the leader. */
    long long prev_offset = node->repl_offset;
    node->repl_offset = follower_repl_offset;

    /* Broadcast this node's offset to all peers when it transitions
     * between 0 and non-zero (replica finishes sync or starts resync). */
    if (prev_offset != follower_repl_offset &&
        (prev_offset == 0 || follower_repl_offset == 0) &&
        !(node->flags & CLUSTER_NODE_JOINING)) {
        clusterRaftBroadcastNodeOffset(node, follower_repl_offset);
    }

    /* Propose NODE_RECOVER if the node is back. */
    if (nodeFailed(node) && !rd->pending_fail_change) {
        rd->pending_fail_change = 1;
        sds entry = sdsnew("NODE_RECOVER ");
        entry = sdscatlen(entry, node->name, CLUSTER_NAMELEN);
        clusterRaftPropose(entry, NULL, NULL);
        sdsfree(entry);
        serverLog(LL_NOTICE, "Node %.40s is back, proposing NODE_RECOVER.", node->name);
    }

    if (success) {
        /* Follower accepted — update matchIndex/nextIndex. */
        rd->match_index = follower_last_index;
        rd->next_index = follower_last_index + 1;

        /* Check if we can advance commitIndex (Raft paper §5.3/§5.4). */
        for (uint64_t i = rs->log_count; i > 0; i--) {
            uint64_t idx = rs->log[i - 1]->index;
            if (idx <= rs->commit_index) break;
            if (rs->log[i - 1]->term != rs->current_term) continue;

            int matches = 1; /* Myself, the leader, always a voter */
            dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
            dictEntry *de;
            while ((de = dictNext(di)) != NULL) {
                clusterNode *peer = dictGetVal(de);
                if (peer == myself) continue;
                if (!nodeIsVoter(peer)) continue;
                if (RAFT_NODE(peer)->match_index >= idx) matches++;
            }
            dictReleaseIterator(di);

            int quorum = clusterRaftQuorum();
            if (matches >= quorum) {
                rs->commit_index = idx;
                break;
            }
        }

        /* Apply newly committed entries. */
        uint64_t prev_commit = rs->last_applied;
        raftApplyCommitted();
        /* If the node we were talking to was forgotten, bail out. */
        if (!link->node) {
            freeClusterLink(link);
            return 0;
        }
        /* If commit advanced, broadcast so followers learn the new commit. */
        if (rs->last_applied > prev_commit) {
            rs->todo_broadcast_ae = 1;
        }
    } else {
        /* Follower rejected — decrement nextIndex and retry immediately. */
        if (rd->next_index > 1) rd->next_index--;
        clusterRaftSendAppendEntries(node->link, node);
    }
    return 1;
}

/* True for a node that we know of but isn't in the log yet, and so isn't in
 * the snapshot with these node IDs. */
static bool clusterRaftIsLocalOnlyNode(clusterNode *node, dict *snapshot_ids) {
    if (!(node->flags & (CLUSTER_NODE_JOINING | CLUSTER_NODE_HANDSHAKE))) return false;
    sds name = sdsnewlen(node->name, CLUSTER_NAMELEN);
    bool in_snapshot = dictFind(snapshot_ids, name) != NULL;
    sdsfree(name);
    return !in_snapshot;
}

/* INSTALL_SNAPSHOT <leader-id> <term> <last-index> <last-term>\n<node lines> */
static int clusterRaftProcessInstallSnapshot(clusterLink *link, int argc, sds *argv, sds *config_lines, int config_line_count) {
    if (argc < 5) return 1;
    if (sdslen(argv[1]) != CLUSTER_NAMELEN) return 1;

    clusterRaftState *rs = RAFT_STATE();
    uint64_t msg_term = strtoull(argv[2], NULL, 10);
    uint64_t snap_index = strtoull(argv[3], NULL, 10);
    uint64_t snap_term = strtoull(argv[4], NULL, 10);

    if (msg_term < rs->current_term) return 1;
    /* A leader's snapshot ends at an entry from its term or an earlier one,
     * and terms start at 1. Reject anything else before acting on it. */
    if (snap_index == 0 || snap_term == 0 || snap_term > msg_term) {
        serverLog(LL_WARNING, "Rejecting INSTALL_SNAPSHOT from %.40s: invalid index %llu or term %llu.",
                  argv[1], (unsigned long long)snap_index, (unsigned long long)snap_term);
        return 1;
    }
    clusterRaftMaybeStepDown(msg_term);
    if (rs->role != RAFT_ROLE_JOINER && rs->role != RAFT_ROLE_LEARNER) rs->role = RAFT_ROLE_FOLLOWER;
    rs->last_heartbeat = monotonicMs();
    clusterRaftSetLeader(argv[1]);

    /* If we're already past the snapshot, ignore. */
    if (snap_index <= rs->last_applied) {
        rs->todo_send_ae_ack = 1;
        return 1;
    }

    /* If our log has the snapshot's last entry, keep the entries after it,
     * which we may have acknowledged, and just commit up to the snapshot
     * (Raft paper, section 7). */
    if (raftLogTermAt(snap_index) == snap_term) {
        if (snap_index > rs->commit_index) rs->commit_index = snap_index;
        raftApplyCommitted();
        rs->todo_send_ae_ack = 1;
        return 1;
    }

    /* Check all node lines before changing any state. If one is invalid, a
     * node appears twice, or the sender isn't included, reject the snapshot.
     * The leader sends it again later. The leader strips its myself flag, so
     * a line with it is invalid too. */
    bool sender_included = false;
    dict *seen = dictCreate(&clusterNodesDictType);
    for (int i = 0; i < config_line_count; i++) {
        if (sdslen(config_lines[i]) == 0) continue;
        int largc;
        sds *largv = sdssplitargs(config_lines[i], &largc);
        bool valid = largv && clusterValidateNodeLine(largv, largc) == C_OK &&
                     !strcasestr(largv[2], "myself") && dictFind(seen, largv[0]) == NULL;
        if (valid) {
            dictAdd(seen, sdsdup(largv[0]), NULL);
            if (!memcmp(largv[0], argv[1], CLUSTER_NAMELEN)) sender_included = true;
        }
        if (largv) sdsfreesplitres(largv, largc);
        if (!valid) {
            serverLog(LL_WARNING, "Rejecting INSTALL_SNAPSHOT from %.40s: invalid node line: %s",
                      argv[1], config_lines[i]);
            dictRelease(seen);
            clusterRaftSendAppendEntriesResponse(link, rs->current_term, 0);
            return 1;
        }
    }
    if (!sender_included) {
        serverLog(LL_WARNING, "Rejecting INSTALL_SNAPSHOT from %.40s: the sender isn't included", argv[1]);
        dictRelease(seen);
        clusterRaftSendAppendEntriesResponse(link, rs->current_term, 0);
        return 1;
    }

    /* Clear slots — the snapshot will re-assign them. Remember the old owners
     * to update the local slot state afterwards, and keep the sharded pubsub
     * subscriptions until we know which slots changed owner. Shard membership
     * is kept: loading a node line moves a node whose shard id changed, and
     * removed nodes leave their shard. */
    clusterNode **old_owners = zmalloc(sizeof(server.cluster->slots));
    memcpy(old_owners, server.cluster->slots, sizeof(server.cluster->slots));
    for (int j = 0; j < CLUSTER_SLOTS; j++) clusterDelSlotKeepChannels(j);

    /* Clear raft log. */
    for (uint64_t i = 0; i < rs->log_count; i++) raftLogFree(rs->log[i]);
    rs->log_count = 0;
    rs->log_trim_index = 0;
    rs->log_trim_term = 0;

    /* clusterLoadNodeLine() only adds flags and replica links to a node that
     * already exists. Reset them first so that the snapshot fully defines the
     * new state. MIGRATE_TO isn't part of the node lines, but loading them
     * sets it again, as when loading nodes.conf. A joiner keeps JOINING until it
     * finds itself in the snapshot. Nodes that we know of but that aren't in
     * the log yet (JOINING or HANDSHAKE) are left out of snapshots, so if the
     * snapshot doesn't include them, they are kept as they are. */
    {
        dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);
            if (node != myself && clusterRaftIsLocalOnlyNode(node, seen)) continue;
            int keep = CLUSTER_NODE_MYSELF;
            if (node == myself && rs->role == RAFT_ROLE_JOINER) keep |= CLUSTER_NODE_JOINING;
            node->flags &= keep;
            if (node != myself) clusterRaftClearOptionalNodeInfo(node);
            node->replicaof = NULL;
            zfree(node->replicas);
            node->replicas = NULL;
            node->num_replicas = 0;
        }
        dictReleaseIterator(di);
    }

    /* Our own address and nofailover come from our local config, and
     * applying NODE_INFO skips myself for the same reason. Remember the
     * address, without the shard id, which the snapshot decides, so it can
     * be restored after loading our node line. */
    sds my_addr = clusterNodeAppendAddressStringNoShardId(sdsempty(), myself, server.tls_cluster);

    /* Load the validated node lines — updates existing nodes, creates new
     * ones. Only an invalid address can still fail here, and the state is
     * already partly replaced. */
    dict *snapshot_nodes = dictCreate(&clusterNodesDictType);
    for (int i = 0; i < config_line_count; i++) {
        if (sdslen(config_lines[i]) == 0) continue;
        int largc;
        sds *largv = sdssplitargs(config_lines[i], &largc);
        if (clusterLoadNodeLine(largv, largc, snapshot_nodes) == C_ERR) {
            serverPanic("Corrupt INSTALL_SNAPSHOT: failed to load node line: %s", config_lines[i]);
        }
        sdsfreesplitres(largv, largc);
    }
    clusterRaftClearOptionalNodeInfo(myself);
    if (clusterNodeParseAddressString(myself, my_addr) == C_ERR) {
        serverPanic("INSTALL_SNAPSHOT: failed to restore our address: %s", my_addr);
    }
    sdsfree(my_addr);
    myself->flags &= ~(CLUSTER_NODE_NOFAILOVER | CLUSTER_NODE_FAIL | CLUSTER_NODE_PFAIL);
    if (server.cluster_replica_no_failover == CLUSTER_REPLICA_NO_FAILOVER_YES) myself->flags |= CLUSTER_NODE_NOFAILOVER;

    /* Do what applying the skipped slot changes would have done locally. The
     * old owners are still valid, since nodes are only deleted below. */
    for (int j = 0; j < CLUSTER_SLOTS; j++) {
        if (old_owners[j] != server.cluster->slots[j]) {
            removeChannelsInSlot(j);
            hotkeysPurgeSlot(j);
            clusterRaftUpdateLocalSlotState(j, old_owners[j], server.cluster->slots[j]);
        }
    }
    zfree(old_owners);

    /* Remove nodes not present in the snapshot. */
    {
        dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);
            if (node == myself) continue;
            sds name = sdsnewlen(node->name, CLUSTER_NAMELEN);
            int in_snapshot = dictFind(snapshot_nodes, name) != NULL;
            sdsfree(name);
            if (in_snapshot || clusterRaftIsLocalOnlyNode(node, seen)) continue;
            clusterDelNode(node);
        }
        dictReleaseIterator(di);
    }
    dictRelease(seen);
    /* If we were a joiner and the snapshot includes us (our NODE_JOIN was
     * committed), promote to learner or follower, same as
     * raftLogApply(NODE_JOIN). Otherwise sync the role with our voter or
     * learner status in the snapshot, as ADD_VOTER and DEL_VOTER do. */
    if (rs->role == RAFT_ROLE_JOINER) {
        sds mykey = sdsnewlen(myself->name, CLUSTER_NAMELEN);
        int found = dictFind(snapshot_nodes, mykey) != NULL;
        sdsfree(mykey);
        if (found) {
            myself->flags &= ~CLUSTER_NODE_JOINING;
            rs->role = nodeIsLearner(myself) ? RAFT_ROLE_LEARNER : RAFT_ROLE_FOLLOWER;
            serverLog(LL_NOTICE, "Promoted from joiner to %s (via snapshot).",
                      nodeIsLearner(myself) ? "learner" : "follower");
        }
    } else {
        rs->role = nodeIsLearner(myself) ? RAFT_ROLE_LEARNER : RAFT_ROLE_FOLLOWER;
    }
    dictRelease(snapshot_nodes);

    /* Update raft state to the snapshot point. */
    rs->last_applied = snap_index;
    rs->commit_index = snap_index;
    rs->snapshot_term = snap_term;

    serverLog(LL_NOTICE, "Installed snapshot from %.40s (index=%llu, term=%llu).",
              argv[1], (unsigned long long)snap_index, (unsigned long long)snap_term);

    /* Recompute cluster size (number of voters) from the loaded snapshot. */
    server.cluster->size = 0;
    {
        dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);
            if (nodeIsVoter(node)) server.cluster->size++;
        }
        dictReleaseIterator(di);
    }

    rs->todo_save_config = 1;
    rs->todo_connect_nodes = 1;
    rs->todo_send_ae_ack = 1;
    rs->todo_update_replication = 1;
    rs->todo_update_slot_coverage = 1;
    clearCachedClusterSlotsResponse();
    /* If our primary is failed, schedule a failover, as applying its
     * NODE_FAIL would have done. */
    if (nodeIsReplica(myself) && myself->replicaof && nodeFailed(myself->replicaof) && !nodeCantFailover(myself)) {
        rs->todo_schedule_failover = 1;
    }
    /* Answer MEETs deferred while we were a joiner, as when our own
     * NODE_JOIN is applied. */
    clusterRaftProcessDeferredMeets();
    return 1;
}

/* PRE_VOTE_REQ <candidate-id> <term> <last-log-index> <last-log-term> */
static void clusterRaftSendPreVoteRequest(clusterLink *link, uint64_t term) {
    sds msg = wireNewMsg("PRE_VOTE_REQ");
    msg = sdscatlen(msg, " ", 1);
    msg = sdscatlen(msg, myself->name, CLUSTER_NAMELEN);
    msg = sdscatfmt(msg, " %U %U %U", (unsigned long long)term,
                    (unsigned long long)raftLogLastIndex(),
                    (unsigned long long)raftLogLastTerm());
    msg = wireFinishMsg(msg);
    clusterRaftSendMsg(link, msg);
}

static void clusterRaftSendRequestVote(clusterLink *link) {
    sds msg = wireNewMsg("VOTE_REQ");
    msg = sdscatlen(msg, " ", 1);
    msg = sdscatlen(msg, myself->name, CLUSTER_NAMELEN);
    msg = sdscatfmt(msg, " %U %U %U", (unsigned long long)RAFT_STATE()->current_term,
                    (unsigned long long)raftLogLastIndex(),
                    (unsigned long long)raftLogLastTerm());
    msg = wireFinishMsg(msg);
    clusterRaftSendMsg(link, msg);
}

/* VOTE <term> <granted> */
static void clusterRaftSendVoteResponse(clusterLink *link, uint64_t term, int granted) {
    sds msg = wireNewMsg("VOTE");
    msg = sdscatfmt(msg, " %U %i", (unsigned long long)term, granted);
    msg = wireFinishMsg(msg);
    clusterRaftSendMsg(link, msg);
}

static void clusterRaftSendPreVoteResponse(clusterLink *link, uint64_t term, int granted) {
    sds msg = wireNewMsg("PRE_VOTE");
    msg = sdscatfmt(msg, " %U %i", (unsigned long long)term, granted);
    msg = wireFinishMsg(msg);
    clusterRaftSendMsg(link, msg);
}

static int clusterRaftCanGrantVote(clusterRaftState *rs, uint64_t candidate_last_index, uint64_t candidate_last_term) {
    if (rs->role == RAFT_ROLE_JOINER) return 0;
    if (nodeIsLearner(myself)) return 0;

    /* Deny vote if we recently heard from the leader (leader lease).
     * Use the base node-timeout, not the randomized election timeout —
     * the randomization is for staggering elections, not for lease duration. */
    mstime_t now = monotonicMs();
    mstime_t lease = server.cluster_node_timeout;
    if (lease < 1000) lease = 1000;
    if (rs->leader[0] != '\0' &&
        now - rs->last_heartbeat < lease) {
        return 0;
    }

    /* Candidate log must be at least as up-to-date as ours. */
    uint64_t my_last_term = raftLogLastTerm();
    if (candidate_last_term != my_last_term) {
        return candidate_last_term > my_last_term;
    }
    return candidate_last_index >= raftLogLastIndex();
}

static int clusterRaftProcessPreVoteRequest(clusterLink *link, int argc, sds *argv) {
    /* argv: PRE_VOTE_REQ <candidate-id> <term> <last-log-index> <last-log-term> */
    if (argc < 5) return 1;
    if (sdslen(argv[1]) != CLUSTER_NAMELEN) return 1;

    clusterRaftState *rs = RAFT_STATE();
    uint64_t msg_term = strtoull(argv[2], NULL, 10);
    uint64_t candidate_last_index = strtoull(argv[3], NULL, 10);
    uint64_t candidate_last_term = strtoull(argv[4], NULL, 10);
    int granted = 0;

    clusterNode *candidate = clusterLookupNode(argv[1], CLUSTER_NAMELEN);
    if (candidate && nodeIsLearner(candidate)) {
        granted = 0;
    } else if (msg_term > rs->current_term &&
               clusterRaftCanGrantVote(rs, candidate_last_index, candidate_last_term)) {
        granted = 1;
    }

    clusterRaftSendPreVoteResponse(link, granted ? msg_term : rs->current_term, granted);
    return 1;
}

static int clusterRaftProcessPreVoteResponse(clusterLink *link, int argc, sds *argv) {
    /* argv: PRE_VOTE <term> <granted> */
    if (argc < 3) return 1;

    clusterRaftState *rs = RAFT_STATE();
    uint64_t msg_term = strtoull(argv[1], NULL, 10);
    int granted = atoi(argv[2]);

    /* PRE_VOTE uses a future term (current_term+1). A granted PRE_VOTE must
     * not cause local term changes; only denied responses may carry a real
     * higher term worth stepping down for. */
    if (!granted && msg_term > rs->current_term) {
        clusterRaftMaybeStepDown(msg_term);
    }

    if (rs->role != RAFT_ROLE_PRE_CANDIDATE) return 1;
    if (msg_term != rs->current_term + 1) return 1;

    if (granted && link->node && nodeIsVoter(link->node)) {
        rs->pre_votes_received++;
        int quorum = clusterRaftQuorum();
        if (rs->pre_votes_received >= quorum) {
            clusterRaftStartElection();
        }
    }
    return 1;
}

static int clusterRaftProcessRequestVote(clusterLink *link, int argc, sds *argv) {
    /* argv: VOTE_REQ <candidate-id> <term> <last-log-index> <last-log-term> */
    if (argc < 5) return 1;
    if (sdslen(argv[1]) != CLUSTER_NAMELEN) return 1;

    clusterRaftState *rs = RAFT_STATE();
    uint64_t msg_term = strtoull(argv[2], NULL, 10);
    uint64_t candidate_last_index = strtoull(argv[3], NULL, 10);
    uint64_t candidate_last_term = strtoull(argv[4], NULL, 10);
    int granted = 0;

    clusterRaftMaybeStepDown(msg_term);

    if (msg_term < rs->current_term) {
        /* Stale term. */
    } else {
        clusterNode *candidate = clusterLookupNode(argv[1], CLUSTER_NAMELEN);
        if (candidate && nodeIsLearner(candidate)) {
            /* Learners are replicated to but cannot become Raft leaders. */
        } else if ((clusterRaftIsVotedForNone() || memcmp(rs->voted_for, argv[1], CLUSTER_NAMELEN) == 0) &&
                   clusterRaftCanGrantVote(rs, candidate_last_index, candidate_last_term)) {
            granted = 1;
            memcpy(rs->voted_for, argv[1], CLUSTER_NAMELEN);
            rs->last_heartbeat = monotonicMs(); /* Reset election timer */
            rs->todo_save_config = 1;
            serverLog(LL_NOTICE, "Voted for %.40s in term %llu.", argv[1], (unsigned long long)msg_term);
        }
    }

    if (granted) {
        /* Defer response until votedFor is persisted. todo_save_config triggers
         * a full rewrite in beforeSleep; the response is sent after that. */
        rs->todo_send_vote_response = 1;
    } else {
        clusterRaftSendVoteResponse(link, rs->current_term, 0);
    }
    return 1;
}

/* Promote to leader after winning an election (quorum reached). */
static void clusterRaftBecomeLeader(void) {
    clusterRaftState *rs = RAFT_STATE();
    char old_leader[CLUSTER_NAMELEN];
    memcpy(old_leader, rs->leader, CLUSTER_NAMELEN);
    rs->role = RAFT_ROLE_LEADER;
    memcpy(rs->leader, myself->name, CLUSTER_NAMELEN);
    serverLog(LL_NOTICE, "Elected as Raft leader (term %llu, %d votes).",
              (unsigned long long)rs->current_term, rs->votes_received);

    if (listLength(rs->pending_proposals) > 0)
        rs->todo_retry_proposals = 1;

    if (server.cluster->size <= 1) return;

    /* Initialize nextIndex for each peer (Raft paper §5.3). */
    uint64_t last = raftLogLastIndex();
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *peer = dictGetVal(de);
        if (peer == myself) continue;
        RAFT_NODE(peer)->next_index = last + 1;
        RAFT_NODE(peer)->match_index = 0;
        RAFT_NODE(peer)->pending_fail_change = 0;
        /* For the old leader, backdate last_ack_time to when we last
         * heard from it, so failure detection kicks in faster. This
         * matters if the old leader is also a primary. */
        if (memcmp(peer->name, old_leader, CLUSTER_NAMELEN) == 0) {
            RAFT_NODE(peer)->last_ack_time = rs->last_heartbeat;
        } else {
            RAFT_NODE(peer)->last_ack_time = monotonicMs();
        }
    }
    dictReleaseIterator(di);
    /* Append a no-op entry to commit prior-term entries (§5.4.2)
     * and establish the new term in the log. */
    uint64_t noop_idx = raftLogLastIndex() + 1;
    raftLogAppend(raftLogCreate(rs->current_term, noop_idx, RAFT_ENTRY_NOOP, sdsempty()));
    /* Immediate heartbeat to assert leadership. */
    clusterRaftBroadcastAppendEntries();
}

static int clusterRaftProcessRequestVoteResponse(clusterLink *link, int argc, sds *argv) {
    /* argv: VOTE <term> <granted> */
    if (argc < 3) return 1;

    clusterRaftState *rs = RAFT_STATE();
    uint64_t msg_term = strtoull(argv[1], NULL, 10);
    int granted = atoi(argv[2]);

    clusterRaftMaybeStepDown(msg_term);

    if (rs->role != RAFT_ROLE_CANDIDATE) return 1;
    if (msg_term != rs->current_term) return 1;

    if (granted && link->node && nodeIsVoter(link->node)) {
        rs->votes_received++;
        int quorum = clusterRaftQuorum();
        if (rs->votes_received >= quorum) {
            clusterRaftBecomeLeader();
        }
    }
    return 1;
}

static void clusterRaftStartPreVote(void) {
    clusterRaftState *rs = RAFT_STATE();
    if (!nodeIsVoter(myself)) return;
    rs->role = RAFT_ROLE_PRE_CANDIDATE;
    rs->pre_votes_received = 1; /* Self pre-vote */
    rs->pre_vote_started = monotonicMs();
    clusterRaftRandomizeElectionTimeout();

    uint64_t candidate_term = rs->current_term + 1;
    serverLog(LL_NOTICE, "Starting Raft pre-vote (candidate term %llu).", (unsigned long long)candidate_term);

    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node == myself || !node->link) continue;
        if (!nodeIsVoter(node)) continue;
        clusterRaftSendPreVoteRequest(node->link, candidate_term);
    }
    dictReleaseIterator(di);

    int quorum = clusterRaftQuorum();
    if (rs->pre_votes_received >= quorum) {
        clusterRaftStartElection();
    }
}

static void clusterRaftStartElection(void) {
    clusterRaftState *rs = RAFT_STATE();
    if (!nodeIsVoter(myself)) return;
    rs->current_term++;
    rs->role = RAFT_ROLE_CANDIDATE;
    memcpy(rs->voted_for, myself->name, CLUSTER_NAMELEN);
    rs->votes_received = 1; /* Vote for self */
    clusterRaftRandomizeElectionTimeout();
    rs->last_heartbeat = monotonicMs();
    rs->todo_save_config = 1;
    /* Defer sending RequestVote until after the term bump and self-vote
     * are persisted. Otherwise a crash could allow double-voting. */
    rs->todo_broadcast_vote_request = 1;

    serverLog(LL_NOTICE, "Starting Raft election (term %llu).", (unsigned long long)rs->current_term);

    /* Single-node: already have quorum. */
    int quorum = clusterRaftQuorum();
    if (rs->votes_received >= quorum) {
        clusterRaftBecomeLeader();
    }
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

static void clusterRaftInit(void) {
    clusterRaftState *rs = zcalloc(sizeof(*rs));
    server.cluster->protocol_data = rs;
    rs->role = RAFT_ROLE_FOLLOWER;
    clusterRaftRandomizeElectionTimeout();
    rs->last_heartbeat = monotonicMs();
    rs->pending_proposals = listCreate();
    rs->pending_meets = listCreate();
    rs->deferred_meets = listCreate();
    listSetFreeMethod(rs->deferred_meets, sdsfreeVoid);
    rs->my_last_committed_info = sdsempty();
    rs->last_node_info_check = monotonicMs();
    rs->last_repl_offsets_broadcast = monotonicMs();
    rs->todo_update_slot_coverage = 1;
    rs->shard_epochs = dictCreate(&raftShardEpochDictType);
    server.cluster->size = 0; /* Incremented by NODE_JOIN apply */
}

static void clusterRaftInitLast(void) {
    clusterListenerInit();

    /* A singleton (no peers) that has never been in a raft log is marked
     * JOINING, to prevent it from committing entries before joining a
     * cluster. Cleared when our own NODE_JOIN is applied (in a multi-node
     * cluster). A singleton that has been in a log, e.g. after the other
     * nodes were forgotten, keeps its state and elects itself, since the
     * election timeout only runs when there are other nodes. */
    clusterRaftState *rs = RAFT_STATE();
    if (dictSize(server.cluster->nodes) <= 1 && rs->last_applied == 0) {
        myself->flags |= CLUSTER_NODE_JOINING;
        server.cluster->size = 0;
    } else if (dictSize(server.cluster->nodes) == 1 && nodeIsVoter(myself)) {
        clusterRaftStartElection();
    }

    /* Fresh single-node cluster: become leader immediately. */
    if (dictSize(server.cluster->nodes) == 1 && server.cluster->size == 0) {
        myself->flags &= ~CLUSTER_NODE_LEARNER;
        rs->role = RAFT_ROLE_LEADER;
        if (rs->current_term == 0) rs->current_term = 1;
        memcpy(rs->leader, myself->name, CLUSTER_NAMELEN);
        serverLog(LL_NOTICE, "Single-node cluster: becoming Raft leader (term %llu).",
                  (unsigned long long)rs->current_term);
    }
}

/* Leader: detect node failures and propose NODE_FAIL. */
static void clusterRaftDetectFailures(mstime_t now) {
    mstime_t node_timeout = server.cluster_node_timeout;

    /* Check individual nodes. */
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node == myself) continue;

        if (nodeFailed(node)) {
            /* For failed nodes, drop stale links periodically so
             * clusterConnectNodes can establish a fresh connection.
             * This allows the leader to detect recovery via AE_ACK. */
            if (node->link) {
                clusterNodeRaftData *rd = RAFT_NODE(node);
                if (rd->last_ack_time > 0 &&
                    now - rd->last_ack_time > node_timeout) {
                    freeClusterLink(node->link);
                    rd->last_ack_time = now;
                }
            }
            continue;
        }
        clusterNodeRaftData *rd = RAFT_NODE(node);
        if (rd->pending_fail_change) continue;
        if (rd->last_ack_time > 0 &&
            now - rd->last_ack_time > node_timeout) {
            serverLog(LL_NOTICE, "Node %.40s not responding, proposing NODE_FAIL.", node->name);

            /* If the failed node is a primary with replicas, send
             * REPL_OFFSETS to each replica with all sibling offsets
             * so they can rank themselves for automatic failover. */
            if (!nodeIsReplica(node) && clusterNodeNumReplicas(node) > 0) {
                int num_replicas = clusterNodeNumReplicas(node);
                sds hint = wireNewMsg("REPL_OFFSETS");
                for (int r = 0; r < num_replicas; r++) {
                    clusterNode *replica = clusterNodeGetReplica(node, r);
                    long long roff = (replica == myself)
                                         ? (nodeIsReplica(myself) ? replicationGetReplicaOffset()
                                                                  : server.primary_repl_offset)
                                         : replica->repl_offset;
                    hint = sdscatlen(hint, " ", 1);
                    hint = sdscatlen(hint, replica->name, CLUSTER_NAMELEN);
                    hint = sdscatfmt(hint, " %I", roff);
                }
                hint = wireFinishMsg(hint);

                /* Build a send block and send to remote replicas. */
                size_t hint_len = sdslen(hint);
                clusterMsgSendBlock *block = clusterAllocMsgSendBlock(hint_len);
                memcpy(block->data, hint, hint_len);
                sdsfree(hint);
                for (int r = 0; r < num_replicas; r++) {
                    clusterNode *replica = clusterNodeGetReplica(node, r);
                    if (replica == myself) continue;
                    clusterLink *link = replica->link;
                    if (link) {
                        clusterLinkSendBlock(link, block);
                    } else {
                        serverLog(LL_WARNING, "No outbound link to replica %.40s for REPL_OFFSETS.",
                                  replica->name);
                    }
                }
                clusterMsgSendBlockDecrRefCount(block);
            }

            sds entry = sdsnew("NODE_FAIL ");
            entry = sdscatlen(entry, node->name, CLUSTER_NAMELEN);
            clusterRaftPropose(entry, NULL, NULL);
            sdsfree(entry);
            rd->pending_fail_change = 1;
        }
    }
    dictReleaseIterator(di);
}

/* Forward a pending proposal to the leader or append locally if we are
 * the leader. Caller must ensure leader link is available for followers. */
static void clusterRaftSendProposal(raftPendingProposal *pp, clusterLink *leader_link) {
    clusterRaftState *rs = RAFT_STATE();

    pp->propose_time = monotonicMs();
    if (rs->role == RAFT_ROLE_LEADER) {
        uint64_t idx = raftLogLastIndex() + 1;
        raftLogAppend(raftLogCreate(rs->current_term, idx, pp->type, sdsdup(pp->data)));
        serverLog(LL_NOTICE, "Leader appended retried %s (index %llu).",
                  raftEntryTypeName(pp->type), (unsigned long long)idx);
        rs->todo_broadcast_ae = 1;
    } else {
        sds entry = sdscatfmt(sdsempty(), "%s %S",
                              raftEntryTypeName(pp->type), pp->data);
        sds msg = wireNewMsg("PROPOSE");
        msg = sdscatlen(msg, " ", 1);
        msg = sdscatlen(msg, entry, sdslen(entry));
        msg = wireFinishMsg(msg);
        clusterRaftSendMsg(leader_link, msg);
        sdsfree(entry);
    }
}

/* Retry proposals from the pending list.
 * Deferred proposals (waiting for epoch advance) are always included — they
 * self-guard via raftRefreshEpochInData() and are skipped if not ready.
 * include_pending: if set, also retry pending proposals (leader-change replay). */
static void clusterRaftRetryProposals(int include_pending) {
    clusterRaftState *rs = RAFT_STATE();
    if (listLength(rs->pending_proposals) == 0) return;

    /* For followers/learners, check leader link once — it's the same for all entries. */
    clusterLink *leader_link = NULL;
    if (rs->role == RAFT_ROLE_FOLLOWER || rs->role == RAFT_ROLE_LEARNER) {
        clusterNode *leader = clusterLookupNode(rs->leader, CLUSTER_NAMELEN);
        if (!leader || !leader->link) {
            rs->todo_retry_proposals = 1;
            return;
        }
        leader_link = leader->link;
    }

    listIter li;
    listNode *ln;
    listRewind(rs->pending_proposals, &li);
    while ((ln = listNext(&li)) != NULL) {
        raftPendingProposal *pp = listNodeValue(ln);

        /* For deferred entries, update to latest shard epoch. */
        if (pp->deferred) {
            if (!raftRefreshEpochInData(pp->type, &pp->data)) continue;

            pp->deferred = 0;
        } else if (!include_pending) {
            continue;
        }

        if (rs->role == RAFT_ROLE_LEADER) {
            raftQuorumChangeStatus status = clusterRaftQuorumChangeStatus(pp->type, pp->data);

            /* A different membership transition is in flight: reject. */
            if (status == RAFT_QC_CONFLICT) {
                if (pp->callback) pp->callback(pp->ctx, GENERIC_PROPOSAL_REJECTION_MSG);
                sdsfree(pp->data);
                zfree(pp);
                listDelNode(rs->pending_proposals, ln);
                continue;
            }

            /* The identical transition is already in the log; it will
             * complete the pending proposal when applied. */
            if (status == RAFT_QC_SAME) continue;

            /* No membership transition in flight: run full pre-validation. */
            RaftProposalResult pre_result = clusterRaftPreValidate(pp->type, pp->data);
            if (pre_result != RAFT_RESULT_OK) {
                if (pre_result == RAFT_RESULT_STALE_EPOCH && pp->retries > 0) {
                    pp->retries--;
                    pp->deferred = 1;
                    rs->todo_retry_deferred = 1;
                    continue;
                }
                if (pp->callback) pp->callback(pp->ctx, raftProposalResultMsg(pre_result));
                sdsfree(pp->data);
                zfree(pp);
                listDelNode(rs->pending_proposals, ln);
                continue;
            }
        }

        clusterRaftSendProposal(pp, leader_link);
    }
}

/* myself: RAFT_ROLE_LEARNER <=> CLUSTER_NODE_LEARNER. */
static void clusterRaftAssertLearnerRole(void) {
    clusterRaftState *rs = RAFT_STATE();
    serverAssert((rs->role == RAFT_ROLE_LEARNER) == nodeIsLearner(myself));
}

/* Pick a learner eligible for automatic promotion to voter. A candidate must
 * be a learner that finished joining (no MEET flag), is not marked failed,
 * has an active link, and has fully caught up with the leader's log. */
static clusterNode *clusterRaftSelectPromotionCandidate(void) {
    uint64_t last = raftLogLastIndex();
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *n = dictGetVal(de);
        if (!nodeIsLearner(n)) continue;
        if (n->flags & CLUSTER_NODE_MEET) continue;
        if (nodeFailed(n)) continue;
        if (!n->link) continue;
        if (RAFT_NODE(n)->match_index < last) continue;
        dictReleaseIterator(di);
        return n;
    }
    dictReleaseIterator(di);
    return NULL;
}

/* Leader-only, one-way reconciliation controller: promote caught-up learners
 * to voters until the voting set reaches RAFT_TARGET_VOTERS. Reuses the
 * ADD_VOTER path. Never demotes; when size >= target it is a no-op. Stops
 * while any membership transition is in flight. */
static void clusterRaftAutoPromoteLearner(void) {
    clusterRaftState *rs = RAFT_STATE();
    if (rs->role != RAFT_ROLE_LEADER) return;
    if (server.cluster->size >= RAFT_TARGET_VOTERS) return;

    clusterNode *candidate = clusterRaftSelectPromotionCandidate();
    if (!candidate) return;

    sds data = sdsnewlen(candidate->name, CLUSTER_NAMELEN);
    /* Only one membership transition in flight at a time. */
    if (clusterRaftQuorumChangeStatus(RAFT_ENTRY_ADD_VOTER, data) != RAFT_QC_NONE) {
        sdsfree(data);
        return;
    }

    sds entry = sdsnew("ADD_VOTER ");
    entry = sdscatsds(entry, data);
    clusterRaftPropose(entry, NULL, NULL);
    sdsfree(data);
    sdsfree(entry);
}

static void clusterRaftCron(void) {
    clusterRaftState *rs = RAFT_STATE();
    mstime_t now = monotonicMs();

    clusterRaftAssertLearnerRole();

    /* Disconnect dead outbound links after node_timeout/2. data_received tracks
     * the last time we got data on our outbound link (HI, AE_ACK, votes).
     * - All roles: no data received on the current connection (HI never
     *   arrived). The peer's kernel accepted our SYN but the process is frozen.
     * - Leader: no AE_ACK. last_ack_time is reset on election, giving peers
     *   time to respond to the first AE. A new link also gets node_timeout/2
     *   to get an AE_ACK through, so it isn't freed again on every cron tick
     *   while last_ack_time is still stale. */
    {
        mstime_t stale = server.cluster_node_timeout / 2;
        dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);
            if (node == myself || !node->link) continue;
            if (node->flags & CLUSTER_NODE_JOINING) continue;
            /* link->ctime and data_received are wall-clock (mstime) times. */
            mstime_t link_age = mstime() - node->link->ctime;
            if ((node->data_received < node->link->ctime && link_age > stale) ||
                (rs->role == RAFT_ROLE_LEADER && now - RAFT_NODE(node)->last_ack_time > stale &&
                 link_age > stale)) {
                freeClusterLink(node->link);
            }
        }
        dictReleaseIterator(di);
    }

    if (!server.debug_cluster_disable_reconnection) clusterConnectNodes();

    /* Joiner timeout: if we stepped down to join a cluster but never
     * received AE, revert to singleton leader so the admin can retry. */
    if (rs->role == RAFT_ROLE_JOINER &&
        now - rs->joiner_since > server.cluster_node_timeout * 4) {
        serverLog(LL_NOTICE, "Joiner timed out waiting to be added, reverting to singleton leader.");
        rs->role = RAFT_ROLE_LEADER;
        rs->current_term++;
        memcpy(rs->voted_for, myself->name, CLUSTER_NAMELEN);
        memcpy(rs->leader, myself->name, CLUSTER_NAMELEN);
    }

    if (dictSize(server.cluster->nodes) > 1) {
        /* Follower/candidate: check election timeout. */
        if (nodeIsVoter(myself) &&
            (rs->role == RAFT_ROLE_FOLLOWER || rs->role == RAFT_ROLE_CANDIDATE) &&
            now - rs->last_heartbeat > rs->election_timeout) {
            clusterRaftStartPreVote();
        }

        if (rs->role == RAFT_ROLE_PRE_CANDIDATE &&
            now - rs->pre_vote_started > rs->election_timeout) {
            rs->role = RAFT_ROLE_FOLLOWER;
            clusterRaftRandomizeElectionTimeout();
            rs->last_heartbeat = now;
        }

        if (rs->role == RAFT_ROLE_LEADER &&
            !clusterRaftLeaderHasFreshQuorum(now)) {
            clusterRaftStepDown(now, "lost quorum freshness");
        }

        /* Expire stale pending proposals to prevent unbounded buildup
         * during prolonged partitions. The timeout covers failure detection
         * (1-2x node_timeout), the election and the commit after the last
         * time the proposal was sent; a resend to a new leader restarts it.
         * The cap bounds the total time if the leader keeps changing. */
        if (listLength(rs->pending_proposals) > 0) {
            mstime_t timeout = server.cluster_node_timeout * 3;
            mstime_t cap = server.cluster_node_timeout * 10;
            listIter ei;
            listNode *eln;
            listRewind(rs->pending_proposals, &ei);
            while ((eln = listNext(&ei)) != NULL) {
                raftPendingProposal *pp = listNodeValue(eln);
                if (now - pp->propose_time > timeout || now - pp->ctime > cap) {
                    serverLog(LL_NOTICE, "Expiring stale proposal %s after %lldms.",
                              raftEntryTypeName(pp->type), (long long)(now - pp->ctime));
                    if (pp->callback) pp->callback(pp->ctx, "-TIMEOUT Cluster consensus not reached in time");
                    sdsfree(pp->data);
                    zfree(pp);
                    listDelNode(rs->pending_proposals, eln);
                }
            }
        }

        /* Periodically check if our NODE_INFO diverged from what was last
         * committed (e.g. a CONFIG SET succeeded but the proposal timed
         * out). Re-propose if needed. Check every 10 seconds. */
        if (now - rs->last_node_info_check > 10000) {
            rs->last_node_info_check = now;
            sds current = clusterRaftBuildMyNodeInfo();
            if (sdscmp(current, rs->my_last_committed_info) != 0) {
                serverLog(LL_NOTICE, "NODE_INFO diverged from last commit. Re-proposing.");
                serverLog(LL_NOTICE, "Old committed and new proposed node-info: %s -> %s",
                          rs->my_last_committed_info, current);
                clusterRaftUpdateMyself(0);
            }
            sdsfree(current);
        }

        /* Leader */
        if (rs->role == RAFT_ROLE_LEADER) {
            /* Send periodic heartbeats. */
            mstime_t heartbeat_interval = rs->election_timeout / 10;
            if (heartbeat_interval < 100) heartbeat_interval = 100;
            if (now - rs->last_heartbeat > heartbeat_interval) {
                rs->last_heartbeat = now;
                clusterRaftBroadcastAppendEntries();
            }

            /* Broadcast all replication offsets every 10s so
             * followers have accurate data for CLUSTER SLOTS/SHARDS. */
            long long my_offset = getNodeReplicationOffset(myself);
            if (now - rs->last_repl_offsets_broadcast > REPL_OFFSETS_BROADCAST_PERIOD_MS) {
                rs->last_repl_offsets_broadcast = now;
                clusterMsgSendBlock *block = clusterRaftBuildAllOffsetsMsg();
                if (block) {
                    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
                    dictEntry *de;
                    while ((de = dictNext(di)) != NULL) {
                        clusterNode *n = dictGetVal(de);
                        if (n == myself || !n->link) continue;
                        clusterLinkSendBlock(n->link, block);
                    }
                    dictReleaseIterator(di);
                    clusterMsgSendBlockDecrRefCount(block);
                }
            } else {
                /* Broadcast leader's own offset on 0 -> non-zero transition. */
                if (myself->repl_offset == 0 && my_offset > 0) {
                    clusterRaftBroadcastNodeOffset(myself, my_offset);
                }
            }
            myself->repl_offset = my_offset;
            clusterRaftDetectFailures(now);
            clusterRaftAutoPromoteLearner();
        }
    }

    /* Coordinated manual failover: check if replication caught up. */
    if (rs->mf_end) {
        if (now > rs->mf_end) {
            /* Timeout. */
            serverLog(LL_NOTICE, "Manual failover timed out.");
            if (rs->mf_callback) rs->mf_callback(rs->mf_ctx, "manual failover timed out");
            rs->mf_end = 0;
            rs->mf_ctx = NULL;
            rs->mf_callback = NULL;
            /* On the primary, writes were paused. */
            unpauseActions(PAUSE_DURING_FAILOVER);
        } else if (nodeIsReplica(myself) && myself->replicaof && server.repl_state == REPL_STATE_CONNECTED &&
                   rs->mf_primary_offset != -1 && replicationGetReplicaOffset() == rs->mf_primary_offset) {
            clusterNode *primary = myself->replicaof;
            uint64_t epoch = clusterGetShardEpoch(primary->shard_id);
            sds entry = sdsnew("FAILOVER ");
            entry = sdscatlen(entry, myself->name, CLUSTER_NAMELEN);
            entry = sdscatlen(entry, " ", 1);
            entry = sdscatlen(entry, primary->name, CLUSTER_NAMELEN);
            entry = sdscatlen(entry, " ", 1);
            entry = sdscatlen(entry, primary->shard_id, CLUSTER_NAMELEN);
            entry = sdscatfmt(entry, " %U", (unsigned long long)epoch);
            clusterRaftPropose(entry, rs->mf_ctx, rs->mf_callback);
            sdsfree(entry);
            rs->mf_end = 0;
            rs->mf_ctx = NULL;
            rs->mf_callback = NULL;
            serverLog(LL_NOTICE, "Replication caught up, proposing FAILOVER.");
        }
    }

    /* Automatic failover: propose FAILOVER when our scheduled time arrives. */
    if (rs->failover_time && now >= rs->failover_time) {
        if (nodeIsReplica(myself) && myself->replicaof && nodeFailed(myself->replicaof)) {
            rs->failover_time = 0;
            clusterNode *primary = myself->replicaof;
            serverLog(LL_NOTICE, "Automatic failover: proposing FAILOVER for primary %.40s.",
                      primary->name);
            uint64_t epoch = clusterGetShardEpoch(primary->shard_id);
            sds entry = sdsnew("FAILOVER ");
            entry = sdscatlen(entry, myself->name, CLUSTER_NAMELEN);
            entry = sdscatlen(entry, " ", 1);
            entry = sdscatlen(entry, primary->name, CLUSTER_NAMELEN);
            entry = sdscatlen(entry, " ", 1);
            entry = sdscatlen(entry, primary->shard_id, CLUSTER_NAMELEN);
            entry = sdscatfmt(entry, " %U", (unsigned long long)epoch);
            clusterRaftPropose(entry, NULL, clusterRaftAutoFailoverCallback);
            sdsfree(entry);
        }
    }
}

static void clusterRaftCheckSlotCoverage(void) {
    int all_slots_covered = 1;
    if (server.cluster_require_full_coverage) {
        for (int j = 0; j < CLUSTER_SLOTS; j++) {
            if (server.cluster->slots[j] == NULL ||
                nodeFailed(server.cluster->slots[j])) {
                all_slots_covered = 0;
                break;
            }
        }
    }
    server.cluster->state = all_slots_covered ? CLUSTER_OK : CLUSTER_FAIL;
}

static void clusterRaftBeforeSleep(bool blocked) {
    UNUSED(blocked);
    clusterRaftState *rs = RAFT_STATE();

    if (rs->todo_connect_nodes) {
        rs->todo_connect_nodes = 0;
        clusterConnectNodes();
    }

    if (rs->todo_save_config ||
        (rs->todo_persist_log &&
         rs->last_applied - rs->last_rewrite_applied >= RAFT_LOG_REWRITE_THRESHOLD)) {
        rs->todo_save_config = 0;
        rs->todo_persist_log = 0;
        clusterSaveConfigOrDie(1);
        rs->last_rewrite_applied = rs->last_applied;
    } else if (rs->todo_persist_log) {
        /* A failed append may have left a torn tail. Replace the file
         * before sending AE_ACK. */
        if (clusterRaftPersistNewLogEntries(rs->persist_log_from) == C_ERR) {
            clusterSaveConfigOrDie(1);
            rs->last_rewrite_applied = rs->last_applied;
        }
        rs->todo_persist_log = 0;
    }
    /* Only trim once new entries are persisted. An incremental append only
     * writes the entries still in the log. */
    raftLogTrimApplied();

    /* Singleton leader: commit after persistence (quorum = 1). */
    if (rs->role == RAFT_ROLE_LEADER && server.cluster->size <= 1 &&
        raftLogLastIndex() > rs->commit_index) {
        rs->commit_index = raftLogLastIndex();
        raftApplyCommitted();
        /* If we have any peer member (voter or learner), replicate the new commit index. */
        if (clusterRaftHasPeerMembers()) rs->todo_broadcast_ae = 1;
    }

    if (rs->todo_send_ae_ack) {
        rs->todo_send_ae_ack = 0;
        clusterNode *leader = clusterLookupNode(rs->leader, CLUSTER_NAMELEN);
        /* AE arrives on the inbound link (leader connects to us). */
        clusterLink *link = leader ? (leader->inbound_link ? leader->inbound_link : leader->link) : NULL;
        if (link) {
            clusterRaftSendAppendEntriesResponse(link, rs->current_term, 1);
        }
    }

    if (rs->todo_send_vote_response) {
        rs->todo_send_vote_response = 0;
        /* Send granted vote response to the candidate we voted for. */
        clusterNode *candidate = clusterLookupNode(rs->voted_for, CLUSTER_NAMELEN);
        clusterLink *link = candidate ? (candidate->inbound_link ? candidate->inbound_link : candidate->link) : NULL;
        if (link) {
            clusterRaftSendVoteResponse(link, rs->current_term, 1);
        }
    }

    if (rs->todo_broadcast_vote_request) {
        rs->todo_broadcast_vote_request = 0;
        dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
        dictEntry *de;
        while ((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);
            if (node == myself || !node->link) continue;
            if (!nodeIsVoter(node)) continue;
            clusterRaftSendRequestVote(node->link);
        }
        dictReleaseIterator(di);
    }

    if (rs->todo_broadcast_ae) {
        rs->todo_broadcast_ae = 0;
        if (rs->role == RAFT_ROLE_LEADER) clusterRaftBroadcastAppendEntries();
    }

    if (rs->todo_retry_deferred || rs->todo_retry_proposals) {
        int include_pending = rs->todo_retry_proposals;
        rs->todo_retry_deferred = 0;
        rs->todo_retry_proposals = 0;
        clusterRaftRetryProposals(include_pending);
    }

    if (rs->todo_update_slot_coverage) {
        rs->todo_update_slot_coverage = 0;
        clusterRaftCheckSlotCoverage();
    }

    if (rs->todo_schedule_failover) {
        rs->todo_schedule_failover = 0;
        /* Schedule automatic failover if my primary is still failed. */
        if (nodeIsReplica(myself) && myself->replicaof &&
            nodeFailed(myself->replicaof) && !nodeCantFailover(myself)) {
            long long my_offset = replicationGetReplicaOffset();
            int rank = 0;
            for (int r = 0; r < clusterNodeNumReplicas(myself->replicaof); r++) {
                clusterNode *sibling = clusterNodeGetReplica(myself->replicaof, r);
                if (sibling == myself || nodeCantFailover(sibling)) continue;
                if (sibling->repl_offset > my_offset) rank++;
                if (sibling->repl_offset != my_offset) continue;
                /* Replication offset is the same. Check replica priority. */
                if (sibling->replica_priority < myself->replica_priority) rank++;
                if (sibling->replica_priority != myself->replica_priority) continue;
                /* Replica priority is the same. Lexicographically compare node id. */
                if (memcmp(sibling->name, myself->name, CLUSTER_NAMELEN) < 0) rank++;
            }
            rs->failover_time = monotonicMs() + rank * 1000;
            serverLog(LL_NOTICE, "Primary %.40s failed, scheduling failover (rank %d).",
                      myself->replicaof->name, rank);
        }
    }

    if (rs->todo_update_replication) {
        rs->todo_update_replication = 0;
        if (nodeIsReplica(myself) && myself->replicaof) {
            clusterNode *primary = myself->replicaof;
            /* Only call clusterSetPrimary if replication target changed. */
            if (!server.primary_host ||
                strcmp(server.primary_host, primary->ip) != 0 ||
                server.primary_port != getNodeDefaultReplicationPort(primary)) {
                int same_shard = memcmp(myself->shard_id, primary->shard_id, CLUSTER_NAMELEN) == 0;
                clusterSetPrimary(primary, !same_shard, !same_shard);
            }
        } else if (nodeIsPrimary(myself) && server.primary_host) {
            replicationUnsetPrimary();
        }
    }

    /* Keep myself->repl_offset up to date for CLUSTER SLOTS/SHARDS. */
    if (nodeIsReplica(myself)) {
        long long new_offset = replicationGetReplicaOffset();
        if (myself->repl_offset == 0 && new_offset > 0) {
        }
        myself->repl_offset = new_offset;
    }
}

static void clusterRaftPrepareShutdown(void) {
    clusterRaftState *rs = RAFT_STATE();
    if (rs->role != RAFT_ROLE_LEADER || server.cluster->size <= 1) return;

    /* Find the follower with the highest match_index. */
    clusterNode *best = NULL;
    uint64_t best_match = 0;
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node == myself || !node->link) continue;
        if (nodeFailed(node)) continue;
        if (node->flags & (CLUSTER_NODE_JOINING | CLUSTER_NODE_HANDSHAKE | CLUSTER_NODE_LEARNER)) continue;
        uint64_t mi = RAFT_NODE(node)->match_index;
        if (!best || mi > best_match) {
            best = node;
            best_match = mi;
        }
    }
    dictReleaseIterator(di);
    if (best) {
        sds msg = wireNewMsg("TIMEOUT_NOW");
        msg = sdscatfmt(msg, " %U", (unsigned long long)rs->current_term);
        msg = wireFinishMsg(msg);
        clusterRaftSendMsg(best->link, msg);
        /* Flush immediately so the message reaches the target even if
         * the server exits right after (SHUTDOWN NOW). */
        clusterWriteHandler(best->link->conn);
        serverLog(LL_NOTICE, "Leadership transfer: sent TIMEOUT_NOW to %.40s.", best->name);
    }
}

static void clusterRaftHandleServerShutdown(void) {
    /* Compact the log on disk so restart is fast. */
    clusterSaveConfigOrDie(1);

    clusterRaftState *rs = RAFT_STATE();
    /* Free pending proposals. */
    listIter li;
    listNode *ln;
    listRewind(rs->pending_proposals, &li);
    while ((ln = listNext(&li)) != NULL) {
        raftPendingProposal *pp = listNodeValue(ln);
        sdsfree(pp->data);
        zfree(pp);
    }
    listRelease(rs->pending_proposals);
    /* Free pending meets. */
    listRewind(rs->pending_meets, &li);
    while ((ln = listNext(&li)) != NULL) {
        raftPendingMeet *pm = listNodeValue(ln);
        sdsfree(pm->addr);
        zfree(pm);
    }
    listRelease(rs->pending_meets);
    listRelease(rs->deferred_meets);
    /* Free raft log. */
    for (uint64_t i = 0; i < rs->log_count; i++) {
        sdsfree(rs->log[i]->data);
        zfree(rs->log[i]);
    }
    zfree(rs->log);
    sdsfree(rs->my_last_committed_info);
    dictRelease(rs->shard_epochs);
    zfree(rs);
    server.cluster->protocol_data = NULL;
}

/* --------------------------------------------------------------------------
 * Message handling
 * -------------------------------------------------------------------------- */

static uint32_t clusterRaftValidateMessageHeader(char *header) {
    if (memcmp(header, "RAFT", 4) != 0) return 0;
    uint32_t totlen;
    memcpy(&totlen, header + 4, 4);
    totlen = ntohl(totlen);
    if (totlen < RAFT_HDR_SIZE) return 0;
    return totlen;
}

static int clusterRaftProcessMessage(struct clusterLink *link) {
    RAFT_STATE()->stats_bytes_received += link->rcvbuf_len;

    /* Track data received on outbound links only. Each node manages its own
     * outbound link; if it goes silent we must detect and reconnect. The
     * inbound link is the peer's responsibility. */
    if (link->node && !link->inbound) link->node->data_received = mstime();

    /* Parse the text payload after the binary header.
     * Split by \n first (AE messages have entry lines), then split
     * the first line by space for the header fields. */
    size_t payload_len = link->rcvbuf_len - RAFT_HDR_SIZE;
    sds payload = sdsnewlen(link->rcvbuf + RAFT_HDR_SIZE, payload_len);
    int line_count;
    sds *lines = sdssplitlen(payload, sdslen(payload), "\n", 1, &line_count);
    sdsfree(payload);

    int ret = 1;
    if (lines == NULL || line_count < 1) goto done;

    int argc;
    sds *argv = sdssplitlen(lines[0], sdslen(lines[0]), " ", 1, &argc);
    if (argv == NULL || argc < 1) {
        if (argv) sdsfreesplitres(argv, argc);
        goto done;
    }

    RAFT_STATE()->processing_link = link;
    if (!strcasecmp(argv[0], "HELLO")) {
        ret = clusterRaftProcessHello(link, argc, argv);
    } else if (!strcasecmp(argv[0], "HI")) {
        ret = clusterRaftProcessHi(link, argc, argv);
    } else if (!strcasecmp(argv[0], "MEET")) {
        ret = clusterRaftProcessMeet(link, argc, argv);
    } else if (!strcasecmp(argv[0], "ADD_ME")) {
        ret = clusterRaftProcessAddme(link, argc, argv);
    } else if (!strcasecmp(argv[0], "WELCOME")) {
        ret = clusterRaftProcessWelcome(link, argc, argv);
    } else if (!strcasecmp(argv[0], "MEET_REJECTED")) {
        ret = clusterRaftProcessMeetRejected(link, argc, argv);
    } else if (!strcasecmp(argv[0], "PROPOSE")) {
        ret = clusterRaftProcessPropose(link, argc, argv);
    } else if (!strcasecmp(argv[0], "REJECT")) {
        ret = clusterRaftProcessReject(link, argc, argv);
    } else if (!strcasecmp(argv[0], "AE")) {
        ret = clusterRaftProcessAppendEntries(link, argc, argv, lines + 1, line_count - 1);
    } else if (!strcasecmp(argv[0], "INSTALL_SNAPSHOT")) {
        ret = clusterRaftProcessInstallSnapshot(link, argc, argv, lines + 1, line_count - 1);
    } else if (!strcasecmp(argv[0], "AE_ACK")) {
        ret = clusterRaftProcessAppendEntriesResponse(link, argc, argv);
    } else if (!strcasecmp(argv[0], "PRE_VOTE_REQ")) {
        ret = clusterRaftProcessPreVoteRequest(link, argc, argv);
    } else if (!strcasecmp(argv[0], "PRE_VOTE")) {
        ret = clusterRaftProcessPreVoteResponse(link, argc, argv);
    } else if (!strcasecmp(argv[0], "VOTE_REQ")) {
        ret = clusterRaftProcessRequestVote(link, argc, argv);
    } else if (!strcasecmp(argv[0], "VOTE")) {
        ret = clusterRaftProcessRequestVoteResponse(link, argc, argv);
    } else if (!strcasecmp(argv[0], "TIMEOUT_NOW") && argc >= 2) {
        /* Leader transfer: immediately start an election. */
        uint64_t term = strtoull(argv[1], NULL, 10);
        clusterRaftState *rs = RAFT_STATE();
        if (nodeIsVoter(myself) && term >= rs->current_term && rs->role == RAFT_ROLE_FOLLOWER) {
            serverLog(LL_NOTICE, "Received TIMEOUT_NOW (term %llu), starting election.",
                      (unsigned long long)term);
            clusterRaftStartElection();
        }
    } else if (!strcasecmp(argv[0], "FAILOVER_PREPARE")) {
        /* Primary side: pause writes for coordinated failover. */
        if (link->node && nodeIsReplica(link->node) && link->node->replicaof == myself) {
            clusterRaftState *rs = RAFT_STATE();
            rs->mf_end = monotonicMs() + server.cluster_mf_timeout;
            /* The pause end is compared with server.mstime, not the monotonic clock. */
            pauseActions(PAUSE_DURING_FAILOVER,
                         server.mstime + (server.cluster_mf_timeout * CLUSTER_MF_PAUSE_MULT),
                         PAUSE_ACTIONS_CLIENT_WRITE_SET);
            serverLog(LL_NOTICE, "Manual failover requested by replica %.40s, pausing writes.",
                      link->node->name);
            /* Tell the replica the offset at which writes were paused, so it
             * knows when it has received all of the primary's writes. */
            sds msg = wireNewMsg("FAILOVER_PAUSED");
            msg = sdscatfmt(msg, " %I", server.primary_repl_offset);
            msg = wireFinishMsg(msg);
            clusterRaftSendMsg(link, msg);
        }
    } else if (!strcasecmp(argv[0], "FAILOVER_PAUSED") && argc >= 2) {
        /* Replica side: the primary paused writes at this offset. */
        clusterRaftState *rs = RAFT_STATE();
        if (rs->mf_end && nodeIsReplica(myself) && link->node && link->node == myself->replicaof) {
            rs->mf_primary_offset = strtoll(argv[1], NULL, 10);
            serverLog(LL_NOTICE, "Primary paused writes at offset %lld for manual failover.", rs->mf_primary_offset);
        }
    } else if (!strcasecmp(argv[0], "REPL_OFFSETS") && argc >= 3) {
        /* Leader broadcasts replication offsets for a set of nodes.
         * argv: REPL_OFFSETS <node-id> <offset> ...
         * Update node->repl_offset for CLUSTER SLOTS/SHARDS health.
         * If my primary is failed, compute failover rank from sibling offsets. */

        /* Update repl_offset for all mentioned nodes. */
        for (int i = 1; i + 1 < argc; i += 2) {
            clusterNode *node = clusterLookupNode(argv[i], sdslen(argv[i]));
            if (node && node != myself) {
                node->repl_offset = strtoll(argv[i + 1], NULL, 10);
            }
        }
    } else if (!strcasecmp(argv[0], "PUBLISH") && argc >= 4) {
        /* PUBLISH <sharded> <chan_len> <msg_len>
         * Followed by raw binary payload (channel + message) after the
         * newline. We read directly from the receive buffer to handle
         * binary-safe data containing spaces, newlines, or nulls. */
        int sharded = atoi(argv[1]);
        RAFT_STATE()->stats_publish_messages_received++;
        RAFT_STATE()->stats_pubsub_bytes_received += link->rcvbuf_len;
        size_t chan_len = strtoull(argv[2], NULL, 10);
        size_t msg_len = strtoull(argv[3], NULL, 10);
        /* Payload starts after the first \n in the raw buffer. */
        char *buf = link->rcvbuf + RAFT_HDR_SIZE;
        size_t buf_len = link->rcvbuf_len - RAFT_HDR_SIZE;
        char *nl = memchr(buf, '\n', buf_len);
        if (nl && chan_len + msg_len >= chan_len &&
            (size_t)(buf + buf_len - nl - 1) >= chan_len + msg_len) {
            char *payload = nl + 1;
            robj *chan = createStringObject(payload, chan_len);
            robj *pmsg = createStringObject(payload + chan_len, msg_len);
            pubsubPublishMessage(chan, pmsg, sharded);
            decrRefCount(chan);
            decrRefCount(pmsg);
        }
    } else if (!strcasecmp(argv[0], "MODULE") && argc >= 4) {
        /* MODULE <module_id> <type> <len>\n<payload> */
        uint64_t module_id = strtoull(argv[1], NULL, 10);
        uint8_t type = atoi(argv[2]);
        uint32_t len = strtoull(argv[3], NULL, 10);
        char *buf = link->rcvbuf + RAFT_HDR_SIZE;
        size_t buf_len = link->rcvbuf_len - RAFT_HDR_SIZE;
        char *nl = memchr(buf, '\n', buf_len);
        if (nl && (size_t)(buf + buf_len - nl - 1) >= len) {
            moduleCallClusterReceivers(link->node ? link->node->name : "",
                                       module_id, type, (const unsigned char *)(nl + 1), len);
            RAFT_STATE()->stats_module_messages_received++;
            RAFT_STATE()->stats_module_bytes_received += link->rcvbuf_len;
        }
    } else {
        serverLog(LL_WARNING, "Unknown Raft message: %s", argv[0]);
    }

    sdsfreesplitres(argv, argc);
done:
    RAFT_STATE()->processing_link = NULL;
    if (lines) sdsfreesplitres(lines, line_count);
    return ret;
}

/* Look up a pending CLUSTER MEET for this node and unblock the client. */
static void clusterRaftUnblockMeet(clusterNode *node) {
    clusterRaftState *rs = RAFT_STATE();
    if (listLength(rs->pending_meets) == 0) return;
    sds addr = sdscatfmt(sdsempty(), "%s:%i", node->ip, node->cport);
    listIter li;
    listNode *ln;
    listRewind(rs->pending_meets, &li);
    while ((ln = listNext(&li)) != NULL) {
        raftPendingMeet *pm = listNodeValue(ln);
        if (!sdscmp(pm->addr, addr)) {
            sdsfree(addr);
            listDelNode(rs->pending_meets, ln);
            pm->callback(pm->ctx, NULL);
            sdsfree(pm->addr);
            zfree(pm);
            return;
        }
    }
    sdsfree(addr);
}

static void clusterRaftPostConnect(struct clusterLink *link) {
    if (myself->ip[0] == '\0' && server.cluster_announce_ip == NULL) {
        connAddrSockName(link->conn, myself->ip, sizeof(myself->ip), NULL);
        serverLog(LL_NOTICE, "IP address for this node updated to %s", myself->ip);
    }
    clusterRaftSendGreeting(link, "HELLO");
    if (link->node && nodeInHandshake(link->node)) {
        clusterRaftSendMeet(link);
    }
}

/* --------------------------------------------------------------------------
 * Config updates — broadcast metadata changes through Raft log
 * -------------------------------------------------------------------------- */

/* Build the NODE_INFO data string for myself: "<node-id> <address> <flags>".
 * Caller must sdsfree() the result. */
static sds clusterRaftBuildMyNodeInfo(void) {
    sds s = sdscatlen(sdsempty(), myself->name, CLUSTER_NAMELEN);
    s = sdscatlen(s, " ", 1);
    s = clusterNodeAppendAddressStringNoShardId(s, myself, server.tls_cluster);
    s = sdscatlen(s, " ", 1);
    s = sdscat(s, (myself->flags & CLUSTER_NODE_NOFAILOVER) ? "nofailover" : "noflags");
    return s;
}

static void clusterRaftUpdateMyself(int old_flags) {
    UNUSED(old_flags);
    /* Clear cached CLUSTER SLOTS immediately — our address/hostname
     * has already changed in the config layer. */
    clearCachedClusterSlotsResponse();
    /* Don't propose NODE_INFO before our own NODE_JOIN is in the log,
     * or if our IP is not yet known. */
    if ((myself->flags & CLUSTER_NODE_JOINING) || myself->ip[0] == '\0') return;
    /* Propose NODE_INFO to propagate the change to other nodes. */
    sds data = clusterRaftBuildMyNodeInfo();
    sds entry = sdsnew("NODE_INFO ");
    entry = sdscatsds(entry, data);
    sdsfree(data);
    clusterRaftPropose(entry, NULL, NULL);
    sdsfree(entry);
}

static void clusterRaftScheduleUpdateState(void) {
    RAFT_STATE()->todo_update_slot_coverage = 1;
}

/* --------------------------------------------------------------------------
 * Node serialization (nodes.conf)
 * -------------------------------------------------------------------------- */

static void clusterRaftInitNodeData(clusterNode *node) {
    node->protocol_data = zcalloc(sizeof(clusterNodeRaftData));
}

static void clusterRaftFreeNodeData(clusterNode *node) {
    zfree(node->protocol_data);
}

/* For raft, the config_epoch field in nodes.conf stores the shard epoch.
 * ping_sent and pong_received have no meaning in raft (no gossip). */
static void clusterRaftGetNodePingPongEpoch(clusterNode *node, long long *ping_sent, long long *pong_received, uint64_t *config_epoch) {
    *ping_sent = 0;
    *pong_received = 0;
    *config_epoch = clusterGetShardEpoch(node->shard_id);
}

/* On load, the config_epoch field is the shard epoch for this node's shard.
 * We use it to restore the shard_epochs dict. ping/pong are ignored. */
static void clusterRaftSetNodePingPongEpoch(clusterNode *node, int ping_active, int pong_active, uint64_t shard_epoch) {
    UNUSED(ping_active);
    UNUSED(pong_active);
    uint64_t current = clusterGetShardEpoch(node->shard_id);
    if (shard_epoch > current) {
        clusterSetShardEpoch(node->shard_id, shard_epoch);
    }
}

static sds clusterRaftAppendVarsLine(sds config) {
    clusterRaftState *rs = RAFT_STATE();
    config = sdscatprintf(config, "vars currentTerm %llu lastApplied %llu snapshotTerm %llu",
                          (unsigned long long)rs->current_term,
                          (unsigned long long)rs->last_applied,
                          (unsigned long long)rs->snapshot_term);
    if (rs->voted_for[0]) {
        config = sdscat(config, " votedFor ");
        config = sdscatlen(config, rs->voted_for, CLUSTER_NAMELEN);
    }
    if (rs->leader[0]) {
        config = sdscat(config, " raftLeader ");
        config = sdscatlen(config, rs->leader, CLUSTER_NAMELEN);
    }
    config = sdscatlen(config, "\n", 1);
    return config;
}

static int clusterRaftParseVarsLine(const char *name, const char *value) {
    clusterRaftState *rs = RAFT_STATE();
    if (!strcasecmp(name, "currentTerm")) {
        rs->current_term = strtoull(value, NULL, 10);
        return 1;
    } else if (!strcasecmp(name, "lastApplied")) {
        rs->last_applied = strtoull(value, NULL, 10);
        rs->commit_index = rs->last_applied;
        return 1;
    } else if (!strcasecmp(name, "snapshotTerm")) {
        rs->snapshot_term = strtoull(value, NULL, 10);
        return 1;
    } else if (!strcasecmp(name, "votedFor")) {
        memcpy(rs->voted_for, value, CLUSTER_NAMELEN);
        return 1;
    } else if (!strcasecmp(name, "raftLeader")) {
        memcpy(rs->leader, value, CLUSTER_NAMELEN);
        return 1;
    }
    return 0;
}

/* Format a single log line with CRC64 checksum. */
static sds raftLogFormatLine(sds buf, raftLogEntry *e) {
    sds payload = sdscatprintf(sdsempty(), "%llu %llu %s",
                               (unsigned long long)e->index,
                               (unsigned long long)e->term,
                               raftEntryTypeName(e->type));
    if (sdslen(e->data) > 0) {
        payload = sdscatlen(payload, " ", 1);
        payload = sdscatsds(payload, e->data);
    }
    uint64_t crc = crc64(0, (unsigned char *)payload, sdslen(payload));
    buf = sdscatprintf(buf, "log %016llx %s\n",
                       (unsigned long long)crc, payload);
    sdsfree(payload);
    return buf;
}

/* Append uncommitted log entries (index > last_applied) as "log" lines
 * at the end of nodes.conf during a full rewrite. */
static sds clusterRaftAppendLogLines(sds config) {
    clusterRaftState *rs = RAFT_STATE();
    for (uint64_t i = 0; i < rs->log_count; i++) {
        raftLogEntry *e = rs->log[i];
        if (e->index <= rs->last_applied) continue;
        config = raftLogFormatLine(config, e);
    }
    return config;
}

/* Append all log entries from index 'from' onwards to nodes.conf. Called
 * from beforeSleep to batch multiple entries into a single write+fsync.
 * Returns C_OK when every byte has been written and fsynced. Returns C_ERR
 * if the append did not complete; a short write may already be on the file,
 * and the caller replaces it with a full rewrite before sending AE_ACK. */
static int clusterRaftPersistNewLogEntries(uint64_t from) {
    clusterRaftState *rs = RAFT_STATE();
    sds buf = sdsempty();
    size_t offset = 0, len;
    int fd;
    for (uint64_t i = 0; i < rs->log_count; i++) {
        raftLogEntry *e = rs->log[i];
        if (e->index < from) continue;
        buf = raftLogFormatLine(buf, e);
    }
    len = sdslen(buf);
    if (len == 0) {
        sdsfree(buf);
        return C_OK;
    }
    fd = open(server.cluster_configfile, O_WRONLY | O_APPEND);
    if (fd == -1) {
        serverLog(LL_WARNING, "Could not open cluster config for log append: %s", strerror(errno));
        sdsfree(buf);
        return C_ERR;
    }
    while (offset < len) {
        ssize_t written = write(fd, buf + offset, len - offset);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) continue;
            serverLog(LL_WARNING, "Could not append log entries to cluster config: %s", strerror(errno));
            close(fd);
            sdsfree(buf);
            return C_ERR;
        }
        offset += written;
    }
    if (valkey_fsync(fd) == -1) {
        serverLog(LL_WARNING, "Could not sync cluster config log append: %s", strerror(errno));
        close(fd);
        sdsfree(buf);
        return C_ERR;
    }
    close(fd);
    sdsfree(buf);
    return C_OK;
}


static int clusterRaftParseLogLine(sds *argv, int argc) {
    /* Format: log <crc64hex> <index> <term> <type> [<data...>] */
    if (argc < 5) {
        serverLog(LL_WARNING, "Corrupt raft log line: too few fields (%d).", argc);
        return C_ERR;
    }

    /* Verify CRC over everything after the checksum field. */
    uint64_t expected_crc = strtoull(argv[1], NULL, 16);
    sds payload = sdsjoinsds(argv + 2, argc - 2, " ", 1);
    uint64_t actual_crc = crc64(0, (unsigned char *)payload, sdslen(payload));
    sdsfree(payload);
    if (expected_crc != 0 && expected_crc != actual_crc) {
        serverLog(LL_WARNING, "Corrupt raft log line: CRC mismatch.");
        return C_ERR;
    }

    uint64_t index = strtoull(argv[2], NULL, 10);
    /* Verify indices are consecutive (catches missing log lines). */
    clusterRaftState *rs = RAFT_STATE();
    uint64_t expected_index = raftLogLastIndex() > 0 ? raftLogLastIndex() + 1 : rs->last_applied + 1;
    if (index != expected_index) {
        serverLog(LL_WARNING, "Corrupt raft log: index gap (expected %llu, got %llu).",
                  (unsigned long long)expected_index, (unsigned long long)index);
        return C_ERR;
    }
    uint64_t term = strtoull(argv[3], NULL, 10);
    if (term == 0) {
        serverLog(LL_WARNING, "Corrupt raft log line at index %llu: term 0.", (unsigned long long)index);
        return C_ERR;
    }
    int type = raftEntryTypeByName(argv[4]);
    if (type < 0) {
        serverLog(LL_WARNING, "Corrupt raft log line at index %llu: unknown type '%s'.",
                  (unsigned long long)index, argv[4]);
        return C_ERR;
    }

    /* Reconstruct data from remaining args (space-separated). */
    sds data = sdsempty();
    for (int i = 5; i < argc; i++) {
        if (i > 5) data = sdscatlen(data, " ", 1);
        data = sdscatsds(data, argv[i]);
    }

    raftLogEntry *e = raftLogCreate(term, index, type, data);
    raftLogAppend(e);
    return C_OK;
}

static void clusterRaftPostLoad(void) {
    clusterRaftState *rs = RAFT_STATE();
    /* commit_index was set to last_applied in parseVarsLine. On startup,
     * the leader will update it via AE. For now, ensure it's consistent. */
    if (rs->commit_index < rs->last_applied)
        rs->commit_index = rs->last_applied;
    /* The log consistency check relies on knowing the term at last_applied. */
    if (rs->last_applied > 0 && rs->snapshot_term == 0) {
        serverPanic("Corrupt cluster config: lastApplied %llu without snapshotTerm.",
                    (unsigned long long)rs->last_applied);
    }
    /* We can't have applied an entry from a later term than our current one. */
    if (rs->current_term < rs->snapshot_term) {
        serverPanic("Corrupt cluster config: currentTerm %llu is lower than snapshotTerm %llu.",
                    (unsigned long long)rs->current_term, (unsigned long long)rs->snapshot_term);
    }
    /* Schedule a full rewrite to produce a clean file (removes any
     * incomplete trailing line from a short write). */
    rs->todo_save_config = 1;
    /* Restore cluster size from loaded nodes (NODE_JOIN already applied). */
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (nodeIsVoter(node)) server.cluster->size++;
    }
    dictReleaseIterator(di);
    /* Sync Raft role with the persisted LEARNER flag for myself.
     * Note: global `myself` is not set yet when postLoad runs. */
    if (server.cluster->myself && nodeIsLearner(server.cluster->myself))
        rs->role = RAFT_ROLE_LEARNER;
    /* If we're a replica, start replication. */
    if (server.cluster->myself &&
        nodeIsReplica(server.cluster->myself) &&
        server.cluster->myself->replicaof) {
        rs->todo_update_replication = 1;
    }
}

/* --------------------------------------------------------------------------
 * Message propagation
 * -------------------------------------------------------------------------- */

/* A PUBLISH message is the header, the "PUBLISH <sharded> <chanlen> <msglen>\n"
 * line and the payload, and its total length must fit the uint32_t totlen. */
static int clusterRaftPublishMessageLengthValid(robj *channel, robj *message) {
    size_t channel_len = stringObjectLen(channel);
    size_t message_len = stringObjectLen(message);
    size_t max_line_len = strlen("PUBLISH 1  \n") + 2 * 20; /* Two 64-bit decimal lengths. */
    size_t max_payload_len = (size_t)UINT32_MAX - RAFT_HDR_SIZE - max_line_len;

    return channel_len <= max_payload_len && message_len <= max_payload_len - channel_len;
}

static void clusterRaftPropagatePublish(robj *channel, robj *message, int sharded) {
    sds chan = objectGetVal(channel);
    sds data = objectGetVal(message);
    sds msg = wireNewMsg("PUBLISH");
    msg = sdscatfmt(msg, " %i %U %U\n", sharded,
                    (unsigned long long)sdslen(chan),
                    (unsigned long long)sdslen(data));
    msg = sdscatlen(msg, chan, sdslen(chan));
    msg = sdscatlen(msg, data, sdslen(data));
    msg = wireFinishMsg(msg);

    int sent = clusterRaftBroadcast(msg, sharded ? myself->shard_id : NULL);
    RAFT_STATE()->stats_publish_messages_sent += sent;
    RAFT_STATE()->stats_pubsub_bytes_sent += sdslen(msg) * sent;
    sdsfree(msg);
}

static int clusterRaftSendModuleMessage(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len) {
    sds msg = wireNewMsg("MODULE");
    msg = sdscatfmt(msg, " %U %i %U\n", (unsigned long long)module_id, (int)type, (unsigned long long)len);
    msg = sdscatlen(msg, payload, len);
    msg = wireFinishMsg(msg);
    size_t msg_len = sdslen(msg);

    if (target) {
        clusterNode *node = clusterLookupNode(target, strlen(target));
        if (!node || !node->link) {
            sdsfree(msg);
            return C_ERR;
        }
        clusterRaftSendMsg(node->link, msg);
        RAFT_STATE()->stats_module_messages_sent++;
        RAFT_STATE()->stats_module_bytes_sent += msg_len;
    } else {
        int sent = clusterRaftBroadcast(msg, NULL);
        sdsfree(msg);
        RAFT_STATE()->stats_module_messages_sent += sent;
        RAFT_STATE()->stats_module_bytes_sent += msg_len * sent;
    }
    return C_OK;
}

/* --------------------------------------------------------------------------
 * Info and stats
 * -------------------------------------------------------------------------- */

static unsigned long clusterRaftGetConnectionsCount(void) {
    return dictSize(server.cluster->nodes) - 1;
}

static void clusterRaftResetStats(void) {
    clusterRaftState *rs = RAFT_STATE();
    rs->stats_module_messages_sent = 0;
    rs->stats_module_messages_received = 0;
    rs->stats_publish_messages_sent = 0;
    rs->stats_publish_messages_received = 0;
    rs->stats_bytes_sent = 0;
    rs->stats_bytes_received = 0;
    rs->stats_pubsub_bytes_sent = 0;
    rs->stats_pubsub_bytes_received = 0;
    rs->stats_module_bytes_sent = 0;
    rs->stats_module_bytes_received = 0;
}

static sds clusterRaftAppendInfoFields(sds info) {
    clusterRaftState *rs = RAFT_STATE();

    /* Common cluster state fields. */
    int slots_assigned = 0, slots_ok = 0;
    dictIterator *di = dictGetIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node->numslots) {
            slots_assigned += node->numslots;
            slots_ok += node->numslots;
        }
    }
    dictReleaseIterator(di);

    const char *statestr = (server.cluster->state == CLUSTER_OK) ? "ok" : "fail";
    info = sdscatfmt(info,
                     "cluster_state:%s\r\n"
                     "cluster_slots_assigned:%i\r\n"
                     "cluster_slots_ok:%i\r\n"
                     "cluster_slots_pfail:0\r\n"
                     "cluster_slots_fail:0\r\n"
                     "cluster_known_nodes:%U\r\n"
                     "cluster_size:%i\r\n"
                     "cluster_config_save_status:%s\r\n"
                     "cluster_config_last_save_time:%I\r\n",
                     statestr, slots_assigned, slots_ok,
                     (unsigned long long)dictSize(server.cluster->nodes),
                     server.cluster->size,
                     (atomic_load_explicit(&server.cluster_config_save_status, memory_order_relaxed) == C_OK) ? "ok" : "err",
                     (long long)atomic_load_explicit(&server.cluster_config_last_save_time, memory_order_relaxed));

    /* Raft-specific fields. */
    const char *role_str = rs->role == RAFT_ROLE_LEADER          ? "leader"
                           : rs->role == RAFT_ROLE_CANDIDATE     ? "candidate"
                           : rs->role == RAFT_ROLE_PRE_CANDIDATE ? "pre-candidate"
                           : rs->role == RAFT_ROLE_LEARNER       ? "learner"
                           : rs->role == RAFT_ROLE_JOINER        ? "joiner"
                                                                 : "follower";
    info = sdscatprintf(info,
                        "cluster_raft_role:%s\r\n"
                        "cluster_raft_current_term:%llu\r\n"
                        "cluster_raft_commit_index:%llu\r\n"
                        "cluster_raft_last_applied:%llu\r\n"
                        "cluster_raft_log_entries:%llu\r\n"
                        "cluster_raft_leader:%.40s\r\n"
                        "cluster_stats_messages_module_sent:%lld\r\n"
                        "cluster_stats_messages_module_received:%lld\r\n"
                        "cluster_stats_messages_publish_sent:%lld\r\n"
                        "cluster_stats_messages_publish_received:%lld\r\n"
                        "cluster_stats_bytes_sent:%llu\r\n"
                        "cluster_stats_bytes_received:%llu\r\n"
                        "cluster_stats_pubsub_bytes_sent:%llu\r\n"
                        "cluster_stats_pubsub_bytes_received:%llu\r\n"
                        "cluster_stats_module_bytes_sent:%llu\r\n"
                        "cluster_stats_module_bytes_received:%llu\r\n"
                        "total_cluster_links_buffer_limit_exceeded:%llu\r\n"
                        "total_cluster_links_established_inbound:%llu\r\n"
                        "total_cluster_links_established_outbound:%llu\r\n"
                        "cluster_io_threaded_reads_processed:%lld\r\n"
                        "cluster_io_threaded_writes_processed:%lld\r\n"
                        "cluster_io_threaded_accepts_processed:%lld\r\n"
                        "cluster_io_main_thread_fallbacks:%lld\r\n",
                        role_str, (unsigned long long)rs->current_term,
                        (unsigned long long)rs->commit_index, (unsigned long long)rs->last_applied,
                        (unsigned long long)rs->log_count, rs->leader,
                        rs->stats_module_messages_sent, rs->stats_module_messages_received,
                        rs->stats_publish_messages_sent, rs->stats_publish_messages_received,
                        (unsigned long long)rs->stats_bytes_sent,
                        (unsigned long long)rs->stats_bytes_received,
                        (unsigned long long)rs->stats_pubsub_bytes_sent,
                        (unsigned long long)rs->stats_pubsub_bytes_received,
                        (unsigned long long)rs->stats_module_bytes_sent,
                        (unsigned long long)rs->stats_module_bytes_received,
                        (unsigned long long)server.cluster->stat_cluster_links_buffer_limit_exceeded,
                        (unsigned long long)server.cluster->stat_cluster_links_established_inbound,
                        (unsigned long long)server.cluster->stat_cluster_links_established_outbound,
                        (long long)server.stat_cluster_threaded_reads_processed,
                        (long long)server.stat_cluster_threaded_writes_processed,
                        (long long)server.stat_cluster_threaded_accepts_processed,
                        (long long)server.stat_cluster_io_main_thread_fallbacks);
    return info;
}

static int clusterRaftGetFailureReportsCount(clusterNode *node) {
    UNUSED(node);
    return 0; /* Raft doesn't use gossip failure reports */
}

/* --------------------------------------------------------------------------
 * Failover
 * -------------------------------------------------------------------------- */

static void clusterRaftCancelManualFailover(void) {
    clusterRaftState *rs = RAFT_STATE();
    if (rs->mf_end) {
        if (rs->mf_callback) rs->mf_callback(rs->mf_ctx, "manual failover aborted");
        rs->mf_end = 0;
        rs->mf_ctx = NULL;
        rs->mf_callback = NULL;
        /* On the primary, writes were paused. Unpause regardless of the outcome. */
        unpauseActions(PAUSE_DURING_FAILOVER);
    }
}

static void clusterRaftCancelAutomaticFailover(void) {
    /* TODO */
}

/* --------------------------------------------------------------------------
 * Node management — propose changes to Raft log
 * -------------------------------------------------------------------------- */

typedef struct {
    void *orig_ctx;
    void (*orig_callback)(void *orig_ctx, const char *error);
    clusterNode *target;
} slotChangeCallbackCtx;

static void clusterRaftSlotChangeApplyCallback(void *ctx, const char *error) {
    slotChangeCallbackCtx *sc = (slotChangeCallbackCtx *)ctx;
    if (!error && clusterNodeGetPrimary(myself)->numslots == 0 && sc->target) {
        clusterHandleLostLastSlot(sc->target);
    }
    if (sc->orig_callback) sc->orig_callback(sc->orig_ctx, error);
    zfree(sc);
}

/* Compact `in` to slots owned by this node. Does not free `in`.
 * *out is newly allocated, or NULL if none remain. */
static int raftOwnedDelSlotRanges(slotRange *in, int numranges, slotRange **out) {
    clusterNode *me = getMyClusterNode();
    int owned = 0, n = 0;
    for (int i = 0; i < numranges; i++) {
        for (int s = in[i].start_slot; s <= in[i].end_slot; s++) {
            if (getNodeBySlot(s) == me) owned++;
        }
    }
    if (owned == 0) {
        *out = NULL;
        return 0;
    }

    slotRange *ranges = zmalloc(sizeof(*ranges) * owned);
    for (int i = 0; i < numranges; i++) {
        for (int s = in[i].start_slot; s <= in[i].end_slot; s++) {
            if (getNodeBySlot(s) != me) continue;
            if (n && s == ranges[n - 1].end_slot + 1)
                ranges[n - 1].end_slot = s;
            else {
                ranges[n].start_slot = s;
                ranges[n].end_slot = s;
                n++;
            }
        }
    }
    *out = ranges;
    return n;
}

/* Invoked for slot assignments and slot migrations (ADDSLOTS, SETSLOT, etc.) */
static void clusterRaftSlotChange(slotRange *ranges, int numranges, clusterNode *target, void *ctx, void (*callback)(void *ctx, const char *error)) {
    clusterRaftState *rs = RAFT_STATE();
    slotRange *filtered = NULL;

    /* DELSLOTS (target=NULL) is cluster-wide; only unassign slots we own.
     * Non-owner is a no-op that replies OK, matching gossip. Mixed ranges
     * keep only locally owned slots. */
    if (target == NULL) {
        numranges = raftOwnedDelSlotRanges(ranges, numranges, &filtered);
        if (numranges == 0) {
            if (callback) callback(ctx, NULL);
            return;
        }
        ranges = filtered;
    }

    /* Singleton with empty log: apply locally without proposing.
     * The entries will be proposed when the first node joins. */
    if (rs->role == RAFT_ROLE_LEADER && server.cluster->size <= 1 &&
        raftLogLastIndex() == 0) {
        for (int i = 0; i < numranges; i++) {
            for (int j = ranges[i].start_slot; j <= ranges[i].end_slot; j++) {
                if (target) {
                    if (server.cluster->slots[j]) clusterDelSlot(j);
                    clusterAddSlot(target, j);
                } else {
                    clusterDelSlot(j);
                }
            }
        }
        rs->todo_update_slot_coverage = 1;
        clearCachedClusterSlotsResponse();
        if (callback) callback(ctx, NULL);
        zfree(filtered);
        return;
    }

    /* Build entry: "SLOT_CHANGE <source-id-or-dash> <source-epoch> <target-id-or-dash> <target-epoch> <ranges...>" */
    clusterNode *source_owner = server.cluster->slots[ranges[0].start_slot];
    serverAssert(source_owner || target); /* At least one side must be set. */
    uint64_t source_epoch = (source_owner) ? clusterGetShardEpoch(source_owner->shard_id) : 0;
    uint64_t target_epoch = target ? clusterGetShardEpoch(target->shard_id) : 0;

    sds entry = sdsnew("SLOT_CHANGE ");
    entry = sdscatlen(entry, source_owner ? source_owner->name : "-", source_owner ? CLUSTER_NAMELEN : 1);
    entry = sdscatfmt(entry, " %U ", (unsigned long long)source_epoch);
    entry = sdscatlen(entry, target ? target->name : "-", target ? CLUSTER_NAMELEN : 1);
    entry = sdscatfmt(entry, " %U", (unsigned long long)target_epoch);
    for (int i = 0; i < numranges; i++) {
        if (ranges[i].start_slot == ranges[i].end_slot)
            entry = sdscatfmt(entry, " %i", ranges[i].start_slot);
        else
            entry = sdscatfmt(entry, " %i-%i", ranges[i].start_slot, ranges[i].end_slot);
    }

    /* Propose through Raft leader. The callback is invoked when the entry is
     * committed and applied. We use two chained callbacks, to handle some
     * side-effects like replica migration, before invoking the original
     * callback. */
    slotChangeCallbackCtx *sc = zmalloc(sizeof(slotChangeCallbackCtx));
    sc->orig_ctx = ctx;
    sc->orig_callback = callback;
    sc->target = target;
    clusterRaftPropose(entry, sc, &clusterRaftSlotChangeApplyCallback);
    sdsfree(entry);
    zfree(filtered);
}

/* Update the local state of a slot that moves from old_owner to new_owner
 * (either may be NULL): delete the keys in a slot that has left our shard,
 * and clear the migrating and importing markers that the move completes. The
 * markers are local to the shard and not part of the snapshot, so this is
 * done both when applying a slot change and when installing a snapshot. */
static void clusterRaftUpdateLocalSlotState(int slot, clusterNode *old_owner, clusterNode *new_owner) {
    clusterNode *my_primary = clusterNodeGetPrimary(myself);
    if (old_owner == myself || new_owner == myself) RAFT_STATE()->todo_save_config = 1;
    /* DELSLOTS keeps the keys and markers, like gossip. */
    if (!new_owner) return;

    /* Only while still bound, like gossip. A slot that moves to another node
     * in our shard, e.g. our replica that took over, keeps its keys. */
    if (old_owner == myself && memcmp(new_owner->shard_id, myself->shard_id, CLUSTER_NAMELEN) != 0) {
        serverLog(LL_NOTICE, "Deleting keys in dirty slot %d on node %.40s", slot, myself->name);
        delKeysInSlot(slot, server.lazyfree_lazy_server_del, true, false);
    }

    /* Target is this shard, not necessarily myself. */
    if (new_owner == my_primary && getImportingSlotSource(slot)) {
        setImportingSlotSource(slot, NULL);
        RAFT_STATE()->todo_save_config = 1;
    }

    /* Clear migrating only if the slot left our shard. */
    if (getMigratingSlotDest(slot) && old_owner &&
        !memcmp(old_owner->shard_id, my_primary->shard_id, CLUSTER_NAMELEN) &&
        memcmp(new_owner->shard_id, my_primary->shard_id, CLUSTER_NAMELEN) != 0) {
        setMigratingSlotDest(slot, NULL);
        RAFT_STATE()->todo_save_config = 1;
    }
}

/* Apply (or validate) a SLOT_CHANGE entry.
 * Format: "<source-id-or-dash> <source-epoch> <target-id-or-dash> <target-epoch> <ranges...>"
 * Ranges use the same format as nodes.conf: "0-5460" or "5461".
 * When validate_only is set, only validation is performed (no state mutation).
 * Returns RAFT_RESULT_OK on success, or RAFT_RESULT_STALE_EPOCH /
 * RAFT_RESULT_REJECTED describing the failure. */
static RaftProposalResult clusterRaftApplySlotChange(sds data, int validate_only) {
    RaftProposalResult result = RAFT_RESULT_OK;
    int argc;
    sds *argv = sdssplitlen(data, sdslen(data), " ", 1, &argc);
    /* Need at least: source + source_epoch + target + target_epoch + one_range = 5 */
    if (!argv || argc < 5) goto reject;

    slotChangeEpochInfo info;
    if (!parseSlotChangeEpochs(argv, argc, &info)) goto reject;

    /* Epoch validation: validate both source and target epochs. */
    if (!clusterValidateShardEpoch(info.source_shard_id, info.source_epoch) ||
        !clusterValidateShardEpoch(info.target_shard_id, info.target_epoch)) {
        result = RAFT_RESULT_STALE_EPOCH;
        goto reject;
    }

    if (validate_only) goto done;

    /* Range fields are argv[range_start_arg] through argv[argc-1]. */
    for (int i = info.range_start_arg; i < argc; i++) {
        int start, end;
        char *p = strchr(argv[i], '-');
        if (p) {
            *p = '\0';
            start = atoi(argv[i]);
            end = atoi(p + 1);
        } else {
            start = end = atoi(argv[i]);
        }
        for (int j = start; j <= end; j++) {
            clusterNode *old_owner = server.cluster->slots[j];
            if (old_owner == info.target_node) continue;
            clusterRaftUpdateLocalSlotState(j, old_owner, info.target_node);
            if (old_owner) clusterDelSlot(j);
            if (info.target_node) clusterAddSlot(info.target_node, j);
        }
    }
    goto done;

reject:
    /* Default to generic rejection; stale-epoch callers set result before jumping here. */
    if (result == RAFT_RESULT_OK) result = RAFT_RESULT_REJECTED;
done:
    if (argv) sdsfreesplitres(argv, argc);
    return result;
}

/* Apply (or validate) a SET_REPLICA_OF entry.
 * Format: "<replica-id> <source-shard> <source-epoch> <primary-id-or-dash> <target-shard> <target-epoch>"
 * When validate_only is set, only validation is performed (no state mutation).
 * Returns RAFT_RESULT_OK on success, or RAFT_RESULT_STALE_EPOCH /
 * RAFT_RESULT_REJECTED describing the failure. */
static RaftProposalResult clusterRaftApplySetReplica(sds data, int validate_only) {
    clusterRaftState *rs = RAFT_STATE();
    RaftProposalResult result = RAFT_RESULT_OK;
    int argc;
    sds *argv = sdssplitlen(data, sdslen(data), " ", 1, &argc);
    if (!argv || argc != 6) goto reject;

    clusterNode *replica = clusterLookupNode(argv[0], sdslen(argv[0]));
    if (!replica) goto reject;

    char *source_shard = argv[1];
    uint64_t source_epoch = strtoull(argv[2], NULL, 10);
    char *target_shard = argv[4];
    uint64_t target_epoch = strtoull(argv[5], NULL, 10);

    /* Validate both shard epochs without bumping. */
    if (!clusterValidateShardEpoch(source_shard, source_epoch) ||
        !clusterValidateShardEpoch(target_shard, target_epoch)) {
        result = RAFT_RESULT_STALE_EPOCH;
        goto reject;
    }

    /* Guard: if a primary is specified, its shard must match target-shard. */
    if (sdslen(argv[3]) == CLUSTER_NAMELEN) {
        clusterNode *primary = clusterLookupNode(argv[3], sdslen(argv[3]));
        if (primary && memcmp(primary->shard_id, target_shard, CLUSTER_NAMELEN) != 0) goto reject;
    }

    if (validate_only) goto done;

    if (replica == myself) rs->todo_save_config = 1;

    if (sdslen(argv[3]) == 1 && argv[3][0] == '-') {
        /* Promote to primary with the target-shard from the entry. */
        if (nodeIsReplica(replica)) {
            if (replica->replicaof) clusterNodeRemoveReplica(replica->replicaof, replica);
            replica->flags &= ~CLUSTER_NODE_REPLICA;
            replica->flags |= CLUSTER_NODE_PRIMARY;
            replica->replicaof = NULL;
            if (replica == myself) rs->todo_update_replication = 1;
        }
        clusterRemoveNodeFromShard(replica);
        memcpy(replica->shard_id, target_shard, CLUSTER_NAMELEN);
        clusterAddNodeToShard(target_shard, replica);
    } else {
        clusterNode *primary = clusterLookupNode(argv[3], sdslen(argv[3]));
        if (!primary) goto reject;
        if (memcmp(primary->shard_id, target_shard, CLUSTER_NAMELEN) != 0) goto reject;
        if (replica == myself) {
            if (myself->replicaof) clusterNodeRemoveReplica(myself->replicaof, myself);
            myself->flags &= ~CLUSTER_NODE_PRIMARY;
            myself->flags |= CLUSTER_NODE_REPLICA;
            myself->replicaof = primary;
            clusterNodeAddReplica(primary, myself);
            clusterRemoveNodeFromShard(myself);
            memcpy(myself->shard_id, primary->shard_id, CLUSTER_NAMELEN);
            clusterAddNodeToShard(primary->shard_id, myself);
            rs->todo_update_replication = 1;
        } else {
            if (replica->replicaof) clusterNodeRemoveReplica(replica->replicaof, replica);
            replica->flags &= ~CLUSTER_NODE_PRIMARY;
            replica->flags |= CLUSTER_NODE_REPLICA;
            replica->replicaof = primary;
            clusterNodeAddReplica(primary, replica);
            clusterRemoveNodeFromShard(replica);
            memcpy(replica->shard_id, primary->shard_id, CLUSTER_NAMELEN);
            clusterAddNodeToShard(primary->shard_id, replica);
        }
    }

    /* Bump both shard epochs after successful apply. */
    clusterSetShardEpoch(source_shard, source_epoch + 1);
    clusterSetShardEpoch(target_shard, target_epoch + 1);
    goto done;

reject:
    /* Default to generic rejection; stale-epoch callers set result before jumping here. */
    if (result == RAFT_RESULT_OK) result = RAFT_RESULT_REJECTED;
done:
    if (argv) sdsfreesplitres(argv, argc);
    return result;
}

/* Apply (or validate) a FAILOVER entry.
 * Format: "<replica-id> <primary-id> <shard-id> <shard-epoch>"
 * The replica takes over the primary's slots and becomes primary.
 * The old primary becomes a replica of the new primary.
 * When validate_only is set, only validation is performed (no state mutation).
 * Returns RAFT_RESULT_OK on success, or RAFT_RESULT_STALE_EPOCH /
 * RAFT_RESULT_REJECTED describing the failure. */
static RaftProposalResult clusterRaftApplyFailover(sds data, int validate_only) {
    clusterRaftState *rs = RAFT_STATE();
    RaftProposalResult result = RAFT_RESULT_OK;
    int argc;
    sds *argv = sdssplitlen(data, sdslen(data), " ", 1, &argc);
    if (!argv || argc != 4) goto reject;

    clusterNode *replica = clusterLookupNode(argv[0], sdslen(argv[0]));
    clusterNode *primary = clusterLookupNode(argv[1], sdslen(argv[1]));
    if (!replica || !primary) goto reject;

    /* Validate that the primary still belongs to the shard claimed in the entry. */
    if (memcmp(primary->shard_id, argv[2], CLUSTER_NAMELEN) != 0) {
        serverLog(LL_WARNING, "FAILOVER rejected: primary %.40s shard mismatch (expected %.40s, got %.40s).",
                  primary->name, argv[2], primary->shard_id);
        goto reject;
    }

    /* Epoch validation: validate against the shard-id from the entry (no bump yet). */
    uint64_t shard_epoch = strtoull(argv[3], NULL, 10);
    if (!clusterValidateShardEpoch(argv[2], shard_epoch)) {
        result = RAFT_RESULT_STALE_EPOCH;
        goto reject;
    }

    if (!nodeIsReplica(replica) || nodeIsReplica(primary) || replica->replicaof != primary) goto reject;

    if (validate_only) goto done;

    /* Transfer slots from old primary to new primary. */
    for (int j = 0; j < CLUSTER_SLOTS; j++) {
        if (server.cluster->slots[j] == primary) {
            clusterDelSlot(j);
            clusterAddSlot(replica, j);
        }
    }

    /* Promote replica to primary. */
    clusterNodeRemoveReplica(primary, replica);
    replica->flags &= ~CLUSTER_NODE_REPLICA;
    replica->flags |= CLUSTER_NODE_PRIMARY;
    replica->replicaof = NULL;

    /* Reassign all remaining replicas of the old primary to the new primary. */
    while (clusterNodeNumReplicas(primary) > 0) {
        clusterNode *r = clusterNodeGetReplica(primary, 0);
        clusterNodeRemoveReplica(primary, r);
        r->replicaof = replica;
        clusterNodeAddReplica(replica, r);
        if (r == myself) rs->todo_update_replication = 1;
    }

    /* Demote old primary to replica of new primary. */
    primary->flags &= ~CLUSTER_NODE_PRIMARY;
    primary->flags |= CLUSTER_NODE_REPLICA;
    primary->replicaof = replica;
    clusterNodeAddReplica(replica, primary);

    /* Move old primary to new primary's shard. */
    clusterRemoveNodeFromShard(primary);
    memcpy(primary->shard_id, replica->shard_id, CLUSTER_NAMELEN);
    clusterAddNodeToShard(replica->shard_id, primary);

    /* If I'm the replica being promoted, start acting as primary. */
    if (replica == myself) {
        rs->todo_update_replication = 1;
        rs->todo_save_config = 1;
    }
    /* If I'm the old primary being demoted, start replicating. */
    if (primary == myself) {
        rs->todo_update_replication = 1;
        rs->todo_save_config = 1;
    }

    /* Bump shard epoch after successful apply. */
    clusterSetShardEpoch(argv[2], shard_epoch + 1);
    goto done;

reject:
    /* Default to generic rejection; stale-epoch callers set result before jumping here. */
    if (result == RAFT_RESULT_OK) result = RAFT_RESULT_REJECTED;
done:
    if (argv) sdsfreesplitres(argv, argc);
    return result;
}

/* Apply (or validate) an ADD_VOTER entry. Format: "<node-id>" */
static RaftProposalResult clusterRaftApplyAddVoter(sds data, int validate_only) {
    clusterRaftState *rs = RAFT_STATE();
    int argc;
    sds *argv = sdssplitlen(data, sdslen(data), " ", 1, &argc);
    if (!argv || argc < 1 || sdslen(argv[0]) != CLUSTER_NAMELEN) {
        if (argv) sdsfreesplitres(argv, argc);
        return RAFT_RESULT_REJECTED;
    }

    clusterNode *node = clusterLookupNode(argv[0], CLUSTER_NAMELEN);
    if (!node || (node->flags & CLUSTER_NODE_JOINING) || !nodeIsLearner(node)) {
        sdsfreesplitres(argv, argc);
        return RAFT_RESULT_REJECTED;
    }

    if (validate_only) {
        clusterNodeRaftData *rd = RAFT_NODE(node);
        if (node != myself && (!node->link || rd->match_index < raftLogLastIndex())) {
            sdsfreesplitres(argv, argc);
            return RAFT_RESULT_REJECTED;
        }
        sdsfreesplitres(argv, argc);
        return RAFT_RESULT_OK;
    }

    node->flags &= ~CLUSTER_NODE_LEARNER;
    if (node == myself) rs->role = RAFT_ROLE_FOLLOWER;
    server.cluster->size++;
    rs->todo_save_config = 1;
    sdsfreesplitres(argv, argc);
    return RAFT_RESULT_OK;
}

/* Apply (or validate) a DEL_VOTER entry. Format: "<node-id>" */
static RaftProposalResult clusterRaftApplyDelVoter(sds data, int validate_only) {
    clusterRaftState *rs = RAFT_STATE();
    int argc;
    sds *argv = sdssplitlen(data, sdslen(data), " ", 1, &argc);
    if (!argv || argc < 1 || sdslen(argv[0]) != CLUSTER_NAMELEN) {
        if (argv) sdsfreesplitres(argv, argc);
        return RAFT_RESULT_REJECTED;
    }

    clusterNode *node = clusterLookupNode(argv[0], CLUSTER_NAMELEN);
    if (!node || (node->flags & CLUSTER_NODE_JOINING) || nodeIsLearner(node) ||
        server.cluster->size <= 1 ||
        memcmp(node->name, rs->leader, CLUSTER_NAMELEN) == 0) {
        sdsfreesplitres(argv, argc);
        return RAFT_RESULT_REJECTED;
    }

    if (validate_only) {
        /* Replacement-first: refuse to demote below the auto-promotion
         * target. The caller must add a replacement voter first. */
        if (server.cluster->size - 1 < RAFT_TARGET_VOTERS) {
            sdsfreesplitres(argv, argc);
            return RAFT_RESULT_REJECTED;
        }
        sdsfreesplitres(argv, argc);
        return RAFT_RESULT_OK;
    }

    node->flags |= CLUSTER_NODE_LEARNER;
    if (node == myself) rs->role = RAFT_ROLE_LEARNER;
    server.cluster->size--;
    rs->todo_save_config = 1;
    sdsfreesplitres(argv, argc);
    return RAFT_RESULT_OK;
}

/* Apply (or validate) a NODE_FORGET entry. Format: "<node-id> <shard-epoch>"
 * When validate_only is set, only validation is performed (no state mutation).
 * Returns RAFT_RESULT_OK on success, or RAFT_RESULT_STALE_EPOCH /
 * RAFT_RESULT_REJECTED describing the failure. */
static RaftProposalResult clusterRaftApplyNodeForget(sds data, int validate_only) {
    clusterRaftState *rs = RAFT_STATE();
    RaftProposalResult result = RAFT_RESULT_OK;
    int argc;
    sds *argv = sdssplitlen(data, sdslen(data), " ", 1, &argc);
    if (!argv || argc < 2) goto reject;

    clusterNode *node = clusterLookupNode(argv[0], sdslen(argv[0]));
    /* Already forgotten: success (idempotent; may be a no-op log entry). */
    if (!node) goto done;
    if (node == myself) goto reject;

    uint64_t epoch = strtoull(argv[1], NULL, 10);
    if (!clusterValidateShardEpoch(node->shard_id, epoch)) {
        result = RAFT_RESULT_STALE_EPOCH;
        goto reject;
    }

    if (nodeIsPrimary(node) && (node->num_replicas > 0 || node->numslots > 0)) {
        serverLog(LL_WARNING, "NODE_FORGET rejected: can't forget a primary with replicas or assigned slots.");
        goto reject;
    }

    if (validate_only) {
        /* Replacement-first: refuse to forget a voter below the target. */
        if (nodeIsVoter(node) && server.cluster->size - 1 < RAFT_TARGET_VOTERS) {
            goto reject;
        }
        goto done;
    }

    /* Save shard_id before deleting the node. */
    char shard_id[CLUSTER_NAMELEN];
    memcpy(shard_id, node->shard_id, CLUSTER_NAMELEN);
    bool was_voter = nodeIsVoter(node);

    /* clusterDelNode() frees the node's links. If we're processing a message
     * from one of them, detach it instead, so the message handler can detect
     * that the node it was talking to is gone and free the link. */
    clusterLink *link = rs->processing_link;
    if (link && link->node == node) {
        if (node->link == link) node->link = NULL;
        if (node->inbound_link == link) node->inbound_link = NULL;
        link->node = NULL;
    }
    clusterDelNode(node);
    if (was_voter && server.cluster->size > 0) server.cluster->size--;
    rs->todo_update_slot_coverage = 1;
    clearCachedClusterSlotsResponse();
    rs->todo_save_config = 1;

    /* Bump shard epoch after successful delete. */
    uint64_t current = clusterGetShardEpoch(shard_id);
    clusterSetShardEpoch(shard_id, current == 0 ? 1 : current + 1);
    goto done;

reject:
    /* Default to generic rejection; stale-epoch callers set result before jumping here. */
    if (result == RAFT_RESULT_OK) result = RAFT_RESULT_REJECTED;
done:
    if (argv) sdsfreesplitres(argv, argc);
    return result;
}


/* Callback for automatic failover proposals. On rejection (stale epoch or any
 * reason), re-schedule the failover if the primary is still failed. The next
 * attempt will rebuild the proposal with a fresh shard epoch. */
static void clusterRaftAutoFailoverCallback(void *ctx, const char *error) {
    UNUSED(ctx);
    if (!error) return; /* Success — nothing to do. */

    clusterNode *myself = getMyClusterNode();
    if (nodeIsReplica(myself) && myself->replicaof &&
        nodeFailed(myself->replicaof) && !nodeCantFailover(myself)) {
        RAFT_STATE()->todo_schedule_failover = 1;
        serverLog(LL_NOTICE, "Automatic failover proposal rejected (%s), re-scheduling.", error);
    }
}

static void clusterRaftCommandCompletion(void *ctx, const char *error) {
    client *c = consumeBlockedClientAsyncHandle((blockedAsyncHandle *)ctx);
    if (!c) return;
    if (error) {
        addReplyError(c, error);
    } else {
        addReply(c, shared.ok);
    }
    unblockClientAsync(c);
}

static void clusterRaftProposeVoterChange(const char *entry_name, const char *node_id, size_t id_len, void *ctx, void (*callback)(void *ctx, const char *error)) {
    if (id_len != CLUSTER_NAMELEN) {
        if (callback) callback(ctx, "invalid node id");
        return;
    }

    sds entry = sdsnew(entry_name);
    entry = sdscatlen(entry, " ", 1);
    entry = sdscatlen(entry, node_id, CLUSTER_NAMELEN);
    clusterRaftPropose(entry, ctx, callback);
    sdsfree(entry);
}

static void clusterRaftForgetNode(const char *node_id, size_t id_len, void *ctx, void (*callback)(void *ctx, const char *error)) {
    /* TODO: Leadership transfer will be taken care as part of issue#4069 */
    clusterRaftState *rs = RAFT_STATE();
    if (id_len == CLUSTER_NAMELEN && memcmp(node_id, rs->leader, CLUSTER_NAMELEN) == 0) {
        clusterNode *leader_node = clusterLookupNode(node_id, id_len);
        if (leader_node && !nodeFailed(leader_node)) {
            serverLog(LL_WARNING, "Attempting to forget raft leader.");
        }
    }

    clusterNode *node = clusterLookupNode(node_id, id_len < CLUSTER_NAMELEN ? id_len : CLUSTER_NAMELEN);
    if (!node) {
        if (callback) callback(ctx, NULL);
        return;
    }
    uint64_t epoch = clusterGetShardEpoch(node->shard_id);
    sds entry = sdsnew("NODE_FORGET ");
    entry = sdscatlen(entry, node_id, CLUSTER_NAMELEN);
    entry = sdscatfmt(entry, " %U", (unsigned long long)epoch);
    clusterRaftPropose(entry, ctx, callback);
    sdsfree(entry);
}

static void clusterRaftSetReplicaOf(clusterNode *primary, void *ctx, void (*callback)(void *ctx, const char *error)) {
    /* Propose SET_REPLICA_OF:
     * "<replica-id> <source-shard> <source-epoch> <primary-id-or-dash> <target-shard> <target-epoch>"
     * Source is myself's current shard. Target is the primary's shard (for assignment)
     * or a new random shard (for promotion to primary). */
    uint64_t source_epoch = clusterGetShardEpoch(myself->shard_id);
    char target_shard[CLUSTER_NAMELEN];
    uint64_t target_epoch;
    if (primary) {
        memcpy(target_shard, primary->shard_id, CLUSTER_NAMELEN);
        target_epoch = clusterGetShardEpoch(primary->shard_id);
    } else {
        getRandomHexChars(target_shard, CLUSTER_NAMELEN);
        target_epoch = 0;
    }

    sds entry = sdsnew("SET_REPLICA_OF ");
    entry = sdscatlen(entry, myself->name, CLUSTER_NAMELEN);
    entry = sdscatlen(entry, " ", 1);
    entry = sdscatlen(entry, myself->shard_id, CLUSTER_NAMELEN);
    entry = sdscatfmt(entry, " %U ", (unsigned long long)source_epoch);
    entry = sdscatlen(entry, primary ? primary->name : "-", primary ? CLUSTER_NAMELEN : 1);
    entry = sdscatlen(entry, " ", 1);
    entry = sdscatlen(entry, target_shard, CLUSTER_NAMELEN);
    entry = sdscatfmt(entry, " %U", (unsigned long long)target_epoch);
    clusterRaftPropose(entry, ctx, callback);
    sdsfree(entry);
}

static void clusterRaftFailover(int force, int takeover, void *ctx, void (*callback)(void *ctx, const char *error)) {
    UNUSED(takeover);
    clusterNode *primary = myself->replicaof;
    if (!primary) {
        if (callback) callback(ctx, "no primary to fail over");
        return;
    }

    if (force) {
        /* FORCE/TAKEOVER: propose immediately without coordination. */
        uint64_t epoch = clusterGetShardEpoch(primary->shard_id);
        sds entry = sdsnew("FAILOVER ");
        entry = sdscatlen(entry, myself->name, CLUSTER_NAMELEN);
        entry = sdscatlen(entry, " ", 1);
        entry = sdscatlen(entry, primary->name, CLUSTER_NAMELEN);
        entry = sdscatlen(entry, " ", 1);
        entry = sdscatlen(entry, primary->shard_id, CLUSTER_NAMELEN);
        entry = sdscatfmt(entry, " %U", (unsigned long long)epoch);
        clusterRaftPropose(entry, ctx, callback);
        sdsfree(entry);
    } else {
        /* Coordinated failover: ask primary to pause writes, then wait
         * for replication to catch up before proposing. */
        clusterRaftState *rs = RAFT_STATE();
        if (rs->mf_end) {
            if (callback) callback(ctx, "manual failover already in progress");
            return;
        }
        if (!primary->link) {
            if (callback) callback(ctx, "no link to primary");
            return;
        }
        rs->mf_end = monotonicMs() + server.cluster_mf_timeout;
        rs->mf_primary_offset = -1;
        rs->mf_ctx = ctx;
        rs->mf_callback = callback;

        /* Send FAILOVER_PREPARE to the primary. */
        sds msg = wireNewMsg("FAILOVER_PREPARE");
        msg = wireFinishMsg(msg);
        clusterRaftSendMsg(primary->link, msg);
    }
}

static void clusterRaftMeet(const char *ip, int port, int cport, void *ctx, void (*callback)(void *ctx, const char *error)) {
    char norm_ip[NET_IP_STR_LEN];
    struct sockaddr_storage sa;

    /* IP validation and normalization. */
    if (inet_pton(AF_INET, ip, &(((struct sockaddr_in *)&sa)->sin_addr))) {
        sa.ss_family = AF_INET;
    } else if (inet_pton(AF_INET6, ip, &(((struct sockaddr_in6 *)&sa)->sin6_addr))) {
        sa.ss_family = AF_INET6;
    } else {
        if (callback) callback(ctx, "Invalid node address specified");
        return;
    }

    memset(norm_ip, 0, NET_IP_STR_LEN);
    if (sa.ss_family == AF_INET)
        inet_ntop(AF_INET, &(((struct sockaddr_in *)&sa)->sin_addr), norm_ip, NET_IP_STR_LEN);
    else
        inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)&sa)->sin6_addr), norm_ip, NET_IP_STR_LEN);

    /* Check if we already have a handshake in progress for this address. */
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    int already = 0;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *n = dictGetVal(de);
        if (!nodeInHandshake(n)) continue;
        if (!strcasecmp(n->ip, norm_ip) && getNodeDefaultClientPort(n) == port && n->cport == cport) {
            already = 1;
            break;
        }
    }
    dictReleaseIterator(di);

    if (!already) {
        clusterNode *n = createClusterNode(NULL, CLUSTER_NODE_JOINING | CLUSTER_NODE_HANDSHAKE);
        memcpy(n->ip, norm_ip, sizeof(n->ip));
        if (server.tls_cluster) {
            n->tls_port = port;
        } else {
            n->tcp_port = port;
        }
        n->cport = cport;
        clusterAddNode(n);
    }

    /* Store callback — it fires when the nodes are joined in the cluster. */
    if (callback) {
        clusterRaftState *rs = RAFT_STATE();
        raftPendingMeet *pm = zmalloc(sizeof(*pm));
        pm->addr = sdscatfmt(sdsempty(), "%s:%i", norm_ip, cport);
        pm->ctx = ctx;
        pm->callback = callback;
        listAddNodeTail(rs->pending_meets, pm);
    }

    /* Connect to the new node in beforeSleep, not waiting for cron. */
    RAFT_STATE()->todo_connect_nodes = 1;
}

/* Reset the cluster to a clean state. Much of this is similar to
 * clusterLegacyReset and could be de-duplicated into common code. */
static void clusterRaftResetCluster(int hard) {
    clusterRaftState *rs = RAFT_STATE();

    /* Unassign all slots. */
    for (int j = 0; j < CLUSTER_SLOTS; j++) clusterDelSlot(j);

    /* Recreate shards dict. */
    dictEmpty(server.cluster->shards, NULL);
    dictEmpty(rs->shard_epochs, NULL);

    /* Forget all nodes except myself. */
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node == myself) continue;
        clusterDelNode(node);
    }
    dictReleaseIterator(di);

    /* Reset raft state. */
    for (uint64_t i = 0; i < rs->log_count; i++) {
        sdsfree(rs->log[i]->data);
        zfree(rs->log[i]);
    }
    rs->log_count = 0;
    /* Clear pending proposals. */
    listIter li;
    listNode *ln;
    listRewind(rs->pending_proposals, &li);
    while ((ln = listNext(&li)) != NULL) {
        raftPendingProposal *pp = listNodeValue(ln);
        if (pp->callback) pp->callback(pp->ctx, "cluster reset");
        sdsfree(pp->data);
        zfree(pp);
        listDelNode(rs->pending_proposals, ln);
    }
    /* Clear pending meets. */
    listRewind(rs->pending_meets, &li);
    while ((ln = listNext(&li)) != NULL) {
        raftPendingMeet *pm = listNodeValue(ln);
        if (pm->callback) pm->callback(pm->ctx, "cluster reset");
        sdsfree(pm->addr);
        zfree(pm);
        listDelNode(rs->pending_meets, ln);
    }
    listEmpty(rs->deferred_meets);
    rs->current_term = 0;
    memset(rs->leader, 0, CLUSTER_NAMELEN);
    memset(rs->voted_for, 0, CLUSTER_NAMELEN);
    rs->commit_index = 0;
    rs->last_applied = 0;
    rs->snapshot_term = 0;
    rs->log_count = 0;
    rs->log_trim_index = 0;
    rs->log_trim_term = 0;
    rs->role = RAFT_ROLE_LEADER;
    rs->failover_time = 0;
    clusterRaftCancelManualFailover();
    server.cluster->size = 0;
    myself->flags |= CLUSTER_NODE_JOINING;
    myself->flags &= ~CLUSTER_NODE_LEARNER;

    /* Hard reset: change node ID. */
    if (hard) {
        sds oldname = sdsnewlen(myself->name, CLUSTER_NAMELEN);
        dictDelete(server.cluster->nodes, oldname);
        sdsfree(oldname);
        getRandomHexChars(myself->name, CLUSTER_NAMELEN);
        clusterAddNode(myself);
    }

    /* New shard-id. */
    clusterRemoveNodeFromShard(myself);
    getRandomHexChars(myself->shard_id, CLUSTER_NAMELEN);
    clusterAddNodeToShard(myself->shard_id, myself);

    /* If replica, promote to primary and flush data. */
    if (nodeIsReplica(myself)) {
        myself->flags &= ~CLUSTER_NODE_REPLICA;
        myself->flags |= CLUSTER_NODE_PRIMARY;
        myself->replicaof = NULL;
        replicationUnsetPrimary();
        flushAllDataAndResetRDB(server.lazyfree_lazy_user_flush ? EMPTYDB_ASYNC : EMPTYDB_NO_FLAGS);
    }

    clearCachedClusterSlotsResponse();
    rs->todo_save_config = 1;
    serverLog(LL_NOTICE, "Cluster %s reset, now I'm %.40s",
              hard ? "hard" : "soft", myself->name);
}

static int clusterRaftSpecialCommand(client *c) {
    /* Accept SET-CONFIG-EPOCH and BUMPEPOCH as no-ops for compatibility
     * with cluster setup tools. Raft uses terms instead of epochs. */
    if (!strcasecmp(objectGetVal(c->argv[1]), "set-config-epoch") && c->argc == 3) {
        addReply(c, shared.ok);
        return 1;
    } else if (!strcasecmp(objectGetVal(c->argv[1]), "bumpepoch") && c->argc == 2) {
        addReply(c, shared.ok);
        return 1;
    } else if (!strcasecmp(objectGetVal(c->argv[1]), "addvoter") && c->argc == 3) {
        void *h = blockClientAsync(c);
        clusterRaftProposeVoterChange("ADD_VOTER", objectGetVal(c->argv[2]),
                                      sdslen(objectGetVal(c->argv[2])), h, clusterRaftCommandCompletion);
        return 1;
    } else if (!strcasecmp(objectGetVal(c->argv[1]), "delvoter") && c->argc == 3) {
        void *h = blockClientAsync(c);
        clusterRaftProposeVoterChange("DEL_VOTER", objectGetVal(c->argv[2]),
                                      sdslen(objectGetVal(c->argv[2])), h, clusterRaftCommandCompletion);
        return 1;
    } else if (!strcasecmp(objectGetVal(c->argv[1]), "raft") && c->argc == 4) {
        if (!strcasecmp(objectGetVal(c->argv[2]), "add-voter")) {
            void *h = blockClientAsync(c);
            clusterRaftProposeVoterChange("ADD_VOTER", objectGetVal(c->argv[3]),
                                          sdslen(objectGetVal(c->argv[3])), h, clusterRaftCommandCompletion);
            return 1;
        } else if (!strcasecmp(objectGetVal(c->argv[2]), "del-voter")) {
            void *h = blockClientAsync(c);
            clusterRaftProposeVoterChange("DEL_VOTER", objectGetVal(c->argv[3]),
                                          sdslen(objectGetVal(c->argv[3])), h, clusterRaftCommandCompletion);
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * vtable
 * -------------------------------------------------------------------------- */

clusterBusType clusterRaftBus = {
    .init = clusterRaftInit,
    .initLast = clusterRaftInitLast,
    .cron = clusterRaftCron,
    .beforeSleep = clusterRaftBeforeSleep,
    .prepareShutdown = clusterRaftPrepareShutdown,
    .handleServerShutdown = clusterRaftHandleServerShutdown,
    .validateMessageHeader = clusterRaftValidateMessageHeader,
    .processMessage = clusterRaftProcessMessage,
    .postConnect = clusterRaftPostConnect,
    .publishMessageLengthValid = clusterRaftPublishMessageLengthValid,
    .propagatePublish = clusterRaftPropagatePublish,
    .sendModuleMessage = clusterRaftSendModuleMessage,
    .onMyselfUpdated = clusterRaftUpdateMyself,
    .scheduleUpdateState = clusterRaftScheduleUpdateState,
    .getConnectionsCount = clusterRaftGetConnectionsCount,
    .resetStats = clusterRaftResetStats,
    .appendInfoFields = clusterRaftAppendInfoFields,
    .getFailureReportsCount = clusterRaftGetFailureReportsCount,
    .getNodePingPongEpoch = clusterRaftGetNodePingPongEpoch,
    .setNodePingPongEpoch = clusterRaftSetNodePingPongEpoch,
    .setNodeFailed = NULL,
    .appendVarsLine = clusterRaftAppendVarsLine,
    .parseVarsLine = clusterRaftParseVarsLine,
    .parseLogLine = clusterRaftParseLogLine,
    .appendLogLines = clusterRaftAppendLogLines,
    .postLoad = clusterRaftPostLoad,
    .initNodeData = clusterRaftInitNodeData,
    .freeNodeData = clusterRaftFreeNodeData,
    .slotChange = clusterRaftSlotChange,
    .cancelManualFailover = clusterRaftCancelManualFailover,
    .cancelAutomaticFailover = clusterRaftCancelAutomaticFailover,
    .forgetNode = clusterRaftForgetNode,
    .setReplicaOf = clusterRaftSetReplicaOf,
    .failover = clusterRaftFailover,
    .meet = clusterRaftMeet,
    .resetCluster = clusterRaftResetCluster,
    .protocolSubcommand = clusterRaftSpecialCommand,
};
