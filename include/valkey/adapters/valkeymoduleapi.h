#ifndef VALKEY_ADAPTERS_VALKEYMODULEAPI_H
#define VALKEY_ADAPTERS_VALKEYMODULEAPI_H

#include "../async.h"
#include "../valkey.h"
#include "valkeymodule.h"

#include <sys/types.h>

typedef struct valkeyModuleEvents {
    valkeyAsyncContext *context;
    ValkeyModuleCtx *module_ctx;
    int fd;
    int reading, writing;
    int timer_active;
    ValkeyModuleTimerID timer_id;
} valkeyModuleEvents;

static inline void valkeyModuleReadEvent(int fd, void *privdata, int mask) {
    (void)fd;
    (void)mask;

    valkeyModuleEvents *e = (valkeyModuleEvents *)privdata;
    valkeyAsyncHandleRead(e->context);
}

static inline void valkeyModuleWriteEvent(int fd, void *privdata, int mask) {
    (void)fd;
    (void)mask;

    valkeyModuleEvents *e = (valkeyModuleEvents *)privdata;
    valkeyAsyncHandleWrite(e->context);
}

static inline void valkeyModuleAddRead(void *privdata) {
    valkeyModuleEvents *e = (valkeyModuleEvents *)privdata;
    if (!e->reading) {
        e->reading = 1;
        ValkeyModule_EventLoopAdd(e->fd, VALKEYMODULE_EVENTLOOP_READABLE, valkeyModuleReadEvent, e);
    }
}

static inline void valkeyModuleDelRead(void *privdata) {
    valkeyModuleEvents *e = (valkeyModuleEvents *)privdata;
    if (e->reading) {
        e->reading = 0;
        ValkeyModule_EventLoopDel(e->fd, VALKEYMODULE_EVENTLOOP_READABLE);
    }
}

static inline void valkeyModuleAddWrite(void *privdata) {
    valkeyModuleEvents *e = (valkeyModuleEvents *)privdata;
    if (!e->writing) {
        e->writing = 1;
        ValkeyModule_EventLoopAdd(e->fd, VALKEYMODULE_EVENTLOOP_WRITABLE, valkeyModuleWriteEvent, e);
    }
}

static inline void valkeyModuleDelWrite(void *privdata) {
    valkeyModuleEvents *e = (valkeyModuleEvents *)privdata;
    if (e->writing) {
        e->writing = 0;
        ValkeyModule_EventLoopDel(e->fd, VALKEYMODULE_EVENTLOOP_WRITABLE);
    }
}

static inline void valkeyModuleStopTimer(void *privdata) {
    valkeyModuleEvents *e = (valkeyModuleEvents *)privdata;
    if (e->timer_active) {
        ValkeyModule_StopTimer(e->module_ctx, e->timer_id, NULL);
    }
    e->timer_active = 0;
}

static inline void valkeyModuleCleanup(void *privdata) {
    valkeyModuleEvents *e = (valkeyModuleEvents *)privdata;
    valkeyModuleDelRead(privdata);
    valkeyModuleDelWrite(privdata);
    valkeyModuleStopTimer(privdata);
    vk_free(e);
}

static inline void valkeyModuleTimeout(ValkeyModuleCtx *ctx, void *privdata) {
    (void)ctx;

    valkeyModuleEvents *e = (valkeyModuleEvents *)privdata;
    e->timer_active = 0;
    valkeyAsyncHandleTimeout(e->context);
}

static inline void valkeyModuleSetTimeout(void *privdata, struct timeval tv) {
    valkeyModuleEvents *e = (valkeyModuleEvents *)privdata;

    valkeyModuleStopTimer(privdata);

    mstime_t millis = tv.tv_sec * 1000 + tv.tv_usec / 1000.0;
    e->timer_id = ValkeyModule_CreateTimer(e->module_ctx, millis, valkeyModuleTimeout, e);
    e->timer_active = 1;
}

/* Check if Redis version is compatible with the adapter. */
static inline int valkeyModuleCompatibilityCheck(void) {
    if (!ValkeyModule_EventLoopAdd ||
        !ValkeyModule_EventLoopDel ||
        !ValkeyModule_CreateTimer ||
        !ValkeyModule_StopTimer) {
        return VALKEY_ERR;
    }
    return VALKEY_OK;
}

static inline int valkeyModuleAttach(valkeyAsyncContext *ac, ValkeyModuleCtx *module_ctx) {
    valkeyContext *c = &(ac->c);
    valkeyModuleEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return VALKEY_ERR;

    /* Create container for context and r/w events */
    e = (valkeyModuleEvents *)vk_malloc(sizeof(*e));
    if (e == NULL)
        return VALKEY_ERR;

    e->context = ac;
    e->module_ctx = module_ctx;
    e->fd = c->fd;
    e->reading = e->writing = 0;
    e->timer_active = 0;

    /* Register functions to start/stop listening for events */
    ac->ev.addRead = valkeyModuleAddRead;
    ac->ev.delRead = valkeyModuleDelRead;
    ac->ev.addWrite = valkeyModuleAddWrite;
    ac->ev.delWrite = valkeyModuleDelWrite;
    ac->ev.cleanup = valkeyModuleCleanup;
    ac->ev.scheduleTimer = valkeyModuleSetTimeout;
    ac->ev.data = e;

    return VALKEY_OK;
}

#endif /* VALKEY_ADAPTERS_VALKEYMODULEAPI_H */
