/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_stream.h"
#include "zmalloc.h"
#include <limits.h>
#include <string.h>

/* ===== VKCS envelope ===== */

typedef enum {
    VKCS_PROBE_NEED_INPUT = 0,
    VKCS_PROBE_PASSTHROUGH = 1,
    VKCS_PROBE_COMPRESSED = 2,
    VKCS_PROBE_ERROR = 3,
} vkcsProbeResult;

typedef struct {
    bool allow_passthrough;
    uint8_t expected_stream_kind;
} vkcsProbeConfig;

typedef struct {
    uint8_t header[VKCS_ENVELOPE_SIZE];
    size_t header_len;
    bool ready;
    bool compressed;
    bool codec_checksum_enabled;
    compressionAlgo algo;
    uint8_t stream_kind;
} vkcsProbe;

static bool vkcsCodecIsSupported(vkcsCodec codec) {
    return codec == VKCS_CODEC_LZ4;
}

static bool vkcsProbeHasMagicPrefix(const vkcsProbe *probe) {
    if (probe->header_len == 0) return false;
    size_t magic_prefix_len = probe->header_len < VKCS_MAGIC_SIZE ? probe->header_len : VKCS_MAGIC_SIZE;
    return memcmp(probe->header, "VKCS", magic_prefix_len) == 0;
}

static void vkcsProbeSetPassthrough(vkcsProbe *probe) {
    probe->ready = true;
    probe->compressed = false;
    probe->codec_checksum_enabled = false;
    probe->algo = ALGO_NONE;
    probe->stream_kind = 0;
}

static void vkcsProbeSetCompressed(vkcsProbe *probe,
                                   compressionAlgo algo,
                                   uint8_t stream_kind,
                                   bool codec_checksum_enabled) {
    probe->ready = true;
    probe->compressed = true;
    probe->codec_checksum_enabled = codec_checksum_enabled;
    probe->algo = algo;
    probe->stream_kind = stream_kind;
}

static int compressionAlgoToVkcsCodec(compressionAlgo algo, vkcsCodec *codec) {
    switch (algo) {
    case ALGO_LZ4: *codec = VKCS_CODEC_LZ4; return 0;
    default: return -1;
    }
}

static int vkcsCodecToCompressionAlgo(vkcsCodec codec, compressionAlgo *algo) {
    switch (codec) {
    case VKCS_CODEC_LZ4: *algo = ALGO_LZ4; return 0;
    default: return -1;
    }
}

static int writeVkcsEnvelope(vkcsEmitFn emit_cb,
                             void *ctx,
                             vkcsCodec codec,
                             uint8_t stream_kind,
                             bool codec_checksum_enabled) {
    if (!emit_cb || !vkcsCodecIsSupported(codec)) return -1;

    uint8_t envelope[VKCS_ENVELOPE_SIZE] = {
        VKCS_MAGIC_0,
        VKCS_MAGIC_1,
        VKCS_MAGIC_2,
        VKCS_MAGIC_3,
        VKCS_VERSION,
        (uint8_t)codec,
        codec_checksum_enabled ? VKCS_FLAG_CODEC_CHECKSUM : 0,
        stream_kind,
    };
    return emit_cb(ctx, envelope, VKCS_ENVELOPE_SIZE) == 0 ? 0 : -1;
}

/* Rejects unknown flag bits so a future format extension fails loud rather
 * than silently corrupting load. */
static int readVkcsEnvelope(const uint8_t *buf,
                            size_t len,
                            vkcsCodec *codec,
                            uint8_t *stream_kind,
                            bool *codec_checksum_enabled) {
    if (!buf || len < VKCS_ENVELOPE_SIZE) return -1;

    if (memcmp(buf, "VKCS", VKCS_MAGIC_SIZE) != 0) return -1;
    if (buf[4] != VKCS_VERSION) return -1;

    vkcsCodec parsed_codec = (vkcsCodec)buf[5];
    if (!vkcsCodecIsSupported(parsed_codec)) return -1;

    uint8_t flags = buf[6];
    if (flags & ~VKCS_FLAG_CODEC_CHECKSUM) return -1;

    if (codec) *codec = parsed_codec;
    if (stream_kind) *stream_kind = buf[7];
    if (codec_checksum_enabled) *codec_checksum_enabled = (flags & VKCS_FLAG_CODEC_CHECKSUM) != 0;
    return 0;
}

