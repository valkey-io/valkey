/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "rdb_transcoder.h"

#include "crc64.h"
#include "endianconv.h"
#include "server.h"

#include <string.h>

/* ===== Streaming RDB transcoder =====
 *
 * Converts a self-describing RDB byte stream into a target whole-stream codec,
 * emitting transcoded bytes through a callback. The input is auto-classified: a
 * VCS envelope is decoded, anything else is treated as a plaintext RDB. When the
 * source codec already equals the target, bytes are emitted verbatim with no
 * codec work.
 *
 *   input bytes -> streamPushReader (decode a VCS frame, or pass plaintext
 *   through) -> logical RDB bytes -> streamWriter (encode to a whole-stream
 *   codec) or a plaintext write -> emit callback
 *
 * It is decoupled from any particular sink (the emit callback hides that) and
 * from wire framing (the caller strips any $EOF mark before feeding bytes), so
 * it can be reused wherever an RDB stream must change codec. */

/* Initialize a transcoder that rewrites a self-describing RDB stream to the
 * `target` whole-stream codec, emitting transcoded bytes through `emit` (which
 * follows the streamWriter C_OK/C_ERR contract). The source codec is detected
 * from the stream, so it need not be known in advance. `checksum` applies only
 * to a plaintext target: when set, the decoded logical CRC64 trailer (zero, since
 * the source frame owned integrity) is replaced with a freshly computed one so
 * the file stays verifiable like a native save; a compressed target relies on its
 * own codec-frame checksums instead. */
void rdbTranscoderInit(rdbTranscoder *t, compressionAlgo target, bool checksum, streamWriterWriteFn emit, void *emit_ctx) {
    memset(t, 0, sizeof(*t));
    t->target = target;
    t->checksum = checksum;
    t->emit = emit;
    t->emit_ctx = emit_ctx;
    t->scratch = sdsempty();
}

/* Decide verbatim vs convert from the buffered leading bytes, and set up codec
 * contexts. A VCS envelope means the source is compressed; anything else
 * (including a body too short to hold an envelope) is a plaintext RDB. */
static int rdbTranscoderClassify(rdbTranscoder *t) {
    bool src_compressed = t->probe_len >= VCS_ENVELOPE_SIZE && t->probe[0] == VCS_MAGIC_0 &&
                          t->probe[1] == VCS_MAGIC_1 && t->probe[2] == VCS_MAGIC_2;
    /* Verbatim when the source codec equals the target. */
    bool verbatim = src_compressed
                        ? (t->target != ALGO_NONE && vcsCodecToAlgo(t->probe[VCS_OFFSET_CODEC]) == t->target)
                        : (t->target == ALGO_NONE);
    if (verbatim) {
        t->route = RDB_TRANSCODE_VERBATIM;
        return C_OK;
    }
    t->route = RDB_TRANSCODE_CONVERT;
    /* Always run the push reader: it decodes a VCS frame or forwards plaintext,
     * yielding logical RDB bytes either way. */
    streamPushReaderInit(&t->reader, VCS_STREAM_RDB);
    t->reader_active = true;
    t->encode = t->target != ALGO_NONE;
    if (t->encode) {
        if (streamWriterInit(&t->writer, t->target, t->checksum, t->emit, t->emit_ctx) == C_ERR) {
            t->failed = true;
            return C_ERR;
        }
        t->writer_active = true;
    } else {
        t->recompute_crc = t->checksum;
    }
    return C_OK;
}

/* Emit decoded plaintext bytes. With recompute_crc, hold back the trailing 8
 * bytes (the source's zero CRC64 trailer) and CRC64 the rest, so a fresh trailer
 * can be appended at finish. */
static int rdbTranscoderEmitPlain(rdbTranscoder *t, const uint8_t *data, size_t len) {
    if (!t->recompute_crc) return t->emit(t->emit_ctx, data, len);
    const size_t keep = sizeof(t->crc_hold);
    size_t total = t->crc_hold_len + len;
    if (total <= keep) {
        memcpy(t->crc_hold + t->crc_hold_len, data, len);
        t->crc_hold_len = total;
        return C_OK;
    }
    size_t emit = total - keep;
    size_t from_hold = emit < t->crc_hold_len ? emit : t->crc_hold_len;
    if (from_hold) {
        t->crc = crc64(t->crc, t->crc_hold, from_hold);
        if (t->emit(t->emit_ctx, t->crc_hold, from_hold) == C_ERR) return C_ERR;
    }
    size_t from_data = emit - from_hold;
    if (from_data) {
        t->crc = crc64(t->crc, data, from_data);
        if (t->emit(t->emit_ctx, data, from_data) == C_ERR) return C_ERR;
    }
    size_t rem_hold = t->crc_hold_len - from_hold;
    if (rem_hold) memmove(t->crc_hold, t->crc_hold + from_hold, rem_hold);
    memcpy(t->crc_hold + rem_hold, data + from_data, len - from_data);
    t->crc_hold_len = rem_hold + (len - from_data); /* == keep */
    return C_OK;
}

