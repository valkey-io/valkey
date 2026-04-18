#include "server.h"
#include "cluster.h"
#include "cluster_state.h"
#include "cluster_link.h"
#include "cluster_bus.h"
#include "cluster_raft.h"
#include <arpa/inet.h>

/* Access Raft protocol-specific state from the cluster. */
#define RAFT_STATE() ((clusterRaftState *)server.cluster->protocol_data)

/* Access Raft protocol-specific data from a clusterNode. */
#define RAFT_DATA(n) ((clusterNodeRaftData *)(n)->protocol_data)

/* Forward declarations */
clusterMsgSendBlock *createClusterRaftMsgSendBlock(int type, uint32_t payload_len);
void clusterRaftStateMachine(void);
void clusterRaftInit(void);
void clusterRaftFree(void);
void clusterRaftCron(void);
void clusterRaftProcessHandshake(clusterLink *link, uint16_t type, clusterMsgRaftHandshake *req);
void clusterRaftConnectToNode(clusterNode *node);

/* Dictionary type for applied entries. */
void raftAppliedEntryFree(void *val) {
    raftAppliedEntry *ae = val;
    sdsfree(ae->value);
    zfree(ae);
}

static void clusterRaftAppliedEntryDictEntryDestructor(void *entry) {
    dictEntry *de = entry;
    dictSdsDestructor(dictGetKey(de));
    raftAppliedEntryFree(dictGetVal(de));
    zfree(de);
}

dictType clusterRaftAppliedEntryDictType = {
    .entryGetKey = dictEntryGetKey,
    .hashFunction = dictSdsHash,
    .keyCompare = dictSdsKeyCompare,
    .entryDestructor = clusterRaftAppliedEntryDictEntryDestructor,
};

static const char *raftRoleToString(int role) {
    switch (role) {
    case RAFT_ROLE_FOLLOWER: return "FOLLOWER";
    case RAFT_ROLE_CANDIDATE: return "CANDIDATE";
    case RAFT_ROLE_LEADER: return "LEADER";
    case RAFT_ROLE_HANDSHAKING: return "HANDSHAKING";
    case RAFT_ROLE_NON_MEMBER: return "NON_MEMBER";
    case RAFT_ROLE_JOINING: return "JOINING";
    default: return "UNKNOWN";
    }
}

static void clusterRaftSetNodeRole(clusterNode *node, int new_role) {
    if (RAFT_DATA(node)->role == new_role) return;
    serverLog(LL_NOTICE, "Raft: Node %.40s changing role: %s -> %s", node->name, raftRoleToString(RAFT_DATA(node)->role), raftRoleToString(new_role));
    RAFT_DATA(node)->role = new_role;

    /* Keep the CLUSTER_NODE_* flags in sync */
    int flags = node->flags;
    if (new_role == RAFT_ROLE_HANDSHAKING || new_role == RAFT_ROLE_NON_MEMBER) {
        flags |= CLUSTER_NODE_HANDSHAKE;
    } else {
        flags &= ~CLUSTER_NODE_HANDSHAKE;
    }
    node->flags = flags;
}

void clusterRaftInit(void) {
    server.cluster->protocol_data = zcalloc(sizeof(clusterRaftState));
    RAFT_STATE()->enabled = server.cluster_raft_enabled;
    RAFT_STATE()->applied_entries = dictCreate(&clusterRaftAppliedEntryDictType);
    RAFT_STATE()->term = 0;
}

static void clusterRaftInitLast(void) {
    /* Initialize fields that depend on server.cluster->myself */
    RAFT_STATE()->leader = server.cluster->myself;
    clusterRaftSetNodeRole(server.cluster->myself, RAFT_ROLE_LEADER);

    /* Bootstrap our metadata with just the membership information of our 1-node
     * group.*/
    raftAppliedEntry *ae = zmalloc(sizeof(*ae));
    ae->index = 1;
    ae->term = 0;
    ae->value = sdsnewlen(server.cluster->myself->name, CLUSTER_NAMELEN);
    dictAdd(RAFT_STATE()->applied_entries, sdsnew("membership"), ae);
    RAFT_STATE()->last_applied_index = 1;
    RAFT_STATE()->last_applied_term = 0;

    clusterListenerInit();
}

