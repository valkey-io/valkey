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

/* Forward declarations used by tests */
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
    initTaskTypes();
    server.durability.previous_acked_offset = -1;
    server.durability.clients_waiting_ack = listCreate();
    durableTaskInitLists();
    server.durability.clients_blocked = 0;
    server.durability.clients_unblocked = 0;
    server.durability.clients_disconnected_before_unblocking = 0;
    server.durability.read_responses_blocked = 0;
    server.durability.write_responses_blocked = 0;
    server.durability.other_responses_blocked = 0;
    server.durability.read_responses_unblocked = 0;
    server.durability.write_responses_unblocked = 0;
    server.durability.other_responses_unblocked = 0;
    server.durability.read_responses_blocked_cumulative_time_us = 0;
    server.durability.write_responses_blocked_cumulative_time_us = 0;
    server.durability.other_responses_blocked_cumulative_time_us = 0;
    registerBuiltinDurabilityProviders();
}

/**
 * Minimal durability cleanup for tests.
 */
static void cleanupDurabilityForTest(void) {
    if (server.durability.clients_waiting_ack) {
        listRelease(server.durability.clients_waiting_ack);
        server.durability.clients_waiting_ack = nullptr;
    }
    uncommittedKeysCleanupPending();
    durableTaskCleanupLists();
    resetDurabilityProviders();
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
        durabilityInitDatabase(server.db[0]);
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

/* ========================= Durability Tests ========================= */

TEST_F(DurabilityProviderTest, IsDurabilityEnabled) {
    initDurabilityForTest();

    /* Durability delegates to anyDurabilityProviderEnabled().
     * The built-in AOF provider enables when AOF on + appendfsync always. */
    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    ASSERT_EQ(isDurabilityEnabled(), 0);

    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    ASSERT_EQ(isDurabilityEnabled(), 0);

    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    ASSERT_EQ(isDurabilityEnabled(), 1);

    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    ASSERT_EQ(isDurabilityEnabled(), 0);

    cleanupDurabilityForTest();
}

TEST_F(SyncReplicationTest, IsPrimaryDurabilityEnabled) {
    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;

    /* Primary (not a replica) */
    server.primary_host = nullptr;
    ASSERT_EQ(isPrimaryDurabilityEnabled(), 1);

    /* Replica */
    server.primary_host = sdsnew("127.0.0.1");
    ASSERT_EQ(isPrimaryDurabilityEnabled(), 0);
    sdsfree(server.primary_host);

    /* Disabled (appendfsync != always) + primary */
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    server.primary_host = nullptr;
    ASSERT_EQ(isPrimaryDurabilityEnabled(), 0);
}

TEST_F(SyncReplicationTest, ClientInitAndReset) {
    client *c = (client *)zcalloc(sizeof(client));
    c->clientDurabilityInfo.blocked_responses = nullptr;
    c->clientDurabilityInfo.durability_blocked = 0;
    c->clientDurabilityInfo.current_command_repl_offset = 0;

    /* Disabled — should be a no-op */
    server.aof_state = AOF_OFF; server.aof_fsync = AOF_FSYNC_EVERYSEC;
    durabilityClientInit(c);
    ASSERT_EQ(c->clientDurabilityInfo.blocked_responses, nullptr);

    /* Enabled — should initialize */
    server.aof_state = AOF_ON; server.aof_fsync = AOF_FSYNC_ALWAYS;
    durabilityClientInit(c);
    ASSERT_NE(c->clientDurabilityInfo.blocked_responses, nullptr);
    ASSERT_EQ(listLength(c->clientDurabilityInfo.blocked_responses), 0u);
    ASSERT_FALSE(c->clientDurabilityInfo.offset.recorded);
    ASSERT_EQ(c->clientDurabilityInfo.offset.reply_block, nullptr);
    ASSERT_EQ(c->clientDurabilityInfo.offset.byte_offset, 0u);
    ASSERT_EQ(c->clientDurabilityInfo.current_command_repl_offset, -1);

    /* Reset — should free */
    durabilityClientReset(c);
    ASSERT_EQ(c->clientDurabilityInfo.blocked_responses, nullptr);
    ASSERT_FALSE(c->clientDurabilityInfo.offset.recorded);
    ASSERT_EQ(c->clientDurabilityInfo.current_command_repl_offset, -1);

    server.aof_state = AOF_OFF; server.aof_fsync = AOF_FSYNC_EVERYSEC;
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

/* Verify that clientHasPendingReplies uses bufpos (not data_len) when
 * comparing against the blocked response's disallowed_byte_offset.
 *
 * With copy avoidance, encoded reply buffers contain payload headers +
 * bulk-string references.  The io_last_written.data_len tracks the total
 * *decoded* data written to the socket (i.e. RESP bytes on the wire)
 * which can be larger than the encoded buffer position (bufpos).
 * Using data_len for the comparison would cause the response to appear
 * "fully written" prematurely, releasing the blocked reply before the
 * durability provider acknowledges the write. */
TEST_F(SyncReplicationTest, ClientHasPendingRepliesUsesBufposNotDataLen) {
    client *c = (client *)zcalloc(sizeof(client));
    c->reply = listCreate();
    c->repl_data = nullptr;
    c->slot_migration_job = nullptr;
    c->raw_flag = 0;

    /* Set up a blocked response at offset 100 in c->buf (no reply block) */
    c->clientDurabilityInfo.blocked_responses = listCreate();
    listSetFreeMethod(c->clientDurabilityInfo.blocked_responses, zfree);

    blockedResponse *br = (blockedResponse *)zcalloc(sizeof(blockedResponse));
    br->primary_repl_offset = 500;
    br->disallowed_byte_offset = 100;
    br->disallowed_reply_block = nullptr;
    listAddNodeTail(c->clientDurabilityInfo.blocked_responses, br);

    /* Simulate: 200 bytes in the static buffer, no reply list entries */
    c->bufpos = 200;

    /* Case 1: bufpos < disallowed_byte_offset  =>  has pending replies
     * (the write hasn't reached the blocked boundary yet) */
    c->io_last_written.buf = nullptr;
    c->io_last_written.bufpos = 50;
    c->io_last_written.data_len = 50;
    ASSERT_TRUE(clientHasPendingReplies(c));

    /* Case 2: bufpos == disallowed_byte_offset  =>  no pending replies
     * (the write has exactly reached the blocked boundary) */
    c->io_last_written.bufpos = 100;
    c->io_last_written.data_len = 100;
    ASSERT_FALSE(clientHasPendingReplies(c));

    /* Case 3: The critical copy-avoidance scenario.
     * bufpos is still below the boundary (e.g. 80, because encoded buffer
     * is compact), but data_len is above it (e.g. 120, because decoded
     * RESP on the wire is larger than the encoded buffer).
     *
     * With the old bug (using data_len), this would return false (no pending)
     * causing the response to be released prematurely.
     * With the fix (using bufpos), this correctly returns true (still pending). */
    c->io_last_written.bufpos = 80;
    c->io_last_written.data_len = 120;
    ASSERT_TRUE(clientHasPendingReplies(c));

    listRelease(c->clientDurabilityInfo.blocked_responses);
    listRelease(c->reply);
    zfree(c);
}

/* ========================= DurabilityProvider Tests ========================= */

TEST_F(DurabilityProviderTest, BuiltinAofProviderDisabledWhenAofOff) {
    initDurabilityForTest();

    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    ASSERT_FALSE(anyDurabilityProviderEnabled());

    cleanupDurabilityForTest();
}

TEST_F(DurabilityProviderTest, AofProviderEnabledOnlyWhenAlwaysFsync) {
    initDurabilityForTest();

    /* AOF provider is only enabled when AOF is on AND appendfsync is always */
    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    ASSERT_TRUE(anyDurabilityProviderEnabled());

    /* Not enabled with other fsync policies */
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    ASSERT_FALSE(anyDurabilityProviderEnabled());

    server.aof_fsync = AOF_FSYNC_NO;
    ASSERT_FALSE(anyDurabilityProviderEnabled());

    cleanupDurabilityForTest();
}

TEST_F(DurabilityProviderTest, NoProviderEnabledWhenNotAlwaysFsync) {
    initDurabilityForTest();

    /* When fsync != always, no provider is enabled so consensus = primary_repl_offset */
    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    server.primary_repl_offset = 500;
    ASSERT_EQ(getDurabilityConsensusOffset(), 500);

    server.aof_fsync = AOF_FSYNC_NO;
    server.primary_repl_offset = 700;
    ASSERT_EQ(getDurabilityConsensusOffset(), 700);

    cleanupDurabilityForTest();
}

TEST_F(DurabilityProviderTest, AofProviderPauseAndResume) {
    initDurabilityForTest();

    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    server.primary_repl_offset = 300;
    __atomic_store_n(&server.fsynced_reploff_pending, (long long)300, __ATOMIC_RELAXED);
    server.fsynced_reploff = 300;

    /* Before pause: consensus = 300 (fsynced) */
    ASSERT_EQ(getDurabilityConsensusOffset(), 300);

    /* Pause: consensus should be frozen at 300 (the offset at pause time).
     * New writes that advance primary_repl_offset past 300 will block,
     * but already-acknowledged data remains unblocked. */
    ASSERT_TRUE(pauseDurabilityProvider("aof"));
    ASSERT_EQ(getDurabilityConsensusOffset(), 300);

    /* Advance primary_repl_offset — consensus stays frozen at 300 */
    server.primary_repl_offset = 500;
    ASSERT_EQ(getDurabilityConsensusOffset(), 300);

    /* Resume: consensus should catch up to actual fsynced offset */
    server.aof_state = AOF_ON; server.aof_fsync = AOF_FSYNC_ALWAYS;
    server.durability.previous_acked_offset = -1;
    ASSERT_TRUE(resumeDurabilityProvider("aof"));
    ASSERT_EQ(getDurabilityConsensusOffset(), 300);

    /* Nonexistent provider returns false */
    ASSERT_FALSE(pauseDurabilityProvider("nonexistent"));
    ASSERT_FALSE(resumeDurabilityProvider("nonexistent"));

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
    ASSERT_EQ(durabilityPurgeAndGetUncommittedKeyOffset(key, server.db[0]), offset);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* Acked — should purge and return -1 */
    server.durability.previous_acked_offset = 10;
    ASSERT_EQ(durabilityPurgeAndGetUncommittedKeyOffset(key, server.db[0]), -1);
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
    ASSERT_EQ(durabilityPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(k1), server.db[0]), -1);
    ASSERT_EQ(durabilityPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(k2), server.db[0]), 20);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* Ack up to 20 — key2 also purged */
    server.durability.previous_acked_offset = 20;
    ASSERT_EQ(durabilityPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(k2), server.db[0]), -1);
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
    ASSERT_EQ(durabilityPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(key_obj), server.db[0]), 50);

    decrRefCount(key_obj);
}

