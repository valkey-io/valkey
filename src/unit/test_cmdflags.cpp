/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "server.h"
}
extern hashtableType commandSetType;
extern hashtableType originalCommandSetType;
extern struct serverCommand serverCommandTable[];


class CmdFlagsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        /* populateCommandTable() fills the static command table, and asserts
         * if it runs twice in one process. Free what an earlier suite (for
         * example BgIterationTest) populated first. */
        releaseCommandTable();
        server.commands = hashtableCreate(&commandSetType);
        server.orig_commands = hashtableCreate(&originalCommandSetType);
        populateCommandTable();
    }

    void TearDown() override {
        releaseCommandTable();
    }

    static void releaseCommandTable(void) {
        for (struct serverCommand *c = serverCommandTable; c->declared_name; c++) {
            if (c->subcommands_ht) {
                hashtableRelease(c->subcommands_ht);
                c->subcommands_ht = NULL;
            }
        }
        if (server.commands) hashtableRelease(server.commands);
        if (server.orig_commands) hashtableRelease(server.orig_commands);
        server.commands = NULL;
        server.orig_commands = NULL;
    }
};


TEST_F(CmdFlagsTest, TestWriteFirstkeyOnly) {
    /* Each command with this flag is explicitly listed here to ensure:
     *   - new commands are not mistakenly detected as write firstkey only
     *   - commands which should be write firstkey only are detected. */
    const char *const writeFirstkeyCommands[] = {
        "bitop", "geosearchstore", "pfmerge", "sdiffstore", "sinterstore",
        "sunionstore", "zdiffstore", "zinterstore", "zrangestore", "zunionstore"};
    int expectedCount = sizeof(writeFirstkeyCommands) / sizeof(char *);

    int count = 0;

    hashtableIterator iter;
    hashtableInitIterator(&iter, server.commands, 0);
    struct serverCommand *c;
    while (hashtableNext(&iter, (void **)&c)) {
        if (c->flags & CMD_WRITE_FIRSTKEY_ONLY) {
            count++;
            bool found = false;
            for (int i = 0; i < expectedCount; i++) {
                if (strcmp(c->declared_name, writeFirstkeyCommands[i]) == 0) {
                    found = true;
                    break;
                }
            }
            EXPECT_TRUE(found);
        }
    }
    hashtableCleanupIterator(&iter);

    EXPECT_EQ(count, expectedCount);
}
