/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"
extern "C" {
#include "server.h"
int getBlockInUseKeyCount(void);
void releaseBlockInUse(void);
}

/* These unit tests were introduced at the time of inuse key blocking. Functions
 * introduced earlier are tested only indirectly through integration tests. */
class BlockedInuseTest : public ::testing::Test {
  protected:
    MockValkey mock;
    RealValkey real;
    static inline ConnectionType dummyConnType = {0};

    static void SetUpTestSuite() {
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
        dummyConnType.set_read_handler = dummySetReadHandler;
    }

    static void TearDownTestSuite() {
        releaseBlockInUse();
    }

    void SetUp() override {
        server.unblocked_clients = listCreate();
        ASSERT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 0u);
        ASSERT_EQ(getBlockInUseKeyCount(), 0);
    }

    void TearDown() override {
        ASSERT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 0u);
        ASSERT_EQ(getBlockInUseKeyCount(), 0);
        ASSERT_EQ(listLength(server.unblocked_clients), 0UL);
        listRelease(server.unblocked_clients);
        server.unblocked_clients = NULL;
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
        c->flag.pending_command = 1;
        return c;
    }

    void freeFakeClient(client *c) {
        freeClientBlockingState(c);
        if (c->conn) zfree(c->conn);
        zfree(c);
    }

    void verifyClientBlockState(client *c, bool blocked, bool unblocked) {
        EXPECT_EQ(c->flag.unblocked, unblocked);
        EXPECT_EQ(c->flag.blocked && c->bstate->btype == BLOCKED_INUSE, blocked);
        if (blocked || unblocked) {
            EXPECT_EQ(c->conn->read_handler, nullptr);
        } else {
            EXPECT_NE(c->conn->read_handler, nullptr);
        }
    }
};

using BlockedInuseDeathTest = BlockedInuseTest;


TEST_F(BlockedInuseTest, blockInitialState) {
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 0u);
    EXPECT_EQ(getBlockInUseKeyCount(), 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0UL);
    ASSERT_NE(server.unblocked_clients, nullptr);
}

TEST_F(BlockedInuseTest, blockClientOnSingleKey) {
    client *c = createFakeClient(1);
    robj *key = createObject(OBJ_STRING, sdsnew("foo"));
    robj *keys[] = {key};

    // Block
    blockClientInUseOnKeys(c, 1, keys);
    verifyClientBlockState(c, 1, 0);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 1u);
    EXPECT_EQ(getBlockInUseKeyCount(), 1);
    EXPECT_EQ(key->refcount, 3u);

    // Unblock
    unblockClientsInUseOnKey(key);
    verifyClientBlockState(c, 0, 1);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 0u);
    EXPECT_EQ(getBlockInUseKeyCount(), 0);
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
    decrRefCount(key);
    freeFakeClient(c);
}

TEST_F(BlockedInuseTest, blockClientClearsLeftoverTimeout) {
    client *c = createFakeClient(1);
    initClientBlockingState(c);
    c->bstate->timeout = 1000;
    robj *key = createObject(OBJ_STRING, sdsnew("foo"));
    robj *keys[] = {key};

    blockClientInUseOnKeys(c, 1, keys);
    EXPECT_EQ(c->bstate->timeout, 0);

    unblockClientsInUseOnKey(key);
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(c)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(c)).Times(1);
    processUnblockedClients();

    decrRefCount(key);
    freeFakeClient(c);
}

