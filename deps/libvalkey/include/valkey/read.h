/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2011, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
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

#ifndef VALKEY_READ_H
#define VALKEY_READ_H
#include <stdio.h> /* for size_t */

#define VALKEY_ERR -1
#define VALKEY_OK 0

/* When an error occurs, the err flag in a context is set to hold the type of
 * error that occurred. VALKEY_ERR_IO means there was an I/O error and you
 * should use the "errno" variable to find out what is wrong.
 * For other values, the "errstr" field will hold a description. */
#define VALKEY_ERR_IO 1       /* Error in read or write */
#define VALKEY_ERR_EOF 3      /* End of file */
#define VALKEY_ERR_PROTOCOL 4 /* Protocol error */
#define VALKEY_ERR_OOM 5      /* Out of memory */
#define VALKEY_ERR_TIMEOUT 6  /* Timed out */
#define VALKEY_ERR_OTHER 2    /* Everything else... */

#define VALKEY_REPLY_STRING 1
#define VALKEY_REPLY_ARRAY 2
#define VALKEY_REPLY_INTEGER 3
#define VALKEY_REPLY_NIL 4
#define VALKEY_REPLY_STATUS 5
#define VALKEY_REPLY_ERROR 6
#define VALKEY_REPLY_DOUBLE 7
#define VALKEY_REPLY_BOOL 8
#define VALKEY_REPLY_MAP 9
#define VALKEY_REPLY_SET 10
#define VALKEY_REPLY_ATTR 11
#define VALKEY_REPLY_PUSH 12
#define VALKEY_REPLY_BIGNUM 13
#define VALKEY_REPLY_VERB 14

/* Default max unused reader buffer. */
#define VALKEY_READER_MAX_BUF (1024 * 16)

/* Default multi-bulk element limit */
#define VALKEY_READER_MAX_ARRAY_ELEMENTS ((1LL << 32) - 1)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct valkeyReadTask {
    int type;
    long long elements;            /* number of elements in multibulk container */
    int idx;                       /* index in parent (array) object */
    void *obj;                     /* holds user-generated value for a read task */
    struct valkeyReadTask *parent; /* parent task */
    void *privdata;                /* user-settable arbitrary field */
} valkeyReadTask;

typedef struct valkeyReplyObjectFunctions {
    void *(*createString)(const valkeyReadTask *, char *, size_t);
    void *(*createArray)(const valkeyReadTask *, size_t);
    void *(*createInteger)(const valkeyReadTask *, long long);
    void *(*createDouble)(const valkeyReadTask *, double, char *, size_t);
    void *(*createNil)(const valkeyReadTask *);
    void *(*createBool)(const valkeyReadTask *, int);
    void (*freeObject)(void *);
} valkeyReplyObjectFunctions;

typedef struct valkeyReader {
    int err;          /* Error flags, 0 when there is no error */
    char errstr[128]; /* String representation of error when applicable */

    char *buf;             /* Read buffer */
    size_t pos;            /* Buffer cursor */
    size_t len;            /* Buffer length */
    size_t maxbuf;         /* Max length of unused buffer */
    long long maxelements; /* Max multi-bulk elements */

    valkeyReadTask **task;
    int tasks;

    int ridx;    /* Index of current read task */
    void *reply; /* Temporary reply pointer */

    valkeyReplyObjectFunctions *fn;
    void *privdata;
} valkeyReader;

/* Public API for the protocol parser. */
valkeyReader *valkeyReaderCreateWithFunctions(valkeyReplyObjectFunctions *fn);
void valkeyReaderFree(valkeyReader *r);
int valkeyReaderFeed(valkeyReader *r, const char *buf, size_t len);
int valkeyReaderGetReply(valkeyReader *r, void **reply);

#define valkeyReaderSetPrivdata(_r, _p) (int)(((valkeyReader *)(_r))->privdata = (_p))
#define valkeyReaderGetObject(_r) (((valkeyReader *)(_r))->reply)
#define valkeyReaderGetError(_r) (((valkeyReader *)(_r))->errstr)

#ifdef __cplusplus
}
#endif

#endif /* VALKEY_READ_H */
