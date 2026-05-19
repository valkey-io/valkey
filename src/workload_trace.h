/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __WORKLOAD_TRACE_H
#define __WORKLOAD_TRACE_H

#include "server.h"

/* Access types for workload trace events */
#define WORKLOAD_ACCESS_READ 0
#define WORKLOAD_ACCESS_WRITE 1
#define WORKLOAD_ACCESS_DELETE 2

/* Format types */
#define WORKLOAD_FORMAT_RESP 0
#define WORKLOAD_FORMAT_CSV 1

/* Per-client tracer config (set when MONITOR TRACE is issued) */
typedef struct workloadTracerConfig {
    int samples;                   /* -1=fast path, 0=sample all, >0=sample N (matches MEMORY USAGE) */
    int negative_lookups;          /* Include READ events where key doesn't exist */
    int format;                    /* WORKLOAD_FORMAT_RESP or WORKLOAD_FORMAT_CSV */
    double sample_rate;            /* 0.0–1.0, fraction of keys to trace (1.0 = all) */
    uint64_t sample_seed;          /* Seed for deterministic key-hash sampling */
    uint64_t sample_threshold;     /* Pre-computed: sample_rate * UINT64_MAX */
    uint8_t sample_seed_bytes[16]; /* siphash key derived from sample_seed */
} workloadTracerConfig;

/* Trace context set in call(), consumed by lookup/write/delete hooks */
typedef struct workloadTraceContext {
    const char *cmd_name; /* Current command name */
    uint64_t seq;         /* Monotonic sequence ID */
} workloadTraceContext;

/* Initialize/cleanup */
void workloadTraceInit(void);
void workloadTraceDetachClient(client *c);

/* MONITOR TRACE setup helper (called from monitorCommand) */
int monitorTraceSetup(client *c, int arg_start);

/* Trace context management (called from call()) */
workloadTraceContext workloadTraceSaveContext(void);
void workloadTraceBeginCommand(client *c);
void workloadTraceEndCommand(workloadTraceContext *prev);

/* Event emitters (called from db.c hot paths) */
void workloadTraceEmitRead(serverDb *db, robj *key, robj *val);
void workloadTraceEmitWrite(serverDb *db, robj *key, robj *val);
void workloadTraceEmitDelete(serverDb *db, robj *key, robj *val);

/* Fast-path check: are any tracers subscribed? */
static inline int workloadTraceActive(void) {
    return server.workload_tracers && listLength(server.workload_tracers) > 0;
}

#endif /* __WORKLOAD_TRACE_H */
