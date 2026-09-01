/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_stream.h"
#include "server.h"
#include "zmalloc.h"
#include <limits.h>
#include <string.h>

/* ===== VCS envelope ===== */

static const uint8_t VCS_MAGIC[VCS_MAGIC_SIZE] = {
    VCS_MAGIC_0,
    VCS_MAGIC_1,
    VCS_MAGIC_2,
};

/* True when the first len bytes of buf match the VCS magic. When len is below
 * VCS_MAGIC_SIZE this only compares that prefix. */
static bool vcsHasMagicPrefix(const uint8_t *buf, size_t len) {
    size_t n = len < VCS_MAGIC_SIZE ? len : VCS_MAGIC_SIZE;
    return memcmp(buf, VCS_MAGIC, n) == 0;
}

static int writeVcsEnvelope(streamWriterWriteFn write_cb,
                            void *ctx,
                            compressionAlgo algo) {
    uint8_t codec;
    switch (algo) {
    case ALGO_LZ4:
        codec = VCS_CODEC_LZ4;
        break;
    default:
        return C_ERR;
    }

    uint8_t envelope[VCS_ENVELOPE_SIZE] = {
        VCS_MAGIC_0,
        VCS_MAGIC_1,
        VCS_MAGIC_2,
        [VCS_OFFSET_VERSION] = VCS_VERSION,
        [VCS_OFFSET_CODEC] = codec,
        [VCS_OFFSET_RESERVED] = 0,
        [VCS_OFFSET_STREAM_KIND] = VCS_STREAM_RDB,
    };
    return write_cb(ctx, envelope, VCS_ENVELOPE_SIZE);
}

/* Reject a nonzero reserved byte so a future envelope extension fails loudly
 * rather than being silently misinterpreted. */
static int readVcsEnvelope(const uint8_t *buf, compressionAlgo *algo) {
    if (buf[VCS_OFFSET_VERSION] != VCS_VERSION) return C_ERR;

    switch (buf[VCS_OFFSET_CODEC]) {
    case VCS_CODEC_LZ4:
        *algo = ALGO_LZ4;
        break;
    default:
        return C_ERR;
    }
    if (buf[VCS_OFFSET_RESERVED] != 0) return C_ERR;
    if (buf[VCS_OFFSET_STREAM_KIND] != VCS_STREAM_RDB) return C_ERR;
    return C_OK;
}

/* ===== Streaming Writer ===== */

#define STREAM_WRITER_INPUT_CHUNK_SIZE (1024 * 1024)

int streamWriterInit(streamWriter *writer, compressionAlgo algo, bool codec_checksum, streamWriterWriteFn write_cb, void *write_ctx) {
    memset(writer, 0, sizeof(*writer));
    writer->write_cb = write_cb;
    writer->write_ctx = write_ctx;

    if (streamCompressorInit(&writer->compressor, algo, 0, codec_checksum) == C_ERR) {
        writer->state = STREAM_WRITER_STATE_ERROR;
        return C_ERR;
    }
    return C_OK;
}

/* Envelope is emitted lazily so a writer that's created but never written
 * doesn't leave a stub envelope on the sink. */
static int streamWriterEnsureEnvelope(streamWriter *writer) {
    if (writer->state == STREAM_WRITER_STATE_ACTIVE) return C_OK;
    if (writer->state != STREAM_WRITER_STATE_INITIAL) return C_ERR;
    if (writeVcsEnvelope(writer->write_cb, writer->write_ctx, writer->compressor.algo) == C_ERR) {
        writer->state = STREAM_WRITER_STATE_ERROR;
        return C_ERR;
    }
    writer->state = STREAM_WRITER_STATE_ACTIVE;
    return C_OK;
}

static int streamWriterFeedAndWrite(streamWriter *writer,
                                    const uint8_t *input,
                                    size_t input_len,
                                    compressFlushMode flush_mode) {
    const size_t needed = streamCompressorOutputBound(&writer->compressor, input_len);
    if (needed > writer->out_buf_size) {
        writer->out_buf = zrealloc(writer->out_buf, needed);
        writer->out_buf_size = needed;
    }

    const ssize_t compressed = streamCompressorFeed(&writer->compressor, writer->out_buf,
                                                    writer->out_buf_size,
                                                    input, input_len, flush_mode);
    if (compressed < 0) {
        writer->state = STREAM_WRITER_STATE_ERROR;
        return C_ERR;
    }
    if (compressed > 0 && writer->write_cb(writer->write_ctx, writer->out_buf, (size_t)compressed) == C_ERR) {
        writer->state = STREAM_WRITER_STATE_ERROR;
        return C_ERR;
    }
    return C_OK;
}

