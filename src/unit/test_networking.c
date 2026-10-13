#include "../networking.c"
#include "../server.c"
#include "test_help.h"

#include <stdatomic.h>

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
    return to_write;
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
    return total;
}

/* Fake connection type */
static ConnectionType CT_Fake = {
    .write = fake_connWrite,
    .writev = fake_connWritev,
};

static fakeConnection *connCreateFake(void) {
    fakeConnection *conn = zcalloc(sizeof(fakeConnection));
    conn->conn.type = &CT_Fake;
    conn->conn.fd = -1;
    conn->conn.iovcnt = IOV_MAX;
    return conn;
}

int test_writeToReplica(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    client *c = zcalloc(sizeof(client));
    initClientReplicationData(c);
    server.repl_buffer_blocks = listCreate();
    createReplicationBacklog();
    c->reply = listCreate();

    /* Test 1: Single block write */
    {
        fakeConnection *fake_conn = connCreateFake();
        fake_conn->buffer = zmalloc(1024);
        fake_conn->buf_size = 1024;
        c->conn = (connection *)fake_conn;

        /* Create replication buffer block */
        replBufBlock *block = zmalloc(sizeof(replBufBlock) + 128);
        block->size = 128;
        block->used = 64;
        memset(block->buf, 'A', 64);

        /* Setup client state */
        listAddNodeTail(server.repl_buffer_blocks, block);
        c->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);
        c->repl_data->ref_block_pos = 0;
        c->bufpos = 0;

        writeToReplica(c);

        TEST_ASSERT(c->nwritten == 64);
        TEST_ASSERT(fake_conn->written == 64);
        TEST_ASSERT(memcmp(fake_conn->buffer, block->buf, 64) == 0);
        TEST_ASSERT((c->write_flags & WRITE_FLAGS_WRITE_ERROR) == 0);

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
        fake_conn->buffer = zmalloc(1024);
        fake_conn->buf_size = 1024;
        c->conn = (connection *)fake_conn;

        /* Create multiple replication buffer blocks */
        replBufBlock *block1 = zmalloc(sizeof(replBufBlock) + 128);
        replBufBlock *block2 = zmalloc(sizeof(replBufBlock) + 128);
        block1->size = 128;
        block1->used = 64;
        block2->size = 128;
        block2->used = 32;
        memset(block1->buf, 'A', 64);
        memset(block2->buf, 'B', 32);

        /* Setup client state */
        listAddNodeTail(server.repl_buffer_blocks, block1);
        listAddNodeTail(server.repl_buffer_blocks, block2);
        c->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);
        c->repl_data->ref_block_pos = 0;
        c->bufpos = 0;

        writeToReplica(c);

        TEST_ASSERT(c->nwritten == 96); /* 64 + 32 */
        TEST_ASSERT(fake_conn->written == 96);
        TEST_ASSERT(memcmp(fake_conn->buffer, block1->buf, 64) == 0);
        TEST_ASSERT(memcmp(fake_conn->buffer + 64, block2->buf, 32) == 0);
        TEST_ASSERT((c->write_flags & WRITE_FLAGS_WRITE_ERROR) == 0);

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
        fake_conn->buffer = zmalloc(1024);
        fake_conn->buf_size = 1024;
        fake_conn->written = 0;
        c->conn = (connection *)fake_conn;

        /* Create replication buffer block */
        replBufBlock *block = zmalloc(sizeof(replBufBlock) + 128);
        block->size = 128;
        block->used = 64;
        memset(block->buf, 'A', 64);

        /* Setup client state */
        listAddNodeTail(server.repl_buffer_blocks, block);
        block->refcount = 1;
        c->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);
        c->repl_data->ref_block_pos = 0;
        c->bufpos = 0;

        writeToReplica(c);

        TEST_ASSERT(c->nwritten <= 0);
        TEST_ASSERT((c->write_flags & WRITE_FLAGS_WRITE_ERROR) != 0);

        /* Cleanup */
        listEmpty(server.repl_buffer_blocks);
        zfree(fake_conn->buffer);
        zfree(fake_conn);
        zfree(block);
        c->repl_data->ref_repl_buf_node = NULL;
    }

    /* Cleanup */
    listRelease(server.repl_buffer_blocks);
    listRelease(c->reply);
    freeClientReplicationData(c);
    zfree(c);

    return 0;
}

int test_postWriteToReplica(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    client *c = zcalloc(sizeof(client));
    initClientReplicationData(c);
    server.repl_buffer_blocks = listCreate();
    c->reply = listCreate();

    /* Test 1: No write case */
    {
        c->nwritten = 0;
        server.stat_net_repl_output_bytes = 0;

        postWriteToReplica(c);

        TEST_ASSERT(server.stat_net_repl_output_bytes == 0);
    }

    /* Test 2: Single block partial write */
    {
        replBufBlock *block = zmalloc(sizeof(replBufBlock) + 128);
        block->size = 128;
        block->used = 100;
        block->refcount = 1;

        listAddNodeTail(server.repl_buffer_blocks, block);
        c->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);
        c->repl_data->ref_block_pos = 20;
        c->nwritten = 30;

        server.stat_net_repl_output_bytes = 0;

        postWriteToReplica(c);

        TEST_ASSERT(server.stat_net_repl_output_bytes == 30);
        TEST_ASSERT(c->repl_data->ref_block_pos == 50); /* 20 + 30 */
        TEST_ASSERT(c->repl_data->ref_repl_buf_node == listFirst(server.repl_buffer_blocks));
        TEST_ASSERT(block->refcount == 1);

        /* Cleanup */
        zfree(block);
        listEmpty(server.repl_buffer_blocks);
    }

    /* Test 3: Multiple blocks write */
    {
        replBufBlock *block1 = zmalloc(sizeof(replBufBlock) + 128);
        replBufBlock *block2 = zmalloc(sizeof(replBufBlock) + 128);
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

        postWriteToReplica(c);

        TEST_ASSERT(server.stat_net_repl_output_bytes == 50);
        TEST_ASSERT(c->repl_data->ref_block_pos == 16); /* (30 + 50) - 64 */
        TEST_ASSERT(c->repl_data->ref_repl_buf_node == listLast(server.repl_buffer_blocks));
        TEST_ASSERT(block1->refcount == 0);
        TEST_ASSERT(block2->refcount == 1);

        /* Cleanup */
        zfree(block1);
        zfree(block2);
        listEmpty(server.repl_buffer_blocks);
    }

    /* Test 4: Write exactly to block boundary */
    {
        replBufBlock *block = zmalloc(sizeof(replBufBlock) + 128);
        block->size = 128;
        block->used = 64;
        block->refcount = 1;

        /* Setup client state */
        listAddNodeTail(server.repl_buffer_blocks, block);
        c->repl_data->ref_repl_buf_node = listFirst(server.repl_buffer_blocks);
        c->repl_data->ref_block_pos = 30;
        c->nwritten = 34; /* Should reach exactly the end of block */

        server.stat_net_repl_output_bytes = 0;

        postWriteToReplica(c);

        TEST_ASSERT(server.stat_net_repl_output_bytes == 34);
        TEST_ASSERT(c->repl_data->ref_block_pos == 64);
        TEST_ASSERT(c->repl_data->ref_repl_buf_node == listFirst(server.repl_buffer_blocks));
        TEST_ASSERT(block->refcount == 1); /* we don't free the last block even if it's fully written */

        /* Cleanup */
        zfree(block);
        c->repl_data->ref_repl_buf_node = NULL;
        listEmpty(server.repl_buffer_blocks);
    }

    /* Cleanup */
    freeClientReplicationData(c);
    raxFree(server.repl_backlog->blocks_index);
    zfree(server.repl_backlog);
    listRelease(server.repl_buffer_blocks);
    listRelease(c->reply);
    zfree(c);

    return 0;
}