int streamReadEnvelopeInfo(const uint8_t *buf,
                           size_t len,
                           uint8_t expected_stream_kind,
                           streamReaderInfo *info) {
    vkcsCodec codec;
    uint8_t stream_kind = 0;
    bool codec_checksum_enabled = false;
    compressionAlgo algo = ALGO_NONE;

    if (!info || len < VKCS_ENVELOPE_SIZE ||
        readVkcsEnvelope(buf, len, &codec, &stream_kind, &codec_checksum_enabled) != 0 ||
        stream_kind != expected_stream_kind ||
        vkcsCodecToCompressionAlgo(codec, &algo) != 0) {
        return -1;
    }

    info->compressed = true;
    info->codec_checksum_enabled = codec_checksum_enabled;
    info->algo = algo;
    info->stream_kind = stream_kind;
    return 0;
}

static void vkcsProbeInit(vkcsProbe *probe) {
    memset(probe, 0, sizeof(*probe));
    probe->algo = ALGO_NONE;
}

/* Incremental: wrapped rios may legally return fewer than VKCS_ENVELOPE_SIZE
 * bytes per read. Consumed bytes are retained in probe->header so passthrough
 * can replay them exactly. */
static vkcsProbeResult vkcsProbeFeed(vkcsProbe *probe,
                                     const vkcsProbeConfig *cfg,
                                     const uint8_t *src,
                                     size_t src_len,
                                     bool input_eof,
                                     size_t *src_consumed) {
    size_t consumed = 0;
    *src_consumed = 0;
    if (probe->ready) {
        return probe->compressed ? VKCS_PROBE_COMPRESSED : VKCS_PROBE_PASSTHROUGH;
    }

    while (consumed < src_len) {
        size_t target = probe->header_len < VKCS_MAGIC_SIZE ? VKCS_MAGIC_SIZE : VKCS_ENVELOPE_SIZE;
        size_t need = target - probe->header_len;
        size_t take = src_len - consumed < need ? src_len - consumed : need;

        memcpy(probe->header + probe->header_len, src + consumed, take);
        probe->header_len += take;
        consumed += take;

        if (probe->header_len >= VKCS_MAGIC_SIZE && memcmp(probe->header, "VKCS", VKCS_MAGIC_SIZE) != 0) {
            *src_consumed = consumed;
            if (!cfg->allow_passthrough) return VKCS_PROBE_ERROR;
            vkcsProbeSetPassthrough(probe);
            return VKCS_PROBE_PASSTHROUGH;
        }

        if (probe->header_len == VKCS_ENVELOPE_SIZE) {
            streamReaderInfo info = {0};
            if (streamReadEnvelopeInfo(probe->header, VKCS_ENVELOPE_SIZE,
                                       cfg->expected_stream_kind, &info) != 0) {
                *src_consumed = consumed;
                return VKCS_PROBE_ERROR;
            }
            vkcsProbeSetCompressed(probe, info.algo, info.stream_kind,
                                   info.codec_checksum_enabled);
            *src_consumed = consumed;
            return VKCS_PROBE_COMPRESSED;
        }
    }

    if (input_eof) {
        *src_consumed = consumed;
        /* EOF mid-magic looks like a truncated VKCS, not a valid passthrough. */
        if (vkcsProbeHasMagicPrefix(probe)) return VKCS_PROBE_ERROR;
        if (!cfg->allow_passthrough) return VKCS_PROBE_ERROR;
        vkcsProbeSetPassthrough(probe);
        return VKCS_PROBE_PASSTHROUGH;
    }

    *src_consumed = consumed;
    return VKCS_PROBE_NEED_INPUT;
}

/* ===== Streaming writer ===== */

