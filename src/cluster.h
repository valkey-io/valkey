#ifndef __CLUSTER_H
#define __CLUSTER_H

#include <stdbool.h>

typedef struct slotRange {
    int start_slot;
    int end_slot;
} slotRange;
/*-----------------------------------------------------------------------------
 * Cluster exported API.
 *----------------------------------------------------------------------------*/

#define CLUSTER_SLOT_MASK_BITS 14                   /* Number of bits used for slot id. */
#define CLUSTER_SLOTS (1 << CLUSTER_SLOT_MASK_BITS) /* Total number of slots in cluster mode, which is 16384. */
#define CLUSTER_OK 0                                /* Everything looks ok */
#define CLUSTER_FAIL 1                              /* The cluster can't work */
#define CLUSTER_NAMELEN 40                          /* sha1 hex length */

/* Reason why the cluster state changes to fail. When adding new reasons,
 * make sure to update clusterLogFailReason. */
#define CLUSTER_FAIL_NONE 0
#define CLUSTER_FAIL_NOT_FULL_COVERAGE 1
#define CLUSTER_FAIL_MINORITY_PARTITION 2

/* Redirection errors returned by getNodeByQuery(). */
#define CLUSTER_REDIR_NONE 0          /* Node can serve the request. */
#define CLUSTER_REDIR_CROSS_SLOT 1    /* -CROSSSLOT request. */
#define CLUSTER_REDIR_UNSTABLE 2      /* -TRYAGAIN redirection required */
#define CLUSTER_REDIR_ASK 3           /* -ASK redirection required. */
#define CLUSTER_REDIR_MOVED 4         /* -MOVED redirection required. */
#define CLUSTER_REDIR_DOWN_STATE 5    /* -CLUSTERDOWN, global state. */
#define CLUSTER_REDIR_DOWN_UNBOUND 6  /* -CLUSTERDOWN, unbound slot. */
#define CLUSTER_REDIR_DOWN_RO_STATE 7 /* -CLUSTERDOWN, allow reads. */

/* Fixed timeout value for cluster operations (milliseconds) */
#define CLUSTER_OPERATION_TIMEOUT 2000

/* Manual failover pause multiplier: pause time = timeout * PAUSE_MULT. */
#define CLUSTER_MF_PAUSE_MULT 2

typedef struct clusterNode clusterNode;
struct clusterState;

/* Flags that a module can set in order to prevent certain Cluster
 * features to be enabled. Useful when implementing a different distributed
 * system on top of Cluster message bus, using modules. */
#define CLUSTER_MODULE_FLAG_NONE 0
#define CLUSTER_MODULE_FLAG_NO_FAILOVER (1 << 1)
#define CLUSTER_MODULE_FLAG_NO_REDIRECTION (1 << 2)

/* ---------------------- API exported outside cluster.c -------------------- */
/* functions requiring mechanism specific implementations */
void clusterInit(void);
void clusterInitLast(void);
void clusterCron(void);
void clusterBeforeSleep(void);
int verifyClusterConfigWithData(void);
void clusterHandleServerShutdown(bool auto_failover);

int clusterSendModuleMessageToTarget(const char *target,
                                     uint64_t module_id,
                                     uint8_t type,
                                     const char *payload,
                                     uint32_t len);

void clusterUpdateMyselfFlags(void);
void clusterUpdateMyselfIp(void);
void clusterUpdateMyselfClientIpV4(void);
void clusterUpdateMyselfClientIpV6(void);
void clusterUpdateMyselfHostname(void);
void clusterUpdateMyselfAnnouncedPorts(void);
void clusterUpdateMyselfHumanNodename(void);
void clusterUpdateMyselfAvailabilityZone(void);

void clusterPropagatePublish(robj *channel, robj *message, int sharded);

unsigned long getClusterConnectionsCount(void);
int isClusterHealthy(void);

sds genClusterInfoString(sds info);
/* handle implementation specific debug cluster commands. Return 1 if handled, 0 otherwise. */
int handleDebugClusterCommand(client *c);
const char **clusterDebugCommandExtendedHelp(void);

