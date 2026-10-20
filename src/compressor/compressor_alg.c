/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Building and releasing a compressorAlg instance. The backends themselves live in
 * compressor_alg_lz4.c and compressor_alg_zstd.c, and this file is the only place
 * that knows which ids map to which function table. */

#include "compressor/compressor_alg.h"
#include "server.h" /* C_OK, C_ERR */
#include "serverassert.h"
#include "zmalloc.h"
#include <strings.h> /* strcasecmp */

/* The function tables, one per compressor_alg_*.c file. Declared here and not in
 * compressor_alg.h, because this is the only file that needs them: everyone else
 * calls newCompressor(). A backend this build leaves out has no table, so its
 * file is not compiled and its case below must be removed too. */
extern const compressorApi compressorApiLz4;

/* Applies the operator's range, then asks the backend. This is the function
 * behind compressorAlg.max_output_size, so it is the same for every backend
 * and the range check exists once. */
static size_t compressorMaxOutputSize(const compressorAlg *instance, size_t input_len) {
    assert(instance != NULL);

    /* The range the operator asked for. 0 means no limit on that side. */
    if (input_len < instance->config.min_input_len) return 0;
    if (instance->config.max_input_len != 0 && input_len > instance->config.max_input_len) return 0;

    /* What the library itself can handle. */
    return instance->api->lib_max_output_size(input_len);
}

compressorAlg *newCompressor(compressorAlgId id, const compressorConfig *config) {
    compressorAlg *c = zcalloc(sizeof(*c));

    switch (id) {
    case COMPRESSOR_ALG_LZ4:
        c->api = &compressorApiLz4;
        c->dict_max_size = COMPRESSOR_LZ4_DICT_MAX;
        break;
    case COMPRESSOR_ALG_ZSTD:
        /* zstd is not vendored yet, so there is no function table to point at. */
        zfree(c);
        return NULL;
    case COMPRESSOR_ALG_NONE:
    default:
        zfree(c);
        return NULL;
    }

    c->id = id;
    c->name = compressorAlgIdName(id);
    c->max_output_size = compressorMaxOutputSize;
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

void freeCompressor(compressorAlg *c) {
    if (c == NULL) return;
    c->api->state_free(c);
    zfree(c);
}

const char *compressorStrerror(int err) {
    switch (err) {
    case COMPRESSOR_ERR_NONE: return "no error";
    case COMPRESSOR_ERR_BAD_SIZE: return "value size out of range for this backend";
    case COMPRESSOR_ERR_COMPRESS: return "compression failed";
    case COMPRESSOR_ERR_DECOMPRESS: return "decompression failed";
    default: return "unknown compressor error";
    }
}

int compressorAlgIdFromName(const char *name, compressorAlgId *id_out) {
    if (name == NULL || id_out == NULL) return C_ERR;
    if (!strcasecmp(name, "off")) {
        *id_out = COMPRESSOR_ALG_NONE;
    } else if (!strcasecmp(name, "lz4")) {
        *id_out = COMPRESSOR_ALG_LZ4;
    } else if (!strcasecmp(name, "zstd")) {
        *id_out = COMPRESSOR_ALG_ZSTD;
    } else {
        return C_ERR;
    }
    return C_OK;
}

const char *compressorAlgIdName(compressorAlgId id) {
    switch (id) {
    case COMPRESSOR_ALG_LZ4: return "lz4";
    case COMPRESSOR_ALG_ZSTD: return "zstd";
    case COMPRESSOR_ALG_NONE: return "off";
    default: return "off";
    }
}