#define STREAM_WRITER_INPUT_CHUNK_SIZE (1024 * 1024)

struct streamWriter {
    streamCompressor compressor;
    uint8_t *out_buf;
    size_t out_buf_size;
    vkcsEmitFn emit_cb;
    void *emit_ctx;
    uint8_t stream_kind;
    bool envelope_written;
    bool finished;
    bool errored;
    uint64_t bytes_emitted;
};

static int streamWriterInitContext(streamWriter *t,
                                   const streamWriterConfig *cfg,
                                   vkcsEmitFn emit_cb,
                                   void *emit_ctx) {
    memset(t, 0, sizeof(*t));
    t->emit_cb = emit_cb;
    t->emit_ctx = emit_ctx;
    t->stream_kind = cfg->stream_kind;

    if (streamCompressorInit(&t->compressor, cfg->algo, cfg->level) != 0) return -1;
    t->compressor.codec_checksum = cfg->codec_checksum_enabled;
    return 0;
}

/* Envelope is emitted lazily so a writer that's created but never written
 * doesn't leave a stub envelope on the sink. */
static int streamWriterEnsureEnvelope(streamWriter *t) {
    if (t->envelope_written) return 0;
    vkcsCodec codec;
    if (compressionAlgoToVkcsCodec(t->compressor.algo, &codec) != 0 ||
        writeVkcsEnvelope(t->emit_cb, t->emit_ctx, codec,
                          t->stream_kind, t->compressor.codec_checksum) != 0) {
        t->errored = true;
        return -1;
    }
    t->bytes_emitted += VKCS_ENVELOPE_SIZE;
    t->envelope_written = true;
    return 0;
}

static int streamWriterEmit(streamWriter *t, const uint8_t *buf, size_t len) {
    if (len == 0) return 0;
    if (t->emit_cb(t->emit_ctx, buf, len) != 0) {
        t->errored = true;
        return -1;
    }
    t->bytes_emitted += len;
    return 0;
}

static int streamWriterEnsureOutBuf(streamWriter *t, size_t input_len) {
    size_t needed = streamCompressOutputBound(&t->compressor, input_len);
    if (needed == 0) {
        t->errored = true;
        return -1;
    }
    if (needed > t->out_buf_size) {
        t->out_buf = zrealloc(t->out_buf, needed);
        t->out_buf_size = needed;
    }
    return 0;
}

static int streamWriterFeedAndEmit(streamWriter *t,
                                   const uint8_t *input,
                                   size_t input_len,
                                   compressFlushMode flush_mode) {
    if (streamWriterEnsureOutBuf(t, input_len) != 0) return -1;

    ssize_t compressed = streamCompressFeed(&t->compressor, t->out_buf,
                                            t->out_buf_size,
                                            input, input_len, flush_mode);
    if (compressed < 0) {
        t->errored = true;
        return -1;
    }
    return streamWriterEmit(t, t->out_buf, (size_t)compressed);
}

static void streamWriterReleaseContext(streamWriter *t) {
    streamCompressorDestroy(&t->compressor);
    if (t->out_buf) {
        zfree(t->out_buf);
        t->out_buf = NULL;
    }
    t->out_buf_size = 0;
}

streamWriter *streamWriterCreate(const streamWriterConfig *cfg,
                                 vkcsEmitFn emit_cb,
                                 void *emit_ctx) {
    if (!cfg || !emit_cb || !compressionAlgoSupportsStreaming(cfg->algo)) return NULL;

    streamWriter *t = zmalloc(sizeof(*t));
    if (streamWriterInitContext(t, cfg, emit_cb, emit_ctx) != 0) {
        zfree(t);
        return NULL;
    }
    return t;
}

