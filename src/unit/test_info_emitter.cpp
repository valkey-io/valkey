/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Unit tests for the info emitter text backend (src/info_emitter.c).
 *
 * Two levels of coverage:
 *   1. Per field-kind checks that the text backend renders each typed field
 *      exactly as the legacy INFO formatting does.
 *   2. A byte-for-byte parity test that reconstructs the CPU section using the
 *      *original* inline format strings and asserts the emitter produces the
 *      identical bytes for the same fixed inputs. This guards the pilot
 *      conversion of the CPU section in genValkeyInfoString(). */

#include "generated_wrappers.hpp"

#include <functional>
#include <string>

extern "C" {
#include "fmacros.h"
#include "info_emitter.h"
#include "sds.h"
}

namespace {

/* Run `fn` against a fresh text emitter and return the accumulated bytes.
 * The section counter starts at 0, so begin_section emits no leading separator
 * (matches the "first section" case: output starts directly with "# ..."). */
std::string emit(const std::function<void(infoEmitter *)> &fn) {
    infoEmitterText te;
    int sections = 0;
    infoEmitterTextInit(&te, sdsempty(), &sections);
    fn(&te.e);
    sds s = infoEmitterTextResult(&te);
    std::string out(s, sdslen(s));
    sdsfree(s);
    return out;
}

} // namespace

class InfoEmitterTextTest : public ::testing::Test {};

TEST_F(InfoEmitterTextTest, BeginSection) {
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitBeginSection(e, "CPU"); }), "# CPU\r\n");
}

TEST_F(InfoEmitterTextTest, FieldLL) {
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldLL(e, "connected_clients", 42); }), "connected_clients:42\r\n");
    /* Negative and boundary values render with the same digits as %lld/%ld/%d. */
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldLL(e, "k", -1); }), "k:-1\r\n");
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldLL(e, "k", -9223372036854775807LL - 1); }),
              "k:-9223372036854775808\r\n");
}

TEST_F(InfoEmitterTextTest, FieldULL) {
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldULL(e, "used_memory", 1002384ULL); }), "used_memory:1002384\r\n");
    /* Full unsigned 64-bit range (matches %llu / %lu / %zu digits). */
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldULL(e, "k", 18446744073709551615ULL); }),
              "k:18446744073709551615\r\n");
}

TEST_F(InfoEmitterTextTest, FieldDoublePrecision) {
    /* mem_fragmentation_ratio uses %.2f. */
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldDouble(e, "ratio", 1.5, 2); }), "ratio:1.50\r\n");
    /* CPU-style precision uses %.6f. */
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldDouble(e, "x", 0.1, 6); }), "x:0.100000\r\n");
}

TEST_F(InfoEmitterTextTest, PercentUnitAppendsSign) {
    /* INFO_UNIT_PERCENT reproduces the legacy "%.2f%%" (trailing literal '%'). */
    EXPECT_EQ(emit([](infoEmitter *e) {
                  infoEmitMetricDouble(e, "used_memory_peak_perc", 3.14, 2, INFO_KIND_GAUGE, INFO_UNIT_PERCENT);
              }),
              "used_memory_peak_perc:3.14%\r\n");
    /* A non-percent double with the same value has no trailing '%'. */
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldDouble(e, "x", 3.14, 2); }), "x:3.14\r\n");
}

TEST_F(InfoEmitterTextTest, MetadataIgnoredByTextForIntegers) {
    /* kind/unit are for structured backends; the text backend renders the same
     * integer regardless of counter-vs-gauge or unit. */
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitCounterLL(e, "k", 42); }), "k:42\r\n");
    EXPECT_EQ(emit([](infoEmitter *e) {
                  infoEmitMetricLL(e, "k", 42, INFO_KIND_COUNTER, INFO_UNIT_BYTES);
              }),
              "k:42\r\n");
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldLL(e, "k", 42); }), "k:42\r\n");
}

TEST_F(InfoEmitterTextTest, FieldStr) {
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldStr(e, "role", "master"); }), "role:master\r\n");
}

TEST_F(InfoEmitterTextTest, FieldUsec) {
    /* Total microseconds render as "sec.usec" with 6-digit zero-padded frac,
     * matching the legacy "%ld.%06ld" / "%lld.%06lld" formatting. */
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldUsec(e, "used_cpu_sys", 1234567); }), "used_cpu_sys:1.234567\r\n");
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldUsec(e, "k", 999999); }), "k:0.999999\r\n");
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldUsec(e, "k", 60000000); }), "k:60.000000\r\n");
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitFieldUsec(e, "k", 0); }), "k:0.000000\r\n");
}

