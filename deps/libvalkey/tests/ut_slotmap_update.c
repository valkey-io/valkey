/* Unit tests of internal functions that parses node and slot information
 * during slotmap updates. */

#ifndef __has_feature
#define __has_feature(feature) 0
#endif

/* Disable the 'One Definition Rule' check if running with address sanitizer
 * since we will include a sourcefile but also link to the library. */
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
const char *__asan_default_options(void) {
    return "detect_odr_violation=0";
}
#endif

/* Includes source files to test static functions. */
#include "cluster.c"
#include "valkey.c"

#include <stdbool.h>

valkeyReply *create_reply(const char *buf, size_t len);
char *resp_encode_array(char *p, sds *resp);

valkeyClusterContext *createClusterContext(const valkeyClusterOptions *options) {
    valkeyClusterContext *cc = vk_calloc(1, sizeof(valkeyClusterContext));
    assert(valkeyClusterContextInit(cc, options) == VALKEY_OK);
    return cc;
}

/* Helper to create a valkeyReply that contains a bulkstring. */
valkeyReply *create_cluster_nodes_reply(const char *str) {
    /* Create a RESP Bulk String. */
    char buf[1024];
    int len = sprintf(buf, "$%zu\r\n%s\r\n", strlen(str), str);

    return create_reply(buf, len);
}

/* Helper to create a valkeyReply that contains a verbatim string. */
valkeyReply *create_cluster_nodes_reply_resp3(const char *str) {
    char buf[1024];
    const char *encoding = "txt:";
    int len = sprintf(buf, "=%zu\r\n%s%s\r\n", strlen(encoding) + strlen(str), encoding, str);

    return create_reply(buf, len);
}

/* Helper to create a cluster slots response.
 * Parses the string using a rudimentary JSON like format which accepts:
 * - arrays   example: [elem1, elem2]
 * - strings  example: 'mystring'
 * - integers example: 123
 * - null     example: null
 * See resp_encode_array for details.
 */
valkeyReply *create_cluster_slots_reply(const char *str) {
    sds resp = sdsempty();

    char *s = strdup(str);
    resp_encode_array(s, &resp);
    free(s);

    valkeyReply *reply = create_reply(resp, sdslen(resp));
    sdsfree(resp);
    return reply;
}

/* Create a valkeyReply from a RESP encoded buffer. */
valkeyReply *create_reply(const char *buf, size_t len) {
    valkeyReply *reply;
    valkeyReader *reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, buf, len);
    assert(valkeyReaderGetReply(reader, (void **)&reply) == VALKEY_OK);
    valkeyReaderFree(reader);
    return reply;
}

/* Primitive parser of a JSON subset which is encoded to RESP.
 * The string `p` is parsed to a RESP encoded string and returned
 * by the pre-allocated sds string `resp`.
 * The function returns the next character to parse in p.
 * The parser accepts:
 * - arrays: [elem1, elem2]
 * - strings: 'mystring'
 * - integers: 123
 * - null values: null
 */
char *resp_encode_array(char *p, sds *resp) {
    int elements = 0;
    sds s = sdsempty();
    while (*p != '\0') {
        if (*p == '\'') {
            /* Parse and encode a string. */
            char *str = ++p; // Skip first ' and find next '
            while (*p != '\'' && *p != '\0')
                ++p;
            assert(*p != '\0'); /* Premature end of indata */
            *p = '\0';
            s = sdscatfmt(s, "$%i\r\n%s\r\n", strlen(str), str);
            ++p; /* Skip last ' */
            elements += 1;
        } else if (*p >= '0' && *p <= '9') {
            /* Parse and encode an integer. */
            char *start = p;
            while (*p >= '0' && *p <= '9')
                ++p;
            int integer = vk_atoi(start, (p - start));
            s = sdscatfmt(s, ":%i\r\n", integer);
            elements += 1;
        } else if (*p == '[') {
            /* Parse and encode an array in current array. */
            p = resp_encode_array(++p, &s);
            elements += 1;
        } else if (*p == ']') {
            /* Finalize the current array. */
            *resp = sdscatfmt(*resp, "*%i\r\n%s", elements, s);
            sdsfree(s);
            return ++p;
        } else if (*p == 'n') {
            /* Parse and encode a null bulk string as in RESP2 */
            if ((strlen(p) >= 4) && memcmp(p, "null", 4) == 0) {
                s = sdscat(s, "$-1\r\n");
                elements += 1;
                p += 4;
            } else {
                ++p; // ignore
            }
        } else {
            ++p; // ignore
        }
    }
    assert(elements == 1); /* Only accept a single top array */
    *resp = sdscat(*resp, s);
    sdsfree(s);
    return p;
}