int clusterAllowFailoverCmd(client *c);

void clusterCommandSlots(client *c);
void clusterCommandMyId(client *c);
void clusterCommandMyShardId(client *c);
void clusterCommandShards(client *c);

int clusterNodeCoversSlot(clusterNode *n, int slot);
int getNodeDefaultClientPort(clusterNode *n);
clusterNode *getMyClusterNode(void);
int getClusterSize(void);
int getMyShardSlotCount(void);
int clusterNodePending(clusterNode *node);
int clusterNodeIsPrimary(clusterNode *n);
int clusterNodeIsVotingPrimary(clusterNode *n);
char **getClusterNodesList(size_t *numnodes);
char *clusterNodeIp(clusterNode *node, client *c);
int clusterNodeIsReplica(clusterNode *node);
clusterNode *clusterNodeGetPrimary(clusterNode *node);
char *clusterNodeGetName(clusterNode *node);
int clusterNodeTimedOut(clusterNode *node);
int clusterNodeIsFailing(clusterNode *node);
int clusterNodeIsNoFailover(clusterNode *node);
char *clusterNodeGetShardId(clusterNode *node);
int clusterNodeNumReplicas(clusterNode *node);
clusterNode *clusterNodeGetReplica(clusterNode *node, int replica_idx);
clusterNode *getMigratingSlotDest(int slot);
clusterNode *getImportingSlotSource(int slot);
clusterNode *getNodeBySlot(int slot);
int clusterNodeClientPort(clusterNode *n, int use_tls, client *c);
char *clusterNodeHostname(clusterNode *node);
const char *clusterNodePreferredEndpoint(clusterNode *n, client *c);
clusterNode *clusterLookupNode(const char *name, int length);
client *createCachedResponseClient(int resp);
void deleteCachedResponseClient(client *recording_client);
void clearCachedClusterSlotsResponse(void);
unsigned int countKeysInSlotForDb(unsigned int hashslot, serverDb *db);
unsigned int countKeysInSlot(unsigned int hashslot);
unsigned int countChannelsInSlot(unsigned int hashslot);
void removeChannelsInSlot(unsigned int slot);
void removeAllNotOwnedShardChannelSubscriptions(void);
int getSlotOrReply(client *c, robj *o);
int getNodeDefaultReplicationPort(clusterNode *node);

/* functions with shared implementations */
int clusterNodeIsMyself(clusterNode *n);
int clusterSlotByCommand(struct serverCommand *cmd, robj **argv, int argc, int *read_flags);
clusterNode *getNodeByQuery(client *c, int *error_code);
int clusterRedirectBlockedClientIfNeeded(client *c);
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code);
void migrateCloseTimedoutSockets(void);
unsigned int keyHashSlot(const char *key, int keylen);
int patternHashSlot(char *pattern, int length);
int isValidAuxString(char *s, unsigned int length);
int verifyClusterNodeId(const char *name, int length);
void migrateCommand(client *c);
void clusterCommand(client *c);
void clusterKeySlotCommand(client *c);
ConnectionType *connTypeOfCluster(void);
long long getNodeReplicationOffset(clusterNode *node);
sds aggregateClientOutputBuffer(client *c);
void resetClusterStats(void);
unsigned int delKeysInSlot(unsigned int hashslot, int lazy, bool propagate_del, bool send_del_event);

unsigned int propagateSlotDeletionByKeys(unsigned int hashslot);
void clusterLogFailReason(int reason);
sds clusterEncodeOpenSlotsAuxField(int rdbflags);
int clusterDecodeOpenSlotsAuxField(int rdbflags, sds s);

/* Change slot ownerships. See clusterBusType.slotChange. */
void clusterSlotChange(slotRange *ranges, int numranges, clusterNode *target, void *ctx, void (*callback)(void *ctx, const char *error));
void clusterCancelManualFailover(void);
void clusterSetPrimary(clusterNode *n, int closeSlots, int full_sync_required);
void clusterScheduleHandleSlotMigration(void);
mstime_t clusterComputeMfPauseEnd(void);

#endif /* __CLUSTER_H */
