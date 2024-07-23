/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include "server.h"
#include "cluster.h"
#include "script.h"
#include "cluster_legacy.h"

void clusterSlotStatReset(int slot);
void clusterSlotStatResetAll(void);
void clusterSlotStatsAddCpuDuration(client *c, ustime_t duration);
void clusterSlotStatsInvalidateSlotIfApplicable(scriptRunCtx *ctx);
void clusterSlotStatsAddNetworkBytesIn(client *c);
void clusterSlotStatsAddNetworkBytesInForShardedPubSub(robj *channel, robj *message);