/* Parse a cluster nodes reply from a basic deployment. */
void test_parse_cluster_nodes(bool parse_replicas) {
    valkeyClusterOptions options = {0};
    if (parse_replicas)
        options.options |= VALKEY_OPT_USE_REPLICAS;

    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyContext *c = valkeyContextInit();
    valkeyClusterNode *node;
    cluster_slot *slot;
    dictIterator di;

    valkeyReply *reply = create_cluster_nodes_reply(
        "07c37dfeb235213a872192d90877d0cd55635b91 127.0.0.1:30004@31004,hostname4 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238317239 4 connected\n"
        "67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1 127.0.0.1:30002@31002,hostname2 master - 0 1426238316232 2 connected 5461-10922\n"
        "292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f 127.0.0.1:30003@31003,hostname3 master - 0 1426238318243 3 connected 10923-16383\n"
        "6ec23923021cf3ffec47632106199cb7f496ce01 127.0.0.1:30005@31005,hostname5 slave 67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1 0 1426238316232 5 connected\n"
        "824fe116063bc5fcf9f4ffd895bc17aee7731ac3 127.0.0.1:30006@31006,hostname6 slave 292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f 0 1426238317741 6 connected\n"
        "e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 127.0.0.1:30001@31001,hostname1 myself,master - 0 0 1 connected 0-5460\n");
    dict *nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 3); /* 3 masters */
    dictInitIterator(&di, nodes);
    /* Verify node 1 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30001") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30001);
    assert(node->role == VALKEY_ROLE_PRIMARY);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 0);
    assert(slot->end == 5460);
    if (parse_replicas) {
        assert(listLength(node->replicas) == 1);
        node = listNodeValue(listFirst(node->replicas));
        assert(strcmp(node->name, "07c37dfeb235213a872192d90877d0cd55635b91") == 0);
        assert(node->role == VALKEY_ROLE_REPLICA);
    } else {
        assert(node->replicas == NULL);
    }
    /* Verify node 2 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30002") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30002);
    assert(node->role == VALKEY_ROLE_PRIMARY);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 5461);
    assert(slot->end == 10922);
    if (parse_replicas) {
        assert(listLength(node->replicas) == 1);
        node = listNodeValue(listFirst(node->replicas));
        assert(strcmp(node->name, "6ec23923021cf3ffec47632106199cb7f496ce01") == 0);
        assert(node->role == VALKEY_ROLE_REPLICA);
    } else {
        assert(node->replicas == NULL);
    }
    /* Verify node 3 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30003") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30003);
    assert(node->role == VALKEY_ROLE_PRIMARY);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 10923);
    assert(slot->end == 16383);
    if (parse_replicas) {
        assert(listLength(node->replicas) == 1);
        node = listNodeValue(listFirst(node->replicas));
        assert(strcmp(node->name, "824fe116063bc5fcf9f4ffd895bc17aee7731ac3") == 0);
        assert(node->role == VALKEY_ROLE_REPLICA);
    } else {
        assert(node->replicas == NULL);
    }

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

void test_parse_cluster_nodes_during_failover(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyContext *c = valkeyContextInit();
    valkeyClusterNode *node;
    cluster_slot *slot;
    dictIterator di;

    /* 10.10.10.122 crashed and 10.10.10.126 promoted to master. */
    valkeyReply *reply = create_cluster_nodes_reply(
        "184ada329264e994781412f3986c425a248f386e 10.10.10.126:7000@17000 master - 0 1625255654350 7 connected 5461-10922\n"
        "5cc0f693985913c553c6901e102ea3cb8d6678bd 10.10.10.122:7000@17000 master,fail - 1625255622147 1625255621143 2 disconnected\n"
        "22de56650b3714c1c42fc0d120f80c66c24d8795 10.10.10.123:7000@17000 master - 0 1625255654000 3 connected 10923-16383\n"
        "ad0f5210dda1736a1b5467cd6e797f011a192097 10.10.10.125:7000@17000 slave 4394d8eb03de1f524b56cb385f0eb9052ce65283 0 1625255656366 1 connected\n"
        "8675cd30fdd4efa088634e50fbd5c0675238a35e 10.10.10.124:7000@17000 slave 22de56650b3714c1c42fc0d120f80c66c24d8795 0 1625255655360 3 connected\n"
        "4394d8eb03de1f524b56cb385f0eb9052ce65283 10.10.10.121:7000@17000 myself,master - 0 1625255653000 1 connected 0-5460\n");
    dict *nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 4); /* 4 masters */
    dictInitIterator(&di, nodes);
    /* Verify node 1 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "5cc0f693985913c553c6901e102ea3cb8d6678bd") == 0);
    assert(strcmp(node->addr, "10.10.10.122:7000") == 0);
    assert(strcmp(node->host, "10.10.10.122") == 0);
    assert(node->port == 7000);
    assert(listLength(node->slots) == 0); /* No slots (fail flag). */
    /* Verify node 2 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "184ada329264e994781412f3986c425a248f386e") == 0);
    assert(strcmp(node->addr, "10.10.10.126:7000") == 0);
    assert(strcmp(node->host, "10.10.10.126") == 0);
    assert(node->port == 7000);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 5461);
    assert(slot->end == 10922);
    /* Verify node 3 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "22de56650b3714c1c42fc0d120f80c66c24d8795") == 0);
    assert(strcmp(node->addr, "10.10.10.123:7000") == 0);
    assert(strcmp(node->host, "10.10.10.123") == 0);
    assert(node->port == 7000);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 10923);
    assert(slot->end == 16383);
    /* Verify node 4 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "4394d8eb03de1f524b56cb385f0eb9052ce65283") == 0);
    assert(strcmp(node->addr, "10.10.10.121:7000") == 0);
    assert(strcmp(node->host, "10.10.10.121") == 0);
    assert(node->port == 7000);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 0);
    assert(slot->end == 5460);

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

/* Skip nodes with the `noaddr` flag. */
void test_parse_cluster_nodes_with_noaddr(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyContext *c = valkeyContextInit();
    valkeyClusterNode *node;
    dictIterator di;

    valkeyReply *reply = create_cluster_nodes_reply(
        "752d150249c157c7cb312b6b056517bbbecb42d2 :0@0 master,noaddr - 1658754833817 1658754833000 3 disconnected 5461-10922\n"
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.0:6379@16379 myself,master - 0 1658755601000 1 connected 0-5460\n"
        "87f785c4a51f58c06e4be55de8c112210a811db9 127.0.0.2:6379@16379 master - 0 1658755602418 3 connected 10923-16383\n");
    dict *nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 2); /* Only 2 primaries since `noaddr` nodes are skipped. */
    dictInitIterator(&di, nodes);
    /* Verify node 1 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.0:6379") == 0);
    /* Verify node 2 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.2:6379") == 0);

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

void test_parse_cluster_nodes_with_empty_ip(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyClusterNode *node;
    dictIterator di;

    /* Set the IP from which the response is received from. */
    valkeyContext *c = valkeyContextInit();
    c->tcp.host = strdup("127.0.0.99");

    valkeyReply *reply = create_cluster_nodes_reply(
        "752d150249c157c7cb312b6b056517bbbecb42d2 :6379@16379 myself,master - 0 0 0 connected 5461-10922\n"
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.1:6379@16379 master - 0 1658755601000 1 connected 0-5460\n"
        "87f785c4a51f58c06e4be55de8c112210a811db9 127.0.0.2:6379@16379 master - 0 1658755602418 3 connected 10923-16383\n");
    dict *nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 3);
    dictInitIterator(&di, nodes);
    /* Verify node 1 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.1:6379") == 0);
    /* Verify node 2 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.2:6379") == 0);
    /* Verify node 3 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.99:6379") == 0); /* Uses the IP from which the response was received from. */

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

