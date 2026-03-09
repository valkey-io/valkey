/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"
extern "C" {
#include "blocked_inuse.h"
}

class BlockedInuseTest : public ::testing::Test {
  protected:
    MockValkey mock;
    RealValkey real;

    static ConnectionType dummyConnType;
    static void SetUpTestSuite() {
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
        server.dbnum = 16;
        server.db = (serverDb **)zcalloc(sizeof(serverDb *) * server.dbnum);
        blockInuse_init();
        dummyConnType.set_read_handler = dummySetReadHandler;
    }

    void SetUp() override {
        server.unblocked_clients = listCreate();
        ASSERT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
        ASSERT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    }

    void TearDown() override {
        ASSERT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
        ASSERT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
        ASSERT_EQ(listLength(server.unblocked_clients), 0UL);
        listRelease(server.unblocked_clients);
        server.unblocked_clients = NULL;
    }

    static void TearDownTestSuite() {
        blockInuse_release();
        zfree(server.db);
    }

    static int dummySetReadHandler(connection *conn, ConnectionCallbackFunc func) {
        conn->read_handler = func;
        return C_OK;
    }

    client *createFakeClient(int client_id) {
        client *c = (client *)zcalloc(sizeof(client));
        c->id = client_id;
        c->conn = (connection *)zcalloc(sizeof(connection));
        c->conn->type = &dummyConnType;
        c->conn->read_handler = (ConnectionCallbackFunc)1;
        return c;
    }

    void freeFakeClient(client *c) {
        if (c->conn) zfree(c->conn);
        zfree(c);
    }

    void verifyClientBlockState(client *c, bool blocked, bool unblocked) {
        EXPECT_EQ(c->flag.blocked, 0u);
        EXPECT_EQ(c->flag.unblocked, unblocked);
        EXPECT_EQ(blockInuse_clientBlocked(c), blocked);
    }
};

ConnectionType BlockedInuseTest::dummyConnType = {0};


TEST_F(BlockedInuseTest, blockInitialState) {
}

TEST_F(BlockedInuseTest, blockClientOnSingleKey) {
    client *c = createFakeClient(1);
    sds key_name = sdsnew("foo");
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), key_name, -1);
    robj *keys[] = {key};

    // Block
    EXPECT_CALL(mock, lookupKeyRead(_, key)).WillOnce(Return(key));
    blockInuse_blockClientOnKeys(c, 1, keys);
    verifyClientBlockState(c, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);
    EXPECT_EQ(key->refcount, 3u);
    EXPECT_EQ(c->conn->read_handler, nullptr);

    // Unblock
    blockInuse_unblockClientsOnKey(key);
    verifyClientBlockState(c, 0, 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_EQ(key->refcount, 1u);
    EXPECT_EQ(listLength(server.unblocked_clients), 1UL);
    EXPECT_EQ(listFirst(server.unblocked_clients)->value, c);

    // Process unblocked client in event loop
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(c)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(c)).Times(1);
    processUnblockedClients();
    verifyClientBlockState(c, 0, 0);
    EXPECT_EQ(key->refcount, 1u);
    EXPECT_EQ(listLength(server.unblocked_clients), 0UL);
    EXPECT_NE(c->conn->read_handler, nullptr);
    sdsfree(key_name);
    decrRefCount(key);
    freeFakeClient(c);
}

TEST_F(BlockedInuseTest, blockClientOnMultipleKeys) {
    client *c = createFakeClient(1);
    sds key1_name = sdsnew("key1");
    sds key2_name = sdsnew("key2");
    robj *key1 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("val1")), key1_name, -1);
    robj *key2 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("val2")), key2_name, -1);
    robj *keys[] = {key1, key2};

    // Block
    EXPECT_CALL(mock, lookupKeyRead(_, key1)).WillOnce(Return(key1));
    EXPECT_CALL(mock, lookupKeyRead(_, key2)).WillOnce(Return(key2));
    blockInuse_blockClientOnKeys(c, 2, keys);
    verifyClientBlockState(c, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 2);
    EXPECT_EQ(key1->refcount, 3u);
    EXPECT_EQ(key2->refcount, 3u);
    EXPECT_EQ(c->conn->read_handler, nullptr);

    // Unblock key1
    blockInuse_unblockClientsOnKey(key1);
    verifyClientBlockState(c, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);
    EXPECT_EQ(key1->refcount, 1u);
    EXPECT_EQ(key2->refcount, 3u);

    // Unblock key2, client gets unblocked
    blockInuse_unblockClientsOnKey(key2);
    verifyClientBlockState(c, 0, 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_EQ(key1->refcount, 1u);
    EXPECT_EQ(key2->refcount, 1u);
    EXPECT_EQ(listLength(server.unblocked_clients), 1UL);
    EXPECT_EQ(listFirst(server.unblocked_clients)->value, c);

    // Process unblocked client in event loop
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(c)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(c)).Times(1);
    processUnblockedClients();
    verifyClientBlockState(c, 0, 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0UL);
    EXPECT_NE(c->conn->read_handler, nullptr);

    EXPECT_EQ(key1->refcount, 1u);
    EXPECT_EQ(key2->refcount, 1u);
    sdsfree(key1_name);
    sdsfree(key2_name);
    decrRefCount(key1);
    decrRefCount(key2);
    freeFakeClient(c);
}

