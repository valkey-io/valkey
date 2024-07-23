#include "server.h"
#include "cluster.h"
#include "script.h"
#include "cluster_legacy.h"

void clusterSlotStatReset(int slot);
void clusterSlotStatResetAll(void);
void clusterSlotStatsAddCpuDuration(client *c, ustime_t duration);
void clusterSlotStatsInvalidateSlotIfApplicable(scriptRunCtx *ctx);
void clusterSlotStatsAddNetworkBytesIn(client *c);
void clusterSlotStatsSetClusterMsgLength(uint32_t len);
void clusterSlotStatsResetClusterMsgLength(void);
void clusterSlotStatsAddNetworkBytesInForShardedPubSub(int slot);
