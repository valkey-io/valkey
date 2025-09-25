#ifndef VALKEY_ADAPTERS_IVYKIS_H
#define VALKEY_ADAPTERS_IVYKIS_H
#include "../async.h"
#include "../cluster.h"
#include "../valkey.h"

#include <iv.h>

typedef struct valkeyIvykisEvents {
    valkeyAsyncContext *context;
    struct iv_fd fd;
} valkeyIvykisEvents;

static void valkeyIvykisReadEvent(void *arg) {
    valkeyAsyncContext *context = (valkeyAsyncContext *)arg;
    valkeyAsyncHandleRead(context);
}

static void valkeyIvykisWriteEvent(void *arg) {
    valkeyAsyncContext *context = (valkeyAsyncContext *)arg;
    valkeyAsyncHandleWrite(context);
}

static void valkeyIvykisAddRead(void *privdata) {
    valkeyIvykisEvents *e = (valkeyIvykisEvents *)privdata;
    iv_fd_set_handler_in(&e->fd, valkeyIvykisReadEvent);
}

static void valkeyIvykisDelRead(void *privdata) {
    valkeyIvykisEvents *e = (valkeyIvykisEvents *)privdata;
    iv_fd_set_handler_in(&e->fd, NULL);
}

static void valkeyIvykisAddWrite(void *privdata) {
    valkeyIvykisEvents *e = (valkeyIvykisEvents *)privdata;
    iv_fd_set_handler_out(&e->fd, valkeyIvykisWriteEvent);
}

static void valkeyIvykisDelWrite(void *privdata) {
    valkeyIvykisEvents *e = (valkeyIvykisEvents *)privdata;
    iv_fd_set_handler_out(&e->fd, NULL);
}

static void valkeyIvykisCleanup(void *privdata) {
    valkeyIvykisEvents *e = (valkeyIvykisEvents *)privdata;

    iv_fd_unregister(&e->fd);
    vk_free(e);
}

static int valkeyIvykisAttach(valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);
    valkeyIvykisEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return VALKEY_ERR;

    /* Create container for context and r/w events */
    e = (valkeyIvykisEvents *)vk_malloc(sizeof(*e));
    if (e == NULL)
        return VALKEY_ERR;

    e->context = ac;

    /* Register functions to start/stop listening for events */
    ac->ev.addRead = valkeyIvykisAddRead;
    ac->ev.delRead = valkeyIvykisDelRead;
    ac->ev.addWrite = valkeyIvykisAddWrite;
    ac->ev.delWrite = valkeyIvykisDelWrite;
    ac->ev.cleanup = valkeyIvykisCleanup;
    ac->ev.data = e;

    /* Initialize and install read/write events */
    IV_FD_INIT(&e->fd);
    e->fd.fd = c->fd;
    e->fd.handler_in = valkeyIvykisReadEvent;
    e->fd.handler_out = valkeyIvykisWriteEvent;
    e->fd.handler_err = NULL;
    e->fd.cookie = e->context;

    iv_fd_register(&e->fd);

    return VALKEY_OK;
}

/* Internal adapter function with correct function signature. */
static int valkeyIvykisAttachAdapter(valkeyAsyncContext *ac, VALKEY_UNUSED void *) {
    return valkeyIvykisAttach(ac);
}

VALKEY_UNUSED
static int valkeyClusterOptionsUseIvykis(valkeyClusterOptions *options) {
    if (options == NULL) {
        return VALKEY_ERR;
    }

    options->attach_fn = valkeyIvykisAttachAdapter;
    return VALKEY_OK;
}

#endif /* VALKEY_ADAPTERS_IVYKIS_H */