TEST_F(BlockedInuseTest, blockClientOnMultipleKeys) {
    client *c = createFakeClient(1);
    robj *key1 = createObject(OBJ_STRING, sdsnew("key1"));
    robj *key2 = createObject(OBJ_STRING, sdsnew("key2"));
    robj *keys[] = {key1, key2};

    // Block
    blockClientInUseOnKeys(c, 2, keys);
    verifyClientBlockState(c, 1, 0);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 1u);
    EXPECT_EQ(getBlockInUseKeyCount(), 2);
    EXPECT_EQ(key1->refcount, 3u);
    EXPECT_EQ(key2->refcount, 3u);

    // Unblock key1
    unblockClientsInUseOnKey(key1);
    verifyClientBlockState(c, 1, 0);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 1u);
    EXPECT_EQ(getBlockInUseKeyCount(), 1);
    EXPECT_EQ(key1->refcount, 1u);
    EXPECT_EQ(key2->refcount, 3u);

    // Unblock key2, client gets unblocked
    unblockClientsInUseOnKey(key2);
    verifyClientBlockState(c, 0, 1);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 0u);
    EXPECT_EQ(getBlockInUseKeyCount(), 0);
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

    EXPECT_EQ(key1->refcount, 1u);
    EXPECT_EQ(key2->refcount, 1u);
    decrRefCount(key1);
    decrRefCount(key2);
    freeFakeClient(c);
}

TEST_F(BlockedInuseTest, blockMultipleClientsOnSameKey) {
    client *c1 = createFakeClient(1);
    client *c2 = createFakeClient(2);
    robj *key = createObject(OBJ_STRING, sdsnew("foo"));
    robj *keys[] = {key};

    // Block
    blockClientInUseOnKeys(c1, 1, keys);
    blockClientInUseOnKeys(c2, 1, keys);
    verifyClientBlockState(c1, 1, 0);
    verifyClientBlockState(c2, 1, 0);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 2u);
    EXPECT_EQ(getBlockInUseKeyCount(), 1);
    EXPECT_EQ(key->refcount, 4u);

    // Unblock
    unblockClientsInUseOnKey(key);
    verifyClientBlockState(c1, 0, 1);
    verifyClientBlockState(c2, 0, 1);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 0u);
    EXPECT_EQ(getBlockInUseKeyCount(), 0);
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

    EXPECT_EQ(key->refcount, 1u);
    decrRefCount(key);
    freeFakeClient(c1);
    freeFakeClient(c2);
}

TEST_F(BlockedInuseTest, unblockBlockedClient) {
    client *c = createFakeClient(1);
    robj *key = createObject(OBJ_STRING, sdsnew("foo"));
    robj *keys[] = {key};

    // Block
    blockClientInUseOnKeys(c, 1, keys);
    verifyClientBlockState(c, 1, 0);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 1u);
    EXPECT_EQ(getBlockInUseKeyCount(), 1);
    EXPECT_EQ(key->refcount, 3u);

    // Unblock client, simulate freeClient
    unblockClient(c, 0);
    EXPECT_FALSE(c->flag.blocked);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 0u);
    EXPECT_EQ(getBlockInUseKeyCount(), 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0UL);
    EXPECT_EQ(key->refcount, 1u);
    decrRefCount(key);
    freeFakeClient(c);
}

TEST_F(BlockedInuseTest, blockClientOnDuplicateKeys) {
    client *c = createFakeClient(1);
    robj *key1 = createObject(OBJ_STRING, sdsnew("foo"));
    robj *key2 = createObject(OBJ_STRING, sdsnew("foo"));
    robj *keys[] = {key1, key2};

    // Block
    blockClientInUseOnKeys(c, 2, keys);
    verifyClientBlockState(c, 1, 0);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 1u);
    EXPECT_EQ(getBlockInUseKeyCount(), 1);
    EXPECT_EQ(key1->refcount, 3u);
    EXPECT_EQ(key2->refcount, 1u); // Key is deduplicated, only blocked once

    // Unblock
    unblockClientsInUseOnKey(key1);
    verifyClientBlockState(c, 0, 1);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 0u);
    EXPECT_EQ(getBlockInUseKeyCount(), 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 1UL);
    EXPECT_EQ(listFirst(server.unblocked_clients)->value, c);

    // Process client in event loop
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(c)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(c)).Times(1);
    processUnblockedClients();
    verifyClientBlockState(c, 0, 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0UL);
    EXPECT_EQ(key1->refcount, 1u);
    EXPECT_EQ(key2->refcount, 1u);
    decrRefCount(key1);
    decrRefCount(key2);
    freeFakeClient(c);
}

