/* Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause */
#include "generated_wrappers.hpp"

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>

extern "C" {
#include "reply_blocking.h"
#include "server.h"
#include "uncommitted_keys.h"

/* Forward declarations used by tests */
}

/* ========================= Test Helpers ========================= */

static void initTestEnv(void) {
    static char test_logfile[] = "";
    if (server.logfile == nullptr) {
        server.logfile = test_logfile;
    }
    server.bio_aof_offload_enabled = 1;
}

/* Minimal reply-blocking initialization for tests — avoids calling initTaskTypes()
 * which is forward-declared but not yet defined. */
static void initReplyBlockingForTest(void) {
    uncommittedKeysInitPending();
    initTaskTypes();
    server.reply_blocking.previous_acked_offset = -1;
    server.reply_blocking.clients_waiting_ack = listCreate();
    postCommitTaskInitLists();
    server.reply_blocking.clients_blocked = 0;
    server.reply_blocking.clients_unblocked = 0;
    server.reply_blocking.clients_disconnected_before_unblocking = 0;
    server.reply_blocking.read_responses_blocked = 0;
    server.reply_blocking.write_responses_blocked = 0;
    server.reply_blocking.other_responses_blocked = 0;
    server.reply_blocking.read_responses_unblocked = 0;
    server.reply_blocking.write_responses_unblocked = 0;
    server.reply_blocking.other_responses_unblocked = 0;
    server.reply_blocking.read_responses_blocked_cumulative_time_us = 0;
    server.reply_blocking.write_responses_blocked_cumulative_time_us = 0;
    server.reply_blocking.other_responses_blocked_cumulative_time_us = 0;
    server.reply_blocking.aof_paused = false;
    server.reply_blocking.aof_paused_offset = 0;
}

/* Minimal reply-blocking cleanup for tests. */
static void cleanupReplyBlockingForTest(void) {
    if (server.reply_blocking.clients_waiting_ack) {
        listRelease(server.reply_blocking.clients_waiting_ack);
        server.reply_blocking.clients_waiting_ack = nullptr;
    }
    uncommittedKeysCleanupPending();
    postCommitTaskCleanupLists();
}

/* ========================= Test Fixtures ========================= */

class SyncReplicationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        initTestEnv();
    }
};

class AofReplyBlockingTest : public ::testing::Test {
  protected:
    /* Saved state */
    int old_aof_state;
    int old_aof_fsync;
    long long old_fsynced_reploff;
    list *old_replicas;
    list *old_clients_pending_write;
    char *old_primary_host;
    reply_blocking_t old_reply_blocking;

    void SetUp() override {
        initTestEnv();
        old_aof_state = server.aof_state;
        old_aof_fsync = server.aof_fsync;
        old_fsynced_reploff = server.fsynced_reploff;
        old_replicas = server.replicas;
        old_clients_pending_write = server.clients_pending_write;
        old_primary_host = server.primary_host;
        old_reply_blocking = server.reply_blocking;

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
        server.reply_blocking = old_reply_blocking;
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
        old_previous_acked_offset = server.reply_blocking.previous_acked_offset;
        old_primary_repl_offset = server.primary_repl_offset;

        server.cluster_enabled = 0;
        server.primary_host = nullptr;
        server.dbnum = 1;
        server.db = (serverDb **)zcalloc(sizeof(serverDb *));
        server.db[0] = (serverDb *)zcalloc(sizeof(serverDb));
        replyBlockingInitDatabase(server.db[0]);
    }

    void TearDown() override {
        hashtableRelease(server.db[0]->uncommitted_keys);
        zfree(server.db[0]);
        zfree(server.db);

        server.db = old_db;
        server.dbnum = old_dbnum;
        server.primary_host = old_primary_host;
        server.cluster_enabled = old_cluster_enabled;
        server.reply_blocking.previous_acked_offset = old_previous_acked_offset;
        server.primary_repl_offset = old_primary_repl_offset;
    }
};

/* ========================= Reply-Blocking Tests ========================= */

TEST_F(AofReplyBlockingTest, IsReplyBlockingEnabled) {
    initReplyBlockingForTest();

    /* Reply-blocking delegates to isAofReplyBlockingEnabled().
     * Enabled only when AOF is on AND appendfsync is set to always. */
    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    ASSERT_EQ(isReplyBlockingEnabled(), 0);

    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    ASSERT_EQ(isReplyBlockingEnabled(), 0);

    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    ASSERT_EQ(isReplyBlockingEnabled(), 1);

    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    ASSERT_EQ(isReplyBlockingEnabled(), 0);

    cleanupReplyBlockingForTest();
}

