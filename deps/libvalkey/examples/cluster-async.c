#include <valkey/cluster.h>

#include <valkey/adapters/libevent.h>

#include <stdio.h>
#include <stdlib.h>

void getCallback(valkeyClusterAsyncContext *cc, void *r, void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    if (reply == NULL) {
        if (cc->err) {
            printf("errstr: %s\n", cc->errstr);
        }
        return;
    }
    printf("privdata: %s reply: %s\n", (char *)privdata, reply->str);

    /* Disconnect after receiving the reply to GET */
    valkeyClusterAsyncDisconnect(cc);
}

void setCallback(valkeyClusterAsyncContext *cc, void *r, void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    if (reply == NULL) {
        if (cc->err) {
            printf("errstr: %s\n", cc->errstr);
        }
        return;
    }
    printf("privdata: %s reply: %s\n", (char *)privdata, reply->str);
}

void connectCallback(valkeyAsyncContext *ac, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", ac->errstr);
        return;
    }
    printf("Connected to %s:%d\n", ac->c.tcp.host, ac->c.tcp.port);
}

void disconnectCallback(const valkeyAsyncContext *ac, int status) {
    if (status != VALKEY_OK) {
        printf("Error: %s\n", ac->errstr);
        return;
    }
    printf("Disconnected from %s:%d\n", ac->c.tcp.host, ac->c.tcp.port);
}

int main(void) {
    struct event_base *base = event_base_new();

    valkeyClusterOptions options = {0};
    options.initial_nodes = "127.0.0.1:7000";
    options.async_connect_callback = connectCallback;
    options.async_disconnect_callback = disconnectCallback;
    valkeyClusterOptionsUseLibevent(&options, base);

    printf("Connecting...\n");
    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    if (!acc) {
        printf("Error: Allocation failure\n");
        exit(-1);
    } else if (acc->err) {
        printf("Error: %s\n", acc->errstr);
        // handle error
        exit(-1);
    }

    int status;
    status = valkeyClusterAsyncCommand(acc, setCallback, (char *)"THE_ID",
                                       "SET %s %s", "key", "value");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", acc->err, acc->errstr);
    }

    status = valkeyClusterAsyncCommand(acc, getCallback, (char *)"THE_ID",
                                       "GET %s", "key");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", acc->err, acc->errstr);
    }

    status = valkeyClusterAsyncCommand(acc, setCallback, (char *)"THE_ID",
                                       "SET %s %s", "key2", "value2");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", acc->err, acc->errstr);
    }

    status = valkeyClusterAsyncCommand(acc, getCallback, (char *)"THE_ID",
                                       "GET %s", "key2");
    if (status != VALKEY_OK) {
        printf("error: err=%d errstr=%s\n", acc->err, acc->errstr);
    }

    printf("Dispatch..\n");
    event_base_dispatch(base);

    printf("Done..\n");
    valkeyClusterAsyncFree(acc);
    event_base_free(base);
    return 0;
}
