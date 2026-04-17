#ifndef __CLUSTER_RAFT_H
#define __CLUSTER_RAFT_H

#include "server.h"
#include "sds.h"
#include "cluster.h"

/* Forward declarations */
typedef struct clusterMsg clusterMsg;
typedef struct clusterMsgPingExt clusterMsgPingExt;

/* Raft roles */
#define RAFT_ROLE_UNKNOWN 0
#define RAFT_ROLE_FOLLOWER 1
#define RAFT_ROLE_CANDIDATE 2
#define RAFT_ROLE_LEADER 3

typedef struct raftAppliedEntry {
    uint64_t index;
    uint64_t term;
    sds value;
} raftAppliedEntry;

typedef struct clusterRaftState {
    bool enabled;
    uint64_t term;       /* Current term */
    clusterNode *leader; /* Pointer to the current leader */

    uint64_t last_applied_index; /* Index of the newest entry in applied_entries */
    uint64_t last_applied_term;  /* Term of the newest entry in applied_entries */
    dict *applied_entries;       /* Map of key -> raftAppliedEntry */
} clusterRaftState;

typedef struct clusterNodeRaftData {
    int role;
} clusterNodeRaftData;

#endif
