#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"

void clusterSlotStatReset(int slot);
void clusterSlotStatResetAll(void);
void clusterSlotStatsAddNetworkBytesOut(client *c);
void clusterSlotStatsAddNetworkBytesOutForReplication(int len);
void clusterSlotStatsAddNetworkBytesOutForShardedPubSub(client *c, int slot);
