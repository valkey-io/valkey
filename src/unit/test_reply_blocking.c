#include "../fmacros.h"
#include "../reply_blocking.h"
#include "test_help.h"
#include "../server.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>

void blockLastResponseIfExist(struct client *c, long long blocked_offset);

static void initReplyBlockingTestEnv(void) {
    static char test_logfile[] = "";
    if (server.logfile == NULL) {
        /* serverLogRaw dereferences server.logfile; use stdout for unit tests. */
        server.logfile = test_logfile;
    }
}

static clientReplyBlock *createReplyBlock(size_t used) {
    clientReplyBlock *block = zmalloc(sizeof(clientReplyBlock) + used);
    block->size = used;
    block->used = used;
    block->last_header = NULL;
    block->flag.buf_encoded = 0;
    return block;
}

/**
 * Test durableClientInit and durableClientReset functions
 * These functions initialize and reset client durability attributes
 */
int test_durableClientInitAndReset(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    initReplyBlockingTestEnv();

    // Create a mock client
    client *c = zcalloc(sizeof(client));
    c->clientDurabilityInfo.blocked_responses = NULL;
    c->clientDurabilityInfo.durable_blocked_client = 0;
    c->clientDurabilityInfo.current_command_repl_offset = 0;

    // Test initialization when durability is disabled
    server.durability.sync_replication_enabled = 0;
    durableClientInit(c);
    TEST_ASSERT(c->clientDurabilityInfo.blocked_responses == NULL);

    // Test initialization when durability is enabled
    server.durability.sync_replication_enabled = 1;
    durableClientInit(c);
    TEST_ASSERT(c->clientDurabilityInfo.blocked_responses != NULL);
    TEST_ASSERT(listLength(c->clientDurabilityInfo.blocked_responses) == 0);
    TEST_ASSERT(c->clientDurabilityInfo.offset.recorded == false);
    TEST_ASSERT(c->clientDurabilityInfo.offset.reply_block == NULL);
    TEST_ASSERT(c->clientDurabilityInfo.offset.byte_offset == 0);
    TEST_ASSERT(c->clientDurabilityInfo.current_command_repl_offset == -1);

    // Test reset
    durableClientReset(c);
    TEST_ASSERT(c->clientDurabilityInfo.blocked_responses == NULL);
    TEST_ASSERT(c->clientDurabilityInfo.offset.recorded == false);
    TEST_ASSERT(c->clientDurabilityInfo.current_command_repl_offset == -1);

    // Reset to default
    server.durability.sync_replication_enabled = 0;

    // Cleanup
    zfree(c);

    return 0;
}

/**
 * Test isDurabilityEnabled function
 */
int test_isDurabilityEnabled(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    // Test that durability is disabled by default
    server.durability.sync_replication_enabled = 0;
    TEST_ASSERT(isDurabilityEnabled() == 0);

    // Test that durability can be enabled
    server.durability.sync_replication_enabled = 1;
    TEST_ASSERT(isDurabilityEnabled() == 1);

    // Reset to default
    server.durability.sync_replication_enabled = 0;

    return 0;
}

/**
 * Test isPrimaryDurabilityEnabled function
 */
int test_isPrimaryDurabilityEnabled(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    // Enable sync replication for testing
    server.durability.sync_replication_enabled = 1;

    // Test when server is primary (not a replica)
    server.primary_host = NULL;
    TEST_ASSERT(isPrimaryDurabilityEnabled() == 1);

    // Test when server is a replica
    server.primary_host = sdsnew("127.0.0.1");
    TEST_ASSERT(isPrimaryDurabilityEnabled() == 0);

    // Test when durability is disabled but server is primary
    server.durability.sync_replication_enabled = 0;
    sdsfree(server.primary_host);
    server.primary_host = NULL;
    TEST_ASSERT(isPrimaryDurabilityEnabled() == 0);

    // Cleanup and reset
    server.durability.sync_replication_enabled = 0;

    return 0;
}

/**
 * Test isClientReplyBufferLimited function
 */
int test_isClientReplyBufferLimited(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    // Create a mock client
    client *c = zcalloc(sizeof(client));

    // Case 1: No blocked_responses list
    c->clientDurabilityInfo.blocked_responses = NULL;
    TEST_ASSERT(isClientReplyBufferLimited(c) == false);

    // Case 2: Empty blocked_responses list
    c->clientDurabilityInfo.blocked_responses = listCreate();
    TEST_ASSERT(isClientReplyBufferLimited(c) == false);

    // Case 3: Non-empty blocked_responses list
    blockedResponse *br = zcalloc(sizeof(blockedResponse));
    br->primary_repl_offset = 100;
    br->disallowed_byte_offset = 0;
    br->disallowed_reply_block = NULL;
    listAddNodeTail(c->clientDurabilityInfo.blocked_responses, br);
    TEST_ASSERT(isClientReplyBufferLimited(c) == true);

    // Cleanup
    listSetFreeMethod(c->clientDurabilityInfo.blocked_responses, zfree);
    listRelease(c->clientDurabilityInfo.blocked_responses);
    zfree(c);

    return 0;
}