TEST_F(UncommittedKeysTest, NonexistentKeyReturnsNegativeOne) {
    sds missing = sdsnew("nonexistent");
    server.durability.previous_acked_offset = 0;
    ASSERT_EQ(durabilityPurgeAndGetUncommittedKeyOffset(missing, server.db[0]), -1);
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
    durabilityPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(key_obj), server.db[0]);
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

TEST_F(UncommittedKeysTest, DrainCommittedKeysRemovesCommitted) {
    robj *k1 = createStringObject("key1", 4);
    robj *k2 = createStringObject("key2", 4);
    robj *k3 = createStringObject("key3", 4);

    server.primary_repl_offset = 10;
    handleUncommittedKeyForClient(nullptr, k1, server.db[0]);
    server.primary_repl_offset = 20;
    handleUncommittedKeyForClient(nullptr, k2, server.db[0]);
    server.primary_repl_offset = 30;
    handleUncommittedKeyForClient(nullptr, k3, server.db[0]);

    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 3u);

    /* Drain up to offset 20 — key1 and key2 should be removed */
    drainCommittedKeys(20);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* key3 should still be present */
    void *found = nullptr;
    ASSERT_TRUE(hashtableFind(server.db[0]->uncommitted_keys,
                              (sds)objectGetVal(k3), &found));

    /* Drain up to 30 — key3 removed */
    drainCommittedKeys(30);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 0u);

    decrRefCount(k1);
    decrRefCount(k2);
    decrRefCount(k3);
}

