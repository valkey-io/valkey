/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "radix.h"

#define RADIX_SCAN_CURSOR_VERSION 1
#define RADIX_SCAN_DEFAULT_COUNT 10

typedef struct radixQueryOptions {
    int lengths;
    int withvalues;
    int fields_index;
    long long numfields;
    long long count;
    long long maxlen;
    int count_set;
    int maxlen_set;
} radixQueryOptions;

static radixObject *radixValue(robj *o) {
    return objectGetVal(o);
}

static int radixLookup(client *c, robj *key, int write, robj **result) {
    *result = write ? lookupKeyWrite(c->db, key) : lookupKeyRead(c->db, key);
    if (*result && checkType(c, *result, OBJ_RADIX)) return C_ERR;
    return C_OK;
}

static robj *radixFindPayload(robj *o, sds path) {
    void *payload = NULL;
    if (!o || !raxFind(radixValue(o)->index, (unsigned char *)path, sdslen(path), &payload)) return NULL;
    return payload;
}

static void radixReplyHashElement(client *c, hashTypeIterator *hi, int what) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;
        hashTypeCurrentFromListpack(hi, what, &vstr, &vlen, &vll);
        if (vstr)
            addReplyBulkCBuffer(c, vstr, vlen);
        else
            addReplyBulkLongLong(c, vll);
    } else {
        size_t len;
        char *value = hashTypeCurrentFromHashTable(hi, what, &len);
        addReplyBulkCBuffer(c, value, len);
    }
}

static void radixReplyPayload(client *c, robj *payload) {
    hashTypeIterator hi;
    addReplyArrayLen(c, hashTypeLength(payload) * 2);
    hashTypeInitIterator(payload, &hi);
    while (hashTypeNext(&hi) != C_ERR) {
        radixReplyHashElement(c, &hi, OBJ_HASH_FIELD);
        radixReplyHashElement(c, &hi, OBJ_HASH_VALUE);
    }
    hashTypeResetIterator(&hi);
}

static void radixReplyField(client *c, robj *payload, sds field) {
    robj *value = hashTypeGetValueObject(payload, field);
    if (value) {
        addReplyBulk(c, value);
        decrRefCount(value);
    } else {
        addReplyNull(c);
    }
}

static void radixReplyMatch(client *c,
                            const unsigned char *path,
                            size_t path_len,
                            robj *payload,
                            const radixQueryOptions *opts) {
    if (!opts->withvalues && opts->fields_index == 0) {
        if (opts->lengths)
            addReplyLongLong(c, path_len);
        else
            addReplyBulkCBuffer(c, path, path_len);
        return;
    }

    addReplyArrayLen(c, 2);
    if (opts->lengths)
        addReplyLongLong(c, path_len);
    else
        addReplyBulkCBuffer(c, path, path_len);

    if (opts->withvalues) {
        radixReplyPayload(c, payload);
    } else {
        addReplyArrayLen(c, opts->numfields);
        for (long long i = 0; i < opts->numfields; i++)
            radixReplyField(c, payload, objectGetVal(c->argv[opts->fields_index + i]));
    }
}

