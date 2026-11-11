/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef RDB_TRANSCODER_H
#define RDB_TRANSCODER_H

#include "compression.h"
#include "compression_stream.h"
#include "sds.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Streaming transcoder that rewrites a self-describing RDB byte stream (a VCS
 * frame or plaintext) into a target whole-stream codec, emitting through a
 * callback. Full pipeline and semantics are documented in rdb_transcoder.c. */

typedef enum {
    RDB_TRANSCODE_UNDECIDED = 0, /* Still probing the source codec. */
    RDB_TRANSCODE_VERBATIM,      /* Source codec == target: emit bytes as-is. */
    RDB_TRANSCODE_CONVERT,       /* Source codec != target: decode + re-encode. */
} rdbTranscodeRoute;

typedef struct rdbTranscoder {
    compressionAlgo target;   /* Output whole-stream codec. */
    bool checksum;            /* Whether a plaintext target should carry a CRC64. */
    streamWriterWriteFn emit; /* Sink for transcoded bytes. */
    void *emit_ctx;
    rdbTranscodeRoute route;
    uint8_t probe[VCS_ENVELOPE_SIZE]; /* Leading bytes buffered to classify the source. */
    size_t probe_len;
    bool encode; /* Target is a whole-stream codec: run the writer. */
    streamPushReader reader;
    bool reader_active;
    streamWriter writer;
    bool writer_active;
    sds scratch;        /* Decoded / passed-through bytes. */
    bool recompute_crc; /* Plaintext target + checksum: rebuild the CRC64 trailer. */
    uint64_t crc;
    uint8_t crc_hold[8]; /* Held-back trailing 8 bytes (the source CRC64 trailer). */
    size_t crc_hold_len;
    bool failed;
} rdbTranscoder;

/* Initialize a transcoder converting a self-describing RDB stream to `target`. */
void rdbTranscoderInit(rdbTranscoder *t, compressionAlgo target, bool checksum, streamWriterWriteFn emit, void *emit_ctx);
/* Feed source bytes. Returns C_OK/C_ERR. */
int rdbTranscoderWrite(rdbTranscoder *t, const void *buf, size_t len);
/* Flush and close the output. Returns C_OK/C_ERR. */
int rdbTranscoderFinish(rdbTranscoder *t);
/* Release codec contexts and scratch memory (does not flush). */
void rdbTranscoderFree(rdbTranscoder *t);

#endif /* RDB_TRANSCODER_H */
