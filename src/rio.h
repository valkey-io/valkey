/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2019, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#ifndef VALKEY_RIO_H
#define VALKEY_RIO_H

#include <stdio.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdint.h>
#include "sds.h"
#include "connection.h"

#define RIO_FLAG_READ_ERROR (1 << 0)
#define RIO_FLAG_WRITE_ERROR (1 << 1)
#define RIO_FLAG_CLOSE_ASAP (1 << 2) /* Rio was closed asynchronously during the current rio operation. */
#define RIO_FLAG_SKIP_RDB_CHECKSUM (1 << 3)
#define RIO_FLAG_STREAMING_COMPRESSION (1 << 4) /* Input uses whole-stream compression. */

#define RIO_TYPE_FILE (1 << 0)
#define RIO_TYPE_BUFFER (1 << 1)
#define RIO_TYPE_CONN (1 << 2)
#define RIO_TYPE_FD (1 << 3)

struct streamWriter;
struct streamReader;

struct _rio {
    /* Backend functions. read and write are the exact-length interface used by
     * parsers: zero means failure, nonzero means all len bytes were processed. */
    size_t (*read)(struct _rio *, void *buf, size_t len);
    size_t (*write)(struct _rio *, const void *buf, size_t len);
    off_t (*tell)(struct _rio *);
    int (*flush)(struct _rio *);
    /* Partial-read backend. Here len is a maximum, not a requirement: return
     * >0 bytes read, 0 on EOF, or -1 on error. A stream decoder uses this
     * because it cannot know how many encoded bytes will produce the decoded
     * bytes requested by its caller. NULL when the backend cannot be read. */
    ssize_t (*read_some)(struct _rio *, void *buf, size_t len);
    /* The update_cksum method if not NULL is used to compute the checksum of
     * all the data that was read or written so far. The method should be
     * designed so that can be called with the current checksum, and the buf
     * and len fields pointing to the new block of data to add to the checksum
     * computation. */
    void (*update_cksum)(struct _rio *, const void *buf, size_t len);

    /* The current checksum and flags (see RIO_FLAG_*) */
    uint64_t cksum, flags;

    /* number of bytes read or written */
    size_t processed_bytes;

    /* Number of bytes read or written on the stream's concrete I/O path. This
     * differs from processed_bytes when a stream transforms data. */
    size_t stream_processed_bytes;

    /* Maximum size of one backend operation, not a total byte limit. Zero
     * means unlimited. rioRead/rioWrite split larger requests into chunks. */
    size_t max_processing_chunk;

    /* Optional stream transforms. The caller owns their lifetime, and rio only
     * dispatches logical bytes through these opaque objects:
     *
     *   write: rioWrite -> streamWriter -> rioWriteRaw -> backend write
     *   read:  rioRead  <- streamReader <- rioReadRawPartial <- backend read
     *
     * Compression policy, framing, buffers, and codec state remain in
     * compression_stream. */
    struct streamWriter *stream_writer;
    struct streamReader *stream_reader;

    /* Backend-specific vars. */
    union {
        /* In-memory buffer target. */
        struct {
            sds ptr;
            off_t pos;
        } buffer;
        /* Stdio file pointer target. */
        struct {
            FILE *fp;
            off_t buffered;             /* Bytes written since last fsync. */
            off_t autosync;             /* fsync after 'autosync' bytes written. */
            unsigned reclaim_cache : 1; /* A flag to indicate reclaim cache after fsync */
        } file;
        /* Connection object (used to read from socket) */
        struct {
            connection *conn;   /* Connection */
            off_t pos;          /* pos in buf that was returned */
            sds buf;            /* buffered data */
            size_t read_limit;  /* don't allow to buffer/read more than that */
            size_t read_so_far; /* amount of data read from the rio (not buffered) */
        } conn;
        /* FD target (used to write to pipe). */
        struct {
            int fd; /* File descriptor. */
            off_t pos;
            sds buf;
        } fd;
        /* Multiple connections target (used to write to N sockets). */
        struct {
            connection **conns; /* Connections */
            int *state;         /* Error state of each fd. 0 (if ok) or errno. */
            int numconns;
            off_t pos;
            sds buf;
        } connset;
    } io;
};

typedef struct _rio rio;

/* The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */

/* Implemented in rio.c, where the opaque stream types are visible. */
size_t rioWriteStream(rio *r, const void *buf, size_t len);
size_t rioReadStream(rio *r, void *buf, size_t len);

/* Write directly to the concrete backend, bypassing the stream writer and
 * logical checksum/accounting. streamWriter uses this to emit encoded bytes
 * without recursively invoking itself. */