/* Parse replies with additional importing and migrating information. */
void test_parse_cluster_nodes_with_special_slot_entries(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyContext *c = valkeyContextInit();
    valkeyClusterNode *node;
    cluster_slot *slot;
    dictIterator di;
    listIter li;

    /* The reply contains special slot entries with migrating slot and
     * importing slot information that will be ignored. */
    valkeyReply *reply = create_cluster_nodes_reply(
        "4394d8eb03de1f524b56cb385f0eb9052ce65283 10.10.10.121:7000@17000 myself,master - 0 1625255653000 1 connected 0 2-5460 [0->-e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca] [1-<-292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f]\n");
    dict *nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 1); /* 1 master */
    dictInitIterator(&di, nodes);
    /* Verify node. */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "4394d8eb03de1f524b56cb385f0eb9052ce65283") == 0);
    assert(strcmp(node->addr, "10.10.10.121:7000") == 0);
    assert(strcmp(node->host, "10.10.10.121") == 0);
    assert(node->port == 7000);
    /* Verify slots in node. */
    assert(listLength(node->slots) == 2); /* 2 slot ranges */
    listRewind(node->slots, &li);
    slot = listNodeValue(listNext(&li));
    assert(slot->start == 0);
    assert(slot->end == 0);
    slot = listNodeValue(listNext(&li));
    assert(slot->start == 2);
    assert(slot->end == 5460);

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