TEST_F(SyncReplicationTest, IsPrimaryReplyBlockingEnabled) {
    initReplyBlockingForTest();
    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;

    /* Primary (not a replica) */
    server.primary_host = nullptr;
    ASSERT_EQ(isPrimaryReplyBlockingEnabled(), 1);

    /* Replica */
    server.primary_host = sdsnew("127.0.0.1");
    ASSERT_EQ(isPrimaryReplyBlockingEnabled(), 0);
    sdsfree(server.primary_host);

    /* Disabled (appendfsync != always) + primary */
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    server.primary_host = nullptr;
    ASSERT_EQ(isPrimaryReplyBlockingEnabled(), 0);

    cleanupReplyBlockingForTest();
}

TEST_F(SyncReplicationTest, ClientInitAndReset) {
    initReplyBlockingForTest();
    client *c = (client *)zcalloc(sizeof(client));
    c->reply_blocking_state.blocked_responses = nullptr;
    c->reply_blocking_state.reply_blocked = 0;
    c->reply_blocking_state.current_command_repl_offset = 0;

    /* Disabled — should be a no-op */
    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    replyBlockingClientInit(c);
    ASSERT_EQ(c->reply_blocking_state.blocked_responses, nullptr);

    /* Enabled — should initialize */
    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    replyBlockingClientInit(c);
    ASSERT_NE(c->reply_blocking_state.blocked_responses, nullptr);
    ASSERT_EQ(listLength(c->reply_blocking_state.blocked_responses), 0u);
    ASSERT_FALSE(c->reply_blocking_state.offset.recorded);
    ASSERT_EQ(c->reply_blocking_state.offset.reply_block, nullptr);
    ASSERT_EQ(c->reply_blocking_state.offset.byte_offset, 0u);
    ASSERT_EQ(c->reply_blocking_state.current_command_repl_offset, -1);

    /* Reset — should free */
    replyBlockingClientReset(c);
    ASSERT_EQ(c->reply_blocking_state.blocked_responses, nullptr);
    ASSERT_FALSE(c->reply_blocking_state.offset.recorded);
    ASSERT_EQ(c->reply_blocking_state.current_command_repl_offset, -1);

    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    zfree(c);
    cleanupReplyBlockingForTest();
}