void clusterRaftCron(void) {
    clusterRaftStateMachine();
}

static void clusterRaftBeforeSleep(void) {
}

static void clusterRaftHandleServerShutdown(bool auto_failover) {
    UNUSED(auto_failover);
}

static uint32_t clusterRaftValidateMessageHeader(char *header) {
    clusterRaftHeader *hdr = (clusterRaftHeader *)header;
    if (memcmp(hdr->sig, "VCv2", 4) != 0) return 0;
    return ntohl(hdr->totlen);
}

static int clusterRaftProcessMessage(clusterLink *link) {
    clusterRaftHeader *hdr = (clusterRaftHeader *)link->rcvbuf;
    uint16_t type = ntohs(hdr->type);
    void *payload = link->rcvbuf + sizeof(clusterRaftHeader);
    clusterNode *sender = link->node;
    if (!sender) {
        if (type != CLUSTERMSG_TYPE_RAFT_HANDSHAKE_REQUEST) {
            serverLog(LL_WARNING, "Raft: Unexpected message type %d from unknown sender", type);
            return 0;
        }
    }

    switch (type) {
    case CLUSTERMSG_TYPE_RAFT_HANDSHAKE_REQUEST:
    case CLUSTERMSG_TYPE_RAFT_HANDSHAKE_REPLY:
        clusterRaftProcessHandshake(link, type, (clusterMsgRaftHandshake *)payload);
        break;
    default:
        serverLog(LL_WARNING, "Unknown Raft message type %d", type);
        return 0;
    }
    return 1;
}

static void clusterRaftPostConnect(clusterLink *link) {
    serverAssert(link->node != NULL);
    serverLog(LL_DEBUG, "Raft: Sending handshake to %.40s after connection established", clusterLinkGetNodeName(link));

    clusterMsgSendBlock *msgblock = createClusterRaftMsgSendBlock(CLUSTERMSG_TYPE_RAFT_HANDSHAKE_REQUEST, sizeof(clusterMsgRaftHandshake));
    clusterMsgRaftHandshake *req = (clusterMsgRaftHandshake *)((char *)msgblock->data + sizeof(clusterRaftHeader));

    memcpy(req->sender_name, myself->name, CLUSTER_NAMELEN);
    req->term = htonu64(RAFT_STATE()->term);
    memcpy(req->remote_ip, link->node->ip, NET_IP_STR_LEN);
    req->plaintext_port = htons(server.port);
    req->tls_port = htons(server.tls_port);
    req->cluster_port = htons(myself->cport);

    clusterLinkSendBlock(link, msgblock);
    clusterMsgSendBlockDecrRefCount(msgblock);
    clusterRaftStateMachine();
}

static void clusterRaftOnMyselfUpdated(int old_flags) {
    UNUSED(old_flags);
}

static void clusterRaftPropagatePublish(robj *channel, robj *message, int sharded) {
    UNUSED(channel);
    UNUSED(message);
    UNUSED(sharded);
}

static int clusterRaftSendModuleMessage(const char *target, uint64_t module_id, uint8_t type, const char *payload, uint32_t len) {
    UNUSED(target);
    UNUSED(module_id);
    UNUSED(type);
    UNUSED(payload);
    UNUSED(len);
    return 0;
}

static unsigned long clusterRaftGetConnectionsCount(void) {
    return 0;
}

static void clusterRaftResetStats(void) {
}

static sds clusterRaftAppendInfoFields(sds info) {
    if (server.cluster == NULL) return info;

    info = sdscatfmt(info, "raft_node_count:%U\r\n", (unsigned long long)dictSize(server.cluster->nodes));
    info = sdscatfmt(info, "raft_role:%s\r\n", raftRoleToString(RAFT_DATA(myself)->role));
    info = sdscatprintf(info, "raft_node_id:%.40s\r\n", server.cluster->myself->name);
    info = sdscatprintf(info, "raft_term:%llu\r\n", (unsigned long long)RAFT_STATE()->term);
    info = sdscatprintf(info, "raft_leader:%.40s\r\n", RAFT_STATE()->leader ? RAFT_STATE()->leader->name : "");
    info = sdscatprintf(info, "raft_applied_key_count:%zu\r\n", dictSize(RAFT_STATE()->applied_entries));
    info = sdscatprintf(info, "raft_applied_index:%llu\r\n", (unsigned long long)RAFT_STATE()->last_applied_index);

    return info;
}

