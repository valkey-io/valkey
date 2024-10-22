/* Listpack -- A lists of strings serialization format
 *
 * This file implements the specification you can find at:
 *
 *  https://github.com/antirez/listpack
 *
 * Copyright (c) 2017,2020, Redis Ltd.
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

#include <stdint.h>
#include <limits.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "listpack.h"
#include "listpack_malloc.h"
#include "serverassert.h"
#include "util.h"

#define LP_HDR_SIZE 6 /* 32 bit total len + 16 bit number of elements. */
#define LP_HDR_NUMELE_UNKNOWN UINT16_MAX
#define LP_MAX_INT_ENCODING_LEN 9
#define LP_MAX_BACKLEN_SIZE 5
#define LP_ENCODING_INT 0
#define LP_ENCODING_STRING 1

#define LP_ENCODING_7BIT_UINT 0
#define LP_ENCODING_7BIT_UINT_MASK 0x80
#define LP_ENCODING_IS_7BIT_UINT(byte) (((byte) & LP_ENCODING_7BIT_UINT_MASK) == LP_ENCODING_7BIT_UINT)
#define LP_ENCODING_7BIT_UINT_ENTRY_SIZE 2

#define LP_ENCODING_6BIT_STR 0x80
#define LP_ENCODING_6BIT_STR_MASK 0xC0
#define LP_ENCODING_IS_6BIT_STR(byte) (((byte) & LP_ENCODING_6BIT_STR_MASK) == LP_ENCODING_6BIT_STR)

#define LP_ENCODING_13BIT_INT 0xC0
#define LP_ENCODING_13BIT_INT_MASK 0xE0
#define LP_ENCODING_IS_13BIT_INT(byte) (((byte) & LP_ENCODING_13BIT_INT_MASK) == LP_ENCODING_13BIT_INT)
#define LP_ENCODING_13BIT_INT_ENTRY_SIZE 3

#define LP_ENCODING_12BIT_STR 0xE0
#define LP_ENCODING_12BIT_STR_MASK 0xF0
#define LP_ENCODING_IS_12BIT_STR(byte) (((byte) & LP_ENCODING_12BIT_STR_MASK) == LP_ENCODING_12BIT_STR)

#define LP_ENCODING_16BIT_INT 0xF1
#define LP_ENCODING_16BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_16BIT_INT(byte) (((byte) & LP_ENCODING_16BIT_INT_MASK) == LP_ENCODING_16BIT_INT)
#define LP_ENCODING_16BIT_INT_ENTRY_SIZE 4

#define LP_ENCODING_24BIT_INT 0xF2
#define LP_ENCODING_24BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_24BIT_INT(byte) (((byte) & LP_ENCODING_24BIT_INT_MASK) == LP_ENCODING_24BIT_INT)
#define LP_ENCODING_24BIT_INT_ENTRY_SIZE 5

#define LP_ENCODING_32BIT_INT 0xF3
#define LP_ENCODING_32BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_32BIT_INT(byte) (((byte) & LP_ENCODING_32BIT_INT_MASK) == LP_ENCODING_32BIT_INT)
#define LP_ENCODING_32BIT_INT_ENTRY_SIZE 6

#define LP_ENCODING_64BIT_INT 0xF4
#define LP_ENCODING_64BIT_INT_MASK 0xFF
#define LP_ENCODING_IS_64BIT_INT(byte) (((byte) & LP_ENCODING_64BIT_INT_MASK) == LP_ENCODING_64BIT_INT)
#define LP_ENCODING_64BIT_INT_ENTRY_SIZE 10

#define LP_ENCODING_32BIT_STR 0xF0
#define LP_ENCODING_32BIT_STR_MASK 0xFF
#define LP_ENCODING_IS_32BIT_STR(byte) (((byte) & LP_ENCODING_32BIT_STR_MASK) == LP_ENCODING_32BIT_STR)

#define LP_EOF 0xFF

#define LP_ENCODING_6BIT_STR_LEN(p) ((p)[0] & 0x3F)
#define LP_ENCODING_12BIT_STR_LEN(p) ((((p)[0] & 0xF) << 8) | (p)[1])
#define LP_ENCODING_32BIT_STR_LEN(p) \
    (((uint32_t)(p)[1] << 0) | ((uint32_t)(p)[2] << 8) | ((uint32_t)(p)[3] << 16) | ((uint32_t)(p)[4] << 24))

#define lpGetTotalBytes(p) \
    (((uint32_t)(p)[0] << 0) | ((uint32_t)(p)[1] << 8) | ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))

#define lpGetNumElements(p) (((uint32_t)(p)[4] << 0) | ((uint32_t)(p)[5] << 8))
#define lpSetTotalBytes(p, v)        \
    do {                             \
        (p)[0] = (v) & 0xff;         \
        (p)[1] = ((v) >> 8) & 0xff;  \
        (p)[2] = ((v) >> 16) & 0xff; \
        (p)[3] = ((v) >> 24) & 0xff; \
    } while (0)

#define lpSetNumElements(p, v)      \
    do {                            \
        (p)[4] = (v) & 0xff;        \
        (p)[5] = ((v) >> 8) & 0xff; \
    } while (0)

/* Validates that 'p' is not outside the listpack.
 * All function that return a pointer to an element in the listpack will assert
 * that this element is valid, so it can be freely used.
 * Generally functions such lpNext and lpDelete assume the input pointer is
 * already validated (since it's the return value of another function). */
#define ASSERT_INTEGRITY(lp, p)                                                  \
    do {                                                                         \
        assert((p) >= (lp) + LP_HDR_SIZE && (p) < (lp) + lpGetTotalBytes((lp))); \
    } while (0)

/* Similar to the above, but validates the entire element length rather than just
 * it's pointer. */
#define ASSERT_INTEGRITY_LEN(lp, p, len)                                                 \
    do {                                                                                 \
        assert((p) >= (lp) + LP_HDR_SIZE && (p) + (len) < (lp) + lpGetTotalBytes((lp))); \
    } while (0)

/* Don't let listpacks grow over 1GB in any case, don't wanna risk overflow in
 * Total Bytes header field */
#define LISTPACK_MAX_SAFETY_SIZE (1 << 30)
int lpSafeToAdd(unsigned char *lp, size_t add) {
    size_t len = lp ? lpGetTotalBytes(lp) : 0;
    if (len + add > LISTPACK_MAX_SAFETY_SIZE) return 0;
    return 1;
}

/* Convert a string into a signed 64 bit integer.
 * The function returns 1 if the string could be parsed into a (non-overflowing)
 * signed 64 bit int, 0 otherwise. The 'value' will be set to the parsed value
 * when the function returns success.
 *
 * Note that this function demands that the string strictly represents
 * a int64 value: no spaces or other characters before or after the string
 * representing the number are accepted, nor zeroes at the start if not
 * for the string "0" representing the zero number.
 *
 * Because of its strictness, it is safe to use this function to check if
 * you can convert a string into a long long, and obtain back the string
 * from the number without any loss in the string representation. *
 *
 * -----------------------------------------------------------------------------
 *
 * Credits: this function was adapted from the Redis OSS source code, file
 * "utils.c", function string2ll(), and is copyright:
 *
 * Copyright(C) 2011, Pieter Noordhuis
 * Copyright(C) 2011, Redis Ltd.
 *
 * The function is released under the BSD 3-clause license.
 */
int lpStringToInt64(const char *s, unsigned long slen, int64_t *value) {
    const char *p = s;
    unsigned long plen = 0;
    int negative = 0;
    uint64_t v;

    /* Abort if length indicates this cannot possibly be an int */
    if (slen == 0 || slen >= LONG_STR_SIZE) return 0;

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0') {
        if (value != NULL) *value = 0;
        return 1;
    }

    if (p[0] == '-') {
        negative = 1;
        p++;
        plen++;

        /* Abort on only a negative sign. */
        if (plen == slen) return 0;
    }

    /* First digit should be 1-9, otherwise the string should just be 0. */
    if (p[0] >= '1' && p[0] <= '9') {
        v = p[0] - '0';
        p++;
        plen++;
    } else {
        return 0;
    }

    while (plen < slen && p[0] >= '0' && p[0] <= '9') {
        if (v > (UINT64_MAX / 10)) /* Overflow. */
            return 0;
        v *= 10;

        if (v > (UINT64_MAX - (p[0] - '0'))) /* Overflow. */
            return 0;
        v += p[0] - '0';

        p++;
        plen++;
    }

    /* Return if not all bytes were used. */
    if (plen < slen) return 0;

    if (negative) {
        if (v > ((uint64_t)(-(INT64_MIN + 1)) + 1)) /* Overflow. */
            return 0;
        if (value != NULL) *value = -v;
    } else {
        if (v > INT64_MAX) /* Overflow. */
            return 0;
        if (value != NULL) *value = v;
    }
    return 1;
}