TEST_F(InfoEmitterTextTest, DictComposite) {
    /* Keyspace-style composite line: key:sub1=v1,sub2=v2\r\n */
    EXPECT_EQ(emit([](infoEmitter *e) {
                  infoEmitBeginDict(e, "db0");
                  infoEmitDictLL(e, "keys", 50);
                  infoEmitDictLL(e, "expires", 0);
                  infoEmitDictLL(e, "avg_ttl", 0);
                  infoEmitEndDict(e);
              }),
              "db0:keys=50,expires=0,avg_ttl=0\r\n");
}

TEST_F(InfoEmitterTextTest, DictSingleField) {
    EXPECT_EQ(emit([](infoEmitter *e) {
                  infoEmitBeginDict(e, "d");
                  infoEmitDictStr(e, "name", "x");
                  infoEmitEndDict(e);
              }),
              "d:name=x\r\n");
}

TEST_F(InfoEmitterTextTest, Raw) {
    EXPECT_EQ(emit([](infoEmitter *e) { infoEmitRaw(e, "custom:%d\r\n", 7); }), "custom:7\r\n");
}

/* Byte-for-byte parity: rebuild the CPU section with the exact legacy format
 * strings for a fixed set of inputs, then produce the same section through the
 * emitter, and assert the two byte streams are identical. */
TEST_F(InfoEmitterTextTest, CpuSectionByteParity) {
    /* Fixed, deterministic inputs (seconds, microseconds). */
    const long sys_s = 12, sys_us = 345678;
    const long usr_s = 7, usr_us = 1;
    const long csys_s = 0, csys_us = 0;
    const long cusr_s = 100, cusr_us = 999999;
    const long msys_s = 3, msys_us = 14;
    const long musr_s = 2, musr_us = 718281;
    const long long active = 5000123; /* used_active_time_main_thread */
    const long long io1 = 250000, io2 = 61000000;

    /* --- Legacy formatting (copied from the pre-refactor genValkeyInfoString) --- */
    sds legacy = sdsempty();
    legacy = sdscatprintf(legacy,
                          "# CPU\r\n"
                          "used_cpu_sys:%ld.%06ld\r\n"
                          "used_cpu_user:%ld.%06ld\r\n"
                          "used_cpu_sys_children:%ld.%06ld\r\n"
                          "used_cpu_user_children:%ld.%06ld\r\n",
                          sys_s, sys_us, usr_s, usr_us, csys_s, csys_us, cusr_s, cusr_us);
    legacy = sdscatprintf(legacy,
                          "used_cpu_sys_main_thread:%ld.%06ld\r\n"
                          "used_cpu_user_main_thread:%ld.%06ld\r\n",
                          msys_s, msys_us, musr_s, musr_us);
    legacy = sdscatprintf(legacy, "used_active_time_main_thread:%lld.%06lld\r\n", active / 1000000, active % 1000000);
    legacy = sdscatprintf(legacy, "used_active_time_io_thread_%d:%lld.%06lld\r\n", 1, io1 / 1000000, io1 % 1000000);
    legacy = sdscatprintf(legacy, "used_active_time_io_thread_%d:%lld.%06lld\r\n", 2, io2 / 1000000, io2 % 1000000);
    std::string expected(legacy, sdslen(legacy));
    sdsfree(legacy);

    /* --- Emitter formatting (mirrors the refactored CPU section) --- */
    std::string actual = emit([&](infoEmitter *e) {
        infoEmitBeginSection(e, "CPU");
        infoEmitFieldUsec(e, "used_cpu_sys", (long long)sys_s * 1000000 + sys_us);
        infoEmitFieldUsec(e, "used_cpu_user", (long long)usr_s * 1000000 + usr_us);
        infoEmitFieldUsec(e, "used_cpu_sys_children", (long long)csys_s * 1000000 + csys_us);
        infoEmitFieldUsec(e, "used_cpu_user_children", (long long)cusr_s * 1000000 + cusr_us);
        infoEmitFieldUsec(e, "used_cpu_sys_main_thread", (long long)msys_s * 1000000 + msys_us);
        infoEmitFieldUsec(e, "used_cpu_user_main_thread", (long long)musr_s * 1000000 + musr_us);
        infoEmitFieldUsec(e, "used_active_time_main_thread", active);
        for (int i = 1; i <= 2; i++) {
            char key[64];
            snprintf(key, sizeof(key), "used_active_time_io_thread_%d", i);
            infoEmitFieldUsec(e, key, i == 1 ? io1 : io2);
        }
    });

    EXPECT_EQ(actual, expected);
}