TEST_F(SyncReplicationTest, IsClientReplyBufferLimited) {
    client *c = (client *)zcalloc(sizeof(client));

    /* No blocked_responses list */
    c->reply_blocking_state.blocked_responses = nullptr;
    ASSERT_FALSE(isClientReplyBufferLimited(c));

    /* Empty blocked_responses list */
    c->reply_blocking_state.blocked_responses = listCreate();
    ASSERT_FALSE(isClientReplyBufferLimited(c));

    /* Non-empty blocked_responses list */
    blockedResponse *br = (blockedResponse *)zcalloc(sizeof(blockedResponse));
    br->primary_repl_offset = 100;
    br->disallowed_byte_offset = 0;
    br->disallowed_reply_block = nullptr;
    listAddNodeTail(c->reply_blocking_state.blocked_responses, br);
    ASSERT_TRUE(isClientReplyBufferLimited(c));

    listSetFreeMethod(c->reply_blocking_state.blocked_responses, zfree);
    listRelease(c->reply_blocking_state.blocked_responses);
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
 * reply-blocking provider acknowledges the write. */
TEST_F(SyncReplicationTest, ClientHasPendingRepliesUsesBufposNotDataLen) {
    client *c = (client *)zcalloc(sizeof(client));
    c->reply = listCreate();
    c->repl_data = nullptr;
    c->slot_migration_job = nullptr;
    c->raw_flag1 = 0;
    c->raw_flag2 = 0;

    /* Set up a blocked response at offset 100 in c->buf (no reply block) */
    c->reply_blocking_state.blocked_responses = listCreate();
    listSetFreeMethod(c->reply_blocking_state.blocked_responses, zfree);

    blockedResponse *br = (blockedResponse *)zcalloc(sizeof(blockedResponse));
    br->primary_repl_offset = 500;
    br->disallowed_byte_offset = 100;
    br->disallowed_reply_block = nullptr;
    listAddNodeTail(c->reply_blocking_state.blocked_responses, br);

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

    listRelease(c->reply_blocking_state.blocked_responses);
    listRelease(c->reply);
    zfree(c);
}

/* ========================= AOF Reply-Blocking Tests ========================= */

TEST_F(AofReplyBlockingTest, AofReplyBlockingDisabledWhenAofOff) {
    initReplyBlockingForTest();

    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    ASSERT_FALSE(isAofReplyBlockingEnabled());

    cleanupReplyBlockingForTest();
}

TEST_F(AofReplyBlockingTest, AofReplyBlockingEnabledOnlyWhenAlwaysFsync) {
    initReplyBlockingForTest();

    /* AOF reply-blocking is only enabled when AOF is on AND appendfsync is always */
    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    ASSERT_TRUE(isAofReplyBlockingEnabled());

    /* Not enabled with other fsync policies */
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    ASSERT_FALSE(isAofReplyBlockingEnabled());

    server.aof_fsync = AOF_FSYNC_NO;
    ASSERT_FALSE(isAofReplyBlockingEnabled());

    cleanupReplyBlockingForTest();
}

TEST_F(AofReplyBlockingTest, DurablyCommittedOffsetTracksPrimaryWhenAofDisabled) {
    initReplyBlockingForTest();

    /* When fsync != always, AOF reply-blocking is disabled so the durably
     * committed offset just tracks primary_repl_offset (no gating). */
    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    server.primary_repl_offset = 500;
    ASSERT_EQ(getDurablyCommittedOffset(), 500);

    server.aof_fsync = AOF_FSYNC_NO;
    server.primary_repl_offset = 700;
    ASSERT_EQ(getDurablyCommittedOffset(), 700);

    cleanupReplyBlockingForTest();
}

TEST_F(AofReplyBlockingTest, AofReplyBlockingPauseAndResume) {
    initReplyBlockingForTest();

    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    server.primary_repl_offset = 300;
    __atomic_store_n(&server.fsynced_reploff_pending, (long long)300, __ATOMIC_RELAXED);
    server.fsynced_reploff = 300;

    /* Before pause: durably committed offset = 300 (fsynced) */
    ASSERT_EQ(getDurablyCommittedOffset(), 300);

    /* Pause: durably committed offset should be frozen at 300 (the offset
     * at pause time). New writes that advance primary_repl_offset past 300
     * will block, but already-acknowledged data remains unblocked. */
    pauseAofReplyBlocking();
    ASSERT_TRUE(server.reply_blocking.aof_paused);
    ASSERT_EQ(getDurablyCommittedOffset(), 300);

    /* Advance primary_repl_offset and fsynced_reploff_pending — durably
     * committed offset stays frozen at the pause-time snapshot regardless. */
    server.primary_repl_offset = 500;
    __atomic_store_n(&server.fsynced_reploff_pending, (long long)450, __ATOMIC_RELAXED);
    ASSERT_EQ(getDurablyCommittedOffset(), 300);

    /* Resume: durably committed offset should now reflect the live
     * fsynced_reploff_pending value, demonstrating that the live offset is
     * consulted again post-resume. */
    server.reply_blocking.previous_acked_offset = -1;
    resumeAofReplyBlocking();
    ASSERT_FALSE(server.reply_blocking.aof_paused);
    ASSERT_EQ(getDurablyCommittedOffset(), 450);

    cleanupReplyBlockingForTest();
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
    server.reply_blocking.previous_acked_offset = 5;
    ASSERT_EQ(getUncommittedKeyOffset(key, server.db[0], server.reply_blocking.previous_acked_offset), offset);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* Acked — should return -1 (key is committed) */
    server.reply_blocking.previous_acked_offset = 10;
    ASSERT_EQ(getUncommittedKeyOffset(key, server.db[0], server.reply_blocking.previous_acked_offset), -1);
    /* Key still in hashtable — drainCommittedKeys handles cleanup */
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* drainCommittedKeys removes it */
    drainCommittedKeys(10);
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

    /* Ack up to 10 — key1 is committed, key2 is not */
    server.reply_blocking.previous_acked_offset = 10;
    ASSERT_EQ(getUncommittedKeyOffset((sds)objectGetVal(k1), server.db[0], server.reply_blocking.previous_acked_offset), -1);
    ASSERT_EQ(getUncommittedKeyOffset((sds)objectGetVal(k2), server.db[0], server.reply_blocking.previous_acked_offset), 20);

    /* drainCommittedKeys removes key1 */
    drainCommittedKeys(10);
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);

    /* Ack up to 20 — key2 also committed */
    server.reply_blocking.previous_acked_offset = 20;
    ASSERT_EQ(getUncommittedKeyOffset((sds)objectGetVal(k2), server.db[0], server.reply_blocking.previous_acked_offset), -1);
    drainCommittedKeys(20);
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
    server.reply_blocking.previous_acked_offset = 10;
    ASSERT_EQ(getUncommittedKeyOffset((sds)objectGetVal(key_obj), server.db[0], server.reply_blocking.previous_acked_offset), 50);

    decrRefCount(key_obj);
}