/* Create a new, empty listpack.
 * On success the new listpack is returned, otherwise an error is returned.
 * Pre-allocate at least `capacity` bytes of memory,
 * over-allocated memory can be shrunk by `lpShrinkToFit`.
 * */
unsigned char *lpNew(size_t capacity) {
    unsigned char *lp = lp_malloc(capacity > LP_HDR_SIZE + 1 ? capacity : LP_HDR_SIZE + 1);
    if (lp == NULL) return NULL;
    lpSetTotalBytes(lp, LP_HDR_SIZE + 1);
    lpSetNumElements(lp, 0);
    lp[LP_HDR_SIZE] = LP_EOF;
    return lp;
}

/* Free the specified listpack. */
void lpFree(unsigned char *lp) {
    lp_free(lp);
}

/* Shrink the memory to fit. */
unsigned char *lpShrinkToFit(unsigned char *lp) {
    size_t size = lpGetTotalBytes(lp);
    if (size < lp_malloc_size(lp)) {
        return lp_realloc(lp, size);
    } else {
        return lp;
    }
}

/* Stores the integer encoded representation of 'v' in the 'intenc' buffer. */
static inline void lpEncodeIntegerGetType(int64_t v, unsigned char *intenc, uint64_t *enclen) {
    if (v >= 0 && v <= 127) {
        /* Single byte 0-127 integer. */
        intenc[0] = v;
        *enclen = 1;
    } else if (v >= -4096 && v <= 4095) {
        /* 13 bit integer. */
        if (v < 0) v = ((int64_t)1 << 13) + v;
        intenc[0] = (v >> 8) | LP_ENCODING_13BIT_INT;
        intenc[1] = v & 0xff;
        *enclen = 2;
    } else if (v >= -32768 && v <= 32767) {
        /* 16 bit integer. */
        if (v < 0) v = ((int64_t)1 << 16) + v;
        intenc[0] = LP_ENCODING_16BIT_INT;
        intenc[1] = v & 0xff;
        intenc[2] = v >> 8;
        *enclen = 3;
    } else if (v >= -8388608 && v <= 8388607) {
        /* 24 bit integer. */
        if (v < 0) v = ((int64_t)1 << 24) + v;
        intenc[0] = LP_ENCODING_24BIT_INT;
        intenc[1] = v & 0xff;
        intenc[2] = (v >> 8) & 0xff;
        intenc[3] = v >> 16;
        *enclen = 4;
    } else if (v >= -2147483648 && v <= 2147483647) {
        /* 32 bit integer. */
        if (v < 0) v = ((int64_t)1 << 32) + v;
        intenc[0] = LP_ENCODING_32BIT_INT;
        intenc[1] = v & 0xff;
        intenc[2] = (v >> 8) & 0xff;
        intenc[3] = (v >> 16) & 0xff;
        intenc[4] = v >> 24;
        *enclen = 5;
    } else {
        /* 64 bit integer. */
        uint64_t uv = v;
        intenc[0] = LP_ENCODING_64BIT_INT;
        intenc[1] = uv & 0xff;
        intenc[2] = (uv >> 8) & 0xff;
        intenc[3] = (uv >> 16) & 0xff;
        intenc[4] = (uv >> 24) & 0xff;
        intenc[5] = (uv >> 32) & 0xff;
        intenc[6] = (uv >> 40) & 0xff;
        intenc[7] = (uv >> 48) & 0xff;
        intenc[8] = uv >> 56;
        *enclen = 9;
    }
}

/* Given an element 'ele' of size 'size', determine if the element can be
 * represented inside the listpack encoded as integer, and returns
 * LP_ENCODING_INT if so. Otherwise returns LP_ENCODING_STR if no integer
 * encoding is possible.
 *
 * If the LP_ENCODING_INT is returned, the function stores the integer encoded
 * representation of the element in the 'intenc' buffer.
 *
 * Regardless of the returned encoding, 'enclen' is populated by reference to
 * the number of bytes that the string or integer encoded element will require
 * in order to be represented. */
static inline int lpEncodeGetType(unsigned char *ele, uint32_t size, unsigned char *intenc, uint64_t *enclen) {
    int64_t v;
    if (lpStringToInt64((const char *)ele, size, &v)) {
        lpEncodeIntegerGetType(v, intenc, enclen);
        return LP_ENCODING_INT;
    } else {
        if (size < 64)
            *enclen = 1 + size;
        else if (size < 4096)
            *enclen = 2 + size;
        else
            *enclen = 5 + (uint64_t)size;
        return LP_ENCODING_STRING;
    }
}

/* Store a reverse-encoded variable length field, representing the length
 * of the previous element of size 'l', in the target buffer 'buf'.
 * The function returns the number of bytes used to encode it, from
 * 1 to 5. If 'buf' is NULL the function just returns the number of bytes
 * needed in order to encode the backlen. */
static inline unsigned long lpEncodeBacklen(unsigned char *buf, uint64_t l) {
    if (l <= 127) {
        if (buf) buf[0] = l;
        return 1;
    } else if (l < 16383) {
        if (buf) {
            buf[0] = l >> 7;
            buf[1] = (l & 127) | 128;
        }
        return 2;
    } else if (l < 2097151) {
        if (buf) {
            buf[0] = l >> 14;
            buf[1] = ((l >> 7) & 127) | 128;
            buf[2] = (l & 127) | 128;
        }
        return 3;
    } else if (l < 268435455) {
        if (buf) {
            buf[0] = l >> 21;
            buf[1] = ((l >> 14) & 127) | 128;
            buf[2] = ((l >> 7) & 127) | 128;
            buf[3] = (l & 127) | 128;
        }
        return 4;
    } else {
        if (buf) {
            buf[0] = l >> 28;
            buf[1] = ((l >> 21) & 127) | 128;
            buf[2] = ((l >> 14) & 127) | 128;
            buf[3] = ((l >> 7) & 127) | 128;
            buf[4] = (l & 127) | 128;
        }
        return 5;
    }
}

/* Decode the backlen and returns it. If the encoding looks invalid (more than
 * 5 bytes are used), UINT64_MAX is returned to report the problem. */
static inline uint64_t lpDecodeBacklen(unsigned char *p) {
    uint64_t val = 0;
    uint64_t shift = 0;
    do {
        val |= (uint64_t)(p[0] & 127) << shift;
        if (!(p[0] & 128)) break;
        shift += 7;
        p--;
        if (shift > 28) return UINT64_MAX;
    } while (1);
    return val;
}

/* Encode the string element pointed by 's' of size 'len' in the target
 * buffer 's'. The function should be called with 'buf' having always enough
 * space for encoding the string. This is done by calling lpEncodeGetType()
 * before calling this function. */
static inline void lpEncodeString(unsigned char *buf, unsigned char *s, uint32_t len) {
    if (len < 64) {
        buf[0] = len | LP_ENCODING_6BIT_STR;
        memcpy(buf + 1, s, len);
    } else if (len < 4096) {
        buf[0] = (len >> 8) | LP_ENCODING_12BIT_STR;
        buf[1] = len & 0xff;
        memcpy(buf + 2, s, len);
    } else {
        buf[0] = LP_ENCODING_32BIT_STR;
        buf[1] = len & 0xff;
        buf[2] = (len >> 8) & 0xff;
        buf[3] = (len >> 16) & 0xff;
        buf[4] = (len >> 24) & 0xff;
        memcpy(buf + 5, s, len);
    }
}