int streamWriterWrite(streamWriter *writer, const void *buf, size_t len) {
    /* Writes after finish are a caller bug; silently dropping them would
     * corrupt the consumer's view of the stream. */
    if (writer->state == STREAM_WRITER_STATE_FINISHED || writer->state == STREAM_WRITER_STATE_ERROR) return C_ERR;
    if (len == 0) return C_OK;

    const uint8_t *src = (const uint8_t *)buf;
    size_t remaining = len;
    if (streamWriterEnsureEnvelope(writer) == C_ERR) return C_ERR;
    while (remaining > 0) {
        size_t chunk_len = remaining < STREAM_WRITER_INPUT_CHUNK_SIZE
                               ? remaining
                               : STREAM_WRITER_INPUT_CHUNK_SIZE;
        if (streamWriterFeedAndWrite(writer, src, chunk_len, COMPRESS_FLUSH_CONTINUE) == C_ERR) return C_ERR;
        src += chunk_len;
        remaining -= chunk_len;
    }
    return C_OK;
}

int streamWriterFinish(streamWriter *writer) {
    if (writer->state == STREAM_WRITER_STATE_ERROR) return C_ERR;
    if (writer->state == STREAM_WRITER_STATE_FINISHED) return C_OK;

    /* Even an empty stream produces a valid envelope + empty frame so the
     * loader sees a well-formed file. */
    if (streamWriterEnsureEnvelope(writer) == C_ERR) return C_ERR;
    if (streamWriterFeedAndWrite(writer, NULL, 0, COMPRESS_FLUSH_END) == C_ERR) return C_ERR;
    writer->state = STREAM_WRITER_STATE_FINISHED;
    return C_OK;
}

void streamWriterFree(streamWriter *writer) {
    streamCompressorFree(&writer->compressor);
    zfree(writer->out_buf);
    writer->out_buf = NULL;
    writer->out_buf_size = 0;
}

/* ===== Streaming Reader ===== */

static void streamReaderSetError(streamReader *reader, streamReaderErrorKind error_kind) {
    if (reader->error_kind == STREAM_READER_ERROR_NONE) reader->error_kind = error_kind;
}

int streamReaderInit(streamReader *reader, const streamReaderConfig *cfg, streamReaderReadFn read_cb, void *read_ctx, compressionAlgo *detected_algo) {
    memset(reader, 0, sizeof(*reader));
    reader->read_cb = read_cb;
    reader->read_ctx = read_ctx;
    reader->buffer_size = cfg->buffer_size < STREAM_READER_BUFFER_SIZE_MIN
                              ? STREAM_READER_BUFFER_SIZE_MIN
                              : cfg->buffer_size;
    compressionAlgo algo = ALGO_NONE;
    while (true) {
        size_t need = reader->probe.header_len < VCS_MAGIC_SIZE
                          ? VCS_MAGIC_SIZE - reader->probe.header_len
                          : VCS_ENVELOPE_SIZE - reader->probe.header_len;
        ssize_t got = reader->read_cb(reader->read_ctx,
                                      reader->probe.header + reader->probe.header_len,
                                      need);

        if (got < 0 || (size_t)got > need) {
            streamReaderSetError(reader, STREAM_READER_ERROR_IO);
            return C_ERR;
        }
        reader->probe.header_len += (size_t)got;

        if (reader->probe.header_len >= VCS_MAGIC_SIZE &&
            !vcsHasMagicPrefix(reader->probe.header, VCS_MAGIC_SIZE)) {
            if (!cfg->allow_passthrough) {
                streamReaderSetError(reader, STREAM_READER_ERROR_INCOMPATIBLE);
                return C_ERR;
            }
            reader->state = STREAM_READER_STATE_PASSTHROUGH;
            break;
        }

        if (reader->probe.header_len == VCS_ENVELOPE_SIZE) {
            if (readVcsEnvelope(reader->probe.header, &algo) == C_ERR) {
                streamReaderSetError(reader, STREAM_READER_ERROR_INCOMPATIBLE);
                return C_ERR;
            }
            if (streamDecompressorInit(&reader->decompressor, algo,
                                       cfg->skip_codec_checksum_validation) == C_ERR) {
                streamReaderSetError(reader, STREAM_READER_ERROR_IO);
                return C_ERR;
            }
            reader->compressed_buf_size = STREAM_READER_COMPRESSED_BUFFER_SIZE;
            reader->compressed_buf = zmalloc(reader->compressed_buf_size);
            reader->decompressed_buf = zmalloc(reader->buffer_size);
            reader->state = STREAM_READER_STATE_COMPRESSED;
            break;
        }

        if (got > 0) continue;

        /* EOF mid-magic looks like a truncated VCS, not passthrough. */
        if ((reader->probe.header_len > 0 &&
             vcsHasMagicPrefix(reader->probe.header, reader->probe.header_len)) ||
            !cfg->allow_passthrough) {
            streamReaderSetError(reader, STREAM_READER_ERROR_INCOMPATIBLE);
            return C_ERR;
        }
        reader->state = STREAM_READER_STATE_PASSTHROUGH;
        break;
    }

    if (detected_algo) *detected_algo = algo;
    return C_OK;
}

