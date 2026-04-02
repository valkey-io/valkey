/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <climits>
#include <cstdio>
#include <cstring>

extern "C" {
#include "server.h"

/* Internal types from networking.c */
typedef enum {
    PLAIN_REPLY,
    BULK_STR_REF,
} payloadType;

typedef struct bulkStrRef {
    robj *obj;
    sds str;
} bulkStrRef;

typedef struct __attribute__((__packed__)) payloadHeader {
    size_t payload_len;
    size_t reply_len;
    int16_t slot;
    uint8_t payload_type : 1;
    uint8_t track_bytes : 1;
    uint8_t reserved : 6;
} payloadHeader;

typedef struct bufWriteMetadata {
    char *buf;
    size_t bufpos;
    uint64_t data_len;
    int complete;
} bufWriteMetadata;

#define BULK_STR_LEN_PREFIX_MAX_SIZE 24

typedef struct replyIOV {
    int iovcnt;
    int iovsize;
    struct iovec *iov;
    ssize_t iov_len_total;
    size_t last_written_len;
    int limit_reached;
    int prfxcnt;
    char (*prefixes)[BULK_STR_LEN_PREFIX_MAX_SIZE];
    char *crlf;
} replyIOV;

/* Forward declarations for helper functions from networking.c */
void _addReplyToBufferOrList(client *c, const char *s, size_t len);
int inMainThread(void);
int clusterSlotStatsEnabled(int slot);

/* Wrapper functions for static functions in networking.c */
void testOnlyPostWriteToReplica(client *c);
void testOnlyWriteToReplica(client *c);
void testOnlyBackupAndUpdateClientArgv(client *c, int new_argc, robj **new_argv);
size_t testOnlyUpsertPayloadHeader(char *buf, size_t *bufpos, payloadHeader **last_header, uint8_t type, size_t len, int slot, size_t available);
int testOnlyIsCopyAvoidPreferred(client *c, robj *obj);
size_t testOnlyAddReplyPayloadToBuffer(client *c, const void *payload, size_t len, uint8_t payload_type);
size_t testOnlyAddBulkStrRefToBuffer(client *c, const void *payload, size_t len);
void testOnlyAddReplyPayloadToList(client *c, list *reply_list, const char *payload, size_t len, uint8_t payload_type);
void testOnlyAddBulkStrRefToToList(client *c, const void *payload, size_t len);
void testOnlyAddBulkStrRefToBufferOrList(client *c, robj *obj);
void testOnlyInitReplyIOV(client *c, int iovsize, struct iovec *iov_arr, char (*prefixes)[BULK_STR_LEN_PREFIX_MAX_SIZE], char *crlf, replyIOV *reply);
void testOnlyAddPlainBufferToReplyIOV(char *buf, size_t buf_len, replyIOV *reply, bufWriteMetadata *metadata);
void testOnlyAddBulkStringToReplyIOV(char *buf, size_t buf_len, replyIOV *reply, bufWriteMetadata *metadata);
void testOnlyAddEncodedBufferToReplyIOV(char *buf, size_t bufpos, replyIOV *reply, bufWriteMetadata *metadata);
void testOnlyAddBufferToReplyIOV(int encoded, char *buf, size_t bufpos, replyIOV *reply, bufWriteMetadata *metadata);
void testOnlySaveLastWrittenBuf(client *c, bufWriteMetadata *metadata, int bufcnt, size_t totlen, size_t totwritten);
}

/* Fake structures and functions */
typedef struct fakeConnection {
    connection conn;
    int error;
    char *buffer;
    size_t buf_size;
    size_t written;
} fakeConnection;

/* Fake connWrite function */
static int fake_connWrite(connection *conn, const void *data, size_t size) {
    fakeConnection *fake_conn = (fakeConnection *)conn;
    if (fake_conn->error) return -1;

    size_t to_write = size;
    if (fake_conn->written + to_write > fake_conn->buf_size) {
        to_write = fake_conn->buf_size - fake_conn->written;
    }

    memcpy(fake_conn->buffer + fake_conn->written, data, to_write);
    fake_conn->written += to_write;
    return (int)to_write;
}

