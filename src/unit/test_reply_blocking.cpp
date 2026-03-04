/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>

extern "C" {
#include "server.h"
#include "reply_blocking.h"
#include "durability_provider.h"
#include "uncommitted_keys.h"
}

/* ========================= Test Helpers ========================= */

static void initTestEnv(void) {
    static char test_logfile[] = "";
    if (server.logfile == nullptr) {
        server.logfile = test_logfile;
    }
}

/**
 * Minimal durability initialization for tests — avoids calling initTaskTypes()
 * which is forward-declared but not yet defined.
 */
static void initDurabilityForTest(void) {
    uncommittedKeysInitPending();
    server.durability.replica_offsets_size = 0;
    server.durability.replica_offsets = nullptr;
    server.durability.previous_acked_offset = -1;
    server.durability.curr_db_scan_idx = 0;
    server.durability.clients_waiting_replica_ack = listCreate();
    for (int i = 0; i < AMZ_TASK_TYPE_MAX; i++) {
        server.durability.tasks_waiting_replica_ack[i] = listCreate();
        server.durability.pending_tasks_waiting_replica_ack[i] = listCreate();
    }
    server.durability.clients_blocked_on_sync_write = 0;
    server.durability.clients_unblocked_on_sync_write = 0;
    server.durability.clients_disconnected_before_unblocking_on_sync_write = 0;
    server.durability.read_responses_blocked_on_sync_write = 0;
    server.durability.write_responses_blocked_on_sync_write = 0;
    server.durability.other_responses_blocked_on_sync_write = 0;
    server.durability.read_responses_unblocked_on_sync_write = 0;
    server.durability.write_responses_unblocked_on_sync_write = 0;
    server.durability.other_responses_unblocked_on_sync_write = 0;
    server.durability.read_responses_blocked_on_sync_write_cumulative_time_us = 0;
    server.durability.write_responses_blocked_on_sync_write_cumulative_time_us = 0;
    server.durability.other_responses_blocked_on_sync_write_cumulative_time_us = 0;
    registerBuiltinDurabilityProviders();
}

/**
 * Minimal durability cleanup for tests.
 */
static void cleanupDurabilityForTest(void) {
    if (server.durability.replica_offsets) {
        zfree(server.durability.replica_offsets);
        server.durability.replica_offsets = nullptr;
    }
    server.durability.replica_offsets_size = 0;
    if (server.durability.clients_waiting_replica_ack) {
        listRelease(server.durability.clients_waiting_replica_ack);
        server.durability.clients_waiting_replica_ack = nullptr;
    }
    uncommittedKeysCleanupPending();
    for (int i = 0; i < AMZ_TASK_TYPE_MAX; i++) {
        if (server.durability.tasks_waiting_replica_ack[i]) {
            listRelease(server.durability.tasks_waiting_replica_ack[i]);
            server.durability.tasks_waiting_replica_ack[i] = nullptr;
        }
        if (server.durability.pending_tasks_waiting_replica_ack[i]) {
            listRelease(server.durability.pending_tasks_waiting_replica_ack[i]);
            server.durability.pending_tasks_waiting_replica_ack[i] = nullptr;
        }
    }
    resetDurabilityProviders();
}

static clientReplyBlock *createReplyBlock(size_t used) {
    clientReplyBlock *block = (clientReplyBlock *)zmalloc(sizeof(clientReplyBlock) + used);
    block->size = used;
    block->used = used;
    block->last_header = nullptr;
    block->flag.buf_encoded = 0;
    return block;
}

/* ========================= Test Fixtures ========================= */

class SyncReplicationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        initTestEnv();
    }
};

class DurabilityProviderTest : public ::testing::Test {
  protected:
    /* Saved state */
    int old_aof_state;
    int old_aof_fsync;
    long long old_fsynced_reploff;
    list *old_replicas;
    list *old_clients_pending_write;
    char *old_primary_host;
    durable_t old_durability;

