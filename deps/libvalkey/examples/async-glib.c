#include <valkey/async.h>
#include <valkey/valkey.h>

#include <valkey/adapters/glib.h>

#include <stdlib.h>

static GMainLoop *mainloop;

static void
connect_cb(valkeyAsyncContext *ac G_GNUC_UNUSED,
           int status) {
    if (status != VALKEY_OK) {
        g_printerr("Failed to connect: %s\n", ac->errstr);
        g_main_loop_quit(mainloop);
    } else {
        g_printerr("Connected...\n");
    }
}

static void
disconnect_cb(const valkeyAsyncContext *ac G_GNUC_UNUSED,
              int status) {
    if (status != VALKEY_OK) {
        g_error("Failed to disconnect: %s", ac->errstr);
    } else {
        g_printerr("Disconnected...\n");
        g_main_loop_quit(mainloop);
    }
}

static void
command_cb(valkeyAsyncContext *ac,
           gpointer r,
           gpointer user_data G_GNUC_UNUSED) {
    valkeyReply *reply = r;

    if (reply) {
        g_print("REPLY: %s\n", reply->str);
    }

    valkeyAsyncDisconnect(ac);
}

gint main(gint argc G_GNUC_UNUSED, gchar *argv[] G_GNUC_UNUSED) {
    valkeyAsyncContext *ac;
    GMainContext *context = NULL;
    GSource *source;

    ac = valkeyAsyncConnect("127.0.0.1", 6379);
    if (ac->err) {
        g_printerr("%s\n", ac->errstr);
        exit(EXIT_FAILURE);
    }

    source = valkey_source_new(ac);
    mainloop = g_main_loop_new(context, FALSE);
    g_source_attach(source, context);

    valkeyAsyncSetConnectCallback(ac, connect_cb);
    valkeyAsyncSetDisconnectCallback(ac, disconnect_cb);
    valkeyAsyncCommand(ac, command_cb, NULL, "SET key 1234");
    valkeyAsyncCommand(ac, command_cb, NULL, "GET key");

    g_main_loop_run(mainloop);

    return EXIT_SUCCESS;
}
