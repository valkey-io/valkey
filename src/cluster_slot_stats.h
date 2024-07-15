#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"

void clusterSlotStatReset(int slot);
void clusterSlotStatResetAll(void);
void clusterSlotStatsAddCpuDuration(client *c, ustime_t duration);
