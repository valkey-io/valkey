/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression.h"
#include "compression_lz4.h"
#include <limits.h>
#include <string.h>

typedef struct {
    int (*compressor_init)(streamCompressor *sc);
    void (*compressor_destroy)(streamCompressor *sc);
    int (*decompressor_init)(streamDecompressor *sd);
    void (*decompressor_destroy)(streamDecompressor *sd);
    size_t (*compress_output_bound)(size_t input_len);
    ssize_t (*compress_feed)(streamCompressor *sc,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             compressFlushMode flush_mode);
    ssize_t (*decompress_feed)(streamDecompressor *sd,
                               uint8_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_len,
                               size_t *input_consumed);
} compressionCodecImpl;

static const compressionCodecImpl compressionLz4CodecImpl = {
    .compressor_init = compressionLz4CompressorInit,
    .compressor_destroy = compressionLz4CompressorDestroy,
    .decompressor_init = compressionLz4DecompressorInit,
    .decompressor_destroy = compressionLz4DecompressorDestroy,
    .compress_output_bound = compressionLz4OutputBound,
    .compress_feed = compressionLz4CompressFeed,
    .decompress_feed = compressionLz4DecompressFeed,
};

typedef struct {
    const char *name;
    const compressionCodecImpl *impl;
} compressionAlgoEntry;

static const compressionAlgoEntry compressionAlgoTable[] = {
    [ALGO_NONE] = {"none", NULL},
    [ALGO_LZF] = {"lzf", NULL},
    [ALGO_LZ4] = {"lz4", &compressionLz4CodecImpl},
};

static const compressionAlgoEntry *compressionAlgoEntryForAlgo(compressionAlgo algo) {
    unsigned int i = (unsigned int)algo;
    if (i >= sizeof(compressionAlgoTable) / sizeof(compressionAlgoTable[0])) return NULL;
    return &compressionAlgoTable[i];
}

bool compressionAlgoSupportsStreaming(compressionAlgo algo) {
    const compressionAlgoEntry *entry = compressionAlgoEntryForAlgo(algo);
    return entry && entry->impl != NULL;
}

const char *compressionAlgoName(compressionAlgo algo) {
    const compressionAlgoEntry *entry = compressionAlgoEntryForAlgo(algo);
    return (entry && entry->name) ? entry->name : "unknown";
}

int streamCompressorInit(streamCompressor *sc, compressionAlgo algo, int level) {
    if (!sc) return -1;
    memset(sc, 0, sizeof(*sc));

    const compressionAlgoEntry *entry = compressionAlgoEntryForAlgo(algo);
    const compressionCodecImpl *impl = entry ? entry->impl : NULL;
    if (!impl) return -1;

    sc->algo = algo;
    sc->level = level;

    if (impl->compressor_init(sc) != 0) {
        streamCompressorDestroy(sc);
        return -1;
    }
    return 0;
}

void streamCompressorDestroy(streamCompressor *sc) {
    if (!sc) return;
    const compressionAlgoEntry *entry = compressionAlgoEntryForAlgo(sc->algo);
    const compressionCodecImpl *impl = entry ? entry->impl : NULL;
    if (impl) impl->compressor_destroy(sc);
    memset(sc, 0, sizeof(*sc));
}

int streamDecompressorInit(streamDecompressor *sd, compressionAlgo algo) {
    if (!sd) return -1;
    memset(sd, 0, sizeof(*sd));

    const compressionAlgoEntry *entry = compressionAlgoEntryForAlgo(algo);
    const compressionCodecImpl *impl = entry ? entry->impl : NULL;
    if (!impl) return -1;

    sd->algo = algo;

    if (impl->decompressor_init(sd) != 0) {
        streamDecompressorDestroy(sd);
        return -1;
    }
    return 0;
}

void streamDecompressorDestroy(streamDecompressor *sd) {
    if (!sd) return;
    const compressionAlgoEntry *entry = compressionAlgoEntryForAlgo(sd->algo);
    const compressionCodecImpl *impl = entry ? entry->impl : NULL;
    if (impl) impl->decompressor_destroy(sd);
    memset(sd, 0, sizeof(*sd));
}

size_t streamCompressOutputBound(const streamCompressor *sc, size_t input_len) {
    if (!sc) return 0;
    const compressionAlgoEntry *entry = compressionAlgoEntryForAlgo(sc->algo);
    const compressionCodecImpl *impl = entry ? entry->impl : NULL;
    if (!impl || !impl->compress_output_bound) return 0;
    return impl->compress_output_bound(input_len);
}

ssize_t streamCompressFeed(streamCompressor *sc,
                           uint8_t *output,
                           size_t output_capacity,
                           const uint8_t *input,
                           size_t input_len,
                           compressFlushMode flush_mode) {
    if (!sc || !output) return -1;
    if (sc->errored) return -1;
    if (input_len > 0 && !input) return -1;

    const compressionAlgoEntry *entry = compressionAlgoEntryForAlgo(sc->algo);
    const compressionCodecImpl *impl = entry ? entry->impl : NULL;
    if (!impl || !impl->compress_feed) return -1;

    return impl->compress_feed(sc, output, output_capacity, input, input_len, flush_mode);
}

ssize_t streamDecompressFeed(streamDecompressor *sd,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             size_t *input_consumed) {
    if (!sd || !input_consumed) return -1;
    if (sd->errored) return -1;
    *input_consumed = 0;
    if (sd->frame_done) return 0;
    if (input_len > 0 && !input) {
        sd->errored = true;
        return -1;
    }
    /* Zero output capacity would let streaming loops spin forever. */
    if (!output || output_capacity == 0) {
        sd->errored = true;
        return -1;
    }
    if (output_capacity > (size_t)SSIZE_MAX) {
        sd->errored = true;
        return -1;
    }

    const compressionAlgoEntry *entry = compressionAlgoEntryForAlgo(sd->algo);
    const compressionCodecImpl *impl = entry ? entry->impl : NULL;
    if (!impl || !impl->decompress_feed) {
        sd->errored = true;
        return -1;
    }

    return impl->decompress_feed(sd, output, output_capacity, input, input_len, input_consumed);
}
