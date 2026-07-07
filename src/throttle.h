/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef THROTTLE_H
#define THROTTLE_H

#include "server.h"
#include <stdbool.h>

static const double THROTTLE_UNLIMITED_RATE = 10000000.0;
static const int THROTTLE_INVALID_ID = -2;

typedef bool throttleCriteriaProc(client *c, void *priv_data);

typedef struct {
    int num_clients;
    int total_throttled_commands;
    double ops_per_sec;
    double incoming_tps;
    long oldest_client_delay_us;
} throttleMetrics;

/* Framework-level metrics */
struct throttle_framework_metrics {
    long long total_throttled_commands;
};
extern struct throttle_framework_metrics throttle_framework_metrics;

/* Public API */
void throttle_init(void);

int throttle_register(throttleCriteriaProc *criteria_proc,
                      void *priv_data,
                      const char *metrics_name);

void throttle_deregister(int id);

void throttle_setRate(int id, double ops_per_sec);

double throttle_adjustRate(int id, double multiplier);

const throttleMetrics *throttle_getMetrics(const char *metrics_name);

void throttle_removeClient(client *c);

bool throttle_deferCommand(client *c);

sds throttle_sdscatMetrics(sds info);

long throttle_getGuardrailSecs(int id);

#endif
