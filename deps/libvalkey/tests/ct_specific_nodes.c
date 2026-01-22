#define _XOPEN_SOURCE 600 /* For strdup() */
#include "adapters/libevent.h"
#include "cluster.h"
#include "test_utils.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLUSTER_NODE "127.0.0.1:7000"

void test_command_to_single_node(valkeyClusterContext *cc) {
    valkeyReply *reply;

    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, cc);
    valkeyClusterNode *node = valkeyClusterNodeNext(&ni);
    assert(node);

    reply = valkeyClusterCommandToNode(cc, node, "DBSIZE");
    CHECK_REPLY(cc, reply);
    CHECK_REPLY_TYPE(reply, VALKEY_REPLY_INTEGER);
    freeReplyObject(reply);
}

void test_command_to_all_nodes(valkeyClusterContext *cc) {

    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, cc);

    valkeyClusterNode *node;
    while ((node = valkeyClusterNodeNext(&ni)) != NULL) {

        valkeyReply *reply;
        reply = valkeyClusterCommandToNode(cc, node, "DBSIZE");
        CHECK_REPLY(cc, reply);
        CHECK_REPLY_TYPE(reply, VALKEY_REPLY_INTEGER);
        freeReplyObject(reply);
    }
}

void test_transaction(valkeyClusterContext *cc) {

    valkeyClusterNode *node = valkeyClusterGetNodeByKey(cc, (char *)"foo");
    assert(node);

    valkeyReply *reply;
    reply = valkeyClusterCommandToNode(cc, node, "MULTI");
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    reply = valkeyClusterCommandToNode(cc, node, "SET foo 99");
    CHECK_REPLY_QUEUED(cc, reply);
    freeReplyObject(reply);

    reply = valkeyClusterCommandToNode(cc, node, "INCR foo");
    CHECK_REPLY_QUEUED(cc, reply);
    freeReplyObject(reply);

    reply = valkeyClusterCommandToNode(cc, node, "EXEC");
    CHECK_REPLY_ARRAY(cc, reply, 2);
    CHECK_REPLY_OK(cc, reply->element[0]);
    CHECK_REPLY_INT(cc, reply->element[1], 100);
    freeReplyObject(reply);
}

void test_streams(valkeyClusterContext *cc) {
    valkeyReply *reply;
    char *id;

    /* Get the node that handles given stream */
    valkeyClusterNode *node = valkeyClusterGetNodeByKey(cc, (char *)"mystream");
    assert(node);

    /* Preparation: remove old stream/key */
    reply = valkeyClusterCommandToNode(cc, node, "DEL mystream");
    CHECK_REPLY_TYPE(reply, VALKEY_REPLY_INTEGER);
    freeReplyObject(reply);

    /* Query wrong node */
    valkeyClusterNode *wrongNode = valkeyClusterGetNodeByKey(cc, (char *)"otherstream");
    assert(node != wrongNode);
    reply = valkeyClusterCommandToNode(cc, wrongNode, "XLEN mystream");
    CHECK_REPLY_ERROR(cc, reply, "MOVED");
    freeReplyObject(reply);

    /* Verify stream length before adding entries */
    reply = valkeyClusterCommandToNode(cc, node, "XLEN mystream");
    CHECK_REPLY_INT(cc, reply, 0);
    freeReplyObject(reply);

    /* Add entries to a created stream */
    reply = valkeyClusterCommandToNode(cc, node, "XADD mystream * name t800");
    CHECK_REPLY_TYPE(reply, VALKEY_REPLY_STRING);
    freeReplyObject(reply);

    reply = valkeyClusterCommandToNode(
        cc, node, "XADD mystream * name Sara surname OConnor");
    CHECK_REPLY_TYPE(reply, VALKEY_REPLY_STRING);
    id = strdup(reply->str); /* Keep this id for later inspections */
    freeReplyObject(reply);

    /* Verify stream length after adding entries */
    reply = valkeyClusterCommandToNode(cc, node, "XLEN mystream");
    CHECK_REPLY_INT(cc, reply, 2);
    freeReplyObject(reply);

    /* Modify the stream */
    reply = valkeyClusterCommandToNode(cc, node, "XTRIM mystream MAXLEN 1");
    CHECK_REPLY_INT(cc, reply, 1);
    freeReplyObject(reply);

    /* Verify stream length after modifying the stream */
    reply = valkeyClusterCommandToNode(cc, node, "XLEN mystream");
    CHECK_REPLY_INT(cc, reply, 1); /* 1 entry left */
    freeReplyObject(reply);

    /* Read from the stream */
    reply = valkeyClusterCommandToNode(cc, node,
                                       "XREAD COUNT 2 STREAMS mystream 0");
    CHECK_REPLY_ARRAY(cc, reply, 1); /* Reply from a single stream */

    /* Verify the reply from stream */
    CHECK_REPLY_ARRAY(cc, reply->element[0], 2);
    CHECK_REPLY_STR(cc, reply->element[0]->element[0], "mystream");
    CHECK_REPLY_ARRAY(cc, reply->element[0]->element[1], 1); /* single entry */

    /* Verify the entry, an array of id and field+value elements */
    valkeyReply *entry = reply->element[0]->element[1]->element[0];
    CHECK_REPLY_ARRAY(cc, entry, 2);
    CHECK_REPLY_STR(cc, entry->element[0], id);

    CHECK_REPLY_ARRAY(cc, entry->element[1], 4);
    CHECK_REPLY_STR(cc, entry->element[1]->element[0], "name");
    CHECK_REPLY_STR(cc, entry->element[1]->element[1], "Sara");
    CHECK_REPLY_STR(cc, entry->element[1]->element[2], "surname");
    CHECK_REPLY_STR(cc, entry->element[1]->element[3], "OConnor");
    freeReplyObject(reply);

    /* Delete the entry in stream */
    reply = valkeyClusterCommandToNode(cc, node, "XDEL mystream %s", id);
    CHECK_REPLY_INT(cc, reply, 1);
    freeReplyObject(reply);

    /* Blocking read of stream */
    reply = valkeyClusterCommandToNode(
        cc, node, "XREAD COUNT 2 BLOCK 200 STREAMS mystream 0");
    CHECK_REPLY_NIL(cc, reply);
    freeReplyObject(reply);

    /* Create a consumer group */
    reply = valkeyClusterCommandToNode(cc, node,
                                       "XGROUP CREATE mystream mygroup1 0");
    CHECK_REPLY_OK(cc, reply);
    freeReplyObject(reply);

    if (!valkey_version_less_than(6, 2)) {
        /* Create a consumer */
        reply = valkeyClusterCommandToNode(
            cc, node, "XGROUP CREATECONSUMER mystream mygroup1 myconsumer123");
        CHECK_REPLY_INT(cc, reply, 1);
        freeReplyObject(reply);
    }

    /* Blocking read of consumer group */
    reply =
        valkeyClusterCommandToNode(cc, node,
                                   "XREADGROUP GROUP mygroup1 myconsumer123 "
                                   "COUNT 2 BLOCK 200 STREAMS mystream 0");
    CHECK_REPLY_TYPE(reply, VALKEY_REPLY_ARRAY);
    freeReplyObject(reply);

    free(id);
}

