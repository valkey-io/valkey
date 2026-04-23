/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Streaming compression/decompression dispatch.
 * Codec-specific implementations live in implementation files. */

#include "compression.h"
#include "compression_lz4.h"
#include <string.h>

typedef struct {
    int (*compressor_init)(stream_compressor_t *sc);
    void (*compressor_destroy)(stream_compressor_t *sc);
    int (*decompressor_init)(stream_decompressor_t *sd);
    void (*decompressor_destroy)(stream_decompressor_t *sd);
    size_t (*compress_output_bound)(size_t input_len);
    ssize_t (*compress_feed)(stream_compressor_t *sc,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             compress_flush_mode_t flush_mode);
    ssize_t (*decompress_feed)(stream_decompressor_t *sd,
                               uint8_t *output,
                               size_t output_capacity,
                               const uint8_t *input,
                               size_t input_len,
                               size_t *input_consumed);
} compression_codec_impl_t;

static const compression_codec_impl_t compression_lz4_codec_impl = {
    .compressor_init = compressionLz4CompressorInit,
    .compressor_destroy = compressionLz4CompressorDestroy,
    .decompressor_init = compressionLz4DecompressorInit,
    .decompressor_destroy = compressionLz4DecompressorDestroy,
    .compress_output_bound = compressionLz4OutputBound,
    .compress_feed = compressionLz4CompressFeed,
    .decompress_feed = compressionLz4DecompressFeed,
};

static const char *const compression_algo_name_by_algo[] = {
    [ALGO_NONE] = "none",
    [ALGO_LZF] = "lzf",
    [ALGO_LZ4] = "lz4",
};

static const compression_codec_impl_t *const compression_codec_impl_by_algo[] = {
    [ALGO_LZ4] = &compression_lz4_codec_impl,
};

static const char *compressionAlgoNameForAlgo(compression_algo_t algo) {
    unsigned int algo_index = (unsigned int)algo;

    if (algo_index >= sizeof(compression_algo_name_by_algo) / sizeof(compression_algo_name_by_algo[0])) {
        return NULL;
    }
    return compression_algo_name_by_algo[algo_index];
}

static const compression_codec_impl_t *compressionCodecImplForAlgo(compression_algo_t algo) {
    unsigned int algo_index = (unsigned int)algo;

    if (algo_index >= sizeof(compression_codec_impl_by_algo) / sizeof(compression_codec_impl_by_algo[0])) {
        return NULL;
    }
    return compression_codec_impl_by_algo[algo_index];
}

bool compressionAlgoSupportsStreaming(compression_algo_t algo) {
    return compressionCodecImplForAlgo(algo) != NULL;
}

const char *compressionAlgoName(compression_algo_t algo) {
    const char *algo_name = compressionAlgoNameForAlgo(algo);

    return algo_name ? algo_name : "unknown";
}

int streamCompressorInit(stream_compressor_t *sc, compression_algo_t algo, int level) {
    if (!sc) return -1;
    memset(sc, 0, sizeof(*sc));

    const compression_codec_impl_t *codec_impl = compressionCodecImplForAlgo(algo);
    if (!codec_impl || !codec_impl->compressor_init) return -1;

    sc->algo = algo;
    sc->level = level;

    if (codec_impl->compressor_init(sc) != 0) {
        streamCompressorDestroy(sc);
        return -1;
    }
    return 0;
}

void streamCompressorDestroy(stream_compressor_t *sc) {
    if (!sc) return;

    const compression_codec_impl_t *codec_impl = compressionCodecImplForAlgo(sc->algo);
    if (codec_impl && codec_impl->compressor_destroy) {
        codec_impl->compressor_destroy(sc);
    }
    memset(sc, 0, sizeof(*sc));
}

int streamDecompressorInit(stream_decompressor_t *sd, compression_algo_t algo) {
    if (!sd) return -1;
    memset(sd, 0, sizeof(*sd));

    const compression_codec_impl_t *codec_impl = compressionCodecImplForAlgo(algo);
    if (!codec_impl || !codec_impl->decompressor_init) return -1;

    sd->algo = algo;

    if (codec_impl->decompressor_init(sd) != 0) {
        streamDecompressorDestroy(sd);
        return -1;
    }
    return 0;
}

void streamDecompressorDestroy(stream_decompressor_t *sd) {
    if (!sd) return;

    const compression_codec_impl_t *codec_impl = compressionCodecImplForAlgo(sd->algo);
    if (codec_impl && codec_impl->decompressor_destroy) {
        codec_impl->decompressor_destroy(sd);
    }
    memset(sd, 0, sizeof(*sd));
}

bool streamDecompressorFrameDone(const stream_decompressor_t *sd) {
    return sd && sd->frame_done;
}

size_t streamCompressOutputBound(const stream_compressor_t *sc, size_t input_len) {
    if (!sc) return 0;
    const compression_codec_impl_t *codec_impl = compressionCodecImplForAlgo(sc->algo);
    if (!codec_impl || !codec_impl->compress_output_bound) return 0;
    return codec_impl->compress_output_bound(input_len);
}

ssize_t streamCompressFeed(stream_compressor_t *sc,
                           uint8_t *output,
                           size_t output_capacity,
                           const uint8_t *input,
                           size_t input_len,
                           compress_flush_mode_t flush_mode) {
    if (!sc || !output) return -1;
    if (sc->errored) return -1;
    if (input_len > 0 && !input) return -1;

    const compression_codec_impl_t *codec_impl = compressionCodecImplForAlgo(sc->algo);
    if (!codec_impl || !codec_impl->compress_feed) return -1;

    return codec_impl->compress_feed(sc, output, output_capacity,
                                     input, input_len, flush_mode);
}

ssize_t streamDecompressFeed(stream_decompressor_t *sd,
                             uint8_t *output,
                             size_t output_capacity,
                             const uint8_t *input,
                             size_t input_len,
                             size_t *input_consumed) {
    if (!sd || !input_consumed) return -1;
    if (sd->errored) return -1;
    *input_consumed = 0;
    if (sd->frame_done) return 0;
    /* Zero output capacity is a caller bug — returning 0 with no progress
     * would cause streaming loops to spin forever. */
    if (!output || output_capacity == 0) {
        sd->errored = true;
        return -1;
    }

    const compression_codec_impl_t *codec_impl = compressionCodecImplForAlgo(sd->algo);
    if (!codec_impl || !codec_impl->decompress_feed) {
        sd->errored = true;
        return -1;
    }

    return codec_impl->decompress_feed(sd, output, output_capacity,
                                       input, input_len, input_consumed);
}
