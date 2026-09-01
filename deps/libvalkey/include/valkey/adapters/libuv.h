/*
 * Copyright (c) 2013, Erik Dubbelboer
 * Copyright (c) 2021, Red Hat
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

#ifndef VALKEY_ADAPTERS_LIBUV_H
#define VALKEY_ADAPTERS_LIBUV_H
#include "../async.h"
#include "../cluster.h"
#include "../valkey.h"

#include <stdlib.h>
#include <string.h>
#include <uv.h>

typedef struct valkeyLibuvEvents {
    valkeyAsyncContext *context;
    uv_poll_t handle;
    uv_timer_t timer;
    int events;
} valkeyLibuvEvents;

static void valkeyLibuvPoll(uv_poll_t *handle, int status, int events) {
    valkeyLibuvEvents *p = (valkeyLibuvEvents *)handle->data;
    int ev = (status ? p->events : events);

    if (p->context != NULL && (ev & UV_READABLE)) {
        valkeyAsyncHandleRead(p->context);
    }
    if (p->context != NULL && (ev & UV_WRITABLE)) {
        valkeyAsyncHandleWrite(p->context);
    }
}

static void valkeyLibuvAddRead(void *privdata) {
    valkeyLibuvEvents *p = (valkeyLibuvEvents *)privdata;

    if (p->events & UV_READABLE) {
        return;
    }

    p->events |= UV_READABLE;

    uv_poll_start(&p->handle, p->events, valkeyLibuvPoll);
}

static void valkeyLibuvDelRead(void *privdata) {
    valkeyLibuvEvents *p = (valkeyLibuvEvents *)privdata;

    p->events &= ~UV_READABLE;

    if (p->events) {
        uv_poll_start(&p->handle, p->events, valkeyLibuvPoll);
    } else {
        uv_poll_stop(&p->handle);
    }
}

static void valkeyLibuvAddWrite(void *privdata) {
    valkeyLibuvEvents *p = (valkeyLibuvEvents *)privdata;

    if (p->events & UV_WRITABLE) {
        return;
    }

    p->events |= UV_WRITABLE;

    uv_poll_start(&p->handle, p->events, valkeyLibuvPoll);
}

static void valkeyLibuvDelWrite(void *privdata) {
    valkeyLibuvEvents *p = (valkeyLibuvEvents *)privdata;

    p->events &= ~UV_WRITABLE;

    if (p->events) {
        uv_poll_start(&p->handle, p->events, valkeyLibuvPoll);
    } else {
        uv_poll_stop(&p->handle);
    }
}

static void on_timer_close(uv_handle_t *handle) {
    valkeyLibuvEvents *p = (valkeyLibuvEvents *)handle->data;
    p->timer.data = NULL;
    if (!p->handle.data) {
        // both timer and handle are closed
        vk_free(p);
    }
    // else, wait for `on_handle_close`
}

static void on_handle_close(uv_handle_t *handle) {
    valkeyLibuvEvents *p = (valkeyLibuvEvents *)handle->data;
    p->handle.data = NULL;
    if (!p->timer.data) {
        // timer never started, or timer already destroyed
        vk_free(p);
    }
    // else, wait for `on_timer_close`
}

// libuv removed `status` parameter since v0.11.23
// see: https://github.com/libuv/libuv/blob/v0.11.23/include/uv.h
#if (UV_VERSION_MAJOR == 0 && UV_VERSION_MINOR < 11) || \
    (UV_VERSION_MAJOR == 0 && UV_VERSION_MINOR == 11 && UV_VERSION_PATCH < 23)
static void valkeyLibuvTimeout(uv_timer_t *timer, int status) {
    (void)status; // unused
#else
static void valkeyLibuvTimeout(uv_timer_t *timer) {
#endif
    valkeyLibuvEvents *e = (valkeyLibuvEvents *)timer->data;
    valkeyAsyncHandleTimeout(e->context);
}

static void valkeyLibuvSetTimeout(void *privdata, struct timeval tv) {
    valkeyLibuvEvents *p = (valkeyLibuvEvents *)privdata;

    uint64_t millisec = tv.tv_sec * 1000 + tv.tv_usec / 1000.0;
    if (!p->timer.data) {
        // timer is uninitialized
        if (uv_timer_init(p->handle.loop, &p->timer) != 0) {
            return;
        }
        p->timer.data = p;
    }
    // updates the timeout if the timer has already started
    // or start the timer
    uv_timer_start(&p->timer, valkeyLibuvTimeout, millisec, 0);
}

static void valkeyLibuvCleanup(void *privdata) {
    valkeyLibuvEvents *p = (valkeyLibuvEvents *)privdata;

    p->context = NULL; // indicate that context might no longer exist
    if (p->timer.data) {
        uv_close((uv_handle_t *)&p->timer, on_timer_close);
    }
    uv_close((uv_handle_t *)&p->handle, on_handle_close);
}

static int valkeyLibuvAttach(valkeyAsyncContext *ac, uv_loop_t *loop) {
    valkeyContext *c = &(ac->c);

    if (ac->ev.data != NULL) {
        return VALKEY_ERR;
    }

    ac->ev.addRead = valkeyLibuvAddRead;
    ac->ev.delRead = valkeyLibuvDelRead;
    ac->ev.addWrite = valkeyLibuvAddWrite;
    ac->ev.delWrite = valkeyLibuvDelWrite;
    ac->ev.cleanup = valkeyLibuvCleanup;
    ac->ev.scheduleTimer = valkeyLibuvSetTimeout;

    valkeyLibuvEvents *p = (valkeyLibuvEvents *)vk_malloc(sizeof(*p));
    if (p == NULL)
        return VALKEY_ERR;

    memset(p, 0, sizeof(*p));

    if (uv_poll_init_socket(loop, &p->handle, c->fd) != 0) {
        vk_free(p);
        return VALKEY_ERR;
    }

    ac->ev.data = p;
    p->handle.data = p;
    p->context = ac;

    return VALKEY_OK;
}

/* Internal adapter function with correct function signature. */
static int valkeyLibuvAttachAdapter(valkeyAsyncContext *ac, void *loop) {
    return valkeyLibuvAttach(ac, (uv_loop_t *)loop);
}

VALKEY_UNUSED
static int valkeyClusterOptionsUseLibuv(valkeyClusterOptions *options,
                                        uv_loop_t *loop) {
    if (options == NULL || loop == NULL) {
        return VALKEY_ERR;
    }

    options->attach_fn = valkeyLibuvAttachAdapter;
    options->attach_data = loop;
    return VALKEY_OK;
}

#endif /* VALKEY_ADAPTERS_LIBUV_H */