/* Return the encoded length of the listpack element pointed by 'p'.
 * This includes the encoding byte, length bytes, and the element data itself.
 * If the element encoding is wrong then 0 is returned.
 * Note that this method may access additional bytes (in case of 12 and 32 bit
 * str), so should only be called when we know 'p' was already validated by
 * lpCurrentEncodedSizeBytes or ASSERT_INTEGRITY_LEN (possibly since 'p' is
 * a return value of another function that validated its return. */
static inline uint32_t lpCurrentEncodedSizeUnsafe(unsigned char *p) {
    if (LP_ENCODING_IS_7BIT_UINT(p[0])) return 1;
    if (LP_ENCODING_IS_6BIT_STR(p[0])) return 1 + LP_ENCODING_6BIT_STR_LEN(p);
    if (LP_ENCODING_IS_13BIT_INT(p[0])) return 2;
    if (LP_ENCODING_IS_16BIT_INT(p[0])) return 3;
    if (LP_ENCODING_IS_24BIT_INT(p[0])) return 4;
    if (LP_ENCODING_IS_32BIT_INT(p[0])) return 5;
    if (LP_ENCODING_IS_64BIT_INT(p[0])) return 9;
    if (LP_ENCODING_IS_12BIT_STR(p[0])) return 2 + LP_ENCODING_12BIT_STR_LEN(p);
    if (LP_ENCODING_IS_32BIT_STR(p[0])) return 5 + LP_ENCODING_32BIT_STR_LEN(p);
    if (p[0] == LP_EOF) return 1;
    return 0;
}

/* Return bytes needed to encode the length of the listpack element pointed by 'p'.
 * This includes just the encoding byte, and the bytes needed to encode the length
 * of the element (excluding the element data itself)
 * If the element encoding is wrong then 0 is returned. */
static inline uint32_t lpCurrentEncodedSizeBytes(unsigned char *p) {
    if (LP_ENCODING_IS_7BIT_UINT(p[0])) return 1;
    if (LP_ENCODING_IS_6BIT_STR(p[0])) return 1;
    if (LP_ENCODING_IS_13BIT_INT(p[0])) return 1;
    if (LP_ENCODING_IS_16BIT_INT(p[0])) return 1;
    if (LP_ENCODING_IS_24BIT_INT(p[0])) return 1;
    if (LP_ENCODING_IS_32BIT_INT(p[0])) return 1;
    if (LP_ENCODING_IS_64BIT_INT(p[0])) return 1;
    if (LP_ENCODING_IS_12BIT_STR(p[0])) return 2;
    if (LP_ENCODING_IS_32BIT_STR(p[0])) return 5;
    if (p[0] == LP_EOF) return 1;
    return 0;
}

/* Skip the current entry returning the next. It is invalid to call this
 * function if the current element is the EOF element at the end of the
 * listpack, however, while this function is used to implement lpNext(),
 * it does not return NULL when the EOF element is encountered. */
unsigned char *lpSkip(unsigned char *p) {
    unsigned long entrylen = lpCurrentEncodedSizeUnsafe(p);
    entrylen += lpEncodeBacklen(NULL, entrylen);
    p += entrylen;
    return p;
}

/* If 'p' points to an element of the listpack, calling lpNext() will return
 * the pointer to the next element (the one on the right), or NULL if 'p'
 * already pointed to the last element of the listpack. */
unsigned char *lpNext(unsigned char *lp, unsigned char *p) {
    assert(p);
    (void)(lp);
    p = lpSkip(p);
    if (p[0] == LP_EOF) return NULL;
    return p;
}

/* If 'p' points to an element of the listpack, calling lpPrev() will return
 * the pointer to the previous element (the one on the left), or NULL if 'p'
 * already pointed to the first element of the listpack. */
unsigned char *lpPrev(unsigned char *lp, unsigned char *p) {
    assert(p);
    if (p - lp == LP_HDR_SIZE) return NULL;
    p--; /* Seek the first backlen byte of the last element. */
    uint64_t prevlen = lpDecodeBacklen(p);
    prevlen += lpEncodeBacklen(NULL, prevlen);
    p -= prevlen - 1; /* Seek the first byte of the previous entry. */
    return p;
}

/* Return a pointer to the first element of the listpack, or NULL if the
 * listpack has no elements. */
unsigned char *lpFirst(unsigned char *lp) {
    unsigned char *p = lp + LP_HDR_SIZE; /* Skip the header. */
    if (p[0] == LP_EOF) return NULL;
    return p;
}

/* Return a pointer to the last element of the listpack, or NULL if the
 * listpack has no elements. */
unsigned char *lpLast(unsigned char *lp) {
    unsigned char *p = lp + lpGetTotalBytes(lp) - 1; /* Seek EOF element. */
    return lpPrev(lp, p);                            /* Will return NULL if EOF is the only element. */
}

/* Return the number of elements inside the listpack. This function attempts
 * to use the cached value when within range, otherwise a full scan is
 * needed. As a side effect of calling this function, the listpack header
 * could be modified, because if the count is found to be already within
 * the 'numele' header field range, the new value is set. */
unsigned long lpLength(unsigned char *lp) {
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN) return numele;

    /* Too many elements inside the listpack. We need to scan in order
     * to get the total number. */
    uint32_t count = 0;
    unsigned char *p = lpFirst(lp);
    while (p) {
        count++;
        p = lpNext(lp, p);
    }

    /* If the count is again within range of the header numele field,
     * set it. */
    if (count < LP_HDR_NUMELE_UNKNOWN) lpSetNumElements(lp, count);
    return count;
}

/* Return the listpack element pointed by 'p'.
 *
 * The function changes behavior depending on the passed 'intbuf' value.
 * Specifically, if 'intbuf' is NULL:
 *
 * If the element is internally encoded as an integer, the function returns
 * NULL and populates the integer value by reference in 'count'. Otherwise if
 * the element is encoded as a string a pointer to the string (pointing inside
 * the listpack itself) is returned, and 'count' is set to the length of the
 * string.
 *
 * If instead 'intbuf' points to a buffer passed by the caller, that must be
 * at least LP_INTBUF_SIZE bytes, the function always returns the element as
 * it was a string (returning the pointer to the string and setting the
 * 'count' argument to the string length by reference). However if the element
 * is encoded as an integer, the 'intbuf' buffer is used in order to store
 * the string representation.
 *
 * The user should use one or the other form depending on what the value will
 * be used for. If there is immediate usage for an integer value returned
 * by the function, than to pass a buffer (and convert it back to a number)
 * is of course useless.
 *
 * If 'entry_size' is not NULL, *entry_size is set to the entry length of the
 * listpack element pointed by 'p'. This includes the encoding bytes, length
 * bytes, the element data itself, and the backlen bytes.
 *
 * If the function is called against a badly encoded ziplist, so that there
 * is no valid way to parse it, the function returns like if there was an
 * integer encoded with value 12345678900000000 + <unrecognized byte>, this may
 * be an hint to understand that something is wrong. To crash in this case is
 * not sensible because of the different requirements of the application using
 * this lib.
 *
 * Similarly, there is no error returned since the listpack normally can be
 * assumed to be valid, so that would be a very high API cost. */
