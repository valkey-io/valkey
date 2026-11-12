/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A replication throttler plug-in for the generic throttler (throttle.h).
 *
 * Throttles client traffic on the primary to establish and maintain healthy replica
 * connections. It monitors replica COB (Client Output Buffer) growth and reduces the
 * command processing rate when needed.
 *
 * Steady-state evaluator (normal replication):
 * Throttles when the projected COB exceeds the target within the convergence window.
 * Rate increases automatically once COB stabilizes.
 */

#ifndef THROTTLE_REPL_H
#define THROTTLE_REPL_H

#include "sds.h"
struct throttleReplConfig {
    int repl_throttling_enabled;
};
extern struct throttleReplConfig throttleRepl_config;

/* Returns true if the client should be exempt from COB disconnect limits because throttling
 * is actively working to stabilize the replica. */
bool throttleRepl_isClientExemptFromCobLimits(client *c);

/* Determine throttling needs and adjust rate. Called from serverCron every 100ms. */
void throttleRepl_adjustThrottling(void);

/* Append replication throttle metrics to the INFO output string. */
sds throttleRepl_sdscatInfoMetrics(sds info);

/* Append verbose debug replication throttle metrics to the INFO output string. */
sds throttleRepl_sdscatInfoDebugMetrics(sds info);

#endif
