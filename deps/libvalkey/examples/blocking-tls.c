#include <valkey/tls.h>
#include <valkey/valkey.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#include <winsock2.h> /* For struct timeval */
#endif

int main(int argc, char **argv) {
    unsigned int j;
    valkeyTLSContext *tls;
    valkeyTLSContextError tls_error = VALKEY_TLS_CTX_NONE;
    valkeyContext *c;
    valkeyReply *reply;
    if (argc < 4) {
        printf("Usage: %s <host> <port> <cert> <key> [ca]\n", argv[0]);
        exit(1);
    }
    const char *hostname = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = atoi(argv[2]);
    const char *cert = argv[3];
    const char *key = argv[4];
    const char *ca = argc > 4 ? argv[5] : NULL;

    valkeyInitOpenSSL();
    tls = valkeyCreateTLSContext(ca, NULL, cert, key, NULL, &tls_error);
    if (!tls || tls_error != VALKEY_TLS_CTX_NONE) {
        printf("TLS Context error: %s\n", valkeyTLSContextGetError(tls_error));
        exit(1);
    }

    struct timeval tv = {1, 500000}; // 1.5 seconds
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, hostname, port);
    options.connect_timeout = &tv;
    c = valkeyConnectWithOptions(&options);

    if (c == NULL || c->err) {
        if (c) {
            printf("Connection error: %s\n", c->errstr);
            valkeyFree(c);
        } else {
            printf("Connection error: can't allocate valkey context\n");
        }
        exit(1);
    }

    if (valkeyInitiateTLSWithContext(c, tls) != VALKEY_OK) {
        printf("Couldn't initialize TLS!\n");
        printf("Error: %s\n", c->errstr);
        valkeyFree(c);
        exit(1);
    }

    /* PING server */
    reply = valkeyCommand(c, "PING");
    printf("PING: %s\n", reply->str);
    freeReplyObject(reply);

    /* Set a key */
    reply = valkeyCommand(c, "SET %s %s", "foo", "hello world");
    printf("SET: %s\n", reply->str);
    freeReplyObject(reply);

    /* Set a key using binary safe API */
    reply = valkeyCommand(c, "SET %b %b", "bar", (size_t)3, "hello", (size_t)5);
    printf("SET (binary API): %s\n", reply->str);
    freeReplyObject(reply);

    /* Try a GET and two INCR */
    reply = valkeyCommand(c, "GET foo");
    printf("GET foo: %s\n", reply->str);
    freeReplyObject(reply);

    reply = valkeyCommand(c, "INCR counter");
    printf("INCR counter: %lld\n", reply->integer);
    freeReplyObject(reply);
    /* again ... */
    reply = valkeyCommand(c, "INCR counter");
    printf("INCR counter: %lld\n", reply->integer);
    freeReplyObject(reply);

    /* Create a list of numbers, from 0 to 9 */
    reply = valkeyCommand(c, "DEL mylist");
    freeReplyObject(reply);
    for (j = 0; j < 10; j++) {
        char buf[64];

        snprintf(buf, 64, "%u", j);
        reply = valkeyCommand(c, "LPUSH mylist element-%s", buf);
        freeReplyObject(reply);
    }

    /* Let's check what we have inside the list */
    reply = valkeyCommand(c, "LRANGE mylist 0 -1");
    if (reply->type == VALKEY_REPLY_ARRAY) {
        for (j = 0; j < reply->elements; j++) {
            printf("%u) %s\n", j, reply->element[j]->str);
        }
    }
    freeReplyObject(reply);

    /* Disconnects and frees the context */
    valkeyFree(c);

    valkeyFreeTLSContext(tls);

    return 0;
}