/* Fake connWritev function */
static int fake_connWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    fakeConnection *fake_conn = (fakeConnection *)conn;
    if (fake_conn->error) return -1;

    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        size_t to_write = iov[i].iov_len;
        if (fake_conn->written + to_write > fake_conn->buf_size) {
            to_write = fake_conn->buf_size - fake_conn->written;
        }
        if (to_write == 0) break;

        memcpy(fake_conn->buffer + fake_conn->written, iov[i].iov_base, to_write);
        fake_conn->written += to_write;
        total += to_write;
    }
    return (int)total;
}

/* Fake connection type - initialized in SetUpTestSuite */
static ConnectionType CT_Fake;

static fakeConnection *connCreateFake(void) {
    fakeConnection *conn = (fakeConnection *)(zcalloc(sizeof(fakeConnection)));
    conn->conn.type = &CT_Fake;
    conn->conn.fd = -1;
    conn->conn.iovcnt = IOV_MAX;
    return conn;
}

/* Test fixture for networking tests - minimal fixture with no setup/teardown */
class NetworkingTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        /* Initialize CT_Fake explicitly by field name to avoid dependency
         * on field order (designated initializers require C++20). */
        memset(&CT_Fake, 0, sizeof(CT_Fake));
        CT_Fake.write = fake_connWrite;
        CT_Fake.writev = fake_connWritev;
    }

    void SetUp() override {
        /* Initialize server fields that are accessed by networking functions */
        server.commandlog[COMMANDLOG_TYPE_LARGE_REPLY].threshold = -1; /* Disable tracking */
        server.debug_client_enforce_reply_list = 0;
    }
};