    void SetUp() override {
        initTestEnv();
        old_aof_state = server.aof_state;
        old_aof_fsync = server.aof_fsync;
        old_fsynced_reploff = server.fsynced_reploff;
        old_replicas = server.replicas;
        old_clients_pending_write = server.clients_pending_write;
        old_primary_host = server.primary_host;
        old_durability = server.durability;

        server.primary_host = nullptr;
        server.clients_pending_write = listCreate();
        server.replicas = listCreate();
    }

    void TearDown() override {
        listRelease(server.clients_pending_write);
        listRelease(server.replicas);

        server.aof_state = old_aof_state;
        server.aof_fsync = old_aof_fsync;
        server.fsynced_reploff = old_fsynced_reploff;
        server.replicas = old_replicas;
        server.clients_pending_write = old_clients_pending_write;
        server.primary_host = old_primary_host;
        server.durability = old_durability;
    }
};

class UncommittedKeysTest : public ::testing::Test {
  protected:
    serverDb **old_db;
    int old_dbnum;
    char *old_primary_host;
    int old_cluster_enabled;
    long long old_previous_acked_offset;
    long long old_primary_repl_offset;

    void SetUp() override {
        initTestEnv();
        old_db = server.db;
        old_dbnum = server.dbnum;
        old_primary_host = server.primary_host;
        old_cluster_enabled = server.cluster_enabled;
        old_previous_acked_offset = server.durability.previous_acked_offset;
        old_primary_repl_offset = server.primary_repl_offset;

        server.cluster_enabled = 0;
        server.primary_host = nullptr;
        server.dbnum = 1;
        server.db = (serverDb **)zcalloc(sizeof(serverDb *));
        server.db[0] = (serverDb *)zcalloc(sizeof(serverDb));
        syncReplicationInitDatabase(server.db[0]);
    }

    void TearDown() override {
        hashtableRelease(server.db[0]->uncommitted_keys);
        zfree(server.db[0]);
        zfree(server.db);

        server.db = old_db;
        server.dbnum = old_dbnum;
        server.primary_host = old_primary_host;
        server.cluster_enabled = old_cluster_enabled;
        server.durability.previous_acked_offset = old_previous_acked_offset;
        server.primary_repl_offset = old_primary_repl_offset;
    }
};

/* ========================= SyncReplication Tests ========================= */

TEST_F(SyncReplicationTest, IsSyncReplicationEnabled) {
    server.durability.sync_replication_enabled = 0;
    ASSERT_EQ(isSyncReplicationEnabled(), 0);

    server.durability.sync_replication_enabled = 1;
    ASSERT_EQ(isSyncReplicationEnabled(), 1);

    server.durability.sync_replication_enabled = 0;
}

TEST_F(SyncReplicationTest, IsPrimarySyncReplicationEnabled) {
    server.durability.sync_replication_enabled = 1;

    /* Primary (not a replica) */
    server.primary_host = nullptr;
    ASSERT_EQ(isPrimarySyncReplicationEnabled(), 1);

    /* Replica */
    server.primary_host = sdsnew("127.0.0.1");
    ASSERT_EQ(isPrimarySyncReplicationEnabled(), 0);
    sdsfree(server.primary_host);

    /* Disabled + primary */
    server.durability.sync_replication_enabled = 0;
    server.primary_host = nullptr;
    ASSERT_EQ(isPrimarySyncReplicationEnabled(), 0);
}

