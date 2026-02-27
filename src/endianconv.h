/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 */

#ifndef __ENDIANCONV_H
#define __ENDIANCONV_H

#include "config.h"
#include <stdint.h>

/* These bswap functions compile to a single bswap instruction. Verified for GCC
 * and Clang with -O2 and -O3.
 *
 * bswap64:
 *         mov     rax, rdi
 *         bswap   rax
 *         ret
 */

static inline uint16_t bswap16(uint16_t x) {
    return ((x & 0xFF00U) >> 8) |
           ((x & 0x00FFU) << 8);
}

static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0xFF000000U) >> 24) |
           ((x & 0x00FF0000U) >> 8) |
           ((x & 0x0000FF00U) << 8) |
           ((x & 0x000000FFU) << 24);
}

static inline uint64_t bswap64(uint64_t x) {
    return ((x & 0xFF00000000000000ULL) >> 56) |
           ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0x0000FF0000000000ULL) >> 24) |
           ((x & 0x000000FF00000000ULL) >> 8) |
           ((x & 0x00000000FF000000ULL) << 8) |
           ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x000000000000FF00ULL) << 40) |
           ((x & 0x00000000000000FFULL) << 56);
}

static inline void memrev16(void *p) {
    uint16_t *u = (uint16_t *)p;
    *u = bswap16(*u);
}

static inline void memrev32(void *p) {
    uint32_t *u = (uint32_t *)p;
    *u = bswap32(*u);
}

static inline void memrev64(void *p) {
    uint64_t *u = (uint64_t *)p;
    *u = bswap64(*u);
}

/* variants of the function doing the actual conversion only if the target
 * host is big endian */
#if (BYTE_ORDER == LITTLE_ENDIAN)
#define memrev16ifbe(p) ((void)(0))
#define memrev32ifbe(p) ((void)(0))
#define memrev64ifbe(p) ((void)(0))
#define intrev16ifbe(v) (v)
#define intrev32ifbe(v) (v)
#define intrev64ifbe(v) (v)
#else
#define memrev16ifbe(p) memrev16(p)
#define memrev32ifbe(p) memrev32(p)
#define memrev64ifbe(p) memrev64(p)
#define intrev16ifbe(v) bswap16(v)
#define intrev32ifbe(v) bswap32(v)
#define intrev64ifbe(v) bswap64(v)
#endif

/* The functions htonu64() and ntohu64() convert the specified value to
 * network byte ordering and back. In big endian systems they are no-ops. */
#if (BYTE_ORDER == BIG_ENDIAN)
#define htonu64(v) (v)
#define ntohu64(v) (v)
#else
#define htonu64(v) bswap64(v)
#define ntohu64(v) bswap64(v)
#endif
#endif
