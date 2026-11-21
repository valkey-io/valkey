/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "server.h"
}

/* Swaps in a private created-database list. server.db is not set up, so tests
 * in this fixture must not iterate with forEachCreatedDb. */
class CreatedDbsTest : public ::testing::Test {
  protected:
    int saved_dbnum;
    int *saved_ids;
    int saved_count;

    void SetUp() override {
        saved_dbnum = server.dbnum;
        saved_ids = server.created_db_ids;
        saved_count = server.created_db_count;
        server.dbnum = 16;
        server.created_db_ids = (int *)zcalloc(sizeof(int) * server.dbnum);
        server.created_db_count = 0;
    }

    void TearDown() override {
        zfree(server.created_db_ids);
        server.dbnum = saved_dbnum;
        server.created_db_ids = saved_ids;
        server.created_db_count = saved_count;
    }
};

/* The list is sorted by id whatever the order databases are created in. */
TEST_F(CreatedDbsTest, SortedForAnyCreationOrder) {
    const int order[] = {5, 3, 9, 0, 15, 1};
    for (int id : order) trackCreatedDatabase(id);

    const int expected[] = {0, 1, 3, 5, 9, 15};
    ASSERT_EQ(server.created_db_count, 6);
    for (int i = 0; i < 6; i++) EXPECT_EQ(server.created_db_ids[i], expected[i]);
}

/* Every id up to dbnum fits, created in descending order. */
TEST_F(CreatedDbsTest, HoldsAllDatabases) {
    for (int id = server.dbnum - 1; id >= 0; id--) trackCreatedDatabase(id);

    ASSERT_EQ(server.created_db_count, server.dbnum);
    for (int i = 0; i < server.dbnum; i++) EXPECT_EQ(server.created_db_ids[i], i);
}

/* Databases created in an array other than server.db are not tracked. */
TEST_F(CreatedDbsTest, TempArrayIsNotTracked) {
    serverDb **tmp = (serverDb **)zcalloc(sizeof(serverDb *) * server.dbnum);
    createDatabaseInArrayIfNeeded(tmp, 3);
    createDatabaseInArrayIfNeeded(tmp, 7);

    EXPECT_NE(tmp[3], nullptr);
    EXPECT_NE(tmp[7], nullptr);
    EXPECT_EQ(server.created_db_count, 0);
    discardTempDb(tmp);
}
