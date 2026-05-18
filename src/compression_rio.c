/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "compression_rio.h"
#include <string.h>
#include <unistd.h>

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

/* ===== compressRio ===== */

static int compressRioEmit(void *ctx, const uint8_t *data, size_t len) {
    compressRio *cr = (compressRio *)ctx;
    return rioWrite(cr->inner, data, len) == 0 ? -1 : 0;
}

static size_t compressRioWrite(rio *r, const void *buf, size_t len) {
    compressRio *cr = (compressRio *)r;
    if (!cr->writer || cr->finalized) {
        r->flags |= RIO_FLAG_WRITE_ERROR;
        return 0;
    }
    if (streamWriterWrite(cr->writer, buf, len) < 0) {
        r->flags |= RIO_FLAG_WRITE_ERROR;
        return 0;
    }
    return 1;
}

static off_t compressRioTell(rio *r) {
    return (off_t)r->processed_bytes;
}

/* Drains buffered data and the inner rio, but keeps the frame open so callers
 * that flush mid-stream don't accidentally close it. */
static int compressRioFlush(rio *r) {
    compressRio *cr = (compressRio *)r;
    if (!cr->writer || streamWriterIsErrored(cr->writer)) {
        r->flags |= RIO_FLAG_WRITE_ERROR;
        return 0;
    }
    if (cr->finalized) return 1;

    if (streamWriterFlush(cr->writer) != 0) {
        r->flags |= RIO_FLAG_WRITE_ERROR;
        return 0;
    }
    if (cr->inner->flush && cr->inner->flush(cr->inner) == 0) {
        streamWriterSetError(cr->writer);
        r->flags |= RIO_FLAG_WRITE_ERROR;
        return 0;
    }
    return 1;
}

int rioInitWithCompress(compressRio *cr, rio *inner, const streamWriterConfig *cfg) {
    if (!cr || !inner || !cfg) return -1;

    memset(cr, 0, sizeof(*cr));
    rioInitBase(&cr->base, rioReadUnsupported, compressRioWrite, compressRioTell,
                compressRioFlush, RIO_FLAG_STREAMING_COMPRESSION, rioCheckType(inner));

    cr->inner = inner;
    cr->writer = streamWriterCreate(cfg, compressRioEmit, cr);
    return cr->writer ? 0 : -1;
}

/* Idempotent: subsequent calls report cached error state. */
int compressRioFinish(compressRio *cr) {
    if (!cr) return -1;
    if (!cr->writer) {
        cr->base.flags |= RIO_FLAG_WRITE_ERROR;
        return -1;
    }
    if (cr->finalized) {
        if (streamWriterIsErrored(cr->writer)) {
            cr->base.flags |= RIO_FLAG_WRITE_ERROR;
            return -1;
        }
        return 0;
    }
    cr->finalized = 1;

    if (streamWriterFinish(cr->writer) != 0) {
        cr->base.flags |= RIO_FLAG_WRITE_ERROR;
        return -1;
    }
    if (cr->inner->flush && cr->inner->flush(cr->inner) == 0) {
        streamWriterSetError(cr->writer);
        cr->base.flags |= RIO_FLAG_WRITE_ERROR;
    }
    if (streamWriterIsErrored(cr->writer)) {
        cr->base.flags |= RIO_FLAG_WRITE_ERROR;
        return -1;
    }
    return 0;
}

void compressRioDestroy(compressRio *cr) {
    if (!cr) return;
    if (cr->writer) {
        streamWriterDestroy(cr->writer);
        cr->writer = NULL;
    }
}

/* ===== decompressRio ===== */

static ssize_t decompressRioReadPartial(void *ctx, void *buf, size_t len) {
    decompressRio *dr = (decompressRio *)ctx;
    return rioReadPartial(dr->inner, buf, len);
}

static size_t decompressRioRead(rio *r, void *buf, size_t len) {
    decompressRio *dr = (decompressRio *)r;
    if (dr->base.flags & RIO_FLAG_READ_ERROR) return 0;
    if (!dr->reader) {
        dr->base.flags |= RIO_FLAG_READ_ERROR;
        return 0;
    }

    uint8_t *dst = (uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t nread = streamReaderRead(dr->reader, dst, remaining);
        if (nread <= 0) {
            /* rio contract is full-or-fail; partial reads are an error. */
            dr->base.flags |= RIO_FLAG_READ_ERROR;
            return 0;
        }
        remaining -= (size_t)nread;
        dst += nread;
    }
    return len;
}

/* Reports transport bytes from the wrapped rio so progress tracks the source
 * stream rather than the decoded byte count. */
static off_t decompressRioTell(rio *r) {
    decompressRio *dr = (decompressRio *)r;
    return (off_t)dr->inner->processed_bytes;
}

streamReaderError decompressRioGetError(const decompressRio *dr) {
    if (!dr || !dr->reader) return STREAM_READER_ERROR_IO;
    return streamReaderGetError(dr->reader);
}

int decompressRioValidateEnd(decompressRio *dr) {
    if (!dr || !dr->reader) return -1;
    return streamReaderValidateEnd(dr->reader);
}

decompressRioInitResult rioInitWithDecompress(decompressRio *dr,
                                              rio *inner,
                                              const streamReaderConfig *cfg,
                                              streamReaderInfo *info) {
    streamReaderInfo local_info = {0};

    if (!dr || !inner || !cfg) return DECOMPRESS_RIO_INIT_ERROR;

    memset(dr, 0, sizeof(*dr));
    rioInitBase(&dr->base, decompressRioRead, rioWriteUnsupported, decompressRioTell,
                rioFlushNoop,
                RIO_FLAG_STREAMING_DECOMPRESSION | (inner->flags & RIO_FLAG_SKIP_RDB_CHECKSUM),
                rioCheckType(inner));
    dr->inner = inner;

    dr->reader = streamReaderCreate(cfg, decompressRioReadPartial, dr);
    if (!dr->reader) {
        dr->base.flags |= RIO_FLAG_READ_ERROR;
        return DECOMPRESS_RIO_INIT_ERROR;
    }
    if (streamReaderGetInfo(dr->reader, &local_info) != 0) {
        streamReaderError error_kind = streamReaderGetError(dr->reader);
        decompressRioDestroy(dr);
        return error_kind == STREAM_READER_ERROR_INCOMPATIBLE
                   ? DECOMPRESS_RIO_INIT_INCOMPATIBLE
                   : DECOMPRESS_RIO_INIT_ERROR;
    }

    if (local_info.compressed) dr->base.flags |= RIO_FLAG_STREAMING_COMPRESSION;
    if (info) *info = local_info;
    return DECOMPRESS_RIO_INIT_OK;
}

void decompressRioDestroy(decompressRio *dr) {
    if (!dr) return;
    if (dr->reader) {
        streamReaderDestroy(dr->reader);
        dr->reader = NULL;
    }
}
