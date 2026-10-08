/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "latency_e2e.h"

/* Names of the command kinds (CMD_KIND_*), as reported in INFO and LATENCY E2E_HISTOGRAM. */
const char *cmdKindNames[CMD_KIND_TOTAL] = {"write", "read", "auth", "other"};

/* Samples are patched together across clients as that helps avoiding stalls related to cache misses. */
#define LATENCY_E2E_SAMPLES 256
static struct {
    uint64_t duration_us;
    latencyE2eCounter counter;
} latency_e2e_samples[LATENCY_E2E_SAMPLES];
static int latency_e2e_samples_count = 0;

/* Finalize the current table. */
void latencyE2eFinalize(client *c) {
    latencyE2e *l = c->latency_e2e;

    /* Untracked, or no samples. */
    if (!l || l->current.aggregated == 0) return;

    /* The previous pending table is still waiting for its write, flush it in place. */
    if (l->pending.aggregated) latencyE2eFlushTable(&l->pending, l->pending_read_time, getMonotonicUs());

    l->pending = l->current;
    l->pending_read_time = l->current_read_time;
    /* current_read_time is kept: commands of the current batch may still be recorded (e.g.
     * commands queued after a blocking command). */
    l->current.aggregated = 0;
}

/* Flush the table into the samples waiting for their histogram update, and clear it. */
void latencyE2eFlushTable(latencyE2eCounter *table, monotime read_time, monotime end_time) {
    if (unlikely(latency_e2e_samples_count == LATENCY_E2E_SAMPLES)) latencyE2eUpdateHistograms();
    latency_e2e_samples[latency_e2e_samples_count].duration_us = end_time - read_time;
    latency_e2e_samples[latency_e2e_samples_count].counter = *table;
    latency_e2e_samples_count++;
    table->aggregated = 0;
}

/* Record the waiting samples into the kind histograms. */
void latencyE2eUpdateHistograms(void) {
    struct hdr_histogram **histograms = server.latency_e2e_histogram;
    for (int j = 0; j < latency_e2e_samples_count; j++) {
        int64_t duration_ns = latency_e2e_samples[j].duration_us * 1000;
        latencyE2eCounter counter = latency_e2e_samples[j].counter;
        for (int i = 0; i < CMD_KIND_TOTAL; i++) {
            if (counter.item[i]) updateCommandLatencyHistogramCount(&histograms[i], duration_ns, counter.item[i]);
        }
    }
    latency_e2e_samples_count = 0;
}

/* Drop the samples waiting for their histogram update. */
void latencyE2eDropSamples(void) {
    latency_e2e_samples_count = 0;
}

/* Drop the client's buffered samples and read/write times. */
void latencyE2eReset(client *c) {
    if (c->latency_e2e) memset(c->latency_e2e, 0, sizeof(*c->latency_e2e));
}

/* Drop the tracking state of every client. */
void latencyE2eResetAllClients(void) {
    listIter li;
    listNode *ln;
    listRewind(server.clients, &li);
    while ((ln = listNext(&li))) latencyE2eReset(listNodeValue(ln));
}
