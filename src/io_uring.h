/*
 * Copyright (c) 2024 Samsung Electronics Co., Ltd. All rights reserved.
 * Author: Wenwen Chen <Wenwen.chen@samsung.com>
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

#ifndef IO_URING_H
#define IO_URING_H
#include <stddef.h>

/* Initialize io_uring for AOF persistence at server startup
 * if have io_uring configured, setup io_uring submission and completion. */
int initAofIOUring(void);

/* Free io_uring. */
void freeAofIOUring(void);

struct io_uring *getAofIOUring(void);

/* Persist aof_buf to file by using io_uring. */
int aofWriteByIOUring(int fd, const char *buf, size_t len);

#endif /* IO_URING_H */