static int radixParseQueryOptions(client *c, int start, int prefixes, radixQueryOptions *opts) {
    *opts = (radixQueryOptions){.count = -1, .maxlen = -1};
    int i = start;
    while (i < c->argc) {
        char *arg = objectGetVal(c->argv[i]);
        if ((!prefixes && !strcasecmp(arg, "length")) || (prefixes && !strcasecmp(arg, "lengths"))) {
            if (opts->lengths) goto syntax;
            opts->lengths = 1;
            i++;
        } else if (!strcasecmp(arg, "withvalues")) {
            if (opts->withvalues || opts->fields_index) goto syntax;
            opts->withvalues = 1;
            i++;
        } else if (!strcasecmp(arg, "fields")) {
            long long count;
            if (opts->withvalues || opts->fields_index || i + 1 >= c->argc) goto syntax;
            if (getLongLongFromObjectOrReply(c, c->argv[i + 1], &count, NULL) != C_OK) return C_ERR;
            if (count < 0 || count > c->argc - i - 2) goto syntax;
            opts->fields_index = i + 2;
            opts->numfields = count;
            i += 2 + count;
        } else if (prefixes && !strcasecmp(arg, "count")) {
            if (opts->count_set || i + 1 >= c->argc) goto syntax;
            if (getLongLongFromObjectOrReply(c, c->argv[i + 1], &opts->count, NULL) != C_OK) return C_ERR;
            if (opts->count < 0 || (unsigned long long)opts->count > SIZE_MAX) {
                addReplyError(c, "COUNT must be non-negative");
                return C_ERR;
            }
            opts->count_set = 1;
            i += 2;
        } else if (prefixes && !strcasecmp(arg, "maxlen")) {
            if (opts->maxlen_set || i + 1 >= c->argc) goto syntax;
            if (getLongLongFromObjectOrReply(c, c->argv[i + 1], &opts->maxlen, NULL) != C_OK) return C_ERR;
            if (opts->maxlen < 0 || (unsigned long long)opts->maxlen > SIZE_MAX) {
                addReplyError(c, "MAXLEN must be non-negative");
                return C_ERR;
            }
            opts->maxlen_set = 1;
            i += 2;
        } else {
            goto syntax;
        }
    }
    return C_OK;

syntax:
    addReplyErrorObject(c, shared.syntaxerr);
    return C_ERR;
}

static void radixSignalChange(client *c, const char *event, long long dirty) {
    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_RADIX, (char *)event, c->argv[1], c->db->id);
    server.dirty += dirty;
}

void rsetCommand(client *c) {
    int nx = 0, xx = 0;
    if (c->argc == 6) {
        char *opt = objectGetVal(c->argv[5]);
        if (!strcasecmp(opt, "nx"))
            nx = 1;
        else if (!strcasecmp(opt, "xx"))
            xx = 1;
        else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    } else if (c->argc != 5) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    robj *o;
    if (radixLookup(c, c->argv[1], 1, &o) != C_OK) return;
    sds path = objectGetVal(c->argv[2]);
    sds field = objectGetVal(c->argv[3]);
    robj *payload = radixFindPayload(o, path);
    int exists = payload && hashTypeExists(payload, field);
    if ((nx && exists) || (xx && !exists)) {
        addReplyNull(c);
        return;
    }

    if (!o) {
        o = createRadixObject();
        dbAdd(c->db, c->argv[1], &o);
    }
    radixObject *rt = radixValue(o);
    if (!payload) {
        payload = createHashObject();
        serverAssert(raxInsert(rt->index, (unsigned char *)path, sdslen(path), payload, NULL));
        rt->num_paths++;
    }

    bool expired = false;
    int updated = hashTypeSet(payload,
                              field,
                              objectGetVal(c->argv[4]),
                              EXPIRY_NONE,
                              HASH_SET_COPY,
                              &expired);
    serverAssert(!expired);
    if (!updated) rt->num_fields++;
    radixSignalChange(c, "rset", 1);
    addReply(c, shared.ok);
}

void rgetCommand(client *c) {
    robj *o;
    if (radixLookup(c, c->argv[1], 0, &o) != C_OK) return;
    robj *payload = radixFindPayload(o, objectGetVal(c->argv[2]));
    if (!payload) {
        addReplyNull(c);
        return;
    }
    radixReplyField(c, payload, objectGetVal(c->argv[3]));
}

void rmgetCommand(client *c) {
    robj *o;
    if (radixLookup(c, c->argv[1], 0, &o) != C_OK) return;
    robj *payload = radixFindPayload(o, objectGetVal(c->argv[2]));
    addReplyArrayLen(c, c->argc - 3);
    for (int i = 3; i < c->argc; i++) {
        if (payload)
            radixReplyField(c, payload, objectGetVal(c->argv[i]));
        else
            addReplyNull(c);
    }
}

void rgetallCommand(client *c) {
    robj *o;
    if (radixLookup(c, c->argv[1], 0, &o) != C_OK) return;
    robj *payload = radixFindPayload(o, objectGetVal(c->argv[2]));
    if (payload)
        radixReplyPayload(c, payload);
    else
        addReplyArrayLen(c, 0);
}