int test_backupAndUpdateClientArgv(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    client *c = zmalloc(sizeof(client));

    /* Test 1: Initial backup of arguments */
    c->argc = 2;
    robj **initial_argv = zmalloc(sizeof(robj *) * 2);
    c->argv = initial_argv;
    c->argv[0] = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test"));
    c->argv[1] = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test2"));
    c->original_argv = NULL;

    backupAndUpdateClientArgv(c, 3, NULL);

    TEST_ASSERT(c->argv != initial_argv);
    TEST_ASSERT(c->original_argv == initial_argv);
    TEST_ASSERT(c->original_argc == 2);
    TEST_ASSERT(c->argc == 3);
    TEST_ASSERT(c->argv_len == 3);
    TEST_ASSERT(c->argv[0]->refcount == 2);
    TEST_ASSERT(c->argv[1]->refcount == 2);
    TEST_ASSERT(c->argv[2] == NULL);

    /* Test 2: Direct argv replacement */
    robj **new_argv = zmalloc(sizeof(robj *) * 2);
    new_argv[0] = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test"));
    new_argv[1] = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test2"));

    backupAndUpdateClientArgv(c, 2, new_argv);

    TEST_ASSERT(c->argv == new_argv);
    TEST_ASSERT(c->argc == 2);
    TEST_ASSERT(c->argv_len == 2);
    TEST_ASSERT(c->original_argv != c->argv);
    TEST_ASSERT(c->original_argv == initial_argv);
    TEST_ASSERT(c->original_argc == 2);
    TEST_ASSERT(c->original_argv[0]->refcount == 1);
    TEST_ASSERT(c->original_argv[1]->refcount == 1);

    /* Test 3: Expanding argc */
    backupAndUpdateClientArgv(c, 4, NULL);

    TEST_ASSERT(c->argc == 4);
    TEST_ASSERT(c->argv_len == 4);
    TEST_ASSERT(c->argv[0] != NULL);
    TEST_ASSERT(c->argv[1] != NULL);
    TEST_ASSERT(c->argv[2] == NULL);
    TEST_ASSERT(c->argv[3] == NULL);
    TEST_ASSERT(c->original_argv == initial_argv);

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

    return 0;
}

int test_rewriteClientCommandArgument(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    client *c = zmalloc(sizeof(client));
    c->argc = 3;
    robj **initial_argv = zmalloc(sizeof(robj *) * 3);
    c->argv = initial_argv;
    c->original_argv = NULL;
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

    TEST_ASSERT(c->argv[1] == newval);
    TEST_ASSERT(c->argv[1]->refcount == 2);
    TEST_ASSERT(c->argv_len_sum == 14); // 3 + 6 + 5
    TEST_ASSERT(c->original_argv == initial_argv);
    TEST_ASSERT(c->original_argv[1] == original_key);
    TEST_ASSERT(c->original_argv[1]->refcount == 1);

    /* Test 3: Extend argument vector */
    robj *extraval = createStringObject("extra", 5);
    rewriteClientCommandArgument(c, 3, extraval);

    TEST_ASSERT(c->argc == 4);
    TEST_ASSERT(c->argv[3] == extraval);
    TEST_ASSERT(c->argv_len_sum == 19); // 3 + 6 + 5 + 5
    TEST_ASSERT(c->original_argv == initial_argv);

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

    return 0;
}

static client *createTestClient(void) {
    client *c = zcalloc(sizeof(client));

    c->buf = zmalloc_usable(PROTO_REPLY_CHUNK_BYTES, &c->buf_usable_size);
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

int test_addRepliesWithOffloadsToBuffer(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    client *c = createTestClient();

    /* Test 1:  Add bulk offloads to the buffer */
    robj *obj = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test"));
    _addBulkStrRefToBufferOrList(c, obj);

    TEST_ASSERT(obj->refcount == 2);
    TEST_ASSERT(c->bufpos == sizeof(payloadHeader) + PTRS_LEN);

    payloadHeader *header1 = c->last_header;
    TEST_ASSERT(header1->payload_type == BULK_STR_REF);
    TEST_ASSERT(header1->payload_len == PTRS_LEN);


    robj *ptr;
    memcpy(&ptr, c->buf + sizeof(payloadHeader), sizeof(ptr));
    TEST_ASSERT(obj == ptr);

    robj *obj2 = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test2"));
    _addBulkStrRefToBufferOrList(c, obj2);

    /* 2 offloads expected in c->buf */
    TEST_ASSERT(c->bufpos == sizeof(payloadHeader) + 2 * PTRS_LEN);
    TEST_ASSERT(header1->payload_type == BULK_STR_REF);
    TEST_ASSERT(header1->payload_len == 2 * PTRS_LEN);

    memcpy(&ptr, c->buf + sizeof(payloadHeader) + PTRS_LEN, sizeof(ptr));
    TEST_ASSERT(obj2 == ptr);

    /* Test 2:  Add plain reply to the buffer */
    const char *plain = "+OK\r\n";
    size_t plain_len = strlen(plain);
    _addReplyToBufferOrList(c, plain, plain_len);

    /* 2 offloads and plain reply expected in c->buf. So 2 headers expected as well */
    TEST_ASSERT(c->bufpos == 2 * sizeof(payloadHeader) + 2 * PTRS_LEN + plain_len);
    TEST_ASSERT(header1->payload_type == BULK_STR_REF);
    TEST_ASSERT(header1->payload_len == 2 * PTRS_LEN);
    payloadHeader *header2 = c->last_header;
    TEST_ASSERT(header2->payload_type == PLAIN_REPLY);
    TEST_ASSERT(header2->payload_len == plain_len);

    /* Add more plain replies. Check same plain reply header updated properly */
    for (int i = 0; i < 9; ++i) _addReplyToBufferOrList(c, plain, plain_len);
    TEST_ASSERT(c->bufpos == 2 * sizeof(payloadHeader) + 2 * PTRS_LEN + 10 * plain_len);
    TEST_ASSERT(header2->payload_type == PLAIN_REPLY);
    TEST_ASSERT(header2->payload_len == plain_len * 10);

    /* Test 3:  Add one more bulk offload to the buffer */
    _addBulkStrRefToBufferOrList(c, obj);
    TEST_ASSERT(obj->refcount == 3);
    TEST_ASSERT(c->bufpos == 3 * sizeof(payloadHeader) + 3 * PTRS_LEN + 10 * plain_len);
    payloadHeader *header3 = c->last_header;
    TEST_ASSERT(header3->payload_type == BULK_STR_REF);
    memcpy(&ptr, (char *)c->last_header + sizeof(payloadHeader), sizeof(ptr));
    TEST_ASSERT(obj == ptr);

    releaseReplyReferences(c);
    decrRefCount(obj);
    decrRefCount(obj2);

    freeReplyOffloadClient(c);

    return 0;
}

int test_addRepliesWithOffloadsToList(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

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

    /* Test 1:  Add bulk offloads to the reply list */

    /* Select reply length so that there is place for 2 headers and 4 bytes only
     * 4 bytes is not enough for object pointer(s)
     * This will force bulk offload to be added to reply list
     */
    size_t reply_len = c->buf_usable_size - 2 * sizeof(payloadHeader) - 4;
    char *reply = zmalloc(reply_len);
    memset(reply, 'a', reply_len);
    _addReplyToBufferOrList(c, reply, reply_len);
    TEST_ASSERT(c->flag.buf_encoded);
    TEST_ASSERT(c->bufpos == sizeof(payloadHeader) + reply_len);
    TEST_ASSERT(listLength(c->reply) == 0);

    /* As bulk offload header+pointer can't be accommodated in c->buf
     * then one block is expected in c->reply */
    robj *obj = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "test"));
    _addBulkStrRefToBufferOrList(c, obj);
    TEST_ASSERT(obj->refcount == 2);
    TEST_ASSERT(c->bufpos == sizeof(payloadHeader) + reply_len);
    TEST_ASSERT(listLength(c->reply) == 1);

    /* Check bulk offload header+pointer inside c->reply */
    listIter iter;
    listRewind(c->reply, &iter);
    listNode *next = listNext(&iter);
    clientReplyBlock *blk = listNodeValue(next);

    TEST_ASSERT(blk->used == sizeof(payloadHeader) + PTRS_LEN);
    payloadHeader *header1 = blk->last_header;
    TEST_ASSERT(header1->payload_type == BULK_STR_REF);
    TEST_ASSERT(header1->payload_len == PTRS_LEN);

    robj *ptr;
    memcpy(&ptr, blk->buf + sizeof(payloadHeader), sizeof(ptr));
    TEST_ASSERT(obj == ptr);

    /* Test 2:  Add one more bulk offload to the reply list */
    _addBulkStrRefToBufferOrList(c, obj);
    TEST_ASSERT(obj->refcount == 3);
    TEST_ASSERT(listLength(c->reply) == 1);
    TEST_ASSERT(blk->used == sizeof(payloadHeader) + 2 * PTRS_LEN);
    TEST_ASSERT(header1->payload_type == BULK_STR_REF);
    TEST_ASSERT(header1->payload_len == 2 * PTRS_LEN);

    /* Test 3: Add plain replies to cause reply list grow  */
    while (reply_len < blk->size - blk->used) _addReplyToBufferOrList(c, reply, reply_len);
    _addReplyToBufferOrList(c, reply, reply_len);

    TEST_ASSERT(listLength(c->reply) == 2);
    /* last header in 1st block */
    payloadHeader *header2 = blk->last_header;
    listRewind(c->reply, &iter);
    listNext(&iter);
    next = listNext(&iter);
    clientReplyBlock *blk2 = listNodeValue(next);
    /* last header in 2nd block */
    payloadHeader *header3 = blk2->last_header;
    TEST_ASSERT(header2->payload_type == PLAIN_REPLY && header3->payload_type == PLAIN_REPLY);
    TEST_ASSERT((header2->payload_len + header3->payload_len) % reply_len == 0);

    releaseReplyReferences(c);
    decrRefCount(obj);

    zfree(reply);

    freeReplyOffloadClient(c);

    /* Restore modified values */
    server.io_threads_num = io_threads_num;
    server.min_io_threads_copy_avoid = min_io_threads_for_copy_avoid;

    return 0;
}

