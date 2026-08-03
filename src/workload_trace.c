/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* workload_trace.c - MONITOR TRACE: key-level access tracing for workload analysis.
 *
 * Streams key-level access events (read, write, delete) to subscribed clients,
 * enabling offline simulation of cache eviction policies and workload analysis.
 *
 * Architecture:
 *   call()          -> workloadTraceBeginCommand/EndCommand (sets trace context)
 *   lookupKey()     -> workloadTraceEmitRead  (key access)
 *   setKey/dbAdd()  -> workloadTraceEmitWrite (key mutation)
 *   dbDelete()      -> workloadTraceEmitDelete
 */

#include "workload_trace.h"

/* File-scoped trace state (safe: command execution is single-threaded). */
static workloadTraceContext trace_ctx = {0};
static uint64_t trace_seq_counter = 0;

/* Initialize the workload tracers list. Called once during server startup. */
void workloadTraceInit(void) {
    server.workload_tracers = listCreate();
}

/* Remove a client from the workload tracers list and free its config.
 * Safe to call even if the client is not a tracer. */
void workloadTraceDetachClient(client *c) {
    if (!c->flag.workload_tracer) return;
    listNode *ln = listSearchKey(server.workload_tracers, c);
    if (ln) listDelNode(server.workload_tracers, ln);
    zfree(c->workload_tracer_config);
    c->workload_tracer_config = NULL;
    c->flag.workload_tracer = 0;
}

/* --- Trace context management --- */

/* Save the current command name and increment the sequence counter.
 * Called from call() before command execution. No-op if no tracers active. */
void workloadTraceBeginCommand(client *c) {
    if (!workloadTraceActive()) return;
    trace_ctx.cmd_name = c->cmd ? c->cmd->fullname : "unknown";
    trace_ctx.seq = ++trace_seq_counter;
}

/* Restore the previous trace context. Called from call() after command execution.
 * Supports nested command execution (modules, scripts).
 * Preserves the current seq to maintain monotonicity across nested calls. */
void workloadTraceEndCommand(workloadTraceContext *prev) {
    uint64_t current_seq = trace_ctx.seq;
    trace_ctx = *prev;
    trace_ctx.seq = current_seq;
}

/* Capture the current trace context for later restoration. */
workloadTraceContext workloadTraceSaveContext(void) {
    return trace_ctx;
}

/* --- Size computation --- */

/* Report the key entry allocation cost (zmalloc_size of the stored robj).
 * When the value is embedded (EMBSTR/hasembval), this includes everything.
 * When external (RAW), this is the robj + embedded key allocation only. */
static size_t computeKeyBytes(robj *val) {
    return zmalloc_size(val);
}

/* Report the value size. Returns 0 for embedded values (already in key_bytes).
 * For external strings: sdsAllocSize of the separate SDS.
 * For complex types: objectComputeSize minus the robj allocation when samples >= 0.
 * samples=-1 means fast path (zmalloc_size only, no deep traversal).
 * samples=0 means sample all (matches MEMORY USAGE SAMPLES 0 semantics). */
static size_t computeValueBytes(robj *key, robj *val, int samples, int dbid) {
    if (val == NULL) return 0;
    if (val->hasembval) return 0; /* Value embedded in the robj allocation */
    if (val->type == OBJ_STRING) {
        if (val->encoding == OBJ_ENCODING_INT) return 0;
        return sdsAllocSize(objectGetVal(val));
    }
    /* Complex types */
    if (samples >= 0) {
        size_t sample_size = (samples == 0) ? LLONG_MAX : (size_t)samples;
        size_t total = objectComputeSize(key, val, sample_size, dbid);
        size_t overhead = zmalloc_size(val);
        return (total <= overhead) ? 0 : total - overhead;
    }
    /* samples=-1: fast path */
    return zmalloc_size(objectGetVal(val));
}

/* --- Event formatting and emission --- */

/* Return the string name for an access type constant. */
static const char *accessTypeName(int access_type) {
    switch (access_type) {
    case WORKLOAD_ACCESS_READ: return "READ";
    case WORKLOAD_ACCESS_WRITE: return "WRITE";
    case WORKLOAD_ACCESS_DELETE: return "DELETE";
    default: return "UNKNOWN";
    }
}

/* Emit a trace event as a RESP array (10 elements, binary-safe keys). */
static void emitEventResp(client *tracer, long long ts_us, uint64_t seq, int db_id, const char *cmd, robj *key, int access_type, int key_exists, const char *obj_type, size_t key_bytes, size_t value_bytes) {
    /* RESP array with 10 elements */
    addReplyArrayLen(tracer, 10);
    addReplyLongLong(tracer, ts_us);                          /* ts_us */
    addReplyLongLong(tracer, (long long)seq);                 /* seq */
    addReplyLongLong(tracer, db_id);                          /* db */
    addReplyBulkCString(tracer, cmd);                         /* cmd */
    addReplyBulk(tracer, key);                                /* key (binary-safe) */
    addReplyBulkCString(tracer, accessTypeName(access_type)); /* access_type */
    addReplyLongLong(tracer, key_exists);                     /* key_exists */
    addReplyBulkCString(tracer, obj_type);                    /* obj_type */
    addReplyLongLong(tracer, (long long)key_bytes);           /* key_bytes */
    if (key_exists) {
        addReplyLongLong(tracer, (long long)value_bytes); /* value_bytes */
    } else {
        addReplyNull(tracer);
    }
}

