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

    void TearDown() override {
        /* The commands themselves live in the static command table, so only the
         * tables are released. */
        hashtableRelease(server.commands);
        hashtableRelease(server.orig_commands);
        server.commands = NULL;
        server.orig_commands = NULL;
    }

    /* Number of commands with at least one subcommand. */
    static int countCommandsWithSubcommands(void) {
        int count = 0;
        hashtableIterator iter;
        struct serverCommand *c;

        hashtableInitIterator(&iter, server.commands, 0);
        while (hashtableNext(&iter, (void **)&c)) {
            if (c->subcommands_ht != NULL) count++;
        }
        hashtableCleanupIterator(&iter);
        return count;
    }

    static struct serverCommand *findCommand(const char *declared_name) {
        hashtableIterator iter;
        struct serverCommand *c;
        struct serverCommand *found = NULL;

        hashtableInitIterator(&iter, server.commands, 0);
        while (hashtableNext(&iter, (void **)&c)) {
            if (strcmp(c->declared_name, declared_name) == 0) {
                found = c;
                break;
            }
        }
        hashtableCleanupIterator(&iter);
        return found;
    }

    /* Replaces the command tables, as SetUp() does for every test. */
    static void repopulateCommandTable(void) {
        hashtableRelease(server.commands);
        hashtableRelease(server.orig_commands);
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

TEST_F(CmdFlagsTest, TestPopulateCommandTableTwice) {
    /* populateCommandTable() keeps the names and the subcommand tables in the
     * static command table, so a second call used to fail the assertion in
     * commandAddSubcommand(). Test suites run in one process, so more than one
     * suite may populate the table. */
    int commands = (int)hashtableSize(server.commands);
    int with_subcommands = countCommandsWithSubcommands();

    EXPECT_GT(commands, 0);
    EXPECT_GT(with_subcommands, 0);

    repopulateCommandTable();

    EXPECT_EQ((int)hashtableSize(server.commands), commands);
    EXPECT_EQ((int)hashtableSize(server.orig_commands), commands);
    EXPECT_EQ(countCommandsWithSubcommands(), with_subcommands);
}

TEST_F(CmdFlagsTest, TestPopulateCommandTableResetsStats) {
    /* The statistics also live in the static command table, so a suite must not
     * see what an earlier suite counted. */
    struct serverCommand *c = findCommand("get");
    ASSERT_TRUE(c != NULL);

    c->calls = 7;
    c->microseconds = 11;
    c->failed_calls = 3;
    c->rejected_calls = 5;
    updateCommandLatencyHistogram(&c->latency_histogram, 100);
    ASSERT_TRUE(c->latency_histogram != NULL);

    repopulateCommandTable();

    c = findCommand("get");
    ASSERT_TRUE(c != NULL);
    EXPECT_EQ(c->calls, 0);
    EXPECT_EQ(c->microseconds, 0);
    EXPECT_EQ(c->failed_calls, 0);
    EXPECT_EQ(c->rejected_calls, 0);
    EXPECT_TRUE(c->latency_histogram == NULL);
}