int test_addBufferToReplyIOV(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    const char *expected_reply = "$5\r\nhello\r\n";
    ssize_t total_len = strlen(expected_reply);
    enum { iovmax = 16 };
    char crlf[2] = {'\r', '\n'};

    /* Test 1: 1st writevToclient invocation */
    client *c = createTestClient();
    robj *obj = createObject(OBJ_STRING, sdscatfmt(sdsempty(), "hello"));
    _addBulkStrRefToBufferOrList(c, obj);

    struct iovec iov_arr[iovmax];
    char prefixes[iovmax / 3 + 1][LONG_STR_SIZE + 3];
    bufWriteMetadata metadata[1];

    replyIOV reply;
    initReplyIOV(c, iovmax, iov_arr, prefixes, crlf, &reply);
    addBufferToReplyIOV(c->flag.buf_encoded, c->buf, c->bufpos, &reply, &metadata[0]);

    TEST_ASSERT(reply.iov_len_total == total_len);
    TEST_ASSERT(reply.iovcnt == 3);
    const char *ptr = expected_reply;
    for (int i = 0; i < reply.iovcnt; ++i) {
        TEST_ASSERT(memcmp(ptr, reply.iov[i].iov_base, reply.iov[i].iov_len) == 0);
        ptr += reply.iov[i].iov_len;
    }

    /* Test 2: Last written buf/pos/data_len after 1st invocation */
    saveLastWrittenBuf(c, metadata, 1, reply.iov_len_total, 1); /* only 1 byte has been written */
    TEST_ASSERT(c->io_last_written.buf == c->buf);
    TEST_ASSERT(c->io_last_written.bufpos == 0); /* incomplete write */
    TEST_ASSERT(c->io_last_written.data_len == 1);

    /* Test 3: 2nd writevToclient invocation */
    struct iovec iov_arr2[iovmax];
    char prefixes2[iovmax / 3 + 1][LONG_STR_SIZE + 3];
    bufWriteMetadata metadata2[1];

    replyIOV reply2;
    initReplyIOV(c, iovmax, iov_arr2, prefixes2, crlf, &reply2);
    addBufferToReplyIOV(c->flag.buf_encoded, c->buf, c->bufpos, &reply2, &metadata2[0]);
    TEST_ASSERT(reply2.iov_len_total == total_len - 1);
    TEST_ASSERT((*(char *)reply2.iov[0].iov_base) == '5');

    /* Test 4: Last written buf/pos/data_len after 2nd invocation */
    saveLastWrittenBuf(c, metadata2, 1, reply2.iov_len_total, 4); /* 4 more bytes has been written */
    TEST_ASSERT(c->io_last_written.buf == c->buf);
    TEST_ASSERT(c->io_last_written.bufpos == 0);   /* incomplete write */
    TEST_ASSERT(c->io_last_written.data_len == 5); /* 1 + 4 */

    /* Test 5: 3rd writevToclient invocation */
    struct iovec iov_arr3[iovmax];
    char prefixes3[iovmax / 3 + 1][LONG_STR_SIZE + 3];
    bufWriteMetadata metadata3[1];

    replyIOV reply3;
    initReplyIOV(c, iovmax, iov_arr3, prefixes3, crlf, &reply3);
    addBufferToReplyIOV(c->flag.buf_encoded, c->buf, c->bufpos, &reply3, &metadata3[0]);
    TEST_ASSERT(reply3.iov_len_total == total_len - 5);
    TEST_ASSERT((*(char *)reply3.iov[0].iov_base) == 'e');

    /* Test 6: Last written buf/pos/data_len after 3rd invocation */
    saveLastWrittenBuf(c, metadata3, 1, reply3.iov_len_total, reply3.iov_len_total); /* everything has been written */
    TEST_ASSERT(c->io_last_written.buf == c->buf);
    TEST_ASSERT(c->io_last_written.bufpos == c->bufpos);
    TEST_ASSERT(c->io_last_written.data_len == (size_t)total_len);

    decrRefCount(obj);
    decrRefCount(obj);

    freeReplyOffloadClient(c);

    return 0;
}

/* Helper: allocate a plain reply block with the given used/size and fill its
 * used bytes with `fill`. Caller adds it to a reply list owned by a client
 * created via createTestClient (freed by freeReplyOffloadClient). */
static clientReplyBlock *makePlainReplyBlock(size_t size, size_t used, char fill) {
    clientReplyBlock *blk = zmalloc(sizeof(clientReplyBlock) + size);
    blk->size = size;
    blk->used = used;
    blk->flag.buf_encoded = 0;
    blk->last_header = NULL;
    memset(blk->buf, fill, used);
    return blk;
}

/* trimReplyUnusedTailSpace must not realloc the tail when the write bookmark
 * (io_last_written.buf) points at it, since freeing/moving it desyncs the
 * bookmark. The guard is pointer-based, so it fires regardless of io_write_state
 * (COMPLETED_IO or IDLE), and it allows the trim when the bookmark points
 * elsewhere. See https://github.com/valkey-io/valkey/pull/4060 */
