#include "cluster.h"
#include "test_utils.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLUSTER_NODE_IPV6 "::1:7200"

// Successful connection an IPv6 cluster
void test_successful_ipv6_connection(void) {
    struct timeval timeout = {0, 500000}; // 0.5s

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE_IPV6;
    options.connect_timeout = &timeout;

    valkeyClusterContext *cc = valkeyClusterConnectWithOptions(&options);
    ASSERT_MSG(cc && cc->err == 0, cc ? cc->errstr : "OOM");

    valkeyReply *reply;
    reply = (valkeyReply *)valkeyClusterCommand(cc, "SET key_ipv6 value");
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    reply = (valkeyReply *)valkeyClusterCommand(cc, "GET key_ipv6");
    CHECK_REPLY_STR(cc, reply, "value");
    freeReplyObject(reply);

    valkeyClusterFree(cc);
}

int main(void) {

    test_successful_ipv6_connection();

    return 0;
}