static int clusterRaftGetFailureReportsCount(clusterNode *node) {
    UNUSED(node);
    return 0;
}

static void clusterRaftGetNodePingPongEpoch(clusterNode *node, long long *ping_sent, long long *pong_received, uint64_t *config_epoch) {
    UNUSED(node);
    *ping_sent = 0;
    *pong_received = 0;
    *config_epoch = 0;
}

static void clusterRaftSetNodePingPongEpoch(clusterNode *node, int ping_active, int pong_active, uint64_t config_epoch) {
    UNUSED(node);
    UNUSED(ping_active);
    UNUSED(pong_active);
    UNUSED(config_epoch);
}

static void clusterRaftSetNodeFailed(clusterNode *node) {
    UNUSED(node);
}

static sds clusterRaftAppendVarsLine(sds config) {
    return config;
}

static int clusterRaftParseVarsLine(const char *name, const char *value) {
    UNUSED(name);
    UNUSED(value);
    return 0;
}

static void clusterRaftPostLoad(void) {
}

void clusterRaftInitNodeData(clusterNode *node) {
    node->protocol_data = zcalloc(sizeof(clusterNodeRaftData));
    clusterRaftSetNodeRole(node, RAFT_ROLE_HANDSHAKING);
}

void clusterRaftFreeNodeData(clusterNode *node) {
    if (node->protocol_data) {
        zfree(node->protocol_data);
        node->protocol_data = NULL;
    }
}

static void clusterRaftSlotChange(slotRange *ranges, int numranges, clusterNode *target, void *ctx, void (*callback)(void *ctx, const char *error)) {
    UNUSED(ranges);
    UNUSED(numranges);
    UNUSED(target);
    if (callback) callback(ctx, NULL);
}

static void clusterRaftCancelManualFailover(void) {
}

static void clusterRaftCancelAutomaticFailover(void) {
}

static void clusterRaftForgetNode(const char *node_id, size_t id_len, void *ctx, void (*callback)(void *ctx, const char *error)) {
    UNUSED(node_id);
    UNUSED(id_len);
    if (callback) callback(ctx, NULL);
}

static void clusterRaftSetReplicaOf(clusterNode *primary, void *ctx, void (*callback)(void *ctx, const char *error)) {
    UNUSED(primary);
    if (callback) callback(ctx, "Not supported in Raft mode");
}

static void clusterRaftFailover(int force, int takeover, void *ctx, void (*callback)(void *ctx, const char *error)) {
    UNUSED(force);
    UNUSED(takeover);
    if (callback) callback(ctx, "Not supported in Raft mode");
}

static void clusterRaftMeet(const char *ip, int port, int cport, void *ctx, void (*callback)(void *ctx, const char *error)) {
    char norm_ip[NET_IP_STR_LEN];
    struct sockaddr_storage sa;
    if (inet_pton(AF_INET, ip, &(((struct sockaddr_in *)&sa)->sin_addr))) {
        sa.ss_family = AF_INET;
    } else if (inet_pton(AF_INET6, ip, &(((struct sockaddr_in6 *)&sa)->sin6_addr))) {
        sa.ss_family = AF_INET6;
    } else {
        if (callback) callback(ctx, "Invalid node address specified");
        return;
    }
    inet_ntop(sa.ss_family, sa.ss_family == AF_INET ? (void *)&(((struct sockaddr_in *)&sa)->sin_addr) : (void *)&(((struct sockaddr_in6 *)&sa)->sin6_addr), norm_ip, NET_IP_STR_LEN);

    clusterNode *n = createClusterNode(NULL, CLUSTER_NODE_HANDSHAKE);
    memcpy(n->ip, norm_ip, sizeof(n->ip));
    if (server.tls_cluster) {
        n->tls_port = port;
    } else {
        n->tcp_port = port;
    }
    n->cport = cport;
    clusterAddNode(n);

    serverLog(LL_DEBUG, "Raft: Initiating outbound connection to %s:%d", norm_ip, port);

    clusterLink *link = createClusterLink(n);
    link->conn = connCreate(connTypeOfCluster());
    connSetPrivateData(link->conn, link);
    if (connConnect(link->conn, n->ip, n->cport, server.bind_source_addr, 0, clusterLinkConnectHandler) == C_ERR) {
        serverLog(LL_WARNING, "Raft: Failed to connect to %s:%d", n->ip, n->cport);
        freeClusterLink(link);
        if (callback) callback(ctx, "Failed to initiate connection");
        return;
    }

    if (callback) callback(ctx, NULL);
}

