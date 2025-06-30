#include <valkey/async.h>
#include <valkey/tls.h>
#include <valkey/valkey.h>

#include <valkey/adapters/libevent.h>

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

    struct event_base *base = event_base_new();
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s <key> <host> <port> <cert> <certKey> [ca]\n", argv[0]);
        exit(1);
    }

    const char *value = argv[1];
    size_t nvalue = strlen(value);

    const char *hostname = argv[2];
    int port = atoi(argv[3]);

    const char *cert = argv[4];
    const char *certKey = argv[5];
    const char *caCert = argc > 5 ? argv[6] : NULL;

    valkeyTLSContext *tls;
    valkeyTLSContextError tls_error = VALKEY_TLS_CTX_NONE;

    valkeyInitOpenSSL();

    tls = valkeyCreateTLSContext(caCert, NULL,
                                 cert, certKey, NULL, &tls_error);
    if (!tls) {
        printf("Error: %s\n", valkeyTLSContextGetError(tls_error));
        return 1;
    }

    valkeyAsyncContext *c = valkeyAsyncConnect(hostname, port);
    if (c->err) {
        /* Let *c leak for now... */
        printf("Error: %s\n", c->errstr);
        return 1;
    }
    if (valkeyInitiateTLSWithContext(&c->c, tls) != VALKEY_OK) {
        printf("TLS Error!\n");
        exit(1);
    }

    valkeyLibeventAttach(c, base);
    valkeyAsyncSetConnectCallback(c, connectCallback);
    valkeyAsyncSetDisconnectCallback(c, disconnectCallback);
    valkeyAsyncCommand(c, NULL, NULL, "SET key %b", value, nvalue);
    valkeyAsyncCommand(c, getCallback, (char *)"end-1", "GET key");
    event_base_dispatch(base);

    valkeyFreeTLSContext(tls);
    return 0;
}
