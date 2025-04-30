#include <valkey/async.h>
#include <valkey/valkey.h>

#include <valkey/adapters/libevent.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void getCallback(valkeyAsyncContext *c, void *r, void *privdata) {
    valkeyReply *reply = r;
    if (reply == NULL) {
        if (c->errstr) {
            printf("errstr: %s\n", c->errstr);
        }
        return;
    }
    printf("argv[%s]: %s\n", (char *)privdata, reply->str);

    /* Disconnect after receiving the reply to GET */
    valkeyAsyncDisconnect(c);
}

void connectCallback(valkeyAsyncContext *c, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", c->errstr);
        return;
    }
    printf("Connected...\n");
}

void disconnectCallback(const valkeyAsyncContext *c, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", c->errstr);
        return;
    }
    printf("Disconnected...\n");
}

int main(int argc, char **argv) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    struct event_base *base = event_base_new();
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, "127.0.0.1", 6379);
    struct timeval tv = {0};
    tv.tv_sec = 1;
    options.connect_timeout = &tv;

    valkeyAsyncContext *c = valkeyAsyncConnectWithOptions(&options);
    if (c->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", c->errstr);
        return 1;
    }

    valkeyLibeventAttach(c, base);
    valkeyAsyncSetConnectCallback(c, connectCallback);
    valkeyAsyncSetDisconnectCallback(c, disconnectCallback);
    valkeyAsyncCommand(
        c, NULL, NULL, "SET key %b", argv[argc - 1], strlen(argv[argc - 1]));
    valkeyAsyncCommand(c, getCallback, (char *)"end-1", "GET key");
    event_base_dispatch(base);
    return 0;
}
