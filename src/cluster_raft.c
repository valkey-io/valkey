/* cluster_raft.c — Raft-based cluster bus implementation.
 *
 * All cluster metadata changes (node membership, slot ownership, replication
 * topology) are replicated through a Raft consensus log. The Raft leader is
 * independent of the data primary/replica role.
 *
 * Wire protocol:
 *   Header: "RAFT" (4 bytes) + totlen (uint32 big-endian) = 8 bytes.
 *   Payload: space-separated text fields. First field is the message type.
 *
 * Message types:
 *   HELLO <node-id> <address-string>
 *   VOTE <candidate-id> <term> <last-log-index> <last-log-term>
 *   VOTE_OK <term> <granted>
 *   AE <leader-id> <term> <prev-log-index> <prev-log-term> <leader-commit>
 *   AE_ACK <term> <success>
 */

#include "server.h"
#include "cluster.h"
#include "cluster_bus.h"
#include "cluster_state.h"
#include "cluster_link.h"
#include "cluster_nodes.h"

#include <arpa/inet.h>

/* --------------------------------------------------------------------------
 * Wire format helpers
 * -------------------------------------------------------------------------- */

#define RAFT_HDR_SIZE 8

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
    size_t len = sdslen(msg);
    clusterMsgSendBlock *block = clusterAllocMsgSendBlock(len);
    memcpy(block->data, msg, len);
    sdsfree(msg);
    clusterLinkSendBlock(link, block);
    clusterMsgSendBlockDecrRefCount(block);
}

/* --------------------------------------------------------------------------
 * Raft log entry types — what gets replicated
 * -------------------------------------------------------------------------- */

enum raftEntryType {
    RAFT_ENTRY_NODE_JOIN = 1,      /* MEET */
    RAFT_ENTRY_NODE_LEAVE = 2,     /* FORGET */
    RAFT_ENTRY_SLOT_CHANGE = 3,    /* Slot ownership */
    RAFT_ENTRY_SET_REPLICA_OF = 4, /* Replication topology */
    RAFT_ENTRY_FAILOVER = 5,       /* Manual failover */
    RAFT_ENTRY_NODE_META = 6,      /* IP, port, hostname, etc. */
    RAFT_ENTRY_NODE_FAIL = 7,      /* Node failure detected by leader */
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
    RAFT_ROLE_LEARNER = 0, /* Receives log but doesn't vote or count for quorum */
    RAFT_ROLE_FOLLOWER = 1,
    RAFT_ROLE_CANDIDATE = 2,
    RAFT_ROLE_LEADER = 3,
};

/* --------------------------------------------------------------------------
 * Per-peer replication state (leader only)
 * -------------------------------------------------------------------------- */

typedef struct {
    uint64_t next_index;
    uint64_t match_index;
    mstime_t last_ack_time; /* Last time we received AE_ACK from this peer */
} raftPeerState;

/* A pending proposal tracks a client waiting for a Raft entry to be
 * committed. Stored on the node that originated the proposal. */
typedef struct {
    uint8_t type; /* Expected entry type */
    sds data;     /* Expected entry data (for matching) */
    void *ctx;    /* Client context */
    void (*callback)(void *ctx, const char *error);
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

    /* Leader state: per-peer tracking, keyed by node name */
    dict *peer_state; /* node name -> raftPeerState */

    /* Pending proposals waiting for commit. */
    list *pending_proposals; /* list of raftPendingProposal */

    /* Pending MEET callbacks waiting for NODE_JOIN commit. */
    list *pending_meets; /* list of raftPendingMeet */

    /* Deferred work for beforeSleep. */
    unsigned int todo_update_slot_coverage : 1;
    unsigned int todo_invalidate_slots_cache : 1;
    unsigned int todo_connect_nodes : 1;
    unsigned int todo_broadcast_ae : 1;

    /* Election */
    int votes_received;
    mstime_t election_timeout; /* Randomized timeout */
    mstime_t last_heartbeat;   /* Last time we heard from leader */

    /* Leader identity */
    char leader[CLUSTER_NAMELEN]; /* All zeros = unknown */
} clusterRaftState;

#define RAFT_STATE() ((clusterRaftState *)server.cluster->protocol_data)

/* --------------------------------------------------------------------------
 * Per-node protocol data (stored in clusterNode.protocol_data)
 * -------------------------------------------------------------------------- */

typedef struct {
    raftPeerState peer; /* Replication tracking when we are leader */
} clusterNodeRaftData;

#define RAFT_DATA(n) ((clusterNodeRaftData *)(n)->protocol_data)

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static void clusterRaftPropose(sds entry, void *ctx, void (*callback)(void *ctx, const char *error));
static void clusterRaftFlushPendingProposals(clusterRaftState *rs, const char *error);
static void clusterRaftCheckSlotCoverage(void);
static void clusterRaftBroadcastAppendEntries(clusterRaftState *rs);
static void clusterRaftSendAppendEntries(clusterLink *link, clusterNode *node);

/* Context for NODE_JOIN callback that sends WELCOME and fires MEET callback. */
typedef struct {
    sds node_name;
    void *meet_ctx;
    void (*meet_callback)(void *ctx, const char *error);
} raftJoinCallbackCtx;

static void clusterRaftJoinCallback(void *ctx, const char *error);
static raftPendingMeet *clusterRaftConsumePendingMeet(clusterNode *node);
static void clusterRaftApplySlotChange(sds data);
static void clusterRaftApplySetReplica(sds data);
static void raftLogApply(raftLogEntry *e);
static raftLogEntry *raftLogCreate(uint64_t term, uint64_t index, uint8_t type, sds data);
static void raftLogAppend(clusterRaftState *rs, raftLogEntry *e);
static raftLogEntry *raftLogGet(clusterRaftState *rs, uint64_t index);
static uint64_t raftLogLastIndex(clusterRaftState *rs);
static uint64_t raftLogTermAt(clusterRaftState *rs, uint64_t index);

static void clusterRaftRandomizeElectionTimeout(clusterRaftState *rs) {
    mstime_t base = server.cluster_node_timeout;
    if (base < 1000) base = 1000;
    rs->election_timeout = base + (rand() % base);
}

/* --------------------------------------------------------------------------
 * HELLO message: HELLO <node-id> <address-string>
 *
 * The address string uses the nodes.conf format:
 *   ip:port@cport[,hostname][,aux=val]*
 * -------------------------------------------------------------------------- */

/* HELLO format: HELLO <node-id> <address> <term> <role> <cluster-size> */

static void clusterRaftSendGreeting(clusterLink *link, const char *verb) {
    clusterRaftState *rs = RAFT_STATE();
    sds msg = wireNewMsg(verb);
    msg = sdscatlen(msg, " ", 1);
    msg = sdscatlen(msg, myself->name, CLUSTER_NAMELEN);
    msg = sdscatlen(msg, " ", 1);
    msg = clusterNodeAppendAddressString(msg, myself, server.tls_cluster);
    msg = sdscatfmt(msg, " %U %i %U", (unsigned long long)rs->current_term, rs->role,
                    (unsigned long long)server.cluster->size);
    msg = wireFinishMsg(msg);
    serverLog(LL_DEBUG, "Sending %s to %.40s on %s link.", verb,
              link->node ? link->node->name : "?",
              link->inbound ? "inbound" : "outbound");
    clusterRaftSendMsg(link, msg);
}

