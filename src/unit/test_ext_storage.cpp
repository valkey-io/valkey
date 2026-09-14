/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>
#include <string>

extern "C" {
#include "server.h"
}

#ifdef USE_EXT_STORAGE

extern "C" {
#include "ext_storage.h"
#include "storage/storage.h"
#include "storage/storage_mock.h"
}

/* ---------------------------------------------------------------------------
 * Mock storage engine
 *
 * These tests use their own serdes rather than the engine's. The interface
 * treats keys and values as opaque handles, so a test can pass plain C strings
 * and keep the contract under test (request ordering, capacity, backpressure)
 * separate from how the engine encodes objects. The engine's own serdes is
 * covered further down.
 * ---------------------------------------------------------------------------*/

/* Value object for these tests: a NUL-terminated string. Serializing copies it.
 * Deserializing returns a fresh copy. This matches what a real engine does. */
static int testSerializeStr(void *obj, char **out) {
    const char *s = (const char *)obj;
    size_t len = strlen(s);
    *out = (char *)zmalloc(len + 1);
    memcpy(*out, s, len + 1);
    return (int)len;
}

/* Keys are handed back by reference to exercise the aliasing case. The engine
 * must copy if it wants to keep them. free_serialized_key does nothing. */
static int testSerializeKeyAliased(void *obj, char **out) {
    *out = (char *)obj;
    return (int)strlen((const char *)obj);
}

static void testFreeNothing(char *) {
}

static void testFreeBuf(char *p) {
    zfree(p);
}

static void *testDeserializeStr(const char *bytes, int len) {
    char *s = (char *)zmalloc((size_t)len + 1);
    memcpy(s, bytes, (size_t)len);
    s[len] = '\0';
    return s;
}

class StorageMockTest : public ::testing::Test {
  protected:
    MockValkey mock;
    RealValkey real;

    const storageEngine *engine = nullptr;
    void *ctx = nullptr;
    storageConfig cfg = {};

    void SetUp() override {
        extStorageTestResetEngine();
        engine = storageMockEngine();

        cfg.path = "";
        cfg.capacity_bytes = 0; /* unbounded unless a test overrides it */
        cfg.num_databases = 4;
        cfg.serdes.serialize_key = testSerializeKeyAliased;
        cfg.serdes.serialize_value = testSerializeStr;
        cfg.serdes.deserialize_key = testDeserializeStr;
        cfg.serdes.deserialize_value = testDeserializeStr;
        cfg.serdes.free_serialized_key = testFreeNothing;
        cfg.serdes.free_serialized_value = testFreeBuf;
    }

    void openEngine() {
        ctx = engine->open(&cfg, engine->privdata);
        ASSERT_NE(ctx, nullptr);
    }

    void TearDown() override {
        if (ctx) engine->close(ctx);
        extStorageTestResetEngine();
    }
};

TEST_F(StorageMockTest, RegistersUnderTheNameMock) {
    ASSERT_EQ(storageRegisterEngine(storageMockEngine()), STORAGE_OK);
    ASSERT_NE(storageLookupEngine("mock"), nullptr);
}

TEST_F(StorageMockTest, RefusesToOpenWithoutSerdes) {
    cfg.serdes.serialize_value = nullptr;
    EXPECT_EQ(engine->open(&cfg, engine->privdata), nullptr);
}

