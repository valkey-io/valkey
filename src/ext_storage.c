/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Data tiering: engine-side wiring.
 *
 * This file connects the server to a storage engine. It holds the callbacks
 * that turn objects into bytes and back, opens the configured engine at
 * startup, and reports the engine's state through INFO. */

#include "server.h"

#ifdef USE_EXT_STORAGE

#include "ext_storage.h"
#include "rdb.h"
#include "storage/storage_mock.h"

/* ---------------------------------------------------------------------------
 * Storage engine holder
 *
 * The server uses one storage engine at a time. Registration happens once at
 * startup before any client is served and a second registration is rejected.
 * Lookup happens once when the engine is opened.
 * ---------------------------------------------------------------------------*/

static storageEngine registered_engine;
static int have_engine = 0;

storageStatus storageRegisterEngine(const storageEngine *engine) {
    if (engine == NULL || engine->name == NULL || engine->name[0] == '\0') return STORAGE_ERR_REJECTED;
    if (have_engine) return STORAGE_ERR_REJECTED;
    registered_engine = *engine;
    have_engine = 1;
    return STORAGE_OK;
}

const storageEngine *storageLookupEngine(const char *name) {
    if (name == NULL || !have_engine) return NULL;
    if (strcmp(registered_engine.name, name) == 0) return &registered_engine;
    return NULL;
}

/* Test-only reset. Allows test fixtures to isolate engine registration. */
void extStorageTestResetEngine(void) {
    have_engine = 0;
}

/* ---------------------------------------------------------------------------
 * Context
 * ---------------------------------------------------------------------------*/

typedef struct extStorageContext {
    const storageEngine *engine;
    void *ctx;
    /* The byte budget the engine was opened with. INFO reports this so it does
     * not have to re-read the config. */
    uint64_t capacity_bytes;
} extStorageContext;

static extStorageContext ext_storage = {0};

/* ---------------------------------------------------------------------------
 * Serialization
 *
 * The engine may call these from its own IO thread, so they must only touch
 * the object passed in.
 * ---------------------------------------------------------------------------*/

/* A key is a string object, so its bytes already live in the object's sds. We
 * return a pointer to those bytes instead of copying them, which is why
 * free_serialized_key has nothing to do. The bytes stay valid as long as the
 * object does, which is all the interface requires. */
static int extStorageSerializeKey(void *key_obj, char **out) {
    robj *o = key_obj;
    serverAssert(o->type == OBJ_STRING);
    sds s = objectGetVal(o);
    *out = s;
    return (int)sdslen(s);
}

static void extStorageFreeSerializedKey(char *bytes) {
    UNUSED(bytes);
}

/* Values are written in the DUMP wire format: a type byte, the object body, an
 * RDB version, and a CRC64. Reusing this format means every type and encoding
 * is already handled, and a record written by one server version can be read
 * by another that accepts the same RDB version. */
static int extStorageSerializeValue(void *val_obj, char **out) {
    rio payload;
    createDumpPayload(&payload, (robj *)val_obj, NULL, -1);
    size_t len = sdslen(payload.io.buffer.ptr);
    if (len > INT_MAX) {
        sdsfree(payload.io.buffer.ptr);
        *out = NULL;
        return -1;
    }
    *out = payload.io.buffer.ptr;
    return (int)len;
}

static void extStorageFreeSerializedValue(char *bytes) {
    if (bytes) sdsfree((sds)bytes);
}

static void *extStorageDeserializeKey(const char *bytes, int len) {
    if (len < 0) return NULL;
    return createStringObject(bytes, (size_t)len);
}

static void *extStorageDeserializeValue(const char *bytes, int len) {
    if (len < 0) return NULL;
    rio payload;
    sds buf = sdsnewlen(bytes, (size_t)len);
    rioInitWithBuffer(&payload, buf);
    int type = rdbLoadType(&payload);
    robj *o = (type == -1) ? NULL : rdbLoadObject(type, &payload, NULL, -1, NULL, RDBFLAGS_NONE, 0);
    sdsfree(buf);
    return o;
}