static void clusterRaftSendHello(clusterLink *link) {
    clusterRaftSendGreeting(link, "HELLO");
}

/* Parse sender identity from a greeting message (HELLO/HI/WELCOME).
 * Returns the sender name from argv[1], or NULL on parse error. */
static char *clusterRaftGreetingSender(int argc, sds *argv) {
    if (argc < 6) return NULL;
    if (sdslen(argv[1]) != CLUSTER_NAMELEN) return NULL;
    return argv[1];
}

/* HELLO: received on an inbound link. The sender identifies itself.
 * Step-down rule 1: singleton leader always steps down on receiving HELLO.
 * If we're still leader after step-down and sender is unknown, propose
 * NODE_JOIN. Always reply HI on the same inbound link. */
static int clusterRaftProcessHello(clusterLink *link, int argc, sds *argv) {
    char *sender_name = clusterRaftGreetingSender(argc, argv);
    if (!sender_name) return 1;
    serverLog(LL_DEBUG, "Received HELLO from %.40s on %s link.",
              sender_name, link->inbound ? "inbound" : "outbound");

    clusterRaftState *rs = RAFT_STATE();
    uint64_t sender_cluster_size = strtoull(argv[5], NULL, 10);

    /* Look up or create the sender node. */
    clusterNode *sender = clusterLookupNode(sender_name, CLUSTER_NAMELEN);

    /* Rule 1: singleton leader steps down on HELLO from an unknown node.
     * This means we're the MEET target — the sender will stay leader.
     * If the sender is already known, this is just a reconnect HELLO. */
    if (rs->role == RAFT_ROLE_LEADER && server.cluster->size <= 1 && !sender) {
        clusterRaftFlushPendingProposals(rs, "leader changed");
        rs->role = RAFT_ROLE_LEARNER;
        memcpy(rs->leader, sender_name, CLUSTER_NAMELEN);
        memset(rs->voted_for, 0, CLUSTER_NAMELEN);
        clusterRaftRandomizeElectionTimeout(rs);
        rs->last_heartbeat = mstime();
        rs->todo_connect_nodes = 1;
        serverLog(LL_NOTICE, "Singleton stepping down on HELLO from %.40s.", sender_name);
    }
    if (!sender) {
        sender = createClusterNode(sender_name, 0);
        if (clusterNodeParseAddressString(sender, argv[2]) == C_ERR) {
            serverLog(LL_WARNING, "Bad address in HELLO from %.40s", sender_name);
            freeClusterNode(sender);
            return 1;
        }
        clusterAddNode(sender);
        if (sender->ip[0] == '\0') {
            connAddrPeerName(link->conn, sender->ip, sizeof(sender->ip), NULL);
        }
        serverLog(LL_NOTICE, "New node %.40s (%s:%d) discovered via HELLO.",
                  sender->name, sender->ip, (int)sender->cport);
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

    /* Reply HI on the same inbound link. */
    clusterRaftSendGreeting(link, "HI");

    /* Propose NODE_JOIN if sender is new to the cluster. */
    if (server.cluster->size > 1 && !nodeInHandshake(sender)) {
        int already_in_log = 0;
        for (uint64_t i = 0; i < rs->log_count; i++) {
            if (rs->log[i]->type == RAFT_ENTRY_NODE_JOIN &&
                sdslen(rs->log[i]->data) >= CLUSTER_NAMELEN &&
                memcmp(rs->log[i]->data, sender->name, CLUSTER_NAMELEN) == 0) {
                already_in_log = 1;
                break;
            }
        }
        if (!already_in_log) {
            sds entry = sdsnew("NODE_JOIN ");
            entry = sdscatlen(entry, sender->name, CLUSTER_NAMELEN);
            entry = sdscatlen(entry, " ", 1);
            entry = clusterNodeAppendAddressString(entry, sender, server.tls_cluster);

            if (rs->role == RAFT_ROLE_LEADER) {
                raftJoinCallbackCtx *jc = zmalloc(sizeof(*jc));
                jc->node_name = sdsnewlen(sender->name, CLUSTER_NAMELEN);
                raftPendingMeet *pm = clusterRaftConsumePendingMeet(sender);
                if (pm) {
                    jc->meet_ctx = pm->ctx;
                    jc->meet_callback = pm->callback;
                    sdsfree(pm->addr);
                    zfree(pm);
                } else {
                    jc->meet_ctx = NULL;
                    jc->meet_callback = NULL;
                }
                clusterRaftPropose(entry, jc, clusterRaftJoinCallback);
            } else {
                clusterRaftPropose(entry, NULL, NULL);
            }
            sdsfree(entry);
        }
    }

    return 1;
}

/* HI: received on an outbound link as reply to our HELLO.
 * Completes the handshake (renames handshake node to real ID).
 * Step-down rule 2: singleton leader steps down on HI from non-singleton.
 * If we're leader, propose NODE_JOIN for the sender. */
static int clusterRaftProcessHi(clusterLink *link, int argc, sds *argv) {
    char *sender_name = clusterRaftGreetingSender(argc, argv);
    if (!sender_name) return 1;
    serverLog(LL_DEBUG, "Received HI from %.40s on %s link.",
              sender_name, link->inbound ? "inbound" : "outbound");

    clusterRaftState *rs = RAFT_STATE();
    uint64_t sender_cluster_size = strtoull(argv[5], NULL, 10);

    /* Rule 2: singleton leader steps down on HI from non-singleton. */
    if (rs->role == RAFT_ROLE_LEADER && server.cluster->size <= 1 && sender_cluster_size > 1) {
        clusterRaftFlushPendingProposals(rs, "leader changed");
        rs->role = RAFT_ROLE_LEARNER;
        memcpy(rs->leader, sender_name, CLUSTER_NAMELEN);
        memset(rs->voted_for, 0, CLUSTER_NAMELEN);
        clusterRaftRandomizeElectionTimeout(rs);
        rs->last_heartbeat = mstime();
        serverLog(LL_NOTICE, "Singleton stepping down on HI from non-singleton %.40s.", sender_name);
    }

    /* Complete handshake: rename handshake node or transfer link. */
    clusterNode *sender = clusterLookupNode(sender_name, CLUSTER_NAMELEN);
    if (link->node && nodeInHandshake(link->node)) {
        if (!sender) {
            /* Normal: rename handshake node to real ID. */
            clusterRenameNode(link->node, sender_name);
            link->node->flags &= ~(CLUSTER_NODE_HANDSHAKE | CLUSTER_NODE_MEET);
            if (clusterNodeParseAddressString(link->node, argv[2]) == C_ERR) {
                serverLog(LL_WARNING, "Bad address in HI from %.40s", sender_name);
                return 1;
            }
            sender = link->node;
        } else {
            /* Real node already exists (from NODE_JOIN apply).
             * Delete handshake node and transfer the link. */
            clusterNode *handshake = link->node;
            link->node = sender;
            sender->link = link;
            handshake->link = NULL;
            clusterDelNode(handshake);
        }
        serverLog(LL_NOTICE, "Handshake with node %.40s completed.", sender->name);
    }

    if (!sender) return 1;

    /* If we're leader, propose NODE_JOIN for the sender. */
    if (rs->role == RAFT_ROLE_LEADER && !nodeInHandshake(sender)) {
        int already_in_log = 0;
        for (uint64_t i = 0; i < rs->log_count; i++) {
            if (rs->log[i]->type == RAFT_ENTRY_NODE_JOIN &&
                sdslen(rs->log[i]->data) >= CLUSTER_NAMELEN &&
                memcmp(rs->log[i]->data, sender->name, CLUSTER_NAMELEN) == 0) {
                already_in_log = 1;
                break;
            }
        }
        if (!already_in_log) {
            sds entry = sdsnew("NODE_JOIN ");
            entry = sdscatlen(entry, sender->name, CLUSTER_NAMELEN);
            entry = sdscatlen(entry, " ", 1);
            entry = clusterNodeAppendAddressString(entry, sender, server.tls_cluster);

            raftJoinCallbackCtx *jc = zmalloc(sizeof(*jc));
            jc->node_name = sdsnewlen(sender->name, CLUSTER_NAMELEN);
            raftPendingMeet *pm = clusterRaftConsumePendingMeet(sender);
            if (pm) {
                jc->meet_ctx = pm->ctx;
                jc->meet_callback = pm->callback;
                sdsfree(pm->addr);
                zfree(pm);
            } else {
                jc->meet_ctx = NULL;
                jc->meet_callback = NULL;
            }
            clusterRaftPropose(entry, jc, clusterRaftJoinCallback);
            sdsfree(entry);
        }
    }

    /* Send AE immediately to catch up the peer. */
    if (rs->role == RAFT_ROLE_LEADER && sender->link) {
        rs->todo_broadcast_ae = 1;
    }

    return 1;
}

/* WELCOME: received on an outbound link after NODE_JOIN committed.
 * Fires the pending MEET callback. */
static int clusterRaftProcessWelcome(clusterLink *link, int argc, sds *argv) {
    UNUSED(link);
    char *sender_name = clusterRaftGreetingSender(argc, argv);
    if (!sender_name) return 1;
    serverLog(LL_DEBUG, "Received WELCOME from %.40s.", sender_name);

    clusterNode *sender = clusterLookupNode(sender_name, CLUSTER_NAMELEN);
    if (!sender) return 1;

    raftPendingMeet *pm = clusterRaftConsumePendingMeet(sender);
    if (pm) {
        pm->callback(pm->ctx, NULL);
        sdsfree(pm->addr);
        zfree(pm);
    }

    return 1;
}

/* --------------------------------------------------------------------------
 * PROPOSE message: PROPOSE <entry-type-name> <data...>
 *
 * Sent by a follower/learner to the leader to propose a log entry.
 * The leader appends it to the Raft log and replicates via AE.
 * The payload after PROPOSE is the same as an AE entry line without
 * the term prefix: "<type-name> <data...>"
 *
 * Examples:
 *   PROPOSE NODE_JOIN <node-id> <address>
 *   PROPOSE SLOT_CHANGE <node-id-or-dash> <range> [<range> ...]
 * -------------------------------------------------------------------------- */

static int raftEntryTypeByName(const char *name) {
    if (!strcasecmp(name, "NODE_JOIN")) return RAFT_ENTRY_NODE_JOIN;
    if (!strcasecmp(name, "NODE_LEAVE")) return RAFT_ENTRY_NODE_LEAVE;
    if (!strcasecmp(name, "SLOT_CHANGE")) return RAFT_ENTRY_SLOT_CHANGE;
    if (!strcasecmp(name, "SET_REPLICA_OF")) return RAFT_ENTRY_SET_REPLICA_OF;
    if (!strcasecmp(name, "FAILOVER")) return RAFT_ENTRY_FAILOVER;
    if (!strcasecmp(name, "NODE_META")) return RAFT_ENTRY_NODE_META;
    if (!strcasecmp(name, "NODE_FAIL")) return RAFT_ENTRY_NODE_FAIL;
    return -1;
}

static const char *raftEntryTypeName(uint8_t type) {
    switch (type) {
    case RAFT_ENTRY_NODE_JOIN: return "NODE_JOIN";
    case RAFT_ENTRY_NODE_LEAVE: return "NODE_LEAVE";
    case RAFT_ENTRY_SLOT_CHANGE: return "SLOT_CHANGE";
    case RAFT_ENTRY_SET_REPLICA_OF: return "SET_REPLICA_OF";
    case RAFT_ENTRY_FAILOVER: return "FAILOVER";
    case RAFT_ENTRY_NODE_META: return "NODE_META";
    case RAFT_ENTRY_NODE_FAIL: return "NODE_FAIL";
    default: return "UNKNOWN";
    }
}

/* Propose a log entry. The entry is an sds containing the type name
 * followed by the data, e.g. "NODE_JOIN <id> <addr>" or
 * "SLOT_CHANGE <id> 0-5460". On the leader, it's appended directly
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

    /* Track pending callback for matching on commit. */
    if (callback) {
        raftPendingProposal *pp = zmalloc(sizeof(*pp));
        pp->type = type;
        pp->data = sdsdup(data);
        pp->ctx = ctx;
        pp->callback = callback;
        listAddNodeTail(rs->pending_proposals, pp);
    }

    if (rs->role == RAFT_ROLE_LEADER) {
        uint64_t idx = raftLogLastIndex(rs) + 1;
        raftLogAppend(rs, raftLogCreate(rs->current_term, idx, type, data));
        serverLog(LL_NOTICE, "Leader appended %s (index %llu).",
                  raftEntryTypeName(type), (unsigned long long)idx);

        /* Single-node cluster: commit and apply immediately. */
        if (server.cluster->size <= 1) {
            rs->commit_index = idx;
            while (rs->last_applied < rs->commit_index) {
                rs->last_applied++;
                raftLogEntry *e = raftLogGet(rs, rs->last_applied);
                if (e) raftLogApply(e);
            }
        } else {
            /* Replicate to followers immediately. */
            rs->todo_broadcast_ae = 1;
        }
    } else {
        sdsfree(data);
        clusterNode *leader = clusterLookupNode(rs->leader, CLUSTER_NAMELEN);
        clusterLink *leader_link = leader ? (leader->link ? leader->link : leader->inbound_link) : NULL;
        if (!leader_link) {
            /* Can't reach leader — flush the proposal we just added. */
            if (callback) {
                listNode *ln = listLast(rs->pending_proposals);
                raftPendingProposal *pp = listNodeValue(ln);
                sdsfree(pp->data);
                zfree(pp);
                listDelNode(rs->pending_proposals, ln);
                callback(ctx, "no leader");
            }
            return;
        }
        sds msg = wireNewMsg("PROPOSE");
        msg = sdscatlen(msg, " ", 1);
        msg = sdscatlen(msg, entry, sdslen(entry));
        msg = wireFinishMsg(msg);
        clusterRaftSendMsg(leader_link, msg);
    }
}

