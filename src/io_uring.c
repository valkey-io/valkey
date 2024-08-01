/*
 * Copyright (c) 2024 Samsung Electronics Co., Ltd. All rights reserved.
 * Author: Wenwen Chen <Wenwen.chen@samsung.com>
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
#include "server.h"
#ifdef HAVE_IO_URING
#include <liburing.h>
#include "zmalloc.h"

/* AOF io_uring max QD and blocksize */
#define AOF_IOURING_MAX_ENTRIES (64)
#define AOF_IOURING_MAX_BLOCKSIZE (32 * 1024)

struct io_data {
    struct iovec iov;
    int used;
};

static struct io_data buffer[AOF_IOURING_MAX_ENTRIES];
static struct io_uring *_aof_io_uring;

static inline void initIoData(void) {
    for (int index = 0; index < AOF_IOURING_MAX_ENTRIES; index++) {
        buffer[index].used = 0;
    }
}

static inline struct io_data *getIoData(void) {
    struct io_data *ret = NULL;
    for (int index = 0; index < AOF_IOURING_MAX_ENTRIES; index++) {
        if (0 == buffer[index].used) {
            ret = &buffer[index];
            ret->used = 1;
            return ret;
        }
    }
    return ret;
}

static inline void releaseIoData(struct io_data *data) {
    if (NULL == data) return;
    data->used = 0;
}

int initAofIOUring(void) {
    _aof_io_uring = NULL;
    struct io_uring *ring = zmalloc(sizeof(struct io_uring));
    if (!ring) return -1;

    int ret = io_uring_queue_init(AOF_IOURING_MAX_ENTRIES, ring, 0);
    if (ret != 0) {
        zfree(ring);
        return -1;
    }

    _aof_io_uring = ring;
    initIoData();
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
    if (!_aof_io_uring) initAofIOUring();

    return _aof_io_uring;
}

static int prepWrite(int fd, struct io_data *data, unsigned nr_vecs, unsigned offset) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(_aof_io_uring);
    if (!sqe) return -1;

    io_uring_prep_writev(sqe, fd, &data->iov, nr_vecs, offset);
    io_uring_sqe_set_data(sqe, data);
    return 0;
}

static int reapCq(int *len) {
    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe(_aof_io_uring, &cqe);
    if (ret < 0) return ret;

    struct io_data *data = io_uring_cqe_get_data(cqe);
    releaseIoData(data);
    *len = cqe->res;
    io_uring_cqe_seen(_aof_io_uring, cqe);
    return 0;
}

int aofWriteByIOUring(int fd, const char *buf, size_t len) {
    size_t submit_size = 0;
    size_t complete_size = 0;
    int submit_cnt = 0;
    int complete_cnt = 0;
    size_t write_left = len;

    while (write_left || (submit_cnt > complete_cnt)) {
        size_t offset = 0;
        size_t this_size = 0;
        int has_submit = submit_cnt;
        struct io_data *data = NULL;

        // Queue up as many writes as we can
        while (write_left && ((submit_cnt - complete_cnt) < AOF_IOURING_MAX_ENTRIES)) {
            this_size = write_left;
            if (this_size > AOF_IOURING_MAX_BLOCKSIZE) this_size = AOF_IOURING_MAX_BLOCKSIZE;

            data = getIoData();
            if (NULL == data) return complete_size;

            data->iov.iov_base = ((char *)buf) + offset;
            data->iov.iov_len = this_size;
            if (0 != prepWrite(fd, data, 1, 0)) return complete_size;

            write_left -= this_size;
            offset += this_size;
            submit_size += this_size;
            submit_cnt++;
        }
        if (has_submit != submit_cnt) {
            int ret = io_uring_submit(_aof_io_uring);
            if (ret < 0) return complete_size;
        }
        // Queue is full at this point. Find at least one completion.
        while (complete_size < len) {
            int cq_size = 0;
            if (0 != reapCq(&cq_size)) break;

            complete_size += cq_size;
            complete_cnt++;
        }
    }

    return complete_size;
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
