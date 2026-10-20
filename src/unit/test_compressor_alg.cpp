/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Tests for the value compression backends. See src/compressor/compressor_alg.h. */

#include "generated_wrappers.hpp"

#include <string.h>

extern "C" {
#include "compressor/compressor_alg.h"
#include "server.h"
#include "zmalloc.h"
}

/* zmalloc.h defines helper macros (__str/__xstr) that collide with libstdc++
 * internals. Keep them local to C headers in this C++ translation unit. */
#ifdef __xstr
#undef __xstr
#endif
#ifdef __str
#undef __str
#endif

/* Sample data. Half the values build the dictionary, half are compressed with
 * it, so no value ever compresses against itself. */
#define SAMPLE_COUNT 64
#define DICT_SAMPLES 32
#define SAMPLE_CAP 512
#define DICT_CAP 16384

typedef struct compressorTestData {
    char value[SAMPLE_COUNT][SAMPLE_CAP];
    size_t len[SAMPLE_COUNT];
    char dict[DICT_CAP];
    size_t dict_len;
} compressorTestData;

/* Builds JSON-like values that share their field names, which is the shape this
 * feature targets. */
static void compressorTestDataInit(compressorTestData *d) {
    d->dict_len = 0;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        int n = snprintf(d->value[i],
                         SAMPLE_CAP,
                         "{\"user_id\":%d,\"user_name\":\"person_%d\","
                         "\"email_address\":\"person_%d@example.com\","
                         "\"created_at\":\"2026-09-10T11:0%d:00Z\",\"is_active\":true,"
                         "\"role\":\"member\",\"preferences\":{\"language\":\"en\","
                         "\"timezone\":\"UTC\",\"theme\":\"dark\"},\"login_count\":%d,"
                         "\"last_seen_at\":\"2026-09-10T11:0%d:00Z\"}",
                         i,
                         i,
                         i,
                         i % 10,
                         i * 7,
                         i % 10);
        d->len[i] = (size_t)n;
        if (i < DICT_SAMPLES && d->dict_len + d->len[i] < DICT_CAP) {
            memcpy(d->dict + d->dict_len, d->value[i], d->len[i]);
            d->dict_len += d->len[i];
        }
    }
}

/* ===== construction ===== */

TEST(CompressorAlg, NewCompressorLz4) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    EXPECT_STREQ(c->name, "lz4");
    EXPECT_EQ(c->id, COMPRESSOR_ALG_LZ4);
    EXPECT_EQ(c->dict_max_size, (size_t)COMPRESSOR_LZ4_DICT_MAX);
    EXPECT_TRUE(c->api != NULL);
    EXPECT_TRUE(c->state != NULL);
    EXPECT_TRUE(c->max_output_size != NULL);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_NONE);
    /* A NULL config means no size limits. */
    EXPECT_EQ(c->config.min_input_len, (size_t)0);
    EXPECT_EQ(c->config.max_input_len, (size_t)0);
    freeCompressor(c);
}

TEST(CompressorAlg, NewCompressorCopiesConfig) {
    compressorConfig cfg;
    cfg.min_input_len = 256;
    cfg.max_input_len = 131072;

    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, &cfg);
    ASSERT_TRUE(c != NULL);
    EXPECT_EQ(c->config.min_input_len, (size_t)256);
    EXPECT_EQ(c->config.max_input_len, (size_t)131072);

    /* The config is copied, so changing the caller's copy must not reach it. */
    cfg.min_input_len = 1;
    cfg.max_input_len = 2;
    EXPECT_EQ(c->config.min_input_len, (size_t)256);
    EXPECT_EQ(c->config.max_input_len, (size_t)131072);
    freeCompressor(c);
}

TEST(CompressorAlg, NewCompressorRejectsUnavailableBackends) {
    /* off has no backend at all. */
    EXPECT_TRUE(newCompressor(COMPRESSOR_ALG_NONE, NULL) == NULL);
    /* zstd is not vendored yet, so it has no function table. */
    EXPECT_TRUE(newCompressor(COMPRESSOR_ALG_ZSTD, NULL) == NULL);
    /* An id outside the enum must not be treated as a valid backend. */
    EXPECT_TRUE(newCompressor((compressorAlgId)99, NULL) == NULL);
}