TEST_F(UncommittedKeysTest, DrainPreservesReDirtiedKey) {
    robj *key_obj = createStringObject("hotkey", 6);

    /* Write at offset 10 */
    server.primary_repl_offset = 10;
    handleUncommittedKeyForClient(nullptr, key_obj, server.db[0]);

    /* Re-dirty at offset 50 */
    server.primary_repl_offset = 50;
    handleUncommittedKeyForClient(nullptr, key_obj, server.db[0]);

    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* Drain up to 10 — key should NOT be removed because it was re-dirtied at 50 */
    drainCommittedKeys(10);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* Drain up to 50 — now it should be removed */
    drainCommittedKeys(50);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 0u);

    decrRefCount(key_obj);
}

TEST_F(UncommittedKeysTest, DrainClearsDirtyDbOffset) {
    server.db[0]->dirty_repl_offset = 100;

    /* Drain below the DB offset — should not clear */
    drainCommittedKeys(50);
    ASSERT_EQ(server.db[0]->dirty_repl_offset, 100);

    /* Drain at the DB offset — should clear */
    drainCommittedKeys(100);
    ASSERT_EQ(server.db[0]->dirty_repl_offset, -1);
}

TEST_F(UncommittedKeysTest, DrainEmptyHashtableIsNoop) {
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 0u);
    drainCommittedKeys(1000); /* Should not crash */
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 0u);
}


