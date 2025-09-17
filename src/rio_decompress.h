/*
 * RIO decompression filter for framed RDB input.
 *
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_RIO_DECOMPRESS_H
#define VALKEY_RIO_DECOMPRESS_H

#include <stddef.h>

#include "rio.h"
#include "sds.h"

typedef struct rio_decompress {
    rio *src;      /* underlying source (file/conn) */
    sds rawbuf;    /* current block's raw data */
    size_t pos;    /* read cursor within rawbuf */
    int eof;       /* seen last-block */
    rio rio_itf;   /* embedded rio interface */
} rio_decompress;

int rioInitDecompress(rio_decompress *rd, rio *src);

#endif /* VALKEY_RIO_DECOMPRESS_H */
