/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Pluggable storage engine interface for data tiering.
 *
 * A storage engine holds values that have been spilled out of memory. Keys
 * never spill. They stay in the main hashtable, which owns each key's TTL and
 * idle time. The engine stores value bytes addressed by (db_id, key).
 *
 * WHAT CROSSES THIS BOUNDARY
 *
 * Keys and values are passed as robj pointers, declared here as void * so
 * engines need not know the object layout. An engine turns them into bytes by
 * calling the serialization callbacks in storageConfig.serdes.
 *
 * Objects cross the boundary instead of bytes for threading. The main thread
 * should not spend CPU on serialization. An asynchronous engine calls
 * serialize_value() from its own IO thread. A synchronous engine has no thread
 * of its own and serializes on the caller's thread.
 *
 * OBJECT LIFETIME
 *
 * For synchronous entry points, objects are valid only for the duration of
 * the call.
 *
 * For asynchronous entry points, the server guarantees that key and value
 * objects stay allocated and unmodified until the engine reports completion.
 * The engine must not retain either pointer after that. Serialized buffers
 * from serdes callbacks must be released with the matching free callback.
 *
 * SERIALIZED KEYS MAY ALIAS SERVER MEMORY
 *
 * serialize_key() may return a pointer into the server's own allocation
 * instead of a copy. In that case free_serialized_key() does nothing. The
 * bytes are valid as long as the object is. An engine that needs to keep the
 * key must copy it.
 */

#ifndef VALKEY_STORAGE_H
#define VALKEY_STORAGE_H

#include <stddef.h>
#include <stdint.h>

/* Bumped when the layout of any struct here or the signature of any function
 * pointer changes. An engine reports the version it was built against. The
 * server refuses a mismatch instead of crashing. */
#define VALKEY_STORAGE_API_VERSION 1

/* Result of a storage operation. Negative values are failures. */
typedef enum {
    STORAGE_OK = 0,
    /* No record exists for this (db_id, key). */
    STORAGE_NOT_FOUND = 1,
    /* The engine cannot accept the request right now. An IO queue is full or a
     * rate limit is in effect. The record is untouched and the caller may
     * retry. Distinct from NOT_FOUND: treating backpressure as a miss would
     * drop a value still on disk. */
    STORAGE_WOULDBLOCK = 2,
    STORAGE_ERR_IO = -1,
    /* Out of capacity. */
    STORAGE_ERR_FULL = -2,
    /* Rejected on policy grounds. For example, the value exceeds a size limit. */
    STORAGE_ERR_REJECTED = -3,
} storageStatus;

typedef enum {
    STORAGE_OP_PUT = 0,
    STORAGE_OP_GET = 1,
    STORAGE_OP_DEL = 2,
} storageOpType;

/* ---------------------------------------------------------------------------
 * Serialization callbacks
 *
 * Supplied by the server, invoked by the engine. All four serialize/
 * deserialize functions may run on the engine's IO thread. They must not
 * touch server data structures beyond the object handed to them.
 * ---------------------------------------------------------------------------*/
typedef struct storageSerdes {
    /* Object to bytes. Returns the byte length, or -1 if the object cannot be
     * serialized. For values, this includes exceeding a configured size limit.
     * On failure *out is set to NULL. */
    int (*serialize_key)(void *key_obj, char **out);
    int (*serialize_value)(void *val_obj, char **out);

    /* Bytes to object. Returns NULL on a malformed buffer. The caller owns the
     * result and hands it to the server through a completion. */
    void *(*deserialize_key)(const char *bytes, int len);
    void *(*deserialize_value)(const char *bytes, int len);

    /* Release a buffer returned by the matching serialize function. May be a
     * no-op when that function aliased server memory. */
    void (*free_serialized_key)(char *bytes);
    void (*free_serialized_value)(char *bytes);
} storageSerdes;

/* ---------------------------------------------------------------------------
 * Completions
 * ---------------------------------------------------------------------------*/

/* One finished asynchronous request. Produced by the engine, consumed by the
 * server on the main thread inside poll_completions(). */