TEST_F(SyncReplicationTest, ClientInitAndReset) {
    client *c = (client *)zcalloc(sizeof(client));
    c->clientDurabilityInfo.blocked_responses = nullptr;
    c->clientDurabilityInfo.durable_blocked_client = 0;
    c->clientDurabilityInfo.current_command_repl_offset = 0;

    /* Disabled — should be a no-op */
    server.durability.sync_replication_enabled = 0;
    syncReplicationClientInit(c);
    ASSERT_EQ(c->clientDurabilityInfo.blocked_responses, nullptr);

    /* Enabled — should initialize */
    server.durability.sync_replication_enabled = 1;
    syncReplicationClientInit(c);
    ASSERT_NE(c->clientDurabilityInfo.blocked_responses, nullptr);
    ASSERT_EQ(listLength(c->clientDurabilityInfo.blocked_responses), 0u);
    ASSERT_FALSE(c->clientDurabilityInfo.offset.recorded);
    ASSERT_EQ(c->clientDurabilityInfo.offset.reply_block, nullptr);
    ASSERT_EQ(c->clientDurabilityInfo.offset.byte_offset, 0u);
    ASSERT_EQ(c->clientDurabilityInfo.current_command_repl_offset, -1);

    /* Reset — should free */
    syncReplicationClientReset(c);
    ASSERT_EQ(c->clientDurabilityInfo.blocked_responses, nullptr);
    ASSERT_FALSE(c->clientDurabilityInfo.offset.recorded);
    ASSERT_EQ(c->clientDurabilityInfo.current_command_repl_offset, -1);

    server.durability.sync_replication_enabled = 0;
    zfree(c);
}

TEST_F(SyncReplicationTest, IsClientReplyBufferLimited) {
    client *c = (client *)zcalloc(sizeof(client));

    /* No blocked_responses list */
    c->clientDurabilityInfo.blocked_responses = nullptr;
    ASSERT_FALSE(isClientReplyBufferLimited(c));

    /* Empty blocked_responses list */
    c->clientDurabilityInfo.blocked_responses = listCreate();
    ASSERT_FALSE(isClientReplyBufferLimited(c));

    /* Non-empty blocked_responses list */
    blockedResponse *br = (blockedResponse *)zcalloc(sizeof(blockedResponse));
    br->primary_repl_offset = 100;
    br->disallowed_byte_offset = 0;
    br->disallowed_reply_block = nullptr;
    listAddNodeTail(c->clientDurabilityInfo.blocked_responses, br);
    ASSERT_TRUE(isClientReplyBufferLimited(c));

    listSetFreeMethod(c->clientDurabilityInfo.blocked_responses, zfree);
    listRelease(c->clientDurabilityInfo.blocked_responses);
    zfree(c);
}

/* ========================= BlockLastResponseIfExist Tests ========================= */

TEST_F(SyncReplicationTest, BlockLastResponseInInitialBuffer) {
    client *c = (client *)zcalloc(sizeof(client));
    c->clientDurabilityInfo.blocked_responses = listCreate();
    listSetFreeMethod(c->clientDurabilityInfo.blocked_responses, zfree);
    c->bufpos = 5;
    c->clientDurabilityInfo.offset.recorded = true;
    c->clientDurabilityInfo.offset.reply_block = nullptr;
    c->clientDurabilityInfo.offset.byte_offset = 3;
    c->reply = listCreate();

    blockLastResponseIfExist(c, 42);
    ASSERT_EQ(listLength(c->clientDurabilityInfo.blocked_responses), 1u);
    blockedResponse *br = (blockedResponse *)listNodeValue(listFirst(c->clientDurabilityInfo.blocked_responses));
    ASSERT_EQ(br->primary_repl_offset, 42);
    ASSERT_EQ(br->disallowed_reply_block, nullptr);
    ASSERT_EQ(br->disallowed_byte_offset, 3u);

    listRelease(c->clientDurabilityInfo.blocked_responses);
    listRelease(c->reply);
    zfree(c);
}

TEST_F(SyncReplicationTest, BlockLastResponseSpillsToReplyList) {
    client *c = (client *)zcalloc(sizeof(client));
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
    c->clientDurabilityInfo.offset.reply_block = nullptr;
    c->clientDurabilityInfo.offset.byte_offset = 3;

    blockLastResponseIfExist(c, 99);
    ASSERT_EQ(listLength(c->clientDurabilityInfo.blocked_responses), 1u);
    blockedResponse *br = (blockedResponse *)listNodeValue(listFirst(c->clientDurabilityInfo.blocked_responses));
    ASSERT_EQ(br->primary_repl_offset, 99);
    ASSERT_EQ(br->disallowed_reply_block, listFirst(c->reply));
    ASSERT_EQ(br->disallowed_byte_offset, 0u);

    listRelease(c->clientDurabilityInfo.blocked_responses);
    listRelease(c->reply);
    zfree(c);
}

