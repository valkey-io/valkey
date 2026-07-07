/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef THROTTLE_REPL_H
#define THROTTLE_REPL_H

#include "sds.h"

/* Replication throttle configuration. */
struct throttle_repl_config {
    int steady_state_repl_throttle_enabled;
};
extern struct throttle_repl_config throttle_repl_config;

/* Returns true if the client should be exempt from COB disconnect limits
 * because throttling is actively working to stabilize the replica. */
bool throttleRepl_isClientExemptFromCobLimits(client *c);

/* Determine throttling needs and adjust rate. Called from serverCron. */
void throttleRepl_adjustThrottling(void);

/* Add repl throttle metrics to INFO string. */
sds throttleRepl_sdscatMetrics(sds info);

#endif