/* Emit a trace event as a RESP simple-string CSV line (RFC 4180 key quoting). */
static void emitEventCsv(client *tracer, long long ts_us, uint64_t seq, int db_id, const char *cmd, robj *key, int access_type, int key_exists, const char *obj_type, size_t key_bytes, size_t value_bytes) {
    sds line = sdscatprintf(sdsempty(), "+%lld,%llu,%d,%s,",
                            ts_us, (unsigned long long)seq, db_id, cmd);
    /* RFC 4180 CSV escaping: enclose in double-quotes, double any internal quotes.
     * Also encode bytes outside printable ASCII as \xHH for binary safety. */
    sds keystr = objectGetVal(key);
    size_t keylen = sdslen(keystr);
    line = sdscatlen(line, "\"", 1);
    for (size_t i = 0; i < keylen; i++) {
        unsigned char ch = keystr[i];
        if (ch == '"') {
            line = sdscatlen(line, "\"\"", 2);
        } else if (ch < 32 || ch == 127 || ch > 127) {
            /* Non-printable: hex escape for binary safety */
            line = sdscatprintf(line, "\\x%02x", ch);
        } else {
            line = sdscatlen(line, &keystr[i], 1);
        }
    }
    line = sdscatlen(line, "\"", 1);
    line = sdscatprintf(line, ",%s,%d,%s,%zu,",
                        accessTypeName(access_type), key_exists, obj_type, key_bytes);
    if (key_exists) {
        line = sdscatprintf(line, "%zu", value_bytes);
    }
    line = sdscatlen(line, "\r\n", 2);
    addReplySds(tracer, line);
}

extern uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);

/* Determine if a key should be traced based on hash-based sampling.
 * Returns 1 if the key's SipHash is within the configured threshold. */
static int keySampled(workloadTracerConfig *cfg, robj *key) {
    if (cfg->sample_threshold == UINT64_MAX) return 1; /* rate=1.0 fast path */
    if (cfg->sample_threshold == 0) return 0;          /* rate=0 fast path */
    /* Keys from command argv are always SDS-encoded (never integer-encoded). */
    serverAssert(key->encoding == OBJ_ENCODING_RAW || key->encoding == OBJ_ENCODING_EMBSTR);
    sds keystr = objectGetVal(key);
    uint64_t h = siphash((const uint8_t *)keystr, sdslen(keystr), cfg->sample_seed_bytes);
    return h <= cfg->sample_threshold;
}

/* Core event emission: iterates all subscribed tracers, applies sampling and
 * filtering, computes sizes, and emits in the configured format. */
static void emitEvent(int access_type, serverDb *db, robj *key, robj *val) {
    if (!workloadTraceActive()) return;
    if (trace_ctx.cmd_name == NULL) return; /* No command context */

    long long ts_us = ustime();
    int key_exists = (val != NULL) ? 1 : 0;
    const char *obj_type = "";
    size_t key_bytes = 0;
    size_t value_bytes = 0;

    if (key_exists) {
        obj_type = getObjectTypeName(val);
    }

    listIter li;
    listNode *ln;
    listRewind(server.workload_tracers, &li);
    while ((ln = listNext(&li))) {
        client *tracer = ln->value;
        workloadTracerConfig *cfg = tracer->workload_tracer_config;
        if (cfg == NULL) continue;

        /* Key-hash sampling: deterministic per-key decision */
        if (!keySampled(cfg, key)) continue;

        /* Filter negative lookups if not requested */
        if (!key_exists && access_type == WORKLOAD_ACCESS_READ && !cfg->negative_lookups) {
            continue;
        }

        /* Compute sizes (once per unique sample config, but typically all tracers share config) */
        if (key_exists) {
            key_bytes = computeKeyBytes(val);
            value_bytes = computeValueBytes(key, val, cfg->samples, db->id);
        }

        if (cfg->format == WORKLOAD_FORMAT_CSV) {
            emitEventCsv(tracer, ts_us, trace_ctx.seq, db->id,
                         trace_ctx.cmd_name, key, access_type, key_exists,
                         obj_type, key_bytes, value_bytes);
        } else {
            emitEventResp(tracer, ts_us, trace_ctx.seq, db->id,
                          trace_ctx.cmd_name, key, access_type, key_exists,
                          obj_type, key_bytes, value_bytes);
        }
        updateClientMemUsageAndBucket(tracer);
    }
}

/* --- Public event emitters --- */