ssize_t streamWriterWrite(streamWriter *t, const void *buf, size_t len) {
    if (!t || t->errored) return -1;
    /* Writes after finish are a caller bug — silently dropping them would
     * corrupt the consumer's view of the stream. */
    if (t->finished) return -1;
    if (len == 0) return 0;
    if (len > (size_t)SSIZE_MAX) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    size_t remaining = len;
    uint64_t emitted_before = t->bytes_emitted;
    if (streamWriterEnsureEnvelope(t) != 0) return -1;
    while (remaining > 0) {
        size_t chunk_len = remaining < STREAM_WRITER_INPUT_CHUNK_SIZE
                               ? remaining
                               : STREAM_WRITER_INPUT_CHUNK_SIZE;
        if (streamWriterFeedAndEmit(t, src, chunk_len, FLUSH_CONTINUE) != 0) return -1;
        src += chunk_len;
        remaining -= chunk_len;
    }
    uint64_t emitted_delta = t->bytes_emitted - emitted_before;
    if (emitted_delta > (uint64_t)SSIZE_MAX) {
        t->errored = true;
        return -1;
    }
    return (ssize_t)emitted_delta;
}

int streamWriterFlush(streamWriter *t) {
    if (!t || t->errored) return -1;
    /* Flush after finish is a no-op: frame is already closed. */
    if (t->finished) return 0;

    if (!t->envelope_written || !t->compressor.stream_started) return 0;
    return streamWriterFeedAndEmit(t, NULL, 0, FLUSH_SYNC);
}

int streamWriterFinish(streamWriter *t) {
    if (!t || t->errored) return -1;
    if (t->finished) return 0;
    t->finished = true;

    /* Even an empty stream produces a valid envelope + empty frame so the
     * loader sees a well-formed file. */
    if (streamWriterEnsureEnvelope(t) != 0) return -1;
    return streamWriterFeedAndEmit(t, NULL, 0, FLUSH_END);
}

void streamWriterDestroy(streamWriter *t) {
    if (!t) return;
    streamWriterReleaseContext(t);
    zfree(t);
}

int streamWriterIsErrored(const streamWriter *t) {
    return t && t->errored;
}

void streamWriterSetError(streamWriter *t) {
    if (!t) return;
    t->errored = true;
}

/* ===== Streaming reader ===== */

struct streamReader {
    streamReaderReadFn read_cb;
    void *read_ctx;

    vkcsProbeConfig probe_cfg;
    vkcsProbe probe;
    size_t probe_replay_pos; /* Passthrough bytes left to replay from probe. */
    size_t buffer_size;
    bool errored;
    streamReaderError error_kind;

    streamDecompressor decompressor;
    bool decompressor_initialized;

    uint8_t *compressed_buf;
    size_t compressed_buf_pos;
    size_t compressed_buf_len;

    uint8_t *decompressed_buf;
    size_t decompressed_buf_pos;
    size_t decompressed_buf_len;
};

static void streamReaderSetError(streamReader *t, streamReaderError error_kind) {
    t->errored = true;
    if (t->error_kind == STREAM_READER_ERROR_NONE) t->error_kind = error_kind;
}

static ssize_t streamReaderFail(streamReader *t, size_t partial_bytes) {
    streamReaderSetError(t, STREAM_READER_ERROR_IO);
    return partial_bytes > 0 ? (ssize_t)partial_bytes : -1;
}

static ssize_t streamReaderFailWithError(streamReader *t,
                                         size_t partial_bytes,
                                         streamReaderError error_kind) {
    streamReaderSetError(t, error_kind);
    return partial_bytes > 0 ? (ssize_t)partial_bytes : -1;
}

static int streamReaderInitCompressedState(streamReader *t, size_t buffer_size) {
    if (streamDecompressorInit(&t->decompressor, t->probe.algo) != 0) return -1;
    t->decompressor_initialized = true;
    t->compressed_buf = zmalloc(buffer_size);
    t->decompressed_buf = zmalloc(buffer_size);
    return 0;
}

static void streamReaderResetCompressedState(streamReader *t) {
    if (t->decompressor_initialized) {
        streamDecompressorDestroy(&t->decompressor);
        t->decompressor_initialized = false;
    }
    if (t->compressed_buf) {
        zfree(t->compressed_buf);
        t->compressed_buf = NULL;
    }
    t->compressed_buf_pos = 0;
    t->compressed_buf_len = 0;
    if (t->decompressed_buf) {
        zfree(t->decompressed_buf);
        t->decompressed_buf = NULL;
    }
    t->decompressed_buf_pos = 0;
    t->decompressed_buf_len = 0;
}