static inline unsigned char *
lpGetWithSize(unsigned char *p, int64_t *count, unsigned char *intbuf, uint64_t *entry_size) {
    int64_t val;
    uint64_t uval, negstart, negmax;

    assert(p); /* assertion for valgrind (avoid NPD) */
    if (LP_ENCODING_IS_7BIT_UINT(p[0])) {
        negstart = UINT64_MAX; /* 7 bit ints are always positive. */
        negmax = 0;
        uval = p[0] & 0x7f;
        if (entry_size) *entry_size = LP_ENCODING_7BIT_UINT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_6BIT_STR(p[0])) {
        *count = LP_ENCODING_6BIT_STR_LEN(p);
        if (entry_size) *entry_size = 1 + *count + lpEncodeBacklen(NULL, *count + 1);
        return p + 1;
    } else if (LP_ENCODING_IS_13BIT_INT(p[0])) {
        uval = ((p[0] & 0x1f) << 8) | p[1];
        negstart = (uint64_t)1 << 12;
        negmax = 8191;
        if (entry_size) *entry_size = LP_ENCODING_13BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_16BIT_INT(p[0])) {
        uval = (uint64_t)p[1] | (uint64_t)p[2] << 8;
        negstart = (uint64_t)1 << 15;
        negmax = UINT16_MAX;
        if (entry_size) *entry_size = LP_ENCODING_16BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_24BIT_INT(p[0])) {
        uval = (uint64_t)p[1] | (uint64_t)p[2] << 8 | (uint64_t)p[3] << 16;
        negstart = (uint64_t)1 << 23;
        negmax = UINT32_MAX >> 8;
        if (entry_size) *entry_size = LP_ENCODING_24BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_32BIT_INT(p[0])) {
        uval = (uint64_t)p[1] | (uint64_t)p[2] << 8 | (uint64_t)p[3] << 16 | (uint64_t)p[4] << 24;
        negstart = (uint64_t)1 << 31;
        negmax = UINT32_MAX;
        if (entry_size) *entry_size = LP_ENCODING_32BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_64BIT_INT(p[0])) {
        uval = (uint64_t)p[1] | (uint64_t)p[2] << 8 | (uint64_t)p[3] << 16 | (uint64_t)p[4] << 24 |
               (uint64_t)p[5] << 32 | (uint64_t)p[6] << 40 | (uint64_t)p[7] << 48 | (uint64_t)p[8] << 56;
        negstart = (uint64_t)1 << 63;
        negmax = UINT64_MAX;
        if (entry_size) *entry_size = LP_ENCODING_64BIT_INT_ENTRY_SIZE;
    } else if (LP_ENCODING_IS_12BIT_STR(p[0])) {
        *count = LP_ENCODING_12BIT_STR_LEN(p);
        if (entry_size) *entry_size = 2 + *count + lpEncodeBacklen(NULL, *count + 2);
        return p + 2;
    } else if (LP_ENCODING_IS_32BIT_STR(p[0])) {
        *count = LP_ENCODING_32BIT_STR_LEN(p);
        if (entry_size) *entry_size = 5 + *count + lpEncodeBacklen(NULL, *count + 5);
        return p + 5;
    } else {
        uval = 12345678900000000ULL + p[0];
        negstart = UINT64_MAX;
        negmax = 0;
    }

    /* We reach this code path only for integer encodings.
     * Convert the unsigned value to the signed one using two's complement
     * rule. */
    if (uval >= negstart) {
        /* This three steps conversion should avoid undefined behaviors
         * in the unsigned -> signed conversion. */
        uval = negmax - uval;
        val = uval;
        val = -val - 1;
    } else {
        val = uval;
    }

    /* Return the string representation of the integer or the value itself
     * depending on intbuf being NULL or not. */
    if (intbuf) {
        *count = ll2string((char *)intbuf, LP_INTBUF_SIZE, (long long)val);
        return intbuf;
    } else {
        *count = val;
        return NULL;
    }
}

unsigned char *lpGet(unsigned char *p, int64_t *count, unsigned char *intbuf) {
    return lpGetWithSize(p, count, intbuf, NULL);
}

/* This is just a wrapper to lpGet() that is able to get entry value directly.
 * When the function returns NULL, it populates the integer value by reference in 'lval'.
 * Otherwise if the element is encoded as a string a pointer to the string (pointing
 * inside the listpack itself) is returned, and 'slen' is set to the length of the
 * string. */
unsigned char *lpGetValue(unsigned char *p, unsigned int *slen, long long *lval) {
    unsigned char *vstr;
    int64_t ele_len;

    vstr = lpGet(p, &ele_len, NULL);
    if (vstr) {
        *slen = ele_len;
    } else {
        *lval = ele_len;
    }
    return vstr;
}

/* Find pointer to the entry equal to the specified entry. Skip 'skip' entries
 * between every comparison. Returns NULL when the field could not be found. */
unsigned char *lpFind(unsigned char *lp, unsigned char *p, unsigned char *s, uint32_t slen, unsigned int skip) {
    int skipcnt = 0;
    unsigned char vencoding = 0;
    unsigned char *value;
    int64_t ll, vll;
    uint64_t entry_size = 123456789; /* initialized to avoid warning. */
    uint32_t lp_bytes = lpBytes(lp);

    assert(p);
    while (p) {
        if (skipcnt == 0) {
            value = lpGetWithSize(p, &ll, NULL, &entry_size);
            if (value) {
                /* check the value doesn't reach outside the listpack before accessing it */
                assert(p >= lp + LP_HDR_SIZE && p + entry_size < lp + lp_bytes);
                if (slen == ll && memcmp(value, s, slen) == 0) {
                    return p;
                }
            } else {
                /* Find out if the searched field can be encoded. Note that
                 * we do it only the first time, once done vencoding is set
                 * to non-zero and vll is set to the integer value. */
                if (vencoding == 0) {
                    /* If the entry can be encoded as integer we set it to
                     * 1, else set it to UCHAR_MAX, so that we don't retry
                     * again the next time. */
                    if (slen >= 32 || slen == 0 || !lpStringToInt64((const char *)s, slen, &vll)) {
                        vencoding = UCHAR_MAX;
                    } else {
                        vencoding = 1;
                    }
                }

                /* Compare current entry with specified entry, do it only
                 * if vencoding != UCHAR_MAX because if there is no encoding
                 * possible for the field it can't be a valid integer. */
                if (vencoding != UCHAR_MAX && ll == vll) {
                    return p;
                }
            }

            /* Reset skip count */
            skipcnt = skip;
            p += entry_size;
        } else {
            /* Skip entry */
            skipcnt--;

            /* Move to next entry, avoid use `lpNext` due to `lpAssertValidEntry` in
             * `lpNext` will call `lpBytes`, will cause performance degradation */
            p = lpSkip(p);
        }

        /* The next call to lpGetWithSize could read at most 8 bytes past `p`
         * We use the slower validation call only when necessary. */
        if (p + 8 < lp + lp_bytes) assert(p >= lp + LP_HDR_SIZE && p < lp + lp_bytes);
        if (p[0] == LP_EOF) break;
    }

    return NULL;
}

/* Insert, delete or replace the specified string element 'elestr' of length
 * 'size' or integer element 'eleint' at the specified position 'p', with 'p'
 * being a listpack element pointer obtained with lpFirst(), lpLast(), lpNext(),
 * lpPrev() or lpSeek().
 *
 * The element is inserted before, after, or replaces the element pointed
 * by 'p' depending on the 'where' argument, that can be LP_BEFORE, LP_AFTER
 * or LP_REPLACE.
 *
 * If both 'elestr' and `eleint` are NULL, the function removes the element
 * pointed by 'p' instead of inserting one.
 * If `eleint` is non-NULL, 'size' is the length of 'eleint', the function insert
 * or replace with a 64 bit integer, which is stored in the 'eleint' buffer.
 * If 'elestr` is non-NULL, 'size' is the length of 'elestr', the function insert
 * or replace with a string, which is stored in the 'elestr' buffer.
 *
 * Returns NULL on out of memory or when the listpack total length would exceed
 * the max allowed size of 2^32-1, otherwise the new pointer to the listpack
 * holding the new element is returned (and the old pointer passed is no longer
 * considered valid)
 *
 * If 'newp' is not NULL, at the end of a successful call '*newp' will be set
 * to the address of the element just added, so that it will be possible to
 * continue an interaction with lpNext() and lpPrev().
 *
 * For deletion operations (both 'elestr' and 'eleint' set to NULL) 'newp' is
 * set to the next element, on the right of the deleted one, or to NULL if the
 * deleted element was the last one. */
