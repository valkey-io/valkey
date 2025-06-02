#define _XOPEN_SOURCE 600 /* For strdup() */
#include <valkey/valkey.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#include <winsock2.h> /* For struct timeval */
#endif

static void example_argv_command(valkeyContext *c, size_t n) {
    char **argv, tmp[42];
    size_t *argvlen;
    valkeyReply *reply;

    /* We're allocating two additional elements for command and key */
    argv = malloc(sizeof(*argv) * (2 + n));
    argvlen = malloc(sizeof(*argvlen) * (2 + n));

    /* First the command */
    argv[0] = (char *)"RPUSH";
    argvlen[0] = sizeof("RPUSH") - 1;

    /* Now our key */
    argv[1] = (char *)"argvlist";
    argvlen[1] = sizeof("argvlist") - 1;

    /* Now add the entries we wish to add to the list */
    for (size_t i = 2; i < (n + 2); i++) {
        argvlen[i] = snprintf(tmp, sizeof(tmp), "argv-element-%zu", i - 2);
        argv[i] = strdup(tmp);
    }

    /* Execute the command using valkeyCommandArgv.  We're sending the arguments with
     * two explicit arrays.  One for each argument's string, and the other for its
     * length. */
    reply = valkeyCommandArgv(
        c, n + 2, (const char **)argv, (const size_t *)argvlen);

    if (reply == NULL || c->err) {
        fprintf(stderr, "Error:  Couldn't execute valkeyCommandArgv\n");
        exit(1);
    }

    if (reply->type == VALKEY_REPLY_INTEGER) {
        printf("%s reply: %lld\n", argv[0], reply->integer);
    }

    freeReplyObject(reply);

    /* Clean up */
    for (size_t i = 2; i < (n + 2); i++) {
        free(argv[i]);
    }

    free(argv);
    free(argvlen);
}

int main(int argc, char **argv) {
    unsigned int j, isunix = 0;
    valkeyContext *c;
    valkeyReply *reply;
    const char *hostname = (argc > 1) ? argv[1] : "127.0.0.1";

    if (argc > 2) {
        if (*argv[2] == 'u' || *argv[2] == 'U') {
            isunix = 1;
            /* in this case, host is the path to the unix socket */
            printf("Will connect to unix socket @%s\n", hostname);
        }
    }

    int port = (argc > 2) ? atoi(argv[2]) : 6379;

    struct timeval timeout = {1, 500000}; // 1.5 seconds
    if (isunix) {
        c = valkeyConnectUnixWithTimeout(hostname, timeout);
    } else {
        c = valkeyConnectWithTimeout(hostname, port, timeout);
    }
    if (c == NULL || c->err) {
        if (c) {
            printf("Connection error: %s\n", c->errstr);
            valkeyFree(c);
        } else {
            printf("Connection error: can't allocate valkey context\n");
        }
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

    /* See function for an example of valkeyCommandArgv */
    example_argv_command(c, 10);

    /* Disconnects and frees the context */
    valkeyFree(c);

    return 0;
}