streamReader *streamReaderCreate(const streamReaderConfig *cfg,
                                 streamReaderReadFn read_cb,
                                 void *read_ctx) {
    if (!cfg || !read_cb) return NULL;

    streamReader *t = zcalloc(sizeof(*t));
    t->read_cb = read_cb;
    t->read_ctx = read_ctx;
    t->probe_cfg.allow_passthrough = cfg->allow_passthrough;
    t->probe_cfg.expected_stream_kind = cfg->expected_stream_kind;
    vkcsProbeInit(&t->probe);
    t->buffer_size = cfg->buffer_size ? cfg->buffer_size : STREAM_READER_BUFFER_SIZE_DEFAULT;
    if (t->buffer_size < STREAM_READER_BUFFER_SIZE_MIN) {
        t->buffer_size = STREAM_READER_BUFFER_SIZE_MIN;
    }
    return t;
}

static size_t streamReaderProbeBytesNeeded(const streamReader *t) {
    if (t->probe.header_len < VKCS_MAGIC_SIZE) return VKCS_MAGIC_SIZE - t->probe.header_len;
    return VKCS_ENVELOPE_SIZE - t->probe.header_len;
}

int streamReaderProbe(streamReader *t) {
    if (!t || t->errored) return -1;
    if (t->probe.ready) return 0;

    while (!t->probe.ready) {
        uint8_t buf[VKCS_ENVELOPE_SIZE];
        size_t need = streamReaderProbeBytesNeeded(t);
        ssize_t got = t->read_cb(t->read_ctx, buf, need);
        size_t consumed = 0;

        if (got < 0 || (size_t)got > need) {
            streamReaderSetError(t, STREAM_READER_ERROR_IO);
            return -1;
        }

        vkcsProbeResult status = vkcsProbeFeed(&t->probe, &t->probe_cfg, buf,
                                               got > 0 ? (size_t)got : 0,
                                               got == 0, &consumed);
        switch (status) {
        case VKCS_PROBE_ERROR:
            streamReaderSetError(t, STREAM_READER_ERROR_INCOMPATIBLE);
            return -1;
        case VKCS_PROBE_NEED_INPUT:
            continue;
        case VKCS_PROBE_COMPRESSED:
            if (!t->decompressor_initialized &&
                streamReaderInitCompressedState(t, t->buffer_size) != 0) {
                streamReaderSetError(t, STREAM_READER_ERROR_IO);
                return -1;
            }
            break;
        case VKCS_PROBE_PASSTHROUGH:
            break;
        default:
            streamReaderSetError(t, STREAM_READER_ERROR_INCOMPATIBLE);
            return -1;
        }
    }
    return 0;
}

static size_t streamReaderProbeAvail(const streamReader *t) {
    if (t->probe.header_len <= t->probe_replay_pos) return 0;
    return t->probe.header_len - t->probe_replay_pos;
}

/* Replay any probe-buffered bytes before reading from the wrapped source. */
static ssize_t streamReaderReadPassthrough(streamReader *t, uint8_t *dst, size_t len) {
    size_t total = 0;
    size_t prefix_avail = streamReaderProbeAvail(t);
    if (prefix_avail > 0) {
        size_t from_prefix = prefix_avail < len ? prefix_avail : len;
        memcpy(dst, t->probe.header + t->probe_replay_pos, from_prefix);
        t->probe_replay_pos += from_prefix;
        dst += from_prefix;
        len -= from_prefix;
        total += from_prefix;
    }
    if (len == 0) return (ssize_t)total;

    ssize_t got = t->read_cb(t->read_ctx, dst, len);
    if (got < 0 || (size_t)got > len) return streamReaderFail(t, total);
    return (ssize_t)(total + (size_t)got);
}