int test_trimReplyUnusedTailSpaceGuardsIoLastWritten(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t alloc_size = PROTO_REPLY_CHUNK_BYTES * 2; /* waste > size/4, used small */

    /* Test 1: tail IS the bookmarked block, CLIENT_COMPLETED_IO -> refuse */
    {
        client *c = createTestClient();
        clientReplyBlock *blk = makePlainReplyBlock(alloc_size, 32, 'X');
        listAddNodeTail(c->reply, blk);
        c->reply_bytes = alloc_size;
        c->io_last_written.buf = blk->buf; /* bookmark on the tail */
        c->io_last_written.bufpos = blk->used;
        c->io_last_written.data_len = blk->used;
        c->io_write_state = CLIENT_COMPLETED_IO;

        trimReplyUnusedTailSpace(c);

        clientReplyBlock *after = listNodeValue(listLast(c->reply));
        TEST_ASSERT(after->size == alloc_size);            /* not reallocated */
        TEST_ASSERT(c->io_last_written.buf == after->buf); /* bookmark still valid */
        freeReplyOffloadClient(c);
    }

    /* Test 2: tail IS the bookmarked block, CLIENT_IDLE -> still refuse.
     * A partial main-thread write leaves the bookmark live (bufpos = 0 sentinel)
     * while the state is IDLE; the pointer guard must still protect the block. */
    {
        client *c = createTestClient();
        clientReplyBlock *blk = makePlainReplyBlock(alloc_size, 32, 'X');
        listAddNodeTail(c->reply, blk);
        c->reply_bytes = alloc_size;
        c->io_last_written.buf = blk->buf; /* bookmark on the tail */
        c->io_last_written.bufpos = 0;     /* partial-write sentinel */
        c->io_last_written.data_len = 16;
        c->io_write_state = CLIENT_IDLE;

        trimReplyUnusedTailSpace(c);

        clientReplyBlock *after = listNodeValue(listLast(c->reply));
        TEST_ASSERT(after->size == alloc_size); /* not reallocated */
        freeReplyOffloadClient(c);
    }

    /* Test 3: bookmark points at a DIFFERENT block, CLIENT_COMPLETED_IO ->
     * proceed. The tail is not the bookmarked block, so trimming it is safe
     * even though an IO write just completed. */
    {
        client *c = createTestClient();
        clientReplyBlock *head = makePlainReplyBlock(64, 20, 'H'); /* bookmarked */
        clientReplyBlock *tail = makePlainReplyBlock(alloc_size, 32, 'T');
        listAddNodeTail(c->reply, head);
        listAddNodeTail(c->reply, tail);
        c->reply_bytes = head->size + tail->size;
        c->io_last_written.buf = head->buf; /* bookmark on the head, not the tail */
        c->io_last_written.bufpos = head->used;
        c->io_last_written.data_len = head->used;
        c->io_write_state = CLIENT_COMPLETED_IO;

        trimReplyUnusedTailSpace(c);

        clientReplyBlock *after = listNodeValue(listLast(c->reply));
        TEST_ASSERT(after->size < alloc_size); /* tail was trimmed */
        TEST_ASSERT(after->used == 32);
        for (size_t i = 0; i < 32; i++) TEST_ASSERT(after->buf[i] == 'T'); /* content preserved */
        freeReplyOffloadClient(c);
    }

    /* Test 4: no bookmark, CLIENT_IDLE -> proceed */
    {
        client *c = createTestClient();
        clientReplyBlock *blk = makePlainReplyBlock(alloc_size, 32, 'X');
        listAddNodeTail(c->reply, blk);
        c->reply_bytes = alloc_size;
        c->io_write_state = CLIENT_IDLE;
        resetLastWrittenBuf(c);

        trimReplyUnusedTailSpace(c);

        clientReplyBlock *after = listNodeValue(listLast(c->reply));
        TEST_ASSERT(after->size < alloc_size); /* trimmed */
        TEST_ASSERT(after->used == 32);
        freeReplyOffloadClient(c);
    }

    return 0;
}

/* Even when the tail is not the bookmarked block, the trim must be refused while
 * CLIENT_PENDING_IO, because an IO thread is concurrently walking the reply list
 * and reallocating any block would race with it. */
int test_trimReplyUnusedTailSpaceRefusedWhilePendingIO(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    client *c = createTestClient();
    size_t alloc_size = PROTO_REPLY_CHUNK_BYTES * 2;
    clientReplyBlock *blk = makePlainReplyBlock(alloc_size, 32, 'X');
    listAddNodeTail(c->reply, blk);
    c->reply_bytes = alloc_size;

    resetLastWrittenBuf(c);                /* no bookmark on the tail */
    c->io_write_state = CLIENT_PENDING_IO; /* IO thread actively writing */

    trimReplyUnusedTailSpace(c);

    clientReplyBlock *after = listNodeValue(listLast(c->reply));
    TEST_ASSERT(after->size == alloc_size); /* not reallocated */
    freeReplyOffloadClient(c);

    return 0;
}

/* setDeferredReply's prev-merge (appending the length header into the node
 * *before* the placeholder) must be skipped when io_last_written points at that
 * prev node, and proceed when it points elsewhere. Guard is pointer-based. */
int test_setDeferredReplyPrevMergeGuardsIoLastWritten(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    const char *hdr = "*2\r\n";
    const size_t hdr_len = 4;

    /* Mock ACL for closeClientOnOutputBufferLimitReached */
    user u;
    DefaultUser = &u;
    DefaultUser->flags = USER_FLAG_NOPASS;

    /* Test 1: prev IS the bookmarked block -> merge must be skipped */
    {
        client *c = createTestClient();
        /* prev block is bookmarked and has room to append the header. */
        clientReplyBlock *prev = makePlainReplyBlock(64, 10, 'P');
        listAddNodeTail(c->reply, prev);
        listAddNodeTail(c->reply, NULL); /* placeholder is the tail (no next) */
        c->reply_bytes = prev->size;
        listNode *placeholder = listLast(c->reply);

        c->io_last_written.buf = prev->buf; /* bookmark on prev */
        c->io_last_written.bufpos = prev->used;
        c->io_last_written.data_len = prev->used;
        c->io_write_state = CLIENT_COMPLETED_IO;

        setDeferredReply(c, placeholder, hdr, hdr_len);

        /* prev must be untouched: merge was refused, header went to a new node
         * filling the placeholder instead. */
        clientReplyBlock *prev_after = listNodeValue(listFirst(c->reply));
        TEST_ASSERT(prev_after == prev);
        TEST_ASSERT(prev_after->used == 10);
        TEST_ASSERT(listLength(c->reply) == 2); /* placeholder filled, not deleted */
        clientReplyBlock *filled = listNodeValue(listLast(c->reply));
        TEST_ASSERT(filled->used == hdr_len);
        TEST_ASSERT(memcmp(filled->buf, hdr, hdr_len) == 0);

        freeReplyOffloadClient(c);
    }

    /* Test 2: bookmark points elsewhere (none) -> prev-merge proceeds */
    {
        client *c = createTestClient();
        clientReplyBlock *prev = makePlainReplyBlock(64, 10, 'P');
        listAddNodeTail(c->reply, prev);
        listAddNodeTail(c->reply, NULL);
        c->reply_bytes = prev->size;
        listNode *placeholder = listLast(c->reply);

        c->io_write_state = CLIENT_COMPLETED_IO; /* non-PENDING; bookmark not on prev */
        resetLastWrittenBuf(c);

        setDeferredReply(c, placeholder, hdr, hdr_len);

        /* Header appended into prev; placeholder removed. */
        clientReplyBlock *prev_after = listNodeValue(listFirst(c->reply));
        TEST_ASSERT(prev_after == prev);
        TEST_ASSERT(prev_after->used == 10 + hdr_len);
        TEST_ASSERT(memcmp(prev_after->buf + 10, hdr, hdr_len) == 0);
        TEST_ASSERT(listLength(c->reply) == 1);

        freeReplyOffloadClient(c);
    }

    return 0;
}

/* setDeferredReply's next-merge (memmove-ing the node *after* the placeholder to
 * prepend the length header) must be skipped when io_last_written points at that
 * next node, and proceed when it points elsewhere. Guard is pointer-based. */