TEST_F(BlockedInuseTest, unblockAllKeys) {
    client *c1 = createFakeClient(1);
    client *c2 = createFakeClient(2);
    robj *key1 = createObject(OBJ_STRING, sdsnew("key1"));
    robj *key2 = createObject(OBJ_STRING, sdsnew("key2"));
    robj *keys1[] = {key1};
    robj *keys2[] = {key2};

    // Block c1 on key1, c2 on key2
    blockClientInUseOnKeys(c1, 1, keys1);
    blockClientInUseOnKeys(c2, 1, keys2);
    verifyClientBlockState(c1, 1, 0);
    verifyClientBlockState(c2, 1, 0);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 2u);
    EXPECT_EQ(getBlockInUseKeyCount(), 2);
    EXPECT_EQ(key1->refcount, 3u);
    EXPECT_EQ(key2->refcount, 3u);

    // Unblock all
    unblockClientsInUseOnAllKeys();
    verifyClientBlockState(c1, 0, 1);
    verifyClientBlockState(c2, 0, 1);
    EXPECT_EQ(server.blocked_clients_by_type[BLOCKED_INUSE], 0u);
    EXPECT_EQ(getBlockInUseKeyCount(), 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 2UL);

    // Process clients
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(c1)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(c1)).Times(1);
    EXPECT_CALL(mock, processPendingCommandAndInputBuffer(c2)).WillOnce(Return(C_OK));
    EXPECT_CALL(mock, beforeNextClient(c2)).Times(1);
    processUnblockedClients();
    verifyClientBlockState(c1, 0, 0);
    verifyClientBlockState(c2, 0, 0);
    EXPECT_EQ(listLength(server.unblocked_clients), 0UL);
    EXPECT_EQ(key1->refcount, 1u);
    EXPECT_EQ(key2->refcount, 1u);
    decrRefCount(key1);
    decrRefCount(key2);
    freeFakeClient(c1);
    freeFakeClient(c2);
}

TEST_F(BlockedInuseDeathTest, blockingOnKeysReplicaClient) {
    client *c = createFakeClient(1);
    robj *key = createObject(OBJ_STRING, sdsnew("foo"));
    robj *keys[] = {key};

    c->flag.replica = 1;
    EXPECT_DEATH(blockClientInUseOnKeys(c, 1, keys), "");
    decrRefCount(key);
    freeFakeClient(c);
}

TEST_F(BlockedInuseDeathTest, blockingOnKeysNonStringType) {
    client *c = createFakeClient(1);
    robj *key = createObject(OBJ_STRING, sdsnew("foo"));
    robj *keys[] = {key};

    keys[0]->type = OBJ_LIST;
    EXPECT_DEATH(blockClientInUseOnKeys(c, 1, keys), "");
    keys[0]->type = OBJ_STRING;
    decrRefCount(key);
    freeFakeClient(c);
}

TEST_F(BlockedInuseDeathTest, blockingOnKeysZeroKeys) {
    client *c = createFakeClient(1);
    robj *key = createObject(OBJ_STRING, sdsnew("foo"));
    robj *keys[] = {key};

    EXPECT_DEATH(blockClientInUseOnKeys(c, 0, keys), "");
    decrRefCount(key);
    freeFakeClient(c);
}

TEST_F(BlockedInuseDeathTest, blockingOnKeysWithoutPendingCommand) {
    client *c = createFakeClient(1);
    c->flag.pending_command = 0;
    robj *key = createObject(OBJ_STRING, sdsnew("foo"));
    robj *keys[] = {key};

    EXPECT_DEATH(blockClientInUseOnKeys(c, 1, keys), "");
    decrRefCount(key);
    freeFakeClient(c);
}
