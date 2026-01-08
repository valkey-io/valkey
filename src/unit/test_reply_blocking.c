#include "../fmacros.h"
#include "../reply_blocking.h"
#include "test_help.h"
#include "../server.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>

/**
 * Test durableClientInit and durableClientReset functions
 * These functions initialize and reset client durability attributes
 */
int test_durableClientInitAndReset(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

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
