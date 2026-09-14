/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* In-memory reference storage engine.
 *
 * Exercises the tiering interface without a real device. Keeps serialized
 * values in a chained hash table per database, addressed by serialized key.
 *
 * Both synchronous and asynchronous operations are implemented. The async path
 * queues requests and executes them inside poll_completions() on the main
 * thread. This proves the request/completion protocol while keeping tests
 * deterministic. It does not move serialization off the main thread.
 *
 * This file links only against storage.h so unit tests can build it without
 * the server.
 */

#include "storage.h"

#include <stdlib.h>
#include <string.h>

#define MOCK_BUCKETS 1024
/* Maximum queued requests before STORAGE_WOULDBLOCK. Lets tests exercise the
 * engine's backpressure handling. */
#define MOCK_MAX_INFLIGHT 256

typedef struct mockRecord {
    char *key;
    size_t klen;
    char *val;
    size_t vlen;
    struct mockRecord *next;
} mockRecord;

typedef struct mockDb {
    mockRecord *buckets[MOCK_BUCKETS];
} mockDb;

/* A queued async request, executed by poll_completions(). */
typedef struct mockRequest {
    storageOpType op;
    uint32_t db_id;
    void *key_obj;
    void *val_obj;
    void *request_ctx;
} mockRequest;

typedef struct mockCtx {
    storageConfig cfg;
    mockDb *dbs;
    uint32_t num_databases;

    mockRequest inflight[MOCK_MAX_INFLIGHT];
    int inflight_head;
    int inflight_len;

    storageStats stats;
} mockCtx;

/* FNV-1a hash. Distribution only needs to be reasonable, not cryptographic. */
static size_t mockHash(const char *p, size_t len) {
    size_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h % MOCK_BUCKETS;
}

static mockRecord **mockFindSlot(mockCtx *c, uint32_t db_id, const char *key, size_t klen) {
    mockRecord **slot = &c->dbs[db_id].buckets[mockHash(key, klen)];
    while (*slot) {
        if ((*slot)->klen == klen && memcmp((*slot)->key, key, klen) == 0) return slot;
        slot = &(*slot)->next;
    }
    return slot; /* Points at NULL terminator, ready for insertion. */
}

static void mockFreeRecord(mockRecord *r) {
    free(r->key);
    free(r->val);
    free(r);
}

/* ---------------------------------------------------------------------------
 * Serialized key helper
 *
 * serialize_key() may return a pointer into engine memory rather than a copy.
 * The bytes are valid only until the matching free callback runs. Use
 * mockAcquireKey/mockReleaseKey as a matched pair.
 * ---------------------------------------------------------------------------*/
typedef struct mockKeyBytes {
    char *p;
    int len;
} mockKeyBytes;

static int mockAcquireKey(mockCtx *c, void *key_obj, mockKeyBytes *out) {
    out->len = c->cfg.serdes.serialize_key(key_obj, &out->p);
    return out->len >= 0 && out->p != NULL;
}

static void mockReleaseKey(mockCtx *c, mockKeyBytes *k) {
    if (c->cfg.serdes.free_serialized_key) c->cfg.serdes.free_serialized_key(k->p);
}

/* ---------------------------------------------------------------------------
 * Synchronous path
 * ---------------------------------------------------------------------------*/