static void clusterRaftReset(int hard) {
    UNUSED(hard);
}

static int clusterRaftProtocolSubcommand(client *c) {
    UNUSED(c);
    return 0;
}

void clusterRaftFree(void) {
    if (RAFT_STATE()) {
        dictRelease(RAFT_STATE()->applied_entries);
        zfree(RAFT_STATE());
        server.cluster->protocol_data = NULL;
    }
}

/* Handshake related functions */

clusterMsgSendBlock *createClusterRaftMsgSendBlock(int type, uint32_t payload_len) {
    uint32_t msglen = sizeof(clusterRaftHeader) + payload_len;
    uint32_t blocklen = sizeof(clusterMsgSendBlock) + msglen;
    clusterMsgSendBlock *msgblock = zcalloc(blocklen);
    msgblock->refcount = 1;
    msgblock->totlen = blocklen;
    msgblock->len = msglen;

    clusterRaftHeader *hdr = (clusterRaftHeader *)msgblock->data;
    memcpy(hdr->sig, "VCv2", 4);
    hdr->totlen = htonl(msglen);
    hdr->ver = htons(1);
    hdr->type = htons(type);

    return msgblock;
}

void clusterRaftConnectToNode(clusterNode *node) {
    if (node->link) return;
    clusterLink *link = createClusterLink(node);
    link->conn = connCreate(connTypeOfCluster());
    connSetPrivateData(link->conn, link);
    if (connConnect(link->conn, node->ip, node->cport, server.bind_source_addr, 0, clusterLinkConnectHandler) == C_ERR) {
        serverLog(LL_WARNING, "Raft: Failed to initiate connection to %.40s", node->name);
        freeClusterLink(link);
    }
}

void clusterRaftSendHandshakeReply(clusterLink *link, const char *node_name) {
    serverLog(LL_DEBUG, "Raft: Sending handshake response to %.40s", node_name);

    clusterMsgSendBlock *msgblock = createClusterRaftMsgSendBlock(CLUSTERMSG_TYPE_RAFT_HANDSHAKE_REPLY, sizeof(clusterMsgRaftHandshake));
    clusterMsgRaftHandshake *resp = (clusterMsgRaftHandshake *)((char *)msgblock->data + sizeof(clusterRaftHeader));
    memcpy(resp->sender_name, server.cluster->myself->name, CLUSTER_NAMELEN);
    char ip[NET_IP_STR_LEN] = {0};
    if (nodeIp2String(ip, link, "") == C_OK) {
        memcpy(resp->remote_ip, ip, NET_IP_STR_LEN);
    }
    resp->plaintext_port = htons(server.port);
    resp->tls_port = htons(server.tls_port);
    resp->cluster_port = htons(myself->cport);

    clusterLinkSendBlock(link, msgblock);
    clusterMsgSendBlockDecrRefCount(msgblock);
}