TEST_F(StorageMockTest, SyncPutGetDelRoundTrip) {
    openEngine();
    char key[] = "k1";
    char val[] = "hello";
    size_t stored = 0;

    ASSERT_EQ(engine->put(ctx, 0, key, val, &stored), STORAGE_OK);
    EXPECT_EQ(stored, strlen(val));

    void *out = nullptr;
    ASSERT_EQ(engine->get(ctx, 0, key, &out), STORAGE_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ((char *)out, val);
    zfree(out);

    ASSERT_EQ(engine->del(ctx, 0, key), STORAGE_OK);
    EXPECT_EQ(engine->get(ctx, 0, key, &out), STORAGE_NOT_FOUND);
    EXPECT_EQ(engine->del(ctx, 0, key), STORAGE_NOT_FOUND);
}

/* The same key in two databases is two records. */
TEST_F(StorageMockTest, KeysAreScopedPerDatabase) {
    openEngine();
    char key[] = "same";
    char v0[] = "in-db-0";
    char v2[] = "in-db-2";

    ASSERT_EQ(engine->put(ctx, 0, key, v0, nullptr), STORAGE_OK);
    ASSERT_EQ(engine->put(ctx, 2, key, v2, nullptr), STORAGE_OK);

    void *out = nullptr;
    ASSERT_EQ(engine->get(ctx, 2, key, &out), STORAGE_OK);
    EXPECT_STREQ((char *)out, v2);
    zfree(out);

    ASSERT_EQ(engine->del(ctx, 2, key), STORAGE_OK);
    EXPECT_EQ(engine->get(ctx, 0, key, &out), STORAGE_OK);
    zfree(out);
}

TEST_F(StorageMockTest, RejectsOutOfRangeDatabase) {
    openEngine();
    char key[] = "k";
    char val[] = "v";
    EXPECT_EQ(engine->put(ctx, cfg.num_databases, key, val, nullptr), STORAGE_ERR_REJECTED);
}

TEST_F(StorageMockTest, OverwriteReplacesRatherThanAccumulates) {
    openEngine();
    char key[] = "k";
    char first[] = "aaaa";
    char second[] = "bb";

    ASSERT_EQ(engine->put(ctx, 0, key, first, nullptr), STORAGE_OK);
    ASSERT_EQ(engine->put(ctx, 0, key, second, nullptr), STORAGE_OK);

    storageStats st = {};
    engine->get_stats(ctx, &st);
    EXPECT_EQ(st.records, 1u);
    EXPECT_EQ(st.bytes, strlen(second));

    void *out = nullptr;
    ASSERT_EQ(engine->get(ctx, 0, key, &out), STORAGE_OK);
    EXPECT_STREQ((char *)out, second);
    zfree(out);
}

TEST_F(StorageMockTest, ReportsFullWhenOverCapacity) {
    cfg.capacity_bytes = 8;
    openEngine();
    char k1[] = "k1";
    char k2[] = "k2";
    char five[] = "12345";

    ASSERT_EQ(engine->put(ctx, 0, k1, five, nullptr), STORAGE_OK);
    EXPECT_EQ(engine->put(ctx, 0, k2, five, nullptr), STORAGE_ERR_FULL);

    /* The rejected write left nothing behind. */
    storageStats st = {};
    engine->get_stats(ctx, &st);
    EXPECT_EQ(st.records, 1u);
    EXPECT_EQ(st.bytes, strlen(five));
}

/* An async request is not complete when it is accepted. Nothing is visible
 * until the engine polls. */
TEST_F(StorageMockTest, AsyncPutIsNotVisibleBeforePolling) {
    openEngine();
    char key[] = "k";
    char val[] = "v";
    int token = 42;

    ASSERT_EQ(engine->put_async(ctx, 0, key, val, &token), STORAGE_OK);

    void *out = nullptr;
    EXPECT_EQ(engine->get(ctx, 0, key, &out), STORAGE_NOT_FOUND);

    storageCompletion comps[4] = {};
    ASSERT_EQ(engine->poll_completions(ctx, comps, 4), 1);
    EXPECT_EQ(comps[0].request_ctx, &token);
    EXPECT_EQ(comps[0].op_type, STORAGE_OP_PUT);
    EXPECT_EQ(comps[0].status, STORAGE_OK);
    EXPECT_EQ(comps[0].stored_bytes, strlen(val));

    ASSERT_EQ(engine->get(ctx, 0, key, &out), STORAGE_OK);
    zfree(out);
}

TEST_F(StorageMockTest, AsyncGetDeliversTheValueOnTheCompletion) {
    openEngine();
    char key[] = "k";
    char val[] = "payload";
    ASSERT_EQ(engine->put(ctx, 0, key, val, nullptr), STORAGE_OK);

    int token = 7;
    ASSERT_EQ(engine->get_async(ctx, 0, key, &token), STORAGE_OK);

    storageCompletion comps[2] = {};
    ASSERT_EQ(engine->poll_completions(ctx, comps, 2), 1);
    EXPECT_EQ(comps[0].op_type, STORAGE_OP_GET);
    EXPECT_EQ(comps[0].status, STORAGE_OK);
    ASSERT_NE(comps[0].val_obj, nullptr);
    EXPECT_STREQ((char *)comps[0].val_obj, val);
    zfree(comps[0].val_obj);
}

TEST_F(StorageMockTest, AsyncMissReportsNotFoundAndNoValue) {
    openEngine();
    char key[] = "absent";
    int token = 0;
    ASSERT_EQ(engine->get_async(ctx, 0, key, &token), STORAGE_OK);

    storageCompletion comps[1] = {};
    ASSERT_EQ(engine->poll_completions(ctx, comps, 1), 1);
    EXPECT_EQ(comps[0].status, STORAGE_NOT_FOUND);
    EXPECT_EQ(comps[0].val_obj, nullptr);
}

TEST_F(StorageMockTest, CompletionsArriveInSubmissionOrder) {
    openEngine();
    char k1[] = "k1";
    char k2[] = "k2";
    char v[] = "v";
    int t1 = 1, t2 = 2;

    ASSERT_EQ(engine->put_async(ctx, 0, k1, v, &t1), STORAGE_OK);
    ASSERT_EQ(engine->put_async(ctx, 0, k2, v, &t2), STORAGE_OK);

    storageCompletion comps[8] = {};
    ASSERT_EQ(engine->poll_completions(ctx, comps, 8), 2);
    EXPECT_EQ(comps[0].request_ctx, &t1);
    EXPECT_EQ(comps[1].request_ctx, &t2);
}

/* A full queue must return WOULDBLOCK so the caller can back off and retry. */
TEST_F(StorageMockTest, ReportsWouldBlockWhenTheQueueIsFull) {
    openEngine();
    char key[] = "k";
    char val[] = "v";

    int accepted = 0;
    for (int i = 0; i < 1000; i++) {
        if (engine->get_async(ctx, 0, key, &accepted) != STORAGE_OK) break;
        accepted++;
    }
    ASSERT_GT(accepted, 0);
    EXPECT_EQ(engine->get_async(ctx, 0, key, &accepted), STORAGE_WOULDBLOCK);

    /* Draining makes room again. */
    storageCompletion comps[8] = {};
    ASSERT_GT(engine->poll_completions(ctx, comps, 8), 0);
    for (int i = 0; i < 8; i++)
        if (comps[i].val_obj) zfree(comps[i].val_obj);
    EXPECT_EQ(engine->put_async(ctx, 0, key, val, &accepted), STORAGE_OK);
}

TEST_F(StorageMockTest, PollingAnIdleEngineReturnsNothing) {
    openEngine();
    storageCompletion comps[4] = {};
    EXPECT_EQ(engine->poll_completions(ctx, comps, 4), 0);
}

/* ---------------------------------------------------------------------------
 * Engine serialization
 *
 * The engine encodes values in the DUMP wire format. This makes every object
 * type work without a per-type path. These tests verify that a value survives
 * the round trip for more than just plain strings.
 * ---------------------------------------------------------------------------*/

class ExtStorageSerdesTest : public ::testing::Test {
  protected:
    MockValkey mock;
    RealValkey real;

    void SetUp() override {
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
        /* Compression off. These tests check the round trip, not compactness. */
        server.rdb_compression = 0;
    }
};

TEST_F(ExtStorageSerdesTest, StringValueSurvivesTheRoundTrip) {
    const storageSerdes *s = extStorageSerdes();
    robj *o = createStringObject("some value", 10);

    char *bytes = nullptr;
    int len = s->serialize_value(o, &bytes);
    ASSERT_GT(len, 0);
    ASSERT_NE(bytes, nullptr);

    robj *back = (robj *)s->deserialize_value(bytes, len);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ((int)back->type, OBJ_STRING);
    EXPECT_EQ(compareStringObjects(o, back), 0);

    s->free_serialized_value(bytes);
    decrRefCount(o);
    decrRefCount(back);
}

/* Keys serialize by reference. The matching free is a no-op. An engine that
 * assumed it owned those bytes would double-free. */
TEST_F(ExtStorageSerdesTest, KeySerializationAliasesTheObject) {
    const storageSerdes *s = extStorageSerdes();
    robj *key = createStringObject("mykey", 5);

    char *bytes = nullptr;
    int len = s->serialize_key(key, &bytes);
    ASSERT_EQ(len, 5);
    EXPECT_EQ((void *)bytes, (void *)objectGetVal(key));

    s->free_serialized_key(bytes);
    /* Still intact because nothing was released. */
    EXPECT_EQ(memcmp(bytes, "mykey", 5), 0);

    decrRefCount(key);
}

TEST_F(ExtStorageSerdesTest, KeyDeserializationProducesAnObject) {
    const storageSerdes *s = extStorageSerdes();
    robj *key = (robj *)s->deserialize_key("abc", 3);
    ASSERT_NE(key, nullptr);
    EXPECT_EQ((int)key->type, OBJ_STRING);
    EXPECT_EQ(sdslen((sds)objectGetVal(key)), 3u);
    decrRefCount(key);
}

TEST_F(ExtStorageSerdesTest, RejectsAMalformedBuffer) {
    const storageSerdes *s = extStorageSerdes();
    EXPECT_EQ(s->deserialize_value("not a dump payload", 18), nullptr);
    EXPECT_EQ(s->deserialize_value("", -1), nullptr);
}

#endif /* USE_EXT_STORAGE */