unsigned char *lpInsert(unsigned char *lp,
                        unsigned char *elestr,
                        unsigned char *eleint,
                        uint32_t size,
                        unsigned char *p,
                        int where,
                        unsigned char **newp) {
    unsigned char intenc[LP_MAX_INT_ENCODING_LEN];
    unsigned char backlen[LP_MAX_BACKLEN_SIZE];

    uint64_t enclen; /* The length of the encoded element. */
    int del_ele = (elestr == NULL && eleint == NULL);

    /* when deletion, it is conceptually replacing the element with a
     * zero-length element. So whatever we get passed as 'where', set
     * it to LP_REPLACE. */
    if (del_ele) where = LP_REPLACE;

    /* If we need to insert after the current element, we just jump to the
     * next element (that could be the EOF one) and handle the case of
     * inserting before. So the function will actually deal with just two
     * cases: LP_BEFORE and LP_REPLACE. */
    if (where == LP_AFTER) {
        p = lpSkip(p);
        where = LP_BEFORE;
        ASSERT_INTEGRITY(lp, p);
    }

    /* Store the offset of the element 'p', so that we can obtain its
     * address again after a reallocation. */
    unsigned long poff = p - lp;

    int enctype;
    if (elestr) {
        /* Calling lpEncodeGetType() results into the encoded version of the
         * element to be stored into 'intenc' in case it is representable as
         * an integer: in that case, the function returns LP_ENCODING_INT.
         * Otherwise if LP_ENCODING_STR is returned, we'll have to call
         * lpEncodeString() to actually write the encoded string on place later.
         *
         * Whatever the returned encoding is, 'enclen' is populated with the
         * length of the encoded element. */
        enctype = lpEncodeGetType(elestr, size, intenc, &enclen);
        if (enctype == LP_ENCODING_INT) eleint = intenc;
    } else if (eleint) {
        enctype = LP_ENCODING_INT;
        enclen = size; /* 'size' is the length of the encoded integer element. */
    } else {
        enctype = -1;
        enclen = 0;
    }

    /* We need to also encode the backward-parsable length of the element
     * and append it to the end: this allows to traverse the listpack from
     * the end to the start. */
    unsigned long backlen_size = (!del_ele) ? lpEncodeBacklen(backlen, enclen) : 0;
    uint64_t old_listpack_bytes = lpGetTotalBytes(lp);
    uint32_t replaced_len = 0;
    if (where == LP_REPLACE) {
        replaced_len = lpCurrentEncodedSizeUnsafe(p);
        replaced_len += lpEncodeBacklen(NULL, replaced_len);
        ASSERT_INTEGRITY_LEN(lp, p, replaced_len);
    }

    uint64_t new_listpack_bytes = old_listpack_bytes + enclen + backlen_size - replaced_len;
    if (new_listpack_bytes > UINT32_MAX) return NULL;

    /* We now need to reallocate in order to make space or shrink the
     * allocation (in case 'when' value is LP_REPLACE and the new element is
     * smaller). However we do that before memmoving the memory to
     * make room for the new element if the final allocation will get
     * larger, or we do it after if the final allocation will get smaller. */

    unsigned char *dst = lp + poff; /* May be updated after reallocation. */

    /* Realloc before: we need more room. */
    if (new_listpack_bytes > old_listpack_bytes && new_listpack_bytes > lp_malloc_size(lp)) {
        if ((lp = lp_realloc(lp, new_listpack_bytes)) == NULL) return NULL;
        dst = lp + poff;
    }

    /* Setup the listpack relocating the elements to make the exact room
     * we need to store the new one. */
    if (where == LP_BEFORE) {
        memmove(dst + enclen + backlen_size, dst, old_listpack_bytes - poff);
    } else { /* LP_REPLACE. */
        memmove(dst + enclen + backlen_size, dst + replaced_len, old_listpack_bytes - poff - replaced_len);
    }

    /* Realloc after: we need to free space. */
    if (new_listpack_bytes < old_listpack_bytes) {
        if ((lp = lp_realloc(lp, new_listpack_bytes)) == NULL) return NULL;
        dst = lp + poff;
    }

    /* Store the entry. */
    if (newp) {
        *newp = dst;
        /* In case of deletion, set 'newp' to NULL if the next element is
         * the EOF element. */
        if (del_ele && dst[0] == LP_EOF) *newp = NULL;
    }
    if (!del_ele) {
        if (enctype == LP_ENCODING_INT) {
            memcpy(dst, eleint, enclen);
        } else if (elestr) {
            lpEncodeString(dst, elestr, size);
        } else {
            valkey_unreachable();
        }
        dst += enclen;
        memcpy(dst, backlen, backlen_size);
        dst += backlen_size;
    }

    /* Update header. */
    if (where != LP_REPLACE || del_ele) {
        uint32_t num_elements = lpGetNumElements(lp);
        if (num_elements != LP_HDR_NUMELE_UNKNOWN) {
            if (!del_ele)
                lpSetNumElements(lp, num_elements + 1);
            else
                lpSetNumElements(lp, num_elements - 1);
        }
    }
    lpSetTotalBytes(lp, new_listpack_bytes);

#if 0
    /* This code path is normally disabled: what it does is to force listpack
     * to return *always* a new pointer after performing some modification to
     * the listpack, even if the previous allocation was enough. This is useful
     * in order to spot bugs in code using listpacks: by doing so we can find
     * if the caller forgets to set the new pointer where the listpack reference
     * is stored, after an update. */
    unsigned char *oldlp = lp;
    lp = lp_malloc(new_listpack_bytes);
    memcpy(lp,oldlp,new_listpack_bytes);
    if (newp) {
        unsigned long offset = (*newp)-oldlp;
        *newp = lp + offset;
    }
    /* Make sure the old allocation contains garbage. */
    memset(oldlp,'A',new_listpack_bytes);
    lp_free(oldlp);
#endif

    return lp;
}

/* This is just a wrapper for lpInsert() to directly use a string. */
unsigned char *
lpInsertString(unsigned char *lp, unsigned char *s, uint32_t slen, unsigned char *p, int where, unsigned char **newp) {
    return lpInsert(lp, s, NULL, slen, p, where, newp);
}

/* This is just a wrapper for lpInsert() to directly use a 64 bit integer
 * instead of a string. */
unsigned char *lpInsertInteger(unsigned char *lp, long long lval, unsigned char *p, int where, unsigned char **newp) {
    uint64_t enclen; /* The length of the encoded element. */
    unsigned char intenc[LP_MAX_INT_ENCODING_LEN];

    lpEncodeIntegerGetType(lval, intenc, &enclen);
    return lpInsert(lp, NULL, intenc, enclen, p, where, newp);
}

/* Append the specified element 's' of length 'slen' at the head of the listpack. */
unsigned char *lpPrepend(unsigned char *lp, unsigned char *s, uint32_t slen) {
    unsigned char *p = lpFirst(lp);
    if (!p) return lpAppend(lp, s, slen);
    return lpInsert(lp, s, NULL, slen, p, LP_BEFORE, NULL);
}

/* Append the specified integer element 'lval' at the head of the listpack. */
unsigned char *lpPrependInteger(unsigned char *lp, long long lval) {
    unsigned char *p = lpFirst(lp);
    if (!p) return lpAppendInteger(lp, lval);
    return lpInsertInteger(lp, lval, p, LP_BEFORE, NULL);
}

/* Append the specified element 'ele' of length 'size' at the end of the
 * listpack. It is implemented in terms of lpInsert(), so the return value is
 * the same as lpInsert(). */
unsigned char *lpAppend(unsigned char *lp, unsigned char *ele, uint32_t size) {
    uint64_t listpack_bytes = lpGetTotalBytes(lp);
    unsigned char *eofptr = lp + listpack_bytes - 1;
    return lpInsert(lp, ele, NULL, size, eofptr, LP_BEFORE, NULL);
}

/* Append the specified integer element 'lval' at the end of the listpack. */
unsigned char *lpAppendInteger(unsigned char *lp, long long lval) {
    uint64_t listpack_bytes = lpGetTotalBytes(lp);
    unsigned char *eofptr = lp + listpack_bytes - 1;
    return lpInsertInteger(lp, lval, eofptr, LP_BEFORE, NULL);
}

/* This is just a wrapper for lpInsert() to directly use a string to replace
 * the current element. The function returns the new listpack as return
 * value, and also updates the current cursor by updating '*p'. */
unsigned char *lpReplace(unsigned char *lp, unsigned char **p, unsigned char *s, uint32_t slen) {
    return lpInsert(lp, s, NULL, slen, *p, LP_REPLACE, p);
}

/* This is just a wrapper for lpInsertInteger() to directly use a 64 bit integer
 * instead of a string to replace the current element. The function returns
 * the new listpack as return value, and also updates the current cursor
 * by updating '*p'. */
unsigned char *lpReplaceInteger(unsigned char *lp, unsigned char **p, long long lval) {
    return lpInsertInteger(lp, lval, *p, LP_REPLACE, p);
}