/* Emit a READ trace event. Called from lookupKey() for non-write lookups. */
void workloadTraceEmitRead(serverDb *db, robj *key, robj *val) {
    emitEvent(WORKLOAD_ACCESS_READ, db, key, val);
}

/* Emit a WRITE trace event. Called from signalModifiedKey(). */
void workloadTraceEmitWrite(serverDb *db, robj *key, robj *val) {
    emitEvent(WORKLOAD_ACCESS_WRITE, db, key, val);
}

/* Emit a DELETE trace event. Called from dbGenericDeleteWithDictIndex(). */
void workloadTraceEmitDelete(serverDb *db, robj *key, robj *val) {
    emitEvent(WORKLOAD_ACCESS_DELETE, db, key, val);
}

/* --- MONITOR TRACE setup (called from monitorCommand) --- */

/* Parse TRACE options starting at argv[arg_start] and subscribe client.
 * Returns C_OK on success, C_ERR if a reply was already sent. */
int monitorTraceSetup(client *c, int arg_start) {
    int samples = -1; /* -1 = fast path (no deep traversal), 0 = sample all, >0 = sample N */
    int negative_lookups = 0;
    int format = WORKLOAD_FORMAT_RESP;
    double sample_rate = 1.0;
    uint64_t sample_seed = 0; /* Default fixed seed */

    for (int i = arg_start; i < c->argc; i++) {
        if (!strcasecmp(objectGetVal(c->argv[i]), "samples") && i + 1 < c->argc) {
            long long val;
            if (getLongLongFromObjectOrReply(c, c->argv[i + 1], &val, NULL) != C_OK) return C_ERR;
            if (val < 0 || val > INT_MAX) {
                addReplyError(c, "SAMPLES must be >= 0 and <= 2147483647");
                return C_ERR;
            }
            samples = (int)val;
            i++;
        } else if (!strcasecmp(objectGetVal(c->argv[i]), "negative-lookups") && i + 1 < c->argc) {
            if (!strcasecmp(objectGetVal(c->argv[i + 1]), "yes")) {
                negative_lookups = 1;
            } else if (!strcasecmp(objectGetVal(c->argv[i + 1]), "no")) {
                negative_lookups = 0;
            } else {
                addReplyError(c, "NEGATIVE-LOOKUPS must be yes or no");
                return C_ERR;
            }
            i++;
        } else if (!strcasecmp(objectGetVal(c->argv[i]), "format") && i + 1 < c->argc) {
            if (!strcasecmp(objectGetVal(c->argv[i + 1]), "resp")) {
                format = WORKLOAD_FORMAT_RESP;
            } else if (!strcasecmp(objectGetVal(c->argv[i + 1]), "csv")) {
                format = WORKLOAD_FORMAT_CSV;
            } else {
                addReplyError(c, "FORMAT must be resp or csv");
                return C_ERR;
            }
            i++;
        } else if (!strcasecmp(objectGetVal(c->argv[i]), "rate") && i + 1 < c->argc) {
            double r;
            if (getDoubleFromObjectOrReply(c, c->argv[i + 1], &r, "RATE must be a float") != C_OK)
                return C_ERR;
            if (r < 0.0 || r > 1.0) {
                addReplyError(c, "RATE must be between 0 and 1");
                return C_ERR;
            }
            sample_rate = r;
            i++;
        } else if (!strcasecmp(objectGetVal(c->argv[i]), "seed") && i + 1 < c->argc) {
            long long s;
            if (getLongLongFromObjectOrReply(c, c->argv[i + 1], &s, NULL) != C_OK) return C_ERR;
            sample_seed = (uint64_t)s;
            i++;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return C_ERR;
        }
    }

    /* Already tracing? Remove old subscription first. */
    workloadTraceDetachClient(c);

    /* Set up tracer config */
    workloadTracerConfig *cfg = zmalloc(sizeof(workloadTracerConfig));
    cfg->samples = samples;
    cfg->negative_lookups = negative_lookups;
    cfg->format = format;
    cfg->sample_rate = sample_rate;
    cfg->sample_seed = sample_seed;

    /* Pre-compute threshold: rate * UINT64_MAX (avoid FP in hot path) */
    if (sample_rate >= 1.0) {
        cfg->sample_threshold = UINT64_MAX;
    } else if (sample_rate <= 0.0) {
        cfg->sample_threshold = 0;
    } else {
        cfg->sample_threshold = (uint64_t)(sample_rate * (double)UINT64_MAX);
    }

    /* Derive 16-byte siphash key from the 64-bit seed (endian-neutral) */
    for (int b = 0; b < 8; b++) {
        cfg->sample_seed_bytes[b] = (uint8_t)(sample_seed >> (b * 8));
        cfg->sample_seed_bytes[b + 8] = cfg->sample_seed_bytes[b];
    }

    c->workload_tracer_config = cfg;
    c->flag.workload_tracer = 1;
    listAddNodeTail(server.workload_tracers, c);
    return C_OK;
}
