/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Building and releasing a blockCompressor instance. The backends live in
 * compression_block_<algorithm>.c files (today only compression_block_lz4.c),
 * and this file is the only place that knows which ids map to which function
 * table. */

#include "fmacros.h" /* must come before any system header */
#include "compression_block_interface.h"
#include "server.h" /* C_OK, C_ERR */
#include "serverassert.h"
#include "zmalloc.h"

/* The function tables, one per compression_block_*.c file. Declared here and not in
 * compression_block_interface.h, because this is the only file that needs them: everyone else
 * calls blockCompressorCreate(). A backend this build leaves out has no table, so its
 * file is not compiled and its case below must be removed too. */
extern const blockCompressorApi blockCompressorApiLz4;

/* Applies the operator's range, then asks the backend. This is the function
 * behind blockCompressor.max_output_size, so it is the same for every backend
 * and the range check exists once. */
static size_t blockCompressorMaxOutputSize(const blockCompressor *instance, size_t input_len) {
    assert(instance != NULL);

    /* The range the operator asked for. 0 means no limit on that side. */
    if (input_len < instance->config.min_input_len) return 0;
    if (instance->config.max_input_len != 0 && input_len > instance->config.max_input_len) return 0;

    /* What the library itself can handle. */
    return instance->api->lib_max_output_size(input_len);
}

blockCompressor *blockCompressorCreate(compressionAlgo id, const blockCompressorConfig *config) {
    blockCompressor *c = zcalloc(sizeof(*c));

    switch (id) {
    case ALGO_LZ4:
        c->api = &blockCompressorApiLz4;
        c->dict_max_size = BLOCK_COMPRESSOR_LZ4_DICT_MAX;
        break;
    case ALGO_ZSTD:
        /* zstd is not vendored yet, so there is no function table to point at. */
        zfree(c);
        return NULL;
    case ALGO_NONE:
    default:
        zfree(c);
        return NULL;
    }

    c->id = id;
    c->name = compressionAlgoName(id);
    c->max_output_size = blockCompressorMaxOutputSize;
    if (config != NULL) c->config = *config;

    if (c->api->state_new(c) != C_OK) {
        /* state_new() may have allocated part of its scratch before it failed, so
         * give the backend a chance to release it. state_free() must accept a
         * partly built instance, which is why lz4StateFree() checks for NULL. */
        c->api->state_free(c);
        zfree(c);
        return NULL;
    }
    return c;
}

void blockCompressorFree(blockCompressor *c) {
    if (c == NULL) return;
    c->api->state_free(c);
    zfree(c);
}

const char *blockCompressorStrerror(int err) {
    switch (err) {
    case BLOCK_COMPRESSOR_ERR_NONE: return "no error";
    case BLOCK_COMPRESSOR_ERR_BAD_SIZE: return "value size out of range for this backend";
    case BLOCK_COMPRESSOR_ERR_COMPRESS: return "compression failed";
    case BLOCK_COMPRESSOR_ERR_DECOMPRESS: return "decompression failed";
    default: return "unknown compressor error";
    }
}
