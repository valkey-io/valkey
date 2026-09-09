/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"

/* Flush the content of the aggregation table to the command histograms. */
static void latencyE2eFlushTable(latencyE2eTable *t, monotime write_time) {
    int64_t duration_ns = (int64_t)(write_time - t->cmd_read_time) * 1000;
    for (int i = 0; i < t->nslots; i++) {
        updateCommandLatencyE2eHistogram(&t->cmds[i]->latency_e2e_histogram, duration_ns, t->counts[i]);
    }
}

/* Flush the aggregation table, clear it & reset it. */
static void latencyE2eFlushTableAndReset(client *c, latencyE2eTable *table) {
    if (table->nslots > 0) {
        latencyE2eFlushTable(table, c->latency_e2e.cur_write_time);
        /* reset table for next cycle of commands. */
        memset(table->cmds, 0, sizeof(table->cmds));
        table->nslots = 0;
    }
    table->boundary = -1ULL;
}

/* Obtain a fresh table for end-to-end latency recording. If already at capacity, flush the oldest table early. */
static latencyE2eTable *latencyE2eOpenTable(client *c) {
    if (c->latency_e2e.tables == NULL) {
        c->latency_e2e.tables = listCreate();
        listSetFreeMethod(c->latency_e2e.tables, zfree);
    }

    /* Flush & reuse the oldest. */
    if (listLength(c->latency_e2e.tables) >= LATENCY_E2E_MAX_TABLES) {
        listNode *ln = listLast(c->latency_e2e.tables);
        latencyE2eTable *oldest = listNodeValue(ln);

        listUnlinkNode(c->latency_e2e.tables, ln);
        listLinkNodeHead(c->latency_e2e.tables, ln);
        latencyE2eFlushTableAndReset(c, oldest);
        oldest->cmd_read_time = c->latency_e2e.cur_cmd_time;
        c->latency_e2e.open = oldest;
        return oldest;
    }

    /* check if the top table went through latencyE2eFlushTableAndReset. */
    if (listLength(c->latency_e2e.tables) != 0) {
        latencyE2eTable *table = listNodeValue(listFirst(c->latency_e2e.tables));
        if (table->boundary == -1ULL) {
            table->cmd_read_time = c->latency_e2e.cur_cmd_time;
            c->latency_e2e.open = table;
            return table;
        }
    }

    latencyE2eTable *table = zcalloc(sizeof(*table));
    table->boundary = -1ULL;
    table->cmd_read_time = c->latency_e2e.cur_cmd_time;
    c->latency_e2e.open = table;
    listAddNodeHead(c->latency_e2e.tables, table);
    return table;
}

/* Add a sample of the command to the current table. */
void latencyE2eRecordCommand(client *c, struct serverCommand *cmd) {
    latencyE2eTable *table = c->latency_e2e.open;
    if (table == NULL || table->boundary != -1ULL) table = latencyE2eOpenTable(c);

    /* Aggregate into an existing slot for this command if present. */
    for (int i = 0; i < LATENCY_E2E_MAX_SLOTS; i++) {
        if (table->cmds[i] == cmd) {
            table->counts[i]++;
            return;
        }
        if (table->cmds[i] == NULL) {
            table->cmds[i] = cmd;
            table->counts[i] = 1;
            table->nslots++;
            return;
        }
    }

    /* If more than 9 unique command types per batch, the 9th+ command will be dropped silently from the tracking table. */
}

/* Flush and remove every table whose replies are fully written. */
void latencyE2eFlushCompleted(client *c) {
    if (listLength(c->latency_e2e.tables) == 1) {
        /* Fast path: single table, flushed in place & reuse. */
        latencyE2eTable *table = listNodeValue(listFirst(c->latency_e2e.tables));
        if (table->boundary != -1ULL && table->boundary <= c->latency_e2e.reply_blocks_removed) {
            latencyE2eFlushTableAndReset(c, table);
        }
    } else {
        listIter li;
        listRewindTail(c->latency_e2e.tables, &li);

        listNode *ln;
        while ((ln = listNext(&li))) {
            latencyE2eTable *table = listNodeValue(ln);
            if (table->boundary > c->latency_e2e.reply_blocks_removed) break;
            if (ln->next == NULL) {
                /* reuse last aggregation table to avoid future allocations. */
                latencyE2eFlushTableAndReset(c, table);
            } else if (table->boundary != -1ULL) {
                latencyE2eFlushTable(table, c->latency_e2e.cur_write_time);
                listDelNode(c->latency_e2e.tables, ln);
            }
        }
    }
}

/* Release all data related to end-to-end latency tracking.
 * Any samples still buffered (e.g. a client disconnected) are dropped. */
void latencyE2eRelease(client *c) {
    if (c->latency_e2e.tables == NULL) return;
    listRelease(c->latency_e2e.tables);
    c->latency_e2e.tables = NULL;
    c->latency_e2e.open = NULL;
}

/* After a client write: close the current aggregation table to edits and flush any table whose replies are fully written. */
void latencyE2ePostClientWrite(client *c) {
    if (c->latency_e2e.tables == NULL || c->nwritten <= 0) return;

    if (!server.latency_tracking_enable_e2e) {
        latencyE2eRelease(c);
        return;
    }

    if (c->latency_e2e.open != NULL && c->latency_e2e.open->nslots > 0) {
        latencyE2eTable *table = c->latency_e2e.open;
        table->boundary = c->latency_e2e.reply_block_watermark;
        /* Let the next command execution handle the creations of a new aggregation table. */
        c->latency_e2e.open = NULL;
    }

    latencyE2eFlushCompleted(c);
}

/* After a replica write: replicas are never tracked, so free any tables allocated before the role was known. */
void latencyE2ePostReplicaWrite(client *c) {
    if (c->latency_e2e.tables == NULL) return;

    /* Feature is not needed for replica clients, free memory. */
    latencyE2eRelease(c);
    /* Avoid future tracking allocations. */
    c->latency_e2e.cur_read_time = 0;
}

/* Called when a module is unloaded, before its registered commands are freed.
 * Aggregation tables hold raw serverCommand* values which might are freed during the module unloading. */
void latencyE2eModuleUnload(void) {
    listIter li;
    listNode *ln;
    listRewind(server.clients, &li);
    while ((ln = listNext(&li))) {
        latencyE2eRelease(listNodeValue(ln));
    }
}