/* Parse a cluster nodes reply containing a primary with multiple replicas. */
void test_parse_cluster_nodes_with_multiple_replicas(void) {
    valkeyClusterOptions options = {0};
    options.options |= VALKEY_OPT_USE_REPLICAS;

    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyContext *c = valkeyContextInit();
    valkeyClusterNode *node;
    cluster_slot *slot;
    dictIterator di;
    listIter li;

    valkeyReply *reply = create_cluster_nodes_reply(
        "07c37dfeb235213a872192d90877d0cd55635b91 127.0.0.1:30004@31004,hostname4 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238317239 4 connected\n"
        "6ec23923021cf3ffec47632106199cb7f496ce01 127.0.0.1:30005@31005,hostname5 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238316232 5 connected\n"
        "824fe116063bc5fcf9f4ffd895bc17aee7731ac3 127.0.0.1:30006@31006,hostname6 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238317741 6 connected\n"
        "e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 127.0.0.1:30001@31001,hostname1 myself,master - 0 0 1 connected 0-16383\n"
        "67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1 127.0.0.1:30002@31002,hostname2 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238316232 2 connected\n"
        "292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f 127.0.0.1:30003@31003,hostname3 slave e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 0 1426238318243 3 connected\n");
    dict *nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);

    /* Verify master. */
    assert(nodes);
    assert(dictSize(nodes) == 1); /* 1 master */
    dictInitIterator(&di, nodes);
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30001") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30001);
    assert(node->role == VALKEY_ROLE_PRIMARY);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 0);
    assert(slot->end == 16383);

    /* Verify replicas. */
    assert(listLength(node->replicas) == 5);
    listRewind(node->replicas, &li);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->name, "07c37dfeb235213a872192d90877d0cd55635b91") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30004") == 0);
    assert(node->role == VALKEY_ROLE_REPLICA);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->name, "6ec23923021cf3ffec47632106199cb7f496ce01") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30005") == 0);
    assert(node->role == VALKEY_ROLE_REPLICA);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->name, "824fe116063bc5fcf9f4ffd895bc17aee7731ac3") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30006") == 0);
    assert(node->role == VALKEY_ROLE_REPLICA);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->name, "67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30002") == 0);
    assert(node->role == VALKEY_ROLE_REPLICA);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->name, "292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30003") == 0);
    assert(node->role == VALKEY_ROLE_REPLICA);

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

/* Give error when parsing erroneous data. */
void test_parse_cluster_nodes_with_parse_error(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyContext *c = valkeyContextInit();
    valkeyReply *reply;
    dict *nodes;

    /* Missing link-state (and slots). */
    reply = create_cluster_nodes_reply(
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.0:30001@31001 myself,master - 0 1658755601000 1 \n");
    nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);
    assert(nodes == NULL);
    assert(cc->err == VALKEY_ERR_OTHER);
    valkeyClusterClearError(cc);

    /* Missing port. */
    reply = create_cluster_nodes_reply(
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.0@31001 myself,master - 0 1658755601000 1 connected 0-5460\n");
    nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);
    assert(nodes == NULL);
    assert(cc->err == VALKEY_ERR_OTHER);
    valkeyClusterClearError(cc);

    /* Missing port and cport. */
    reply = create_cluster_nodes_reply(
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.0 myself,master - 0 1658755601000 1 connected 0-5460\n");
    nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);
    assert(nodes == NULL);
    assert(cc->err == VALKEY_ERR_OTHER);
    valkeyClusterClearError(cc);

    /* Invalid port. */
    reply = create_cluster_nodes_reply(
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.0:66000@67000 myself,master - 0 1658755601000 1 connected 0-5460\n");
    nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);
    assert(nodes == NULL);
    assert(cc->err == VALKEY_ERR_OTHER);
    valkeyClusterClearError(cc);

    valkeyFree(c);
    valkeyClusterFree(cc);
}

