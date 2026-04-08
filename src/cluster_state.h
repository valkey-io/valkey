#ifndef CLUSTER_STATE_H
#define CLUSTER_STATE_H

#include "cluster.h"
#include "dict.h"

/* Forward declaration. Full definition is in the protocol implementation. */
typedef struct clusterLink clusterLink;

/* Dict types for common cluster state (nodes, shards, slots). */
extern dictType clusterNodesDictType;
extern dictType clusterSdsToListType;
extern dictType clusterSlotDictType;

/* Cluster node flags and macros. */
#define CLUSTER_NODE_PRIMARY (1 << 0)    /* The node is a primary */
#define CLUSTER_NODE_REPLICA (1 << 1)    /* The node is a replica */
#define CLUSTER_NODE_PFAIL (1 << 2)      /* Failure? Need acknowledge */
#define CLUSTER_NODE_FAIL (1 << 3)       /* The node is believed to be malfunctioning */
#define CLUSTER_NODE_MYSELF (1 << 4)     /* This node is myself */
#define CLUSTER_NODE_HANDSHAKE (1 << 5)  /* We have still to exchange the first ping */
#define CLUSTER_NODE_NOADDR (1 << 6)     /* We don't know the address of this node */
#define CLUSTER_NODE_MEET (1 << 7)       /* Send a MEET message to this node */
#define CLUSTER_NODE_MIGRATE_TO (1 << 8) /* Primary eligible for replica migration. */
#define CLUSTER_NODE_NOFAILOVER (1 << 9) /* Replica will not try to failover. */

#define nodeIsPrimary(n) ((n)->flags & CLUSTER_NODE_PRIMARY)
#define nodeIsReplica(n) ((n)->flags & CLUSTER_NODE_REPLICA)
#define nodeInHandshake(n) ((n)->flags & CLUSTER_NODE_HANDSHAKE)
#define nodeHasAddr(n) (!((n)->flags & CLUSTER_NODE_NOADDR))
#define nodeTimedOut(n) ((n)->flags & CLUSTER_NODE_PFAIL)
#define nodeFailed(n) ((n)->flags & CLUSTER_NODE_FAIL)
#define nodeCantFailover(n) ((n)->flags & CLUSTER_NODE_NOFAILOVER)

struct clusterNode {
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

/* Default cluster bus port offset from the client port. */
#define CLUSTER_PORT_INCR 10000

/* Returns the default client-facing port (TLS port if TLS cluster, else TCP). */
int defaultClientPort(void);

/* Derive announced ports from server configuration. */
void deriveAnnouncedPorts(int *announced_tcp_port,
                          int *announced_tls_port,
                          int *announced_cport,
                          int *announced_client_tcp_port,
                          int *announced_client_tls_port);

/* Node accessor used by protocol implementations and description generation. */
char *humanNodename(clusterNode *node);

/* Bitmap helpers for slot bitmaps. */
int bitmapTestBit(unsigned char *bitmap, int pos);
void bitmapSetBit(unsigned char *bitmap, int pos);
void bitmapClearBit(unsigned char *bitmap, int pos);

/* Slot migration state. */
void setMigratingSlotDest(int slot, clusterNode *node);
void setImportingSlotSource(int slot, clusterNode *node);
void clusterCloseAllSlots(void);
int clusterNodeRemoveReplica(clusterNode *primary, clusterNode *replica);
int clusterNodeAddReplica(clusterNode *primary, clusterNode *replica);
int clusterCountNonFailingReplicas(clusterNode *n);
int clusterPrimariesHaveReplicas(void);
int clusterNodeClearSlotBit(clusterNode *n, int slot);
void clusterRemoveNodeFromShard(clusterNode *node);
void clusterAddNodeToShard(const char *shard_id, clusterNode *node);
void clusterAddNode(clusterNode *node);
list *clusterGetNodesInMyShard(clusterNode *node);

/* Node creation. */
clusterNode *createClusterNode(char *nodename, int flags);

/* Slot assignment. */
void clusterNodeSetSlotBit(clusterNode *n, int slot);
int clusterAddSlot(clusterNode *n, int slot);
int clusterDelSlot(int slot);
int clusterDelNodeSlots(clusterNode *node);

bool isAnySlotInManualImportingState(void);
bool isAnySlotInManualMigratingState(void);

#endif /* CLUSTER_STATE_H */