TEST(CompressorAlg, FreeCompressorAcceptsNull) {
    freeCompressor(NULL);
}

TEST(CompressorAlg, Lz4HasNoTrainerAndNoReleaseHook) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    /* LZ4 has no dictionary trainer of its own, so this stays NULL. */
    EXPECT_TRUE(c->api->train == NULL);
    /* LZ4 keeps no per-frame library state, so there is nothing to release. */
    EXPECT_TRUE(c->api->release == NULL);
    freeCompressor(c);
}

/* ===== name mapping ===== */

TEST(CompressorAlg, IdFromName) {
    compressorAlgId id;

    EXPECT_EQ(compressorAlgIdFromName("off", &id), C_OK);
    EXPECT_EQ(id, COMPRESSOR_ALG_NONE);
    EXPECT_EQ(compressorAlgIdFromName("lz4", &id), C_OK);
    EXPECT_EQ(id, COMPRESSOR_ALG_LZ4);
    EXPECT_EQ(compressorAlgIdFromName("zstd", &id), C_OK);
    EXPECT_EQ(id, COMPRESSOR_ALG_ZSTD);

    /* The comparison ignores case, like other config values. */
    EXPECT_EQ(compressorAlgIdFromName("LZ4", &id), C_OK);
    EXPECT_EQ(id, COMPRESSOR_ALG_LZ4);

    EXPECT_EQ(compressorAlgIdFromName("gzip", &id), C_ERR);
    EXPECT_EQ(compressorAlgIdFromName("", &id), C_ERR);
    EXPECT_EQ(compressorAlgIdFromName(NULL, &id), C_ERR);
    EXPECT_EQ(compressorAlgIdFromName("lz4", NULL), C_ERR);
}

TEST(CompressorAlg, IdName) {
    EXPECT_STREQ(compressorAlgIdName(COMPRESSOR_ALG_NONE), "off");
    EXPECT_STREQ(compressorAlgIdName(COMPRESSOR_ALG_LZ4), "lz4");
    EXPECT_STREQ(compressorAlgIdName(COMPRESSOR_ALG_ZSTD), "zstd");
    /* An unknown id must still return usable text, never NULL. */
    EXPECT_STREQ(compressorAlgIdName((compressorAlgId)99), "off");
}

TEST(CompressorAlg, IdNameRoundTrip) {
    compressorAlgId ids[3] = {COMPRESSOR_ALG_NONE, COMPRESSOR_ALG_LZ4, COMPRESSOR_ALG_ZSTD};
    for (int i = 0; i < 3; i++) {
        compressorAlgId back;
        EXPECT_EQ(compressorAlgIdFromName(compressorAlgIdName(ids[i]), &back), C_OK);
        EXPECT_EQ(back, ids[i]);
    }
}

TEST(CompressorAlg, Strerror) {
    EXPECT_TRUE(compressorStrerror(COMPRESSOR_ERR_NONE) != NULL);
    EXPECT_TRUE(compressorStrerror(COMPRESSOR_ERR_BAD_SIZE) != NULL);
    EXPECT_TRUE(compressorStrerror(COMPRESSOR_ERR_COMPRESS) != NULL);
    EXPECT_TRUE(compressorStrerror(COMPRESSOR_ERR_DECOMPRESS) != NULL);
    /* Every code, known or not, maps to text. */
    EXPECT_TRUE(compressorStrerror(12345) != NULL);
    /* Two different codes must not read the same. */
    EXPECT_STRNE(compressorStrerror(COMPRESSOR_ERR_COMPRESS), compressorStrerror(COMPRESSOR_ERR_DECOMPRESS));
}

/* ===== max_output_size ===== */

TEST(CompressorAlg, MaxOutputSizeAppliesConfigRange) {
    compressorConfig cfg;
    cfg.min_input_len = 256;
    cfg.max_input_len = 131072;
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, &cfg);
    ASSERT_TRUE(c != NULL);

    /* Below the low end and above the high end are both refused. */
    EXPECT_EQ(c->max_output_size(c, 1), (size_t)0);
    EXPECT_EQ(c->max_output_size(c, 255), (size_t)0);
    EXPECT_EQ(c->max_output_size(c, 131073), (size_t)0);

    /* Both ends of the range are inclusive. */
    EXPECT_GT(c->max_output_size(c, 256), (size_t)0);
    EXPECT_GT(c->max_output_size(c, 131072), (size_t)0);

    freeCompressor(c);
}

