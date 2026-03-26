#ifndef CLUSTER_LEGACY_H
#define CLUSTER_LEGACY_H

#include <stdint.h>
#define CLUSTER_PORT_INCR 10000 /* Cluster port = baseport + PORT_INCR */

/* The following defines are amount of time, sometimes expressed as
 * multipliers of the node timeout value (when ending with MULT). */
#define CLUSTER_FAIL_REPORT_VALIDITY_MULT 2  /* Fail report validity. */
#define CLUSTER_FAIL_UNDO_TIME_MULT 2        /* Undo fail if primary is back. */
#define CLUSTER_MF_PAUSE_MULT 2              /* Primary pause manual failover mult. */
#define CLUSTER_REPLICA_MIGRATION_DELAY 5000 /* Delay for replica migration. */

/* Reasons why a replica is not able to failover. */
#define CLUSTER_CANT_FAILOVER_NONE 0
#define CLUSTER_CANT_FAILOVER_DATA_AGE 1
#define CLUSTER_CANT_FAILOVER_WAITING_DELAY 2
#define CLUSTER_CANT_FAILOVER_EXPIRED 3
#define CLUSTER_CANT_FAILOVER_WAITING_VOTES 4
#define CLUSTER_CANT_FAILOVER_RELOG_PERIOD 1 /* seconds. */

/* clusterState todo_before_sleep flags. */
#define CLUSTER_TODO_HANDLE_FAILOVER (1 << 0)
#define CLUSTER_TODO_UPDATE_STATE (1 << 1)
#define CLUSTER_TODO_SAVE_CONFIG (1 << 2)
#define CLUSTER_TODO_FSYNC_CONFIG (1 << 3)
#define CLUSTER_TODO_HANDLE_MANUALFAILOVER (1 << 4)
#define CLUSTER_TODO_BROADCAST_ALL (1 << 5)
#define CLUSTER_TODO_HANDLE_SLOT_MIGRATION (1 << 6)

/* Cluster node flags and macros. */
#define CLUSTER_NODE_PRIMARY (1 << 0)                                             /* The node is a primary */
#define CLUSTER_NODE_REPLICA (1 << 1)                                             /* The node is a replica */
#define CLUSTER_NODE_PFAIL (1 << 2)                                               /* Failure? Need acknowledge */
#define CLUSTER_NODE_FAIL (1 << 3)                                                /* The node is believed to be malfunctioning */
#define CLUSTER_NODE_MYSELF (1 << 4)                                              /* This node is myself */
#define CLUSTER_NODE_HANDSHAKE (1 << 5)                                           /* We have still to exchange the first ping */
#define CLUSTER_NODE_NOADDR (1 << 6)                                              /* We don't know the address of this node */
#define CLUSTER_NODE_MEET (1 << 7)                                                /* Send a MEET message to this node */
#define CLUSTER_NODE_MIGRATE_TO (1 << 8)                                          /* Primary eligible for replica migration. */
#define CLUSTER_NODE_NOFAILOVER (1 << 9)                                          /* Replica will not try to failover. */
#define CLUSTER_NODE_EXTENSIONS_SUPPORTED (1 << 10)                               /* This node supports extensions. */
#define CLUSTER_NODE_LIGHT_HDR_PUBLISH_SUPPORTED (1 << 11)                        /* This node supports light message header for publish type. */
#define CLUSTER_NODE_LIGHT_HDR_MODULE_SUPPORTED (1 << 12)                         /* This node supports light message header for module type. */
#define CLUSTER_NODE_MULTI_MEET_SUPPORTED CLUSTER_NODE_LIGHT_HDR_MODULE_SUPPORTED /* This node handles multi meet packet.                             \
                                                                                     Light hdr for module and multi meet were both introduced in 8.1, \
                                                                                     so we could reduce the same flag value. */
#define CLUSTER_NODE_MY_PRIMARY_FAIL (1 << 13)                                    /* myself is a replica and my primary is FAIL in my view. \
                                                                                   * myself will gossip this flag to other replica in the   \
                                                                                   * shard so that the replicas can make a better ranking   \
                                                                                   * decisions to help with the failover. */