TEST_F(SyncReplicationTest, BlockLastResponseNextReplyBlock) {
    client *c = (client *)zcalloc(sizeof(client));
    c->clientDurabilityInfo.blocked_responses = listCreate();
    listSetFreeMethod(c->clientDurabilityInfo.blocked_responses, zfree);
    c->reply = listCreate();
    listSetFreeMethod(c->reply, zfree);
    clientReplyBlock *first = createReplyBlock(4);
    clientReplyBlock *second = createReplyBlock(6);
    listAddNodeTail(c->reply, first);
    listAddNodeTail(c->reply, second);
    c->clientDurabilityInfo.offset.recorded = true;
    c->clientDurabilityInfo.offset.reply_block = listFirst(c->reply);
    c->clientDurabilityInfo.offset.byte_offset = 4;

    blockLastResponseIfExist(c, 7);
    ASSERT_EQ(listLength(c->clientDurabilityInfo.blocked_responses), 1u);
    blockedResponse *br = (blockedResponse *)listNodeValue(listFirst(c->clientDurabilityInfo.blocked_responses));
    ASSERT_EQ(br->primary_repl_offset, 7);
    ASSERT_EQ(br->disallowed_reply_block, listFirst(c->reply)->next);
    ASSERT_EQ(br->disallowed_byte_offset, 0u);

    listRelease(c->clientDurabilityInfo.blocked_responses);
    listRelease(c->reply);
    zfree(c);
}

TEST_F(SyncReplicationTest, BlockLastResponseNoNewResponse) {
    client *c = (client *)zcalloc(sizeof(client));
    c->clientDurabilityInfo.blocked_responses = listCreate();
    listSetFreeMethod(c->clientDurabilityInfo.blocked_responses, zfree);
    c->bufpos = 3;
    c->clientDurabilityInfo.offset.recorded = true;
    c->clientDurabilityInfo.offset.reply_block = nullptr;
    c->clientDurabilityInfo.offset.byte_offset = 3;
    c->reply = listCreate();

    /* bufpos == byte_offset and reply list empty => no new response */
    blockLastResponseIfExist(c, 42);
    ASSERT_EQ(listLength(c->clientDurabilityInfo.blocked_responses), 0u);

    listRelease(c->clientDurabilityInfo.blocked_responses);
    listRelease(c->reply);
    zfree(c);
}

/* ========================= DurabilityProvider Tests ========================= */

TEST_F(DurabilityProviderTest, BuiltinAofProviderDisabledByDefault) {
    initDurabilityForTest();

    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    ASSERT_FALSE(anyDurabilityProviderEnabled());

    cleanupDurabilityForTest();
}

TEST_F(DurabilityProviderTest, AofProviderEnabledWhenAlwaysFsync) {
    initDurabilityForTest();

    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    ASSERT_TRUE(anyDurabilityProviderEnabled());

    cleanupDurabilityForTest();
}

TEST_F(DurabilityProviderTest, AofProviderNotEnabledWithEverysecFsync) {
    initDurabilityForTest();

    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    ASSERT_FALSE(anyDurabilityProviderEnabled());

    cleanupDurabilityForTest();
}

/* Custom test provider */
static bool testProviderEnabled = true;
static long long testProviderOffset = 50;
static bool testCustomIsEnabled(void) { return testProviderEnabled; }
static long long testCustomGetAckedOffset(void) { return testProviderOffset; }

