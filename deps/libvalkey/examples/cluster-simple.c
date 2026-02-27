#include <valkey/cluster.h>

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    struct timeval timeout = {1, 500000}; // 1.5s

    valkeyClusterOptions options = {0};
    options.initial_nodes = "127.0.0.1:7000";
    options.connect_timeout = &timeout;

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
    printf("SET: %s\n", reply->str);
    freeReplyObject(reply);

    valkeyReply *reply2 = valkeyClusterCommand(cc, "GET %s", "key");
    printf("GET: %s\n", reply2->str);
    freeReplyObject(reply2);

    valkeyClusterFree(cc);
    return 0;
}