/**
 * Test durableInit function
 */
int test_durableInit(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    // Clean up any existing state
    if (server.durability.clients_waiting_replica_ack != NULL) {
        listRelease(server.durability.clients_waiting_replica_ack);
    }

    // Initialize durability
    durableInit();

    // Verify initialization
    TEST_ASSERT(server.durability.sync_replication_enabled == 0);
    TEST_ASSERT(server.durability.clients_waiting_replica_ack != NULL);
    TEST_ASSERT(listLength(server.durability.clients_waiting_replica_ack) == 0);
    TEST_ASSERT(server.durability.previous_acked_offset == -1);
    TEST_ASSERT(server.durability.curr_db_scan_idx == 0);

    // Cleanup to prevent memory leak
    listRelease(server.durability.clients_waiting_replica_ack);
    server.durability.clients_waiting_replica_ack = NULL;

    return 0;
}

/**
 * Test blockLastResponseIfExist function
 */
int test_blockLastResponseIfExist(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    // Case 1: Response appended to initial buffer
    client *c = zcalloc(sizeof(client));
    c->clientDurabilityInfo.blocked_responses = listCreate();
    listSetFreeMethod(c->clientDurabilityInfo.blocked_responses, zfree);
    c->bufpos = 5;
    c->clientDurabilityInfo.offset.recorded = true;
    c->clientDurabilityInfo.offset.reply_block = NULL;
    c->clientDurabilityInfo.offset.byte_offset = 3;
    c->reply = listCreate();

    blockLastResponseIfExist(c, 42);
    TEST_ASSERT(listLength(c->clientDurabilityInfo.blocked_responses) == 1);
    blockedResponse *br = listNodeValue(listFirst(c->clientDurabilityInfo.blocked_responses));
    TEST_ASSERT(br->primary_repl_offset == 42);
    TEST_ASSERT(br->disallowed_reply_block == NULL);
    TEST_ASSERT(br->disallowed_byte_offset == 3);

    listRelease(c->clientDurabilityInfo.blocked_responses);
    listRelease(c->reply);
    zfree(c);

    // Case 2: Response appended to reply list (first block)
    c = zcalloc(sizeof(client));
    c->clientDurabilityInfo.blocked_responses = listCreate();
    listSetFreeMethod(c->clientDurabilityInfo.blocked_responses, zfree);
    c->reply = listCreate();
    listSetFreeMethod(c->reply, zfree);
    clientReplyBlock *first = createReplyBlock(4);
    clientReplyBlock *second = createReplyBlock(2);
    listAddNodeTail(c->reply, first);
    listAddNodeTail(c->reply, second);
    c->bufpos = 3;
    c->clientDurabilityInfo.offset.recorded = true;
    c->clientDurabilityInfo.offset.reply_block = NULL;
    c->clientDurabilityInfo.offset.byte_offset = 3;

    blockLastResponseIfExist(c, 99);
    TEST_ASSERT(listLength(c->clientDurabilityInfo.blocked_responses) == 1);
    br = listNodeValue(listFirst(c->clientDurabilityInfo.blocked_responses));
    TEST_ASSERT(br->primary_repl_offset == 99);
    TEST_ASSERT(br->disallowed_reply_block == listFirst(c->reply));
    TEST_ASSERT(br->disallowed_byte_offset == 0);

    listRelease(c->clientDurabilityInfo.blocked_responses);
    listRelease(c->reply);
    zfree(c);

    // Case 3: Response starts in next reply block
    c = zcalloc(sizeof(client));
    c->clientDurabilityInfo.blocked_responses = listCreate();
    listSetFreeMethod(c->clientDurabilityInfo.blocked_responses, zfree);
    c->reply = listCreate();
    listSetFreeMethod(c->reply, zfree);
    first = createReplyBlock(4);
    second = createReplyBlock(6);
    listAddNodeTail(c->reply, first);
    listAddNodeTail(c->reply, second);
    c->clientDurabilityInfo.offset.recorded = true;
    c->clientDurabilityInfo.offset.reply_block = listFirst(c->reply);
    c->clientDurabilityInfo.offset.byte_offset = 4;

    blockLastResponseIfExist(c, 7);
    TEST_ASSERT(listLength(c->clientDurabilityInfo.blocked_responses) == 1);
    br = listNodeValue(listFirst(c->clientDurabilityInfo.blocked_responses));
    TEST_ASSERT(br->primary_repl_offset == 7);
    TEST_ASSERT(br->disallowed_reply_block == listFirst(c->reply)->next);
    TEST_ASSERT(br->disallowed_byte_offset == 0);

    listRelease(c->clientDurabilityInfo.blocked_responses);
    listRelease(c->reply);
    zfree(c);

    return 0;
}