TEST_F(DurabilityProviderTest, CustomProviderRegistrationAndConsensus) {
    initDurabilityForTest();

    durabilityProvider customProvider = {
        .name = "custom-test",
        .isEnabled = testCustomIsEnabled,
        .getAckedOffset = testCustomGetAckedOffset,
    };

    /* Enable AOF provider */
    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    __atomic_store_n(&server.fsynced_reploff_pending, (long long)300, __ATOMIC_RELAXED);
    server.fsynced_reploff = 300;

    /* Register custom provider */
    testProviderEnabled = true;
    testProviderOffset = 50;
    registerDurabilityProvider(&customProvider);
    ASSERT_TRUE(anyDurabilityProviderEnabled());

    /* Consensus = MIN(aof=300, custom=50) = 50 */
    server.primary_repl_offset = 300;
    ASSERT_EQ(getDurabilityConsensusOffset(), 50);

    /* Unregister */
    unregisterDurabilityProvider(&customProvider);

    cleanupDurabilityForTest();
}

TEST_F(DurabilityProviderTest, CustomProviderDisabledIsSkipped) {
    initDurabilityForTest();

    durabilityProvider customProvider = {
        .name = "custom-disabled",
        .isEnabled = testCustomIsEnabled,
        .getAckedOffset = testCustomGetAckedOffset,
    };

    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    __atomic_store_n(&server.fsynced_reploff_pending, (long long)200, __ATOMIC_RELAXED);
    server.fsynced_reploff = 200;

    testProviderEnabled = false;
    testProviderOffset = 10;
    registerDurabilityProvider(&customProvider);

    /* Custom disabled, only AOF enabled => consensus = 200 */
    server.primary_repl_offset = 300;
    ASSERT_EQ(getDurabilityConsensusOffset(), 200);

    unregisterDurabilityProvider(&customProvider);
    cleanupDurabilityForTest();
}

static long long negativeOffsetProvider(void) { return -1; }
static bool alwaysEnabled(void) { return true; }

TEST_F(DurabilityProviderTest, ProviderReturningNegativeOneBlocksConsensus) {
    initDurabilityForTest();

    durabilityProvider blockingProvider = {
        .name = "blocking",
        .isEnabled = alwaysEnabled,
        .getAckedOffset = negativeOffsetProvider,
    };

    registerDurabilityProvider(&blockingProvider);
    server.primary_repl_offset = 300;
    ASSERT_EQ(getDurabilityConsensusOffset(), -1);

    unregisterDurabilityProvider(&blockingProvider);
    cleanupDurabilityForTest();
}

TEST_F(DurabilityProviderTest, NotifyDurabilityProgressUnblocksClients) {
    initDurabilityForTest();
    server.durability.sync_replication_enabled = 1;
    server.durability.previous_acked_offset = 0;
    server.primary_repl_offset = 200;

    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    __atomic_store_n(&server.fsynced_reploff_pending, (long long)200, __ATOMIC_RELAXED);
    server.fsynced_reploff = 200;

    /* Set up a blocked client */
    client *c = (client *)zcalloc(sizeof(client));
    syncReplicationClientInit(c);
    c->reply = listCreate();
    listSetFreeMethod(c->reply, zfree);
    c->bufpos = 5;
    c->clientDurabilityInfo.offset.recorded = true;
    c->clientDurabilityInfo.offset.reply_block = nullptr;
    c->clientDurabilityInfo.offset.byte_offset = 0;
    blockLastResponseIfExist(c, 100);
    ASSERT_EQ(listLength(c->clientDurabilityInfo.blocked_responses), 1u);
    listAddNodeTail(server.durability.clients_waiting_replica_ack, c);
    c->clientDurabilityInfo.durable_blocked_client = 1;

    /* notifyDurabilityProgress should unblock the client */
    notifyDurabilityProgress();
    ASSERT_EQ(server.durability.previous_acked_offset, 200);
    ASSERT_EQ(listLength(c->clientDurabilityInfo.blocked_responses), 0u);
    ASSERT_EQ(c->clientDurabilityInfo.durable_blocked_client, 0u);

    listRelease(c->reply);
    syncReplicationClientReset(c);
    zfree(c);
    cleanupDurabilityForTest();
}