/* Remove the element pointed by 'p', and return the resulting listpack.
 * If 'newp' is not NULL, the next element pointer (to the right of the
 * deleted one) is returned by reference. If the deleted element was the
 * last one, '*newp' is set to NULL. */
unsigned char *lpDelete(unsigned char *lp, unsigned char *p, unsigned char **newp) {
    return lpInsert(lp, NULL, NULL, 0, p, LP_REPLACE, newp);
}

/* Delete a range of entries from the listpack start with the element pointed by 'p'. */
unsigned char *lpDeleteRangeWithEntry(unsigned char *lp, unsigned char **p, unsigned long num) {
    size_t bytes = lpBytes(lp);
    unsigned long deleted = 0;
    unsigned char *eofptr = lp + bytes - 1;
    unsigned char *first, *tail;
    first = tail = *p;

    if (num == 0) return lp; /* Nothing to delete, return ASAP. */

    /* Find the next entry to the last entry that needs to be deleted.
     * lpLength may be unreliable due to corrupt data, so we cannot
     * treat 'num' as the number of elements to be deleted. */
    while (num--) {
        deleted++;
        tail = lpSkip(tail);
        if (tail[0] == LP_EOF) break;
    }

    /* Store the offset of the element 'first', so that we can obtain its
     * address again after a reallocation. */
    unsigned long poff = first - lp;

    /* Move tail to the front of the listpack */
    memmove(first, tail, eofptr - tail + 1);
    lpSetTotalBytes(lp, bytes - (tail - first));
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN) lpSetNumElements(lp, numele - deleted);
    lp = lpShrinkToFit(lp);

    /* Store the entry. */
    *p = lp + poff;
    if ((*p)[0] == LP_EOF) *p = NULL;

    return lp;
}

/* Delete a range of entries from the listpack. */
unsigned char *lpDeleteRange(unsigned char *lp, long index, unsigned long num) {
    unsigned char *p;
    uint32_t numele = lpGetNumElements(lp);

    if (num == 0) return lp; /* Nothing to delete, return ASAP. */
    if ((p = lpSeek(lp, index)) == NULL) return lp;

    /* If we know we're gonna delete beyond the end of the listpack, we can just move
     * the EOF marker, and there's no need to iterate through the entries,
     * but if we can't be sure how many entries there are, we rather avoid calling lpLength
     * since that means an additional iteration on all elements.
     *
     * Note that index could overflow, but we use the value after seek, so when we
     * use it no overflow happens. */
    if (numele != LP_HDR_NUMELE_UNKNOWN && index < 0) index = (long)numele + index;
    if (numele != LP_HDR_NUMELE_UNKNOWN && (numele - (unsigned long)index) <= num) {
        p[0] = LP_EOF;
        lpSetTotalBytes(lp, p - lp + 1);
        lpSetNumElements(lp, index);
        lp = lpShrinkToFit(lp);
    } else {
        lp = lpDeleteRangeWithEntry(lp, &p, num);
    }

    return lp;
}

/* Delete the elements 'ps' passed as an array of 'count' element pointers and
 * return the resulting listpack. The elements must be given in the same order
 * as they apper in the listpack. */
unsigned char *lpBatchDelete(unsigned char *lp, unsigned char **ps, unsigned long count) {
    if (count == 0) return lp;
    unsigned char *dst = ps[0];
    size_t total_bytes = lpGetTotalBytes(lp);
    unsigned char *lp_end = lp + total_bytes; /* After the EOF element. */
    assert(lp_end[-1] == LP_EOF);
    /*
     * ----+--------+-----------+--------+---------+-----+---+
     * ... | Delete | Keep      | Delete | Keep    | ... |EOF|
     * ... |xxxxxxxx|           |xxxxxxxx|         | ... |   |
     * ----+--------+-----------+--------+---------+-----+---+
     *     ^        ^           ^                            ^
     *     |        |           |                            |
     *     ps[i]    |           ps[i+1]                      |
     *     skip     keep_start  keep_end                     lp_end
     *
     * The loop memmoves the bytes between keep_start and keep_end to dst.
     */
    for (unsigned long i = 0; i < count; i++) {
        unsigned char *skip = ps[i];
        assert(skip != NULL && skip[0] != LP_EOF);
        unsigned char *keep_start = lpSkip(skip);
        unsigned char *keep_end;
        if (i + 1 < count) {
            keep_end = ps[i + 1];
            /* Deleting consecutive elements. Nothing to keep between them. */
            if (keep_start == keep_end) continue;
        } else {
            /* Keep the rest of the listpack including the EOF marker. */
            keep_end = lp_end;
        }
        assert(keep_end > keep_start);
        size_t bytes_to_keep = keep_end - keep_start;
        memmove(dst, keep_start, bytes_to_keep);
        dst += bytes_to_keep;
    }
    /* Update total size and num elements. */
    size_t deleted_bytes = lp_end - dst;
    total_bytes -= deleted_bytes;
    assert(lp[total_bytes - 1] == LP_EOF);
    lpSetTotalBytes(lp, total_bytes);
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN) lpSetNumElements(lp, numele - count);
    return lpShrinkToFit(lp);
}

/* Merge listpacks 'first' and 'second' by appending 'second' to 'first'.
 *
 * NOTE: The larger listpack is reallocated to contain the new merged listpack.
 * Either 'first' or 'second' can be used for the result.  The parameter not
 * used will be free'd and set to NULL.
 *
 * After calling this function, the input parameters are no longer valid since
 * they are changed and free'd in-place.
 *
 * The result listpack is the contents of 'first' followed by 'second'.
 *
 * On failure: returns NULL if the merge is impossible.
 * On success: returns the merged listpack (which is expanded version of either
 * 'first' or 'second', also frees the other unused input listpack, and sets the
 * input listpack argument equal to newly reallocated listpack return value. */
unsigned char *lpMerge(unsigned char **first, unsigned char **second) {
    /* If any params are null, we can't merge, so NULL. */
    if (first == NULL || *first == NULL || second == NULL || *second == NULL) return NULL;

    /* Can't merge same list into itself. */
    if (*first == *second) return NULL;

    size_t first_bytes = lpBytes(*first);
    unsigned long first_len = lpLength(*first);

    size_t second_bytes = lpBytes(*second);
    unsigned long second_len = lpLength(*second);

    int append;
    unsigned char *source, *target;
    size_t target_bytes, source_bytes;
    /* Pick the largest listpack so we can resize easily in-place.
     * We must also track if we are now appending or prepending to
     * the target listpack. */
    if (first_bytes >= second_bytes) {
        /* retain first, append second to first. */
        target = *first;
        target_bytes = first_bytes;
        source = *second;
        source_bytes = second_bytes;
        append = 1;
    } else {
        /* else, retain second, prepend first to second. */
        target = *second;
        target_bytes = second_bytes;
        source = *first;
        source_bytes = first_bytes;
        append = 0;
    }

    /* Calculate final bytes (subtract one pair of metadata) */
    unsigned long long lpbytes = (unsigned long long)first_bytes + second_bytes - LP_HDR_SIZE - 1;
    assert(lpbytes < UINT32_MAX); /* larger values can't be stored */
    unsigned long lplength = first_len + second_len;

    /* Combined lp length should be limited within UINT16_MAX */
    lplength = lplength < UINT16_MAX ? lplength : UINT16_MAX;

    /* Extend target to new lpbytes then append or prepend source. */
    target = lp_realloc(target, lpbytes);
    if (append) {
        /* append == appending to target */
        /* Copy source after target (copying over original [END]):
         *   [TARGET - END, SOURCE - HEADER] */
        memcpy(target + target_bytes - 1, source + LP_HDR_SIZE, source_bytes - LP_HDR_SIZE);
    } else {
        /* !append == prepending to target */
        /* Move target *contents* exactly size of (source - [END]),
         * then copy source into vacated space (source - [END]):
         *   [SOURCE - END, TARGET - HEADER] */
        memmove(target + source_bytes - 1, target + LP_HDR_SIZE, target_bytes - LP_HDR_SIZE);
        memcpy(target, source, source_bytes - 1);
    }

    lpSetNumElements(target, lplength);
    lpSetTotalBytes(target, lpbytes);

    /* Now free and NULL out what we didn't realloc */
    if (append) {
        lp_free(*second);
        *second = NULL;
        *first = target;
    } else {
        lp_free(*first);
        *first = NULL;
        *second = target;
    }

    return target;
}