void rdelCommand(client *c) {
    robj *o;
    if (radixLookup(c, c->argv[1], 1, &o) != C_OK) return;
    if (!o) {
        addReplyLongLong(c, 0);
        return;
    }

    radixObject *rt = radixValue(o);
    sds path = objectGetVal(c->argv[2]);
    robj *payload = radixFindPayload(o, path);
    if (!payload) {
        addReplyLongLong(c, 0);
        return;
    }

    long long deleted = 0;
    if (c->argc == 3) {
        rt->num_fields -= hashTypeLength(payload);
        serverAssert(raxRemove(rt->index, (unsigned char *)path, sdslen(path), NULL));
        decrRefCount(payload);
        rt->num_paths--;
        deleted = 1;
    } else {
        for (int i = 3; i < c->argc; i++) {
            if (hashTypeDelete(payload, objectGetVal(c->argv[i]))) {
                deleted++;
                rt->num_fields--;
            }
        }
        if (hashTypeLength(payload) == 0) {
            serverAssert(raxRemove(rt->index, (unsigned char *)path, sdslen(path), NULL));
            decrRefCount(payload);
            rt->num_paths--;
        }
    }

    if (deleted) {
        int keyremoved = rt->num_paths == 0;
        if (keyremoved) {
            dbDelete(c->db, c->argv[1]);
        }
        radixSignalChange(c, "rdel", deleted);
        if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    }
    addReplyLongLong(c, deleted);
}

void rlongestCommand(client *c) {
    radixQueryOptions opts;
    if (radixParseQueryOptions(c, 3, 0, &opts) != C_OK) return;

    robj *o;
    if (radixLookup(c, c->argv[1], 0, &o) != C_OK) return;
    if (!o) {
        addReplyNull(c);
        return;
    }

    sds query = objectGetVal(c->argv[2]);
    size_t match_len;
    void *payload;
    if (!raxFindLongestPrefix(radixValue(o)->index,
                              (unsigned char *)query,
                              sdslen(query),
                              &match_len,
                              &payload)) {
        addReplyNull(c);
        return;
    }
    radixReplyMatch(c, (unsigned char *)query, match_len, payload, &opts);
}

typedef struct radixPrefixCollector {
    robj **payloads;
    size_t *lengths;
    size_t capacity;
    size_t total;
    size_t maxlen;
    int use_maxlen;
} radixPrefixCollector;

static int radixCollectPrefix(unsigned char *key, size_t key_len, void *value, void *privdata) {
    UNUSED(key);
    radixPrefixCollector *collector = privdata;
    if (collector->use_maxlen && key_len > collector->maxlen) return 1;
    if (collector->capacity) {
        size_t slot = collector->total % collector->capacity;
        collector->payloads[slot] = value;
        collector->lengths[slot] = key_len;
    }
    collector->total++;
    return 1;
}

void rprefixesCommand(client *c) {
    radixQueryOptions opts;
    if (radixParseQueryOptions(c, 3, 1, &opts) != C_OK) return;

    robj *o;
    if (radixLookup(c, c->argv[1], 0, &o) != C_OK) return;
    if (!o || (opts.count_set && opts.count == 0)) {
        addReplyArrayLen(c, 0);
        return;
    }

    sds query = objectGetVal(c->argv[2]);
    size_t capacity = sdslen(query) + 1;
    if (radixValue(o)->num_paths < capacity) capacity = radixValue(o)->num_paths;
    if (opts.count_set && (size_t)opts.count < capacity) capacity = (size_t)opts.count;
    serverAssert(capacity <= SIZE_MAX / sizeof(robj *));
    serverAssert(capacity <= SIZE_MAX / sizeof(size_t));
    radixPrefixCollector collector = {
        .payloads = capacity ? zmalloc(sizeof(robj *) * capacity) : NULL,
        .lengths = capacity ? zmalloc(sizeof(size_t) * capacity) : NULL,
        .capacity = capacity,
        .maxlen = opts.maxlen_set ? (size_t)opts.maxlen : 0,
        .use_maxlen = opts.maxlen_set,
    };
    raxForEachPrefix(radixValue(o)->index,
                     (unsigned char *)query,
                     sdslen(query),
                     radixCollectPrefix,
                     &collector);

    size_t returned = collector.total < capacity ? collector.total : capacity;
    size_t start = collector.total > capacity ? collector.total % capacity : 0;
    addReplyArrayLen(c, returned);
    for (size_t i = 0; i < returned; i++) {
        size_t slot = (start + i) % capacity;
        radixReplyMatch(c,
                        (unsigned char *)query,
                        collector.lengths[slot],
                        collector.payloads[slot],
                        &opts);
    }
    zfree(collector.payloads);
    zfree(collector.lengths);
}

