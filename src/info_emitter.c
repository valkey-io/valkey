/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Text backend for the info emitter (see info_emitter.h).
 *
 * Byte-for-byte parity with the legacy genValkeyInfoString() formatting is the
 * hard requirement here, since the INFO wire format is depended on by clients,
 * tooling and tests:
 *   - integers reproduce exactly: %lld/%d/%ld render the same digits, so
 *     field_ll uses ll2string; field_ull uses ull2string (%llu/%lu/%zu family);
 *   - floating point preserves the per-field precision via `prec`;
 *     INFO_UNIT_PERCENT appends a literal '%' to reproduce "%.2f%%";
 *   - field_usec reproduces the "sec.usec" layout (%lld.%06lld);
 *   - dict lines render as "key:sub1=v1,sub2=v2\r\n";
 *   - raw() prints verbatim for anything that does not fit a typed field.
 *
 * Performance: the legacy code batched a whole section into one sdscatprintf(),
 * which parses a format string but writes each field's bytes into the sds
 * buffer exactly once. To be at least as fast per field, this backend writes
 * fields DIRECTLY into the sds's own buffer: ie_reserve() ensures room at the
 * tail (growing on demand and committing the length only then), and each field
 * is formatted in place with ll2string()/ull2string() (integers, no vsnprintf)
 * or a single snprintf() (doubles/usec). This is one copy per byte (vs a scratch
 * buffer's two) with the sds length committed on grow rather
 * than per-field, and no format parsing for integers.
 *
 * The (kind, unit) metadata is otherwise ignored by the text backend; it exists
 * for structured backends (OTLP). */

#include "fmacros.h"
#include "info_emitter.h"
#include "util.h"
#include <stdio.h>
#include <string.h>

/* Large enough for any single INFO numeric value: %.6f of the largest double is
 * ~322 chars; usec ("%lld.%06lld") and integers are far smaller. */
#define INFO_EMIT_NUMBUF 340

/* The emitter is the first member of infoEmitterText, so the container can be
 * recovered from the base pointer without an offset. */
static infoEmitterText *textOf(infoEmitter *e) {
    return (infoEmitterText *)e;
}

/* Commit any bytes written past the sds length. */
static void ie_flush(infoEmitterText *t) {
    if (t->pend) {
        sdsIncrLen(t->s, (ssize_t)t->pend);
        t->pend = 0;
    }
}

/* Ensure `n` writable bytes exist at the sds tail beyond the committed length
 * plus the currently-pending (written-but-not-committed) bytes, and return the
 * write pointer. Commits + grows only when necessary. We request exactly `n`
 * from sdsMakeRoomFor and rely on its own greedy doubling for amortization —
 * requesting a larger minimum would compound with the doubling and leave the
 * INFO reply buffer noticeably over-allocated (which inflates the reported
 * used_memory, since the buffer is live while INFO is being generated). The
 * caller writes up to `n` bytes and then calls ie_commit(); it must not call
 * ie_reserve() again before committing (a grow may reallocate the buffer). */
static char *ie_reserve(infoEmitterText *t, size_t n) {
    if (sdsavail(t->s) < t->pend + n) {
        ie_flush(t);
        t->s = sdsMakeRoomFor(t->s, n);
    }
    return t->s + sdslen(t->s) + t->pend;
}

static void ie_commit(infoEmitterText *t, size_t n) {
    t->pend += n;
}

/* Write raw bytes directly into the sds tail. */
static void ie_put(infoEmitterText *t, const char *data, size_t n) {
    char *p = ie_reserve(t, n);
    memcpy(p, data, n);
    ie_commit(t, n);
}

static void textBeginSection(infoEmitter *e, const char *name) {
    infoEmitterText *t = textOf(e);
    /* Reproduce the legacy per-call-site separator: emit "\r\n" before every
     * section except the first (post-increment matches "if (sections++)"). */
    size_t nlen = strlen(name);
    int sep = (t->section_counter && (*t->section_counter)++) ? 1 : 0;
    char *p0 = ie_reserve(t, (sep ? 2 : 0) + 2 + nlen + 2);
    char *p = p0;
    if (sep) {
        *p++ = '\r';
        *p++ = '\n';
    }
    *p++ = '#';
    *p++ = ' ';
    memcpy(p, name, nlen);
    p += nlen;
    *p++ = '\r';
    *p++ = '\n';
    ie_commit(t, (size_t)(p - p0));
}

static void textFieldLL(infoEmitter *e, const char *key, long long v, infoKind kind, infoUnit unit) {
    (void)kind;
    (void)unit;
    infoEmitterText *t = textOf(e);
    size_t klen = strlen(key);
    char *p0 = ie_reserve(t, klen + 1 + LONG_STR_SIZE + 2);
    char *p = p0;
    memcpy(p, key, klen);
    p += klen;
    *p++ = ':';
    p += ll2string(p, LONG_STR_SIZE, v);
    *p++ = '\r';
    *p++ = '\n';
    ie_commit(t, (size_t)(p - p0));
}

static void textFieldULL(infoEmitter *e, const char *key, unsigned long long v, infoKind kind, infoUnit unit) {
    (void)kind;
    (void)unit;
    infoEmitterText *t = textOf(e);
    size_t klen = strlen(key);
    char *p0 = ie_reserve(t, klen + 1 + LONG_STR_SIZE + 2);
    char *p = p0;
    memcpy(p, key, klen);
    p += klen;
    *p++ = ':';
    p += ull2string(p, LONG_STR_SIZE, v);
    *p++ = '\r';
    *p++ = '\n';
    ie_commit(t, (size_t)(p - p0));
}

static void textFieldDouble(infoEmitter *e, const char *key, double v, int prec, infoKind kind, infoUnit unit) {
    (void)kind;
    infoEmitterText *t = textOf(e);
    size_t klen = strlen(key);
    char *p0 = ie_reserve(t, klen + 1 + INFO_EMIT_NUMBUF + 2);
    char *p = p0;
    memcpy(p, key, klen);
    p += klen;
    *p++ = ':';
    /* INFO_UNIT_PERCENT reproduces the legacy "%.2f%%" (trailing literal '%'). */
    int n;
    if (unit == INFO_UNIT_PERCENT)
        n = snprintf(p, INFO_EMIT_NUMBUF, "%.*f%%", prec, v);
    else
        n = snprintf(p, INFO_EMIT_NUMBUF, "%.*f", prec, v);
    if (n < 0) n = 0;
    if (n >= INFO_EMIT_NUMBUF) n = INFO_EMIT_NUMBUF - 1;
    p += n;
    *p++ = '\r';
    *p++ = '\n';
    ie_commit(t, (size_t)(p - p0));
}

static void textFieldStr(infoEmitter *e, const char *key, const char *v) {
    infoEmitterText *t = textOf(e);
    size_t klen = strlen(key), vlen = strlen(v);
    char *p0 = ie_reserve(t, klen + 1 + vlen + 2);
    char *p = p0;
    memcpy(p, key, klen);
    p += klen;
    *p++ = ':';
    memcpy(p, v, vlen);
    p += vlen;
    *p++ = '\r';
    *p++ = '\n';
    ie_commit(t, (size_t)(p - p0));
}

static void textFieldUsec(infoEmitter *e, const char *key, long long usec, infoKind kind) {
    (void)kind;
    infoEmitterText *t = textOf(e);
    size_t klen = strlen(key);
    char *p0 = ie_reserve(t, klen + 1 + INFO_EMIT_NUMBUF + 2);
    char *p = p0;
    memcpy(p, key, klen);
    p += klen;
    *p++ = ':';
    int n = snprintf(p, INFO_EMIT_NUMBUF, "%lld.%06lld", usec / 1000000, usec % 1000000);
    if (n < 0) n = 0;
    if (n >= INFO_EMIT_NUMBUF) n = INFO_EMIT_NUMBUF - 1;
    p += n;
    *p++ = '\r';
    *p++ = '\n';
    ie_commit(t, (size_t)(p - p0));
}

static void textBeginDict(infoEmitter *e, const char *key) {
    infoEmitterText *t = textOf(e);
    size_t klen = strlen(key);
    char *p0 = ie_reserve(t, klen + 1);
    memcpy(p0, key, klen);
    p0[klen] = ':';
    ie_commit(t, klen + 1);
    t->dict_empty = 1;
}

/* Write "sub=" preceded by "," for every sub-field except the first, into a
 * region already reserved by the caller; returns the advanced write pointer. */
static char *textDictSub(infoEmitterText *t, char *p, const char *sub, size_t slen) {
    if (!t->dict_empty) *p++ = ',';
    t->dict_empty = 0;
    memcpy(p, sub, slen);
    p += slen;
    *p++ = '=';
    return p;
}

static void textDictLL(infoEmitter *e, const char *sub, long long v, infoKind kind, infoUnit unit) {
    (void)kind;
    (void)unit;
    infoEmitterText *t = textOf(e);
    size_t slen = strlen(sub);
    char *p0 = ie_reserve(t, 1 + slen + 1 + LONG_STR_SIZE);
    char *p = textDictSub(t, p0, sub, slen);
    p += ll2string(p, LONG_STR_SIZE, v);
    ie_commit(t, (size_t)(p - p0));
}

static void textDictDouble(infoEmitter *e, const char *sub, double v, int prec, infoKind kind, infoUnit unit) {
    (void)kind;
    (void)unit;
    infoEmitterText *t = textOf(e);
    size_t slen = strlen(sub);
    char *p0 = ie_reserve(t, 1 + slen + 1 + INFO_EMIT_NUMBUF);
    char *p = textDictSub(t, p0, sub, slen);
    int n = snprintf(p, INFO_EMIT_NUMBUF, "%.*f", prec, v);
    if (n < 0) n = 0;
    if (n >= INFO_EMIT_NUMBUF) n = INFO_EMIT_NUMBUF - 1;
    p += n;
    ie_commit(t, (size_t)(p - p0));
}

static void textDictStr(infoEmitter *e, const char *sub, const char *v) {
    infoEmitterText *t = textOf(e);
    size_t slen = strlen(sub), vlen = strlen(v);
    char *p0 = ie_reserve(t, 1 + slen + 1 + vlen);
    char *p = textDictSub(t, p0, sub, slen);
    memcpy(p, v, vlen);
    p += vlen;
    ie_commit(t, (size_t)(p - p0));
}

static void textEndDict(infoEmitter *e) {
    infoEmitterText *t = textOf(e);
    ie_put(t, "\r\n", 2);
}

static void textRaw(infoEmitter *e, const char *fmt, va_list ap) {
    infoEmitterText *t = textOf(e);
    ie_flush(t); /* Preserve ordering: pending bytes come first. */
    t->s = sdscatvprintf(t->s, fmt, ap);
}

static const infoEmitterOps textOps = {
    .begin_section = textBeginSection,
    .field_ll = textFieldLL,
    .field_ull = textFieldULL,
    .field_double = textFieldDouble,
    .field_str = textFieldStr,
    .field_usec = textFieldUsec,
    .begin_dict = textBeginDict,
    .dict_ll = textDictLL,
    .dict_double = textDictDouble,
    .dict_str = textDictStr,
    .end_dict = textEndDict,
    .raw = textRaw,
};

void infoEmitterTextInit(infoEmitterText *t, sds s, int *section_counter) {
    t->e.ops = &textOps;
    t->s = s;
    t->section_counter = section_counter;
    t->dict_empty = 1;
    t->pend = 0;
}

sds infoEmitterTextResult(infoEmitterText *t) {
    ie_flush(t);
    return t->s;
}
