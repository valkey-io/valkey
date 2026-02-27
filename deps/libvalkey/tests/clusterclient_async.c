/*
 * This program connects to a cluster and then reads commands from stdin, such
 * as "SET foo bar", one per line and prints the results to stdout.
 *
 * The behaviour is the same as that of clusterclient.c, but the asynchronous
 * API of the library is used rather than the synchronous API.
 * The following action commands can alter the default behaviour:
 *
 * !async  - Send multiple commands and then wait for their responses.
 *           Will send all following commands until EOF or the command `!sync`
 *
 * !sync   - Send a single command and wait for its response before sending next
 *           command. This is the default behaviour.
 *
 * !resend - Resend a failed command from its reply callback.
 *           Will resend all following failed commands until EOF.
 *
 * !sleep  - Sleep a second. Can be used to allow timers to timeout.
 *           Currently not supported while in !async mode.
 *
 * !all    - Send each command to all nodes in the cluster.
 *           Will send following commands using the `..ToNode()` API and a
 *           cluster node iterator to send each command to all known nodes.
 *
 * !disconnect - Disconnect the client.
 *
 * An example input of first sending 2 commands and waiting for their responses,
 * before sending a single command and waiting for its response:
 *
 * !async
 * SET dual-1 command
 * SET dual-2 command
 * !sync
 * SET single command
 *
 */

#include "adapters/libevent.h"
#include "cluster.h"
#include "test_utils.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#include <winsock2.h> /* For struct timeval */
#endif

#define CMD_SIZE 256
#define HISTORY_DEPTH 16

char cmd_history[HISTORY_DEPTH][CMD_SIZE];

int num_running = 0;
int resend_failed_cmd = 0;
int send_to_all = 0;
int show_events = 0;
int blocking_initial_update = 0;

void sendNextCommand(evutil_socket_t, short, void *);

void printReply(const valkeyReply *reply) {
    switch (reply->type) {
    case VALKEY_REPLY_ERROR:
    case VALKEY_REPLY_STATUS:
    case VALKEY_REPLY_STRING:
    case VALKEY_REPLY_VERB:
    case VALKEY_REPLY_BIGNUM:
        printf("%s\n", reply->str);
        break;
    case VALKEY_REPLY_INTEGER:
        printf("%lld\n", reply->integer);
        break;
    default:
        printf("Unhandled reply type: %d\n", reply->type);
    }
}

void replyCallback(valkeyClusterAsyncContext *acc, void *r, void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    intptr_t cmd_id = (intptr_t)privdata; /* ID for corresponding cmd */

    if (reply == NULL) {
        if (acc->err) {
            printf("error: %s\n", acc->errstr);
        } else {
            printf("unknown error\n");
        }

        if (resend_failed_cmd) {
            printf("resend '%s'\n", cmd_history[cmd_id]);
            if (valkeyClusterAsyncCommand(acc, replyCallback, (void *)cmd_id,
                                          cmd_history[cmd_id]) != VALKEY_OK)
                printf("send error\n");
            return;
        }
    } else {
        printReply(reply);
    }

    if (--num_running == 0) {
        /* Schedule a read from stdin and send next command */
        struct timeval timeout = {0, 10};
        struct event_base *base = acc->attach_data;
        event_base_once(base, -1, EV_TIMEOUT, sendNextCommand, acc, &timeout);
    }
}

void sendNextCommand(evutil_socket_t fd, short kind, void *arg) {
    UNUSED(fd);
    UNUSED(kind);
    valkeyClusterAsyncContext *acc = arg;
    int async = 0;

    char cmd[CMD_SIZE];
    while (fgets(cmd, CMD_SIZE, stdin)) {
        size_t len = strlen(cmd);
        if (cmd[len - 1] == '\n') /* Chop trailing line break */
            cmd[len - 1] = '\0';

        if (cmd[0] == '\0') /* Skip empty lines */
            continue;
        if (cmd[0] == '#') /* Skip comments */
            continue;
        if (cmd[0] == '!') {
            if (strcmp(cmd, "!sleep") == 0) {
                ASSERT_MSG(async == 0, "!sleep in !async not supported");
                struct timeval timeout = {1, 0};
                struct event_base *base = acc->attach_data;
                event_base_once(base, -1, EV_TIMEOUT, sendNextCommand, acc, &timeout);
                return;
            }
            if (strcmp(cmd, "!async") == 0) /* Enable async send */
                async = 1;
            if (strcmp(cmd, "!sync") == 0) { /* Disable async send */
                if (async)
                    return; /* We are done sending commands */
            }
            if (strcmp(cmd, "!resend") == 0) /* Enable resend of failed cmd */
                resend_failed_cmd = 1;
            if (strcmp(cmd, "!all") == 0) { /* Enable send to all nodes */
                ASSERT_MSG(resend_failed_cmd == 0,
                           "!all in !resend not supported");
                send_to_all = 1;
            }
            if (strcmp(cmd, "!disconnect") == 0)
                valkeyClusterAsyncDisconnect(acc);
            continue; /* Skip line */
        }

        /* Copy command string to history buffer */
        assert(num_running < HISTORY_DEPTH);
        strcpy(cmd_history[num_running], cmd);

        if (send_to_all) {
            valkeyClusterNodeIterator ni;
            valkeyClusterInitNodeIterator(&ni, &acc->cc);

            valkeyClusterNode *node;
            while ((node = valkeyClusterNodeNext(&ni)) != NULL) {
                int status = valkeyClusterAsyncCommandToNode(
                    acc, node, replyCallback, (void *)((intptr_t)num_running),
                    cmd);
                ASSERT_MSG(status == VALKEY_OK, acc->errstr);
                num_running++;
            }
        } else {
            int status = valkeyClusterAsyncCommand(
                acc, replyCallback, (void *)((intptr_t)num_running), cmd);
            if (status == VALKEY_OK) {
                num_running++;
            } else {
                printf("error: %s\n", acc->errstr);

                /* Schedule a read from stdin and handle next command. */
                struct timeval timeout = {0, 10};
                struct event_base *base = acc->attach_data;
                event_base_once(base, -1, EV_TIMEOUT, sendNextCommand, acc, &timeout);
            }
        }

        if (async)
            continue; /* Send next command as well */

        return;
    }

    /* Disconnect if nothing is left to read from stdin */
    valkeyClusterAsyncDisconnect(acc);
}

