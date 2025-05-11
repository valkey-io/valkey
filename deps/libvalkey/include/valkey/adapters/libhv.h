#ifndef VALKEY_ADAPTERS_LIBHV_H
#define VALKEY_ADAPTERS_LIBHV_H

#include "../async.h"
#include "../cluster.h"
#include "../valkey.h"

#include <hv/hloop.h>

typedef struct valkeyLibhvEvents {
    hio_t *io;
    htimer_t *timer;
} valkeyLibhvEvents;

static void valkeyLibhvHandleEvents(hio_t *io) {
    valkeyAsyncContext *context = (valkeyAsyncContext *)hevent_userdata(io);
    int events = hio_events(io);
    int revents = hio_revents(io);
    if (context && (events & HV_READ) && (revents & HV_READ)) {
        valkeyAsyncHandleRead(context);
    }
    if (context && (events & HV_WRITE) && (revents & HV_WRITE)) {
        valkeyAsyncHandleWrite(context);
    }
}

static void valkeyLibhvAddRead(void *privdata) {
    valkeyLibhvEvents *events = (valkeyLibhvEvents *)privdata;
    hio_add(events->io, valkeyLibhvHandleEvents, HV_READ);
}

static void valkeyLibhvDelRead(void *privdata) {
    valkeyLibhvEvents *events = (valkeyLibhvEvents *)privdata;
    hio_del(events->io, HV_READ);
}

static void valkeyLibhvAddWrite(void *privdata) {
    valkeyLibhvEvents *events = (valkeyLibhvEvents *)privdata;
    hio_add(events->io, valkeyLibhvHandleEvents, HV_WRITE);
}

static void valkeyLibhvDelWrite(void *privdata) {
    valkeyLibhvEvents *events = (valkeyLibhvEvents *)privdata;
    hio_del(events->io, HV_WRITE);
}

static void valkeyLibhvCleanup(void *privdata) {
    valkeyLibhvEvents *events = (valkeyLibhvEvents *)privdata;

    if (events->timer)
        htimer_del(events->timer);

    hio_close(events->io);
    hevent_set_userdata(events->io, NULL);

    vk_free(events);
}

static void valkeyLibhvTimeout(htimer_t *timer) {
    hio_t *io = (hio_t *)hevent_userdata(timer);
    valkeyAsyncHandleTimeout((valkeyAsyncContext *)hevent_userdata(io));
}

static void valkeyLibhvSetTimeout(void *privdata, struct timeval tv) {
    valkeyLibhvEvents *events;
    uint32_t millis;
    hloop_t *loop;

    events = (valkeyLibhvEvents *)privdata;
    millis = tv.tv_sec * 1000 + tv.tv_usec / 1000;

    if (millis == 0) {
        /* Libhv disallows zero'd timers so treat this as a delete or NO OP */
        if (events->timer) {
            htimer_del(events->timer);
            events->timer = NULL;
        }
    } else if (events->timer == NULL) {
        /* Add new timer */
        loop = hevent_loop(events->io);
        events->timer = htimer_add(loop, valkeyLibhvTimeout, millis, 1);
        hevent_set_userdata(events->timer, events->io);
    } else {
        /* Update existing timer */
        htimer_reset(events->timer, millis);
    }
}

static int valkeyLibhvAttach(valkeyAsyncContext *ac, hloop_t *loop) {
    valkeyContext *c = &(ac->c);
    valkeyLibhvEvents *events;
    hio_t *io = NULL;

    if (ac->ev.data != NULL) {
        return VALKEY_ERR;
    }

    /* Create container struct to keep track of our io and any timer */
    events = (valkeyLibhvEvents *)vk_malloc(sizeof(*events));
    if (events == NULL) {
        return VALKEY_ERR;
    }

    io = hio_get(loop, c->fd);
    if (io == NULL) {
        vk_free(events);
        return VALKEY_ERR;
    }

    hevent_set_userdata(io, ac);

    events->io = io;
    events->timer = NULL;

    ac->ev.addRead = valkeyLibhvAddRead;
    ac->ev.delRead = valkeyLibhvDelRead;
    ac->ev.addWrite = valkeyLibhvAddWrite;
    ac->ev.delWrite = valkeyLibhvDelWrite;
    ac->ev.cleanup = valkeyLibhvCleanup;
    ac->ev.scheduleTimer = valkeyLibhvSetTimeout;
    ac->ev.data = events;

    return VALKEY_OK;
}

/* Internal adapter function with correct function signature. */
static int valkeyLibhvAttachAdapter(valkeyAsyncContext *ac, void *loop) {
    return valkeyLibhvAttach(ac, (hloop_t *)loop);
}

VALKEY_UNUSED
static int valkeyClusterOptionsUseLibhv(valkeyClusterOptions *options,
                                        hloop_t *loop) {
    if (options == NULL || loop == NULL) {
        return VALKEY_ERR;
    }

    options->attach_fn = valkeyLibhvAttachAdapter;
    options->attach_data = loop;
    return VALKEY_OK;
}

#endif /* VALKEY_ADAPTERS_LIBHV_H */