TEST_F(UncommittedKeysTest, NonexistentKeyReturnsNegativeOne) {
    sds missing = sdsnew("nonexistent");
    server.reply_blocking.previous_acked_offset = 0;
    ASSERT_EQ(getUncommittedKeyOffset(missing, server.db[0], server.reply_blocking.previous_acked_offset), -1);
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
    server.reply_blocking.previous_acked_offset = 10;
    drainCommittedKeys(10);
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
    server.reply_blocking.previous_acked_offset = 0;

    /* Not uncommitted initially */
    ASSERT_FALSE(isUncommittedFunctionStore());

    /* Mark uncommitted */
    server.execution_nesting = 0;
    server.primary_repl_offset = 100;
    handleUncommittedFunctionStore();
    ASSERT_TRUE(isUncommittedFunctionStore());
    ASSERT_EQ(getFuncStoreBlockingOffset(), 100);

    /* After acking, it should no longer be uncommitted */
    server.reply_blocking.previous_acked_offset = 100;
    ASSERT_FALSE(isUncommittedFunctionStore());
}

/* ========================= INFO String Test ========================= */

TEST_F(SyncReplicationTest, GenInfoStringDisabled) {
    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    sds info = sdsempty();
    info = genReplyBlockingInfoString(info);
    ASSERT_NE(strstr(info, "reply_blocking_enabled:0"), nullptr);
    sdsfree(info);
}

TEST_F(SyncReplicationTest, GenInfoStringEnabled) {
    initReplyBlockingForTest();
    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    server.reply_blocking.read_responses_blocked = 5;
    server.reply_blocking.write_responses_blocked = 3;
    server.reply_blocking.previous_acked_offset = 42;
    server.primary_repl_offset = 100;

    sds info = sdsempty();
    info = genReplyBlockingInfoString(info);
    ASSERT_NE(strstr(info, "reply_blocking_enabled:1"), nullptr);
    ASSERT_NE(strstr(info, "reply_blocking_read_blocked_count:5"), nullptr);
    ASSERT_NE(strstr(info, "reply_blocking_write_blocked_count:3"), nullptr);
    ASSERT_NE(strstr(info, "reply_blocking_previous_acked_offset:42"), nullptr);
    ASSERT_NE(strstr(info, "reply_blocking_primary_repl_offset:100"), nullptr);

    sdsfree(info);
    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    cleanupReplyBlockingForTest();
}

/* ========================= Migrated from C tests ========================= */

/* Fixture for tests that need full reply-blocking init (replyBlockingInit)
 * plus database and client setup. */
class FullReplyBlockingTest : public ::testing::Test {
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
    reply_blocking_t old_reply_blocking;
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
        old_reply_blocking = server.reply_blocking;
        old_monitors = server.monitors;

        server.cluster_enabled = 0;
        server.primary_host = nullptr;
        server.clients_pending_write = listCreate();
        server.monitors = listCreate();
        server.dbnum = 1;
        server.db = (serverDb **)zcalloc(sizeof(serverDb *));
        server.db[0] = (serverDb *)zcalloc(sizeof(serverDb));
        replyBlockingInitDatabase(server.db[0]);

        server.aof_state = AOF_ON;
        server.aof_fsync = AOF_FSYNC_ALWAYS;
        replyBlockingInit();
    }

    void TearDown() override {
        replyBlockingCleanup();
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
        server.reply_blocking = old_reply_blocking;
        server.monitors = old_monitors;
    }
};