/* Stats-section parity for the %.2f fields whose legacy argument is a
 * "(float)value / 1024" expression (instantaneous_*_kbps) or a "double * 100"
 * expression (expired_stale_perc). The float case is the subtle one: the value
 * is computed in float precision, then widened to double for %.2f. field_double
 * takes a double, so the same widening yields identical bytes. */
TEST_F(InfoEmitterTextTest, StatsFloat2fParity) {
    /* kbps: (float)metric / 1024, rendered with %.2f. Pick values that exercise
     * rounding at the 2nd decimal. */
    const long long metrics[] = {0, 1500, 1048576, 123456789};
    for (long long m : metrics) {
        float kbps = (float)m / 1024;
        sds legacy = sdscatprintf(sdsempty(), "instantaneous_input_kbps:%.2f\r\n", (double)kbps);
        std::string expected(legacy, sdslen(legacy));
        sdsfree(legacy);
        std::string actual =
            emit([&](infoEmitter *e) { infoEmitFieldDouble(e, "instantaneous_input_kbps", (double)kbps, 2); });
        EXPECT_EQ(actual, expected) << "kbps mismatch for metric=" << m;
    }

    /* percentage: double * 100, rendered with %.2f. */
    const double percs[] = {0.0, 0.5, 0.123456, 1.0};
    for (double p : percs) {
        double v = p * 100;
        sds legacy = sdscatprintf(sdsempty(), "expired_stale_perc:%.2f\r\n", v);
        std::string expected(legacy, sdslen(legacy));
        sdsfree(legacy);
        std::string actual = emit([&](infoEmitter *e) { infoEmitFieldDouble(e, "expired_stale_perc", v, 2); });
        EXPECT_EQ(actual, expected) << "perc mismatch for p=" << p;
    }
}

/* begin_section owns the inter-section "\r\n" separator: none before the first
 * section, one before each subsequent section, matching the legacy
 * "if (sections++) sdscat("\r\n")" behavior. The counter is shared with any
 * legacy inline sections still present during migration. */
TEST_F(InfoEmitterTextTest, SectionSeparatorOwnedByEmitter) {
    infoEmitterText te;
    int sections = 0;
    infoEmitterTextInit(&te, sdsempty(), &sections);
    infoEmitBeginSection(&te.e, "Alpha");
    infoEmitFieldLL(&te.e, "a", 1);
    infoEmitBeginSection(&te.e, "Beta");
    infoEmitFieldLL(&te.e, "b", 2);
    sds s = infoEmitterTextResult(&te);
    std::string out(s, sdslen(s));
    sdsfree(s);
    EXPECT_EQ(out, "# Alpha\r\na:1\r\n\r\n# Beta\r\nb:2\r\n");
    EXPECT_EQ(sections, 2);
}

/* When the caller starts the counter above zero (a prior legacy section already
 * emitted), the first emitter section is correctly separated too. */
TEST_F(InfoEmitterTextTest, SectionSeparatorRespectsPriorSections) {
    infoEmitterText te;
    int sections = 1; /* Pretend a legacy inline section ran first. */
    infoEmitterTextInit(&te, sdsempty(), &sections);
    infoEmitBeginSection(&te.e, "CPU");
    sds s = infoEmitterTextResult(&te);
    std::string out(s, sdslen(s));
    sdsfree(s);
    EXPECT_EQ(out, "\r\n# CPU\r\n");
    EXPECT_EQ(sections, 2);
}

/* Cluster-section parity: single cluster_enabled field via field_ll. */
TEST_F(InfoEmitterTextTest, ClusterSectionParity) {
    for (int enabled = 0; enabled <= 1; enabled++) {
        sds legacy = sdscatprintf(sdsempty(), "# Cluster\r\ncluster_enabled:%d\r\n", enabled);
        std::string expected(legacy, sdslen(legacy));
        sdsfree(legacy);
        std::string actual = emit([&](infoEmitter *e) {
            infoEmitBeginSection(e, "Cluster");
            infoEmitFieldLL(e, "cluster_enabled", enabled);
        });
        EXPECT_EQ(actual, expected);
    }
}
