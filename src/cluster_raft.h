#ifndef __CLUSTER_RAFT_H
#define __CLUSTER_RAFT_H

#include "server.h"
#include "sds.h"
#include "cluster.h"

/* Forward declarations */
typedef struct clusterMsg clusterMsg;
typedef struct clusterMsgPingExt clusterMsgPingExt;
struct clusterLink;

/* Raft message types */
#define CLUSTERMSG_TYPE_RAFT_HANDSHAKE_REQUEST 0
#define CLUSTERMSG_TYPE_RAFT_HANDSHAKE_REPLY 1

/* Raft message header */
typedef struct {
    char sig[4];     /* Signature "VCv2" */
    uint32_t totlen; /* Total length of this message */
    uint16_t ver;    /* Protocol version */
    uint16_t type;   /* Message type */
} clusterRaftHeader;

/* Raft node role state machine:
 *
 *  CLUSTER MEET
 *   Remote Node
 *       |
 *       v
 * +-------------+         +-------------+         +-------------+
 * |             | Handsh. |             | Propos. |             |
 * | HANDSHAKING |-------->| NON_MEMBER  |-------->|   JOINING   |
 * |             | Compl.  |             |         |             |
 * +-------------+         +-------------+         +-------------+
 *                                                   ^    |
 *                      +: : : : Join Decision : : : :    | Committed
 *                      :                                 v
 *               +-------------+         +-------------+         +-------------+
 *               |             | Quorum  |             | Elect.  |             |
 * Bootstrap --->|   LEADER    |<--------|  CANDIDATE  |<--------|  FOLLOWER   |
 *               |             | Recv.   |             | Timeout |             |
 *               +-------------+         +-------------+         +-------------+
 *                      |                       ^                       |
 *                      |                       | Leader Discov.        |
 *                      |                       +-----------------------+
 *                      |                                               |
 *                      |               Higher Term Seen                |
 *                      +-----------------------------------------------+
 */
#define RAFT_ROLE_UNKNOWN 0     /* Unused outside of node initialization */
#define RAFT_ROLE_FOLLOWER 1    /* A follower in my Raft group*/
#define RAFT_ROLE_CANDIDATE 2   /* A candidate to be leader in my Raft group */
#define RAFT_ROLE_LEADER 3      /* The leader of my Raft group */
#define RAFT_ROLE_HANDSHAKING 4 /* A non-member node I am handshaking with */
#define RAFT_ROLE_NON_MEMBER 5  /* A non-member node I have finished handshaking */
#define RAFT_ROLE_JOINING 6     /* A proposed member node that has not been fully committed.*/

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
    uint32_t member_count;               /* Raft member count of the node */
    mstime_t outbound_link_attempt_time; /* Time of last connection attempt */
} clusterNodeRaftData;

typedef struct {
    uint64_t term;
    char sender_name[CLUSTER_NAMELEN];
    uint32_t member_count;
    char remote_ip[NET_IP_STR_LEN];
    uint16_t plaintext_port;
    uint16_t tls_port;
    uint16_t cluster_port;
} clusterMsgRaftHandshake;

#endif
