#include "server.h"
#include "cluster.h"
#include "cluster_state.h"
#include "cluster_link.h"
#include "cluster_bus.h"
#include "cluster_raft.h"

/* Access Raft protocol-specific state from the cluster. */
#define RAFT_STATE() ((clusterRaftState *)server.cluster->protocol_data)

/* Access Raft protocol-specific data from a clusterNode. */
#define RAFT_DATA(n) ((clusterNodeRaftData *)(n)->protocol_data)

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
    default: return "UNKNOWN";
    }
}

static void clusterRaftSetNodeRole(clusterNode *node, int new_role) {
    serverLog(LL_NOTICE, "Raft: Node %.40s changing role: %s -> %s", node->name, raftRoleToString(RAFT_DATA(node)->role), raftRoleToString(new_role));
    RAFT_DATA(node)->role = new_role;
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
}

static void clusterRaftBeforeSleep(void) {
}

static void clusterRaftHandleServerShutdown(bool auto_failover) {
    UNUSED(auto_failover);
}

static uint32_t clusterRaftValidateMessageHeader(char *header) {
    UNUSED(header);
    return 0;
}

static int clusterRaftProcessMessage(clusterLink *link) {
    UNUSED(link);
    return 1;
}

static void clusterRaftPostConnect(clusterLink *link) {
    UNUSED(link);
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
    UNUSED(ip);
    UNUSED(port);
    UNUSED(cport);
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