int test_setDeferredReplyNextMergeGuardsIoLastWritten(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    const char *hdr = "*2\r\n";
    const size_t hdr_len = 4;

    /* Mock ACL for closeClientOnOutputBufferLimitReached */
    user u;
    DefaultUser = &u;
    DefaultUser->flags = USER_FLAG_NOPASS;

    /* Test 1: next IS the bookmarked block -> next-merge must be skipped */
    {
        client *c = createTestClient();
        listAddNodeTail(c->reply, NULL); /* placeholder is the head (no prev) */
        listNode *placeholder = listFirst(c->reply);
        clientReplyBlock *next = makePlainReplyBlock(64, 10, 'Y');
        listAddNodeTail(c->reply, next);
        c->reply_bytes = next->size;

        c->io_last_written.buf = next->buf; /* bookmark on next */
        c->io_last_written.bufpos = next->used;
        c->io_last_written.data_len = next->used;
        c->io_write_state = CLIENT_COMPLETED_IO;

        setDeferredReply(c, placeholder, hdr, hdr_len);

        /* next must be untouched (no memmove): header went to a new node filling
         * the placeholder. */
        clientReplyBlock *next_after = listNodeValue(listLast(c->reply));
        TEST_ASSERT(next_after == next);
        TEST_ASSERT(next_after->used == 10);
        for (size_t i = 0; i < 10; i++) TEST_ASSERT(next_after->buf[i] == 'Y');
        TEST_ASSERT(listLength(c->reply) == 2); /* placeholder filled, not deleted */
        clientReplyBlock *filled = listNodeValue(listFirst(c->reply));
        TEST_ASSERT(filled->used == hdr_len);
        TEST_ASSERT(memcmp(filled->buf, hdr, hdr_len) == 0);

        freeReplyOffloadClient(c);
    }

    /* Test 2: bookmark points elsewhere (none) -> next-merge proceeds */
    {
        client *c = createTestClient();
        listAddNodeTail(c->reply, NULL);
        listNode *placeholder = listFirst(c->reply);
        clientReplyBlock *next = makePlainReplyBlock(64, 10, 'Y');
        listAddNodeTail(c->reply, next);
        c->reply_bytes = next->size;

        c->io_write_state = CLIENT_COMPLETED_IO; /* non-PENDING; bookmark not on next */
        resetLastWrittenBuf(c);

        setDeferredReply(c, placeholder, hdr, hdr_len);

        /* Header prepended into next (existing content shifted right); placeholder
         * removed. */
        clientReplyBlock *next_after = listNodeValue(listFirst(c->reply));
        TEST_ASSERT(next_after == next);
        TEST_ASSERT(next_after->used == 10 + hdr_len);
        TEST_ASSERT(memcmp(next_after->buf, hdr, hdr_len) == 0);
        for (size_t i = 0; i < 10; i++) TEST_ASSERT(next_after->buf[hdr_len + i] == 'Y');
        TEST_ASSERT(listLength(c->reply) == 1);

        freeReplyOffloadClient(c);
    }

    return 0;
}

/* -----------------------------------------------------------------------------
 * Client-side caching: reclaiming dead client IDs from the tracking table.
 *
 * The sweep decides liveness with lookupClientByID(), i.e. server.clients_index,
 * so these tests install a fake client index and a hand-built tracking table.
 * They are white-box by design: budget boundaries, mid-key resumption and
 * cursor invalidation need precise table shapes that cannot be built or
 * observed through the public tracking API.
 * -------------------------------------------------------------------------- */

/* Internal to tracking.c and deliberately absent from server.h. */
extern uint64_t TrackingTableTotalItems;
int trackingSweepStep(monotime endtime, uint64_t *removed);
rax **unitTestOnly_getTrackingTable(void);
int unitTestOnly_trackingSweepChecksPerClock(void);

/* An endtime that is already in the past under the fake clock below: every
 * step stops at its first clock check. */
#define TRACKING_TEST_EXPIRED_DEADLINE ((monotime)1)

static monotime trackingTestFakeMonotimeUs;
static monotime trackingTestFakeGetMonotonicUs(void) {
    return trackingTestFakeMonotimeUs;
}

/* Make lookupClientByID(id) return non-NULL. The key encoding matches
 * linkClient(): the id in network byte order. */
static void trackingTestAddLiveClient(uint64_t id) {
    client *c = zcalloc(sizeof(client));
    c->id = id;
    uint64_t be = htonu64(id);
    raxInsert(server.clients_index, (unsigned char *)&be, sizeof(be), c, NULL);
}

/* Register 'ids' as trackers of 'keyname' the way trackingRememberKeys does,
 * keeping TrackingTableTotalItems consistent with the table. */
static void trackingTestTrackKeyIds(rax *tt, const char *keyname, const uint64_t *ids, int n) {
    void *found;
    rax *inner;
    size_t klen = strlen(keyname);
    if (!raxFind(tt, (unsigned char *)keyname, klen, &found)) {
        inner = raxNew();
        raxInsert(tt, (unsigned char *)keyname, klen, inner, NULL);
    } else {
        inner = found;
    }
    for (int i = 0; i < n; i++) {
        uint64_t id = ids[i];
        if (raxTryInsert(inner, (unsigned char *)&id, sizeof(id), NULL, NULL)) TrackingTableTotalItems++;
    }
}

static int trackingTestKeyHasId(rax *inner, uint64_t id) {
    void *found;
    return raxFind(inner, (unsigned char *)&id, sizeof(id), &found);
}

/* Count every ID in the table and report whether all of them reference a live client. */
static uint64_t trackingTestCountIdsAndCheckLive(rax *tt, int *all_live_out) {
    uint64_t total = 0;
    int all_live = 1;
    raxIterator ri;
    raxStart(&ri, tt);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
        rax *ids = ri.data;
        total += raxSize(ids);
        raxIterator idi;
        raxStart(&idi, ids);
        raxSeek(&idi, "^", NULL, 0);
        while (raxNext(&idi)) {
            uint64_t id;
            memcpy(&id, idi.key, sizeof(id));
            if (lookupClientByID(id) == NULL) all_live = 0;
        }
        raxStop(&idi);
    }
    raxStop(&ri);
    *all_live_out = all_live;
    return total;
}

/* Drop 'keyname' the way the tail of trackingInvalidateKey does (minus the
 * invalidation send), simulating a key invalidated between two sweep steps. */
static void trackingTestRemoveKey(rax *tt, const char *keyname) {
    void *found;
    size_t klen = strlen(keyname);
    if (!raxFind(tt, (unsigned char *)keyname, klen, &found)) return;
    rax *inner = found;
    TrackingTableTotalItems -= raxSize(inner);
    raxFree(inner);
    raxRemove(tt, (unsigned char *)keyname, klen, NULL);
}

/* Sweep with an expired deadline until the pass completes, checking on every
 * step that it made progress and kept the counter consistent with the table.
 * Returns the number of steps that stopped on the deadline. */
static int trackingTestSweepToEnd(rax *tt, uint64_t expected_live_total) {
    int steps = 0;
    uint64_t removed;
    while (!trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed)) {
        steps++;
        TEST_EXPECT(removed > 0);
        int all_live = 0;
        uint64_t actual = trackingTestCountIdsAndCheckLive(tt, &all_live);
        TEST_EXPECT(actual == TrackingTableTotalItems);
        TEST_EXPECT(actual >= expected_live_total);
        if (steps > 100000) break; /* never an open loop */
    }
    return steps;
}

/* Run 'body' against an empty tracking table, an empty fake client index and a
 * fake monotonic clock, then restore everything so nothing leaks between tests
 * (including when the body bails out on a failed assertion). */
static int runTrackingSweepTest(int (*body)(rax *tt)) {
    monotime (*orig_get_monotonic_us)(void) = getMonotonicUs;
    rax *saved_clients_index = server.clients_index;
    rax **tt = unitTestOnly_getTrackingTable();

    getMonotonicUs = trackingTestFakeGetMonotonicUs;
    trackingTestFakeMonotimeUs = 1000000;
    server.clients_index = raxNew();
    *tt = raxNew();
    TrackingTableTotalItems = 0;

    int result = body(*tt);

    raxIterator ri;
    raxStart(&ri, *tt);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) raxFree(ri.data);
    raxStop(&ri);
    raxFree(*tt);
    *tt = NULL;
    TrackingTableTotalItems = 0;

    /* With no tracking table the sweep entry points forget their static state
     * (resume cursor, scheduler period). */
    trackingSweepStep(0, NULL);
    trackingSweepDeadClients();

    raxFreeWithCallback(server.clients_index, zfree);
    server.clients_index = saved_clients_index;
    getMonotonicUs = orig_get_monotonic_us;
    return result;
}