TEST_F(NetworkingTest, TestWriteToReplica) {
    client *c = (client *)(zcalloc(sizeof(client)));
    initClientReplicationData(c);
    server.repl_buffer_blocks = listCreate();
    /* Ensure replicas list exists before creating backlog */
    if (!server.replicas) {
        server.replicas = listCreate();
    }
    createReplicationBacklog();
    c->reply = listCreate();
    /* Test 1: Single block write */
    {
        fakeConnection *fake_conn = connCreateFake();
        fake_conn->buffer = (char *)zmalloc(1024);
        fake_conn->buf_size = 1024;
        c->conn = (connection *)fake_conn;

        /* Create replication buffer block */
        replBufBlock *block = (replBufBlock *)(zmalloc(sizeof(replBufBlock) + 128));
        block->size = 128;
        block->used = 64;
        block->refcount = 1; /* Initialize refcount - client will reference it */
        memset(block->buf, 'A', 64);

        /* Setup client state */
        listAddNodeTail(server.repl_buffer_blocks, block);
        c->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);
        c->repl_data->ref_block_pos = 0;
        c->bufpos = 0;

        testOnlyWriteToReplica(c);

        ASSERT_EQ(c->nwritten, 64);
        ASSERT_EQ(fake_conn->written, 64u);
        ASSERT_EQ(memcmp(fake_conn->buffer, block->buf, 64), 0);
        ASSERT_EQ((c->write_flags & WRITE_FLAGS_WRITE_ERROR), 0);

        /* Cleanup */
        zfree(fake_conn->buffer);
        zfree(fake_conn);
        zfree(block);
        listEmpty(server.repl_buffer_blocks);
    }

    /* Test 2: Multiple blocks write */
    {
        fakeConnection *fake_conn = connCreateFake();
        fake_conn->error = 0;
        fake_conn->written = 0;
        fake_conn->buffer = (char *)zmalloc(1024);
        fake_conn->buf_size = 1024;
        c->conn = (connection *)fake_conn;

        /* Create multiple replication buffer blocks */
        replBufBlock *block1 = (replBufBlock *)(zmalloc(sizeof(replBufBlock) + 128));
        replBufBlock *block2 = (replBufBlock *)(zmalloc(sizeof(replBufBlock) + 128));
        block1->size = 128;
        block1->used = 64;
        block1->refcount = 1; /* Initialize refcount */
        block2->size = 128;
        block2->used = 32;
        block2->refcount = 0; /* Not referenced by client initially */
        memset(block1->buf, 'A', 64);
        memset(block2->buf, 'B', 32);

        /* Setup client state */
        listAddNodeTail(server.repl_buffer_blocks, block1);
        listAddNodeTail(server.repl_buffer_blocks, block2);
        c->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);
        c->repl_data->ref_block_pos = 0;
        c->bufpos = 0;

        testOnlyWriteToReplica(c);

        ASSERT_EQ(c->nwritten, 96); /* 64 + 32 */
        ASSERT_EQ(fake_conn->written, 96u);
        ASSERT_EQ(memcmp(fake_conn->buffer, block1->buf, 64), 0);
        ASSERT_EQ(memcmp(fake_conn->buffer + 64, block2->buf, 32), 0);
        ASSERT_EQ((c->write_flags & WRITE_FLAGS_WRITE_ERROR), 0);

        /* Cleanup */
        zfree(fake_conn->buffer);
        zfree(fake_conn);
        zfree(block1);
        zfree(block2);
        listEmpty(server.repl_buffer_blocks);
    }

    /* Test 3: Write error */
    {
        fakeConnection *fake_conn = connCreateFake();
        fake_conn->error = 1; /* Simulate write error */
        fake_conn->buffer = (char *)zmalloc(1024);
        fake_conn->buf_size = 1024;
        fake_conn->written = 0;
        c->conn = (connection *)fake_conn;

        /* Create replication buffer block */
        replBufBlock *block = (replBufBlock *)(zmalloc(sizeof(replBufBlock) + 128));
        block->size = 128;
        block->used = 64;
        memset(block->buf, 'A', 64);

        /* Setup client state */
        listAddNodeTail(server.repl_buffer_blocks, block);
        block->refcount = 1;
        c->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);
        c->repl_data->ref_block_pos = 0;
        c->bufpos = 0;

        testOnlyWriteToReplica(c);

        ASSERT_LE(c->nwritten, 0);
        ASSERT_NE((c->write_flags & WRITE_FLAGS_WRITE_ERROR), 0);

        /* Cleanup */
        listEmpty(server.repl_buffer_blocks);
        zfree(fake_conn->buffer);
        zfree(fake_conn);
        zfree(block);
        c->repl_data->ref_repl_buf_node = nullptr;
    }

    /* Cleanup */
    listRelease(server.repl_buffer_blocks);
    listRelease(c->reply);
    freeClientReplicationData(c);
    zfree(c);

    /* Clean up replication backlog */
    if (server.repl_backlog) {
        if (server.repl_backlog->ref_repl_buf_node) {
            server.repl_backlog->ref_repl_buf_node = NULL;
        }
        raxFree(server.repl_backlog->blocks_index);
        zfree(server.repl_backlog);
        server.repl_backlog = NULL;
    }
}