/* Initialization must consume enough bytes to distinguish a VCS envelope from
 * a plain stream. If the source is plain, those probe bytes are part of the
 * caller's payload and cannot be discarded. Replay them first, then continue
 * reading directly from the wrapped source so passthrough is byte-for-byte
 * transparent to the logical parser. */
static ssize_t streamReaderReadPassthrough(streamReader *reader, uint8_t *dst, size_t len) {
    size_t total = 0;
    size_t prefix_avail = reader->probe.header_len - reader->probe_replay_pos;
    if (prefix_avail > 0) {
        size_t from_prefix = prefix_avail < len ? prefix_avail : len;
        memcpy(dst, reader->probe.header + reader->probe_replay_pos, from_prefix);
        reader->probe_replay_pos += from_prefix;
        dst += from_prefix;
        len -= from_prefix;
        total += from_prefix;
    }
    if (len == 0) return (ssize_t)total;

    ssize_t got = reader->read_cb(reader->read_ctx, dst, len);
    if (got < 0 || (size_t)got > len) {
        streamReaderSetError(reader, STREAM_READER_ERROR_IO);
        return total > 0 ? (ssize_t)total : -1;
    }
    return (ssize_t)(total + (size_t)got);
}

/* Fill the decoded-output buffer by consuming buffered input or reading more
 * from the source. Errors are sticky, but decoded bytes produced before an
 * error remain available to the caller. */
static int streamReaderFillDecompressedBuf(streamReader *reader) {
    size_t written = 0;
    int result = C_OK;

    reader->decompressed_buf_pos = 0;
    reader->decompressed_buf_len = 0;

    while (written < reader->buffer_size && !reader->decompressor.frame_done) {
        if (reader->compressed_buf_len > 0) {
            size_t consumed = 0;
            size_t feed_len = reader->compressed_buf_len;
            size_t input_hint = reader->decompressor.input_hint;
            if (input_hint > 0 && feed_len > input_hint) feed_len = input_hint;

            size_t output_capacity = reader->buffer_size - written;
            ssize_t produced = streamDecompressorFeed(
                &reader->decompressor,
                reader->decompressed_buf + written, output_capacity,
                reader->compressed_buf + reader->compressed_buf_pos,
                feed_len, &consumed);
            if (produced < 0) {
                streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
                result = C_ERR;
                break;
            }
            if (consumed > feed_len || (size_t)produced > output_capacity) {
                streamReaderSetError(reader, STREAM_READER_ERROR_INTERNAL);
                result = C_ERR;
                break;
            }

            written += (size_t)produced;
            reader->compressed_buf_pos += consumed;
            reader->compressed_buf_len -= consumed;
            if (consumed > 0 || produced > 0) continue;
        }

        if (reader->compressed_buf_len == 0) reader->compressed_buf_pos = 0;

        /* Move any unconsumed suffix to the start of the input buffer before
         * asking the source for more bytes. */
        if (reader->compressed_buf_pos > 0) {
            memmove(reader->compressed_buf, reader->compressed_buf + reader->compressed_buf_pos,
                    reader->compressed_buf_len);
            reader->compressed_buf_pos = 0;
        }

        size_t read_size = reader->compressed_buf_size - reader->compressed_buf_len;
        /* A full input buffer with no codec progress is invalid. */
        if (read_size == 0) {
            streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
            result = C_ERR;
            break;
        }

        /* Limit the source read to the codec's preferred input size so it does
         * not read past the current frame into trailing data. */
        size_t input_hint = reader->decompressor.input_hint;
        if (input_hint > 0 && read_size > input_hint) read_size = input_hint;
        if (read_size > (size_t)SSIZE_MAX) read_size = (size_t)SSIZE_MAX;

        /* Refill the compressed-input buffer, then retry decoding. */
        ssize_t got = reader->read_cb(
            reader->read_ctx,
            reader->compressed_buf + reader->compressed_buf_pos + reader->compressed_buf_len,
            read_size);
        if (got < 0 || (size_t)got > read_size) {
            streamReaderSetError(reader, STREAM_READER_ERROR_IO);
            result = C_ERR;
            break;
        }
        if (got == 0) break;
        reader->compressed_buf_len += (size_t)got;
    }

    reader->decompressed_buf_len = written;
    return result;
}