static int clusterRaftProcessPropose(clusterLink *link, int argc, sds *argv) {
    UNUSED(link);
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

    uint64_t idx = raftLogLastIndex(rs) + 1;
    raftLogAppend(rs, raftLogCreate(rs->current_term, idx, type, data));
    serverLog(LL_NOTICE, "Leader appended proposed %s (index %llu).",
              raftEntryTypeName(type), (unsigned long long)idx);

    /* Single-node cluster: commit and apply immediately. */
    if (server.cluster->size <= 1) {
        rs->commit_index = idx;
        while (rs->last_applied < rs->commit_index) {
            rs->last_applied++;
            raftLogEntry *e = raftLogGet(rs, rs->last_applied);
            if (e) raftLogApply(e);
        }
    } else {
        /* Replicate to followers immediately. */
        rs->todo_broadcast_ae = 1;
    }

    sdsfree(entry);
    return 1;
}

/* --------------------------------------------------------------------------
 * Raft election and heartbeat
 * -------------------------------------------------------------------------- */

/* Flush all pending proposals with an error. Called on leader change. */
static void clusterRaftFlushPendingProposals(clusterRaftState *rs, const char *error) {
    listIter li;
    listNode *ln;
    listRewind(rs->pending_proposals, &li);
    while ((ln = listNext(&li)) != NULL) {
        raftPendingProposal *pp = listNodeValue(ln);
        pp->callback(pp->ctx, error);
        sdsfree(pp->data);
        zfree(pp);
        listDelNode(rs->pending_proposals, ln);
    }
    /* Note: pending_meets are NOT flushed here. A MEET remains valid
     * across leader changes — the WELCOME will arrive eventually. */
}