void test_pipeline_to_single_node(valkeyClusterContext *cc) {
    int status;
    valkeyReply *reply;

    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, cc);
    valkeyClusterNode *node = valkeyClusterNodeNext(&ni);
    assert(node);

    status = valkeyClusterAppendCommandToNode(cc, node, "DBSIZE");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    // Trigger send of pipeline commands
    valkeyClusterGetReply(cc, (void *)&reply);
    CHECK_REPLY(cc, reply);
    CHECK_REPLY_TYPE(reply, VALKEY_REPLY_INTEGER);
    freeReplyObject(reply);
}

void test_pipeline_to_all_nodes(valkeyClusterContext *cc) {

    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, cc);

    valkeyClusterNode *node;
    while ((node = valkeyClusterNodeNext(&ni)) != NULL) {
        int status = valkeyClusterAppendCommandToNode(cc, node, "DBSIZE");
        ASSERT_MSG(status == VALKEY_OK, cc->errstr);
    }

    // Get replies from 3 node cluster
    valkeyReply *reply;
    valkeyClusterGetReply(cc, (void *)&reply);
    CHECK_REPLY(cc, reply);
    CHECK_REPLY_TYPE(reply, VALKEY_REPLY_INTEGER);
    freeReplyObject(reply);

    valkeyClusterGetReply(cc, (void *)&reply);
    CHECK_REPLY(cc, reply);
    CHECK_REPLY_TYPE(reply, VALKEY_REPLY_INTEGER);
    freeReplyObject(reply);

    valkeyClusterGetReply(cc, (void *)&reply);
    CHECK_REPLY(cc, reply);
    CHECK_REPLY_TYPE(reply, VALKEY_REPLY_INTEGER);
    freeReplyObject(reply);

    valkeyClusterGetReply(cc, (void *)&reply);
    assert(reply == NULL);
}