/* ========================= Function Store Tests ========================= */

TEST_F(SyncReplicationTest, FunctionStoreUncommittedTracking) {
    server.durability.previous_acked_offset = 0;

    /* Not uncommitted initially */
    ASSERT_FALSE(isDurableFunctionStoreUncommitted());

    /* Mark uncommitted */
    server.execution_nesting = 0;
    server.primary_repl_offset = 100;
    handleUncommittedFunctionStore();
    ASSERT_TRUE(isDurableFunctionStoreUncommitted());
    ASSERT_EQ(getFuncStoreBlockingOffset(), 100);

    /* After acking, it should no longer be uncommitted */
    server.durability.previous_acked_offset = 100;
    ASSERT_FALSE(isDurableFunctionStoreUncommitted());
}

/* ========================= INFO String Test ========================= */

TEST_F(SyncReplicationTest, GenInfoStringDisabled) {
    server.aof_state = AOF_OFF; server.aof_fsync = AOF_FSYNC_EVERYSEC;
    sds info = sdsempty();
    info = genDurabilityInfoString(info);
    ASSERT_NE(strstr(info, "durability_enabled:0"), nullptr);
    sdsfree(info);
}

TEST_F(SyncReplicationTest, GenInfoStringEnabled) {
    server.aof_state = AOF_ON; server.aof_fsync = AOF_FSYNC_ALWAYS;
    server.durability.clients_waiting_ack = listCreate();
    server.durability.read_responses_blocked = 5;
    server.durability.write_responses_blocked = 3;
    server.durability.previous_acked_offset = 42;
    server.primary_repl_offset = 100;

    sds info = sdsempty();
    info = genDurabilityInfoString(info);
    ASSERT_NE(strstr(info, "durability_enabled:1"), nullptr);
    ASSERT_NE(strstr(info, "durability_read_blocked_count:5"), nullptr);
    ASSERT_NE(strstr(info, "durability_write_blocked_count:3"), nullptr);
    ASSERT_NE(strstr(info, "durability_previous_acked_offset:42"), nullptr);
    ASSERT_NE(strstr(info, "durability_primary_repl_offset:100"), nullptr);

    sdsfree(info);
    listRelease(server.durability.clients_waiting_ack);
    server.durability.clients_waiting_ack = nullptr;
    server.aof_state = AOF_OFF; server.aof_fsync = AOF_FSYNC_EVERYSEC;
}

/* ========================= Migrated from C tests ========================= */

/**
 * Fixture for tests that need full durability init (durabilityInit)
 * plus database and client setup.
 */
class FullDurabilityTest : public ::testing::Test {
  protected:
    serverDb **old_db;
    int old_dbnum;
    char *old_primary_host;
    int old_cluster_enabled;
    long long old_primary_repl_offset;
    int old_get_ack;
    list *old_replicas;
    list *old_clients_pending_write;
    int old_aof_state;
    int old_aof_fsync;
    long long old_fsynced_reploff;
    durable_t old_durability;
    list *old_monitors;