TEST_F(NetworkingTest, TestPostWriteToReplica) {
    client *c = (client *)(zcalloc(sizeof(client)));
    initClientReplicationData(c);
    server.repl_buffer_blocks = listCreate();
    /* Ensure replicas list exists before creating backlog */
    if (!server.replicas) {
        server.replicas = listCreate();
    }
    createReplicationBacklog();
    c->reply = listCreate();
    /* Test 1: No write case */
    {
        c->nwritten = 0;
        server.stat_net_repl_output_bytes = 0;

        testOnlyPostWriteToReplica(c);

        ASSERT_EQ(server.stat_net_repl_output_bytes, 0);
    }

    /* Test 2: Single block partial write */
    {
        replBufBlock *block = (replBufBlock *)(zmalloc(sizeof(replBufBlock) + 128));
        block->size = 128;
        block->used = 100;
        block->refcount = 1;

        listAddNodeTail(server.repl_buffer_blocks, block);
        c->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);
        c->repl_data->ref_block_pos = 20;
        c->nwritten = 30;

        server.stat_net_repl_output_bytes = 0;

        testOnlyPostWriteToReplica(c);

        ASSERT_EQ(server.stat_net_repl_output_bytes, 30);
        ASSERT_EQ(c->repl_data->ref_block_pos, 50u); /* 20 + 30 */
        ASSERT_EQ(c->repl_data->ref_repl_buf_node, listFirst(server.repl_buffer_blocks));
        ASSERT_EQ(block->refcount, 1);

        /* Cleanup */
        zfree(block);
        listEmpty(server.repl_buffer_blocks);
    }

    /* Test 3: Multiple blocks write */
    {
        replBufBlock *block1 = (replBufBlock *)(zmalloc(sizeof(replBufBlock) + 128));
        replBufBlock *block2 = (replBufBlock *)(zmalloc(sizeof(replBufBlock) + 128));
        block1->size = 128;
        block1->used = 64;
        block1->refcount = 1;
        block2->size = 128;
        block2->used = 100;
        block2->refcount = 0;

        listAddNodeTail(server.repl_buffer_blocks, block1);
        listAddNodeTail(server.repl_buffer_blocks, block2);
        c->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);
        c->repl_data->ref_block_pos = 30;
        c->nwritten = 50;

        server.stat_net_repl_output_bytes = 0;

        testOnlyPostWriteToReplica(c);

        ASSERT_EQ(server.stat_net_repl_output_bytes, 50);
        ASSERT_EQ(c->repl_data->ref_block_pos, 16u); /* (30 + 50) - 64 */
        ASSERT_EQ(c->repl_data->ref_repl_buf_node, listLast(server.repl_buffer_blocks));
        ASSERT_EQ(block1->refcount, 0);
        ASSERT_EQ(block2->refcount, 1);

        /* Cleanup */
        zfree(block1);
        zfree(block2);
        listEmpty(server.repl_buffer_blocks);
    }

    /* Test 4: Write exactly to block boundary */
    {
        replBufBlock *block = (replBufBlock *)(zmalloc(sizeof(replBufBlock) + 128));
        block->size = 128;
        block->used = 64;
        block->refcount = 1;

        /* Setup client state */
        listAddNodeTail(server.repl_buffer_blocks, block);
        c->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);
        c->repl_data->ref_block_pos = 30;
        c->nwritten = 34; /* Should reach exactly the end of block */

        server.stat_net_repl_output_bytes = 0;

        testOnlyPostWriteToReplica(c);

        ASSERT_EQ(server.stat_net_repl_output_bytes, 34);
        ASSERT_EQ(c->repl_data->ref_block_pos, 64u);
        ASSERT_EQ(c->repl_data->ref_repl_buf_node, listFirst(server.repl_buffer_blocks));
        ASSERT_EQ(block->refcount, 1); /* we don't free the last block even if it's fully written */

        /* Cleanup */
        zfree(block);
        c->repl_data->ref_repl_buf_node = nullptr;
        listEmpty(server.repl_buffer_blocks);
    }

    /* Cleanup */
    freeClientReplicationData(c);
    raxFree(server.repl_backlog->blocks_index);
    zfree(server.repl_backlog);
    listRelease(server.repl_buffer_blocks);
    listRelease(c->reply);
    zfree(c);
}