/* A mix of live and dead IDs is swept so that only live IDs remain, the counter
 * matches, and keys whose inner radix tree became empty are removed. The dead
 * ID in k1 sits between two live ones, so the post-removal re-seek must land on
 * the live successor and preserve it. */
static int trackingSweeperRemovesDeadIdsAndEmptyKeys(rax *tt) {
    uint64_t live1 = 1, dead2 = 2, live3 = 3;

    /* Clients 1 and 3 are connected; client 2 is gone (never registered). */
    trackingTestAddLiveClient(live1);
    trackingTestAddLiveClient(live3);

    uint64_t k1ids[3] = {live1, dead2, live3}; /* dead between two live */
    uint64_t k2ids[1] = {dead2};               /* only dead -> becomes empty */
    uint64_t k3ids[1] = {live3};               /* only live */
    trackingTestTrackKeyIds(tt, "k1", k1ids, 3);
    trackingTestTrackKeyIds(tt, "k2", k2ids, 1);
    trackingTestTrackKeyIds(tt, "k3", k3ids, 1);

    TEST_EXPECT(trackingGetTotalKeys() == 3);
    TEST_EXPECT(trackingGetTotalItems() == 5);

    /* No deadline: a single step sweeps the whole table and reaches EOF. */
    TEST_EXPECT(trackingSweepStep(0, NULL) == 1);

    /* k2 became empty and was removed; k1 and k3 remain. */
    TEST_EXPECT(trackingGetTotalKeys() == 2);
    TEST_EXPECT(trackingGetTotalItems() == 3);

    void *found;
    TEST_EXPECT(!raxFind(tt, (unsigned char *)"k2", 2, &found));

    /* k1 keeps both live IDs, including the one after the removed ID. */
    TEST_ASSERT(raxFind(tt, (unsigned char *)"k1", 2, &found));
    rax *k1inner = found;
    TEST_EXPECT(raxSize(k1inner) == 2);
    TEST_EXPECT(trackingTestKeyHasId(k1inner, live1));
    TEST_EXPECT(!trackingTestKeyHasId(k1inner, dead2));
    TEST_EXPECT(trackingTestKeyHasId(k1inner, live3));

    /* k3's live id is preserved. */
    TEST_ASSERT(raxFind(tt, (unsigned char *)"k3", 2, &found));
    rax *k3inner = found;
    TEST_EXPECT(raxSize(k3inner) == 1);
    TEST_EXPECT(trackingTestKeyHasId(k3inner, live3));

    /* The counter matches the surviving IDs, all of which are live. */
    int all_live = 0;
    uint64_t actual = trackingTestCountIdsAndCheckLive(tt, &all_live);
    TEST_EXPECT(actual == trackingGetTotalItems());
    TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperRemovesDeadIdsAndEmptyKeys(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperRemovesDeadIdsAndEmptyKeys);
}

/* The step reports the number of removed IDs through its out parameter (set,
 * not accumulated), and trackingSweepFull (DEBUG SWEEP-TRACKING-TABLE) is a
 * no-op on a clean table. */
static int trackingSweepStepReportsRemovedAndFullSweepDrains(rax *tt) {
    uint64_t kids[2] = {1, 2}; /* no clients registered: both dead */
    trackingTestTrackKeyIds(tt, "k1", kids, 2);
    TEST_EXPECT(trackingGetTotalItems() == 2);

    /* One unbounded step drains the table and accounts for every reclaimed ID. */
    uint64_t removed;
    TEST_EXPECT(trackingSweepStep(0, &removed) == 1);
    TEST_EXPECT(removed == 2);
    TEST_EXPECT(trackingGetTotalKeys() == 0);
    TEST_EXPECT(trackingGetTotalItems() == 0);

    /* A step over a clean table reports zero. */
    TEST_EXPECT(trackingSweepStep(0, &removed) == 1);
    TEST_EXPECT(removed == 0);

    trackingSweepFull();
    TEST_EXPECT(trackingGetTotalItems() == 0);
    return 0;
}

int test_trackingSweepStepReportsRemovedAndFullSweepDrains(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweepStepReportsRemovedAndFullSweepDrains);
}

/* The scheduled entry point (serverCron path) self-gates on an adaptive period:
 * a call that reclaims something halves it, a call that finds nothing doubles
 * it, clamped to [100ms, 60s]. Driven under the fake clock. */
static int trackingSweepSchedulerAdaptsPeriodUnderFakeClock(rax *tt) {
    uint64_t dead1 = 1, dead2 = 2, dead3 = 3;
    trackingTestTrackKeyIds(tt, "k1", &dead1, 1);
    TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Freshly reset: the first call is due and the period starts at its 60s
     * maximum. Reclaiming halves it to 30s. */
    trackingSweepDeadClients();
    TEST_EXPECT(trackingGetTotalItems() == 0);

    /* Immediately after: inside the period, a new dead ID survives. */
    trackingTestTrackKeyIds(tt, "k2", &dead2, 1);
    trackingSweepDeadClients();
    TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Advance just under 30s: still gated. */
    trackingTestFakeMonotimeUs += 29 * 1000 * 1000;
    trackingSweepDeadClients();
    TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Cross 30s: due, reclaims, period halves again to 15s. */
    trackingTestFakeMonotimeUs += 2 * 1000 * 1000;
    trackingSweepDeadClients();
    TEST_EXPECT(trackingGetTotalItems() == 0);

    /* 15s later with nothing to reclaim: runs, finds nothing, period doubles to 30s. */
    trackingTestFakeMonotimeUs += 16 * 1000 * 1000;
    trackingSweepDeadClients();
    trackingTestTrackKeyIds(tt, "k3", &dead3, 1);
    /* 16s later: inside the doubled 30s period, so still gated. */
    trackingTestFakeMonotimeUs += 16 * 1000 * 1000;
    trackingSweepDeadClients();
    TEST_EXPECT(trackingGetTotalItems() == 1);
    /* Another 15s: past 30s, due. */
    trackingTestFakeMonotimeUs += 15 * 1000 * 1000;
    trackingSweepDeadClients();
    TEST_EXPECT(trackingGetTotalItems() == 0);
    return 0;
}

int test_trackingSweepSchedulerAdaptsPeriodUnderFakeClock(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweepSchedulerAdaptsPeriodUnderFakeClock);
}

/* Incrementality across keys: with more IDs than one expired-deadline step
 * covers, the sweep takes several steps, each making progress and never
 * touching a live ID, and completes the pass. */
static int trackingSweeperIsIncrementalAndResumesAcrossCalls(rax *tt) {
    const int total_keys = 300;
    const int ids_per_key = 4;
    uint64_t canary = 1000000;
    trackingTestAddLiveClient(canary);

    for (int i = 0; i < total_keys; i++) {
        char kn[16];
        /* Zero-padded so lexicographic order equals numeric order. */
        snprintf(kn, sizeof(kn), "key%03d", i);
        uint64_t dead_ids[4];
        for (int j = 0; j < ids_per_key; j++) dead_ids[j] = (uint64_t)(i * ids_per_key + j + 1); /* no client -> dead */
        trackingTestTrackKeyIds(tt, kn, dead_ids, ids_per_key);
    }
    trackingTestTrackKeyIds(tt, "key150", &canary, 1);

    TEST_EXPECT(trackingGetTotalKeys() == (uint64_t)total_keys);
    TEST_EXPECT(trackingGetTotalItems() == (uint64_t)(total_keys * ids_per_key + 1));

    int steps = trackingTestSweepToEnd(tt, 1);
    TEST_EXPECT(steps > 1);

    /* Every dead ID and emptied key is gone; the canary's key survives with only the canary. */
    TEST_EXPECT(trackingGetTotalKeys() == 1);
    TEST_EXPECT(trackingGetTotalItems() == 1);
    int all_live = 0;
    TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperIsIncrementalAndResumesAcrossCalls(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperIsIncrementalAndResumesAcrossCalls);
}