TEST_F(DurabilityProviderTest, NoProgressWhenOffsetNotAdvanced) {
    initDurabilityForTest();
    server.durability.sync_replication_enabled = 1;

    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    __atomic_store_n(&server.fsynced_reploff_pending, (long long)50, __ATOMIC_RELAXED);
    server.fsynced_reploff = 50;

    server.durability.previous_acked_offset = 50;
    server.primary_repl_offset = 100;

    /* Set up a blocked client at offset 80 */
    client *c = (client *)zcalloc(sizeof(client));
    syncReplicationClientInit(c);
    c->reply = listCreate();
    listSetFreeMethod(c->reply, zfree);
    c->bufpos = 5;
    c->clientDurabilityInfo.offset.recorded = true;
    c->clientDurabilityInfo.offset.reply_block = nullptr;
    c->clientDurabilityInfo.offset.byte_offset = 0;
    blockLastResponseIfExist(c, 80);
    listAddNodeTail(server.durability.clients_waiting_replica_ack, c);
    c->clientDurabilityInfo.durable_blocked_client = 1;

    /* Previous offset = 50, consensus = 50 => no progress, client stays blocked */
    notifyDurabilityProgress();
    ASSERT_EQ(server.durability.previous_acked_offset, 50);
    ASSERT_EQ(listLength(c->clientDurabilityInfo.blocked_responses), 1u);
    ASSERT_EQ(c->clientDurabilityInfo.durable_blocked_client, 1u);

    listRelease(c->reply);
    syncReplicationClientReset(c);
    zfree(c);
    cleanupDurabilityForTest();
}

/* ========================= UncommittedKeys Tests ========================= */

TEST_F(UncommittedKeysTest, HandleAndPurgeUncommittedKey) {
    robj *key_obj = createStringObject("key", 3);
    sds key = (sds)objectGetVal(key_obj);
    long long offset = 10;
    server.primary_repl_offset = offset;
    handleUncommittedKeyForClient(nullptr, key_obj, server.db[0]);

    /* Key should be in uncommitted set */
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* Not yet acked — should return the offset */
    server.durability.previous_acked_offset = 5;
    ASSERT_EQ(syncReplicationPurgeAndGetUncommittedKeyOffset(key, server.db[0]), offset);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* Acked — should purge and return -1 */
    server.durability.previous_acked_offset = 10;
    ASSERT_EQ(syncReplicationPurgeAndGetUncommittedKeyOffset(key, server.db[0]), -1);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 0u);

    decrRefCount(key_obj);
}

TEST_F(UncommittedKeysTest, MultipleKeysTracked) {
    robj *k1 = createStringObject("key1", 4);
    robj *k2 = createStringObject("key2", 4);

    server.primary_repl_offset = 10;
    handleUncommittedKeyForClient(nullptr, k1, server.db[0]);
    server.primary_repl_offset = 20;
    handleUncommittedKeyForClient(nullptr, k2, server.db[0]);

    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 2u);

    /* Ack up to 10 — only key1 should be purged */
    server.durability.previous_acked_offset = 10;
    ASSERT_EQ(syncReplicationPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(k1), server.db[0]), -1);
    ASSERT_EQ(syncReplicationPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(k2), server.db[0]), 20);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* Ack up to 20 — key2 also purged */
    server.durability.previous_acked_offset = 20;
    ASSERT_EQ(syncReplicationPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(k2), server.db[0]), -1);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 0u);

    decrRefCount(k1);
    decrRefCount(k2);
}

