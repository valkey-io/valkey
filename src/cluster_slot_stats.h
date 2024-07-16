#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"

void clusterSlotStatReset(int slot);
void clusterSlotStatResetAll(void);
void clusterSlotStatsAddNetworkBytesOutForUserClient(client *c);
void clusterSlotStatsAddNetworkBytesOutForReplication(int len);
void clusterSlotStatsAddNetworkBytesOutForShardedPubSubInternalPropagation(client *c, int slot);
void clusterSlotStatsAddNetworkBytesOutForShardedPubSubExternalPropagation(size_t len);
