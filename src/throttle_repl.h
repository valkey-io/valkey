/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef THROTTLE_REPL_H
#define THROTTLE_REPL_H

#include "sds.h"

/* Returns true if replication throttle is enabled. */
bool throttleRepl_isEnabled(void);

/* Get the COB target size for steady-state throttling. */
unsigned long throttleRepl_getCobTargetSize(void);

/* Returns true if the client should be exempt from COB disconnect limits
 * because throttling is actively working to stabilize the replica. */
bool throttleRepl_isClientExempt(client *c);

/* Determine throttling needs and adjust rate. Called from serverCron. */
void throttleRepl_adjustThrottling(void);

/* Add repl throttle metrics to INFO string. */
sds throttleRepl_sdscatMetrics(sds info);

#endif /* THROTTLE_REPL_H */