TEST_F(UncommittedKeysTest, KeyOffsetUpdatedOnRewrite) {
    robj *key_obj = createStringObject("key", 3);

    server.primary_repl_offset = 10;
    handleUncommittedKeyForClient(nullptr, key_obj, server.db[0]);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* Rewrite same key at higher offset */
    server.primary_repl_offset = 50;
    handleUncommittedKeyForClient(nullptr, key_obj, server.db[0]);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* Old offset acked but new offset not */
    server.durability.previous_acked_offset = 10;
    ASSERT_EQ(syncReplicationPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(key_obj), server.db[0]), 50);

    decrRefCount(key_obj);
}

TEST_F(UncommittedKeysTest, NonexistentKeyReturnsNegativeOne) {
    sds missing = sdsnew("nonexistent");
    server.durability.previous_acked_offset = 0;
    ASSERT_EQ(syncReplicationPurgeAndGetUncommittedKeyOffset(missing, server.db[0]), -1);
    sdsfree(missing);
}

TEST_F(UncommittedKeysTest, HasUncommittedKeysAcrossDBs) {
    /* No uncommitted keys initially */
    ASSERT_EQ(hasUncommittedKeys(), 0);

    robj *key_obj = createStringObject("key", 3);
    server.primary_repl_offset = 10;
    handleUncommittedKeyForClient(nullptr, key_obj, server.db[0]);
    ASSERT_EQ(hasUncommittedKeys(), 1);

    /* Purge it */
    server.durability.previous_acked_offset = 10;
    syncReplicationPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(key_obj), server.db[0]);
    ASSERT_EQ(hasUncommittedKeys(), 0);

    decrRefCount(key_obj);
}

TEST_F(UncommittedKeysTest, GetNumberOfUncommittedKeys) {
    ASSERT_EQ(getNumberOfUncommittedKeys(), 0u);

    robj *k1 = createStringObject("a", 1);
    robj *k2 = createStringObject("b", 1);
    robj *k3 = createStringObject("c", 1);

    server.primary_repl_offset = 10;
    handleUncommittedKeyForClient(nullptr, k1, server.db[0]);
    handleUncommittedKeyForClient(nullptr, k2, server.db[0]);
    handleUncommittedKeyForClient(nullptr, k3, server.db[0]);

    ASSERT_EQ(getNumberOfUncommittedKeys(), 3u);

    decrRefCount(k1);
    decrRefCount(k2);
    decrRefCount(k3);
}

TEST_F(UncommittedKeysTest, CleanupTimeLimitScalesWithKeyCount) {
    /* Set the cleanup time limit config (normally set by server init) */
    server.durability.keys_cleanup_time_limit_ms = 100;

    /* 0 keys => 1ms */
    ASSERT_EQ(getUncommittedKeysCleanupTimeLimit(0), 1u);

    /* Small count => small limit */
    unsigned long long small_limit = getUncommittedKeysCleanupTimeLimit(100);
    ASSERT_GE(small_limit, 1u);

    /* Larger count => larger limit (monotonically increasing) */
    unsigned long long larger = getUncommittedKeysCleanupTimeLimit(500000);
    ASSERT_GE(larger, small_limit);

    /* At 1 million keys, should hit the configured max */
    unsigned long long at_max = getUncommittedKeysCleanupTimeLimit(1000000);
    ASSERT_EQ(at_max, 100u);
}

/* ========================= Function Store Tests ========================= */

TEST_F(SyncReplicationTest, FunctionStoreUncommittedTracking) {
    server.durability.previous_acked_offset = 0;

    /* Not uncommitted initially */
    ASSERT_FALSE(amzDurableFunctions_isFunctionStoreUncommitted());

    /* Mark uncommitted */
    server.execution_nesting = 0;
    server.primary_repl_offset = 100;
    amzDurableFunctions_handleUncommittedFunctionStore();
    ASSERT_TRUE(amzDurableFunctions_isFunctionStoreUncommitted());
    ASSERT_EQ(amzDurableFunctions_getBlockingOffset(), 100);

    /* After acking, it should no longer be uncommitted */
    server.durability.previous_acked_offset = 100;
    ASSERT_FALSE(amzDurableFunctions_isFunctionStoreUncommitted());
}

/* ========================= INFO String Test ========================= */