static storageStatus mockPut(void *ctx, uint32_t db_id, void *key_obj, void *val_obj, size_t *stored_bytes) {
    mockCtx *c = ctx;
    if (db_id >= c->num_databases) return STORAGE_ERR_REJECTED;

    char *vbytes = NULL;
    int vlen = c->cfg.serdes.serialize_value(val_obj, &vbytes);
    if (vlen < 0 || vbytes == NULL) return STORAGE_ERR_REJECTED;

    mockKeyBytes k;
    if (!mockAcquireKey(c, key_obj, &k)) {
        c->cfg.serdes.free_serialized_value(vbytes);
        return STORAGE_ERR_REJECTED;
    }

    storageStatus st = STORAGE_OK;
    mockRecord **slot = mockFindSlot(c, db_id, k.p, (size_t)k.len);

    /* On overwrite, release old value first so capacity check reflects the
     * final cost. */
    size_t freed = *slot ? (*slot)->vlen : 0;
    if (c->cfg.capacity_bytes && c->stats.bytes - freed + (uint64_t)vlen > c->cfg.capacity_bytes) {
        st = STORAGE_ERR_FULL;
        goto done;
    }

    if (*slot) {
        free((*slot)->val);
        c->stats.bytes -= (*slot)->vlen;
    } else {
        mockRecord *r = calloc(1, sizeof(*r));
        if (r == NULL) {
            st = STORAGE_ERR_IO;
            goto done;
        }
        r->key = malloc((size_t)k.len ? (size_t)k.len : 1);
        if (r->key == NULL) {
            free(r);
            st = STORAGE_ERR_IO;
            goto done;
        }
        memcpy(r->key, k.p, (size_t)k.len);
        r->klen = (size_t)k.len;
        *slot = r;
        c->stats.records++;
    }

    (*slot)->val = malloc((size_t)vlen ? (size_t)vlen : 1);
    if ((*slot)->val == NULL) {
        st = STORAGE_ERR_IO;
        goto done;
    }
    memcpy((*slot)->val, vbytes, (size_t)vlen);
    (*slot)->vlen = (size_t)vlen;
    c->stats.bytes += (size_t)vlen;
    c->stats.puts++;
    if (stored_bytes) *stored_bytes = (size_t)vlen;

done:
    mockReleaseKey(c, &k);
    c->cfg.serdes.free_serialized_value(vbytes);
    return st;
}

static storageStatus mockGet(void *ctx, uint32_t db_id, void *key_obj, void **val_obj_out) {
    mockCtx *c = ctx;
    *val_obj_out = NULL;
    if (db_id >= c->num_databases) return STORAGE_ERR_REJECTED;

    mockKeyBytes k;
    if (!mockAcquireKey(c, key_obj, &k)) return STORAGE_ERR_REJECTED;

    storageStatus st;
    mockRecord **slot = mockFindSlot(c, db_id, k.p, (size_t)k.len);
    if (*slot == NULL) {
        st = STORAGE_NOT_FOUND;
    } else {
        void *obj = c->cfg.serdes.deserialize_value((*slot)->val, (int)(*slot)->vlen);
        if (obj == NULL) {
            st = STORAGE_ERR_IO;
        } else {
            *val_obj_out = obj;
            c->stats.gets++;
            st = STORAGE_OK;
        }
    }
    mockReleaseKey(c, &k);
    return st;
}

static storageStatus mockDel(void *ctx, uint32_t db_id, void *key_obj) {
    mockCtx *c = ctx;
    if (db_id >= c->num_databases) return STORAGE_ERR_REJECTED;

    mockKeyBytes k;
    if (!mockAcquireKey(c, key_obj, &k)) return STORAGE_ERR_REJECTED;

    storageStatus st;
    mockRecord **slot = mockFindSlot(c, db_id, k.p, (size_t)k.len);
    if (*slot == NULL) {
        st = STORAGE_NOT_FOUND;
    } else {
        mockRecord *victim = *slot;
        *slot = victim->next;
        c->stats.bytes -= victim->vlen;
        c->stats.records--;
        c->stats.dels++;
        mockFreeRecord(victim);
        st = STORAGE_OK;
    }
    mockReleaseKey(c, &k);
    return st;
}

/* ---------------------------------------------------------------------------
 * Asynchronous path
 * ---------------------------------------------------------------------------*/

static storageStatus mockSubmit(mockCtx *c, storageOpType op, uint32_t db_id, void *key_obj, void *val_obj,
                                void *request_ctx) {
    if (c->inflight_len == MOCK_MAX_INFLIGHT) return STORAGE_WOULDBLOCK;
    int tail = (c->inflight_head + c->inflight_len) % MOCK_MAX_INFLIGHT;
    c->inflight[tail] = (mockRequest){
        .op = op, .db_id = db_id, .key_obj = key_obj, .val_obj = val_obj, .request_ctx = request_ctx};
    c->inflight_len++;
    return STORAGE_OK;
}