#define CLUSTER_NODE_NULL_NAME                                                                                         \
    "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000" \
    "\000\000\000\000\000\000\000\000\000\000\000\000"

#define nodeIsPrimary(n) ((n)->flags & CLUSTER_NODE_PRIMARY)
#define nodeIsReplica(n) ((n)->flags & CLUSTER_NODE_REPLICA)
#define nodeInHandshake(n) ((n)->flags & CLUSTER_NODE_HANDSHAKE)
#define nodeInMeetState(n) ((n)->flags & CLUSTER_NODE_MEET)
#define nodeHasAddr(n) (!((n)->flags & CLUSTER_NODE_NOADDR))
#define nodeTimedOut(n) ((n)->flags & CLUSTER_NODE_PFAIL)
#define nodeFailed(n) ((n)->flags & CLUSTER_NODE_FAIL)
#define nodeCantFailover(n) ((n)->flags & CLUSTER_NODE_NOFAILOVER)
#define nodeSupportsExtensions(n) ((n)->flags & CLUSTER_NODE_EXTENSIONS_SUPPORTED)
#define nodeSupportsMultiMeet(n) ((n)->flags & CLUSTER_NODE_MULTI_MEET_SUPPORTED)
#define nodeInNormalState(n) (!((n)->flags & (CLUSTER_NODE_HANDSHAKE | CLUSTER_NODE_MEET | CLUSTER_NODE_PFAIL | CLUSTER_NODE_FAIL)))
#define nodePrimaryIsFail(n) ((n)->flags & CLUSTER_NODE_MY_PRIMARY_FAIL)

/* Forward declaration for clusterNode. Full definition in cluster_legacy.c. */
typedef struct clusterLink clusterLink;

#define CLUSTERMSG_TYPE_COUNT 11 /* Total number of message types. */

/* Legacy protocol-specific data, stored in clusterNode.protocol_data. */
typedef struct clusterNodeLegacyData {
    uint64_t configEpoch;                   /* Last configEpoch observed for this node */
    unsigned long long last_in_ping_gossip; /* The number of the last carried in the ping gossip section */
    mstime_t ping_sent;                     /* Unix time we sent latest ping */
    mstime_t pong_received;                 /* Unix time we received the pong */
    mstime_t meet_sent;                     /* Unix time we sent latest meet packet */
    mstime_t fail_time;                     /* Unix time when FAIL flag was set */
    mstime_t orphaned_time;                 /* Starting time of orphaned primary condition */
    rax *fail_reports;                      /* Radix tree for failure reports with sorted order by timestamp */
} clusterNodeLegacyData;

struct _clusterNode {
    mstime_t ctime;                         /* Node object creation time. */
    char name[CLUSTER_NAMELEN];             /* Node name, hex string, sha1-size */
    char shard_id[CLUSTER_NAMELEN];         /* shard id, hex string, sha1-size */
    int flags;                              /* CLUSTER_NODE_... */
    unsigned char slots[CLUSTER_SLOTS / 8]; /* slots handled by this node */
    uint16_t *slot_info_pairs;              /* Slots info represented as (start/end) pair (consecutive index). */
    int slot_info_pairs_count;              /* Used number of slots in slot_info_pairs */
    int numslots;                           /* Number of slots handled by this node */
    int num_replicas;                       /* Number of replica nodes, if this is a primary */
    clusterNode **replicas;                 /* pointers to replica nodes */
    clusterNode *replicaof;                 /* pointer to the primary node. Note that it
                                             may be NULL even if the node is a replica
                                             if we don't have the primary node in our
                                             tables. */
    mstime_t data_received;                 /* Unix time we received any data */
    mstime_t outbound_link_attempt_time;    /* Unix time we last tried to establish an outgoing link */
    mstime_t inbound_link_freed_time;       /* Last time we freed the inbound link for this node.
                                               If it was never freed, it is the same as ctime */
    long long repl_offset;                  /* Last known repl offset for this node. */
    char ip[NET_IP_STR_LEN];                /* Latest known IP address of this node */
    sds announce_client_ipv4;               /* IPv4 for clients only. */
    sds announce_client_ipv6;               /* IPv6 for clients only. */
    sds hostname;                           /* The known hostname for this node */
    sds human_nodename;                     /* The known human readable nodename for this node */
    sds availability_zone;                  /* The known availability zone for this node */
    int tcp_port;                           /* Latest known clients TCP port. */
    int tls_port;                           /* Latest known clients TLS port */
    int cport;                              /* Latest known cluster port of this node. */
    int announce_client_tcp_port;           /* Port for clients only. */
    int announce_client_tls_port;           /* TLS port for clients only. */
    clusterLink *link;                      /* TCP/IP link established toward this node */
    clusterLink *inbound_link;              /* TCP/IP link accepted from this node */
    int is_node_healthy;                    /* Boolean indicating the cached node health.
                                               Update with updateAndCountChangedNodeHealth(). */
    void *protocol_data;                    /* Protocol-specific data (e.g. clusterNodeLegacyData) */
};