/* Redis pre-v4.0 returned node addresses without the clusterbus port,
 * i.e. `ip:port` instead of `ip:port@cport` */
void test_parse_cluster_nodes_with_legacy_format(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyContext *c = valkeyContextInit();
    valkeyClusterNode *node;
    dictIterator di;

    valkeyReply *reply = create_cluster_nodes_reply(
        "e839a12fbed631de867016f636d773e644562e72 127.0.0.0:6379 myself,master - 0 1658755601000 1 connected 0-5460\n"
        "752d150249c157c7cb312b6b056517bbbecb42d2 :0 master,noaddr - 1658754833817 1658754833000 3 disconnected 5461-10922\n");
    dict *nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 1); /* Only 1 primary since `noaddr` nodes are skipped. */
    dictInitIterator(&di, nodes);
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.0:6379") == 0);

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

/* Parse a cluster nodes reply when RESP3 is used,
 * i.e. the reply is a verbatim string. */
void test_parse_cluster_nodes_with_resp3(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyContext *c = valkeyContextInit();
    valkeyClusterNode *node;
    dictIterator di;

    valkeyReply *reply = create_cluster_nodes_reply_resp3(
        "67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1 127.0.0.1:30002@31002,hostname2 master - 0 1426238316232 2 connected 5461-10922\n"
        "292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f 127.0.0.1:30003@31003,hostname3 master - 0 1426238318243 3 connected 10923-16383\n"
        "e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca 127.0.0.1:30001@31001,hostname1 myself,master - 0 0 1 connected 0-5460\n");
    dict *nodes = parse_cluster_nodes(cc, c, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 3); /* 3 primaries */
    dictInitIterator(&di, nodes);
    /* Verify node 1 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30001") == 0);
    /* Verify node 2 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30002") == 0);
    /* Verify node 3 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->name, "292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f") == 0);
    assert(strcmp(node->addr, "127.0.0.1:30003") == 0);

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

/* Parse a cluster slots reply from a basic deployment. */
void test_parse_cluster_slots(bool parse_replicas) {
    valkeyClusterOptions options = {0};
    if (parse_replicas)
        options.options |= VALKEY_OPT_USE_REPLICAS;

    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyContext *c = valkeyContextInit();
    valkeyClusterNode *node;
    cluster_slot *slot;
    dictIterator di;

    valkeyReply *reply = create_cluster_slots_reply(
        "[[0, 5460, ['127.0.0.1', 30001, 'e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca', ['hostname', 'localhost']],"
        "           ['127.0.0.1', 30004, '07c37dfeb235213a872192d90877d0cd55635b91', ['hostname', 'localhost']]],"
        " [5461, 10922, ['127.0.0.1', 30002, '67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1', ['hostname', 'localhost']],"
        "               ['127.0.0.1', 30005, '6ec23923021cf3ffec47632106199cb7f496ce01', ['hostname', 'localhost']]],"
        " [10923, 16383, ['127.0.0.1', 30003, '292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f', ['hostname', 'localhost']]"
        "                ['127.0.0.1', 30006, '824fe116063bc5fcf9f4ffd895bc17aee7731ac3', ['hostname', 'localhost']]]]");

    dict *nodes = parse_cluster_slots(cc, c, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 3); /* 3 primaries */
    dictInitIterator(&di, nodes);
    /* Verify node 1 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.1:30001") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30001);
    assert(node->role == VALKEY_ROLE_PRIMARY);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 0);
    assert(slot->end == 5460);
    if (parse_replicas) {
        assert(listLength(node->replicas) == 1);
        node = listNodeValue(listFirst(node->replicas));
        assert(strcmp(node->addr, "127.0.0.1:30004") == 0);
        assert(node->role == VALKEY_ROLE_REPLICA);
    } else {
        assert(node->replicas == NULL);
    }
    /* Verify node 2 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.1:30002") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30002);
    assert(node->role == VALKEY_ROLE_PRIMARY);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 5461);
    assert(slot->end == 10922);
    if (parse_replicas) {
        assert(listLength(node->replicas) == 1);
        node = listNodeValue(listFirst(node->replicas));
        assert(strcmp(node->addr, "127.0.0.1:30005") == 0);
        assert(node->role == VALKEY_ROLE_REPLICA);
    } else {
        assert(node->replicas == NULL);
    }
    /* Verify node 3 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.1:30003") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30003);
    assert(node->role == VALKEY_ROLE_PRIMARY);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 10923);
    assert(slot->end == 16383);
    if (parse_replicas) {
        assert(listLength(node->replicas) == 1);
        node = listNodeValue(listFirst(node->replicas));
        assert(strcmp(node->addr, "127.0.0.1:30006") == 0);
        assert(node->role == VALKEY_ROLE_REPLICA);
    } else {
        assert(node->replicas == NULL);
    }

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

void test_parse_cluster_slots_with_empty_ip(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyClusterNode *node;
    dictIterator di;

    /* Set the IP from which the response is received from. */
    valkeyContext *c = valkeyContextInit();
    c->tcp.host = strdup("127.0.0.99");

    valkeyReply *reply = create_cluster_slots_reply(
        "[[0, 5460, ['', 6379, 'e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca']],"
        " [5461, 10922, ['127.0.0.1', 6379, '67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1']],"
        " [10923, 16383, ['127.0.0.2', 6379, '292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f']]]");

    dict *nodes = parse_cluster_slots(cc, c, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 3);
    dictInitIterator(&di, nodes);
    /* Verify node 1 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.1:6379") == 0);
    /* Verify node 2 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.2:6379") == 0);
    /* Verify node 3 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.99:6379") == 0); /* Uses the IP from which the response was received from. */

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

void test_parse_cluster_slots_with_null_ip(void) {
    valkeyClusterOptions options = {0};
    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyClusterNode *node;
    dictIterator di;

    /* Set the IP from which the response is received from. */
    valkeyContext *c = valkeyContextInit();
    c->tcp.host = strdup("127.0.0.99");

    valkeyReply *reply = create_cluster_slots_reply(
        "[[0, 5460, [null, 6379, 'e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca']],"
        " [5461, 10922, ['127.0.0.1', 6379, '67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1']],"
        " [10923, 16383, ['127.0.0.2', 6379, '292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f']]]");

    dict *nodes = parse_cluster_slots(cc, c, reply);
    freeReplyObject(reply);

    assert(nodes);
    assert(dictSize(nodes) == 3);
    dictInitIterator(&di, nodes);
    /* Verify node 1 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.1:6379") == 0);
    /* Verify node 2 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.2:6379") == 0);
    /* Verify node 3 */
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.99:6379") == 0); /* Uses the IP from which the response was received from. */

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

/* Parse a cluster slots reply containing a primary with multiple replicas. */
void test_parse_cluster_slots_with_multiple_replicas(void) {
    valkeyClusterOptions options = {0};
    options.options |= VALKEY_OPT_USE_REPLICAS;

    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyContext *c = valkeyContextInit();
    valkeyClusterNode *node;
    cluster_slot *slot;
    dictIterator di;
    listIter li;

    valkeyReply *reply = create_cluster_slots_reply(
        "[[0, 16383, ['127.0.0.1', 30001, 'e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca'],"
        "            ['127.0.0.1', 30004, '07c37dfeb235213a872192d90877d0cd55635b91'],"
        "            ['127.0.0.1', 30005, '67ed2db8d677e59ec4a4cefb06858cf2a1a89fa1'],"
        "            ['127.0.0.1', 30006, '292f8b365bb7edb5e285caf0b7e6ddc7265d2f4f'],"
        "            ['127.0.0.1', 30002, '6ec23923021cf3ffec47632106199cb7f496ce01'],"
        "            ['127.0.0.1', 30003, '824fe116063bc5fcf9f4ffd895bc17aee7731ac3']]]");

    dict *nodes = parse_cluster_slots(cc, c, reply);
    freeReplyObject(reply);

    /* Verify primary. */
    assert(nodes);
    assert(dictSize(nodes) == 1); /* 1 primary */
    dictInitIterator(&di, nodes);
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.1:30001") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30001);
    assert(node->role == VALKEY_ROLE_PRIMARY);
    assert(listLength(node->slots) == 1); /* 1 slot range */
    slot = listNodeValue(listFirst(node->slots));
    assert(slot->start == 0);
    assert(slot->end == 16383);

    /* Verify replicas. */
    assert(listLength(node->replicas) == 5);
    listRewind(node->replicas, &li);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->addr, "127.0.0.1:30004") == 0);
    assert(node->role == VALKEY_ROLE_REPLICA);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->addr, "127.0.0.1:30005") == 0);
    assert(node->role == VALKEY_ROLE_REPLICA);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->addr, "127.0.0.1:30006") == 0);
    assert(node->role == VALKEY_ROLE_REPLICA);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->addr, "127.0.0.1:30002") == 0);
    assert(node->role == VALKEY_ROLE_REPLICA);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->addr, "127.0.0.1:30003") == 0);
    assert(node->role == VALKEY_ROLE_REPLICA);

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