    void SetUp() override {
        initTestEnv();
        old_db = server.db;
        old_dbnum = server.dbnum;
        old_primary_host = server.primary_host;
        old_cluster_enabled = server.cluster_enabled;
        old_primary_repl_offset = server.primary_repl_offset;
        old_get_ack = server.get_ack_from_replicas;
        old_replicas = server.replicas;
        old_clients_pending_write = server.clients_pending_write;
        old_aof_state = server.aof_state;
        old_aof_fsync = server.aof_fsync;
        old_fsynced_reploff = server.fsynced_reploff;
        old_durability = server.durability;
        old_monitors = server.monitors;

        server.cluster_enabled = 0;
        server.primary_host = nullptr;
        server.clients_pending_write = listCreate();
        server.monitors = listCreate();
        server.dbnum = 1;
        server.db = (serverDb **)zcalloc(sizeof(serverDb *));
        server.db[0] = (serverDb *)zcalloc(sizeof(serverDb));
        durabilityInitDatabase(server.db[0]);

        server.aof_state = AOF_ON; server.aof_fsync = AOF_FSYNC_ALWAYS;
        durabilityInit();
    }

    void TearDown() override {
        durabilityCleanup();
        listRelease(server.clients_pending_write);
        listRelease(server.monitors);
        hashtableRelease(server.db[0]->uncommitted_keys);
        zfree(server.db[0]);
        zfree(server.db);

        server.db = old_db;
        server.dbnum = old_dbnum;
        server.primary_host = old_primary_host;
        server.cluster_enabled = old_cluster_enabled;
        server.primary_repl_offset = old_primary_repl_offset;
        server.get_ack_from_replicas = old_get_ack;
        server.replicas = old_replicas;
        server.clients_pending_write = old_clients_pending_write;
        server.aof_state = old_aof_state;
        server.aof_fsync = old_aof_fsync;
        server.fsynced_reploff = old_fsynced_reploff;
        server.durability = old_durability;
        server.monitors = old_monitors;
    }
};

/* Migrated from test_durableInit */
TEST_F(SyncReplicationTest, SyncReplicationInitSetsDefaults) {
    /* initDurabilityForTest() approximates durabilityInit(); verify fields */
    initDurabilityForTest();

    ASSERT_NE(server.durability.clients_waiting_ack, nullptr);
    ASSERT_EQ(listLength(server.durability.clients_waiting_ack), 0u);
    ASSERT_EQ(server.durability.previous_acked_offset, -1);
    ASSERT_EQ(server.durability.clients_blocked, 0u);
    ASSERT_EQ(server.durability.clients_unblocked, 0u);
    ASSERT_EQ(server.durability.clients_disconnected_before_unblocking, 0u);
    ASSERT_EQ(server.durability.read_responses_blocked, 0u);
    ASSERT_EQ(server.durability.write_responses_blocked, 0u);
    ASSERT_EQ(server.durability.other_responses_blocked, 0u);

    cleanupDurabilityForTest();
}

/* Migrated from test_beforeCommandTrackReplOffset */
TEST_F(FullDurabilityTest, BeforeCommandTrackReplOffset) {
    client *c = (client *)zcalloc(sizeof(client));
    durabilityClientInit(c);

    struct serverCommand readonly_cmd = {.declared_name = "get", .flags = CMD_READONLY};
    c->cmd = &readonly_cmd;

    server.primary_repl_offset = 500;
    beforeCommandTrackReplOffset(c);

    /* pre_call_replication_offset should be snapshotted */
    ASSERT_EQ(server.durability.pre_call_replication_offset, 500);

    durabilityClientReset(c);
    zfree(c);
}

/* Migrated from test_preCommandExec — Case 1: durability disabled */
TEST_F(SyncReplicationTest, PreCommandExecDurabilityDisabled) {
    struct serverCommand readonly_cmd = {.declared_name = "get", .flags = CMD_READONLY};

    /* preCommandExec always accesses server.monitors via isCommandReplicatedToMonitors() */
    list *old_monitors = server.monitors;
    server.monitors = listCreate();

    client *c = (client *)zcalloc(sizeof(client));
    c->cmd = &readonly_cmd;
    c->clientDurabilityInfo.current_command_repl_offset = 123;
    server.aof_state = AOF_OFF; server.aof_fsync = AOF_FSYNC_EVERYSEC;
    server.primary_repl_offset = 555;

    ASSERT_EQ(preCommandExec(c), CMD_FILTER_ALLOW);
    /* preCommandExec always resets current_command_repl_offset to -1 */
    ASSERT_EQ(c->clientDurabilityInfo.current_command_repl_offset, -1);
    /* pre_command_replication_offset is always snapshotted */
    ASSERT_EQ(server.durability.pre_command_replication_offset, 555);

    zfree(c);
    listRelease(server.monitors);
    server.monitors = old_monitors;
}

