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

#ifndef VALKEY_ADAPTERS_LIBEVENT_H
#define VALKEY_ADAPTERS_LIBEVENT_H
#include "../async.h"
#include "../cluster.h"
#include "../valkey.h"

#include <event2/event.h>

#define VALKEY_LIBEVENT_DELETED 0x01
#define VALKEY_LIBEVENT_ENTERED 0x02

typedef struct valkeyLibeventEvents {
    valkeyAsyncContext *context;
    struct event *ev;
    struct event_base *base;
    struct timeval tv;
    short flags;
    short state;
} valkeyLibeventEvents;

static void valkeyLibeventDestroy(valkeyLibeventEvents *e) {
    vk_free(e);
}

static void valkeyLibeventHandler(evutil_socket_t fd, short event, void *arg) {
    ((void)fd);
    valkeyLibeventEvents *e = (valkeyLibeventEvents *)arg;
    e->state |= VALKEY_LIBEVENT_ENTERED;

#define CHECK_DELETED()                       \
    if (e->state & VALKEY_LIBEVENT_DELETED) { \
        valkeyLibeventDestroy(e);             \
        return;                               \
    }

    if ((event & EV_TIMEOUT) && (e->state & VALKEY_LIBEVENT_DELETED) == 0) {
        valkeyAsyncHandleTimeout(e->context);
        CHECK_DELETED();
    }

    if ((event & EV_READ) && e->context && (e->state & VALKEY_LIBEVENT_DELETED) == 0) {
        valkeyAsyncHandleRead(e->context);
        CHECK_DELETED();
    }

    if ((event & EV_WRITE) && e->context && (e->state & VALKEY_LIBEVENT_DELETED) == 0) {
        valkeyAsyncHandleWrite(e->context);
        CHECK_DELETED();
    }

    e->state &= ~VALKEY_LIBEVENT_ENTERED;
#undef CHECK_DELETED
}

static void valkeyLibeventUpdate(void *privdata, short flag, int isRemove) {
    valkeyLibeventEvents *e = (valkeyLibeventEvents *)privdata;
    const struct timeval *tv = e->tv.tv_sec || e->tv.tv_usec ? &e->tv : NULL;

    if (isRemove) {
        if ((e->flags & flag) == 0) {
            return;
        } else {
            e->flags &= ~flag;
        }
    } else {
        if (e->flags & flag) {
            return;
        } else {
            e->flags |= flag;
        }
    }

    event_del(e->ev);
    event_assign(e->ev, e->base, e->context->c.fd, e->flags | EV_PERSIST,
                 valkeyLibeventHandler, privdata);
    event_add(e->ev, tv);
}

static void valkeyLibeventAddRead(void *privdata) {
    valkeyLibeventUpdate(privdata, EV_READ, 0);
}

static void valkeyLibeventDelRead(void *privdata) {
    valkeyLibeventUpdate(privdata, EV_READ, 1);
}

static void valkeyLibeventAddWrite(void *privdata) {
    valkeyLibeventUpdate(privdata, EV_WRITE, 0);
}

static void valkeyLibeventDelWrite(void *privdata) {
    valkeyLibeventUpdate(privdata, EV_WRITE, 1);
}

static void valkeyLibeventCleanup(void *privdata) {
    valkeyLibeventEvents *e = (valkeyLibeventEvents *)privdata;
    if (!e) {
        return;
    }
    event_del(e->ev);
    event_free(e->ev);
    e->ev = NULL;

    if (e->state & VALKEY_LIBEVENT_ENTERED) {
        e->state |= VALKEY_LIBEVENT_DELETED;
    } else {
        valkeyLibeventDestroy(e);
    }
}

static void valkeyLibeventSetTimeout(void *privdata, struct timeval tv) {
    valkeyLibeventEvents *e = (valkeyLibeventEvents *)privdata;
    short flags = e->flags;
    e->flags = 0;
    e->tv = tv;
    valkeyLibeventUpdate(e, flags, 0);
}

static int valkeyLibeventAttach(valkeyAsyncContext *ac, struct event_base *base) {
    valkeyContext *c = &(ac->c);
    valkeyLibeventEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return VALKEY_ERR;

    /* Create container for context and r/w events */
    e = (valkeyLibeventEvents *)vk_calloc(1, sizeof(*e));
    if (e == NULL)
        return VALKEY_ERR;

    e->context = ac;

    /* Register functions to start/stop listening for events */
    ac->ev.addRead = valkeyLibeventAddRead;
    ac->ev.delRead = valkeyLibeventDelRead;
    ac->ev.addWrite = valkeyLibeventAddWrite;
    ac->ev.delWrite = valkeyLibeventDelWrite;
    ac->ev.cleanup = valkeyLibeventCleanup;
    ac->ev.scheduleTimer = valkeyLibeventSetTimeout;
    ac->ev.data = e;

    /* Initialize and install read/write events */
    e->ev = event_new(base, c->fd, EV_READ | EV_WRITE, valkeyLibeventHandler, e);
    e->base = base;
    return VALKEY_OK;
}

/* Internal adapter function with correct function signature. */
static int valkeyLibeventAttachAdapter(valkeyAsyncContext *ac, void *base) {
    return valkeyLibeventAttach(ac, (struct event_base *)base);
}

VALKEY_UNUSED
static int valkeyClusterOptionsUseLibevent(valkeyClusterOptions *options,
                                           struct event_base *base) {
    if (options == NULL || base == NULL) {
        return VALKEY_ERR;
    }

    options->attach_fn = valkeyLibeventAttachAdapter;
    options->attach_data = base;
    return VALKEY_OK;
}

#endif /* VALKEY_ADAPTERS_LIBEVENT_H */