static int radixPathHasPrefix(const unsigned char *path,
                              size_t path_len,
                              const unsigned char *prefix,
                              size_t prefix_len) {
    return path_len >= prefix_len && memcmp(path, prefix, prefix_len) == 0;
}

void rdelprefixCommand(client *c) {
    robj *o;
    if (radixLookup(c, c->argv[1], 1, &o) != C_OK) return;
    if (!o) {
        addReplyLongLong(c, 0);
        return;
    }

    sds prefix = objectGetVal(c->argv[2]);
    radixObject *rt = radixValue(o);
    raxIterator ri;
    raxStart(&ri, rt->index);
    raxSeek(&ri, ">=", (unsigned char *)prefix, sdslen(prefix));
    long long deleted = 0;
    while (raxNext(&ri) &&
           radixPathHasPrefix(ri.key, ri.key_len, (unsigned char *)prefix, sdslen(prefix))) {
        sds path = sdsnewlen(ri.key, ri.key_len);
        robj *payload = ri.data;
        rt->num_fields -= hashTypeLength(payload);
        serverAssert(raxRemove(rt->index, (unsigned char *)path, sdslen(path), NULL));
        decrRefCount(payload);
        rt->num_paths--;
        deleted++;
        raxSeek(&ri, ">=", (unsigned char *)path, sdslen(path));
        sdsfree(path);
    }
    raxStop(&ri);

    if (deleted) {
        int keyremoved = rt->num_paths == 0;
        if (keyremoved) {
            dbDelete(c->db, c->argv[1]);
        }
        radixSignalChange(c, "rdelprefix", deleted);
        if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    }
    addReplyLongLong(c, deleted);
}

static sds radixEncodeScanCursor(sds prefix, sds last) {
    size_t prefix_len = sdslen(prefix);
    serverAssert(prefix_len <= UINT32_MAX);
    sds cursor = sdsnewlen(SDS_NOINIT, 5 + prefix_len + sdslen(last));
    cursor[0] = RADIX_SCAN_CURSOR_VERSION;
    cursor[1] = (prefix_len >> 24) & 0xff;
    cursor[2] = (prefix_len >> 16) & 0xff;
    cursor[3] = (prefix_len >> 8) & 0xff;
    cursor[4] = prefix_len & 0xff;
    memcpy(cursor + 5, prefix, prefix_len);
    memcpy(cursor + 5 + prefix_len, last, sdslen(last));
    return cursor;
}

static int radixDecodeScanCursor(client *c, sds cursor, sds prefix, unsigned char **last, size_t *last_len) {
    if (sdslen(cursor) == 1 && cursor[0] == '0') {
        *last = NULL;
        *last_len = 0;
        return C_OK;
    }
    if (sdslen(cursor) < 5 || (unsigned char)cursor[0] != RADIX_SCAN_CURSOR_VERSION) goto invalid;
    uint32_t prefix_len = ((uint32_t)(unsigned char)cursor[1] << 24) |
                          ((uint32_t)(unsigned char)cursor[2] << 16) |
                          ((uint32_t)(unsigned char)cursor[3] << 8) |
                          (uint32_t)(unsigned char)cursor[4];
    if ((size_t)prefix_len > sdslen(cursor) - 5 ||
        prefix_len != sdslen(prefix) ||
        memcmp(cursor + 5, prefix, prefix_len) != 0)
        goto invalid;
    *last = (unsigned char *)cursor + 5 + prefix_len;
    *last_len = sdslen(cursor) - 5 - prefix_len;
    return C_OK;

invalid:
    addReplyError(c, "invalid radix cursor");
    return C_ERR;
}

static int radixScanSeek(raxIterator *ri,
                         sds prefix,
                         unsigned char *last,
                         size_t last_len) {
    if (last)
        return raxSeek(ri, ">", last, last_len);
    if (sdslen(prefix))
        return raxSeek(ri, ">=", (unsigned char *)prefix, sdslen(prefix));
    return raxSeek(ri, "^", NULL, 0);
}