static int streamReaderDrainCompressedBuf(streamReader *t,
                                          uint8_t *out,
                                          size_t out_size,
                                          size_t *out_written) {
    *out_written = 0;
    while (t->compressed_buf_len > 0 && *out_written < out_size) {
        size_t consumed = 0;
        size_t feed_len = t->compressed_buf_len;
        size_t input_hint = t->decompressor.input_hint;
        if (input_hint > 0 && feed_len > input_hint) feed_len = input_hint;
        ssize_t produced = streamDecompressFeed(
            &t->decompressor,
            out + *out_written, out_size - *out_written,
            t->compressed_buf + t->compressed_buf_pos,
            feed_len, &consumed);
        if (produced < 0 ||
            consumed > feed_len ||
            (size_t)produced > out_size - *out_written) {
            streamReaderSetError(t, STREAM_READER_ERROR_CORRUPT);
            return -1;
        }
        *out_written += (size_t)produced;
        t->compressed_buf_pos += consumed;
        t->compressed_buf_len -= consumed;
        if (t->decompressor.frame_done) break;
        if (consumed == 0 && produced == 0) break;
    }
    if (t->compressed_buf_len == 0) t->compressed_buf_pos = 0;
    return 0;
}

static size_t streamReaderCompressedBufTailSpace(streamReader *t) {
    size_t tail_space = t->buffer_size - t->compressed_buf_pos - t->compressed_buf_len;
    if (tail_space > 0) return tail_space;

    if (t->compressed_buf_len == 0) {
        t->compressed_buf_pos = 0;
        return t->buffer_size;
    }

    if (t->compressed_buf_pos > 0) {
        memmove(t->compressed_buf, t->compressed_buf + t->compressed_buf_pos, t->compressed_buf_len);
        t->compressed_buf_pos = 0;
        return t->buffer_size - t->compressed_buf_len;
    }

    /* Buffer full and the codec made no progress — treat as corrupt rather
     * than grow buffers indefinitely. */
    streamReaderSetError(t, STREAM_READER_ERROR_CORRUPT);
    return 0;
}

static int streamReaderRefillCompressedBuf(streamReader *t) {
    if (t->decompressor.frame_done) return 0;

    size_t read_size = streamReaderCompressedBufTailSpace(t);
    size_t input_hint = t->decompressor.input_hint;
    if (input_hint > 0 && read_size > input_hint) read_size = input_hint;
    if (read_size > (size_t)SSIZE_MAX) read_size = (size_t)SSIZE_MAX;
    if (read_size == 0) return -1;

    ssize_t got = t->read_cb(t->read_ctx,
                             t->compressed_buf + t->compressed_buf_pos + t->compressed_buf_len,
                             read_size);
    if (got < 0 || (size_t)got > read_size) return -1;
    if (got == 0) return 0;
    t->compressed_buf_len += (size_t)got;
    return 1;
}

static ssize_t streamReaderFillDecompressedBuf(streamReader *t) {
    size_t written = 0;

    t->decompressed_buf_pos = 0;
    t->decompressed_buf_len = 0;

    while (written < t->buffer_size) {
        if (t->compressed_buf_len > 0) {
            size_t chunk_written = 0;
            if (streamReaderDrainCompressedBuf(t, t->decompressed_buf + written,
                                               t->buffer_size - written, &chunk_written) != 0) {
                written += chunk_written;
                break;
            }
            written += chunk_written;
            if (written >= t->buffer_size) break;
        }

        int read_rc = streamReaderRefillCompressedBuf(t);
        if (read_rc < 0) {
            streamReaderSetError(t, STREAM_READER_ERROR_IO);
            if (written == 0) return -1;
            break;
        }
        if (read_rc == 0) break;
    }

    t->decompressed_buf_len = written;
    return (ssize_t)written;
}

static inline size_t streamReaderDecompressedBufAvail(const streamReader *t) {
    if (t->decompressed_buf_len <= t->decompressed_buf_pos) return 0;
    return t->decompressed_buf_len - t->decompressed_buf_pos;
}