void eventCallback(const valkeyClusterContext *cc, int event, void *privdata) {
    (void)cc;
    (void)privdata;
    if (event == VALKEYCLUSTER_EVENT_READY) {
        /* Schedule a read from stdin and send next command.
         * Get the async context by a simple cast since in the Async API a
         * valkeyClusterAsyncContext is an extension of the valkeyClusterContext. */
        valkeyClusterAsyncContext *acc = (valkeyClusterAsyncContext *)cc;
        struct timeval timeout = {0, 10};
        struct event_base *base = acc->attach_data;
        event_base_once(base, -1, EV_TIMEOUT, sendNextCommand, acc, &timeout);
    }

    if (!show_events)
        return;

    const char *e = NULL;
    switch (event) {
    case VALKEYCLUSTER_EVENT_SLOTMAP_UPDATED:
        e = "slotmap-updated";
        break;
    case VALKEYCLUSTER_EVENT_READY:
        e = "ready";
        break;
    case VALKEYCLUSTER_EVENT_FREE_CONTEXT:
        e = "free-context";
        break;
    default:
        e = "unknown";
    }
    printf("Event: %s\n", e);
}

void connectCallback(valkeyAsyncContext *ac, int status) {
    const char *s = "";
    if (status != VALKEY_OK)
        s = "failed to ";
    printf("Event: %sconnect to %s:%d\n", s, ac->c.tcp.host, ac->c.tcp.port);
}

void disconnectCallback(const valkeyAsyncContext *ac, int status) {
    const char *s = "";
    if (status != VALKEY_OK)
        s = "failed to ";
    printf("Event: %sdisconnect from %s:%d\n", s, ac->c.tcp.host,
           ac->c.tcp.port);
}

int main(int argc, char **argv) {
    int use_cluster_nodes = 0;
    int show_connection_events = 0;
    int select_db = 0;

    int optind;
    for (optind = 1; optind < argc && argv[optind][0] == '-'; optind++) {
        if (strcmp(argv[optind], "--use-cluster-nodes") == 0) {
            use_cluster_nodes = 1;
        } else if (strcmp(argv[optind], "--events") == 0) {
            show_events = 1;
        } else if (strcmp(argv[optind], "--connection-events") == 0) {
            show_connection_events = 1;
        } else if (strcmp(argv[optind], "--blocking-initial-update") == 0) {
            blocking_initial_update = 1;
        } else if (strcmp(argv[optind], "--select-db") == 0) {
            if (++optind < argc) /* Need an additional argument */
                select_db = atoi(argv[optind]);
            if (select_db == 0) {
                fprintf(stderr, "Missing or faulty argument for --select-db\n");
                exit(1);
            }
        } else {
            fprintf(stderr, "Unknown argument: '%s'\n", argv[optind]);
        }
    }

    if (optind >= argc) {
        fprintf(stderr,
                "Usage: clusterclient_async [--events] [--connection-events] "
                "[--use-cluster-nodes] [--select-db NUM] HOST:PORT\n");
        exit(1);
    }
    const char *initnode = argv[optind];
    struct timeval timeout = {0, 500000};
    struct event_base *base = event_base_new();

    valkeyClusterOptions options = {0};
    options.initial_nodes = initnode;
    options.connect_timeout = &timeout;
    options.command_timeout = &timeout;
    options.event_callback = eventCallback;
    options.max_retry = 1;
    if (blocking_initial_update) {
        options.options = VALKEY_OPT_BLOCKING_INITIAL_UPDATE;
    }
    if (use_cluster_nodes) {
        options.options |= VALKEY_OPT_USE_CLUSTER_NODES;
    }
    if (show_connection_events) {
        options.async_connect_callback = connectCallback;
        options.async_disconnect_callback = disconnectCallback;
    }
    if (select_db > 0) {
        options.select_db = select_db;
    }
    valkeyClusterOptionsUseLibevent(&options, base);

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    if (acc == NULL || acc->err != 0) {
        printf("Connect error: %s\n", acc ? acc->errstr : "OOM");
        exit(2);
    }

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
    return 0;
}