TEST(CompressorAlg, MaxOutputSizeZeroMeansNoLimit) {
    compressorConfig cfg;
    cfg.min_input_len = 0;
    cfg.max_input_len = 0;
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, &cfg);
    ASSERT_TRUE(c != NULL);

    /* With both limits at 0 only the backend's own limits apply. */
    EXPECT_GT(c->max_output_size(c, 1), (size_t)0);
    EXPECT_GT(c->max_output_size(c, 100), (size_t)0);
    EXPECT_GT(c->max_output_size(c, 10 * 1024 * 1024), (size_t)0);

    freeCompressor(c);
}

TEST(CompressorAlg, MaxOutputSizeRespectsBackendLimits) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    /* An empty value is never compressed, so 0 is refused even with no config. */
    EXPECT_EQ(c->max_output_size(c, 0), (size_t)0);

    /* LZ4_MAX_INPUT_SIZE is 0x7E000000. One byte past it must be refused. */
    EXPECT_GT(c->max_output_size(c, 0x7E000000), (size_t)0);
    EXPECT_EQ(c->max_output_size(c, (size_t)0x7E000000 + 1), (size_t)0);

    freeCompressor(c);
}

TEST(CompressorAlg, MaxOutputSizeIsAtLeastTheInput) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    /* The bound covers data that does not compress at all, so it can never be
     * smaller than the input. It also grows with the input. */
    size_t sizes[5] = {1, 256, 1024, 65536, 131072};
    size_t previous = 0;
    for (int i = 0; i < 5; i++) {
        size_t bound = c->max_output_size(c, sizes[i]);
        EXPECT_GE(bound, sizes[i]);
        EXPECT_GT(bound, previous);
        previous = bound;
    }
    freeCompressor(c);
}

/* ===== round trip ===== */

TEST(CompressorAlg, RoundTripWithoutDictionary) {
    compressorTestData *d = (compressorTestData *)zmalloc(sizeof(*d));
    compressorTestDataInit(d);

    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    for (int i = 0; i < SAMPLE_COUNT; i++) {
        size_t bound = c->max_output_size(c, d->len[i]);
        ASSERT_GT(bound, (size_t)0);
        char *out = (char *)zmalloc(bound);
        char *back = (char *)zmalloc(d->len[i]);

        size_t n = c->api->compress(c, NULL, d->value[i], d->len[i], out, bound);
        EXPECT_GT(n, (size_t)0);
        EXPECT_LE(n, bound);
        EXPECT_EQ(c->last_error, COMPRESSOR_ERR_NONE);

        size_t back_len = c->api->decompress(c, NULL, out, n, back, d->len[i]);
        EXPECT_EQ(back_len, d->len[i]);
        EXPECT_EQ(memcmp(back, d->value[i], d->len[i]), 0);

        zfree(out);
        zfree(back);
    }
    freeCompressor(c);
    zfree(d);
}

TEST(CompressorAlg, RoundTripWithDictionary) {
    compressorTestData *d = (compressorTestData *)zmalloc(sizeof(*d));
    compressorTestDataInit(d);

    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    void *cdict = c->api->dict_load(d->dict, d->dict_len);
    ASSERT_TRUE(cdict != NULL);

    for (int i = DICT_SAMPLES; i < SAMPLE_COUNT; i++) {
        size_t bound = c->max_output_size(c, d->len[i]);
        ASSERT_GT(bound, (size_t)0);
        char *out = (char *)zmalloc(bound);
        char *back = (char *)zmalloc(d->len[i]);

        size_t n = c->api->compress(c, cdict, d->value[i], d->len[i], out, bound);
        EXPECT_GT(n, (size_t)0);

        size_t back_len = c->api->decompress(c, cdict, out, n, back, d->len[i]);
        EXPECT_EQ(back_len, d->len[i]);
        EXPECT_EQ(memcmp(back, d->value[i], d->len[i]), 0);

        zfree(out);
        zfree(back);
    }
    c->api->dict_free(cdict);
    freeCompressor(c);
    zfree(d);
}

/* This is the reason the whole feature exists: a few hundred bytes of JSON have
 * almost no repeats inside one value, so a dictionary is what makes them shrink. */
