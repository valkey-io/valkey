#ifndef VALKEY_ADAPTERS_GLIB_H
#define VALKEY_ADAPTERS_GLIB_H

#include "../async.h"
#include "../cluster.h"
#include "../valkey.h"

#include <glib.h>

typedef struct
{
    GSource source;
    valkeyAsyncContext *ac;
    GPollFD poll_fd;
} ValkeySource;

static void
valkey_source_add_read(gpointer data) {
    ValkeySource *source = (ValkeySource *)data;
    g_return_if_fail(source);
    source->poll_fd.events |= G_IO_IN;
    g_main_context_wakeup(g_source_get_context((GSource *)data));
}

static void
valkey_source_del_read(gpointer data) {
    ValkeySource *source = (ValkeySource *)data;
    g_return_if_fail(source);
    source->poll_fd.events &= ~G_IO_IN;
    g_main_context_wakeup(g_source_get_context((GSource *)data));
}

static void
valkey_source_add_write(gpointer data) {
    ValkeySource *source = (ValkeySource *)data;
    g_return_if_fail(source);
    source->poll_fd.events |= G_IO_OUT;
    g_main_context_wakeup(g_source_get_context((GSource *)data));
}

static void
valkey_source_del_write(gpointer data) {
    ValkeySource *source = (ValkeySource *)data;
    g_return_if_fail(source);
    source->poll_fd.events &= ~G_IO_OUT;
    g_main_context_wakeup(g_source_get_context((GSource *)data));
}

static void
valkey_source_cleanup(gpointer data) {
    ValkeySource *source = (ValkeySource *)data;

    g_return_if_fail(source);

    valkey_source_del_read(source);
    valkey_source_del_write(source);
    /*
     * It is not our responsibility to remove ourself from the
     * current main loop. However, we will remove the GPollFD.
     */
    if (source->poll_fd.fd >= 0) {
        g_source_remove_poll((GSource *)data, &source->poll_fd);
        source->poll_fd.fd = -1;
    }
}

static gboolean
valkey_source_prepare(GSource *source,
                      gint *timeout_) {
    ValkeySource *valkey = (ValkeySource *)source;
    *timeout_ = -1;
    return !!(valkey->poll_fd.events & valkey->poll_fd.revents);
}

static gboolean
valkey_source_check(GSource *source) {
    ValkeySource *valkey = (ValkeySource *)source;
    return !!(valkey->poll_fd.events & valkey->poll_fd.revents);
}

static gboolean
valkey_source_dispatch(GSource *source,
                       GSourceFunc callback,
                       gpointer user_data) {
    ValkeySource *valkey = (ValkeySource *)source;

    if ((valkey->poll_fd.revents & G_IO_OUT)) {
        valkeyAsyncHandleWrite(valkey->ac);
        valkey->poll_fd.revents &= ~G_IO_OUT;
    }

    if ((valkey->poll_fd.revents & G_IO_IN)) {
        valkeyAsyncHandleRead(valkey->ac);
        valkey->poll_fd.revents &= ~G_IO_IN;
    }

    if (callback) {
        return callback(user_data);
    }

    return TRUE;
}

static void
valkey_source_finalize(GSource *source) {
    ValkeySource *valkey = (ValkeySource *)source;

    if (valkey->poll_fd.fd >= 0) {
        g_source_remove_poll(source, &valkey->poll_fd);
        valkey->poll_fd.fd = -1;
    }
}

static GSource *
valkey_source_new(valkeyAsyncContext *ac) {
    static GSourceFuncs source_funcs = {
        .prepare = valkey_source_prepare,
        .check = valkey_source_check,
        .dispatch = valkey_source_dispatch,
        .finalize = valkey_source_finalize,
    };
    valkeyContext *c = &ac->c;
    ValkeySource *source;

    g_return_val_if_fail(ac != NULL, NULL);

    source = (ValkeySource *)g_source_new(&source_funcs, sizeof *source);
    if (source == NULL)
        return NULL;

    source->ac = ac;
    source->poll_fd.fd = c->fd;
    source->poll_fd.events = 0;
    source->poll_fd.revents = 0;
    g_source_add_poll((GSource *)source, &source->poll_fd);

    ac->ev.addRead = valkey_source_add_read;
    ac->ev.delRead = valkey_source_del_read;
    ac->ev.addWrite = valkey_source_add_write;
    ac->ev.delWrite = valkey_source_del_write;
    ac->ev.cleanup = valkey_source_cleanup;
    ac->ev.data = source;

    return (GSource *)source;
}

/* Internal adapter function with correct function signature. */
static int valkeyGlibAttachAdapter(valkeyAsyncContext *ac, void *context) {
    if (g_source_attach(valkey_source_new(ac), (GMainContext *)context) > 0) {
        return VALKEY_OK;
    }
    return VALKEY_ERR;
}

VALKEY_UNUSED
static int valkeyClusterOptionsUseGlib(valkeyClusterOptions *options,
                                       GMainContext *context) {
    if (options == NULL) { // A NULL context is accepted.
        return VALKEY_ERR;
    }

    options->attach_fn = valkeyGlibAttachAdapter;
    options->attach_data = context;
    return VALKEY_OK;
}

#endif /* VALKEY_ADAPTERS_GLIB_H */