static size_t radixScanPass(radixObject *rt,
                            sds prefix,
                            unsigned char *last,
                            size_t last_len,
                            size_t count,
                            sds *new_last,
                            int reply,
                            int withvalues,
                            client *c,
                            int *has_more) {
    raxIterator ri;
    raxStart(&ri, rt->index);
    radixScanSeek(&ri, prefix, last, last_len);
    size_t returned = 0;
    while (returned < count && raxNext(&ri)) {
        if (!radixPathHasPrefix(ri.key, ri.key_len, (unsigned char *)prefix, sdslen(prefix))) break;
        if (reply) {
            if (withvalues) {
                addReplyArrayLen(c, 2);
                addReplyBulkCBuffer(c, ri.key, ri.key_len);
                radixReplyPayload(c, ri.data);
            } else {
                addReplyBulkCBuffer(c, ri.key, ri.key_len);
            }
        }
        if (new_last) {
            sdsfree(*new_last);
            *new_last = sdsnewlen(ri.key, ri.key_len);
        }
        returned++;
    }
    if (has_more) {
        *has_more = 0;
        if (returned == count && raxNext(&ri) &&
            radixPathHasPrefix(ri.key, ri.key_len, (unsigned char *)prefix, sdslen(prefix)))
            *has_more = 1;
    }
    raxStop(&ri);
    return returned;
}

void rscanCommand(client *c) {
    sds prefix = sdsempty();
    long long count = RADIX_SCAN_DEFAULT_COUNT;
    int withvalues = 0, prefix_set = 0, count_set = 0;
    for (int i = 3; i < c->argc;) {
        char *opt = objectGetVal(c->argv[i]);
        if (!strcasecmp(opt, "prefix") && !prefix_set && i + 1 < c->argc) {
            sdsfree(prefix);
            prefix = sdsdup(objectGetVal(c->argv[i + 1]));
            prefix_set = 1;
            i += 2;
        } else if (!strcasecmp(opt, "count") && !count_set && i + 1 < c->argc) {
            if (getLongLongFromObjectOrReply(c, c->argv[i + 1], &count, NULL) != C_OK) {
                sdsfree(prefix);
                return;
            }
            if (count <= 0 || (unsigned long long)count > SIZE_MAX) {
                addReplyError(c, "COUNT must be greater than 0");
                sdsfree(prefix);
                return;
            }
            count_set = 1;
            i += 2;
        } else if (!strcasecmp(opt, "withvalues") && !withvalues) {
            withvalues = 1;
            i++;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            sdsfree(prefix);
            return;
        }
    }

    sds cursor = objectGetVal(c->argv[2]);
    unsigned char *last;
    size_t last_len;
    if (radixDecodeScanCursor(c, cursor, prefix, &last, &last_len) != C_OK) {
        sdsfree(prefix);
        return;
    }

    robj *o;
    if (radixLookup(c, c->argv[1], 0, &o) != C_OK) {
        sdsfree(prefix);
        return;
    }
    if (!o) {
        addReplyArrayLen(c, 2);
        addReplyBulkCBuffer(c, "0", 1);
        addReplyArrayLen(c, 0);
        sdsfree(prefix);
        return;
    }

    sds new_last = NULL;
    int has_more;
    size_t returned = radixScanPass(radixValue(o),
                                    prefix,
                                    last,
                                    last_len,
                                    (size_t)count,
                                    &new_last,
                                    0,
                                    withvalues,
                                    c,
                                    &has_more);
    addReplyArrayLen(c, 2);
    if (has_more) {
        sds next_cursor = radixEncodeScanCursor(prefix, new_last);
        addReplyBulkCBuffer(c, next_cursor, sdslen(next_cursor));
        sdsfree(next_cursor);
    } else {
        addReplyBulkCBuffer(c, "0", 1);
    }
    addReplyArrayLen(c, returned);
    radixScanPass(radixValue(o),
                  prefix,
                  last,
                  last_len,
                  returned,
                  NULL,
                  1,
                  withvalues,
                  c,
                  NULL);
    sdsfree(new_last);
    sdsfree(prefix);
}

void rcardCommand(client *c) {
    robj *o;
    if (radixLookup(c, c->argv[1], 0, &o) != C_OK) return;
    addReplyLongLong(c, o ? radixValue(o)->num_paths : 0);
}