unsigned char *lpDup(unsigned char *lp) {
    size_t lpbytes = lpBytes(lp);
    unsigned char *newlp = lp_malloc(lpbytes);
    memcpy(newlp, lp, lpbytes);
    return newlp;
}

/* Return the total number of bytes the listpack is composed of. */
size_t lpBytes(unsigned char *lp) {
    return lpGetTotalBytes(lp);
}

/* Returns the size of a listpack consisting of an integer repeated 'rep' times. */
size_t lpEstimateBytesRepeatedInteger(long long lval, unsigned long rep) {
    uint64_t enclen;
    unsigned char intenc[LP_MAX_INT_ENCODING_LEN];
    lpEncodeIntegerGetType(lval, intenc, &enclen);
    unsigned long backlen = lpEncodeBacklen(NULL, enclen);
    return LP_HDR_SIZE + (enclen + backlen) * rep + 1;
}

/* Seek the specified element and returns the pointer to the seeked element.
 * Positive indexes specify the zero-based element to seek from the head to
 * the tail, negative indexes specify elements starting from the tail, where
 * -1 means the last element, -2 the penultimate and so forth. If the index
 * is out of range, NULL is returned. */
unsigned char *lpSeek(unsigned char *lp, long index) {
    int forward = 1; /* Seek forward by default. */

    /* We want to seek from left to right or the other way around
     * depending on the listpack length and the element position.
     * However if the listpack length cannot be obtained in constant time,
     * we always seek from left to right. */
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN) {
        if (index < 0) index = (long)numele + index;
        if (index < 0) return NULL;             /* Index still < 0 means out of range. */
        if (index >= (long)numele) return NULL; /* Out of range the other side. */
        /* We want to scan right-to-left if the element we are looking for
         * is past the half of the listpack. */
        if (index > (long)numele / 2) {
            forward = 0;
            /* Right to left scanning always expects a negative index. Convert
             * our index to negative form. */
            index -= numele;
        }
    } else {
        /* If the listpack length is unspecified, for negative indexes we
         * want to always scan right-to-left. */
        if (index < 0) forward = 0;
    }

    /* Forward and backward scanning is trivially based on lpNext()/lpPrev(). */
    if (forward) {
        unsigned char *ele = lpFirst(lp);
        while (index > 0 && ele) {
            ele = lpNext(lp, ele);
            index--;
        }
        return ele;
    } else {
        unsigned char *ele = lpLast(lp);
        while (index < -1 && ele) {
            ele = lpPrev(lp, ele);
            index++;
        }
        return ele;
    }
}

/* Same as lpFirst but without validation assert, to be used right before lpValidateNext. */
unsigned char *lpValidateFirst(unsigned char *lp) {
    unsigned char *p = lp + LP_HDR_SIZE; /* Skip the header. */
    if (p[0] == LP_EOF) return NULL;
    return p;
}

/* Validate the integrity of a single listpack entry and move to the next one.
 * The input argument 'pp' is a reference to the current record and is advanced on exit.
 * Returns 1 if valid, 0 if invalid. */
int lpValidateNext(unsigned char *lp, unsigned char **pp, size_t lpbytes) {
#define OUT_OF_RANGE(p) ((p) < lp + LP_HDR_SIZE || (p) > lp + lpbytes - 1)
    unsigned char *p = *pp;
    if (!p) return 0;

    /* Before accessing p, make sure it's valid. */
    if (OUT_OF_RANGE(p)) return 0;

    if (*p == LP_EOF) {
        *pp = NULL;
        return 1;
    }

    /* check that we can read the encoded size */
    uint32_t lenbytes = lpCurrentEncodedSizeBytes(p);
    if (!lenbytes) return 0;

    /* make sure the encoded entry length doesn't reach outside the edge of the listpack */
    if (OUT_OF_RANGE(p + lenbytes)) return 0;

    /* get the entry length and encoded backlen. */
    unsigned long entrylen = lpCurrentEncodedSizeUnsafe(p);
    unsigned long encodedBacklen = lpEncodeBacklen(NULL, entrylen);
    entrylen += encodedBacklen;

    /* make sure the entry doesn't reach outside the edge of the listpack */
    if (OUT_OF_RANGE(p + entrylen)) return 0;

    /* move to the next entry */
    p += entrylen;

    /* make sure the encoded length at the end patches the one at the beginning. */
    uint64_t prevlen = lpDecodeBacklen(p - 1);
    if (prevlen + encodedBacklen != entrylen) return 0;

    *pp = p;
    return 1;
#undef OUT_OF_RANGE
}

/* Validate the integrity of the data structure.
 * when `deep` is 0, only the integrity of the header is validated.
 * when `deep` is 1, we scan all the entries one by one. */
int lpValidateIntegrity(unsigned char *lp, size_t size, int deep, listpackValidateEntryCB entry_cb, void *cb_userdata) {
    /* Check that we can actually read the header. (and EOF) */
    if (size < LP_HDR_SIZE + 1) return 0;

    /* Check that the encoded size in the header must match the allocated size. */
    size_t bytes = lpGetTotalBytes(lp);
    if (bytes != size) return 0;

    /* The last byte must be the terminator. */
    if (lp[size - 1] != LP_EOF) return 0;

    if (!deep) return 1;

    /* Validate the individual entries. */
    uint32_t count = 0;
    uint32_t numele = lpGetNumElements(lp);
    unsigned char *p = lp + LP_HDR_SIZE;
    while (p && p[0] != LP_EOF) {
        unsigned char *prev = p;

        /* Validate this entry and move to the next entry in advance
         * to avoid callback crash due to corrupt listpack. */
        if (!lpValidateNext(lp, &p, bytes)) return 0;

        /* Optionally let the caller validate the entry too. */
        if (entry_cb && !entry_cb(prev, numele, cb_userdata)) return 0;

        count++;
    }

    /* Make sure 'p' really does point to the end of the listpack. */
    if (p != lp + size - 1) return 0;

    /* Check that the count in the header is correct */
    if (numele != LP_HDR_NUMELE_UNKNOWN && numele != count) return 0;

    return 1;
}

/* Compare entry pointer to by 'p' with string 's' of length 'slen'.
 * Return 1 if equal. */
unsigned int lpCompare(unsigned char *p, unsigned char *s, uint32_t slen) {
    unsigned char *value;
    int64_t sz;
    if (p[0] == LP_EOF) return 0;

    value = lpGet(p, &sz, NULL);
    if (value) {
        return (slen == sz) && memcmp(value, s, slen) == 0;
    } else {
        /* We use lpStringToInt64() to get an integer representation of the
         * string 's' and compare it to 'sval', it's much faster than convert
         * integer to string and comparing. */
        int64_t sval;
        if (lpStringToInt64((const char *)s, slen, &sval)) return sz == sval;
    }

    return 0;
}

/* uint compare for qsort */
static int uintCompare(const void *a, const void *b) {
    return (*(unsigned int *)a - *(unsigned int *)b);
}

/* Helper method to store a string into from val or lval into dest */
static inline void lpSaveValue(unsigned char *val, unsigned int len, int64_t lval, listpackEntry *dest) {
    dest->sval = val;
    dest->slen = len;
    dest->lval = lval;
}

/* Randomly select a pair of key and value.
 * total_count is a pre-computed length/2 of the listpack (to avoid calls to lpLength)
 * 'key' and 'val' are used to store the result key value pair.
 * 'val' can be NULL if the value is not needed. */
void lpRandomPair(unsigned char *lp, unsigned long total_count, listpackEntry *key, listpackEntry *val) {
    unsigned char *p;

    /* Avoid div by zero on corrupt listpack */
    assert(total_count);

    /* Generate even numbers, because listpack saved K-V pair */
    int r = (rand() % total_count) * 2;
    assert((p = lpSeek(lp, r)));
    key->sval = lpGetValue(p, &(key->slen), &(key->lval));

    if (!val) return;
    assert((p = lpNext(lp, p)));
    val->sval = lpGetValue(p, &(val->slen), &(val->lval));
}

