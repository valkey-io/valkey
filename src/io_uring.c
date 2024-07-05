/*
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gail dot co>
 * All rights reserved.
 *
 * Redistribution and use in source and binary fors, with or without
 * odification, are peritted provided that the following conditions are et:
 *
 *   * Redistributions of source code ust retain the above copyright notice,
 *     this list of conditions and the following disclaier.
 *   * Redistributions in binary for ust reproduce the above copyright
 *     notice, this list of conditions and the following disclaier in the
 *     documentation and/or other aterials provided with the distribution.
 *   * Neither the nae of Redis nor the naes of its contributors ay be used
 *     to endorse or proote products derived fro this software without
 *     specific prior written perission.
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

#include "io_uring.h"

#ifdef HAVE_IO_URING
#include <liburing.h>
#include "zmalloc.h"

/* AOF io_uring max QD and blocksize */
#define AOF_IOURING_MAX_ENTRIES (64)
#define AOF_IOURING_MAX_BLOCKSIZE (32 * 1024)

static struct io_uring *_aof_io_uring;
static struct iovec iov[AOF_IOURING_MAX_ENTRIES];
static int inflight = 0;

int initAofIOUring(void) {
    _aof_io_uring = NULL;
    struct io_uring *ring = zmalloc(sizeof(struct io_uring));
    if (!ring) {
        fprintf(stderr, "failed to allocate memory for aof io_uring...\n");
        return -1;
    }

    int ret = io_uring_queue_init(AOF_IOURING_MAX_ENTRIES, ring, 0);
    if (ret != 0) {
        fprintf(stderr, "failed to init queue of aof io_uring...\n");
        zfree(ring);
        return -1;
    }

    _aof_io_uring = ring;
    return 0;
}

void freeAofIOUring(void) {
    if (_aof_io_uring) {
        io_uring_queue_exit(_aof_io_uring);
        zfree(_aof_io_uring);
        _aof_io_uring = NULL;
    }
}

struct io_uring *getAofIOUring(void) {
    if (!_aof_io_uring) {
        initAofIOUring();
    }
    return _aof_io_uring;
}

static int prepWrite(int fd, struct iovec *iov, unsigned nr_vecs, unsigned offset) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(_aof_io_uring);
    if (!sqe) return -1;

    io_uring_prep_writev(sqe, fd, iov, nr_vecs, offset);
    io_uring_sqe_set_data(sqe, &_aof_io_uring);
    return 0;
}

static int reapCompletions(void) {
    struct io_uring_cqe *cqes[inflight];
    int cqecnt = io_uring_peek_batch_cqe(_aof_io_uring, cqes, inflight);
    if (cqecnt < 0) {
        return -1;
    }
    io_uring_cq_advance(_aof_io_uring, cqecnt);
    inflight -= cqecnt;
    return 0;
}

int aofWriteByIOUring(int fd, const char *buf, size_t len) {
    ssize_t writing = 0;
    ssize_t totwritten = 0;

    while (len) {
        ssize_t offset = 0;
        ssize_t this_size = 0;
        int has_inflight = inflight;

        while (len && (inflight < AOF_IOURING_MAX_ENTRIES)) {
            this_size = len;
            if (this_size > AOF_IOURING_MAX_BLOCKSIZE) this_size = AOF_IOURING_MAX_BLOCKSIZE;

            iov[inflight].iov_base = ((char *)buf) + offset;
            iov[inflight].iov_len = this_size;
            if (0 != prepWrite(fd, &iov[inflight], 1, 0)) {
                fprintf(stderr, "## prepWrite failed when persist AOF file by io_uring...\n");
            }

            len -= this_size;
            offset += this_size;
            writing += this_size;
            inflight++;
        }

        if (has_inflight != inflight) io_uring_submit(_aof_io_uring);

        int depth;
        if (len)
            depth = AOF_IOURING_MAX_ENTRIES;
        else
            depth = 1;

        while (inflight >= depth) {
            if (0 != reapCompletions()) {
                fprintf(stderr, "## reapCompletions failed when persist AOF file by io_uring...\n");
                return totwritten;
            }
        }
        totwritten = writing;
    }

    return totwritten;
}

#else

#ifndef UNUSED
#define UNUSED(V) ((void)V)
#endif

int initAofIOUring(void) {
    return 0;
}

void freeAofIOUring(void) {
    return;
}

struct io_uring *getAofIOUring(void) {
    return 0;
}

int aofWriteByIOUring(int fd, const char *buf, size_t len) {
    UNUSED(fd);
    UNUSED(buf);
    UNUSED(len);
    return 0;
}
#endif /* end of HAVE_IO_URING */