TEST(CompressorAlg, DictionaryBeatsNoDictionaryOnSmallValues) {
    compressorTestData *d = (compressorTestData *)zmalloc(sizeof(*d));
    compressorTestDataInit(d);

    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    void *cdict = c->api->dict_load(d->dict, d->dict_len);
    ASSERT_TRUE(cdict != NULL);

    size_t total_plain = 0, total_no_dict = 0, total_with_dict = 0;
    for (int i = DICT_SAMPLES; i < SAMPLE_COUNT; i++) {
        size_t bound = c->max_output_size(c, d->len[i]);
        ASSERT_GT(bound, (size_t)0);
        char *no_dict = (char *)zmalloc(bound);
        char *with_dict = (char *)zmalloc(bound);

        total_plain += d->len[i];
        total_no_dict += c->api->compress(c, NULL, d->value[i], d->len[i], no_dict, bound);
        total_with_dict += c->api->compress(c, cdict, d->value[i], d->len[i], with_dict, bound);

        zfree(no_dict);
        zfree(with_dict);
    }

    /* Every sample is in the range this feature targets. */
    EXPECT_GE(total_plain / (SAMPLE_COUNT - DICT_SAMPLES), (size_t)256);
    /* Without a dictionary these values barely shrink. */
    EXPECT_GT(total_no_dict * 100 / total_plain, (size_t)50);
    /* With one they shrink a lot. Half the original size is a loose floor: the
     * measured figure is near 13%, so this will not flake on small LZ4 changes. */
    EXPECT_LT(total_with_dict * 2, total_plain);
    EXPECT_LT(total_with_dict, total_no_dict);

    c->api->dict_free(cdict);
    freeCompressor(c);
    zfree(d);
}

/* A dictionary is read only after dict_load, so instances on different threads
 * share one. Here two instances stand in for two threads. */
TEST(CompressorAlg, DictionaryIsSharedBetweenInstances) {
    compressorTestData *d = (compressorTestData *)zmalloc(sizeof(*d));
    compressorTestDataInit(d);

    compressorAlg *writer = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    compressorAlg *reader = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(writer != NULL);
    ASSERT_TRUE(reader != NULL);
    EXPECT_TRUE(writer->state != reader->state); /* separate scratch buffers */

    void *cdict = writer->api->dict_load(d->dict, d->dict_len);
    ASSERT_TRUE(cdict != NULL);

    size_t bound = writer->max_output_size(writer, d->len[40]);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(d->len[40]);

    size_t n = writer->api->compress(writer, cdict, d->value[40], d->len[40], out, bound);
    EXPECT_GT(n, (size_t)0);

    size_t back_len = reader->api->decompress(reader, cdict, out, n, back, d->len[40]);
    EXPECT_EQ(back_len, d->len[40]);
    EXPECT_EQ(memcmp(back, d->value[40], d->len[40]), 0);

    zfree(out);
    zfree(back);
    reader->api->dict_free(cdict);
    freeCompressor(writer);
    freeCompressor(reader);
    zfree(d);
}

/* ===== dictionary loading ===== */

TEST(CompressorAlg, DictLoadRejectsEmptyInput) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    EXPECT_TRUE(c->api->dict_load(NULL, 100) == NULL);
    EXPECT_TRUE(c->api->dict_load("abc", 0) == NULL);
    freeCompressor(c);
}

/* LZ4 cannot hash a dictionary shorter than one hash unit, so dict_load refuses
 * anything under COMPRESSOR_LZ4_DICT_MIN bytes. The floor must be exact: one byte
 * below is refused and the floor itself is accepted. */