/* Step down to follower if we see a higher term. Returns 1 if stepped down. */
static int clusterRaftMaybeStepDown(clusterRaftState *rs, uint64_t term) {
    if (term > rs->current_term) {
        clusterRaftFlushPendingProposals(rs, "leader changed");
        rs->current_term = term;
        rs->role = RAFT_ROLE_FOLLOWER;
        memset(rs->voted_for, 0, CLUSTER_NAMELEN);
        memset(rs->leader, 0, CLUSTER_NAMELEN);
        clusterRaftRandomizeElectionTimeout(rs);
        rs->last_heartbeat = mstime();
        return 1;
    }
    return 0;
}

static int clusterRaftIsVotedForNone(clusterRaftState *rs) {
    char zero[CLUSTER_NAMELEN] = {0};
    return memcmp(rs->voted_for, zero, CLUSTER_NAMELEN) == 0;
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

static void raftLogAppend(clusterRaftState *rs, raftLogEntry *e) {
    if (rs->log_count == rs->log_alloc) {
        rs->log_alloc = rs->log_alloc ? rs->log_alloc * 2 : 16;
        rs->log = zrealloc(rs->log, rs->log_alloc * sizeof(raftLogEntry *));
    }
    rs->log[rs->log_count++] = e;
}

/* O(1) lookup by index. Returns NULL if out of range. Indices start at 1. */
static raftLogEntry *raftLogGet(clusterRaftState *rs, uint64_t index) {
    if (index == 0 || index > raftLogLastIndex(rs)) return NULL;
    /* Entries are stored sequentially; first entry's index may not be 1
     * after future log compaction. For now, base is always 1. */
    uint64_t base = rs->log_count > 0 ? rs->log[0]->index : 1;
    if (index < base) return NULL;
    return rs->log[index - base];
}

/* Truncate the log from the given index onwards (inclusive). */
static void raftLogTruncateFrom(clusterRaftState *rs, uint64_t index) {
    while (rs->log_count > 0 && rs->log[rs->log_count - 1]->index >= index) {
        raftLogFree(rs->log[--rs->log_count]);
    }
}

static uint64_t raftLogLastIndex(clusterRaftState *rs) {
    return rs->log_count > 0 ? rs->log[rs->log_count - 1]->index : 0;
}

static uint64_t raftLogLastTerm(clusterRaftState *rs) {
    return rs->log_count > 0 ? rs->log[rs->log_count - 1]->term : 0;
}

static uint64_t raftLogTermAt(clusterRaftState *rs, uint64_t index) {
    raftLogEntry *e = raftLogGet(rs, index);
    return e ? e->term : 0;
}

/* Apply a committed log entry. */
static void raftLogApply(raftLogEntry *e) {
    clusterRaftState *rs = RAFT_STATE();
    switch (e->type) {
    case RAFT_ENTRY_NODE_JOIN: {
        /* data: "<node-id> <address>" */
        int argc;
        sds *argv = sdssplitlen(e->data, sdslen(e->data), " ", 1, &argc);
        if (argv && argc >= 2 && sdslen(argv[0]) == CLUSTER_NAMELEN) {
            clusterNode *existing = clusterLookupNode(argv[0], CLUSTER_NAMELEN);
            if (!existing) {
                clusterNode *n = createClusterNode(argv[0], 0);
                if (clusterNodeParseAddressString(n, argv[1]) == C_OK) {
                    clusterAddNode(n);
                } else {
                    freeClusterNode(n);
                    if (argv) sdsfreesplitres(argv, argc);
                    break;
                }
            }
            server.cluster->size++;
            rs->todo_invalidate_slots_cache = 1;
            rs->todo_connect_nodes = 1;
            serverLog(LL_NOTICE, "Applied NODE_JOIN for %.40s (size=%d).",
                      argv[0], server.cluster->size);

            /* Leader: initialize replication state for the new peer. */
            if (rs->role == RAFT_ROLE_LEADER) {
                clusterNode *joined = clusterLookupNode(argv[0], CLUSTER_NAMELEN);
                if (joined && joined != myself) {
                    RAFT_DATA(joined)->peer.next_index = 1;
                    RAFT_DATA(joined)->peer.match_index = 0;
                    RAFT_DATA(joined)->peer.last_ack_time = mstime();
                }
            }

            /* If this entry is about us, promote from learner to follower. */
            if (memcmp(argv[0], myself->name, CLUSTER_NAMELEN) == 0 &&
                rs->role == RAFT_ROLE_LEARNER) {
                rs->role = RAFT_ROLE_FOLLOWER;
                serverLog(LL_NOTICE, "Promoted from learner to follower.");
            }
        }
        if (argv) sdsfreesplitres(argv, argc);
        break;
    }
    case RAFT_ENTRY_SLOT_CHANGE:
        clusterRaftApplySlotChange(e->data);
        rs->todo_update_slot_coverage = 1;
        rs->todo_invalidate_slots_cache = 1;
        serverLog(LL_NOTICE, "Applied SLOT_CHANGE (index %llu).", (unsigned long long)e->index);
        break;
    case RAFT_ENTRY_SET_REPLICA_OF:
        clusterRaftApplySetReplica(e->data);
        rs->todo_invalidate_slots_cache = 1;
        serverLog(LL_NOTICE, "Applied SET_REPLICA_OF (index %llu).", (unsigned long long)e->index);
        break;
    case RAFT_ENTRY_NODE_FAIL: {
        clusterNode *node = clusterLookupNode(e->data, sdslen(e->data));
        if (node && node != myself) {
            node->flags |= CLUSTER_NODE_FAIL;
            rs->todo_update_slot_coverage = 1;
            rs->todo_invalidate_slots_cache = 1;
        }
        serverLog(LL_NOTICE, "Applied NODE_FAIL %.40s (index %llu).",
                  e->data, (unsigned long long)e->index);
        break;
    }
    default:
        serverLog(LL_NOTICE, "Applied log entry type %d (index %llu).", e->type,
                  (unsigned long long)e->index);
        break;
    }

    /* Check pending proposals for a matching callback (FIFO). */
    if (listLength(rs->pending_proposals) > 0) {
        listNode *ln = listFirst(rs->pending_proposals);
        raftPendingProposal *pp = listNodeValue(ln);
        if (pp->type == e->type && !sdscmp(pp->data, e->data)) {
            pp->callback(pp->ctx, NULL);
            sdsfree(pp->data);
            zfree(pp);
            listDelNode(rs->pending_proposals, ln);
        }
    }
}

/* --------------------------------------------------------------------------
 * AE (AppendEntries) message
 *
 * Header line: AE <leader-id> <term> <prev-log-idx> <prev-log-term> <commit> <count>
 * Entry lines: <term> <type> <data>
 * -------------------------------------------------------------------------- */

static void clusterRaftSendAppendEntries(clusterLink *link, clusterNode *node) {
    clusterRaftState *rs = RAFT_STATE();
    clusterNodeRaftData *rd = RAFT_DATA(node);

    uint64_t next = rd->peer.next_index;
    uint64_t prev_index = next > 0 ? next - 1 : 0;
    uint64_t prev_term = raftLogTermAt(rs, prev_index);

    /* Collect entries to send starting from next_index. */
    int count = 0;
    sds entries = sdsempty();
    for (uint64_t idx = next; idx <= raftLogLastIndex(rs); idx++) {
        raftLogEntry *e = raftLogGet(rs, idx);
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

static void clusterRaftBroadcastAppendEntries(clusterRaftState *rs) {
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    int pending = 0;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node == myself) continue;
        if (!node->link) {
            pending = 1;
            continue;
        }
        clusterRaftSendAppendEntries(node->link, node);
    }
    dictReleaseIterator(di);
    /* Retry in next beforeSleep if some nodes had no link yet. */
    if (pending) rs->todo_broadcast_ae = 1;
}

/* AE_ACK <term> <success> <last-log-index> */
static void clusterRaftSendAppendEntriesResponse(clusterLink *link, uint64_t term, int success) {
    clusterRaftState *rs = RAFT_STATE();
    sds msg = wireNewMsg("AE_ACK");
    msg = sdscatfmt(msg, " %U %i %U", (unsigned long long)term, success,
                    (unsigned long long)raftLogLastIndex(rs));
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

    clusterRaftMaybeStepDown(rs, msg_term);

    /* Accept heartbeat. */
    if (rs->role != RAFT_ROLE_LEARNER) rs->role = RAFT_ROLE_FOLLOWER;
    rs->last_heartbeat = mstime();
    memcpy(rs->leader, argv[1], CLUSTER_NAMELEN);

    /* Log consistency check: verify prev_log_index/term match. */
    if (prev_log_index > 0 && raftLogTermAt(rs, prev_log_index) != prev_log_term) {
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

        raftLogEntry *existing = raftLogGet(rs, new_index);
        if (existing && existing->term != e_term) {
            /* Conflict: truncate from here and append. */
            raftLogTruncateFrom(rs, new_index);
            existing = NULL;
        }
        if (!existing) {
            raftLogAppend(rs, raftLogCreate(e_term, new_index, e_type, e_data));
        } else {
            sdsfree(e_data); /* Already have this entry. */
        }
    }

    /* Update commit index and apply. */
    if (leader_commit > rs->commit_index) {
        rs->commit_index = leader_commit;
        if (rs->commit_index > raftLogLastIndex(rs)) rs->commit_index = raftLogLastIndex(rs);
    }
    while (rs->last_applied < rs->commit_index) {
        rs->last_applied++;
        raftLogEntry *e = raftLogGet(rs, rs->last_applied);
        if (e) raftLogApply(e);
    }

    clusterRaftSendAppendEntriesResponse(link, rs->current_term, 1);
    return 1;
}

static int clusterRaftProcessAppendEntriesResponse(clusterLink *link, int argc, sds *argv) {
    /* argv: AE_ACK <term> <success> <last-log-index> */
    if (argc < 4) return 1;
    serverLog(LL_DEBUG, "Received AE_ACK: term=%s success=%s last_index=%s from %.40s",
              argv[1], argv[2], argv[3], link->node ? link->node->name : "?");

    clusterRaftState *rs = RAFT_STATE();
    uint64_t msg_term = strtoull(argv[1], NULL, 10);
    int success = atoi(argv[2]);
    uint64_t follower_last_index = strtoull(argv[3], NULL, 10);

    clusterRaftMaybeStepDown(rs, msg_term);
    if (rs->role != RAFT_ROLE_LEADER) return 1;

    clusterNode *node = link->node;
    if (!node) return 1;
    clusterNodeRaftData *rd = RAFT_DATA(node);
    rd->peer.last_ack_time = mstime();

    /* Clear FAIL flag if the node is back. */
    if (nodeFailed(node)) {
        node->flags &= ~CLUSTER_NODE_FAIL;
        rs->todo_update_slot_coverage = 1;
        rs->todo_invalidate_slots_cache = 1;
        serverLog(LL_NOTICE, "Node %.40s is back, clearing FAIL.", node->name);
    }

    if (success) {
        /* Follower accepted — update matchIndex/nextIndex. */
        rd->peer.match_index = follower_last_index;
        rd->peer.next_index = follower_last_index + 1;

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
                if (RAFT_DATA(peer)->peer.match_index >= idx) matches++;
            }
            dictReleaseIterator(di);

            int quorum = server.cluster->size / 2 + 1;
            if (matches >= quorum) {
                rs->commit_index = idx;
                break;
            }
        }

        /* Apply newly committed entries. */
        uint64_t prev_commit = rs->last_applied;
        while (rs->last_applied < rs->commit_index) {
            rs->last_applied++;
            raftLogEntry *e = raftLogGet(rs, rs->last_applied);
            if (e) raftLogApply(e);
        }
        /* If commit advanced, broadcast so followers learn the new commit. */
        if (rs->last_applied > prev_commit) {
            rs->todo_broadcast_ae = 1;
        }
    } else {
        /* Follower rejected — decrement nextIndex and retry immediately. */
        if (rd->peer.next_index > 1) rd->peer.next_index--;
        clusterRaftSendAppendEntries(node->link, node);
    }
    return 1;
}