/* One key holding many more dead IDs than one step covers: the mid-key cursor
 * resumes inside the key across steps, the key survives while IDs remain, and
 * is reclaimed once drained. */
static int trackingSweeperBoundsWorkWithinOneHugeKey(rax *tt) {
    const uint64_t total_ids = 2500;

    for (uint64_t id = 1; id <= total_ids; id++) trackingTestTrackKeyIds(tt, "hotkey", &id, 1); /* all dead */
    TEST_EXPECT(trackingGetTotalKeys() == 1);
    TEST_EXPECT(trackingGetTotalItems() == total_ids);

    /* The first step stops inside the key: the key is still there, with fewer IDs. */
    uint64_t removed;
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TEST_EXPECT(removed > 0);
    TEST_EXPECT(removed < total_ids);
    TEST_EXPECT(trackingGetTotalKeys() == 1);
    TEST_EXPECT(trackingGetTotalItems() == total_ids - removed);

    int steps = trackingTestSweepToEnd(tt, 0);
    TEST_EXPECT(steps > 1);
    TEST_EXPECT(trackingGetTotalItems() == 0);
    TEST_EXPECT(trackingGetTotalKeys() == 0);
    return 0;
}

int test_trackingSweeperBoundsWorkWithinOneHugeKey(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperBoundsWorkWithinOneHugeKey);
}

/* Live IDs scattered through a huge key are preserved across mid-key
 * resumptions, wherever the step boundaries fall. */
static int trackingSweeperPreservesLiveIdsInHugeKey(rax *tt) {
    const uint64_t total_ids = 1200;
    const uint64_t live_ids[3] = {5, 500, 1200};

    for (uint64_t id = 1; id <= total_ids; id++) trackingTestTrackKeyIds(tt, "hotkey", &id, 1);
    for (int i = 0; i < 3; i++) trackingTestAddLiveClient(live_ids[i]);
    TEST_EXPECT(trackingGetTotalItems() == total_ids);

    int steps = trackingTestSweepToEnd(tt, 3);
    TEST_EXPECT(steps > 1);

    TEST_EXPECT(trackingGetTotalKeys() == 1);
    TEST_EXPECT(trackingGetTotalItems() == 3);
    void *found;
    TEST_ASSERT(raxFind(tt, (unsigned char *)"hotkey", 6, &found));
    rax *inner = found;
    for (int i = 0; i < 3; i++) TEST_EXPECT(trackingTestKeyHasId(inner, live_ids[i]));
    int all_live = 0;
    TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 3);
    TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperPreservesLiveIdsInHugeKey(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperPreservesLiveIdsInHugeKey);
}

/* Boundary: a step that stops exactly on a live ID must neither remove it nor
 * skip past the IDs after it. Live IDs are placed precisely on the first two
 * stop points. */
static int trackingSweeperStopsOnLiveIdAndResumesPastIt(rax *tt) {
    const uint64_t s = (uint64_t)unitTestOnly_trackingSweepChecksPerClock();
    const uint64_t total_ids = 2 * s + 8;
    void *found;

    for (uint64_t id = 1; id <= total_ids; id++) trackingTestTrackKeyIds(tt, "k", &id, 1);
    trackingTestAddLiveClient(s);
    trackingTestAddLiveClient(2 * s);
    TEST_EXPECT(trackingGetTotalItems() == total_ids);

    uint64_t removed;
    /* Step 1: checks 1..s; removes 1..s-1; stops on the live ID s. */
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TEST_EXPECT(removed == s - 1);
    TEST_EXPECT(trackingGetTotalItems() == total_ids - (s - 1));
    TEST_ASSERT(raxFind(tt, (unsigned char *)"k", 1, &found));
    rax *inner = found;
    TEST_EXPECT(trackingTestKeyHasId(inner, s));
    TEST_EXPECT(trackingTestKeyHasId(inner, s + 1)); /* not yet visited */

    /* Step 2: resumes after s; removes s+1..2s-1; stops on the live ID 2s. */
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TEST_EXPECT(removed == s - 1);
    TEST_EXPECT(trackingGetTotalItems() == total_ids - 2 * (s - 1));
    TEST_EXPECT(trackingTestKeyHasId(inner, s));
    TEST_EXPECT(trackingTestKeyHasId(inner, 2 * s));
    TEST_EXPECT(trackingTestKeyHasId(inner, 2 * s + 1));

    /* Step 3: the remaining 8 dead IDs (fewer than a clock cadence) are
     * removed, the key stays with its two live IDs, the pass completes. */
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TEST_EXPECT(removed == 8);
    TEST_EXPECT(trackingGetTotalKeys() == 1);
    TEST_EXPECT(trackingGetTotalItems() == 2);
    int all_live = 0;
    TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 2);
    TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperStopsOnLiveIdAndResumesPastIt(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperStopsOnLiveIdAndResumesPastIt);
}

/* Boundary: the deadline hits exactly as a key's last ID is removed. The
 * emptied key must be reclaimed right away, and the next step must start at
 * the successor key's first ID rather than resuming inside a key that no
 * longer exists. */
static int trackingSweeperReclaimsKeyEmptiedExactlyAtDeadline(rax *tt) {
    const uint64_t s = (uint64_t)unitTestOnly_trackingSweepChecksPerClock();
    uint64_t live = 100;
    void *found;

    /* "kA" < "kB" < "kC". kA holds exactly one step's worth of dead IDs. */
    for (uint64_t id = 1; id <= s; id++) trackingTestTrackKeyIds(tt, "kA", &id, 1);
    uint64_t kb_ids[3] = {live, 101, 102};
    trackingTestTrackKeyIds(tt, "kB", kb_ids, 3);
    uint64_t kc_id = 200;
    trackingTestTrackKeyIds(tt, "kC", &kc_id, 1);
    trackingTestAddLiveClient(live);
    TEST_EXPECT(trackingGetTotalItems() == s + 4);

    uint64_t removed;
    /* Step 1: drains kA on the s-th check, which is also the deadline check.
     * kA is reclaimed in place; kB and kC are untouched. */
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TEST_EXPECT(removed == s);
    TEST_EXPECT(!raxFind(tt, (unsigned char *)"kA", 2, &found));
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kB", 2, &found));
    TEST_EXPECT(raxSize(found) == 3);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kC", 2, &found));
    TEST_EXPECT(raxSize(found) == 1);
    TEST_EXPECT(trackingGetTotalItems() == 4);

    /* Step 2: starts at kB's first ID (live, kept), removes 101 and 102,
     * drains and reclaims kC, and reaches EOF. */
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TEST_EXPECT(removed == 3);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kB", 2, &found));
    rax *kb = found;
    TEST_EXPECT(raxSize(kb) == 1);
    TEST_EXPECT(trackingTestKeyHasId(kb, live));
    TEST_EXPECT(!raxFind(tt, (unsigned char *)"kC", 2, &found));
    TEST_EXPECT(trackingGetTotalKeys() == 1);
    TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Step 3: a fresh pass finds nothing to do. */
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TEST_EXPECT(removed == 0);
    int all_live = 0;
    TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperReclaimsKeyEmptiedExactlyAtDeadline(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperReclaimsKeyEmptiedExactlyAtDeadline);
}

/* The key the cursor stopped inside may be invalidated before the next call:
 * the ">=" seek must then land on the successor key and finish the pass. If
 * the same key name is re-created holding an ID that sorts before the saved
 * in-key cursor, that ID is missed by the resumed pass and reclaimed by the
 * next one: the guarantee is eventual reclamation, not single-pass completeness. */