/* Emit already-classified bytes into the chosen route. */
static int rdbTranscoderRun(rdbTranscoder *t, const uint8_t *data, size_t len) {
    if (t->route == RDB_TRANSCODE_VERBATIM) return t->emit(t->emit_ctx, data, len);
    /* Convert: push into the decoder, drain decoded output into the encoder or a
     * plaintext write. Loop to honor NEED_OUTPUT backpressure (decode budget hit). */
    const void *src = data;
    size_t src_len = len;
    while (true) {
        sdsclear(t->scratch);
        streamPushReaderResult r =
            streamPushReaderFeed(&t->reader, src, src_len, &t->scratch, STREAM_READER_BUFFER_SIZE_DEFAULT);
        src = NULL;
        src_len = 0; /* Subsequent iterations drain with an empty feed. */
        if (r == STREAM_PUSH_READER_ERR) {
            t->failed = true;
            return C_ERR;
        }
        if (sdslen(t->scratch) > 0) {
            int wr = t->encode ? streamWriterWrite(&t->writer, t->scratch, sdslen(t->scratch))
                               : rdbTranscoderEmitPlain(t, (uint8_t *)t->scratch, sdslen(t->scratch));
            if (wr == C_ERR) {
                t->failed = true;
                return C_ERR;
            }
        }
        if (r == STREAM_PUSH_READER_NEED_OUTPUT) continue;
        /* OK: input consumed. FRAME_DONE: the frame ended; the caller has already
         * excluded any trailing bytes, so there is nothing more to feed. */
        break;
    }
    return C_OK;
}

/* Feed source bytes. The leading bytes are buffered on first use to classify the
 * source codec; thereafter bytes are emitted verbatim (source codec == target) or
 * decoded and re-encoded to the target. Errors are sticky. Returns C_OK/C_ERR. */
int rdbTranscoderWrite(rdbTranscoder *t, const void *buf, size_t len) {
    if (t->failed) return C_ERR;
    if (len == 0) return C_OK;
    const uint8_t *data = buf;
    if (t->route == RDB_TRANSCODE_UNDECIDED) {
        size_t take = VCS_ENVELOPE_SIZE - t->probe_len;
        if (take > len) take = len;
        memcpy(t->probe + t->probe_len, data, take);
        t->probe_len += take;
        data += take;
        len -= take;
        if (t->probe_len < VCS_ENVELOPE_SIZE) return C_OK; /* Need more to classify. */
        if (rdbTranscoderClassify(t) == C_ERR) return C_ERR;
        if (rdbTranscoderRun(t, t->probe, t->probe_len) == C_ERR) return C_ERR;
        t->probe_len = 0;
        if (len == 0) return C_OK;
    }
    return rdbTranscoderRun(t, data, len);
}

/* Flush any buffered bytes and close the output: finalize the encoder frame for a
 * compressed target, or append the recomputed CRC64 trailer for a plaintext one.
 * Call once after the final Write. Returns C_OK/C_ERR. */
int rdbTranscoderFinish(rdbTranscoder *t) {
    if (t->failed) return C_ERR;
    if (t->route == RDB_TRANSCODE_UNDECIDED) {
        /* Body shorter than a VCS envelope: it cannot be a frame, so classify (as
         * plaintext) and flush the remainder. */
        if (rdbTranscoderClassify(t) == C_ERR) return C_ERR;
        if (t->probe_len && rdbTranscoderRun(t, t->probe, t->probe_len) == C_ERR) return C_ERR;
        t->probe_len = 0;
    }
    if (t->route != RDB_TRANSCODE_CONVERT) return C_OK;
    if (t->encode) {
        if (streamWriterFinish(&t->writer) == C_ERR) {
            t->failed = true;
            return C_ERR;
        }
    } else if (t->recompute_crc) {
        uint64_t cksum = t->crc;
        memrev64ifbe(&cksum);
        if (t->emit(t->emit_ctx, (uint8_t *)&cksum, sizeof(cksum)) == C_ERR) {
            t->failed = true;
            return C_ERR;
        }
    }
    return C_OK;
}

/* Release the codec contexts and scratch memory. Does not flush; call Finish
 * first to produce a complete stream. */
void rdbTranscoderFree(rdbTranscoder *t) {
    if (t->reader_active) streamPushReaderFree(&t->reader);
    if (t->writer_active) streamWriterFree(&t->writer);
    sdsfree(t->scratch);
    t->reader_active = false;
    t->writer_active = false;
    t->scratch = NULL;
}