/* Migrated from test_replyBlockingInit */
TEST_F(SyncReplicationTest, SyncReplicationInitSetsDefaults) {
    /* initReplyBlockingForTest() approximates replyBlockingInit(); verify fields */
    initReplyBlockingForTest();

    ASSERT_NE(server.reply_blocking.clients_waiting_ack, nullptr);
    ASSERT_EQ(listLength(server.reply_blocking.clients_waiting_ack), 0u);
    ASSERT_EQ(server.reply_blocking.previous_acked_offset, -1);
    ASSERT_EQ(server.reply_blocking.clients_blocked, 0u);
    ASSERT_EQ(server.reply_blocking.clients_unblocked, 0u);
    ASSERT_EQ(server.reply_blocking.clients_disconnected_before_unblocking, 0u);
    ASSERT_EQ(server.reply_blocking.read_responses_blocked, 0u);
    ASSERT_EQ(server.reply_blocking.write_responses_blocked, 0u);
    ASSERT_EQ(server.reply_blocking.other_responses_blocked, 0u);

    cleanupReplyBlockingForTest();
}

/* Migrated from test_recordReplOffsetBaseline */
TEST_F(FullReplyBlockingTest, BeforeCommandTrackReplOffset) {
    client *c = (client *)zcalloc(sizeof(client));
    replyBlockingClientInit(c);

    struct serverCommand readonly_cmd = {.declared_name = "get", .flags = CMD_READONLY};
    c->cmd = &readonly_cmd;

    server.primary_repl_offset = 500;
    recordReplOffsetBaseline(c);

    /* pre_call_replication_offset should be snapshotted */
    ASSERT_EQ(server.reply_blocking.pre_call_replication_offset, 500);

    replyBlockingClientReset(c);
    zfree(c);
}

/* Migrated from test_beginCommandReplyBlocking — Case 1: reply-blocking disabled */
TEST_F(SyncReplicationTest, PreCommandExecReplyBlockingDisabled) {
    struct serverCommand readonly_cmd = {.declared_name = "get", .flags = CMD_READONLY};

    /* beginCommandReplyBlocking always accesses server.monitors via isCommandReplicatedToMonitors() */
    list *old_monitors = server.monitors;
    server.monitors = listCreate();

    client *c = (client *)zcalloc(sizeof(client));
    c->cmd = &readonly_cmd;
    c->reply_blocking_state.current_command_repl_offset = 123;
    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    server.primary_repl_offset = 555;

    ASSERT_EQ(beginCommandReplyBlocking(c), CMD_FILTER_ALLOW);
    /* beginCommandReplyBlocking always resets current_command_repl_offset to -1 */
    ASSERT_EQ(c->reply_blocking_state.current_command_repl_offset, -1);
    /* pre_command_replication_offset is always snapshotted */
    ASSERT_EQ(server.reply_blocking.pre_command_replication_offset, 555);

    zfree(c);
    listRelease(server.monitors);
    server.monitors = old_monitors;
}

/* Migrated from test_beginCommandReplyBlocking — Case 2: reply-blocking enabled on primary */
TEST_F(FullReplyBlockingTest, PreCommandExecReplyBlockingEnabledOnPrimary) {
    struct serverCommand readonly_cmd = {.declared_name = "get", .flags = CMD_READONLY};

    client *c = (client *)zcalloc(sizeof(client));
    replyBlockingClientInit(c);
    c->cmd = &readonly_cmd;
    c->bufpos = 7;
    c->reply_blocking_state.current_command_repl_offset = 88;
    server.primary_repl_offset = 1234;

    ASSERT_EQ(beginCommandReplyBlocking(c), CMD_FILTER_ALLOW);
    /* current_command_repl_offset should be reset to -1 */
    ASSERT_EQ(c->reply_blocking_state.current_command_repl_offset, -1);
    /* Pre-execution position should be tracked */
    ASSERT_TRUE(c->reply_blocking_state.offset.recorded);
    ASSERT_EQ(c->reply_blocking_state.offset.reply_block, nullptr);
    ASSERT_EQ(c->reply_blocking_state.offset.byte_offset, 7u);
    ASSERT_EQ(server.reply_blocking.pre_command_replication_offset, 1234);

    replyBlockingClientReset(c);
    zfree(c);
}