TEST_F(NetworkingTest, TestBackupAndUpdateClientArgv) {
    client *c = (client *)(zmalloc(sizeof(client)));
    /* Test 1: Initial backup of arguments */
    c->argc = 2;
    robj **initial_argv = (robj **)(zmalloc(sizeof(robj *) * 2));
    c->argv = initial_argv;
    c->argv[0] = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test"));
    c->argv[1] = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test2"));
    c->original_argv = nullptr;

    testOnlyBackupAndUpdateClientArgv(c, 3, nullptr);

    ASSERT_NE(c->argv, initial_argv);
    ASSERT_EQ(c->original_argv, initial_argv);
    ASSERT_EQ(c->original_argc, 2);
    ASSERT_EQ(c->argc, 3);
    ASSERT_EQ(c->argv_len, 3);
    ASSERT_EQ(c->argv[0]->refcount, 2u);
    ASSERT_EQ(c->argv[1]->refcount, 2u);
    ASSERT_EQ(c->argv[2], nullptr);

    /* Test 2: Direct argv replacement */
    robj **new_argv = (robj **)(zmalloc(sizeof(robj *) * 2));
    new_argv[0] = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test"));
    new_argv[1] = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test2"));

    testOnlyBackupAndUpdateClientArgv(c, 2, new_argv);

    ASSERT_EQ(c->argv, new_argv);
    ASSERT_EQ(c->argc, 2);
    ASSERT_EQ(c->argv_len, 2);
    ASSERT_NE(c->original_argv, c->argv);
    ASSERT_EQ(c->original_argv, initial_argv);
    ASSERT_EQ(c->original_argc, 2);
    ASSERT_EQ(c->original_argv[0]->refcount, 1u);
    ASSERT_EQ(c->original_argv[1]->refcount, 1u);

    /* Test 3: Expanding argc */
    testOnlyBackupAndUpdateClientArgv(c, 4, nullptr);

    ASSERT_EQ(c->argc, 4);
    ASSERT_EQ(c->argv_len, 4);
    ASSERT_NE(c->argv[0], nullptr);
    ASSERT_NE(c->argv[1], nullptr);
    ASSERT_EQ(c->argv[2], nullptr);
    ASSERT_EQ(c->argv[3], nullptr);
    ASSERT_EQ(c->original_argv, initial_argv);

    /* Cleanup */
    for (int i = 0; i < c->original_argc; i++) {
        decrRefCount(c->original_argv[i]);
    }
    zfree(c->original_argv);

    for (int i = 0; i < c->argc; i++) {
        if (c->argv[i]) decrRefCount(c->argv[i]);
    }
    zfree(c->argv);
    zfree(c);
}

TEST_F(NetworkingTest, TestRewriteClientCommandArgument) {
    client *c = (client *)(zmalloc(sizeof(client)));
    c->argc = 3;
    robj **initial_argv = (robj **)(zmalloc(sizeof(robj *) * 3));
    c->argv = initial_argv;
    c->original_argv = nullptr;
    c->argv_len_sum = 0;

    /* Initialize client with command "SET key value" */
    c->argv[0] = createStringObject("SET", 3);
    robj *original_key = createStringObject("key", 3);
    c->argv[1] = original_key;
    c->argv[2] = createStringObject("value", 5);
    c->argv_len_sum = 11; // 3 + 3 + 5

    /* Test 1: Rewrite existing argument */
    robj *newval = createStringObject("newkey", 6);
    rewriteClientCommandArgument(c, 1, newval);

    ASSERT_EQ(c->argv[1], newval);
    ASSERT_EQ(c->argv[1]->refcount, 2u);
    ASSERT_EQ(c->argv_len_sum, 14u); // 3 + 6 + 5
    ASSERT_EQ(c->original_argv, initial_argv);
    ASSERT_EQ(c->original_argv[1], original_key);
    ASSERT_EQ(c->original_argv[1]->refcount, 1u);

    /* Test 2: Extend argument vector */
    robj *extraval = createStringObject("extra", 5);
    rewriteClientCommandArgument(c, 3, extraval);

    ASSERT_EQ(c->argc, 4);
    ASSERT_EQ(c->argv[3], extraval);
    ASSERT_EQ(c->argv_len_sum, 19u); // 3 + 6 + 5 + 5
    ASSERT_EQ(c->original_argv, initial_argv);

    /* Cleanup */
    for (int i = 0; i < c->argc; i++) {
        if (c->argv[i]) decrRefCount(c->argv[i]);
    }
    zfree(c->argv);

    for (int i = 0; i < c->original_argc; i++) {
        if (c->original_argv[i]) decrRefCount(c->original_argv[i]);
    }
    zfree(c->original_argv);

    decrRefCount(newval);
    decrRefCount(extraval);

    zfree(c);
}

/* Helper function to create test client */
static client *createTestClient(void) {
    client *c = (client *)(zcalloc(sizeof(client)));

    c->buf = (char *)zmalloc_usable(PROTO_REPLY_CHUNK_BYTES, &c->buf_usable_size);
    c->reply = listCreate();
    listSetFreeMethod(c->reply, freeClientReplyValue);
    listSetDupMethod(c->reply, dupClientReplyValue);
    /* dummy connection to bypass assert in closeClientOnOutputBufferLimitReached */
    c->conn = (connection *)c;
    c->deferred_reply_bytes = ULLONG_MAX;

    return c;
}

