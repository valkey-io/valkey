/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include "server.h"
#include "cluster.h"

void clusterSlotStatReset(int slot);
void clusterSlotStatsReset(void);
void clusterSlotStatsAddCpuDuration(int slot, long duration);