void test_pipeline_transaction(valkeyClusterContext *cc) {
    int status;
    valkeyReply *reply;

    valkeyClusterNode *node = valkeyClusterGetNodeByKey(cc, (char *)"foo");
    assert(node);

    status = valkeyClusterAppendCommandToNode(cc, node, "MULTI");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);
    status = valkeyClusterAppendCommandToNode(cc, node, "SET foo 199");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);
    status = valkeyClusterAppendCommandToNode(cc, node, "INCR foo");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);
    status = valkeyClusterAppendCommandToNode(cc, node, "EXEC");
    ASSERT_MSG(status == VALKEY_OK, cc->errstr);

    // Trigger send of pipeline commands
    {
        valkeyClusterGetReply(cc, (void *)&reply); // MULTI
        CHECK_REPLY_OK(cc, reply);
        freeReplyObject(reply);

        valkeyClusterGetReply(cc, (void *)&reply); // SET
        CHECK_REPLY_QUEUED(cc, reply);
        freeReplyObject(reply);

        valkeyClusterGetReply(cc, (void *)&reply); // INCR
        CHECK_REPLY_QUEUED(cc, reply);
        freeReplyObject(reply);

        valkeyClusterGetReply(cc, (void *)&reply); // EXEC
        CHECK_REPLY_ARRAY(cc, reply, 2);
        CHECK_REPLY_OK(cc, reply->element[0]);
        CHECK_REPLY_INT(cc, reply->element[1], 200);
        freeReplyObject(reply);
    }
}

//------------------------------------------------------------------------------
// Async API
//------------------------------------------------------------------------------
typedef struct ExpectedResult {
    int type;
    const char *str;
    size_t elements;
    bool disconnect;
    bool noreply;
    const char *errstr;
} ExpectedResult;

// Callback for Valkey connects and disconnects
void connectCallback(valkeyAsyncContext *ac, int status) {
    UNUSED(ac);
    assert(status == VALKEY_OK);
}
void disconnectCallback(const valkeyAsyncContext *ac, int status) {
    UNUSED(ac);
    assert(status == VALKEY_OK);
}

// Callback for async commands, verifies the valkeyReply
void commandCallback(valkeyClusterAsyncContext *cc, void *r, void *privdata) {
    valkeyReply *reply = (valkeyReply *)r;
    ExpectedResult *expect = (ExpectedResult *)privdata;
    if (expect->noreply) {
        assert(reply == NULL);
        assert(strcmp(cc->errstr, expect->errstr) == 0);
    } else {
        assert(reply != NULL);
        assert(reply->type == expect->type);
        switch (reply->type) {
        case VALKEY_REPLY_ARRAY:
            assert(reply->elements == expect->elements);
            assert(reply->str == NULL);
            break;
        case VALKEY_REPLY_INTEGER:
            assert(reply->elements == 0);
            assert(reply->str == NULL);
            break;
        case VALKEY_REPLY_STATUS:
            assert(strcmp(reply->str, expect->str) == 0);
            assert(reply->elements == 0);
            break;
        default:
            assert(0);
        }
    }
    if (expect->disconnect)
        valkeyClusterAsyncDisconnect(cc);
}

void test_async_to_single_node(void) {
    struct event_base *base = event_base_new();

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE;
    options.options = VALKEY_OPT_BLOCKING_INITIAL_UPDATE;
    options.max_retry = 1;
    options.async_connect_callback = connectCallback;
    options.async_disconnect_callback = disconnectCallback;
    valkeyClusterOptionsUseLibevent(&options, base);

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    ASSERT_MSG(acc && acc->err == 0, acc ? acc->errstr : "OOM");

    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, &acc->cc);
    valkeyClusterNode *node = valkeyClusterNodeNext(&ni);
    assert(node);

    int status;
    ExpectedResult r1 = {.type = VALKEY_REPLY_INTEGER, .disconnect = true};
    status = valkeyClusterAsyncCommandToNode(acc, node, commandCallback, &r1,
                                             "DBSIZE");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
}

void test_async_formatted_to_single_node(void) {
    struct event_base *base = event_base_new();

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE;
    options.options = VALKEY_OPT_BLOCKING_INITIAL_UPDATE;
    options.max_retry = 1;
    options.async_connect_callback = connectCallback;
    options.async_disconnect_callback = disconnectCallback;
    valkeyClusterOptionsUseLibevent(&options, base);

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    ASSERT_MSG(acc && acc->err == 0, acc ? acc->errstr : "OOM");

    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, &acc->cc);
    valkeyClusterNode *node = valkeyClusterNodeNext(&ni);
    assert(node);

    int status;
    ExpectedResult r1 = {.type = VALKEY_REPLY_INTEGER, .disconnect = true};
    char command[] = "*1\r\n$6\r\nDBSIZE\r\n";
    status = valkeyClusterAsyncFormattedCommandToNode(
        acc, node, commandCallback, &r1, command, strlen(command));
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
}