static inline size_t rioWriteRaw(rio *r, const void *buf, size_t len) {
    if (r->flags & RIO_FLAG_WRITE_ERROR || r->flags & RIO_FLAG_CLOSE_ASAP) return 0;
    while (len) {
        size_t bytes_to_write =
            (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        if (r->write(r, buf, bytes_to_write) == 0) {
            r->flags |= RIO_FLAG_WRITE_ERROR;
            return 0;
        }
        buf = (const char *)buf + bytes_to_write;
        len -= bytes_to_write;
        r->stream_processed_bytes += bytes_to_write;
    }
    return 1;
}

static inline size_t rioWrite(rio *r, const void *buf, size_t len) {
    if (r->flags & RIO_FLAG_WRITE_ERROR || r->flags & RIO_FLAG_CLOSE_ASAP) return 0;
    while (len) {
        size_t bytes_to_write =
            (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        if (r->update_cksum) r->update_cksum(r, buf, bytes_to_write);
        if (r->stream_writer) {
            if (rioWriteStream(r, buf, bytes_to_write) == 0) return 0;
        } else {
            if (rioWriteRaw(r, buf, bytes_to_write) == 0) return 0;
        }
        buf = (const char *)buf + bytes_to_write;
        len -= bytes_to_write;
        r->processed_bytes += bytes_to_write;
    }
    return 1;
}

static inline size_t rioRead(rio *r, void *buf, size_t len) {
    if (r->flags & RIO_FLAG_READ_ERROR || r->flags & RIO_FLAG_CLOSE_ASAP) return 0;
    while (len) {
        size_t bytes_to_read =
            (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        if (r->stream_reader) {
            if (rioReadStream(r, buf, bytes_to_read) == 0) return 0;
        } else {
            if (r->read(r, buf, bytes_to_read) == 0) {
                r->flags |= RIO_FLAG_READ_ERROR;
                return 0;
            }
            r->stream_processed_bytes += bytes_to_read;
        }
        if (r->update_cksum) r->update_cksum(r, buf, bytes_to_read);
        buf = (char *)buf + bytes_to_read;
        len -= bytes_to_read;
        r->processed_bytes += bytes_to_read;
    }
    return 1;
}

static inline off_t rioTell(rio *r) {
    /* Stream readers report physical bytes consumed from the source, which
     * drives file-loading progress for decoded streams. */
    if (r->stream_reader) return (off_t)r->stream_processed_bytes;
    return r->tell(r);
}

static inline int rioFlushRaw(rio *r) {
    return r->flush ? r->flush(r) : 1;
}

static inline int rioFlush(rio *r) {
    return rioFlushRaw(r);
}

static inline void rioCloseASAP(rio *r) {
    r->flags |= RIO_FLAG_CLOSE_ASAP;
}

/* This function allows to know if there was a read error in any past
 * operation, since the rio stream was created or since the last call
 * to rioClearError(). */
static inline int rioGetReadError(rio *r) {
    return (r->flags & RIO_FLAG_READ_ERROR) != 0;
}

/* Like rioGetReadError() but for write errors. */
static inline int rioGetWriteError(rio *r) {
    return (r->flags & RIO_FLAG_WRITE_ERROR) != 0;
}

/* Like rioGetReadError() but for async close errors. */
static inline int rioGetAsyncCloseError(rio *r) {
    return (r->flags & RIO_FLAG_CLOSE_ASAP) != 0;
}

static inline void rioClearErrors(rio *r) {
    r->flags &= ~(RIO_FLAG_READ_ERROR | RIO_FLAG_WRITE_ERROR | RIO_FLAG_CLOSE_ASAP);
}

void rioInitWithFile(rio *r, FILE *fp);
void rioInitWithBuffer(rio *r, sds s);
void rioInitWithConn(rio *r, connection *conn, size_t read_limit);
void rioInitWithFd(rio *r, int fd);
void rioAttachStreamWriter(rio *r, struct streamWriter *writer);
void rioDetachStreamWriter(rio *r);
void rioAttachStreamReader(rio *r, struct streamReader *reader);
void rioDetachStreamReader(rio *r);

void rioFreeFd(rio *r);
void rioFreeConn(rio *r, sds *out_remainingBufferedData);

size_t rioWriteBulkCount(rio *r, char prefix, long count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, long long l);
size_t rioWriteBulkDouble(rio *r, double d);

struct serverObject;
int rioWriteBulkObject(rio *r, struct serverObject *obj);

void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len);
ssize_t rioReadRawPartial(rio *r, void *buf, size_t len);
void rioSetAutoSync(rio *r, off_t bytes);
void rioSetReclaimCache(rio *r, int enabled);
uint8_t rioCheckType(rio *r);
void rioInitWithConnset(rio *r, connection **conns, int numconns);
void rioFreeConnset(rio *r);
#endif