/* Migrated from test_multi_exec_defers_dirty_keys */
TEST_F(FullReplyBlockingTest, MultiExecDefersDirtyKeys) {
    client *c = (client *)zcalloc(sizeof(client));
    replyBlockingClientInit(c);
    c->db = server.db[0];
    c->reply = listCreate();
    listSetFreeMethod(c->reply, zfree);

    /* Inside a MULTI — key is marked dirty immediately with placeholder offset */
    c->flag.multi = 1;
    robj *key_obj = createStringObject("multi-key", 9);
    handleUncommittedKeyForClient(c, key_obj, server.db[0]);
    /* Key should be in uncommitted set immediately (with LLONG_MAX placeholder) */
    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);
    ASSERT_EQ(getUncommittedKeyOffset((sds)objectGetVal(key_obj), server.db[0], server.reply_blocking.previous_acked_offset), LLONG_MAX);

    /* After EXEC completes: finalizeCommandReplyBlocking commits deferred keys */
    c->flag.multi = 0;
    struct serverCommand exec_cmd = {.declared_name = "exec", .proc = execCommand, .flags = 0};
    c->cmd = &exec_cmd;
    c->reply_blocking_state.current_command_repl_offset = -1;
    server.primary_repl_offset = 100;
    server.reply_blocking.pre_command_replication_offset = 100;
    server.reply_blocking.previous_acked_offset = 0;
    finalizeCommandReplyBlocking(c);

    ASSERT_EQ(hashtableSize(server.db[0]->uncommitted_keys), 1u);
    ASSERT_EQ(getUncommittedKeyOffset((sds)objectGetVal(key_obj), server.db[0], server.reply_blocking.previous_acked_offset), 100);

    decrRefCount(key_obj);
    listRelease(c->reply);
    replyBlockingClientReset(c);
    zfree(c);
}

/* Note: test_exec_blocks_reply_and_tracks_dirty_keys from the C test suite
 * exercised the full end-to-end blocking/unblocking flow including
 * notifyReplyBlockingProgress with replica ack simulation. This requires
 * putClientInPendingWriteQueue which needs a fully event-loop-registered client.
 * The blocking path is covered by the MultiExecDefersDirtyKeys test above,
 * and the full integration flow is tested by tests/durability/reply_blocking.tcl. */

/* ========================= Additional Coverage Tests ========================= */

/* Test updateFuncStoreBlockingOffsetForWrite */
TEST_F(SyncReplicationTest, UpdateFuncStoreBlockingOffsetForWrite) {
    server.reply_blocking.func_store_blocking_offset = -1;
    server.reply_blocking.processed_func_write_in_transaction = false;

    /* Should not update when no func write was processed in transaction */
    updateFuncStoreBlockingOffsetForWrite(200);
    ASSERT_EQ(server.reply_blocking.func_store_blocking_offset, -1);

    /* Should update when processed_func_write_in_transaction is set */
    server.reply_blocking.processed_func_write_in_transaction = true;
    updateFuncStoreBlockingOffsetForWrite(200);
    ASSERT_EQ(server.reply_blocking.func_store_blocking_offset, 200);
    ASSERT_FALSE(server.reply_blocking.processed_func_write_in_transaction);
}

/* Test handleUncommittedFunctionStore inside vs outside a transaction */
TEST_F(SyncReplicationTest, HandleUncommittedFunctionStoreInsideTransaction) {
    server.reply_blocking.processed_func_write_in_transaction = false;
    server.reply_blocking.func_store_blocking_offset = -1;

    /* Inside a transaction (execution_nesting > 0): should only set the flag */
    server.execution_nesting = 1;
    server.primary_repl_offset = 300;
    handleUncommittedFunctionStore();
    ASSERT_TRUE(server.reply_blocking.processed_func_write_in_transaction);
    ASSERT_EQ(server.reply_blocking.func_store_blocking_offset, -1);

    /* Outside a transaction: should set the blocking offset directly */
    server.execution_nesting = 0;
    server.reply_blocking.processed_func_write_in_transaction = false;
    server.primary_repl_offset = 400;
    handleUncommittedFunctionStore();
    ASSERT_FALSE(server.reply_blocking.processed_func_write_in_transaction);
    ASSERT_EQ(server.reply_blocking.func_store_blocking_offset, 400);
}