static void freeReplyOffloadClient(client *c) {
    listRelease(c->reply);
    zfree(c->buf);
    zfree(c);
}

/* Each bulk offload puts 2 pointers to a reply buffer */
#define PTRS_LEN (sizeof(void *) * 2)

TEST_F(NetworkingTest, TestAddRepliesWithOffloadsToBuffer) {
    client *c = createTestClient();
    /* Test 1: Add bulk offloads to the buffer */
    robj *obj = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test"));
    testOnlyAddBulkStrRefToBufferOrList(c, obj);

    ASSERT_EQ(obj->refcount, 2u);
    ASSERT_EQ(c->bufpos, sizeof(payloadHeader) + PTRS_LEN);

    payloadHeader *header1 = c->last_header;
    ASSERT_EQ(header1->payload_type, BULK_STR_REF);
    ASSERT_EQ(header1->payload_len, PTRS_LEN);

    robj *ptr;
    memcpy(&ptr, c->buf + sizeof(payloadHeader), sizeof(ptr));
    ASSERT_EQ(obj, ptr);

    robj *obj2 = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test2"));
    testOnlyAddBulkStrRefToBufferOrList(c, obj2);

    /* 2 offloads expected in c->buf */
    ASSERT_EQ(c->bufpos, sizeof(payloadHeader) + 2 * PTRS_LEN);
    ASSERT_EQ(header1->payload_type, BULK_STR_REF);
    ASSERT_EQ(header1->payload_len, 2 * PTRS_LEN);

    memcpy(&ptr, c->buf + sizeof(payloadHeader) + PTRS_LEN, sizeof(ptr));
    ASSERT_EQ(obj2, ptr);

    /* Test 2: Add plain reply to the buffer */
    const char *plain = "+OK\r\n";
    size_t plain_len = strlen(plain);
    _addReplyToBufferOrList(c, plain, plain_len);

    /* 2 offloads and plain reply expected in c->buf. So 2 headers expected as well */
    ASSERT_EQ(c->bufpos, 2 * sizeof(payloadHeader) + 2 * PTRS_LEN + plain_len);
    ASSERT_EQ(header1->payload_type, BULK_STR_REF);
    ASSERT_EQ(header1->payload_len, 2 * PTRS_LEN);
    payloadHeader *header2 = c->last_header;
    ASSERT_EQ(header2->payload_type, PLAIN_REPLY);
    ASSERT_EQ(header2->payload_len, plain_len);

    /* Add more plain replies. Check same plain reply header updated properly */
    for (int i = 0; i < 9; ++i) _addReplyToBufferOrList(c, plain, plain_len);
    ASSERT_EQ(c->bufpos, 2 * sizeof(payloadHeader) + 2 * PTRS_LEN + 10 * plain_len);
    ASSERT_EQ(header2->payload_type, PLAIN_REPLY);
    ASSERT_EQ(header2->payload_len, plain_len * 10);

    /* Test 3: Add one more bulk offload to the buffer */
    testOnlyAddBulkStrRefToBufferOrList(c, obj);
    ASSERT_EQ(obj->refcount, 3u);
    ASSERT_EQ(c->bufpos, 3 * sizeof(payloadHeader) + 3 * PTRS_LEN + 10 * plain_len);
    payloadHeader *header3 = c->last_header;
    ASSERT_EQ(header3->payload_type, BULK_STR_REF);

    memcpy(&ptr, (char *)c->last_header + sizeof(payloadHeader), sizeof(ptr));
    ASSERT_EQ(obj, ptr);

    releaseReplyReferences(c);
    decrRefCount(obj);
    decrRefCount(obj2);

    freeReplyOffloadClient(c);
}