/* VOTE <candidate-id> <term> <last-log-index> <last-log-term> */
static void clusterRaftSendRequestVote(clusterLink *link, clusterRaftState *rs) {
    sds msg = wireNewMsg("VOTE");
    msg = sdscatlen(msg, " ", 1);
    msg = sdscatlen(msg, myself->name, CLUSTER_NAMELEN);
    msg = sdscatfmt(msg, " %U %U %U", (unsigned long long)rs->current_term,
                    (unsigned long long)raftLogLastIndex(rs),
                    (unsigned long long)raftLogLastTerm(rs));
    msg = wireFinishMsg(msg);
    clusterRaftSendMsg(link, msg);
}

/* VOTE_OK <term> <granted> */
static void clusterRaftSendVoteResponse(clusterLink *link, uint64_t term, int granted) {
    sds msg = wireNewMsg("VOTE_OK");
    msg = sdscatfmt(msg, " %U %i", (unsigned long long)term, granted);
    msg = wireFinishMsg(msg);
    clusterRaftSendMsg(link, msg);
}

static int clusterRaftProcessRequestVote(clusterLink *link, int argc, sds *argv) {
    /* argv: VOTE <candidate-id> <term> <last-log-index> <last-log-term> */
    if (argc < 5) return 1;
    if (sdslen(argv[1]) != CLUSTER_NAMELEN) return 1;

    clusterRaftState *rs = RAFT_STATE();
    uint64_t msg_term = strtoull(argv[2], NULL, 10);
    int granted = 0;

    clusterRaftMaybeStepDown(rs, msg_term);

    if (msg_term < rs->current_term) {
        /* Stale term. */
    } else if (clusterRaftIsVotedForNone(rs) || memcmp(rs->voted_for, argv[1], CLUSTER_NAMELEN) == 0) {
        /* TODO: log completeness check (compare last log index/term) */
        granted = 1;
        memcpy(rs->voted_for, argv[1], CLUSTER_NAMELEN);
        rs->last_heartbeat = mstime(); /* Reset election timer */
        serverLog(LL_NOTICE, "Voted for %.40s in term %llu.", argv[1], (unsigned long long)msg_term);
    }

    clusterRaftSendVoteResponse(link, rs->current_term, granted);
    return 1;
}

