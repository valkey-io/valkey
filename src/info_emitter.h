/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Info emitter: a pluggable sink for INFO field generation.
 *
 * `genValkeyInfoString()` historically formatted the INFO output by writing
 * "key:value\r\n" text directly into an sds. Anything that wants INFO data as
 * structured values (the module API, a metrics exporter) then has to parse that
 * text back into numbers, losing the type information the producing code had.
 *
 * The info emitter abstracts "emit a section / emit a typed field" behind a
 * backend vtable so the same generation code can target multiple backends:
 *   - the text backend reproduces the current INFO output byte-for-byte;
 *   - structured backends (e.g. OTLP metrics) receive typed values plus
 *     semantic metadata (counter vs gauge, unit) directly, with no round trip.
 *
 * This header defines the interface and the built-in text backend. */

#ifndef INFO_EMITTER_H
#define INFO_EMITTER_H

#include "sds.h"
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Metric semantics carried alongside each numeric field. The text backend
 * ignores these except INFO_UNIT_PERCENT (which appends a literal '%'); structured
 * backends (OTLP) use them to choose sum-vs-gauge and set the unit, so they no
 * longer need name-based heuristics. */
typedef enum {
    INFO_KIND_GAUGE = 0, /* Point-in-time value that can go up or down. */
    INFO_KIND_COUNTER,   /* Monotonically increasing cumulative total. */
} infoKind;

typedef enum {
    INFO_UNIT_NONE = 0,
    INFO_UNIT_BYTES,
    INFO_UNIT_SECONDS,
    INFO_UNIT_MILLISECONDS,
    INFO_UNIT_MICROSECONDS,
    INFO_UNIT_PERCENT, /* Text backend appends '%' to reproduce "%.2f%%". */
} infoUnit;

typedef struct infoEmitter infoEmitter;

/* Backend operations. Numeric fields carry (kind, unit) metadata; the text
 * backend renders each exactly as the legacy inline formatting did. */
typedef struct infoEmitterOps {
    /* Section header, e.g. "# CPU\r\n". `name` excludes the leading "# ". The
     * text backend also owns the inter-section blank-line separator. */
    void (*begin_section)(infoEmitter *e, const char *name);

    /* Scalar fields ("key:value\r\n"). */
    void (*field_ll)(infoEmitter *e, const char *key, long long v, infoKind kind, infoUnit unit);
    void (*field_ull)(infoEmitter *e, const char *key, unsigned long long v, infoKind kind, infoUnit unit);
    /* `prec` preserves per-field precision (e.g. 2 for %.2f, 6 for %.6f). */
    void (*field_double)(infoEmitter *e, const char *key, double v, int prec, infoKind kind, infoUnit unit);
    /* Strings are identity/metadata, not metrics: no kind/unit. */
    void (*field_str)(infoEmitter *e, const char *key, const char *v);
    /* Fixed-point seconds.microseconds (%lld.%06lld); `usec` is total us. */
    void (*field_usec)(infoEmitter *e, const char *key, long long usec, infoKind kind);

    /* Composite ("dict") lines: key:sub1=v1,sub2=v2\r\n */
    void (*begin_dict)(infoEmitter *e, const char *key);
    void (*dict_ll)(infoEmitter *e, const char *sub, long long v, infoKind kind, infoUnit unit);
    void (*dict_double)(infoEmitter *e, const char *sub, double v, int prec, infoKind kind, infoUnit unit);
    void (*dict_str)(infoEmitter *e, const char *sub, const char *v);
    void (*end_dict)(infoEmitter *e);

    /* Escape hatch for irregular lines that do not map to a typed field. The
     * text backend prints it verbatim; structured backends may skip it. */
    void (*raw)(infoEmitter *e, const char *fmt, va_list ap);
} infoEmitterOps;

struct infoEmitter {
    const infoEmitterOps *ops;
};

/* ---- Text backend --------------------------------------------------------
 * Appends INFO-formatted text into an sds, byte-for-byte identical to the
 * legacy inline formatting. Stack-allocate an infoEmitterText, init it with the
 * sds to append to, pass &t.e to the generation code, then read the (possibly
 * reallocated) result back with infoEmitterTextResult(). */
/* Text backend state. Fields are written directly into the sds's own buffer
 * (no intermediate copy); the sds length is committed only when the buffer must
 * grow rather than once per field. */