/**
 * Test durablePurgeAndGetUncommittedKeyOffset function
 */
int test_durablePurgeAndGetUncommittedKeyOffset(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    serverDb **old_db = server.db;
    int old_dbnum = server.dbnum;
    char *old_primary_host = server.primary_host;
    int old_cluster_enabled = server.cluster_enabled;
    long long old_previous_acked_offset = server.durability.previous_acked_offset;
    long long old_primary_repl_offset = server.primary_repl_offset;

    server.cluster_enabled = 0;
    server.primary_host = NULL;
    server.dbnum = 1;
    server.db = zcalloc(sizeof(serverDb *));

    serverDb *db = zcalloc(sizeof(serverDb));
    durableInitDatabase(db);
    server.db[0] = db;

    robj *key_obj = createStringObject("key", 3);
    sds key = objectGetVal(key_obj);
    long long offset = 10;
    server.primary_repl_offset = offset;
    handleUncommittedKeyForClient(NULL, key_obj, db);

    server.durability.previous_acked_offset = 5;
    TEST_ASSERT(durablePurgeAndGetUncommittedKeyOffset(key, db) == offset);

    TEST_ASSERT(hashtableSize(db->uncommitted_keys) == 1);

    server.durability.previous_acked_offset = 10;
    TEST_ASSERT(durablePurgeAndGetUncommittedKeyOffset(key, db) == -1);
    TEST_ASSERT(hashtableSize(db->uncommitted_keys) == 0);

    decrRefCount(key_obj);
    hashtableRelease(db->uncommitted_keys);
    zfree(db);
    zfree(server.db);

    server.db = old_db;
    server.dbnum = old_dbnum;
    server.primary_host = old_primary_host;
    server.cluster_enabled = old_cluster_enabled;
    server.durability.previous_acked_offset = old_previous_acked_offset;
    server.primary_repl_offset = old_primary_repl_offset;

    return 0;
}

int test_beforeCommandTrackReplOffset(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    // Create a mock client
    client *c = zcalloc(sizeof(client));
    durableClientInit(c);
    durableInit();

    // Test when durability is enabled
    server.durability.sync_replication_enabled = 1;
    beforeCommandTrackReplOffset();
    TEST_ASSERT(c->clientDurabilityInfo.offset.reply_block == NULL);
    TEST_ASSERT(c->clientDurabilityInfo.offset.byte_offset == 0);
    TEST_ASSERT(c->clientDurabilityInfo.current_command_repl_offset == 0);

    // Cleanup
    zfree(c);

    // Reset to default
    server.durability.sync_replication_enabled = 0;

    durableCleanup();
    return 0;
}

int test_preCommandExec(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    initReplyBlockingTestEnv();

    struct serverCommand readonly_cmd = {.declared_name = "get", .flags = CMD_READONLY};

    // Case 1: Durability disabled, allow command and leave tracking untouched.
    client *c = zcalloc(sizeof(client));
    c->cmd = &readonly_cmd;
    c->clientDurabilityInfo.current_command_repl_offset = 123;
    server.durability.sync_replication_enabled = 0;
    server.durability.pre_command_replication_offset = 999;
    server.primary_repl_offset = 555;
    TEST_ASSERT(preCommandExec(c) == CMD_FILTER_ALLOW);
    TEST_ASSERT(c->clientDurabilityInfo.current_command_repl_offset == 123);
    TEST_ASSERT(server.durability.pre_command_replication_offset == 999);
    zfree(c);

    // Case 2: Durability enabled on primary, track pre-exec position.
    c = zcalloc(sizeof(client));
    c->cmd = &readonly_cmd;
    c->bufpos = 7;
    c->clientDurabilityInfo.current_command_repl_offset = 88;
    server.durability.sync_replication_enabled = 1;
    server.primary_host = NULL;
    server.primary_repl_offset = 1234;
    TEST_ASSERT(preCommandExec(c) == CMD_FILTER_ALLOW);
    TEST_ASSERT(c->clientDurabilityInfo.current_command_repl_offset == -1);
    TEST_ASSERT(c->clientDurabilityInfo.offset.recorded == true);
    TEST_ASSERT(c->clientDurabilityInfo.offset.reply_block == NULL);
    TEST_ASSERT(c->clientDurabilityInfo.offset.byte_offset == 7);
    TEST_ASSERT(server.durability.pre_command_replication_offset == 1234);
    zfree(c);

    server.durability.sync_replication_enabled = 0;
    durableCleanup();
    durableClientReset(c);
    return 0;
}