static int clusterRaftProcessRequestVoteResponse(clusterLink *link, int argc, sds *argv) {
    UNUSED(link);
    /* argv: VOTE_OK <term> <granted> */
    if (argc < 3) return 1;

    clusterRaftState *rs = RAFT_STATE();
    uint64_t msg_term = strtoull(argv[1], NULL, 10);
    int granted = atoi(argv[2]);

    clusterRaftMaybeStepDown(rs, msg_term);

    if (rs->role != RAFT_ROLE_CANDIDATE) return 1;
    if (msg_term != rs->current_term) return 1;

    if (granted) {
        rs->votes_received++;
        int quorum = server.cluster->size / 2 + 1;
        if (rs->votes_received >= quorum) {
            rs->role = RAFT_ROLE_LEADER;
            memcpy(rs->leader, myself->name, CLUSTER_NAMELEN);
            serverLog(LL_NOTICE, "Elected as Raft leader (term %llu, %d votes).",
                      (unsigned long long)rs->current_term, rs->votes_received);
            /* Initialize nextIndex for each peer (Raft paper §5.3). */
            uint64_t last = raftLogLastIndex(rs);
            dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
            dictEntry *de;
            while ((de = dictNext(di)) != NULL) {
                clusterNode *peer = dictGetVal(de);
                if (peer == myself) continue;
                RAFT_DATA(peer)->peer.next_index = last + 1;
                RAFT_DATA(peer)->peer.match_index = 0;
                RAFT_DATA(peer)->peer.last_ack_time = mstime();
            }
            dictReleaseIterator(di);
            /* Immediate heartbeat to assert leadership. */
            clusterRaftBroadcastAppendEntries(rs);
        }
    }
    return 1;
}

static void clusterRaftStartElection(clusterRaftState *rs) {
    rs->current_term++;
    rs->role = RAFT_ROLE_CANDIDATE;
    memcpy(rs->voted_for, myself->name, CLUSTER_NAMELEN);
    rs->votes_received = 1; /* Vote for self */
    clusterRaftRandomizeElectionTimeout(rs);
    rs->last_heartbeat = mstime();

    serverLog(LL_NOTICE, "Starting Raft election (term %llu).", (unsigned long long)rs->current_term);

    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node == myself || !node->link) continue;
        clusterRaftSendRequestVote(node->link, rs);
    }
    dictReleaseIterator(di);

    /* Single-node: already have quorum. */
    int quorum = server.cluster->size / 2 + 1;
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
    rs->role = RAFT_ROLE_FOLLOWER;
    clusterRaftRandomizeElectionTimeout(rs);
    rs->last_heartbeat = mstime();
    rs->peer_state = dictCreate(&clusterNodesDictType);
    rs->pending_proposals = listCreate();
    rs->pending_meets = listCreate();
    server.cluster->protocol_data = rs;
    server.cluster->size = 1; /* Myself */
}

static void clusterRaftInitLast(void) {
    clusterListenerInit();

    /* Single-node cluster: become leader immediately. */
    clusterRaftState *rs = RAFT_STATE();
    if (dictSize(server.cluster->nodes) == 1) {
        rs->role = RAFT_ROLE_LEADER;
        rs->current_term = 1;
        memcpy(rs->leader, myself->name, CLUSTER_NAMELEN);
        serverLog(LL_NOTICE, "Single-node cluster: becoming Raft leader (term %llu).",
                  (unsigned long long)rs->current_term);
    }
}