void test_parse_cluster_slots_with_noncontiguous_slots(void) {
    valkeyClusterOptions options = {0};
    options.options |= VALKEY_OPT_USE_REPLICAS;

    valkeyClusterContext *cc = createClusterContext(&options);
    valkeyContext *c = valkeyContextInit();
    valkeyClusterNode *node;
    cluster_slot *slot;
    dictIterator di;
    listIter li;

    valkeyReply *reply = create_cluster_slots_reply(
        "[[0, 0, ['127.0.0.1', 30001, 'e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca'],"
        "        ['127.0.0.1', 30004, '07c37dfeb235213a872192d90877d0cd55635b91']],"
        " [2, 2, ['127.0.0.1', 30001, 'e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca'],"
        "        ['127.0.0.1', 30004, '07c37dfeb235213a872192d90877d0cd55635b91']],"
        " [4, 5460, ['127.0.0.1', 30001, 'e7d1eecce10fd6bb5eb35b9f99a514335d9ba9ca'],"
        "           ['127.0.0.1', 30004, '07c37dfeb235213a872192d90877d0cd55635b91']]]");

    dict *nodes = parse_cluster_slots(cc, c, reply);
    freeReplyObject(reply);

    /* Verify primary. */
    assert(nodes);
    assert(dictSize(nodes) == 1); /* 1 primary */
    dictInitIterator(&di, nodes);
    node = dictGetVal(dictNext(&di));
    assert(strcmp(node->addr, "127.0.0.1:30001") == 0);
    assert(strcmp(node->host, "127.0.0.1") == 0);
    assert(node->port == 30001);
    assert(node->role == VALKEY_ROLE_PRIMARY);
    assert(listLength(node->slots) == 3); /* 3 slot range */
    listRewind(node->slots, &li);
    slot = listNodeValue(listNext(&li));
    assert(slot->start == 0);
    assert(slot->end == 0);
    slot = listNodeValue(listNext(&li));
    assert(slot->start == 2);
    assert(slot->end == 2);
    slot = listNodeValue(listNext(&li));
    assert(slot->start == 4);
    assert(slot->end == 5460);

    /* Verify replica. */
    assert(listLength(node->replicas) == 1);
    listRewind(node->replicas, &li);
    node = listNodeValue(listNext(&li));
    assert(strcmp(node->addr, "127.0.0.1:30004") == 0);
    assert(node->role == VALKEY_ROLE_REPLICA);

    dictRelease(nodes);
    valkeyFree(c);
    valkeyClusterFree(cc);
}

int main(void) {
    test_parse_cluster_nodes(false /* replicas not parsed */);
    test_parse_cluster_nodes(true /* replicas parsed */);
    test_parse_cluster_nodes_during_failover();
    test_parse_cluster_nodes_with_noaddr();
    test_parse_cluster_nodes_with_empty_ip();
    test_parse_cluster_nodes_with_special_slot_entries();
    test_parse_cluster_nodes_with_multiple_replicas();
    test_parse_cluster_nodes_with_parse_error();
    test_parse_cluster_nodes_with_legacy_format();
    test_parse_cluster_nodes_with_resp3();

    test_parse_cluster_slots(false /* replicas not parsed */);
    test_parse_cluster_slots(true /* replicas parsed */);
    test_parse_cluster_slots_with_empty_ip();
    test_parse_cluster_slots_with_null_ip();
    test_parse_cluster_slots_with_multiple_replicas();
    test_parse_cluster_slots_with_noncontiguous_slots();
    return 0;
}
