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
 * - server.cluster->size counts nodes with a committed NODE_JOIN entry.
 *   Initialized to 0; incremented by NODE_JOIN apply, decremented by
 *   NODE_FORGET apply. (This is used for computing the quorum.)
 * - The first log entry referencing any node is its NODE_JOIN. Other
 *   entry types (SLOT_CHANGE, SET_REPLICA_OF, etc.) are only proposed
 *   after the node's NODE_JOIN is in the log.
 * - CLUSTER_NODE_MEET means "not yet in the raft log" — set on all
 *   nodes (including myself) at creation, cleared on NODE_JOIN apply.
 *
 * Constraints:
 * - AE, PRE_VOTE_REQ, and VOTE_REQ are never sent to nodes with CLUSTER_NODE_MEET.
 *   This prevents AE from arriving before MEET on the same link.
 * - Configuration (quorum) takes effect on apply, not on append.
 *   This allows multiple NODE_JOINs in flight simultaneously.
 * - A singleton leader keeps its log empty until it proposes its first
 *   real entry (inviting another node). This prevents log conflicts
 *   when two singletons at the same term merge via MEET.
 *
 * See design-docs/cluster-raft.md for wire protocol and message details.
 */

#include "server.h"
#include "cluster.h"
#include "cluster_bus.h"
#include "cluster_state.h"
#include "cluster_link.h"
#include "cluster_nodes.h"

#include <arpa/inet.h>

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
    RAFT_ROLE_FOLLOWER = 1,
    RAFT_ROLE_PRE_CANDIDATE = 2,
    RAFT_ROLE_CANDIDATE = 3,
    RAFT_ROLE_LEADER = 4,
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
    mstime_t ctime; /* Creation time for expiry */
    int retries;    /* Remaining epoch-retry attempts */
    int deferred;   /* Waiting for epoch advance before re-propose */
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
    char voted_for[CLUSTER_NAMELEN]; /* All zeros = none */

    /* Volatile Raft state */
    enum raftRole role;
    uint64_t commit_index;
    uint64_t last_applied;

    /* Log */
    raftLogEntry **log;
    uint64_t log_count;
    uint64_t log_alloc;

    /* Pending proposals waiting for commit. */
    list *pending_proposals; /* list of raftPendingProposal */

    /* Pending MEET callbacks waiting for NODE_JOIN commit. */
    list *pending_meets;  /* CLUSTER MEET commands waiting for OK reply */
    list *deferred_meets; /* Inbound MEET messages deferred until size > 1 */

    /* Deferred work for beforeSleep. */
    unsigned int todo_update_slot_coverage : 1;
    unsigned int todo_invalidate_slots_cache : 1;
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
    mstime_t mf_end; /* Timeout for manual failover, 0 = not in progress */
    void *mf_ctx;    /* blockedAsyncHandle for the CLUSTER FAILOVER client */
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
    unsigned int pending_fail_change : 1; /* NODE_FAIL or NODE_RECOVER in flight */
} clusterNodeRaftData;

#define RAFT_NODE(n) ((clusterNodeRaftData *)(n)->protocol_data)

/* --------------------------------------------------------------------------
 * Wire format helpers
 * -------------------------------------------------------------------------- */

#define RAFT_HDR_SIZE 8
#define REPL_OFFSETS_BROADCAST_PERIOD_MS 10000
#define RAFT_LOG_REWRITE_THRESHOLD 100
#define PROPOSAL_MAX_RETRIES 10

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
        if (n->flags & CLUSTER_NODE_MEET) continue;
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
static RaftProposalResult clusterRaftApplySetReplica(sds data, int validate_only);
static RaftProposalResult clusterRaftApplyFailover(sds data, int validate_only);
static RaftProposalResult clusterRaftApplyNodeForget(sds data, int validate_only);
static void raftLogApply(raftLogEntry *e);
static raftLogEntry *raftLogCreate(uint64_t term, uint64_t index, uint8_t type, sds data);
static void raftLogAppend(raftLogEntry *e);
static raftLogEntry *raftLogGet(uint64_t index);
static uint64_t raftLogLastIndex(void);
static uint64_t raftLogTermAt(uint64_t index);
static void raftLogTruncateFrom(uint64_t index);
static void clusterRaftPersistNewLogEntries(uint64_t from);
static uint64_t raftLogLastTerm(void);
static int clusterRaftCanGrantVote(clusterRaftState *rs, uint64_t candidate_last_index, uint64_t candidate_last_term);
static void clusterRaftStartElection(void);

