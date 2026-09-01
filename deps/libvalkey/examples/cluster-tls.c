#include <valkey/cluster.h>
#include <valkey/tls.h>

#include <stdio.h>
#include <stdlib.h>

#define CLUSTER_NODE_TLS "127.0.0.1:7301"

int main(void) {
    valkeyTLSContext *tls;
    valkeyTLSContextError tls_error;

    valkeyInitOpenSSL();
    tls = valkeyCreateTLSContext("ca.crt", NULL, "client.crt", "client.key",
                                 NULL, &tls_error);
    if (!tls) {
        printf("TLS Context error: %s\n", valkeyTLSContextGetError(tls_error));
        exit(1);
    }

    struct timeval timeout = {1, 500000}; // 1.5s

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE_TLS;
    options.connect_timeout = &timeout;
    options.tls = tls;
    options.tls_init_fn = &valkeyInitiateTLSWithContext;

    valkeyClusterContext *cc = valkeyClusterConnectWithOptions(&options);
    if (!cc) {
        printf("Error: Allocation failure\n");
        exit(-1);
    } else if (cc->err) {
        printf("Error: %s\n", cc->errstr);
        // handle error
        exit(-1);
    }

    valkeyReply *reply = valkeyClusterCommand(cc, "SET %s %s", "key", "value");
    if (!reply) {
        printf("Reply missing: %s\n", cc->errstr);
        exit(-1);
    }
    printf("SET: %s\n", reply->str);
    freeReplyObject(reply);

    valkeyReply *reply2 = valkeyClusterCommand(cc, "GET %s", "key");
    if (!reply2) {
        printf("Reply missing: %s\n", cc->errstr);
        exit(-1);
    }
    printf("GET: %s\n", reply2->str);
    freeReplyObject(reply2);

    valkeyClusterFree(cc);
    valkeyFreeTLSContext(tls);
    return 0;
}