/* Struct used for storing slot statistics. */
typedef struct slotStat {
    uint64_t cpu_usec;
    uint64_t network_bytes_in;
    uint64_t network_bytes_out;
} slotStat;

typedef struct slotRange {
    int start_slot;
    int end_slot;
} slotRange;

/* Legacy protocol-specific state, stored in clusterState.protocol_data. */
typedef struct clusterLegacyState {
    uint64_t currentEpoch;
    int safe_to_join;       /* Can the restarted node safely join the cluster? */
    dict *nodes_black_list; /* Nodes we don't re-add for a few seconds. */
    /* The following fields are used to take the replica state on elections. */
    mstime_t failover_auth_time;      /* Time of previous or next election. */
    int failover_auth_count;          /* Number of votes received so far. */
    int failover_auth_sent;           /* True if we already asked for votes. */
    int failover_auth_rank;           /* This replica rank for current auth request. */
    int failover_failed_primary_rank; /* The rank of this instance in the context of all failed primary list. */
    uint64_t failover_auth_epoch;     /* Epoch of the current election. */
    int cant_failover_reason;         /* Why a replica is currently not able to
                                       * failover. See the CANT_FAILOVER_* macros. */
    /* Manual failover state in common. */
    mstime_t mf_end; /* Manual failover time limit (ms unixtime).
                        It is zero if there is no MF in progress. */
    /* Manual failover state of primary. */
    clusterNode *mf_replica; /* replica performing the manual failover. */
    /* Manual failover state of replica. */
    long long mf_primary_offset; /* Primary offset the replica needs to start MF
                                   or -1 if still not received. */
    int mf_can_start;            /* If non-zero signal that the manual failover
                                    can start requesting primary vote. */
    /* The following fields are used by primaries to take state on elections. */
    uint64_t lastVoteEpoch; /* Epoch of the last vote granted. */
    int todo_before_sleep;  /* Things to do in clusterBeforeSleep(). */
    /* Stats */
    long long stats_bus_messages_sent[CLUSTERMSG_TYPE_COUNT];
    long long stats_bus_messages_received[CLUSTERMSG_TYPE_COUNT];
    long long stats_pfail_nodes;                                 /* Number of nodes in PFAIL status,
                                                                    excluding nodes without address. */
    unsigned long long stat_cluster_links_buffer_limit_exceeded; /* Total number of cluster links freed due to
                                                                    exceeding buffer limit */
    unsigned char owner_not_claiming_slot[CLUSTER_SLOTS / 8];
} clusterLegacyState;

struct clusterState {
    clusterNode *myself; /* This node */
    int state;           /* CLUSTER_OK, CLUSTER_FAIL, ... */
    int fail_reason;     /* Why the cluster state changes to fail. */
    int size;            /* Num of primary nodes with at least one slot */
    dict *nodes;         /* Hash table of name -> clusterNode structures */
    dict *shards;        /* Hash table of shard_id -> list (of nodes) structures */
    dict *migrating_slots_to;
    dict *importing_slots_from;
    clusterNode *slots[CLUSTER_SLOTS];
    list *slot_migration_jobs; /* List storing all slot migration jobs. */
    slotStat slot_stats[CLUSTER_SLOTS];
    void *protocol_data; /* Protocol-specific state (e.g. clusterLegacyState) */
};

#endif // CLUSTER_LEGACY_H