static const storageSerdes ext_storage_serdes = {
    .serialize_key = extStorageSerializeKey,
    .serialize_value = extStorageSerializeValue,
    .deserialize_key = extStorageDeserializeKey,
    .deserialize_value = extStorageDeserializeValue,
    .free_serialized_key = extStorageFreeSerializedKey,
    .free_serialized_value = extStorageFreeSerializedValue,
};

const storageSerdes *extStorageSerdes(void) {
    return &ext_storage_serdes;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------*/

/* Registers the engines that ship with the server. */
void extStorageRegisterBuiltinEngines(void) {
    if (storageRegisterEngine(storageMockEngine()) != STORAGE_OK) {
        serverLog(LL_WARNING, "Failed to register the built-in \"mock\" storage engine.");
        exit(1);
    }
}

void extStorageInit(void) {
    serverAssert(server.ext_storage_enabled);

    if (server.ext_storage_engine == NULL || server.ext_storage_engine[0] == '\0') {
        serverLog(LL_WARNING, "ext-storage-enabled is set but ext-storage-engine is empty.");
        exit(1);
    }

    const storageEngine *engine = storageLookupEngine(server.ext_storage_engine);
    if (engine == NULL) {
        serverLog(LL_WARNING,
                  "Unknown ext-storage-engine \"%s\". A module engine only appears if its module "
                  "was loaded from the configuration file or with --loadmodule.",
                  server.ext_storage_engine);
        exit(1);
    }

    storageConfig cfg = {
        .path = server.ext_storage_path,
        .capacity_bytes = server.ext_storage_capacity,
        .num_databases = (uint32_t)server.dbnum,
        .serdes = ext_storage_serdes,
    };

    void *ctx = engine->open(&cfg, engine->privdata);
    if (ctx == NULL) {
        serverLog(LL_WARNING, "Storage engine \"%s\" failed to open (path=\"%s\", capacity=%llu bytes).",
                  engine->name, server.ext_storage_path ? server.ext_storage_path : "",
                  (unsigned long long)server.ext_storage_capacity);
        exit(1);
    }

    ext_storage.engine = engine;
    ext_storage.ctx = ctx;
    ext_storage.capacity_bytes = cfg.capacity_bytes;

    serverLog(LL_NOTICE, "Data tiering enabled. Engine \"%s\", capacity %llu bytes, %s IO.", engine->name,
              (unsigned long long)server.ext_storage_capacity,
              engine->put_async ? "asynchronous" : "synchronous");
}

void extStorageDeinit(void) {
    if (ext_storage.ctx == NULL) return;
    ext_storage.engine->close(ext_storage.ctx);
    ext_storage.ctx = NULL;
    ext_storage.engine = NULL;
}

int extStorageIsActive(void) {
    return ext_storage.ctx != NULL;
}

const storageEngine *extStorageEngine(void) {
    return ext_storage.engine;
}

void *extStorageEngineCtx(void) {
    return ext_storage.ctx;
}

/* ---------------------------------------------------------------------------
 * INFO
 * ---------------------------------------------------------------------------*/
sds extStorageInfoString(sds info) {
    if (!extStorageIsActive()) return sdscatprintf(info, "ext_storage_enabled:0\r\n");

    storageStats st = {0};
    ext_storage.engine->get_stats(ext_storage.ctx, &st);

    return sdscatprintf(info,
                        "ext_storage_enabled:1\r\n"
                        "ext_storage_engine:%s\r\n"
                        "ext_storage_api_version:%d\r\n"
                        "ext_storage_async_io:%d\r\n"
                        "ext_storage_capacity_bytes:%llu\r\n"
                        "ext_storage_records:%llu\r\n"
                        "ext_storage_bytes:%llu\r\n"
                        "ext_storage_puts:%llu\r\n"
                        "ext_storage_gets:%llu\r\n"
                        "ext_storage_dels:%llu\r\n",
                        ext_storage.engine->name, VALKEY_STORAGE_API_VERSION,
                        ext_storage.engine->put_async != NULL ? 1 : 0, (unsigned long long)ext_storage.capacity_bytes,
                        (unsigned long long)st.records, (unsigned long long)st.bytes, (unsigned long long)st.puts,
                        (unsigned long long)st.gets, (unsigned long long)st.dels);
}

#endif /* USE_EXT_STORAGE */