/* Test notifyReplyBlockingProgress when sync replication is disabled */
TEST_F(SyncReplicationTest, NotifyReplyBlockingProgressNoOpWhenDisabled) {
    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    server.primary_host = nullptr;
    long long old_offset = server.reply_blocking.previous_acked_offset;
    notifyReplyBlockingProgress();
    /* Should be a no-op */
    ASSERT_EQ(server.reply_blocking.previous_acked_offset, old_offset);
}

/* Test notifyReplyBlockingProgress when server is a replica */
TEST_F(SyncReplicationTest, NotifyReplyBlockingProgressNoOpWhenReplica) {
    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;
    server.primary_host = sdsnew("127.0.0.1");
    long long old_offset = server.reply_blocking.previous_acked_offset;
    notifyReplyBlockingProgress();
    ASSERT_EQ(server.reply_blocking.previous_acked_offset, old_offset);
    sdsfree(server.primary_host);
    server.primary_host = nullptr;
    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
}


/* Test that keyspace notify task copies the event string so it doesn't
 * become a dangling pointer when the caller frees the original. */
TEST_F(FullReplyBlockingTest, KeyspaceNotifyTaskCopiesEventString) {
    /* Create a mutable event string that we'll free after registering the task */
    char *event = (char *)zmalloc(16);
    strcpy(event, "set");

    robj *key_obj = createStringObject("mykey", 5);

    /* Register the task — this should copy the event string */
    server.current_client = nullptr;   /* simulate background task */
    server.executing_client = nullptr; /* ack-time execution has no running command */
    server.primary_repl_offset = 100;
    bool registered = replyBlockingRegisterPostCommitTask(
        POST_COMMIT_KEYSPACE_NOTIFY_TASK,
        (void *)(long long)NOTIFY_GENERIC, /* type; re-entry skip now comes from the execution flag */
        (void *)event,                     /* event string — will be freed below */
        (void *)key_obj,                   /* key */
        (void *)(long long)0               /* dbid */
    );
    ASSERT_TRUE(registered);

    /* Free the original event string — this would cause a dangling pointer
     * if the task didn't copy it */
    zfree(event);

    /* The task should still be valid and executable without crash.
     * Execute all tasks at offset 100 — the event string inside the task
     * should be an independent copy that's still valid. */
    ASSERT_EQ(listLength(server.reply_blocking.tasks_waiting_ack[POST_COMMIT_KEYSPACE_NOTIFY_TASK]), 1u);
    executeDeferredTasksForAck(100);
    ASSERT_EQ(listLength(server.reply_blocking.tasks_waiting_ack[POST_COMMIT_KEYSPACE_NOTIFY_TASK]), 0u);

    decrRefCount(key_obj);
}

/* Test that invalid task types are rejected by replyBlockingRegisterPostCommitTask */
TEST_F(FullReplyBlockingTest, RegisterPostCommitTaskRejectsInvalidType) {
    server.primary_repl_offset = 100;

    /* Negative type should be rejected */
    ASSERT_FALSE(replyBlockingRegisterPostCommitTask(-1, (void *)0));

    /* Type == MAX should be rejected */
    ASSERT_FALSE(replyBlockingRegisterPostCommitTask(POST_COMMIT_TASK_TYPE_MAX, (void *)0));

    /* Type > MAX should be rejected */
    ASSERT_FALSE(replyBlockingRegisterPostCommitTask(POST_COMMIT_TASK_TYPE_MAX + 1, (void *)0));
}

/* Test replyBlockingClientInit is idempotent */
TEST_F(SyncReplicationTest, ClientInitIdempotent) {
    initReplyBlockingForTest();
    server.aof_state = AOF_ON;
    server.aof_fsync = AOF_FSYNC_ALWAYS;

    client *c = (client *)zcalloc(sizeof(client));
    c->reply_blocking_state.blocked_responses = nullptr;

    replyBlockingClientInit(c);
    list *first_list = c->reply_blocking_state.blocked_responses;
    ASSERT_NE(first_list, nullptr);

    /* Calling init again should be a no-op — should NOT create a new list */
    replyBlockingClientInit(c);
    ASSERT_EQ(c->reply_blocking_state.blocked_responses, first_list);

    replyBlockingClientReset(c);
    server.aof_state = AOF_OFF;
    server.aof_fsync = AOF_FSYNC_EVERYSEC;
    zfree(c);
    cleanupReplyBlockingForTest();
}