void clusterRaftProcessHandshake(clusterLink *link, uint16_t type, clusterMsgRaftHandshake *req) {
    char *sender_name = req->sender_name;
    char *remote_ip = req->remote_ip;

    /* Set my IP if I don't know it */
    if (myself->ip[0] == '\0' && remote_ip[0] != '\0') {
        valkey_strlcpy(myself->ip, remote_ip, NET_IP_STR_LEN);
        char *colon = strchr(myself->ip, ':');
        if (colon) *colon = '\0';
        serverLog(LL_VERBOSE, "Raft: Discovered my IP: %s", myself->ip);
    }

    if (verifyClusterNodeId(sender_name, CLUSTER_NAMELEN) != C_OK) {
        serverLog(LL_WARNING, "Raft: Received handshake with invalid sender name");
        return;
    }

    clusterNode *sender = clusterLookupNode(sender_name, CLUSTER_NAMELEN);
    if (!sender) {
        if (link->node && (link->node->flags & CLUSTER_NODE_HANDSHAKE)) {
            /* Rename the handshake node to the real name */
            clusterRenameNode(link->node, sender_name);
            sender = link->node;
        } else {
            /* Create new node if we don't have a handshake node for it */
            sender = createClusterNode(sender_name, CLUSTER_NODE_HANDSHAKE);
            clusterAddNode(sender);
            nodeIp2String(sender->ip, link, "");
            serverLog(LL_DEBUG, "Raft: Created handshake node for %.40s", sender_name);
        }
    }

    sender->tcp_port = ntohs(req->plaintext_port);
    sender->tls_port = ntohs(req->tls_port);
    sender->cport = ntohs(req->cluster_port);

    if (link->node && (link->node->flags & CLUSTER_NODE_HANDSHAKE)) {
        if (link->node != sender) {
            serverLog(LL_DEBUG, "Raft: Moving link from handshake node %.40s to real node %.40s", link->node->name, sender->name);
            clusterNode *old_node = link->node;
            old_node->link = NULL;
            sender->link = link;
            link->node = sender;

            /* Delete the old temporary node */
            clusterDelNode(old_node);
        }
    } else if (!link->node) {
        setClusterNodeToInboundClusterLink(sender, link);
        if (!sender->link) {
            clusterRaftConnectToNode(sender);
        }
    }

    RAFT_DATA(sender)->member_count = ntohl(req->member_count);
    clusterRaftStateMachine();

    if (type == CLUSTERMSG_TYPE_RAFT_HANDSHAKE_REQUEST) {
        clusterRaftSendHandshakeReply(link, sender_name);
    }
}

void clusterRaftStateMachine(void) {
    if (server.cluster == NULL || !RAFT_STATE()) return;

    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    while ((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        if (node == server.cluster->myself) continue;
        if (!RAFT_DATA(node)) continue;

        /* Handshake timeout */
        if (node->flags & CLUSTER_NODE_HANDSHAKE) {
            mstime_t timeout = server.cluster_node_timeout > 1000 ? server.cluster_node_timeout : 1000;
            if (mstime() - node->ctime > timeout) {
                serverLog(LL_WARNING, "Raft: Handshake timeout for node %.40s", node->name);
                clusterDelNode(node);
                continue;
            }
        }

        switch (RAFT_DATA(node)->role) {
        case RAFT_ROLE_HANDSHAKING:
            if (node->link && connGetState(node->link->conn) == CONN_STATE_CONNECTED && node->inbound_link != NULL) {
                clusterRaftSetNodeRole(node, RAFT_ROLE_NON_MEMBER);
            }
            break;
        default:
            break;
        }
    }
    dictReleaseIterator(di);
}

void clusterRaftGetMetadata(client *c) {
    UNUSED(c);
}

void clusterRaftSetMetadata(client *c) {
    UNUSED(c);
}

clusterBusType clusterRaftBus = {
    .init = clusterRaftInit,
    .initLast = clusterRaftInitLast,
    .cron = clusterRaftCron,
    .beforeSleep = clusterRaftBeforeSleep,
    .handleServerShutdown = clusterRaftHandleServerShutdown,
    .validateMessageHeader = clusterRaftValidateMessageHeader,
    .processMessage = clusterRaftProcessMessage,
    .postConnect = clusterRaftPostConnect,
    .onMyselfUpdated = clusterRaftOnMyselfUpdated,
    .propagatePublish = clusterRaftPropagatePublish,
    .sendModuleMessage = clusterRaftSendModuleMessage,
    .getConnectionsCount = clusterRaftGetConnectionsCount,
    .resetStats = clusterRaftResetStats,
    .appendInfoFields = clusterRaftAppendInfoFields,
    .getFailureReportsCount = clusterRaftGetFailureReportsCount,
    .getNodePingPongEpoch = clusterRaftGetNodePingPongEpoch,
    .setNodePingPongEpoch = clusterRaftSetNodePingPongEpoch,
    .setNodeFailed = clusterRaftSetNodeFailed,
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
    .resetCluster = clusterRaftReset,
    .protocolSubcommand = clusterRaftProtocolSubcommand,
};
