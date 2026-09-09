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

#ifndef VALKEY_ADAPTERS_LIBEV_H
#define VALKEY_ADAPTERS_LIBEV_H
#include "../async.h"
#include "../cluster.h"
#include "../valkey.h"

#include <ev.h>
#include <stdlib.h>
#include <sys/types.h>

typedef struct valkeyLibevEvents {
    valkeyAsyncContext *context;
    struct ev_loop *loop;
    int reading, writing;
    ev_io rev, wev;
    ev_timer timer;
} valkeyLibevEvents;

static void valkeyLibevReadEvent(EV_P_ ev_io *watcher, int revents) {
#if EV_MULTIPLICITY
    ((void)EV_A);
#endif
    ((void)revents);

    valkeyLibevEvents *e = (valkeyLibevEvents *)watcher->data;
    valkeyAsyncHandleRead(e->context);
}

static void valkeyLibevWriteEvent(EV_P_ ev_io *watcher, int revents) {
#if EV_MULTIPLICITY
    ((void)EV_A);
#endif
    ((void)revents);

    valkeyLibevEvents *e = (valkeyLibevEvents *)watcher->data;
    valkeyAsyncHandleWrite(e->context);
}

static void valkeyLibevAddRead(void *privdata) {
    valkeyLibevEvents *e = (valkeyLibevEvents *)privdata;
#if EV_MULTIPLICITY
    struct ev_loop *loop = e->loop;
#endif
    if (!e->reading) {
        e->reading = 1;
        ev_io_start(EV_A_ & e->rev);
    }
}

static void valkeyLibevDelRead(void *privdata) {
    valkeyLibevEvents *e = (valkeyLibevEvents *)privdata;
#if EV_MULTIPLICITY
    struct ev_loop *loop = e->loop;
#endif
    if (e->reading) {
        e->reading = 0;
        ev_io_stop(EV_A_ & e->rev);
    }
}

static void valkeyLibevAddWrite(void *privdata) {
    valkeyLibevEvents *e = (valkeyLibevEvents *)privdata;
#if EV_MULTIPLICITY
    struct ev_loop *loop = e->loop;
#endif
    if (!e->writing) {
        e->writing = 1;
        ev_io_start(EV_A_ & e->wev);
    }
}

static void valkeyLibevDelWrite(void *privdata) {
    valkeyLibevEvents *e = (valkeyLibevEvents *)privdata;
#if EV_MULTIPLICITY
    struct ev_loop *loop = e->loop;
#endif
    if (e->writing) {
        e->writing = 0;
        ev_io_stop(EV_A_ & e->wev);
    }
}

static void valkeyLibevStopTimer(void *privdata) {
    valkeyLibevEvents *e = (valkeyLibevEvents *)privdata;
#if EV_MULTIPLICITY
    struct ev_loop *loop = e->loop;
#endif
    ev_timer_stop(EV_A_ & e->timer);
}

static void valkeyLibevCleanup(void *privdata) {
    valkeyLibevEvents *e = (valkeyLibevEvents *)privdata;
    valkeyLibevDelRead(privdata);
    valkeyLibevDelWrite(privdata);
    valkeyLibevStopTimer(privdata);
    vk_free(e);
}

static void valkeyLibevTimeout(EV_P_ ev_timer *timer, int revents) {
#if EV_MULTIPLICITY
    ((void)EV_A);
#endif
    ((void)revents);
    valkeyLibevEvents *e = (valkeyLibevEvents *)timer->data;
    valkeyAsyncHandleTimeout(e->context);
}

static void valkeyLibevSetTimeout(void *privdata, struct timeval tv) {
    valkeyLibevEvents *e = (valkeyLibevEvents *)privdata;
#if EV_MULTIPLICITY
    struct ev_loop *loop = e->loop;
#endif

    if (!ev_is_active(&e->timer)) {
        ev_init(&e->timer, valkeyLibevTimeout);
        e->timer.data = e;
    }

    e->timer.repeat = tv.tv_sec + tv.tv_usec / 1000000.00;
    ev_timer_again(EV_A_ & e->timer);
}

static int valkeyLibevAttach(EV_P_ valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);
    valkeyLibevEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return VALKEY_ERR;

    /* Create container for context and r/w events */
    e = (valkeyLibevEvents *)vk_calloc(1, sizeof(*e));
    if (e == NULL)
        return VALKEY_ERR;

    e->context = ac;
#if EV_MULTIPLICITY
    e->loop = EV_A;
#else
    e->loop = NULL;
#endif
    e->rev.data = e;
    e->wev.data = e;

    /* Register functions to start/stop listening for events */
    ac->ev.addRead = valkeyLibevAddRead;
    ac->ev.delRead = valkeyLibevDelRead;
    ac->ev.addWrite = valkeyLibevAddWrite;
    ac->ev.delWrite = valkeyLibevDelWrite;
    ac->ev.cleanup = valkeyLibevCleanup;
    ac->ev.scheduleTimer = valkeyLibevSetTimeout;
    ac->ev.data = e;

    /* Initialize read/write events */
    ev_io_init(&e->rev, valkeyLibevReadEvent, c->fd, EV_READ);
    ev_io_init(&e->wev, valkeyLibevWriteEvent, c->fd, EV_WRITE);
    return VALKEY_OK;
}

/* Internal adapter function with correct function signature. */
static int valkeyLibevAttachAdapter(valkeyAsyncContext *ac, void *loop) {
    return valkeyLibevAttach((struct ev_loop *)loop, ac);
}

VALKEY_UNUSED
static int valkeyClusterOptionsUseLibev(valkeyClusterOptions *options,
                                        struct ev_loop *loop) {
    if (options == NULL || loop == NULL) {
        return VALKEY_ERR;
    }

    options->attach_fn = valkeyLibevAttachAdapter;
    options->attach_data = loop;
    return VALKEY_OK;
}

#endif /* VALKEY_ADAPTERS_LIBEV_H */