/* Randomly select 'count' entries and store them in the 'entries' array, which
 * needs to have space for 'count' listpackEntry structs. The order is random
 * and duplicates are possible. */
void lpRandomEntries(unsigned char *lp, unsigned int count, listpackEntry *entries) {
    struct pick {
        unsigned int index;
        unsigned int order;
    } *picks = lp_malloc(count * sizeof(struct pick));
    unsigned int total_size = lpLength(lp);
    assert(total_size);
    for (unsigned int i = 0; i < count; i++) {
        picks[i].index = rand() % total_size;
        picks[i].order = i;
    }

    /* Sort by index. */
    qsort(picks, count, sizeof(struct pick), uintCompare);

    /* Iterate over listpack in index order and store the values in the entries
     * array respecting the original order. */
    unsigned char *p = lpFirst(lp);
    unsigned int j = 0; /* index in listpack */
    for (unsigned int i = 0; i < count; i++) {
        /* Advance listpack pointer to until we reach 'index' listpack. */
        while (j < picks[i].index) {
            p = lpNext(lp, p);
            j++;
        }
        int storeorder = picks[i].order;
        unsigned int len = 0;
        long long llval = 0;
        unsigned char *str = lpGetValue(p, &len, &llval);
        lpSaveValue(str, len, llval, &entries[storeorder]);
    }
    lp_free(picks);
}

/* Randomly select count of key value pairs and store into 'keys' and
 * 'vals' args. The order of the picked entries is random, and the selections
 * are non-unique (repetitions are possible).
 * The 'vals' arg can be NULL in which case we skip these. */
void lpRandomPairs(unsigned char *lp, unsigned int count, listpackEntry *keys, listpackEntry *vals) {
    unsigned char *p, *key, *value;
    unsigned int klen = 0, vlen = 0;
    long long klval = 0, vlval = 0;

    /* Notice: the index member must be first due to the use in uintCompare */
    typedef struct {
        unsigned int index;
        unsigned int order;
    } rand_pick;
    rand_pick *picks = lp_malloc(sizeof(rand_pick) * count);
    unsigned int total_size = lpLength(lp) / 2;

    /* Avoid div by zero on corrupt listpack */
    assert(total_size);

    /* create a pool of random indexes (some may be duplicate). */
    for (unsigned int i = 0; i < count; i++) {
        picks[i].index = (rand() % total_size) * 2; /* Generate even indexes */
        /* keep track of the order we picked them */
        picks[i].order = i;
    }

    /* sort by indexes. */
    qsort(picks, count, sizeof(rand_pick), uintCompare);

    /* fetch the elements form the listpack into a output array respecting the original order. */
    unsigned int lpindex = picks[0].index, pickindex = 0;
    p = lpSeek(lp, lpindex);
    while (p && pickindex < count) {
        key = lpGetValue(p, &klen, &klval);
        assert((p = lpNext(lp, p)));
        value = lpGetValue(p, &vlen, &vlval);
        while (pickindex < count && lpindex == picks[pickindex].index) {
            int storeorder = picks[pickindex].order;
            lpSaveValue(key, klen, klval, &keys[storeorder]);
            if (vals) lpSaveValue(value, vlen, vlval, &vals[storeorder]);
            pickindex++;
        }
        lpindex += 2;
        p = lpNext(lp, p);
    }

    lp_free(picks);
}

/* Randomly select count of key value pairs and store into 'keys' and
 * 'vals' args. The selections are unique (no repetitions), and the order of
 * the picked entries is NOT-random.
 * The 'vals' arg can be NULL in which case we skip these.
 * The return value is the number of items picked which can be lower than the
 * requested count if the listpack doesn't hold enough pairs. */
unsigned int lpRandomPairsUnique(unsigned char *lp, unsigned int count, listpackEntry *keys, listpackEntry *vals) {
    unsigned char *p, *key;
    unsigned int klen = 0;
    long long klval = 0;
    unsigned int total_size = lpLength(lp) / 2;
    unsigned int index = 0;
    if (count > total_size) count = total_size;

    p = lpFirst(lp);
    unsigned int picked = 0, remaining = count;
    while (picked < count && p) {
        assert((p = lpNextRandom(lp, p, &index, remaining, 1)));
        key = lpGetValue(p, &klen, &klval);
        lpSaveValue(key, klen, klval, &keys[picked]);
        assert((p = lpNext(lp, p)));
        index++;
        if (vals) {
            key = lpGetValue(p, &klen, &klval);
            lpSaveValue(key, klen, klval, &vals[picked]);
        }
        p = lpNext(lp, p);
        remaining--;
        picked++;
        index++;
    }
    return picked;
}

/* Iterates forward to the "next random" element, given we are yet to pick
 * 'remaining' unique elements between the starting element 'p' (inclusive) and
 * the end of the list. The 'index' needs to be initialized according to the
 * current zero-based index matching the position of the starting element 'p'
 * and is updated to match the returned element's zero-based index. If
 * 'even_only' is nonzero, an element with an even index is picked, which is
 * useful if the listpack represents a key-value pair sequence.
 *
 * Note that this function can return p. In order to skip the previously
 * returned element, you need to call lpNext() or lpDelete() after each call to
 * lpNextRandom(). Idea:
 *
 *     assert(remaining <= lpLength(lp));
 *     p = lpFirst(lp);
 *     i = 0;
 *     while (remaining > 0) {
 *         p = lpNextRandom(lp, p, &i, remaining--, 0);
 *
 *         // ... Do stuff with p ...
 *
 *         p = lpNext(lp, p);
 *         i++;
 *     }
 */
unsigned char *
lpNextRandom(unsigned char *lp, unsigned char *p, unsigned int *index, unsigned int remaining, int even_only) {
    /* To only iterate once, every time we try to pick a member, the probability
     * we pick it is the quotient of the count left we want to pick and the
     * count still we haven't visited. This way, we could make every member be
     * equally likely to be picked. */
    unsigned int i = *index;
    unsigned int total_size = lpLength(lp);
    while (i < total_size && p != NULL) {
        if (even_only && i % 2 != 0) {
            p = lpNext(lp, p);
            i++;
            continue;
        }

        /* Do we pick this element? */
        unsigned int available = total_size - i;
        if (even_only) available /= 2;
        double randomDouble = ((double)rand()) / RAND_MAX;
        double threshold = ((double)remaining) / available;
        if (randomDouble <= threshold) {
            *index = i;
            return p;
        }

        p = lpNext(lp, p);
        i++;
    }

    return NULL;
}

/* Print info of listpack which is used in debugCommand */
void lpRepr(unsigned char *lp) {
    unsigned char *p, *vstr;
    int64_t vlen;
    unsigned char intbuf[LP_INTBUF_SIZE];
    int index = 0;

    printf("{total bytes %zu} {num entries %lu}\n", lpBytes(lp), lpLength(lp));

    p = lpFirst(lp);
    while (p) {
        uint32_t encoded_size_bytes = lpCurrentEncodedSizeBytes(p);
        uint32_t encoded_size = lpCurrentEncodedSizeUnsafe(p);
        unsigned long back_len = lpEncodeBacklen(NULL, encoded_size);
        printf("{\n"
               "\taddr: 0x%08lx,\n"
               "\tindex: %2d,\n"
               "\toffset: %1lu,\n"
               "\thdr+entrylen+backlen: %2lu,\n"
               "\thdrlen: %3u,\n"
               "\tbacklen: %2lu,\n"
               "\tpayload: %1u\n",
               (long unsigned)p, index, (unsigned long)(p - lp), encoded_size + back_len, encoded_size_bytes, back_len,
               encoded_size - encoded_size_bytes);
        printf("\tbytes: ");
        for (unsigned int i = 0; i < (encoded_size + back_len); i++) {
            printf("%02x|", p[i]);
        }
        printf("\n");

        vstr = lpGet(p, &vlen, intbuf);
        printf("\t[str]");
        if (vlen > 40) {
            if (fwrite(vstr, 40, 1, stdout) == 0) perror("fwrite");
            printf("...");
        } else {
            if (fwrite(vstr, vlen, 1, stdout) == 0) perror("fwrite");
        }
        printf("\n}\n");
        index++;
        p = lpNext(lp, p);
    }
    printf("{end}\n\n");
}
