/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern "C" {
#include "module.h"
#include "rdb.h"
#include "rio.h"
#include "server.h"
void createSharedObjects(void);
}

/* A dual-channel replica cancels its RDB load from the main-channel read
 * handler, which runs inside processEventsWhileBlocked() during the load
 * progress callback, by calling rioCloseASAP(server.loading_rio). These tests
 * load an RDB file with rdbLoad(), the function a disk-loading replica uses,
 * and cancel the load from the same callback after a chosen byte. A cancelled
 * load must fail without running the RDB checker, which exits the process. */

/* Cancel the load in the progress callback of the byte at this offset. */
static long long cancel_at_offset = -1;

static void cancelLoadAtOffset(void) {
    rio *r = server.loading_rio;
    if (r && (long long)r->processed_bytes == cancel_at_offset) rioCloseASAP(r);
}

class RdbLoadCancelTest : public ::testing::Test {
  protected:
    MockValkey mock;
    char path[64];

    static void SetUpTestSuite() {
        monotonicInit();
        bzero(&server, sizeof(server));
        server.hz = 100;
        server.logfile = (char *)"";
        server.verbosity = LL_WARNING;
        createSharedObjects();
        moduleInitModulesSystem();
    }

    void SetUp() override {
        server.dbnum = 1;
        server.db = (serverDb **)zcalloc(sizeof(serverDb *) * server.dbnum);
        /* Run the progress callback on every byte so a test can cancel the
         * load at any offset. */
        server.loading_process_events_interval_bytes = 1;
        cancel_at_offset = -1;
        snprintf(path, sizeof(path), "/tmp/rdb-load-cancel-%d.rdb", (int)getpid());

        EXPECT_CALL(mock, processEventsWhileBlocked()).WillRepeatedly(Invoke(cancelLoadAtOffset));
        /* The RDB checker exits the process, a cancelled load must not run it. */
        EXPECT_CALL(mock, redis_check_rdb_main(_, _, _)).Times(0);
    }

    void TearDown() override {
        unlink(path);
        for (int i = 0; i < server.dbnum; i++) {
            serverDb *db = server.db[i];
            if (!db) continue;
            kvstoreRelease(db->keys);
            kvstoreRelease(db->expires);
            kvstoreRelease(db->keys_with_volatile_items);
            dictRelease(db->blocking_keys);
            dictRelease(db->blocking_keys_unblock_on_nokey);
            dictRelease(db->ready_keys);
            dictRelease(db->watched_keys);
            zfree(db);
        }
        zfree(server.db);
        server.db = NULL;
    }

    void writeRdb(const unsigned char *buf, size_t len) {
        FILE *fp = fopen(path, "w");
        ASSERT_NE(fp, (FILE *)NULL);
        ASSERT_EQ(fwrite(buf, 1, len, fp), len);
        fclose(fp);
    }

    int loadRdb(void) {
        return rdbLoad(path, NULL, RDBFLAGS_REPLICATION | RDBFLAGS_KEEP_CACHE);
    }
};

/* Header, EOF opcode and a zero checksum (no checksum check). */
static const unsigned char empty_rdb[] = {'V', 'A', 'L', 'K', 'E', 'Y', '0', '8', '1', RDB_OPCODE_EOF, 0, 0, 0, 0, 0, 0, 0, 0};

/* The header, then a stream key whose value is cut short. */
static const unsigned char stream_rdb[] = {'V', 'A', 'L', 'K', 'E', 'Y', '0', '8', '1', RDB_TYPE_STREAM_LISTPACKS, 1, 's'};

TEST_F(RdbLoadCancelTest, LoadWithoutCancelSucceeds) {
    writeRdb(empty_rdb, sizeof(empty_rdb));
    EXPECT_EQ(loadRdb(), RDB_OK);
}

/* The next top-level read fails, so the load ends at the loader's eoferr. */
TEST_F(RdbLoadCancelTest, CancelDuringHeaderFailsLoad) {
    writeRdb(empty_rdb, sizeof(empty_rdb));
    cancel_at_offset = 8; /* the last byte of the header */
    EXPECT_EQ(loadRdb(), RDB_FAILED);
}

/* The stream loader reports the failed read itself through rdbReportError(). */
TEST_F(RdbLoadCancelTest, CancelInsideValueFailsLoad) {
    writeRdb(stream_rdb, sizeof(stream_rdb));
    cancel_at_offset = sizeof(stream_rdb) - 1; /* the last byte of the key */
    EXPECT_EQ(loadRdb(), RDB_FAILED);
}

/* No read follows the checksum, so nothing fails a read after the cancel. */
TEST_F(RdbLoadCancelTest, CancelOnFinalReadFailsLoad) {
    writeRdb(empty_rdb, sizeof(empty_rdb));
    cancel_at_offset = sizeof(empty_rdb) - 1; /* the last checksum byte */
    EXPECT_EQ(loadRdb(), RDB_FAILED);
}
