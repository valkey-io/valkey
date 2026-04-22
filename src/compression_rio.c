/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Rio decorators (compress_rio, decompress_rio). */

#include "compression_rio.h"
#include <string.h>
#include <unistd.h>

/* Shared rio callbacks for unsupported/no-op operations. */
static size_t rioReadUnsupported(rio *r, void *buf, size_t len) {
    (void)r;
    (void)buf;
    (void)len;
    return 0;
}

static size_t rioWriteUnsupported(rio *r, const void *buf, size_t len) {
    (void)r;
    (void)buf;
    (void)len;
    return 0;
}

static int rioFlushNoop(rio *r) {
    (void)r;
    return 1;
}

/* Shared rio base initializer used by all decorators in this file. */
static void rioInitBase(rio *base,
                        size_t (*read_fn)(rio *, void *, size_t),
                        size_t (*write_fn)(rio *, const void *, size_t),
                        off_t (*tell_fn)(rio *),
                        int (*flush_fn)(rio *),
                        uint64_t flags,
                        uint8_t type) {
    base->read = read_fn;
    base->write = write_fn;
    base->tell = tell_fn;
    base->flush = flush_fn;
    base->read_some = NULL;
    base->update_cksum = NULL;
    base->cksum = 0;
    base->flags = flags;
    base->processed_bytes = 0;
    base->max_processing_chunk = 0;
    base->type = type;
}

/* ===================================================================
 * Compression Rio Decorator
 * Wraps an inner rio for transparent compression on write.
 * Currently used by file-backed RDB save paths.
 *
 * RDB CHECKSUM SEMANTICS:
 * - when codec checksums are enabled, they are the authoritative integrity
 *   signal for the compressed stream and the RDB checksum field is left zero.
 * - otherwise the standard RDB checksum protects the logical stream.
 * =================================================================== */

/* Emit callback for compress_rio: writes compressed bytes to inner rio.
 * Returns 0 on success, -1 on error. */
static int compressRioEmit(void *ctx, const uint8_t *data, size_t len) {
    compress_rio_t *cr = (compress_rio_t *)ctx;
    if (rioWrite(cr->inner, data, len) == 0) return -1;
    return 0;
}

/* rio vtable: write callback — compress then delegate to inner rio */
static size_t compressRioWrite(rio *r, const void *buf, size_t len) {
    compress_rio_t *cr = (compress_rio_t *)r;
    if (!cr->compressor || cr->finalized || stream_writer_is_errored(cr->compressor)) {
        r->flags |= RIO_FLAG_WRITE_ERROR;
        return 0;
    }
    if (stream_writer_write(cr->compressor, buf, len) < 0) {
        r->flags |= RIO_FLAG_WRITE_ERROR;
        return 0;
    }
    return 1; /* rio write callback contract: 0 on error, non-zero on success */
}

/* rio vtable: tell callback — returns processed bytes from base */
static off_t compressRioTell(rio *r) {
    return (off_t)r->processed_bytes;
}

/* rio vtable: flush callback — algorithm flush (emit buffered data,
 * keep frame open) + inner flush. Does NOT end the frame.
 * This is critical because some call sites flush mid-stream. */
static int compressRioFlush(rio *r) {
    compress_rio_t *cr = (compress_rio_t *)r;
    if (!cr->compressor || stream_writer_is_errored(cr->compressor)) return 0;
    if (cr->finalized) return 1;

    if (stream_writer_flush(cr->compressor) != 0) return 0;

    if (cr->inner->flush && cr->inner->flush(cr->inner) == 0) {
        stream_writer_set_error(cr->compressor);
        return 0;
    }
    return 1;
}

/* Initialize a compression rio decorator wrapping an inner rio.
 * Sets up the rio vtable so callers can use standard rioWrite/rioFlush.
 * The compressor is initialized with a fresh algorithm context (fork-safe). */
/* Returns 0 on success, -1 on failure (e.g., compressor init failed). */
int rioInitWithCompress(compress_rio_t *cr, rio *inner, const stream_writer_config_t *cfg) {
    if (!cr || !inner || !cfg) return -1;

    memset(cr, 0, sizeof(*cr));

    uint64_t flags = RIO_FLAG_STREAMING_COMPRESSION;
    if (cfg->codec_checksum_enabled) flags |= RIO_FLAG_STREAMING_CODEC_CHECKSUM;
    flags |= inner->flags & RIO_FLAG_SKIP_RDB_CHECKSUM;

    rioInitBase(&cr->base, rioReadUnsupported, compressRioWrite, compressRioTell,
                compressRioFlush, flags, rioCheckType(inner));
    /* Track the uncompressed byte stream only when the standard RDB checksum is
     * the active integrity mechanism. Honor explicit skip requests from the
     * wrapped rio and skip checksum tracking whenever codec checksums are
     * authoritative. */
    if (!(flags & (RIO_FLAG_SKIP_RDB_CHECKSUM | RIO_FLAG_STREAMING_CODEC_CHECKSUM))) {
        cr->base.update_cksum = rioGenericUpdateChecksum;
    }

    cr->inner = inner;
    cr->finalized = 0;
    cr->compressor = stream_writer_create(cfg, compressRioEmit, cr);
    return cr->compressor ? 0 : -1;
}