TEST(CompressorAlg, DictLoadRejectsTooSmallDictionary) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    char buf[COMPRESSOR_LZ4_DICT_MIN + 1];
    memset(buf, 'k', sizeof(buf));

    for (size_t len = 1; len < COMPRESSOR_LZ4_DICT_MIN; len++) {
        EXPECT_TRUE(c->api->dict_load(buf, len) == NULL) << "len=" << len << " should be refused";
    }

    /* At the floor and above, the load succeeds and the dictionary is usable. */
    for (size_t len = COMPRESSOR_LZ4_DICT_MIN; len <= COMPRESSOR_LZ4_DICT_MIN + 1; len++) {
        void *cdict = c->api->dict_load(buf, len);
        ASSERT_TRUE(cdict != NULL) << "len=" << len << " should be accepted";

        const char *val = "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk";
        size_t val_len = strlen(val);
        size_t bound = c->max_output_size(c, val_len);
        ASSERT_GT(bound, (size_t)0);
        char *out = (char *)zmalloc(bound);
        char *back = (char *)zmalloc(val_len);
        size_t n = c->api->compress(c, cdict, val, val_len, out, bound);
        EXPECT_GT(n, (size_t)0);
        EXPECT_EQ(c->api->decompress(c, cdict, out, n, back, val_len), val_len);
        EXPECT_EQ(memcmp(back, val, val_len), 0);
        zfree(out);
        zfree(back);
        c->api->dict_free(cdict);
    }
    freeCompressor(c);
}

/* Values over 4 KiB with a dictionary attached take a different path inside LZ4:
 * LZ4_compress_fast_continue() copies the dictionary tables over the whole working
 * stream and switches from usingDictCtx to usingExtDict. Every other dictionary
 * test here uses values of a few hundred bytes, so without this test that branch
 * never runs. compression-max-value-size defaults to 128 KiB, so it matters. */
TEST(CompressorAlg, RoundTripWithDictionaryAboveFourKiB) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    size_t dict_len = 40000;
    char *dict = (char *)zmalloc(dict_len);
    for (size_t i = 0; i < dict_len; i++) dict[i] = (char)('a' + (i * 7 % 26));
    void *cdict = c->api->dict_load(dict, dict_len);
    ASSERT_TRUE(cdict != NULL);

    /* Around the 4 KiB boundary, and up to the default value size cap. */
    size_t sizes[6] = {4095, 4096, 4097, 8192, 65536, 131072};
    for (int i = 0; i < 6; i++) {
        size_t len = sizes[i];
        char *val = (char *)zmalloc(len);
        for (size_t j = 0; j < len; j++) val[j] = (char)('a' + ((j * 7 + 3) % 26));

        size_t bound = c->max_output_size(c, len);
        ASSERT_GT(bound, (size_t)0);
        char *out = (char *)zmalloc(bound);
        char *again = (char *)zmalloc(bound);
        char *back = (char *)zmalloc(len);

        size_t n = c->api->compress(c, cdict, val, len, out, bound);
        EXPECT_GT(n, (size_t)0);
        EXPECT_EQ(c->api->decompress(c, cdict, out, n, back, len), len);
        EXPECT_EQ(memcmp(back, val, len), 0);

        /* The copy that LZ4 makes on this path must not leave state behind that
         * changes the next call on the same instance. */
        size_t n2 = c->api->compress(c, cdict, val, len, again, bound);
        EXPECT_EQ(n2, n);
        EXPECT_EQ(memcmp(again, out, n), 0);

        zfree(val);
        zfree(out);
        zfree(again);
        zfree(back);
    }
    c->api->dict_free(cdict);
    zfree(dict);
    freeCompressor(c);
}

/* One instance alternating between a large value and a small one, so the two LZ4
 * paths run one after the other on the same working stream. */
TEST(CompressorAlg, MixedLargeAndSmallValuesOnOneInstance) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    size_t dict_len = 8000;
    char *dict = (char *)zmalloc(dict_len);
    memset(dict, 'q', dict_len);
    void *cdict = c->api->dict_load(dict, dict_len);
    ASSERT_TRUE(cdict != NULL);

    size_t big_len = 20000, small_len = 300;
    char *big = (char *)zmalloc(big_len);
    char *small = (char *)zmalloc(small_len);
    memset(big, 'q', big_len);
    memset(small, 'r', small_len);

    for (int round = 0; round < 3; round++) {
        size_t big_bound = c->max_output_size(c, big_len);
        size_t small_bound = c->max_output_size(c, small_len);
        char *big_out = (char *)zmalloc(big_bound);
        char *small_out = (char *)zmalloc(small_bound);
        char *big_back = (char *)zmalloc(big_len);
        char *small_back = (char *)zmalloc(small_len);

        size_t bn = c->api->compress(c, cdict, big, big_len, big_out, big_bound);
        size_t sn = c->api->compress(c, cdict, small, small_len, small_out, small_bound);
        EXPECT_GT(bn, (size_t)0);
        EXPECT_GT(sn, (size_t)0);

        EXPECT_EQ(c->api->decompress(c, cdict, big_out, bn, big_back, big_len), big_len);
        EXPECT_EQ(memcmp(big_back, big, big_len), 0);
        EXPECT_EQ(c->api->decompress(c, cdict, small_out, sn, small_back, small_len), small_len);
        EXPECT_EQ(memcmp(small_back, small, small_len), 0);

        zfree(big_out);
        zfree(small_out);
        zfree(big_back);
        zfree(small_back);
    }
    zfree(big);
    zfree(small);
    c->api->dict_free(cdict);
    zfree(dict);
    freeCompressor(c);
}