TEST_F(NetworkingTest, TestAddRepliesWithOffloadsToList) {
    /* Required for isCopyAvoidPreferred / isCopyAvoidIndicatedByIOThreads */
    int io_threads_num = server.io_threads_num;
    int min_io_threads_for_copy_avoid = server.min_io_threads_copy_avoid;
    server.io_threads_num = 1;
    server.min_io_threads_copy_avoid = 1;

    client *c = createTestClient();

    // Mock ACL
    user u;
    DefaultUser = &u;
    DefaultUser->flags = USER_FLAG_NOPASS;
    /* Test 1: Add bulk offloads to the reply list */
    /* Select reply length so that there is place for 2 headers and 4 bytes only
     * 4 bytes is not enough for object pointer(s)
     * This will force bulk offload to be added to reply list
     */
    size_t reply_len = c->buf_usable_size - 2 * sizeof(payloadHeader) - 4;
    char *reply = (char *)zmalloc(reply_len);
    memset(reply, 'a', reply_len);
    _addReplyToBufferOrList(c, reply, reply_len);
    ASSERT_TRUE(c->flag.buf_encoded);
    ASSERT_EQ(c->bufpos, sizeof(payloadHeader) + reply_len);
    ASSERT_EQ(listLength(c->reply), 0u);

    /* As bulk offload header+pointer can't be accommodated in c->buf
     * then one block is expected in c->reply */
    robj *obj = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test"));
    testOnlyAddBulkStrRefToBufferOrList(c, obj);
    ASSERT_EQ(obj->refcount, 2u);
    ASSERT_EQ(c->bufpos, sizeof(payloadHeader) + reply_len);
    ASSERT_EQ(listLength(c->reply), 1u);

    /* Check bulk offload header+pointer inside c->reply */
    listIter iter;
    listRewind(c->reply, &iter);
    listNode *next = listNext(&iter);
    clientReplyBlock *blk = (clientReplyBlock *)(listNodeValue(next));

    ASSERT_EQ(blk->used, sizeof(payloadHeader) + PTRS_LEN);
    payloadHeader *header1 = blk->last_header;
    ASSERT_EQ(header1->payload_type, BULK_STR_REF);
    ASSERT_EQ(header1->payload_len, PTRS_LEN);

    robj *ptr;
    memcpy(&ptr, blk->buf + sizeof(payloadHeader), sizeof(ptr));
    ASSERT_EQ(obj, ptr);

    /* Test 2: Add one more bulk offload to the reply list */
    testOnlyAddBulkStrRefToBufferOrList(c, obj);
    ASSERT_EQ(obj->refcount, 3u);
    ASSERT_EQ(listLength(c->reply), 1u);
    ASSERT_EQ(blk->used, sizeof(payloadHeader) + 2 * PTRS_LEN);
    ASSERT_EQ(header1->payload_type, BULK_STR_REF);
    ASSERT_EQ(header1->payload_len, 2 * PTRS_LEN);

    /* Test 3: Add plain replies to cause reply list grow */
    while (reply_len < blk->size - blk->used) _addReplyToBufferOrList(c, reply, reply_len);
    _addReplyToBufferOrList(c, reply, reply_len);

    ASSERT_EQ(listLength(c->reply), 2u);
    /* last header in 1st block */
    payloadHeader *header2 = blk->last_header;
    listRewind(c->reply, &iter);
    listNext(&iter);
    next = listNext(&iter);
    clientReplyBlock *blk2 = (clientReplyBlock *)(listNodeValue(next));
    /* last header in 2nd block */
    payloadHeader *header3 = blk2->last_header;
    ASSERT_EQ(header2->payload_type, PLAIN_REPLY);
    ASSERT_EQ(header3->payload_type, PLAIN_REPLY);
    ASSERT_EQ((header2->payload_len + header3->payload_len) % reply_len, 0u);

    zfree(reply);
    decrRefCount(obj);

    releaseReplyReferences(c);
    freeReplyOffloadClient(c);

    /* Restore modified values */
    server.io_threads_num = io_threads_num;
    server.min_io_threads_copy_avoid = min_io_threads_for_copy_avoid;
}

