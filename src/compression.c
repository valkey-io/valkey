/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression.h"
#include "compression_lz4.h"
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

static const char *const compressionAlgoNameByAlgo[] = {
    [ALGO_NONE] = "none",
    [ALGO_LZF] = "lzf",
    [ALGO_LZ4] = "lz4",
};

static const compressionCodecImpl *const compressionCodecImplByAlgo[] = {
    [ALGO_LZ4] = &compressionLz4CodecImpl,
};

static const char *compressionAlgoNameForAlgo(compressionAlgo algo) {
    unsigned int i = (unsigned int)algo;
    if (i >= sizeof(compressionAlgoNameByAlgo) / sizeof(compressionAlgoNameByAlgo[0])) return NULL;
    return compressionAlgoNameByAlgo[i];
}

static const compressionCodecImpl *compressionCodecImplForAlgo(compressionAlgo algo) {
    unsigned int i = (unsigned int)algo;
    if (i >= sizeof(compressionCodecImplByAlgo) / sizeof(compressionCodecImplByAlgo[0])) return NULL;
    return compressionCodecImplByAlgo[i];
}

bool compressionAlgoSupportsStreaming(compressionAlgo algo) {
    return compressionCodecImplForAlgo(algo) != NULL;
}

const char *compressionAlgoName(compressionAlgo algo) {
    const char *name = compressionAlgoNameForAlgo(algo);
    return name ? name : "unknown";
}

int streamCompressorInit(streamCompressor *sc, compressionAlgo algo, int level) {
    if (!sc) return -1;
    memset(sc, 0, sizeof(*sc));

    const compressionCodecImpl *impl = compressionCodecImplForAlgo(algo);
    if (!impl || !impl->compressor_init) return -1;

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
    const compressionCodecImpl *impl = compressionCodecImplForAlgo(sc->algo);
    if (impl && impl->compressor_destroy) impl->compressor_destroy(sc);
    memset(sc, 0, sizeof(*sc));
}

int streamDecompressorInit(streamDecompressor *sd, compressionAlgo algo) {
    if (!sd) return -1;
    memset(sd, 0, sizeof(*sd));

    const compressionCodecImpl *impl = compressionCodecImplForAlgo(algo);
    if (!impl || !impl->decompressor_init) return -1;

    sd->algo = algo;

    if (impl->decompressor_init(sd) != 0) {
        streamDecompressorDestroy(sd);
        return -1;
    }
    return 0;
}

void streamDecompressorDestroy(streamDecompressor *sd) {
    if (!sd) return;
    const compressionCodecImpl *impl = compressionCodecImplForAlgo(sd->algo);
    if (impl && impl->decompressor_destroy) impl->decompressor_destroy(sd);
    memset(sd, 0, sizeof(*sd));
}

bool streamDecompressorFrameDone(const streamDecompressor *sd) {
    return sd && sd->frame_done;
}

size_t streamDecompressorInputHint(const streamDecompressor *sd) {
    return sd ? sd->input_hint : 0;
}

size_t streamCompressOutputBound(const streamCompressor *sc, size_t input_len) {
    if (!sc) return 0;
    const compressionCodecImpl *impl = compressionCodecImplForAlgo(sc->algo);
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

    const compressionCodecImpl *impl = compressionCodecImplForAlgo(sc->algo);
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

    const compressionCodecImpl *impl = compressionCodecImplForAlgo(sd->algo);
    if (!impl || !impl->decompress_feed) {
        sd->errored = true;
        return -1;
    }

    return impl->decompress_feed(sd, output, output_capacity, input, input_len, input_consumed);
}