static size_t streamReaderCopyFromDecompressedBuf(streamReader *t,
                                                  uint8_t **dst,
                                                  size_t *remaining) {
    size_t avail = streamReaderDecompressedBufAvail(t);
    if (avail == 0 || *remaining == 0) return 0;

    size_t to_copy = avail < *remaining ? avail : *remaining;
    memcpy(*dst, t->decompressed_buf + t->decompressed_buf_pos, to_copy);
    t->decompressed_buf_pos += to_copy;
    *dst += to_copy;
    *remaining -= to_copy;
    return to_copy;
}

static ssize_t streamReaderReadCompressed(streamReader *t, uint8_t *dst, size_t len) {
    size_t remaining = len;
    size_t total = 0;

    total += streamReaderCopyFromDecompressedBuf(t, &dst, &remaining);
    while (remaining > 0) {
        if (streamReaderDecompressedBufAvail(t) == 0) {
            ssize_t filled = streamReaderFillDecompressedBuf(t);
            if (filled < 0) {
                return streamReaderFailWithError(
                    t, total,
                    t->error_kind == STREAM_READER_ERROR_NONE ? STREAM_READER_ERROR_IO
                                                              : t->error_kind);
            }
            if (filled == 0 && !t->decompressor.frame_done) {
                return streamReaderFailWithError(t, total, STREAM_READER_ERROR_CORRUPT);
            }
            if (filled == 0) break;
        }

        total += streamReaderCopyFromDecompressedBuf(t, &dst, &remaining);
        if (t->errored) break;
    }

    return (ssize_t)total;
}

ssize_t streamReaderRead(streamReader *t, void *buf, size_t len) {
    if (!t || !buf || t->errored) return -1;
    if (len == 0) return 0;
    if (len > (size_t)SSIZE_MAX) return -1;

    if (!t->probe.ready && streamReaderProbe(t) != 0) return -1;

    if (!t->probe.compressed) {
        return streamReaderReadPassthrough(t, (uint8_t *)buf, len);
    }
    return streamReaderReadCompressed(t, (uint8_t *)buf, len);
}

int streamReaderGetInfo(streamReader *t, streamReaderInfo *info) {
    if (!t || !info) return -1;
    if (streamReaderProbe(t) != 0) return -1;

    info->compressed = t->probe.compressed;
    info->codec_checksum_enabled = t->probe.compressed ? t->probe.codec_checksum_enabled : false;
    info->algo = t->probe.compressed ? t->probe.algo : ALGO_NONE;
    info->stream_kind = t->probe.stream_kind;
    return 0;
}

streamReaderError streamReaderGetError(const streamReader *t) {
    return t ? t->error_kind : STREAM_READER_ERROR_IO;
}

int streamReaderValidateEnd(streamReader *t) {
    uint8_t buf[4096];

    if (!t) return -1;
    if (streamReaderProbe(t) != 0) return -1;
    if (!t->probe.compressed) return 0;
    if (streamReaderDecompressedBufAvail(t) > 0) {
        streamReaderSetError(t, STREAM_READER_ERROR_CORRUPT);
        return -1;
    }

    while (!t->decompressor.frame_done) {
        ssize_t nread = streamReaderRead(t, buf, sizeof(buf));
        if (nread < 0) return -1;
        if (nread > 0) {
            streamReaderSetError(t, STREAM_READER_ERROR_CORRUPT);
            return -1;
        }
    }

    if (t->compressed_buf_len > 0) {
        streamReaderSetError(t, STREAM_READER_ERROR_CORRUPT);
        return -1;
    }

    ssize_t got = t->read_cb(t->read_ctx, buf, 1);
    if (got < 0) {
        streamReaderSetError(t, STREAM_READER_ERROR_IO);
        return -1;
    }
    if (got > 0) {
        streamReaderSetError(t, STREAM_READER_ERROR_CORRUPT);
        return -1;
    }
    return 0;
}

void streamReaderDestroy(streamReader *t) {
    if (!t) return;
    streamReaderResetCompressedState(t);
    zfree(t);
}