static int trackingSweeperToleratesCursorKeyRemovalBetweenCalls(rax *tt) {
    const int mid_ids = 4 * unitTestOnly_trackingSweepChecksPerClock(); /* several steps' worth */
    uint64_t canary = 42;
    void *found;
    uint64_t removed;

    trackingTestAddLiveClient(canary);

    /* "kaaa" < "kmid" < "kzzz": the first step drains "kaaa" and stops inside "kmid". */
    for (int i = 0; i < 3; i++) {
        uint64_t dead_id = (uint64_t)(100 + i); /* no client -> dead */
        trackingTestTrackKeyIds(tt, "kaaa", &dead_id, 1);
    }
    for (int i = 0; i < mid_ids; i++) {
        /* Every ID carries 0xFF in its lowest byte so that, in either byte
         * order, the ID 1 re-inserted below sorts before any of them - and
         * thus before the saved in-key cursor. */
        uint64_t dead_id = ((uint64_t)(i + 1) << 8) | 0xFF;
        trackingTestTrackKeyIds(tt, "kmid", &dead_id, 1);
    }
    for (int i = 0; i < 5; i++) {
        uint64_t dead_id = (uint64_t)(9000 + i);
        trackingTestTrackKeyIds(tt, "kzzz", &dead_id, 1);
    }
    trackingTestTrackKeyIds(tt, "kzzz", &canary, 1);
    TEST_EXPECT(trackingGetTotalItems() == (uint64_t)(3 + mid_ids + 6));

    /* Call 1: stops inside "kmid". */
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TEST_EXPECT(!raxFind(tt, (unsigned char *)"kaaa", 4, &found));
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kmid", 4, &found));
    TEST_EXPECT(raxSize(found) > 0);

    /* Remove the cursor key between calls, as trackingInvalidateKey would. */
    trackingTestRemoveKey(tt, "kmid");
    TEST_EXPECT(trackingGetTotalItems() == 6);

    /* The ">=" seek lands on the successor "kzzz"; the pass finishes, its dead
     * IDs are reclaimed and the live canary survives. */
    trackingTestSweepToEnd(tt, 1);
    TEST_EXPECT(trackingGetTotalKeys() == 1);
    TEST_EXPECT(trackingGetTotalItems() == 1);
    int all_live = 0;
    TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TEST_EXPECT(all_live == 1);

    /* Re-creation variant: park the cursor inside "kmid" again... */
    for (int i = 0; i < 3; i++) {
        uint64_t dead_id = (uint64_t)(200 + i);
        trackingTestTrackKeyIds(tt, "kaaa", &dead_id, 1);
    }
    for (int i = 0; i < mid_ids; i++) {
        uint64_t dead_id = ((uint64_t)(i + 1) << 8) | 0xFF;
        trackingTestTrackKeyIds(tt, "kmid", &dead_id, 1);
    }
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kmid", 4, &found));

    /* ...remove it, then re-create the same key name with a single dead ID
     * that sorts before the saved cursor. */
    trackingTestRemoveKey(tt, "kmid");
    uint64_t small_dead = 1;
    trackingTestTrackKeyIds(tt, "kmid", &small_dead, 1);
    TEST_EXPECT(trackingGetTotalItems() == 2);

    /* The resumed call matches the re-created key by name and seeks strictly
     * past the saved cursor, so ID 1 is missed by this pass while the rest of
     * the table is still processed to EOF. */
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TEST_EXPECT(removed == 0);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kmid", 4, &found));
    TEST_EXPECT(trackingTestKeyHasId(found, small_dead));
    TEST_EXPECT(trackingGetTotalItems() == 2);

    /* The next pass restarts from the beginning and reclaims the missed ID. */
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TEST_EXPECT(removed == 1);
    TEST_EXPECT(!raxFind(tt, (unsigned char *)"kmid", 4, &found));
    TEST_EXPECT(trackingGetTotalKeys() == 1);
    TEST_EXPECT(trackingGetTotalItems() == 1);
    all_live = 0;
    TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperToleratesCursorKeyRemovalBetweenCalls(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperToleratesCursorKeyRemovalBetweenCalls);
}

/* A key added lexicographically before the in-flight cursor is not seen by the
 * current pass and is reclaimed on the next pass once the cursor wraps at EOF. */
static int trackingSweeperReclaimsKeyInsertedBehindCursorOnNextPass(rax *tt) {
    uint64_t canary = 42;
    void *found;
    uint64_t removed;

    trackingTestAddLiveClient(canary);

    /* "kc" holds more IDs than one step covers, so the first step stops
     * inside it, leaving "kd" untouched. */
    const char *names[4] = {"ka", "kc", "kd", "kf"};
    const int counts[4] = {2, 4 * unitTestOnly_trackingSweepChecksPerClock(), 5, 5};
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < counts[k]; i++) {
            uint64_t dead_id = (uint64_t)((k + 1) * 10000 + i); /* no client -> dead */
            trackingTestTrackKeyIds(tt, names[k], &dead_id, 1);
        }
    }
    trackingTestTrackKeyIds(tt, "kf", &canary, 1);
    TEST_EXPECT(trackingGetTotalItems() == (uint64_t)(counts[0] + counts[1] + counts[2] + counts[3] + 1));

    /* Call 1: "ka" is drained and removed; the cursor sits inside "kc". */
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TEST_EXPECT(!raxFind(tt, (unsigned char *)"ka", 2, &found));
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kc", 2, &found));
    TEST_EXPECT(raxSize(found) > 0);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kd", 2, &found));
    TEST_EXPECT(raxSize(found) == (uint64_t)counts[2]);

    /* Insert a new key behind the cursor: "ka" < "kb" < "kc". */
    uint64_t behind_dead = 7777;
    trackingTestTrackKeyIds(tt, "kb", &behind_dead, 1);

    /* Finish the pass: "kc" is drained and reclaimed, "kd" and "kf" are swept
     * and EOF resets the cursor. "kb" sits behind the cursor for the whole
     * pass and must be untouched. */
    trackingTestSweepToEnd(tt, 2);
    TEST_ASSERT(raxFind(tt, (unsigned char *)"kb", 2, &found));
    TEST_EXPECT(trackingTestKeyHasId(found, behind_dead));
    TEST_EXPECT(trackingGetTotalKeys() == 2); /* "kb" + "kf" (canary) */
    TEST_EXPECT(trackingGetTotalItems() == 2);

    /* The next pass starts from the beginning and reclaims "kb". */
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TEST_EXPECT(removed == 1);
    TEST_EXPECT(!raxFind(tt, (unsigned char *)"kb", 2, &found));
    TEST_EXPECT(trackingGetTotalKeys() == 1);
    TEST_EXPECT(trackingGetTotalItems() == 1);
    int all_live = 0;
    TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperReclaimsKeyInsertedBehindCursorOnNextPass(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperReclaimsKeyInsertedBehindCursorOnNextPass);
}

/* When every ID references a still-connected client, the sweeper removes
 * nothing and the counter is unchanged. */
static int trackingSweeperPreservesLiveIds(rax *tt) {
    uint64_t ka_ids[3] = {1, 2, 3};
    uint64_t kb_ids[2] = {1, 2};

    trackingTestAddLiveClient(1);
    trackingTestAddLiveClient(2);
    trackingTestAddLiveClient(3);

    trackingTestTrackKeyIds(tt, "ka", ka_ids, 3);
    trackingTestTrackKeyIds(tt, "kb", kb_ids, 2);

    TEST_EXPECT(trackingGetTotalKeys() == 2);
    TEST_EXPECT(trackingGetTotalItems() == 5);

    /* A full pass (5 checks, under one clock cadence) removes nothing. */
    uint64_t removed;
    TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TEST_EXPECT(removed == 0);

    TEST_EXPECT(trackingGetTotalKeys() == 2);
    TEST_EXPECT(trackingGetTotalItems() == 5);

    int all_live = 0;
    TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 5);
    TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperPreservesLiveIds(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperPreservesLiveIds);
}