/* dict_load copies the bytes, so the caller may free the source right after. */
TEST(CompressorAlg, DictLoadCopiesTheSourceBytes) {
    compressorTestData *d = (compressorTestData *)zmalloc(sizeof(*d));
    compressorTestDataInit(d);

    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    char *source = (char *)zmalloc(d->dict_len);
    memcpy(source, d->dict, d->dict_len);
    void *cdict = c->api->dict_load(source, d->dict_len);
    ASSERT_TRUE(cdict != NULL);

    /* Overwrite and free the source. The dictionary must be unaffected. */
    memset(source, 0, d->dict_len);
    zfree(source);

    size_t bound = c->max_output_size(c, d->len[40]);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(d->len[40]);
    size_t n = c->api->compress(c, cdict, d->value[40], d->len[40], out, bound);
    EXPECT_GT(n, (size_t)0);
    EXPECT_EQ(c->api->decompress(c, cdict, out, n, back, d->len[40]), d->len[40]);
    EXPECT_EQ(memcmp(back, d->value[40], d->len[40]), 0);

    zfree(out);
    zfree(back);
    c->api->dict_free(cdict);
    freeCompressor(c);
    zfree(d);
}

TEST(CompressorAlg, DictLoadTrimsOversizedDictionary) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    /* LZ4 uses only the last 64 KiB. A larger dictionary must be trimmed and
     * accepted, not rejected. */
    size_t big_len = (size_t)COMPRESSOR_LZ4_DICT_MAX * 3;
    char *big = (char *)zmalloc(big_len);
    memset(big, 'q', big_len);

    void *cdict = c->api->dict_load(big, big_len);
    EXPECT_TRUE(cdict != NULL);

    zfree(big); /* dict_load copies, so freeing the source now must be safe */

    /* The trimmed dictionary still works for a full round trip. */
    const char *val = "qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq";
    size_t val_len = strlen(val);
    size_t bound = c->max_output_size(c, val_len);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(val_len);
    size_t n = c->api->compress(c, cdict, val, val_len, out, bound);
    EXPECT_GT(n, (size_t)0);
    EXPECT_EQ(c->api->decompress(c, cdict, out, n, back, val_len), val_len);
    EXPECT_EQ(memcmp(back, val, val_len), 0);

    zfree(out);
    zfree(back);
    c->api->dict_free(cdict);
    freeCompressor(c);
}

TEST(CompressorAlg, DictFreeAcceptsNull) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    c->api->dict_free(NULL);
    freeCompressor(c);
}

/* ===== failure paths ===== */

TEST(CompressorAlg, CompressFailsWhenDestinationTooSmall) {
    compressorTestData *d = (compressorTestData *)zmalloc(sizeof(*d));
    compressorTestDataInit(d);

    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    char tiny[4];
    size_t n = c->api->compress(c, NULL, d->value[0], d->len[0], tiny, sizeof(tiny));
    EXPECT_EQ(n, (size_t)0);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_COMPRESS);

    /* The instance must still work after a failure, and the error must clear. */
    size_t bound = c->max_output_size(c, d->len[0]);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    EXPECT_GT(c->api->compress(c, NULL, d->value[0], d->len[0], out, bound), (size_t)0);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_NONE);

    zfree(out);
    freeCompressor(c);
    zfree(d);
}

TEST(CompressorAlg, CompressRejectsEmptyInput) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    char out[64];
    EXPECT_EQ(c->api->compress(c, NULL, "abc", 0, out, sizeof(out)), (size_t)0);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_BAD_SIZE);
    freeCompressor(c);
}