void test_async_command_argv_to_single_node(void) {
    struct event_base *base = event_base_new();

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE;
    options.options = VALKEY_OPT_BLOCKING_INITIAL_UPDATE;
    options.max_retry = 1;
    options.async_connect_callback = connectCallback;
    options.async_disconnect_callback = disconnectCallback;
    valkeyClusterOptionsUseLibevent(&options, base);

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    ASSERT_MSG(acc && acc->err == 0, acc ? acc->errstr : "OOM");

    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, &acc->cc);
    valkeyClusterNode *node = valkeyClusterNodeNext(&ni);
    assert(node);

    int status;
    ExpectedResult r1 = {.type = VALKEY_REPLY_INTEGER, .disconnect = true};
    status = valkeyClusterAsyncCommandArgvToNode(
        acc, node, commandCallback, &r1, 1, (const char *[]){"DBSIZE"},
        (size_t[]){6});
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
}

void test_async_to_all_nodes(void) {
    struct event_base *base = event_base_new();

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE;
    options.options = VALKEY_OPT_BLOCKING_INITIAL_UPDATE;
    options.max_retry = 1;
    options.async_connect_callback = connectCallback;
    options.async_disconnect_callback = disconnectCallback;
    valkeyClusterOptionsUseLibevent(&options, base);

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    ASSERT_MSG(acc && acc->err == 0, acc ? acc->errstr : "OOM");

    valkeyClusterNodeIterator ni;
    valkeyClusterInitNodeIterator(&ni, &acc->cc);

    int status;
    ExpectedResult r1 = {.type = VALKEY_REPLY_INTEGER};

    valkeyClusterNode *node;
    while ((node = valkeyClusterNodeNext(&ni)) != NULL) {

        status = valkeyClusterAsyncCommandToNode(acc, node, commandCallback,
                                                 &r1, "DBSIZE");
        ASSERT_MSG(status == VALKEY_OK, acc->errstr);
    }

    // Normal command to trigger disconnect
    ExpectedResult r2 = {
        .type = VALKEY_REPLY_STATUS, .str = "OK", .disconnect = true};
    status =
        valkeyClusterAsyncCommand(acc, commandCallback, &r2, "SET foo bar");

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
}

void test_async_transaction(void) {
    struct event_base *base = event_base_new();

    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE;
    options.options = VALKEY_OPT_BLOCKING_INITIAL_UPDATE;
    options.max_retry = 1;
    options.async_connect_callback = connectCallback;
    options.async_disconnect_callback = disconnectCallback;
    valkeyClusterOptionsUseLibevent(&options, base);

    valkeyClusterAsyncContext *acc = valkeyClusterAsyncConnectWithOptions(&options);
    ASSERT_MSG(acc && acc->err == 0, acc ? acc->errstr : "OOM");

    valkeyClusterNode *node = valkeyClusterGetNodeByKey(&acc->cc, (char *)"foo");
    assert(node);

    int status;
    ExpectedResult r1 = {.type = VALKEY_REPLY_STATUS, .str = "OK"};
    status = valkeyClusterAsyncCommandToNode(acc, node, commandCallback, &r1,
                                             "MULTI");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    ExpectedResult r2 = {.type = VALKEY_REPLY_STATUS, .str = "QUEUED"};
    status = valkeyClusterAsyncCommandToNode(acc, node, commandCallback, &r2,
                                             "SET foo 99");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    ExpectedResult r3 = {.type = VALKEY_REPLY_STATUS, .str = "QUEUED"};
    status = valkeyClusterAsyncCommandToNode(acc, node, commandCallback, &r3,
                                             "INCR foo");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    /* The EXEC command will return an array with result from 2 queued commands. */
    ExpectedResult r4 = {
        .type = VALKEY_REPLY_ARRAY, .elements = 2, .disconnect = true};
    status = valkeyClusterAsyncCommandToNode(acc, node, commandCallback, &r4,
                                             "EXEC ");
    ASSERT_MSG(status == VALKEY_OK, acc->errstr);

    event_base_dispatch(base);

    valkeyClusterAsyncFree(acc);
    event_base_free(base);
}

int main(void) {
    valkeyClusterOptions options = {0};
    options.initial_nodes = CLUSTER_NODE;
    options.max_retry = 1;

    valkeyClusterContext *cc = valkeyClusterConnectWithOptions(&options);
    ASSERT_MSG(cc && cc->err == 0, cc ? cc->errstr : "OOM");
    load_valkey_version(cc);

    // Synchronous API
    test_command_to_single_node(cc);
    test_command_to_all_nodes(cc);
    test_transaction(cc);
    test_streams(cc);

    // Pipeline API
    test_pipeline_to_single_node(cc);
    test_pipeline_to_all_nodes(cc);
    test_pipeline_transaction(cc);

    valkeyClusterFree(cc);

    // Asynchronous API
    test_async_to_single_node();
    test_async_formatted_to_single_node();
    test_async_command_argv_to_single_node();
    test_async_to_all_nodes();
    test_async_transaction();

    return 0;
}