typedef struct storageCompletion {
    /* Opaque token the server passed to the *_async call. Returned unchanged. */
    void *request_ctx;
    storageOpType op_type;
    storageStatus status;
    uint32_t db_id;

    /* GET only, when status is STORAGE_OK: the reconstructed value object from
     * deserialize_value(). Ownership transfers to the server. NULL for other
     * ops and for any failure. */
    void *val_obj;

    /* PUT only: byte length the value occupied after serialization. The server
     * uses it to account for what the spill reclaimed. Zero when not
     * applicable. */
    size_t stored_bytes;
} storageCompletion;

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------*/
typedef struct storageConfig {
    /* Filesystem path the engine should use. Engines that do not touch the
     * filesystem ignore it. */
    const char *path;
    /* Byte budget the engine must stay within. */
    uint64_t capacity_bytes;
    /* Number of logical databases. db_id passed to any operation is always
     * less than this. */
    uint32_t num_databases;
    /* Serialization callbacks. Valid for the lifetime of the engine. */
    storageSerdes serdes;
} storageConfig;

typedef struct storageStats {
    uint64_t puts;
    uint64_t gets;
    uint64_t dels;
    /* Records currently held. */
    uint64_t records;
    /* Serialized bytes currently held, excluding engine-internal overhead. */
    uint64_t bytes;
} storageStats;

/* ---------------------------------------------------------------------------
 * The engine itself
 *
 * A storage engine implements the synchronous triple (put, get, del), the
 * asynchronous set (put_async, get_async, del_async, poll_completions), or
 * both. The asynchronous set is all-or-none. storageRegisterEngine() enforces
 * these rules. The server picks the asynchronous path when available.
 * ---------------------------------------------------------------------------*/
typedef struct storageEngine {
    /* Value of ext-storage-engine that selects this implementation. */
    const char *name;
    /* Must equal VALKEY_STORAGE_API_VERSION. */
    int api_version;
    /* Opaque pointer supplied at registration and handed back to open(). An
     * engine compiled into the server leaves it NULL. A module-based engine
     * uses it to find its own dispatch table. */
    void *privdata;

    /* Returns an opaque engine context, or NULL on failure. */
    void *(*open)(const storageConfig *cfg, void *privdata);
    /* Releases everything open() acquired. The server will not call any other
     * entry point on this context afterwards. */
    void (*close)(void *ctx);

    /* Synchronous operations. Serialization happens on the calling thread. */
    storageStatus (*put)(void *ctx, uint32_t db_id, void *key_obj, void *val_obj, size_t *stored_bytes);
    storageStatus (*get)(void *ctx, uint32_t db_id, void *key_obj, void **val_obj_out);
    storageStatus (*del)(void *ctx, uint32_t db_id, void *key_obj);

    /* Asynchronous operations. STORAGE_OK means the request was accepted and
     * exactly one completion will follow. Any other return means it was not
     * accepted and no completion will follow. */
    storageStatus (*put_async)(void *ctx, uint32_t db_id, void *key_obj, void *val_obj, void *request_ctx);
    storageStatus (*get_async)(void *ctx, uint32_t db_id, void *key_obj, void *request_ctx);
    storageStatus (*del_async)(void *ctx, uint32_t db_id, void *key_obj, void *request_ctx);

    /* Moves up to max finished requests into out[], newest last. Returns how
     * many were written. Called on the main thread. */
    int (*poll_completions)(void *ctx, storageCompletion *out, int max);

    void (*get_stats)(void *ctx, storageStats *out);
} storageEngine;

/* ---------------------------------------------------------------------------
 * Engine registration
 *
 * The server uses one storage engine at a time. Registration happens once at
 * startup before any client is served. The engine compiled into the server
 * registers from extStorageRegisterBuiltinEngines(), and a module engine
 * registers through ValkeyModule_RegisterStorageEngine().
 *
 * Registration copies the storageEngine struct. The caller's memory need not
 * outlive the call. A second registration is rejected. The only validation
 * performed is that the name is non-empty.
 * ---------------------------------------------------------------------------*/

/* Registers a storage engine. Returns STORAGE_OK on success, or
 * STORAGE_ERR_REJECTED if an engine is already registered or the name is
 * null or empty. */
storageStatus storageRegisterEngine(const storageEngine *engine);

/* Returns the registered engine if its name matches, otherwise NULL. */
const storageEngine *storageLookupEngine(const char *name);

#endif /* VALKEY_STORAGE_H */