TEST(CompressorAlg, DecompressRejectsBadSizes) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    char out[64];

    EXPECT_EQ(c->api->decompress(c, NULL, "abc", 0, out, sizeof(out)), (size_t)0);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_BAD_SIZE);

    EXPECT_EQ(c->api->decompress(c, NULL, "abc", 3, out, 0), (size_t)0);
    EXPECT_EQ(c->last_error, COMPRESSOR_ERR_BAD_SIZE);

    freeCompressor(c);
}

TEST(CompressorAlg, DecompressFailsOnCorruptBody) {
    compressorTestData *d = (compressorTestData *)zmalloc(sizeof(*d));
    compressorTestDataInit(d);

    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    size_t bound = c->max_output_size(c, d->len[0]);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(d->len[0]);
    size_t n = c->api->compress(c, NULL, d->value[0], d->len[0], out, bound);
    ASSERT_GT(n, (size_t)0);

    /* Damage the body. LZ4's safe decoder must refuse it or return the wrong
     * length. It must never write past dstcap, which the sanitizer builds check. */
    out[n / 2] = (char)(out[n / 2] ^ 0xFF);
    size_t back_len = c->api->decompress(c, NULL, out, n, back, d->len[0]);
    if (back_len == d->len[0]) {
        /* A one-bit change can still decode to a valid but different value. */
        EXPECT_NE(memcmp(back, d->value[0], d->len[0]), 0);
    } else {
        EXPECT_EQ(back_len, (size_t)0);
        EXPECT_EQ(c->last_error, COMPRESSOR_ERR_DECOMPRESS);
    }

    zfree(out);
    zfree(back);
    freeCompressor(c);
    zfree(d);
}

TEST(CompressorAlg, DecompressWithWrongDictionaryDoesNotReturnTheValue) {
    compressorTestData *d = (compressorTestData *)zmalloc(sizeof(*d));
    compressorTestDataInit(d);

    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    void *right = c->api->dict_load(d->dict, d->dict_len);
    char other[4096];
    memset(other, 'x', sizeof(other));
    void *wrong = c->api->dict_load(other, sizeof(other));
    ASSERT_TRUE(right != NULL);
    ASSERT_TRUE(wrong != NULL);

    size_t bound = c->max_output_size(c, d->len[40]);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(d->len[40]);
    size_t n = c->api->compress(c, right, d->value[40], d->len[40], out, bound);
    ASSERT_GT(n, (size_t)0);

    /* Decoding with the wrong dictionary version must not hand back the value.
     * This is why every frame stores a dict_id. */
    size_t back_len = c->api->decompress(c, wrong, out, n, back, d->len[40]);
    if (back_len == d->len[40]) {
        EXPECT_NE(memcmp(back, d->value[40], d->len[40]), 0);
    } else {
        EXPECT_EQ(back_len, (size_t)0);
    }

    zfree(out);
    zfree(back);
    c->api->dict_free(right);
    c->api->dict_free(wrong);
    freeCompressor(c);
    zfree(d);
}

/* ===== data shapes ===== */

TEST(CompressorAlg, RoundTripOnIncompressibleData) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    /* Random bytes do not shrink. The frame may grow, which is what the bound is
     * for, and the net-savings guard above this layer throws such frames away. */
    size_t len = 4096;
    char *val = (char *)zmalloc(len);
    for (size_t i = 0; i < len; i++) val[i] = (char)(rand() & 0xFF);

    size_t bound = c->max_output_size(c, len);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(len);

    size_t n = c->api->compress(c, NULL, val, len, out, bound);
    EXPECT_GT(n, (size_t)0);
    EXPECT_LE(n, bound);
    EXPECT_EQ(c->api->decompress(c, NULL, out, n, back, len), len);
    EXPECT_EQ(memcmp(back, val, len), 0);

    zfree(val);
    zfree(out);
    zfree(back);
    freeCompressor(c);
}

TEST(CompressorAlg, RoundTripOnHighlyCompressibleData) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    size_t len = 8192;
    char *val = (char *)zmalloc(len);
    memset(val, 'a', len);

    size_t bound = c->max_output_size(c, len);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(len);

    size_t n = c->api->compress(c, NULL, val, len, out, bound);
    EXPECT_GT(n, (size_t)0);
    EXPECT_LT(n * 100, len); /* well under 1% of the original */
    EXPECT_EQ(c->api->decompress(c, NULL, out, n, back, len), len);
    EXPECT_EQ(memcmp(back, val, len), 0);

    zfree(val);
    zfree(out);
    zfree(back);
    freeCompressor(c);
}

