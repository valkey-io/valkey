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


class CmdFlagsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        server.commands = hashtableCreate(&commandSetType);
        server.orig_commands = hashtableCreate(&originalCommandSetType);
        populateCommandTable();
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