TEST_F(SyncReplicationTest, GenInfoStringDisabled) {
    server.durability.sync_replication_enabled = 0;
    sds info = sdsempty();
    info = genSyncReplicationInfoString(info);
    ASSERT_NE(strstr(info, "sync_replication_enabled:0"), nullptr);
    sdsfree(info);
}

TEST_F(SyncReplicationTest, GenInfoStringEnabled) {
    server.durability.sync_replication_enabled = 1;
    server.durability.clients_waiting_replica_ack = listCreate();
    server.durability.read_responses_blocked_on_sync_write = 5;
    server.durability.write_responses_blocked_on_sync_write = 3;
    server.durability.previous_acked_offset = 42;
    server.primary_repl_offset = 100;

    sds info = sdsempty();
    info = genSyncReplicationInfoString(info);
    ASSERT_NE(strstr(info, "sync_replication_enabled:1"), nullptr);
    ASSERT_NE(strstr(info, "sync_repl_read_blocked_count:5"), nullptr);
    ASSERT_NE(strstr(info, "sync_repl_write_blocked_count:3"), nullptr);
    ASSERT_NE(strstr(info, "sync_repl_previous_acked_offset:42"), nullptr);
    ASSERT_NE(strstr(info, "sync_repl_primary_repl_offset:100"), nullptr);

    sdsfree(info);
    listRelease(server.durability.clients_waiting_replica_ack);
    server.durability.clients_waiting_replica_ack = nullptr;
    server.durability.sync_replication_enabled = 0;
}

/* ========================= Command Parameter Helpers ========================= */

TEST_F(SyncReplicationTest, AmzSwapdbGetParams) {
    robj *argv[3];
    argv[0] = createStringObject("SWAPDB", 6);
    argv[1] = createStringObject("0", 1);
    argv[2] = createStringObject("1", 1);
    int id1, id2;

    server.cluster_enabled = 0;
    server.dbnum = 16;
    ASSERT_TRUE(amzSwapdbGetParams(argv, 3, &id1, &id2));
    ASSERT_EQ(id1, 0);
    ASSERT_EQ(id2, 1);

    /* Same DB should fail */
    decrRefCount(argv[2]);
    argv[2] = createStringObject("0", 1);
    ASSERT_FALSE(amzSwapdbGetParams(argv, 3, &id1, &id2));

    /* Wrong argc should fail */
    ASSERT_FALSE(amzSwapdbGetParams(argv, 2, &id1, &id2));

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
    decrRefCount(argv[2]);
}

TEST_F(SyncReplicationTest, AmzSelectGetParams) {
    robj *argv[2];
    argv[0] = createStringObject("SELECT", 6);
    argv[1] = createStringObject("5", 1);
    int dbid;

    server.dbnum = 16;
    ASSERT_TRUE(amzSelectGetParams(argv, 2, nullptr, &dbid));
    ASSERT_EQ(dbid, 5);

    /* Out of range */
    decrRefCount(argv[1]);
    argv[1] = createStringObject("99", 2);
    ASSERT_FALSE(amzSelectGetParams(argv, 2, nullptr, &dbid));

    /* Wrong argc */
    ASSERT_FALSE(amzSelectGetParams(argv, 1, nullptr, &dbid));

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

TEST_F(SyncReplicationTest, AmzGetDbIdFromRobj) {
    int db_id;
    server.dbnum = 16;

    robj *valid = createStringObject("5", 1);
    ASSERT_TRUE(amzGetDbIdFromRobj(valid, &db_id));
    ASSERT_EQ(db_id, 5);
    decrRefCount(valid);

    robj *negative = createStringObject("-1", 2);
    ASSERT_FALSE(amzGetDbIdFromRobj(negative, &db_id));
    decrRefCount(negative);

    robj *too_big = createStringObject("99", 2);
    ASSERT_FALSE(amzGetDbIdFromRobj(too_big, &db_id));
    decrRefCount(too_big);
}