TEST_F(NetworkingTest, TestAddBufferToReplyIOV) {
    client *c = createTestClient();
    const char *expected_reply = "$5\r\nhello\r\n";
    ssize_t total_len = (ssize_t)(strlen(expected_reply));
    const int iovmax = 16;
    char crlf[2];
    crlf[0] = '\r';
    crlf[1] = '\n';

    robj *obj = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "hello"));
    testOnlyAddBulkStrRefToBufferOrList(c, obj);

    /* Test 1: 1st writevToclient invocation */
    struct iovec iov_arr[iovmax];
    char prefixes[iovmax / 3 + 1][LONG_STR_SIZE + 3];
    bufWriteMetadata metadata[1];

    replyIOV reply;
    testOnlyInitReplyIOV(c, iovmax, iov_arr, prefixes, crlf, &reply);
    testOnlyAddBufferToReplyIOV(c->flag.buf_encoded, c->buf, c->bufpos, &reply, &metadata[0]);

    ASSERT_EQ(reply.iov_len_total, (ssize_t)total_len);
    ASSERT_EQ(reply.iovcnt, 3);
    const char *ptr = expected_reply;
    for (int i = 0; i < reply.iovcnt; ++i) {
        ASSERT_EQ(memcmp(ptr, reply.iov[i].iov_base, reply.iov[i].iov_len), 0);
        ptr += reply.iov[i].iov_len;
    }

    /* Test 2: Last written buf/pos/data_len after 1st invocation */
    testOnlySaveLastWrittenBuf(c, metadata, 1, reply.iov_len_total, 1); /* only 1 byte has been written */
    ASSERT_EQ(c->io_last_written.buf, c->buf);
    ASSERT_EQ(c->io_last_written.bufpos, 0u); /* incomplete write */
    ASSERT_EQ(c->io_last_written.data_len, 1u);

    /* Test 3: 2nd writevToclient invocation */
    struct iovec iov_arr2[iovmax];
    char prefixes2[iovmax / 3 + 1][LONG_STR_SIZE + 3];
    bufWriteMetadata metadata2[1];

    replyIOV reply2;
    testOnlyInitReplyIOV(c, iovmax, iov_arr2, prefixes2, crlf, &reply2);
    testOnlyAddBufferToReplyIOV(c->flag.buf_encoded, c->buf, c->bufpos, &reply2, &metadata2[0]);
    ASSERT_EQ(reply2.iov_len_total, (ssize_t)(total_len - 1));
    ASSERT_EQ(*(char *)reply2.iov[0].iov_base, '5');

    /* Test 4: Last written buf/pos/data_len after 2nd invocation */
    testOnlySaveLastWrittenBuf(c, metadata2, 1, reply2.iov_len_total, 4); /* 4 more bytes has been written */
    ASSERT_EQ(c->io_last_written.buf, c->buf);
    ASSERT_EQ(c->io_last_written.bufpos, 0u);   /* incomplete write */
    ASSERT_EQ(c->io_last_written.data_len, 5u); /* 1 + 4 */

    /* Test 5: 3rd writevToclient invocation */
    struct iovec iov_arr3[iovmax];
    char prefixes3[iovmax / 3 + 1][LONG_STR_SIZE + 3];
    bufWriteMetadata metadata3[1];

    replyIOV reply3;
    testOnlyInitReplyIOV(c, iovmax, iov_arr3, prefixes3, crlf, &reply3);
    testOnlyAddBufferToReplyIOV(c->flag.buf_encoded, c->buf, c->bufpos, &reply3, &metadata3[0]);
    ASSERT_EQ(reply3.iov_len_total, (ssize_t)(total_len - 5));
    ASSERT_EQ(*(char *)reply3.iov[0].iov_base, 'e');

    /* Test 6: Last written buf/pos/data_len after 3rd invocation */
    testOnlySaveLastWrittenBuf(c, metadata3, 1, reply3.iov_len_total, reply3.iov_len_total); /* everything has been written */
    ASSERT_EQ(c->io_last_written.buf, c->buf);
    ASSERT_EQ(c->io_last_written.bufpos, c->bufpos);
    ASSERT_EQ(c->io_last_written.data_len, (size_t)total_len);

    decrRefCount(obj);

    releaseReplyReferences(c);
    freeReplyOffloadClient(c);
}