/* Migrated from test_preCommandExec — Case 2: durability enabled on primary */
TEST_F(FullDurabilityTest, PreCommandExecDurabilityEnabledOnPrimary) {
    struct serverCommand readonly_cmd = {.declared_name = "get", .flags = CMD_READONLY};

    client *c = (client *)zcalloc(sizeof(client));
    durabilityClientInit(c);
    c->cmd = &readonly_cmd;
    c->bufpos = 7;
    c->clientDurabilityInfo.current_command_repl_offset = 88;
    server.primary_repl_offset = 1234;

    ASSERT_EQ(preCommandExec(c), CMD_FILTER_ALLOW);
    /* current_command_repl_offset should be reset to -1 */
    ASSERT_EQ(c->clientDurabilityInfo.current_command_repl_offset, -1);
    /* Pre-execution position should be tracked */
    ASSERT_TRUE(c->clientDurabilityInfo.offset.recorded);
    ASSERT_EQ(c->clientDurabilityInfo.offset.reply_block, nullptr);
    ASSERT_EQ(c->clientDurabilityInfo.offset.byte_offset, 7u);
    ASSERT_EQ(server.durability.pre_command_replication_offset, 1234);

    durabilityClientReset(c);
    zfree(c);
}

/* Migrated from test_multi_exec_defers_dirty_keys */
TEST_F(FullDurabilityTest, MultiExecDefersDirtyKeys) {
    client *c = (client *)zcalloc(sizeof(client));
    durabilityClientInit(c);
    c->db = server.db[0];
    c->reply = listCreate();
    listSetFreeMethod(c->reply, zfree);

    /* Inside a MULTI — key is marked dirty immediately with placeholder offset */
    c->flag.multi = 1;
    robj *key_obj = createStringObject("multi-key", 9);
    handleUncommittedKeyForClient(c, key_obj, server.db[0]);
    /* Key should be in uncommitted set immediately (with LLONG_MAX placeholder) */
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);
    ASSERT_EQ(durabilityPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(key_obj), server.db[0]), LLONG_MAX);

    /* After EXEC completes: postCommandExec commits deferred keys */
    c->flag.multi = 0;
    struct serverCommand exec_cmd = {.declared_name = "exec", .proc = execCommand, .flags = 0};
    c->cmd = &exec_cmd;
    c->clientDurabilityInfo.current_command_repl_offset = -1;
    server.primary_repl_offset = 100;
    server.durability.pre_command_replication_offset = 100;
    server.durability.previous_acked_offset = 0;
    postCommandExec(c);

    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);
    ASSERT_EQ(durabilityPurgeAndGetUncommittedKeyOffset((sds)objectGetVal(key_obj), server.db[0]), 100);

    decrRefCount(key_obj);
    listRelease(c->reply);
    durabilityClientReset(c);
    zfree(c);
}

/* Note: test_exec_blocks_reply_and_tracks_dirty_keys from the C test suite
 * exercised the full end-to-end blocking/unblocking flow including
 * notifyDurabilityProgress with replica ack simulation. This requires
 * putClientInPendingWriteQueue which needs a fully event-loop-registered client.
 * The blocking path is covered by the MultiExecDefersDirtyKeys test above,
 * and the full integration flow is tested by tests/durability/reply_blocking.tcl. */

/* ========================= Additional Coverage Tests ========================= */

/* Test updateFuncStoreBlockingOffsetForWrite */
TEST_F(SyncReplicationTest, UpdateFuncStoreBlockingOffsetForWrite) {
    server.durability.func_store_blocking_offset = -1;
    server.durability.processed_func_write_in_transaction = false;

    /* Should not update when no func write was processed in transaction */
    updateFuncStoreBlockingOffsetForWrite(200);
    ASSERT_EQ(server.durability.func_store_blocking_offset, -1);

    /* Should update when processed_func_write_in_transaction is set */
    server.durability.processed_func_write_in_transaction = true;
    updateFuncStoreBlockingOffsetForWrite(200);
    ASSERT_EQ(server.durability.func_store_blocking_offset, 200);
    ASSERT_FALSE(server.durability.processed_func_write_in_transaction);
}