TEST(CompressorAlg, RoundTripAtSizeBoundaries) {
    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);

    /* One byte, the smallest the backend accepts, up to the 128 KiB cap. */
    size_t sizes[6] = {1, 2, 255, 256, 65535, 131072};
    for (int i = 0; i < 6; i++) {
        size_t len = sizes[i];
        char *val = (char *)zmalloc(len);
        for (size_t j = 0; j < len; j++) val[j] = (char)('a' + (j % 26));

        size_t bound = c->max_output_size(c, len);
        ASSERT_GT(bound, (size_t)0);
        char *out = (char *)zmalloc(bound);
        char *back = (char *)zmalloc(len);

        size_t n = c->api->compress(c, NULL, val, len, out, bound);
        EXPECT_GT(n, (size_t)0);
        EXPECT_EQ(c->api->decompress(c, NULL, out, n, back, len), len);
        EXPECT_EQ(memcmp(back, val, len), 0);

        zfree(val);
        zfree(out);
        zfree(back);
    }
    freeCompressor(c);
}

/* The same instance is reused for many values in a row, so a leftover from one
 * call must never change the next. */
TEST(CompressorAlg, RepeatedUseOfOneInstanceIsStable) {
    compressorTestData *d = (compressorTestData *)zmalloc(sizeof(*d));
    compressorTestDataInit(d);

    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    void *cdict = c->api->dict_load(d->dict, d->dict_len);
    ASSERT_TRUE(cdict != NULL);

    size_t bound = c->max_output_size(c, d->len[40]);
    ASSERT_GT(bound, (size_t)0);
    char *first = (char *)zmalloc(bound);
    char *again = (char *)zmalloc(bound);

    size_t n1 = c->api->compress(c, cdict, d->value[40], d->len[40], first, bound);
    ASSERT_GT(n1, (size_t)0);

    /* Compress other values in between, then the same value once more. The output
     * must be byte for byte what it was the first time. */
    for (int i = 41; i < SAMPLE_COUNT; i++) {
        char *scratch = (char *)zmalloc(c->max_output_size(c, d->len[i]));
        c->api->compress(c, cdict, d->value[i], d->len[i], scratch, c->max_output_size(c, d->len[i]));
        zfree(scratch);
    }
    size_t n2 = c->api->compress(c, cdict, d->value[40], d->len[40], again, bound);
    EXPECT_EQ(n2, n1);
    EXPECT_EQ(memcmp(again, first, n1), 0);

    zfree(first);
    zfree(again);
    c->api->dict_free(cdict);
    freeCompressor(c);
    zfree(d);
}

/* Compressing with a dictionary and decompressing without it must fail, and the
 * other way round too. The dictionary is not optional per frame. */
TEST(CompressorAlg, DictionaryMismatchBetweenCompressAndDecompress) {
    compressorTestData *d = (compressorTestData *)zmalloc(sizeof(*d));
    compressorTestDataInit(d);

    compressorAlg *c = newCompressor(COMPRESSOR_ALG_LZ4, NULL);
    ASSERT_TRUE(c != NULL);
    void *cdict = c->api->dict_load(d->dict, d->dict_len);
    ASSERT_TRUE(cdict != NULL);

    size_t bound = c->max_output_size(c, d->len[40]);
    ASSERT_GT(bound, (size_t)0);
    char *out = (char *)zmalloc(bound);
    char *back = (char *)zmalloc(d->len[40]);

    size_t n = c->api->compress(c, cdict, d->value[40], d->len[40], out, bound);
    ASSERT_GT(n, (size_t)0);
    size_t back_len = c->api->decompress(c, NULL, out, n, back, d->len[40]);
    if (back_len == d->len[40]) {
        EXPECT_NE(memcmp(back, d->value[40], d->len[40]), 0);
    } else {
        EXPECT_EQ(back_len, (size_t)0);
    }

    zfree(out);
    zfree(back);
    c->api->dict_free(cdict);
    freeCompressor(c);
    zfree(d);
}
