#include <valkey/async.h>
#include <valkey/valkey.h>

#include <valkey/adapters/ivykis.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void getCallback(valkeyAsyncContext *c, void *r, void *privdata) {
    valkeyReply *reply = r;
    if (reply == NULL)
        return;
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

    iv_init();

    valkeyAsyncContext *c = valkeyAsyncConnect("127.0.0.1", 6379);
    if (c->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", c->errstr);
        return 1;
    }

    valkeyIvykisAttach(c);
    valkeyAsyncSetConnectCallback(c, connectCallback);
    valkeyAsyncSetDisconnectCallback(c, disconnectCallback);
    valkeyAsyncCommand(
        c, NULL, NULL, "SET key %b", argv[argc - 1], strlen(argv[argc - 1]));
    valkeyAsyncCommand(c, getCallback, (char *)"end-1", "GET key");

    iv_main();

    iv_deinit();

    return 0;
}
