/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <string.h>

extern "C" {
#include "functions.h"
#include "server.h"
#include "zmalloc.h"

int rewriteAppendOnlyFileRio(rio *aof);
}

/* A buffer rio whose write fails on the Nth SELECT command header. */
static const char select_header[] = "*2\r\n$6\r\nSELECT\r\n";
static size_t (*buffer_write)(rio *, const void *, size_t);
static int select_writes;
static int fail_on_select;

static size_t failOnSelectWrite(rio *r, const void *buf, size_t len) {
    if (len == sizeof(select_header) - 1 && memcmp(buf, select_header, len) == 0 &&
        ++select_writes == fail_on_select) {
        return 0;
    }
    return buffer_write(r, buf, len);
}

static void addStringKey(serverDb *db, const char *key, const char *val) {
    sds keysds = sdsnew(key);
    robj *o = createStringObjectWithKeyAndExpire(val, strlen(val), keysds, -1);
    sdsfree(keysds);
    ASSERT_TRUE(kvstoreHashtableAdd(db->keys, 0, o));
}

/* Two created databases with only a keys kvstore. Unit tests run one per
 * process, so the global state set here is not restored. */
static void setupTwoDbs(void) {
    functionsInit();
    server.child_info_pipe[1] = -1;
    server.dbnum = 2;
    server.db = (serverDb **)zcalloc(sizeof(serverDb *) * server.dbnum);
    server.created_db_ids = (int *)zcalloc(sizeof(int) * server.dbnum);
    server.created_db_count = server.dbnum;
    server.created_db_capacity = server.dbnum;
    for (int j = 0; j < server.dbnum; j++) {
        server.db[j] = (serverDb *)zcalloc(sizeof(serverDb));
        server.db[j]->id = j;
        server.created_db_ids[j] = j;
        server.db[j]->keys = kvstoreCreate(&kvstoreKeysHashtableType, 0, 0);
    }
}

/* The SELECT for the second non-empty db fails after the iterator of the
 * first db was released. The error path must not release it again. */
TEST(AofRewriteTest, selectWriteErrorOnSecondDbReleasesIteratorOnce) {
    setupTwoDbs();
    addStringKey(server.db[0], "k0", "v0");
    addStringKey(server.db[1], "k1", "v1");

    size_t used_before = zmalloc_used_memory();
    rio r;
    rioInitWithBuffer(&r, sdsempty());
    buffer_write = r.write;
    r.write = failOnSelectWrite;
    select_writes = 0;
    fail_on_select = 2;

    EXPECT_EQ(rewriteAppendOnlyFileRio(&r), C_ERR);
    EXPECT_EQ(select_writes, 2);

    sdsfree(r.io.buffer.ptr);
    /* A second zfree of the db 0 iterator also subtracts its size twice. */
    EXPECT_EQ(zmalloc_used_memory(), used_before);
}