/* Finalize the compression frame and flush inner rio.
 * Must be called exactly once at end of stream.
 * Idempotent: safe to call multiple times (second call is a no-op). */
/* Returns 0 on success, -1 if the compressor or inner flush errored. */
int compress_rio_finish(compress_rio_t *cr) {
    if (!cr) return -1;
    if (!cr->compressor) return -1;
    if (cr->finalized) return stream_writer_is_errored(cr->compressor) ? -1 : 0;
    cr->finalized = 1;

    if (stream_writer_finish(cr->compressor) != 0) {
        return -1;
    }

    /* Flush inner rio to ensure all bytes reach the destination.
     * Propagate flush failure to the compressor error state so
     * callers can detect it. */
    if (cr->inner->flush && cr->inner->flush(cr->inner) == 0) {
        stream_writer_set_error(cr->compressor);
    }
    return stream_writer_is_errored(cr->compressor) ? -1 : 0;
}

/* Free compressor context and buffers. Does NOT finalize the frame.
 * Call compress_rio_finish() first on all exit paths. */
void compress_rio_destroy(compress_rio_t *cr) {
    if (!cr) return;
    if (cr->compressor) {
        stream_writer_destroy(cr->compressor);
        cr->compressor = NULL;
    }
}

/* ===================================================================
 * Decompression Rio Decorator
 * Thin rio adapter around stream_reader_t.
 * =================================================================== */

/* Read up to `len` bytes from the inner rio (partial reads allowed).
 * Returns >0 bytes, 0 on EOF, -1 on error. */
static ssize_t decompressRioReadPartial(void *ctx, void *buf, size_t len) {
    decompress_rio_t *dr = (decompress_rio_t *)ctx;
    return rioReadPartial(dr->inner, buf, len);
}

static size_t decompressRioRead(rio *r, void *buf, size_t len) {
    decompress_rio_t *dr = (decompress_rio_t *)r;
    if (dr->base.flags & RIO_FLAG_READ_ERROR) return 0;
    if (!dr->reader) {
        dr->base.flags |= RIO_FLAG_READ_ERROR;
        return 0;
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t nread = stream_reader_read(dr->reader, dst, remaining);
        if (nread < 0) {
            dr->base.flags |= RIO_FLAG_READ_ERROR;
            return 0;
        }
        if (nread == 0) {
            /* rio contract: partial read is failure for requested len. */
            dr->base.flags |= RIO_FLAG_READ_ERROR;
            return 0;
        }
        remaining -= (size_t)nread;
        dst += nread;
    }
    return len;
}

/* rio vtable: tell callback — report transport bytes consumed from the wrapped
 * rio so progress stays tied to source-stream position. */
static off_t decompressRioTell(rio *r) {
    decompress_rio_t *dr = (decompress_rio_t *)r;
    return (off_t)dr->inner->processed_bytes;
}

stream_reader_error_t decompress_rio_get_error(const decompress_rio_t *dr) {
    if (!dr || !dr->reader) return STREAM_READER_ERROR_IO;
    return stream_reader_get_error(dr->reader);
}

/* Initialize a decompression rio and eagerly probe the wrapped stream so the
 * caller gets a stable classification up front: passthrough, compressed, or
 * incompatible envelope. */
decompress_rio_init_result_t rioInitWithDecompress(decompress_rio_t *dr,
                                                   rio *inner,
                                                   const stream_reader_config_t *cfg,
                                                   stream_reader_info_t *info) {
    stream_reader_info_t local_info = {0};

    if (!dr || !inner || !cfg) return DECOMPRESS_RIO_INIT_ERROR;

    memset(dr, 0, sizeof(*dr));
    rioInitBase(&dr->base, decompressRioRead, rioWriteUnsupported, decompressRioTell,
                rioFlushNoop,
                RIO_FLAG_STREAMING_DECOMPRESSION | (inner->flags & RIO_FLAG_SKIP_RDB_CHECKSUM),
                rioCheckType(inner));
    dr->inner = inner;

    dr->reader = stream_reader_create(cfg, decompressRioReadPartial, dr);
    if (!dr->reader) {
        dr->base.flags |= RIO_FLAG_READ_ERROR;
        return DECOMPRESS_RIO_INIT_ERROR;
    }
    if (stream_reader_get_info(dr->reader, &local_info) != 0) {
        stream_reader_error_t error_kind = stream_reader_get_error(dr->reader);
        decompress_rio_destroy(dr);
        return error_kind == STREAM_READER_ERROR_INCOMPATIBLE
                   ? DECOMPRESS_RIO_INIT_INCOMPATIBLE
                   : DECOMPRESS_RIO_INIT_ERROR;
    }

    if (local_info.compressed) {
        dr->base.flags |= RIO_FLAG_STREAMING_COMPRESSION;
        if (local_info.codec_checksum_enabled) {
            dr->base.flags |= RIO_FLAG_STREAMING_CODEC_CHECKSUM;
        }
    }
    if (info) *info = local_info;
    return DECOMPRESS_RIO_INIT_OK;
}

void decompress_rio_destroy(decompress_rio_t *dr) {
    if (!dr) return;
    if (dr->reader) {
        stream_reader_destroy(dr->reader);
        dr->reader = NULL;
    }
}