TEST_F(BlockedInuseTest, blockMultipleClientsOnSameKey) {
    client *c1 = createFakeClient(1);
    client *c2 = createFakeClient(2);
    sds key_name = sdsnew("foo");
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), key_name, -1);
    robj *keys[] = {key};

    // Block
    EXPECT_CALL(mock, lookupKeyRead(_, key)).Times(2).WillRepeatedly(Return(key));
    blockInuse_blockClientOnKeys(c1, 1, keys);
    blockInuse_blockClientOnKeys(c2, 1, keys);
    verifyClientBlockState(c1, 1, 0);
    verifyClientBlockState(c2, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 2);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);
    EXPECT_EQ(key->refcount, 4u);
    EXPECT_EQ(c1->conn->read_handler, nullptr);
    EXPECT_EQ(c2->conn->read_handler, nullptr);

    // Unblock
    blockInuse_unblockClientsOnKey(key);
    verifyClientBlockState(c1, 0, 1);
    verifyClientBlockState(c2, 0, 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_EQ(key->refcount, 1u);
    EXPECT_EQ(listLength(server.unblocked_clients), 2UL);

    // Process client in event loop
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(c1)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(c1)).Times(1);
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(c2)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(c2)).Times(1);
    processUnblockedClients();
    verifyClientBlockState(c1, 0, 0);
    verifyClientBlockState(c2, 0, 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0UL);
    EXPECT_NE(c1->conn->read_handler, nullptr);
    EXPECT_NE(c2->conn->read_handler, nullptr);

    EXPECT_EQ(key->refcount, 1u);
    sdsfree(key_name);
    decrRefCount(key);
    freeFakeClient(c1);
    freeFakeClient(c2);
}

TEST_F(BlockedInuseTest, unlinkBlockedClient) {
    client *c = createFakeClient(1);
    sds key_name = sdsnew("foo");
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), key_name, -1);
    robj *keys[] = {key};

    // Block
    EXPECT_CALL(mock, lookupKeyRead(_, key)).WillOnce(Return(key));
    blockInuse_blockClientOnKeys(c, 1, keys);
    verifyClientBlockState(c, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);
    EXPECT_EQ(key->refcount, 3u);
    EXPECT_EQ(c->conn->read_handler, nullptr);

    // Unlink
    blockInuse_unlinkClient(c);
    verifyClientBlockState(c, 0, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0UL);
    EXPECT_EQ(key->refcount, 1u);
    sdsfree(key_name);
    decrRefCount(key);
    freeFakeClient(c);
}

TEST_F(BlockedInuseTest, blockClientOnDuplicateKeys) {
    client *c = createFakeClient(1);
    sds key1_name = sdsnew("foo");
    sds key2_name = sdsnew("foo");
    robj *key1 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), key1_name, -1);
    robj *key2 = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), key2_name, -1);
    robj *keys[] = {key1, key2};

    // Block
    EXPECT_CALL(mock, lookupKeyRead(_, key1)).WillOnce(Return(key1));
    EXPECT_CALL(mock, lookupKeyRead(_, key2)).WillOnce(Return(key2));
    blockInuse_blockClientOnKeys(c, 2, keys);
    verifyClientBlockState(c, 1, 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 1);
    EXPECT_EQ(key1->refcount, 3u);
    EXPECT_EQ(key2->refcount, 1u); // Key is deduplicated, only blocked once
    EXPECT_EQ(c->conn->read_handler, nullptr);

    // Unblock
    blockInuse_unblockClientsOnKey(key1);
    verifyClientBlockState(c, 0, 1);
    EXPECT_EQ(blockInuse_getNumberOfBlockedClients(), 0);
    EXPECT_EQ(blockInuse_getNumberOfBlockedKeys(), 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 1UL);
    EXPECT_EQ(listFirst(server.unblocked_clients)->value, c);

    // Process client in event loop
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(c)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(c)).Times(1);
    processUnblockedClients();
    verifyClientBlockState(c, 0, 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0UL);
    EXPECT_NE(c->conn->read_handler, nullptr);
    EXPECT_EQ(key1->refcount, 1u);
    EXPECT_EQ(key2->refcount, 1u);
    sdsfree(key1_name);
    sdsfree(key2_name);
    decrRefCount(key1);
    decrRefCount(key2);
    freeFakeClient(c);
}

using BlockedInuseDeathTest = BlockedInuseTest;

TEST_F(BlockedInuseDeathTest, initCalledTwice) {
    EXPECT_DEATH({ blockInuse_init(); }, ""); // second call is not allowed
}

TEST_F(BlockedInuseDeathTest, blockingOnKeysReplicaClient) {
    client *c = createFakeClient(1);
    sds key_name = sdsnew("foo");
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), key_name, -1);
    robj *keys[] = {key};

    c->flag.replica = 1;
    EXPECT_DEATH({ blockInuse_blockClientOnKeys(c, 1, keys); }, "");
    sdsfree(key_name);
    decrRefCount(key);
    freeFakeClient(c);
}

TEST_F(BlockedInuseDeathTest, blockingOnKeysNonStringType) {
    client *c = createFakeClient(1);
    sds key_name = sdsnew("foo");
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), key_name, -1);
    robj *keys[] = {key};

    keys[0]->type = OBJ_LIST;
    EXPECT_DEATH({ blockInuse_blockClientOnKeys(c, 1, keys); }, "");
    keys[0]->type = OBJ_STRING;
    sdsfree(key_name);
    decrRefCount(key);
    freeFakeClient(c);
}

TEST_F(BlockedInuseDeathTest, blockingOnKeysZeroKeys) {
    client *c = createFakeClient(1);
    sds key_name = sdsnew("foo");
    robj *key = objectSetKeyAndExpire(createObject(OBJ_STRING, sdsnew("bar")), key_name, -1);
    robj *keys[] = {key};

    EXPECT_DEATH({ blockInuse_blockClientOnKeys(c, 0, keys); }, "");
    sdsfree(key_name);
    decrRefCount(key);
    freeFakeClient(c);
}
