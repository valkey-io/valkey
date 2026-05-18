/*
 * This program connects to a cluster and then reads commands from stdin, such
 * as "SET foo bar", one per line and prints the results to stdout.
 *
 * The behaviour of the client can be altered by following action commands:
 *
 * !all    - Send each command to all nodes in the cluster.
 *           Will send following commands using the `..ToNode()` API and a
 *           cluster node iterator to send each command to all known nodes.
 *
 * Exit statuses this program can return:
 *   0 - Successful execution of program.
 *   1 - Bad arguments.
 *   2 - Client failed to get initial slotmap from given "HOST:PORT".
 */

#include "cluster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#include <winsock2.h> /* For struct timeval */
#endif

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

void eventCallback(const valkeyClusterContext *cc, int event, void *privdata) {
    (void)cc;
    (void)privdata;
    const char *e;
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

void connectCallback(const valkeyContext *c, int status) {
    const char *s = "";
    if (status != VALKEY_OK)
        s = "failed to ";
    printf("Event: %sconnect to %s:%d\n", s, c->tcp.host, c->tcp.port);
}

int main(int argc, char **argv) {
    int show_events = 0;
    int use_cluster_nodes = 0;
    int send_to_all = 0;
    int show_connection_events = 0;
    int select_db = 0;

    int argindex;
    for (argindex = 1; argindex < argc && argv[argindex][0] == '-';
         argindex++) {
        if (strcmp(argv[argindex], "--events") == 0) {
            show_events = 1;
        } else if (strcmp(argv[argindex], "--connection-events") == 0) {
            show_connection_events = 1;
        } else if (strcmp(argv[argindex], "--use-cluster-nodes") == 0) {
            use_cluster_nodes = 1;
        } else if (strcmp(argv[argindex], "--select-db") == 0) {
            if (++argindex < argc) /* Need an additional argument */
                select_db = atoi(argv[argindex]);
            if (select_db == 0) {
                fprintf(stderr, "Missing or faulty argument for --select-db\n");
                exit(1);
            }
        } else {
            fprintf(stderr, "Unknown argument: '%s'\n", argv[argindex]);
            exit(1);
        }
    }

    if (argindex >= argc) {
        fprintf(stderr, "Usage: clusterclient [--events] [--connection-events] "
                        "[--use-cluster-nodes] [--select-db NUM] HOST:PORT\n");
        exit(1);
    }
    const char *initnode = argv[argindex];

    struct timeval timeout = {1, 500000}; // 1.5s

    valkeyClusterOptions options = {0};
    options.initial_nodes = initnode;
    options.connect_timeout = &timeout;
    if (use_cluster_nodes) {
        options.options = VALKEY_OPT_USE_CLUSTER_NODES;
    }
    if (show_events) {
        options.event_callback = eventCallback;
    }
    if (show_connection_events) {
        options.connect_callback = connectCallback;
    }
    if (select_db > 0) {
        options.select_db = select_db;
    }

    valkeyClusterContext *cc = valkeyClusterConnectWithOptions(&options);
    if (cc == NULL || cc->err) {
        printf("Connect error: %s\n", cc ? cc->errstr : "OOM");
        exit(2);
    }

    char command[256];
    while (fgets(command, 256, stdin)) {
        size_t len = strlen(command);
        if (command[len - 1] == '\n') // Chop trailing line break
            command[len - 1] = '\0';

        if (command[0] == '\0') /* Skip empty lines */
            continue;
        if (command[0] == '#') /* Skip comments */
            continue;
        if (command[0] == '!') {
            if (strcmp(command, "!all") == 0) /* Enable send to all nodes */
                send_to_all = 1;
            continue;
        }

        if (send_to_all) {
            valkeyClusterNodeIterator ni;
            valkeyClusterInitNodeIterator(&ni, cc);
            uint64_t route_version = cc->route_version;

            valkeyClusterNode *node;
            while ((node = valkeyClusterNodeNext(&ni)) != NULL) {
                valkeyReply *reply =
                    valkeyClusterCommandToNode(cc, node, command);
                if (!reply || cc->err) {
                    printf("error: %s\n", cc->errstr);
                } else {
                    printReply(reply);
                }
                freeReplyObject(reply);
                if (route_version != cc->route_version) {
                    /* Updated slotmap resets the iterator. Abort iteration. */
                    break;
                }
            }
        } else {
            valkeyReply *reply = valkeyClusterCommand(cc, command);
            if (!reply || cc->err) {
                printf("error: %s\n", cc->errstr);
            } else {
                printReply(reply);
            }
            freeReplyObject(reply);
        }
    }

    valkeyClusterFree(cc);
    return 0;
}