ssize_t streamReaderRead(streamReader *reader, void *buf, size_t len) {
    if (reader->error_kind != STREAM_READER_ERROR_NONE) return -1;
    if (reader->state == STREAM_READER_STATE_FINISHED) return 0;
    if (len == 0) return 0;
    if (len > (size_t)SSIZE_MAX) return -1;

    if (reader->state == STREAM_READER_STATE_PASSTHROUGH) {
        return streamReaderReadPassthrough(reader, (uint8_t *)buf, len);
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = len;
    size_t total = 0;
    while (remaining > 0) {
        int fill_result = C_OK;
        size_t available = reader->decompressed_buf_len - reader->decompressed_buf_pos;
        if (available == 0) {
            fill_result = streamReaderFillDecompressedBuf(reader);
            available = reader->decompressed_buf_len;
            if (available == 0 && fill_result == C_ERR) return total > 0 ? (ssize_t)total : -1;
            if (available == 0 && !reader->decompressor.frame_done) {
                streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
                return total > 0 ? (ssize_t)total : -1;
            }
            if (available == 0) break;
        }

        size_t to_copy = available < remaining ? available : remaining;
        memcpy(dst, reader->decompressed_buf + reader->decompressed_buf_pos, to_copy);
        reader->decompressed_buf_pos += to_copy;
        dst += to_copy;
        remaining -= to_copy;
        total += to_copy;
        if (fill_result == C_ERR) break;
    }
    return (ssize_t)total;
}

int streamReaderFinish(streamReader *reader) {
    uint8_t buf[4096];

    if (reader->error_kind != STREAM_READER_ERROR_NONE) return C_ERR;
    if (reader->state == STREAM_READER_STATE_FINISHED) return C_OK;
    if (reader->state == STREAM_READER_STATE_PASSTHROUGH) {
        reader->state = STREAM_READER_STATE_FINISHED;
        return C_OK;
    }
    if (reader->decompressed_buf_len > reader->decompressed_buf_pos) {
        streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
        return C_ERR;
    }

    while (!reader->decompressor.frame_done) {
        ssize_t nread = streamReaderRead(reader, buf, sizeof(buf));
        if (nread < 0) return C_ERR;
        if (nread > 0) {
            streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
            return C_ERR;
        }
    }

    if (reader->compressed_buf_len > 0) {
        streamReaderSetError(reader, STREAM_READER_ERROR_CORRUPT);
        return C_ERR;
    }

    reader->state = STREAM_READER_STATE_FINISHED;
    return C_OK;
}

void streamReaderFree(streamReader *reader) {
    streamDecompressorFree(&reader->decompressor);
    zfree(reader->compressed_buf);
    zfree(reader->decompressed_buf);
    reader->compressed_buf = NULL;
    reader->compressed_buf_size = 0;
    reader->compressed_buf_pos = 0;
    reader->compressed_buf_len = 0;
    reader->decompressed_buf = NULL;
    reader->decompressed_buf_len = 0;
    reader->decompressed_buf_pos = 0;
}
