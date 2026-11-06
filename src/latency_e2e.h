/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LATENCY_E2E_H
#define LATENCY_E2E_H

#include "server.h"

void latencyE2eFinalize(client *c);
void latencyE2eFlushTable(latencyE2eCounter *table, monotime read_time, monotime end_time);
void latencyE2eUpdateHistograms(void);
void latencyE2eDropSamples(void);
void latencyE2eReset(client *c);
void latencyE2eResetAllClients(void);

/* Stamp the read time of a client's next batch. */
static inline void latencyE2eRecordReadEvent(client *c) {
    int type = getClientType(c);
    if (c->flag.monitor || unlikely(type != CLIENT_TYPE_NORMAL && type != CLIENT_TYPE_PUBSUB)) {
        if (c->latency_e2e) c->latency_e2e->cur_read_time = 0;
    } else {
        if (unlikely(!c->latency_e2e)) c->latency_e2e = zcalloc(sizeof(latencyE2e));
        c->latency_e2e->cur_read_time = server.el->wakeup_time;
    }
}

/* Start a new batch of parsed commands. */
static inline void latencyE2eInitialize(client *c) {
    latencyE2e *l = c->latency_e2e;
    /* Untracked client. */
    if (!l) return;
    /* Same batch, (loop inside processInputBuffer). */
    if (l->current_read_time == l->cur_read_time) return;
    /* An earlier batch still not finalized. */
    if (unlikely(l->current.aggregated)) latencyE2eFinalize(c);
    l->current_read_time = l->cur_read_time;
}

/* Flush the pending table after a write. */
static inline void latencyE2ePostClientWrite(client *c) {
    if (server.latency_tracking_enable_e2e && c->nwritten > 0) {
        latencyE2e *l = c->latency_e2e;
        if (l && l->pending.aggregated) {
            latencyE2eFlushTable(&l->pending, l->pending_read_time, l->cur_write_time);
        }
    }
}

/* Add a sample of the command to the current table. */
static inline void latencyE2eRecordCommand(client *c, struct serverCommand *cmd) {
    c->latency_e2e->current.item[getCommandType(c, cmd)]++;
}

#endif