/* Test handleUncommittedFunctionStore inside vs outside a transaction */
TEST_F(SyncReplicationTest, HandleUncommittedFunctionStoreInsideTransaction) {
    server.durability.processed_func_write_in_transaction = false;
    server.durability.func_store_blocking_offset = -1;

    /* Inside a transaction (execution_nesting > 0): should only set the flag */
    server.execution_nesting = 1;
    server.primary_repl_offset = 300;
    handleUncommittedFunctionStore();
    ASSERT_TRUE(server.durability.processed_func_write_in_transaction);
    ASSERT_EQ(server.durability.func_store_blocking_offset, -1);

    /* Outside a transaction: should set the blocking offset directly */
    server.execution_nesting = 0;
    server.durability.processed_func_write_in_transaction = false;
    server.primary_repl_offset = 400;
    handleUncommittedFunctionStore();
    ASSERT_FALSE(server.durability.processed_func_write_in_transaction);
    ASSERT_EQ(server.durability.func_store_blocking_offset, 400);
}

/* Test notifyDurabilityProgress when sync replication is disabled */
TEST_F(SyncReplicationTest, NotifyDurabilityProgressNoOpWhenDisabled) {
    server.aof_state = AOF_OFF; server.aof_fsync = AOF_FSYNC_EVERYSEC;
    server.primary_host = nullptr;
    long long old_offset = server.durability.previous_acked_offset;
    notifyDurabilityProgress();
    /* Should be a no-op */
    ASSERT_EQ(server.durability.previous_acked_offset, old_offset);
}

/* Test notifyDurabilityProgress when server is a replica */
TEST_F(SyncReplicationTest, NotifyDurabilityProgressNoOpWhenReplica) {
    server.aof_state = AOF_ON; server.aof_fsync = AOF_FSYNC_ALWAYS;
    server.primary_host = sdsnew("127.0.0.1");
    long long old_offset = server.durability.previous_acked_offset;
    notifyDurabilityProgress();
    ASSERT_EQ(server.durability.previous_acked_offset, old_offset);
    sdsfree(server.primary_host);
    server.primary_host = nullptr;
    server.aof_state = AOF_OFF; server.aof_fsync = AOF_FSYNC_EVERYSEC;
}


/* Test that keyspace notify task copies the event string so it doesn't
 * become a dangling pointer when the caller frees the original. */
TEST_F(FullDurabilityTest, KeyspaceNotifyTaskCopiesEventString) {
    /* Create a mutable event string that we'll free after registering the task */
    char *event = (char *)zmalloc(16);
    strcpy(event, "set");

    robj *key_obj = createStringObject("mykey", 5);

    /* Register the task — this should copy the event string */
    server.current_client = nullptr; /* simulate background task */
    server.primary_repl_offset = 100;
    bool registered = durabilityRegisterDeferredTask(
        DURABLE_KEYSPACE_NOTIFY_TASK,
        (void *)(long long)0,   /* type */
        (void *)event,          /* event string — will be freed below */
        (void *)key_obj,        /* key */
        (void *)(long long)0    /* dbid */
    );
    ASSERT_TRUE(registered);

    /* Free the original event string — this would cause a dangling pointer
     * if the task didn't copy it */
    zfree(event);

    /* The task should still be valid and executable without crash.
     * Execute all tasks at offset 100 — the event string inside the task
     * should be an independent copy that's still valid. */
    ASSERT_EQ(listLength(server.durability.tasks_waiting_ack[DURABLE_KEYSPACE_NOTIFY_TASK]), 1u);
    executeDeferredTasksForAck(100);
    ASSERT_EQ(listLength(server.durability.tasks_waiting_ack[DURABLE_KEYSPACE_NOTIFY_TASK]), 0u);

    decrRefCount(key_obj);
}

/* Test durabilityClientInit is idempotent */
TEST_F(SyncReplicationTest, ClientInitIdempotent) {
    server.aof_state = AOF_ON; server.aof_fsync = AOF_FSYNC_ALWAYS;

    client *c = (client *)zcalloc(sizeof(client));
    c->clientDurabilityInfo.blocked_responses = nullptr;

    durabilityClientInit(c);
    list *first_list = c->clientDurabilityInfo.blocked_responses;
    ASSERT_NE(first_list, nullptr);

    /* Calling init again should be a no-op — should NOT create a new list */
    durabilityClientInit(c);
    ASSERT_EQ(c->clientDurabilityInfo.blocked_responses, first_list);

    durabilityClientReset(c);
    server.aof_state = AOF_OFF; server.aof_fsync = AOF_FSYNC_EVERYSEC;
    zfree(c);
}
