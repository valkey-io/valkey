#ifndef VALKEY_ADAPTERS_LIBSDEVENT_H
#define VALKEY_ADAPTERS_LIBSDEVENT_H
#include "../async.h"
#include "../cluster.h"
#include "../valkey.h"

#include <systemd/sd-event.h>

#define VALKEY_LIBSDEVENT_DELETED 0x01
#define VALKEY_LIBSDEVENT_ENTERED 0x02

typedef struct valkeyLibsdeventEvents {
    valkeyAsyncContext *context;
    struct sd_event *event;
    struct sd_event_source *fdSource;
    struct sd_event_source *timerSource;
    int fd;
    short flags;
    short state;
} valkeyLibsdeventEvents;

static void valkeyLibsdeventDestroy(valkeyLibsdeventEvents *e) {
    if (e->fdSource) {
        e->fdSource = sd_event_source_disable_unref(e->fdSource);
    }
    if (e->timerSource) {
        e->timerSource = sd_event_source_disable_unref(e->timerSource);
    }
    sd_event_unref(e->event);
    vk_free(e);
}

static int valkeyLibsdeventTimeoutHandler(sd_event_source *s, uint64_t usec, void *userdata) {
    ((void)s);
    ((void)usec);
    valkeyLibsdeventEvents *e = (valkeyLibsdeventEvents *)userdata;
    valkeyAsyncHandleTimeout(e->context);
    return 0;
}

static int valkeyLibsdeventHandler(sd_event_source *s, int fd, uint32_t event, void *userdata) {
    ((void)s);
    ((void)fd);
    valkeyLibsdeventEvents *e = (valkeyLibsdeventEvents *)userdata;
    e->state |= VALKEY_LIBSDEVENT_ENTERED;

#define CHECK_DELETED()                         \
    if (e->state & VALKEY_LIBSDEVENT_DELETED) { \
        valkeyLibsdeventDestroy(e);             \
        return 0;                               \
    }

    if ((event & EPOLLIN) && e->context && (e->state & VALKEY_LIBSDEVENT_DELETED) == 0) {
        valkeyAsyncHandleRead(e->context);
        CHECK_DELETED();
    }

    if ((event & EPOLLOUT) && e->context && (e->state & VALKEY_LIBSDEVENT_DELETED) == 0) {
        valkeyAsyncHandleWrite(e->context);
        CHECK_DELETED();
    }

    e->state &= ~VALKEY_LIBSDEVENT_ENTERED;
#undef CHECK_DELETED

    return 0;
}

static void valkeyLibsdeventAddRead(void *userdata) {
    valkeyLibsdeventEvents *e = (valkeyLibsdeventEvents *)userdata;

    if (e->flags & EPOLLIN) {
        return;
    }

    e->flags |= EPOLLIN;

    if (e->flags & EPOLLOUT) {
        sd_event_source_set_io_events(e->fdSource, e->flags);
    } else {
        sd_event_add_io(e->event, &e->fdSource, e->fd, e->flags, valkeyLibsdeventHandler, e);
    }
}

static void valkeyLibsdeventDelRead(void *userdata) {
    valkeyLibsdeventEvents *e = (valkeyLibsdeventEvents *)userdata;

    e->flags &= ~EPOLLIN;

    if (e->flags) {
        sd_event_source_set_io_events(e->fdSource, e->flags);
    } else {
        e->fdSource = sd_event_source_disable_unref(e->fdSource);
    }
}

static void valkeyLibsdeventAddWrite(void *userdata) {
    valkeyLibsdeventEvents *e = (valkeyLibsdeventEvents *)userdata;

    if (e->flags & EPOLLOUT) {
        return;
    }

    e->flags |= EPOLLOUT;

    if (e->flags & EPOLLIN) {
        sd_event_source_set_io_events(e->fdSource, e->flags);
    } else {
        sd_event_add_io(e->event, &e->fdSource, e->fd, e->flags, valkeyLibsdeventHandler, e);
    }
}

static void valkeyLibsdeventDelWrite(void *userdata) {
    valkeyLibsdeventEvents *e = (valkeyLibsdeventEvents *)userdata;

    e->flags &= ~EPOLLOUT;

    if (e->flags) {
        sd_event_source_set_io_events(e->fdSource, e->flags);
    } else {
        e->fdSource = sd_event_source_disable_unref(e->fdSource);
    }
}

static void valkeyLibsdeventCleanup(void *userdata) {
    valkeyLibsdeventEvents *e = (valkeyLibsdeventEvents *)userdata;

    if (!e) {
        return;
    }

    if (e->state & VALKEY_LIBSDEVENT_ENTERED) {
        e->state |= VALKEY_LIBSDEVENT_DELETED;
    } else {
        valkeyLibsdeventDestroy(e);
    }
}

static void valkeyLibsdeventSetTimeout(void *userdata, struct timeval tv) {
    valkeyLibsdeventEvents *e = (valkeyLibsdeventEvents *)userdata;

    uint64_t usec = tv.tv_sec * 1000000 + tv.tv_usec;
    if (!e->timerSource) {
        sd_event_add_time_relative(e->event, &e->timerSource, CLOCK_MONOTONIC, usec, 1, valkeyLibsdeventTimeoutHandler, e);
    } else {
        sd_event_source_set_time_relative(e->timerSource, usec);
    }
}

static int valkeyLibsdeventAttach(valkeyAsyncContext *ac, struct sd_event *event) {
    valkeyContext *c = &(ac->c);
    valkeyLibsdeventEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return VALKEY_ERR;

    /* Create container for context and r/w events */
    e = (valkeyLibsdeventEvents *)vk_calloc(1, sizeof(*e));
    if (e == NULL)
        return VALKEY_ERR;

    /* Initialize and increase event refcount */
    e->context = ac;
    e->event = event;
    e->fd = c->fd;
    sd_event_ref(event);

    /* Register functions to start/stop listening for events */
    ac->ev.addRead = valkeyLibsdeventAddRead;
    ac->ev.delRead = valkeyLibsdeventDelRead;
    ac->ev.addWrite = valkeyLibsdeventAddWrite;
    ac->ev.delWrite = valkeyLibsdeventDelWrite;
    ac->ev.cleanup = valkeyLibsdeventCleanup;
    ac->ev.scheduleTimer = valkeyLibsdeventSetTimeout;
    ac->ev.data = e;

    return VALKEY_OK;
}

/* Internal adapter function with correct function signature. */
static int valkeyLibsdeventAttachAdapter(valkeyAsyncContext *ac, void *event) {
    return valkeyLibsdeventAttach(ac, (struct sd_event *)event);
}

VALKEY_UNUSED
static int valkeyClusterOptionsUseLibsdevent(valkeyClusterOptions *options,
                                             struct sd_event *event) {
    if (options == NULL || event == NULL) {
        return VALKEY_ERR;
    }

    options->attach_fn = valkeyLibsdeventAttachAdapter;
    options->attach_data = event;
    return VALKEY_OK;
}

#endif /* VALKEY_ADAPTERS_LIBSDEVENT_H */
