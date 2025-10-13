/*
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

#ifndef VALKEY_ADAPTERS_AE_H
#define VALKEY_ADAPTERS_AE_H
#include "../async.h"
#include "../cluster.h"
#include "../valkey.h"

#include <ae.h>
#include <sys/types.h>

typedef struct valkeyAeEvents {
    valkeyAsyncContext *context;
    aeEventLoop *loop;
    int fd;
    int reading, writing;
} valkeyAeEvents;

static void valkeyAeReadEvent(aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void)el);
    ((void)fd);
    ((void)mask);

    valkeyAeEvents *e = (valkeyAeEvents *)privdata;
    valkeyAsyncHandleRead(e->context);
}

static void valkeyAeWriteEvent(aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void)el);
    ((void)fd);
    ((void)mask);

    valkeyAeEvents *e = (valkeyAeEvents *)privdata;
    valkeyAsyncHandleWrite(e->context);
}

static void valkeyAeAddRead(void *privdata) {
    valkeyAeEvents *e = (valkeyAeEvents *)privdata;
    aeEventLoop *loop = e->loop;
    if (!e->reading) {
        e->reading = 1;
        aeCreateFileEvent(loop, e->fd, AE_READABLE, valkeyAeReadEvent, e);
    }
}

static void valkeyAeDelRead(void *privdata) {
    valkeyAeEvents *e = (valkeyAeEvents *)privdata;
    aeEventLoop *loop = e->loop;
    if (e->reading) {
        e->reading = 0;
        aeDeleteFileEvent(loop, e->fd, AE_READABLE);
    }
}

static void valkeyAeAddWrite(void *privdata) {
    valkeyAeEvents *e = (valkeyAeEvents *)privdata;
    aeEventLoop *loop = e->loop;
    if (!e->writing) {
        e->writing = 1;
        aeCreateFileEvent(loop, e->fd, AE_WRITABLE, valkeyAeWriteEvent, e);
    }
}

static void valkeyAeDelWrite(void *privdata) {
    valkeyAeEvents *e = (valkeyAeEvents *)privdata;
    aeEventLoop *loop = e->loop;
    if (e->writing) {
        e->writing = 0;
        aeDeleteFileEvent(loop, e->fd, AE_WRITABLE);
    }
}

static void valkeyAeCleanup(void *privdata) {
    valkeyAeEvents *e = (valkeyAeEvents *)privdata;
    valkeyAeDelRead(privdata);
    valkeyAeDelWrite(privdata);
    vk_free(e);
}

static int valkeyAeAttach(aeEventLoop *loop, valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);
    valkeyAeEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return VALKEY_ERR;

    /* Create container for context and r/w events */
    e = (valkeyAeEvents *)vk_malloc(sizeof(*e));
    if (e == NULL)
        return VALKEY_ERR;

    e->context = ac;
    e->loop = loop;
    e->fd = c->fd;
    e->reading = e->writing = 0;

    /* Register functions to start/stop listening for events */
    ac->ev.addRead = valkeyAeAddRead;
    ac->ev.delRead = valkeyAeDelRead;
    ac->ev.addWrite = valkeyAeAddWrite;
    ac->ev.delWrite = valkeyAeDelWrite;
    ac->ev.cleanup = valkeyAeCleanup;
    ac->ev.data = e;

    return VALKEY_OK;
}

/* Internal adapter function with correct function signature. */
static int valkeyAeAttachAdapter(valkeyAsyncContext *ac, void *loop) {
    return valkeyAeAttach((aeEventLoop *)loop, ac);
}

VALKEY_UNUSED
static int valkeyClusterOptionsUseAe(valkeyClusterOptions *options,
                                     aeEventLoop *loop) {
    if (options == NULL || loop == NULL) {
        return VALKEY_ERR;
    }

    options->attach_fn = valkeyAeAttachAdapter;
    options->attach_data = loop;
    return VALKEY_OK;
}

#endif /* VALKEY_ADAPTERS_AE_H */