static void clusterRaftCron(void) {
    clusterRaftState *rs = RAFT_STATE();
    mstime_t now = mstime();

    clusterConnectNodes();

    if (dictSize(server.cluster->nodes) > 1) {
        /* Follower/candidate: check election timeout. */
        if ((rs->role == RAFT_ROLE_FOLLOWER || rs->role == RAFT_ROLE_CANDIDATE) &&
            now - rs->last_heartbeat > rs->election_timeout) {
            clusterRaftStartElection(rs);
        }

        /* Leader: send periodic heartbeats. */
        if (rs->role == RAFT_ROLE_LEADER) {
            mstime_t heartbeat_interval = rs->election_timeout / 10;
            if (heartbeat_interval < 100) heartbeat_interval = 100;
            if (now - rs->last_heartbeat > heartbeat_interval) {
                rs->last_heartbeat = now;
                clusterRaftBroadcastAppendEntries(rs);
            }

            /* Leader: detect node failures. */
            mstime_t node_timeout = server.cluster_node_timeout;
            dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
            dictEntry *de;
            while ((de = dictNext(di)) != NULL) {
                clusterNode *node = dictGetVal(de);
                if (node == myself || nodeFailed(node)) continue;
                clusterNodeRaftData *rd = RAFT_DATA(node);
                if (rd->peer.last_ack_time > 0 &&
                    now - rd->peer.last_ack_time > node_timeout) {
                    serverLog(LL_NOTICE, "Node %.40s not responding, proposing NODE_FAIL.", node->name);
                    sds entry = sdsnew("NODE_FAIL ");
                    entry = sdscatlen(entry, node->name, CLUSTER_NAMELEN);
                    clusterRaftPropose(entry, NULL, NULL);
                    sdsfree(entry);
                }
            }
            dictReleaseIterator(di);
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

static void clusterRaftBeforeSleep(void) {
    clusterRaftState *rs = RAFT_STATE();

    if (rs->todo_connect_nodes) {
        rs->todo_connect_nodes = 0;
        clusterConnectNodes();
    }

    if (rs->todo_broadcast_ae && rs->role == RAFT_ROLE_LEADER) {
        rs->todo_broadcast_ae = 0;
        clusterRaftBroadcastAppendEntries(rs);
    } else {
        rs->todo_broadcast_ae = 0;
    }

    if (rs->todo_update_slot_coverage) {
        rs->todo_update_slot_coverage = 0;
        clusterRaftCheckSlotCoverage();
    }

    if (rs->todo_invalidate_slots_cache) {
        rs->todo_invalidate_slots_cache = 0;
        clearCachedClusterSlotsResponse();
    }
}

static void clusterRaftHandleServerShutdown(bool auto_failover) {
    UNUSED(auto_failover);
    /* TODO: step down if leader, persist state */
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
    } else if (!strcasecmp(argv[0], "WELCOME")) {
        ret = clusterRaftProcessWelcome(link, argc, argv);
    } else if (!strcasecmp(argv[0], "PROPOSE")) {
        ret = clusterRaftProcessPropose(link, argc, argv);
    } else if (!strcasecmp(argv[0], "AE")) {
        ret = clusterRaftProcessAppendEntries(link, argc, argv, lines + 1, line_count - 1);
    } else if (!strcasecmp(argv[0], "AE_ACK")) {
        ret = clusterRaftProcessAppendEntriesResponse(link, argc, argv);
    } else if (!strcasecmp(argv[0], "VOTE")) {
        ret = clusterRaftProcessRequestVote(link, argc, argv);
    } else if (!strcasecmp(argv[0], "VOTE_OK")) {
        ret = clusterRaftProcessRequestVoteResponse(link, argc, argv);
    } else {
        serverLog(LL_WARNING, "Unknown Raft message: %s", argv[0]);
    }

    sdsfreesplitres(argv, argc);
done:
    if (lines) sdsfreesplitres(lines, line_count);
    return ret;
}

/* Look up and consume a pending MEET callback by node address. */
static raftPendingMeet *clusterRaftConsumePendingMeet(clusterNode *node) {
    clusterRaftState *rs = RAFT_STATE();
    sds addr = sdscatfmt(sdsempty(), "%s:%i", node->ip, node->cport);
    listIter li;
    listNode *ln;
    listRewind(rs->pending_meets, &li);
    while ((ln = listNext(&li)) != NULL) {
        raftPendingMeet *pm = listNodeValue(ln);
        if (!sdscmp(pm->addr, addr)) {
            sdsfree(addr);
            listDelNode(rs->pending_meets, ln);
            return pm;
        }
    }
    sdsfree(addr);
    return NULL;
}

static void clusterRaftJoinCallback(void *ctx, const char *error) {
    raftJoinCallbackCtx *jc = ctx;
    /* Send WELCOME to the joined node. */
    if (!error) {
        clusterNode *n = clusterLookupNode(jc->node_name, sdslen(jc->node_name));
        if (n && n->link) clusterRaftSendGreeting(n->link, "WELCOME");
    }
    /* Fire the MEET callback if present. */
    if (jc->meet_callback) jc->meet_callback(jc->meet_ctx, error);
    sdsfree(jc->node_name);
    zfree(jc);
}

static void clusterRaftPostConnect(struct clusterLink *link) {
    /* Send HELLO on every new outbound link for identification. */
    clusterRaftSendHello(link);
}

/* --------------------------------------------------------------------------
 * Config updates — broadcast metadata changes through Raft log
 * -------------------------------------------------------------------------- */

static void clusterRaftUpdateMyself(int old_flags) {
    UNUSED(old_flags);
    /* TODO: propose a NODE_META entry to the Raft log */
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

static sds clusterRaftAppendVarsLine(sds config) {
    clusterRaftState *rs = RAFT_STATE();
    config = sdscatprintf(config, "vars currentTerm %llu raftLeader %.40s\n",
                          (unsigned long long)rs->current_term, rs->leader);
    return config;
}

static int clusterRaftParseVarsLine(const char *name, const char *value) {
    clusterRaftState *rs = RAFT_STATE();
    if (!strcasecmp(name, "currentTerm")) {
        rs->current_term = strtoull(value, NULL, 10);
        return 1;
    } else if (!strcasecmp(name, "raftLeader")) {
        memcpy(rs->leader, value, CLUSTER_NAMELEN);
        return 1;
    }
    return 0;
}

static void clusterRaftPostLoad(void) {
    /* TODO: rebuild peer state from loaded nodes */
}

/* --------------------------------------------------------------------------
 * Message propagation
 * -------------------------------------------------------------------------- */

static void clusterRaftPropagatePublish(robj *channel, robj *message, int sharded) {
    UNUSED(channel);
    UNUSED(message);
    UNUSED(sharded);
    /* TODO: pub/sub propagation over Raft links */
}

static int clusterRaftSendModuleMessage(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len) {
    UNUSED(target);
    UNUSED(module_id);
    UNUSED(type);
    UNUSED(payload);
    UNUSED(len);
    return C_ERR; /* TODO */
}

/* --------------------------------------------------------------------------
 * Info and stats
 * -------------------------------------------------------------------------- */

static unsigned long clusterRaftGetConnectionsCount(void) {
    return dictSize(server.cluster->nodes) - 1;
}

static void clusterRaftResetStats(void) {
    /* TODO */
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
    const char *role_str = rs->role == RAFT_ROLE_LEADER      ? "leader"
                           : rs->role == RAFT_ROLE_CANDIDATE ? "candidate"
                           : rs->role == RAFT_ROLE_LEARNER   ? "learner"
                                                             : "follower";
    info = sdscatprintf(info,
                        "cluster_raft_role:%s\r\n"
                        "cluster_raft_current_term:%llu\r\n"
                        "cluster_raft_commit_index:%llu\r\n"
                        "cluster_raft_last_applied:%llu\r\n"
                        "cluster_raft_log_entries:%llu\r\n"
                        "cluster_raft_leader:%.40s\r\n",
                        role_str, (unsigned long long)rs->current_term,
                        (unsigned long long)rs->commit_index, (unsigned long long)rs->last_applied,
                        (unsigned long long)rs->log_count, rs->leader);
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
    /* TODO */
}

static void clusterRaftCancelAutomaticFailover(void) {
    /* TODO */
}

/* --------------------------------------------------------------------------
 * Node management — propose changes to Raft log
 * -------------------------------------------------------------------------- */


static void clusterRaftSlotChange(slotRange *ranges, int numranges, clusterNode *target, void *ctx, void (*callback)(void *ctx, const char *error)) {
    /* Build entry: "SLOT_CHANGE <node-id-or-dash> <ranges...>" */
    sds entry = sdsnew("SLOT_CHANGE ");
    entry = sdscatlen(entry, target ? target->name : "-", target ? CLUSTER_NAMELEN : 1);
    for (int i = 0; i < numranges; i++) {
        if (ranges[i].start_slot == ranges[i].end_slot)
            entry = sdscatfmt(entry, " %i", ranges[i].start_slot);
        else
            entry = sdscatfmt(entry, " %i-%i", ranges[i].start_slot, ranges[i].end_slot);
    }

    /* Propose through Raft (leader appends, follower forwards).
     * The callback is invoked when the entry is committed and applied. */
    clusterRaftPropose(entry, ctx, callback);
    sdsfree(entry);
}

/* Apply a SLOT_CHANGE entry. Format: "<node-id-or-dash> <range> [<range> ...]"
 * Ranges use the same format as nodes.conf: "0-5460" or "5461". */
static void clusterRaftApplySlotChange(sds data) {
    int argc;
    sds *argv = sdssplitlen(data, sdslen(data), " ", 1, &argc);
    if (!argv || argc < 2) goto done;

    clusterNode *target = (sdslen(argv[0]) == CLUSTER_NAMELEN)
                              ? clusterLookupNode(argv[0], CLUSTER_NAMELEN)
                              : NULL;

    for (int i = 1; i < argc; i++) {
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
            if (target) {
                if (server.cluster->slots[j]) clusterDelSlot(j);
                clusterAddSlot(target, j);
            } else {
                clusterDelSlot(j);
            }
        }
    }
done:
    if (argv) sdsfreesplitres(argv, argc);
}

/* Apply a SET_REPLICA_OF entry. Format: "<replica-id> <primary-id-or-dash>" */
static void clusterRaftApplySetReplica(sds data) {
    int argc;
    sds *argv = sdssplitlen(data, sdslen(data), " ", 1, &argc);
    if (!argv || argc != 2) goto done;

    clusterNode *replica = clusterLookupNode(argv[0], sdslen(argv[0]));
    if (!replica) goto done;

    if (sdslen(argv[1]) == 1 && argv[1][0] == '-') {
        /* Promote to primary. */
        if (nodeIsReplica(replica)) {
            if (replica->replicaof) clusterNodeRemoveReplica(replica->replicaof, replica);
            replica->flags &= ~CLUSTER_NODE_REPLICA;
            replica->flags |= CLUSTER_NODE_PRIMARY;
            replica->replicaof = NULL;
            if (replica == myself) replicationUnsetPrimary();
        }
        char new_shard_id[CLUSTER_NAMELEN];
        getRandomHexChars(new_shard_id, CLUSTER_NAMELEN);
        clusterRemoveNodeFromShard(replica);
        memcpy(replica->shard_id, new_shard_id, CLUSTER_NAMELEN);
        clusterAddNodeToShard(new_shard_id, replica);
    } else {
        clusterNode *primary = clusterLookupNode(argv[1], sdslen(argv[1]));
        if (!primary) goto done;
        if (replica == myself) {
            clusterSetPrimary(primary, 1, 1);
        } else {
            if (replica->replicaof) clusterNodeRemoveReplica(replica->replicaof, replica);
            replica->flags &= ~CLUSTER_NODE_PRIMARY;
            replica->flags |= CLUSTER_NODE_REPLICA;
            replica->replicaof = primary;
            clusterNodeAddReplica(primary, replica);
            /* TODO: Replication offsets are not propagated in the raft
             * protocol (gossip piggybacks them on ping/pong). Set to
             * non-zero so isNodeAvailable() returns true and CLUSTER SLOTS
             * includes this replica. To make CLUSTER SHARDS fully compatible,
             * we would need to propagate offsets periodically (e.g. on
             * heartbeats) or at least a loading/online flag. */
            replica->repl_offset = 1;
            /* Move replica to primary's shard. */
            clusterRemoveNodeFromShard(replica);
            memcpy(replica->shard_id, primary->shard_id, CLUSTER_NAMELEN);
            clusterAddNodeToShard(primary->shard_id, replica);
        }
    }
done:
    if (argv) sdsfreesplitres(argv, argc);
}

static void clusterRaftForgetNode(const char *node_id, size_t id_len, void *ctx, void (*callback)(void *ctx, const char *error)) {
    UNUSED(node_id);
    UNUSED(id_len);
    /* TODO: propose NODE_LEAVE entry */
    if (callback) callback(ctx, "not implemented");
}

static void clusterRaftSetReplicaOf(clusterNode *primary, void *ctx, void (*callback)(void *ctx, const char *error)) {
    /* Propose SET_REPLICA_OF: "<myself-id> <primary-id-or-dash>" */
    sds entry = sdsnew("SET_REPLICA_OF ");
    entry = sdscatlen(entry, myself->name, CLUSTER_NAMELEN);
    entry = sdscatlen(entry, " ", 1);
    entry = sdscatlen(entry, primary ? primary->name : "-", primary ? CLUSTER_NAMELEN : 1);
    clusterRaftPropose(entry, ctx, callback);
    sdsfree(entry);
}

static void clusterRaftFailover(int force, int takeover, void *ctx, void (*callback)(void *ctx, const char *error)) {
    UNUSED(force);
    UNUSED(takeover);
    /* TODO: propose FAILOVER entry */
    if (callback) callback(ctx, "not implemented");
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
        clusterNode *n = createClusterNode(NULL, CLUSTER_NODE_HANDSHAKE | CLUSTER_NODE_MEET);
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

static void clusterRaftResetCluster(int hard) {
    UNUSED(hard);
    /* TODO */
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
    .handleServerShutdown = clusterRaftHandleServerShutdown,
    .validateMessageHeader = clusterRaftValidateMessageHeader,
    .processMessage = clusterRaftProcessMessage,
    .postConnect = clusterRaftPostConnect,
    .propagatePublish = clusterRaftPropagatePublish,
    .sendModuleMessage = clusterRaftSendModuleMessage,
    .onMyselfUpdated = clusterRaftUpdateMyself,
    .getConnectionsCount = clusterRaftGetConnectionsCount,
    .resetStats = clusterRaftResetStats,
    .appendInfoFields = clusterRaftAppendInfoFields,
    .getFailureReportsCount = clusterRaftGetFailureReportsCount,
    .getNodePingPongEpoch = NULL,
    .setNodePingPongEpoch = NULL,
    .setNodeFailed = NULL,
    .appendVarsLine = clusterRaftAppendVarsLine,
    .parseVarsLine = clusterRaftParseVarsLine,
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