typedef struct infoEmitterText {
    infoEmitter e; /* Must be the first member: &t.e aliases the container. */
    sds s;
    int *section_counter; /* Shared inter-section counter (see begin_section). */
    int dict_empty;       /* No dict sub-field written yet (controls the comma). */
    size_t pend;          /* Bytes written past sdslen(s) but not yet committed. */
} infoEmitterText;

void infoEmitterTextInit(infoEmitterText *t, sds s, int *section_counter);
sds infoEmitterTextResult(infoEmitterText *t);

/* ---- Dispatch helpers ----------------------------------------------------
 * Full-control forms take explicit (kind, unit); the terse Field/Counter forms
 * default to the common cases so most call sites stay short. */
static inline void infoEmitBeginSection(infoEmitter *e, const char *name) {
    e->ops->begin_section(e, name);
}
static inline void infoEmitMetricLL(infoEmitter *e, const char *k, long long v, infoKind kind, infoUnit unit) {
    e->ops->field_ll(e, k, v, kind, unit);
}
static inline void infoEmitMetricULL(infoEmitter *e, const char *k, unsigned long long v, infoKind kind, infoUnit unit) {
    e->ops->field_ull(e, k, v, kind, unit);
}
static inline void infoEmitMetricDouble(infoEmitter *e, const char *k, double v, int prec, infoKind kind, infoUnit unit) {
    e->ops->field_double(e, k, v, prec, kind, unit);
}
/* Gauge shorthands (kind=GAUGE, unit=NONE). */
static inline void infoEmitFieldLL(infoEmitter *e, const char *k, long long v) {
    e->ops->field_ll(e, k, v, INFO_KIND_GAUGE, INFO_UNIT_NONE);
}
static inline void infoEmitFieldULL(infoEmitter *e, const char *k, unsigned long long v) {
    e->ops->field_ull(e, k, v, INFO_KIND_GAUGE, INFO_UNIT_NONE);
}
static inline void infoEmitFieldDouble(infoEmitter *e, const char *k, double v, int prec) {
    e->ops->field_double(e, k, v, prec, INFO_KIND_GAUGE, INFO_UNIT_NONE);
}
/* Counter shorthands (kind=COUNTER, unit=NONE). */
static inline void infoEmitCounterLL(infoEmitter *e, const char *k, long long v) {
    e->ops->field_ll(e, k, v, INFO_KIND_COUNTER, INFO_UNIT_NONE);
}
static inline void infoEmitCounterULL(infoEmitter *e, const char *k, unsigned long long v) {
    e->ops->field_ull(e, k, v, INFO_KIND_COUNTER, INFO_UNIT_NONE);
}
static inline void infoEmitFieldStr(infoEmitter *e, const char *k, const char *v) {
    e->ops->field_str(e, k, v);
}
/* CPU times are cumulative (counter); current_* durations are gauges. */
static inline void infoEmitFieldUsec(infoEmitter *e, const char *k, long long usec) {
    e->ops->field_usec(e, k, usec, INFO_KIND_COUNTER);
}
static inline void infoEmitGaugeUsec(infoEmitter *e, const char *k, long long usec) {
    e->ops->field_usec(e, k, usec, INFO_KIND_GAUGE);
}
static inline void infoEmitBeginDict(infoEmitter *e, const char *k) {
    e->ops->begin_dict(e, k);
}
static inline void infoEmitDictLL(infoEmitter *e, const char *sub, long long v) {
    e->ops->dict_ll(e, sub, v, INFO_KIND_GAUGE, INFO_UNIT_NONE);
}
static inline void infoEmitDictCounterLL(infoEmitter *e, const char *sub, long long v) {
    e->ops->dict_ll(e, sub, v, INFO_KIND_COUNTER, INFO_UNIT_NONE);
}
static inline void infoEmitDictMetricLL(infoEmitter *e, const char *sub, long long v, infoKind kind, infoUnit unit) {
    e->ops->dict_ll(e, sub, v, kind, unit);
}
static inline void infoEmitDictDouble(infoEmitter *e, const char *sub, double v, int prec) {
    e->ops->dict_double(e, sub, v, prec, INFO_KIND_GAUGE, INFO_UNIT_NONE);
}
static inline void infoEmitDictStr(infoEmitter *e, const char *sub, const char *v) {
    e->ops->dict_str(e, sub, v);
}
static inline void infoEmitEndDict(infoEmitter *e) {
    e->ops->end_dict(e);
}
static inline void infoEmitRaw(infoEmitter *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    e->ops->raw(e, fmt, ap);
    va_end(ap);
}

#ifdef __cplusplus
}
#endif

#endif /* INFO_EMITTER_H */