static void clusterRaftRandomizeElectionTimeout(void) {
    mstime_t base = server.cluster_node_timeout;
    if (base < 1000) base = 1000;
    RAFT_STATE()->election_timeout = base + (rand() % base);
}

static int clusterRaftQuorum(void) {
    return server.cluster->size / 2 + 1;
}

/* Leader step-down to follower. */
static void clusterRaftStepDown(mstime_t now, const char *reason) {
    clusterRaftState *rs = RAFT_STATE();
    clusterRaftDeferPendingProposals();
    rs->role = RAFT_ROLE_FOLLOWER;
    rs->votes_received = 0;
    memset(rs->voted_for, 0, CLUSTER_NAMELEN);
    memset(rs->leader, 0, CLUSTER_NAMELEN);
    clusterRaftRandomizeElectionTimeout();
    rs->last_heartbeat = now;
    rs->todo_save_config = 1;
    serverLog(LL_NOTICE, "Stepping down to follower: %s.", reason);
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
        if (node->flags & CLUSTER_NODE_MEET) continue;
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
    const char *flag = (server.cluster->size > 1) ? "cluster" : "singleton";
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
        sender = createClusterNode(sender_name, CLUSTER_NODE_MEET | CLUSTER_NODE_PRIMARY);
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
    if (rs->role == RAFT_ROLE_FOLLOWER && listLength(rs->pending_proposals) > 0 &&
        link->node && memcmp(link->node->name, rs->leader, CLUSTER_NAMELEN) == 0) {
        rs->todo_retry_proposals = 1;
    }

    /* If we're the leader, send AE immediately to minimize commit latency
     * after a reconnection (especially in 2-node clusters). */
    if (rs->role == RAFT_ROLE_LEADER && link->node &&
        !(link->node->flags & CLUSTER_NODE_MEET)) {
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

    if (server.cluster->size > 1 && !sender_is_singleton) {
        /* Both sides are in a cluster — reject. Merging non-singleton
         * clusters is not supported. */
        serverLog(LL_WARNING, "Rejecting MEET from %.40s: both sides are in a cluster.", sender->name);
        clusterRaftSendBare(link, "MEET_REJECTED");
    } else if (server.cluster->size > 1) {
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
        listAddNodeTail(rs->deferred_meets, link);
        serverLog(LL_NOTICE, "Deferring MEET from %.40s (waiting to join cluster).", sender->name);
    }

    return 1;
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
    if (rs->role == RAFT_ROLE_LEADER && server.cluster->size <= 1) {
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
 *   PROPOSE NODE_JOIN <node-id> <address>
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
        if (server.cluster->size > 1) {
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
    if (server.cluster->size > 1) {
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
    if (index == 0 || index > raftLogLastIndex()) return NULL;
    /* Entries are stored sequentially; first entry's index may not be 1
     * after future log compaction. For now, base is always 1. */
    uint64_t base = rs->log_count > 0 ? rs->log[0]->index : 1;
    if (index < base) return NULL;
    return rs->log[index - base];
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

static uint64_t raftLogLastIndex(void) {
    clusterRaftState *rs = RAFT_STATE();
    return rs->log_count > 0 ? rs->log[rs->log_count - 1]->index : 0;
}

static uint64_t raftLogLastTerm(void) {
    clusterRaftState *rs = RAFT_STATE();
    return rs->log_count > 0 ? rs->log[rs->log_count - 1]->term : 0;
}

static uint64_t raftLogTermAt(uint64_t index) {
    raftLogEntry *e = raftLogGet(index);
    return e ? e->term : 0;
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
        /* data: "<node-id> <address>" */
        int argc;
        sds *argv = sdssplitlen(e->data, sdslen(e->data), " ", 1, &argc);
        if (argv && argc >= 2 && sdslen(argv[0]) == CLUSTER_NAMELEN) {
            clusterNode *existing = clusterLookupNode(argv[0], CLUSTER_NAMELEN);
            if (!existing) {
                clusterNode *n = createClusterNode(argv[0], CLUSTER_NODE_PRIMARY);
                if (clusterNodeParseAddressString(n, argv[1]) == C_OK) {
                    clusterAddNode(n);
                } else {
                    freeClusterNode(n);
                    if (argv) sdsfreesplitres(argv, argc);
                    break;
                }
            } else if (existing != myself) {
                /* Update address for existing node. */
                clusterNodeParseAddressString(existing, argv[1]);
            }
            /* Increment size on first official join. Nodes created from MEET
             * handshake have CLUSTER_NODE_MEET set; clear it now. Nodes created
             * fresh (on followers) don't exist yet. Either way, count once. */
            clusterNode *joined = existing ? existing : clusterLookupNode(argv[0], CLUSTER_NAMELEN);
            if (joined) {
                if (!existing || (joined->flags & CLUSTER_NODE_MEET)) {
                    joined->flags &= ~CLUSTER_NODE_MEET;
                    server.cluster->size++;
                    clusterAddNodeToShard(joined->shard_id, joined);

                    /* Process deferred MEET messages now that we're in a cluster. */
                    if (server.cluster->size > 1 && listLength(rs->deferred_meets) > 0) {
                        listIter dli;
                        listNode *dln;
                        listRewind(rs->deferred_meets, &dli);
                        while ((dln = listNext(&dli)) != NULL) {
                            clusterLink *mlink = listNodeValue(dln);
                            if (mlink->node) {
                                clusterRaftSendBare(mlink, "WELCOME");
                                clusterRaftInvitePeer(mlink->node);
                            }
                        }
                        listEmpty(rs->deferred_meets);
                    }
                }
            }
            rs->todo_invalidate_slots_cache = 1;
            rs->todo_connect_nodes = 1;
            serverLog(LL_NOTICE, "Applied NODE_JOIN for %.40s addr=%s (size=%d).",
                      argv[0], argc >= 2 ? argv[1] : "?", server.cluster->size);

            /* Leader: initialize replication state for the new peer. */
            if (rs->role == RAFT_ROLE_LEADER) {
                clusterNode *joined = clusterLookupNode(argv[0], CLUSTER_NAMELEN);
                if (joined && joined != myself) {
                    RAFT_NODE(joined)->next_index = 1;
                    RAFT_NODE(joined)->match_index = 0;
                    RAFT_NODE(joined)->last_ack_time = monotonicMs();
                }
            }

            /* If this entry is about us, promote from joiner to follower. */
            if (memcmp(argv[0], myself->name, CLUSTER_NAMELEN) == 0) {
                sdsfree(rs->my_last_committed_info);
                rs->my_last_committed_info = clusterRaftBuildMyNodeInfo();

                if (rs->role == RAFT_ROLE_JOINER) {
                    rs->role = RAFT_ROLE_FOLLOWER;
                    rs->todo_save_config = 1;
                    serverLog(LL_NOTICE, "Promoted from joiner to follower.");

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
            rs->todo_invalidate_slots_cache = 1;
        }
        serverLog(LL_NOTICE, "%s SLOT_CHANGE (index %llu).",
                  entry_result != RAFT_RESULT_OK ? "Skipped" : "Applied", (unsigned long long)e->index);
        break;
    }
    case RAFT_ENTRY_SET_REPLICA_OF: {
        entry_result = clusterRaftApplySetReplica(e->data, 0);
        if (entry_result == RAFT_RESULT_OK) {
            rs->todo_invalidate_slots_cache = 1;
        }
        serverLog(LL_NOTICE, "%s SET_REPLICA_OF (index %llu).",
                  entry_result != RAFT_RESULT_OK ? "Skipped" : "Applied", (unsigned long long)e->index);
        break;
    }
    case RAFT_ENTRY_FAILOVER: {
        entry_result = clusterRaftApplyFailover(e->data, 0);
        if (entry_result == RAFT_RESULT_OK) {
            rs->todo_update_slot_coverage = 1;
            rs->todo_invalidate_slots_cache = 1;
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
    case RAFT_ENTRY_NODE_FAIL: {
        clusterNode *node = clusterLookupNode(e->data, sdslen(e->data));
        if (node && node != myself) {
            node->flags |= CLUSTER_NODE_FAIL;
            RAFT_NODE(node)->pending_fail_change = 0;
            rs->todo_update_slot_coverage = 1;
            rs->todo_invalidate_slots_cache = 1;
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
            rs->todo_invalidate_slots_cache = 1;
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
                /* Reset optional fields so absent aux fields get cleared. */
                node->announce_client_tcp_port = 0;
                node->announce_client_tls_port = 0;
                sdsclear(node->hostname);
                sdsclear(node->human_nodename);
                sdsclear(node->announce_client_ipv4);
                sdsclear(node->announce_client_ipv6);
                sdsclear(node->availability_zone);
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

/* --------------------------------------------------------------------------
 * AE (AppendEntries) message
 *
 * Header line: AE <leader-id> <term> <prev-log-idx> <prev-log-term> <commit> <count>
 * Entry lines: <term> <type> <data>
 * -------------------------------------------------------------------------- */

static void clusterRaftSendAppendEntries(clusterLink *link, clusterNode *node) {
    clusterRaftState *rs = RAFT_STATE();
    clusterNodeRaftData *rd = RAFT_NODE(node);

    uint64_t next = rd->next_index;
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
        if (node->flags & CLUSTER_NODE_MEET) continue;
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
    if (rs->role != RAFT_ROLE_JOINER) rs->role = RAFT_ROLE_FOLLOWER;
    rs->last_heartbeat = monotonicMs();
    memcpy(rs->leader, argv[1], CLUSTER_NAMELEN);

    /* Log consistency check: verify prev_log_index/term match. */
    if (prev_log_index > 0 && raftLogTermAt(prev_log_index) != prev_log_term) {
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
            continue;
        }
        uint64_t e_term = strtoull(eargv[0], NULL, 10);
        int e_type_int = raftEntryTypeByName(eargv[1]);
        if (e_type_int < 0) {
            sdsfreesplitres(eargv, eargc);
            continue;
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
        if (!existing) {
            raftLogAppend(raftLogCreate(e_term, new_index, e_type, e_data));
        } else {
            sdsfree(e_data); /* Already have this entry. */
        }
    }

    /* Update commit index and apply. */
    if (leader_commit > rs->commit_index) {
        rs->commit_index = leader_commit;
        if (rs->commit_index > raftLogLastIndex()) rs->commit_index = raftLogLastIndex();
    }
    while (rs->last_applied < rs->commit_index) {
        rs->last_applied++;
        raftLogEntry *e = raftLogGet(rs->last_applied);
        if (e) raftLogApply(e);
    }

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
        !(node->flags & CLUSTER_NODE_MEET)) {
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

            int matches = 1; /* Self */
            dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
            dictEntry *de;
            while ((de = dictNext(di)) != NULL) {
                clusterNode *peer = dictGetVal(de);
                if (peer == myself) continue;
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
        while (rs->last_applied < rs->commit_index) {
            rs->last_applied++;
            raftLogEntry *e = raftLogGet(rs->last_applied);
            if (e) raftLogApply(e);
        }
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

    /* Don't vote while we still consider a leader alive in this term. */
    mstime_t now = monotonicMs();
    if (rs->leader[0] != '\0' &&
        now - rs->last_heartbeat < rs->election_timeout) {
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

    if (msg_term > rs->current_term &&
        clusterRaftCanGrantVote(rs, candidate_last_index, candidate_last_term)) {
        granted = 1;
    }

    clusterRaftSendPreVoteResponse(link, granted ? msg_term : rs->current_term, granted);
    return 1;
}

static int clusterRaftProcessPreVoteResponse(clusterLink *link, int argc, sds *argv) {
    UNUSED(link);
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

    if (granted) {
        rs->pre_votes_received++;
        int quorum = server.cluster->size / 2 + 1;
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
    } else if ((clusterRaftIsVotedForNone() || memcmp(rs->voted_for, argv[1], CLUSTER_NAMELEN) == 0) &&
               clusterRaftCanGrantVote(rs, candidate_last_index, candidate_last_term)) {
        granted = 1;
        memcpy(rs->voted_for, argv[1], CLUSTER_NAMELEN);
        rs->last_heartbeat = monotonicMs(); /* Reset election timer */
        rs->todo_save_config = 1;
        serverLog(LL_NOTICE, "Voted for %.40s in term %llu.", argv[1], (unsigned long long)msg_term);
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

static int clusterRaftProcessRequestVoteResponse(clusterLink *link, int argc, sds *argv) {
    UNUSED(link);
    /* argv: VOTE <term> <granted> */
    if (argc < 3) return 1;

    clusterRaftState *rs = RAFT_STATE();
    uint64_t msg_term = strtoull(argv[1], NULL, 10);
    int granted = atoi(argv[2]);

    clusterRaftMaybeStepDown(msg_term);

    if (rs->role != RAFT_ROLE_CANDIDATE) return 1;
    if (msg_term != rs->current_term) return 1;

    if (granted) {
        rs->votes_received++;
        int quorum = clusterRaftQuorum();
        if (rs->votes_received >= quorum) {
            char old_leader[CLUSTER_NAMELEN];
            memcpy(old_leader, rs->leader, CLUSTER_NAMELEN);
            rs->role = RAFT_ROLE_LEADER;
            memcpy(rs->leader, myself->name, CLUSTER_NAMELEN);
            serverLog(LL_NOTICE, "Elected as Raft leader (term %llu, %d votes).",
                      (unsigned long long)rs->current_term, rs->votes_received);
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
            /* Immediate heartbeat to assert leadership. */
            clusterRaftBroadcastAppendEntries();
        }
    }
    return 1;
}

static void clusterRaftStartPreVote(void) {
    clusterRaftState *rs = RAFT_STATE();
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
        if (node->flags & CLUSTER_NODE_MEET) continue;
        clusterRaftSendPreVoteRequest(node->link, candidate_term);
    }
    dictReleaseIterator(di);

    int quorum = server.cluster->size / 2 + 1;
    if (rs->pre_votes_received >= quorum) {
        clusterRaftStartElection();
    }
}

static void clusterRaftStartElection(void) {
    clusterRaftState *rs = RAFT_STATE();
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
        rs->role = RAFT_ROLE_LEADER;
        memcpy(rs->leader, myself->name, CLUSTER_NAMELEN);
        serverLog(LL_NOTICE, "Elected as Raft leader (term %llu, %d votes).",
                  (unsigned long long)rs->current_term, rs->votes_received);
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
    rs->my_last_committed_info = sdsempty();
    rs->last_node_info_check = monotonicMs();
    rs->last_repl_offsets_broadcast = monotonicMs();
    rs->todo_update_slot_coverage = 1;
    rs->shard_epochs = dictCreate(&raftShardEpochDictType);
    server.cluster->size = 0; /* Incremented by NODE_JOIN apply */
}

static void clusterRaftInitLast(void) {
    clusterListenerInit();

    /* On fresh start (size == 0), mark myself as not yet in the raft log.
     * Cleared when our own NODE_JOIN is applied. On restart, size is
     * restored by postLoad so we skip this. */
    if (server.cluster->size == 0) {
        myself->flags |= CLUSTER_NODE_MEET;
    }

    /* Fresh single-node cluster: become leader immediately. */
    clusterRaftState *rs = RAFT_STATE();
    if (dictSize(server.cluster->nodes) == 1 && server.cluster->size == 0) {
        rs->role = RAFT_ROLE_LEADER;
        rs->current_term = 1;
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

    /* For followers, check leader link once — it's the same for all entries. */
    clusterLink *leader_link = NULL;
    if (rs->role == RAFT_ROLE_FOLLOWER) {
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

        /* For deferred entries, update to latest shard epoch */
        if (pp->deferred) {
            if (!raftRefreshEpochInData(pp->type, &pp->data)) continue;

            pp->deferred = 0;

            if (rs->role == RAFT_ROLE_LEADER) {
                RaftProposalResult pre_result = clusterRaftPreValidate(pp->type, pp->data);
                if (pre_result != RAFT_RESULT_OK) {
                    if (pp->callback) pp->callback(pp->ctx, raftProposalResultMsg(pre_result));
                    sdsfree(pp->data);
                    zfree(pp);
                    listDelNode(rs->pending_proposals, ln);
                    continue;
                }
            }
        } else {
            if (!include_pending) continue;
        }

        clusterRaftSendProposal(pp, leader_link);
    }
}

static void clusterRaftCron(void) {
    clusterRaftState *rs = RAFT_STATE();
    mstime_t now = monotonicMs();

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
        if ((rs->role == RAFT_ROLE_FOLLOWER || rs->role == RAFT_ROLE_CANDIDATE) &&
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
         * during prolonged partitions. Timeout covers election detection
         * (1-2x node_timeout), election itself (~1x), plus margin. */
        if (listLength(rs->pending_proposals) > 0) {
            mstime_t timeout = server.cluster_node_timeout * 3;
            listIter ei;
            listNode *eln;
            listRewind(rs->pending_proposals, &ei);
            while ((eln = listNext(&ei)) != NULL) {
                raftPendingProposal *pp = listNodeValue(eln);
                if (now - pp->ctime > timeout) {
                    serverLog(LL_NOTICE, "Expiring stale proposal %s after %lldms.",
                              raftEntryTypeName(pp->type), (long long)(now - pp->ctime));
                    if (pp->callback) pp->callback(pp->ctx, "-TIMEOUT Cluster consensus not reached in time");
                    sdsfree(pp->data);
                    zfree(pp);
                    listDelNode(rs->pending_proposals, eln);
                }
            }
        }

        /* Retry proposals when leader link becomes available. */
        clusterRaftRetryProposals(1);

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
        } else if (nodeIsReplica(myself) && myself->replicaof &&
                   replicationGetReplicaOffset() == server.primary_repl_offset &&
                   server.primary_repl_offset > 0) {
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
        rs->todo_persist_log = 0;
        clusterRaftPersistNewLogEntries(rs->persist_log_from);
    }

    /* Singleton leader: commit after persistence (quorum = 1). */
    if (rs->role == RAFT_ROLE_LEADER && server.cluster->size <= 1 &&
        raftLogLastIndex() > rs->commit_index) {
        rs->commit_index = raftLogLastIndex();
        while (rs->last_applied < rs->commit_index) {
            rs->last_applied++;
            raftLogEntry *e = raftLogGet(rs->last_applied);
            if (e) raftLogApply(e);
        }
        /* If we just grew beyond singleton, replicate to the new peer. */
        if (server.cluster->size > 1) rs->todo_broadcast_ae = 1;
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
            if (node->flags & CLUSTER_NODE_MEET) continue;
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

    if (rs->todo_invalidate_slots_cache) {
        rs->todo_invalidate_slots_cache = 0;
        clearCachedClusterSlotsResponse();
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
                if (sibling == myself) continue;
                if (sibling->repl_offset > my_offset) rank++;
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
        if (node->flags & (CLUSTER_NODE_MEET | CLUSTER_NODE_HANDSHAKE)) continue;
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
        if (term >= rs->current_term && rs->role == RAFT_ROLE_FOLLOWER) {
            serverLog(LL_NOTICE, "Received TIMEOUT_NOW (term %llu), starting election.",
                      (unsigned long long)term);
            clusterRaftStartElection();
        }
    } else if (!strcasecmp(argv[0], "FAILOVER_PREPARE")) {
        /* Primary side: pause writes for coordinated failover. */
        if (link->node && nodeIsReplica(link->node) && link->node->replicaof == myself) {
            clusterRaftState *rs = RAFT_STATE();
            rs->mf_end = monotonicMs() + server.cluster_mf_timeout;
            pauseActions(PAUSE_DURING_FAILOVER,
                         monotonicMs() + (server.cluster_mf_timeout * CLUSTER_MF_PAUSE_MULT),
                         PAUSE_ACTIONS_CLIENT_WRITE_SET);
            serverLog(LL_NOTICE, "Manual failover requested by replica %.40s, pausing writes.",
                      link->node->name);
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
        if (nl && (size_t)(buf + buf_len - nl - 1) >= chan_len + msg_len) {
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
    if ((myself->flags & CLUSTER_NODE_MEET) || myself->ip[0] == '\0') return;
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
    config = sdscatprintf(config, "vars currentTerm %llu lastApplied %llu",
                          (unsigned long long)rs->current_term,
                          (unsigned long long)rs->last_applied);
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
 * from beforeSleep to batch multiple entries into a single write+fsync. */
static void clusterRaftPersistNewLogEntries(uint64_t from) {
    clusterRaftState *rs = RAFT_STATE();
    sds buf = sdsempty();
    for (uint64_t i = 0; i < rs->log_count; i++) {
        raftLogEntry *e = rs->log[i];
        if (e->index < from) continue;
        buf = raftLogFormatLine(buf, e);
    }
    if (sdslen(buf) == 0) {
        sdsfree(buf);
        return;
    }
    int fd = open(server.cluster_configfile, O_WRONLY | O_APPEND);
    if (fd == -1) {
        serverLog(LL_WARNING, "Could not open cluster config for log append: %s", strerror(errno));
        sdsfree(buf);
        return;
    }
    if (write(fd, buf, sdslen(buf)) == -1) {
        serverLog(LL_WARNING, "Could not append log entries to cluster config: %s", strerror(errno));
    }
    valkey_fsync(fd);
    close(fd);
    sdsfree(buf);
}


static int clusterRaftParseLogLine(sds *argv, int argc) {
    /* Format: log <crc64hex> <index> <term> <type> <data...> */
    if (argc < 6) {
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
    int type = raftEntryTypeByName(argv[4]);
    if (type < 0) {
        serverLog(LL_WARNING, "Corrupt raft log line at index %llu: unknown type '%s'.",
                  (unsigned long long)index, argv[4]);
        return C_ERR;
    }

    /* Reconstruct data from remaining args (space-separated). */
    sds data = sdsdup(argv[5]);
    for (int i = 6; i < argc; i++) {
        data = sdscatlen(data, " ", 1);
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
    /* Schedule a full rewrite to produce a clean file (removes any
     * incomplete trailing line from a short write). */
    rs->todo_save_config = 1;
    /* Restore cluster size from loaded nodes (NODE_JOIN already applied). */
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (!(node->flags & CLUSTER_NODE_MEET)) server.cluster->size++;
    }
    dictReleaseIterator(di);
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
                     "cluster_size:%i\r\n",
                     statestr, slots_assigned, slots_ok,
                     (unsigned long long)dictSize(server.cluster->nodes),
                     server.cluster->size);

    /* Raft-specific fields. */
    const char *role_str = rs->role == RAFT_ROLE_LEADER          ? "leader"
                           : rs->role == RAFT_ROLE_CANDIDATE     ? "candidate"
                           : rs->role == RAFT_ROLE_PRE_CANDIDATE ? "pre-candidate"
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
                        "total_cluster_links_buffer_limit_exceeded:%llu\r\n",
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
                        (unsigned long long)server.cluster->stat_cluster_links_buffer_limit_exceeded);
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

/* Invoked for slot assignments and slot migrations (ADDSLOTS, SETSLOT, etc.) */
static void clusterRaftSlotChange(slotRange *ranges, int numranges, clusterNode *target, void *ctx, void (*callback)(void *ctx, const char *error)) {
    clusterRaftState *rs = RAFT_STATE();

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
        rs->todo_invalidate_slots_cache = 1;
        if (callback) callback(ctx, NULL);
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
            if (info.target_node == myself || server.cluster->slots[j] == myself)
                RAFT_STATE()->todo_save_config = 1;
            if (info.target_node) {
                if (server.cluster->slots[j] == myself && info.target_node != myself) {
                    serverLog(LL_NOTICE, "Deleting keys in dirty slot %d on node %.40s",
                              j, myself->name);
                    delKeysInSlot(j, server.lazyfree_lazy_server_del, true, false);
                }
                if (server.cluster->slots[j]) clusterDelSlot(j);
                clusterAddSlot(info.target_node, j);
            } else {
                if (server.cluster->slots[j] == myself) {
                    delKeysInSlot(j, server.lazyfree_lazy_server_del, true, false);
                }
                clusterDelSlot(j);
            }
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
    if (!node || node == myself) goto reject;

    uint64_t epoch = strtoull(argv[1], NULL, 10);
    if (!clusterValidateShardEpoch(node->shard_id, epoch)) {
        result = RAFT_RESULT_STALE_EPOCH;
        goto reject;
    }

    if (nodeIsPrimary(node) && (node->num_replicas > 0 || node->numslots > 0)) {
        serverLog(LL_WARNING, "NODE_FORGET rejected: can't forget a primary with replicas or assigned slots.");
        goto reject;
    }

    if (validate_only) goto done;

    /* Save shard_id before deleting the node. */
    char shard_id[CLUSTER_NAMELEN];
    memcpy(shard_id, node->shard_id, CLUSTER_NAMELEN);

    /* Detach link before deleting so clusterReadHandler can detect
     * that the node it was talking to is gone. */
    if (node->link) {
        node->link->node = NULL;
        node->link = NULL;
    }
    clusterDelNode(node);
    rs->todo_update_slot_coverage = 1;
    rs->todo_invalidate_slots_cache = 1;

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
        rs->mf_end = monotonicMs() + server.cluster_mf_timeout;
        rs->mf_ctx = ctx;
        rs->mf_callback = callback;

        /* Send FAILOVER_PREPARE to the primary. */
        if (!primary->link) {
            if (callback) callback(ctx, "no link to primary");
            return;
        }
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
        clusterNode *n = createClusterNode(NULL, CLUSTER_NODE_MEET | CLUSTER_NODE_HANDSHAKE);
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
    rs->current_term = 0;
    memset(rs->leader, 0, CLUSTER_NAMELEN);
    memset(rs->voted_for, 0, CLUSTER_NAMELEN);
    rs->commit_index = 0;
    rs->last_applied = 0;
    rs->log_count = 0;
    rs->role = RAFT_ROLE_LEADER;
    rs->failover_time = 0;
    rs->mf_end = 0;
    server.cluster->size = 0;
    myself->flags |= CLUSTER_NODE_MEET;

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