static storageStatus mockPutAsync(void *ctx, uint32_t db_id, void *key_obj, void *val_obj, void *request_ctx) {
    return mockSubmit(ctx, STORAGE_OP_PUT, db_id, key_obj, val_obj, request_ctx);
}

static storageStatus mockGetAsync(void *ctx, uint32_t db_id, void *key_obj, void *request_ctx) {
    return mockSubmit(ctx, STORAGE_OP_GET, db_id, key_obj, NULL, request_ctx);
}

static storageStatus mockDelAsync(void *ctx, uint32_t db_id, void *key_obj, void *request_ctx) {
    return mockSubmit(ctx, STORAGE_OP_DEL, db_id, key_obj, NULL, request_ctx);
}

static int mockPollCompletions(void *ctx, storageCompletion *out, int max) {
    mockCtx *c = ctx;
    int n = 0;
    while (n < max && c->inflight_len > 0) {
        mockRequest *req = &c->inflight[c->inflight_head];
        storageCompletion *comp = &out[n];
        memset(comp, 0, sizeof(*comp));
        comp->request_ctx = req->request_ctx;
        comp->op_type = req->op;
        comp->db_id = req->db_id;

        switch (req->op) {
        case STORAGE_OP_PUT:
            comp->status = mockPut(c, req->db_id, req->key_obj, req->val_obj, &comp->stored_bytes);
            break;
        case STORAGE_OP_GET: comp->status = mockGet(c, req->db_id, req->key_obj, &comp->val_obj); break;
        case STORAGE_OP_DEL: comp->status = mockDel(c, req->db_id, req->key_obj); break;
        }

        c->inflight_head = (c->inflight_head + 1) % MOCK_MAX_INFLIGHT;
        c->inflight_len--;
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------*/

static void *mockOpen(const storageConfig *cfg, void *privdata) {
    (void)privdata;
    if (cfg == NULL || cfg->num_databases == 0) return NULL;
    /* Serialization callbacks are required. Without them the engine cannot
     * convert objects to bytes. */
    if (cfg->serdes.serialize_key == NULL || cfg->serdes.serialize_value == NULL ||
        cfg->serdes.deserialize_value == NULL || cfg->serdes.free_serialized_value == NULL) {
        return NULL;
    }

    mockCtx *c = calloc(1, sizeof(*c));
    if (c == NULL) return NULL;
    c->dbs = calloc(cfg->num_databases, sizeof(mockDb));
    if (c->dbs == NULL) {
        free(c);
        return NULL;
    }
    c->cfg = *cfg;
    c->num_databases = cfg->num_databases;
    return c;
}

static void mockClose(void *ctx) {
    mockCtx *c = ctx;
    if (c == NULL) return;
    for (uint32_t d = 0; d < c->num_databases; d++) {
        for (int b = 0; b < MOCK_BUCKETS; b++) {
            mockRecord *r = c->dbs[d].buckets[b];
            while (r) {
                mockRecord *next = r->next;
                mockFreeRecord(r);
                r = next;
            }
        }
    }
    free(c->dbs);
    free(c);
}

static void mockGetStats(void *ctx, storageStats *out) {
    mockCtx *c = ctx;
    *out = c->stats;
}

static const storageEngine mock_storage_engine = {
    .name = "mock",
    .api_version = VALKEY_STORAGE_API_VERSION,
    .open = mockOpen,
    .close = mockClose,
    .put = mockPut,
    .get = mockGet,
    .del = mockDel,
    .put_async = mockPutAsync,
    .get_async = mockGetAsync,
    .del_async = mockDelAsync,
    .poll_completions = mockPollCompletions,
    .get_stats = mockGetStats,
};

const storageEngine *storageMockEngine(void) {
    return &mock_storage_engine;
}
