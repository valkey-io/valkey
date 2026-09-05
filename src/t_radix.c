/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"

typedef enum radixReplyMode {
    RADIX_REPLY_PATH,
    RADIX_REPLY_LENGTH,
} radixReplyMode;

typedef enum radixValueMode {
    RADIX_VALUES_NONE,
    RADIX_VALUES_ALL,
    RADIX_VALUES_FIELDS,
} radixValueMode;

typedef struct radixMatch {
    size_t path_len;
    robj *payload;
} radixMatch;

typedef struct radixMatchList {
    radixMatch *items;
    size_t len;
    size_t cap;
    size_t head;
    size_t limit;
    size_t max_path_len;
} radixMatchList;

typedef struct radixScanEntry {
    sds path;
    robj *payload;
} radixScanEntry;

#define RADIX_DELETE_CHUNK_SIZE 256

static void freeRadixPayload(void *data) {
    decrRefCount(data);
}

robj *createRadixObject(void) {
    radixObject *radix = zmalloc(sizeof(*radix));
    radix->index = raxNew();
    radix->num_fields = 0;
    robj *o = createObject(OBJ_RADIX, radix);
    objectSetEncoding(o, OBJ_ENCODING_RADIX);
    return o;
}

void freeRadixObject(robj *o) {
    radixObject *radix = objectGetVal(o);
    raxFreeWithCallback(radix->index, freeRadixPayload);
    zfree(radix);
}

robj *radixTypeDup(robj *o) {
    radixObject *source = objectGetVal(o);
    robj *copy = createRadixObject();
    radixObject *target = objectGetVal(copy);
    raxIterator iter;

    raxStart(&iter, source->index);
    raxSeek(&iter, "^", NULL, 0);
    while (raxNext(&iter)) {
        robj *payload = hashTypeDup(iter.data);
        serverAssert(raxInsert(target->index, iter.key, iter.key_len, payload, NULL));
    }
    raxStop(&iter);
    target->num_fields = source->num_fields;
    return copy;
}

size_t radixTypeMemUsage(robj *o, size_t sample_size) {
    radixObject *radix = objectGetVal(o);
    size_t size = sizeof(*radix) + raxAllocSize(radix->index);
    size_t payload_size = 0;
    size_t samples = 0;
    raxIterator iter;

    raxStart(&iter, radix->index);
    raxSeek(&iter, "^", NULL, 0);
    while (samples < sample_size && raxNext(&iter)) {
        payload_size += objectComputeSize(NULL, iter.data, sample_size, -1);
        samples++;
    }
    raxStop(&iter);
    if (samples) size += (double)payload_size / samples * raxSize(radix->index);
    return size;
}

void radixTypeDigest(unsigned char *digest, robj *o) {
    radixObject *radix = objectGetVal(o);
    raxIterator paths;

    raxStart(&paths, radix->index);
    raxSeek(&paths, "^", NULL, 0);
    while (raxNext(&paths)) {
        hashTypeIterator fields;
        hashTypeInitIterator(paths.data, &fields);
        while (hashTypeNext(&fields) != C_ERR) {
            unsigned char entry_digest[20] = {0};
            sds field = hashTypeCurrentObjectNewSds(&fields, OBJ_HASH_FIELD);
            sds value = hashTypeCurrentObjectNewSds(&fields, OBJ_HASH_VALUE);
            mixDigest(entry_digest, paths.key, paths.key_len);
            mixDigest(entry_digest, field, sdslen(field));
            mixDigest(entry_digest, value, sdslen(value));
            xorDigest(digest, entry_digest, sizeof(entry_digest));
            sdsfree(field);
            sdsfree(value);
        }
        hashTypeResetIterator(&fields);
    }
    raxStop(&paths);
}

static robj *radixLookupPayload(robj *o, robj *path) {
    if (o == NULL) return NULL;
    radixObject *radix = objectGetVal(o);
    void *payload = NULL;
    sds pathstr = objectGetVal(path);
    if (!raxFind(radix->index, (unsigned char *)pathstr, sdslen(pathstr), &payload)) return NULL;
    return payload;
}

static void radixReplyPayload(client *c, robj *payload) {
    /* A payload is a field/value map, so reply like HGETALL does: a map in
     * RESP3 and a flat array in RESP2. */
    addReplyMapLen(c, hashTypeLength(payload));
    hashTypeIterator iter;
    hashTypeInitIterator(payload, &iter);
    while (hashTypeNext(&iter) != C_ERR) {
        addReplyBulkSds(c, hashTypeCurrentObjectNewSds(&iter, OBJ_HASH_FIELD));
        addReplyBulkSds(c, hashTypeCurrentObjectNewSds(&iter, OBJ_HASH_VALUE));
    }
    hashTypeResetIterator(&iter);
}

static void radixReplyField(client *c, robj *payload, robj *field) {
    robj *value = payload ? hashTypeGetValueObject(payload, objectGetVal(field)) : NULL;
    if (value) {
        addReplyBulk(c, value);
        decrRefCount(value);
    } else {
        addReplyNull(c);
    }
}

static void radixReplyFields(client *c, robj *payload, robj **fields, long numfields) {
    addReplyArrayLen(c, numfields);
    for (long i = 0; i < numfields; i++) radixReplyField(c, payload, fields[i]);
}

static void radixReplyMatch(client *c,
                            robj *query,
                            size_t path_len,
                            robj *payload,
                            radixReplyMode reply_mode,
                            radixValueMode value_mode,
                            robj **fields,
                            long numfields) {
    if (value_mode != RADIX_VALUES_NONE) addReplyArrayLen(c, 2);
    if (reply_mode == RADIX_REPLY_LENGTH)
        addReplyLongLong(c, path_len);
    else
        addReplyBulkCBuffer(c, objectGetVal(query), path_len);

    if (value_mode == RADIX_VALUES_ALL)
        radixReplyPayload(c, payload);
    else if (value_mode == RADIX_VALUES_FIELDS)
        radixReplyFields(c, payload, fields, numfields);
}

static robj *radixCreatePayload(radixObject *radix, robj *path) {
    robj *payload = createHashObject();
    sds pathstr = objectGetVal(path);
    if (!raxInsert(radix->index, (unsigned char *)pathstr, sdslen(pathstr), payload, NULL)) {
        decrRefCount(payload);
        return NULL;
    }
    return payload;
}

static void radixSetField(radixObject *radix, robj *payload, robj *field, robj *value) {
    bool expired_overwritten = false;
    int updated = hashTypeSet(payload,
                              objectGetVal(field),
                              objectGetVal(value),
                              EXPIRY_NONE,
                              HASH_SET_COPY,
                              &expired_overwritten);
    serverAssert(!expired_overwritten);
    if (!updated) radix->num_fields++;
}

/* Validate one or more groups in the following form before the caller emits
 * replies or applies mutations:
 *
 *     path FIELDS numfields field [value] [field [value] ...]
 *
 * Values are present for RAXMSET and omitted for RAXMGET. */
static int radixValidateFieldGroups(client *c, int with_values, long long *total_fields) {
    int stride = with_values ? 2 : 1;
    int argpos = 2;
    *total_fields = 0;

    while (argpos < c->argc) {
        if (argpos + 2 >= c->argc || strcasecmp(objectGetVal(c->argv[argpos + 1]), "fields")) goto syntax;

        long numfields;
        if (getRangeLongFromObjectOrReply(c, c->argv[argpos + 2], 1, LONG_MAX, &numfields, NULL) != C_OK)
            return C_ERR;

        int fields_index = argpos + 3;
        int remaining = c->argc - fields_index;
        if (numfields > remaining / stride) goto syntax;

        argpos = fields_index + numfields * stride;
        *total_fields += numfields;
    }
    return C_OK;

syntax:
    addReplyErrorObject(c, shared.syntaxerr);
    return C_ERR;
}

void raxsetCommand(client *c) {
    int fnx = 0, fxx = 0;
    int argpos = 3;
    if (argpos < c->argc && !strcasecmp(objectGetVal(c->argv[argpos]), "fnx")) {
        fnx = 1;
        argpos++;
    } else if (argpos < c->argc && !strcasecmp(objectGetVal(c->argv[argpos]), "fxx")) {
        fxx = 1;
        argpos++;
    }
    if (argpos + 2 > c->argc || strcasecmp(objectGetVal(c->argv[argpos]), "fields")) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    long numfields;
    if (getRangeLongFromObjectOrReply(c, c->argv[argpos + 1], 1, LONG_MAX, &numfields, NULL) != C_OK) return;
    int fields_index = argpos + 2;
    int remaining = c->argc - fields_index;
    if ((remaining & 1) || numfields != remaining / 2) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) return;
    robj *payload = radixLookupPayload(o, c->argv[2]);
    if (fnx || fxx) {
        for (long i = 0; i < numfields; i++) {
            int field_exists = payload && hashTypeExists(payload, objectGetVal(c->argv[fields_index + i * 2]));
            if ((fnx && field_exists) || (fxx && !field_exists)) {
                addReplyNull(c);
                return;
            }
        }
    }

    /* Build the tree before publishing it in the keyspace, so a failed path
     * insertion cannot leave subscribers with a key that never existed. */
    int created_tree = 0;
    if (o == NULL) {
        o = createRadixObject();
        created_tree = 1;
    }
    radixObject *radix = objectGetVal(o);
    if (payload == NULL) {
        payload = radixCreatePayload(radix, c->argv[2]);
        if (payload == NULL) {
            if (created_tree) decrRefCount(o);
            addReplyError(c, "failed to allocate radix path");
            return;
        }
    }
    for (long i = 0; i < numfields; i++)
        radixSetField(radix, payload, c->argv[fields_index + i * 2], c->argv[fields_index + i * 2 + 1]);

    /* Publish the key only now that it carries the field: dbAdd() fires the
     * "new" keyspace event and module subscribers run synchronously. */
    if (created_tree) dbAdd(c->db, c->argv[1], &o);

    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_RADIX, "raxset", c->argv[1], c->db->id);
    server.dirty += numfields;
    addReply(c, shared.ok);
}

void raxmsetCommand(client *c) {
    long long assignments;
    if (radixValidateFieldGroups(c, 1, &assignments) != C_OK) return;

    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) return;
    int created_tree = 0;
    if (o == NULL) {
        o = createRadixObject();
        created_tree = 1;
    }
    radixObject *radix = objectGetVal(o);
    for (int argpos = 2; argpos < c->argc;) {
        long long numfields;
        serverAssert(getLongLongFromObject(c->argv[argpos + 2], &numfields) == C_OK);
        int fields_index = argpos + 3;
        robj *payload = radixLookupPayload(o, c->argv[argpos]);
        if (payload == NULL) {
            payload = radixCreatePayload(radix, c->argv[argpos]);
            if (payload == NULL) {
                if (created_tree) decrRefCount(o);
                addReplyError(c, "failed to allocate radix path");
                return;
            }
        }
        for (long long i = 0; i < numfields; i++)
            radixSetField(radix, payload, c->argv[fields_index + i * 2], c->argv[fields_index + i * 2 + 1]);
        argpos = fields_index + numfields * 2;
    }
    if (created_tree) dbAdd(c->db, c->argv[1], &o);
    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_RADIX, "raxmset", c->argv[1], c->db->id);
    server.dirty += assignments;
    addReply(c, shared.ok);
}

void raxgetCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) return;
    robj *payload = radixLookupPayload(o, c->argv[2]);
    if (c->argc == 4)
        radixReplyField(c, payload, c->argv[3]);
    else
        radixReplyFields(c, payload, c->argv + 3, c->argc - 3);
}

void raxmgetCommand(client *c) {
    long long total_fields;
    if (radixValidateFieldGroups(c, 0, &total_fields) != C_OK) return;

    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) return;
    addReplyArrayLen(c, total_fields);
    for (int argpos = 2; argpos < c->argc;) {
        long long numfields;
        serverAssert(getLongLongFromObject(c->argv[argpos + 2], &numfields) == C_OK);
        int fields_index = argpos + 3;
        robj *payload = radixLookupPayload(o, c->argv[argpos]);
        for (long long i = 0; i < numfields; i++) radixReplyField(c, payload, c->argv[fields_index + i]);
        argpos = fields_index + numfields;
    }
}

void raxgetallCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) return;
    robj *payload = radixLookupPayload(o, c->argv[2]);
    if (payload)
        radixReplyPayload(c, payload);
    else
        addReply(c, shared.emptymap[c->resp]);
}

void raxexistsCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) return;
    addReplyLongLong(c, radixLookupPayload(o, c->argv[2]) != NULL);
}

void raxdelCommand(client *c) {
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) return;
    if (o == NULL) {
        addReply(c, shared.czero);
        return;
    }
    radixObject *radix = objectGetVal(o);
    robj *payload = radixLookupPayload(o, c->argv[2]);
    if (payload == NULL) {
        addReply(c, shared.czero);
        return;
    }

    long long deleted = 0;
    if (c->argc == 3) {
        deleted = 1;
        radix->num_fields -= hashTypeLength(payload);
    } else {
        for (int i = 3; i < c->argc; i++) {
            if (hashTypeDelete(payload, objectGetVal(c->argv[i]))) {
                deleted++;
                radix->num_fields--;
            }
        }
    }
    if (c->argc == 3 || hashTypeLength(payload) == 0) {
        sds path = objectGetVal(c->argv[2]);
        void *removed = NULL;
        serverAssert(raxRemove(radix->index, (unsigned char *)path, sdslen(path), &removed));
        decrRefCount(removed);
    }
    if (deleted) {
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_RADIX, "raxdel", c->argv[1], c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c, deleted);
}

static int radixParseMatchOptions(client *c,
                                  int start,
                                  int plural_length,
                                  int allow_limits,
                                  radixReplyMode *reply_mode,
                                  radixValueMode *value_mode,
                                  robj ***fields,
                                  long *numfields,
                                  long long *count,
                                  long long *maxlen) {
    *reply_mode = RADIX_REPLY_PATH;
    *value_mode = RADIX_VALUES_NONE;
    *fields = NULL;
    *numfields = 0;
    *count = -1;
    *maxlen = -1;
    for (int i = start; i < c->argc;) {
        char *arg = objectGetVal(c->argv[i]);
        if ((!plural_length && !strcasecmp(arg, "length")) ||
            (plural_length && !strcasecmp(arg, "lengths"))) {
            if (*reply_mode == RADIX_REPLY_LENGTH) goto syntax;
            *reply_mode = RADIX_REPLY_LENGTH;
            i++;
        } else if (!strcasecmp(arg, "withvalues")) {
            if (*value_mode != RADIX_VALUES_NONE) goto syntax;
            *value_mode = RADIX_VALUES_ALL;
            i++;
        } else if (!strcasecmp(arg, "fields")) {
            if (*value_mode != RADIX_VALUES_NONE || i + 1 >= c->argc) goto syntax;
            long field_count;
            if (getRangeLongFromObjectOrReply(c, c->argv[i + 1], 1, LONG_MAX, &field_count, NULL) != C_OK)
                return C_ERR;
            if (field_count > c->argc - i - 2) goto syntax;
            *value_mode = RADIX_VALUES_FIELDS;
            *numfields = field_count;
            *fields = c->argv + i + 2;
            i += 2 + field_count;
        } else if (allow_limits && !strcasecmp(arg, "count")) {
            if (*count != -1 || i + 1 >= c->argc) goto syntax;
            if (getLongLongFromObjectOrReply(c, c->argv[i + 1], count, NULL) != C_OK) return C_ERR;
            if (*count <= 0) {
                addReplyError(c, "COUNT must be greater than zero");
                return C_ERR;
            }
            i += 2;
        } else if (allow_limits && !strcasecmp(arg, "maxlen")) {
            if (*maxlen != -1 || i + 1 >= c->argc) goto syntax;
            if (getLongLongFromObjectOrReply(c, c->argv[i + 1], maxlen, NULL) != C_OK) return C_ERR;
            if (*maxlen < 0) {
                addReplyError(c, "MAXLEN must be non-negative");
                return C_ERR;
            }
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

void raxlongestCommand(client *c) {
    radixReplyMode reply_mode;
    radixValueMode value_mode;
    robj **fields;
    long numfields;
    long long count, maxlen;
    if (radixParseMatchOptions(c, 3, 0, 0, &reply_mode, &value_mode, &fields, &numfields, &count, &maxlen) !=
        C_OK)
        return;

    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) return;
    if (o == NULL) {
        addReplyNull(c);
        return;
    }
    radixObject *radix = objectGetVal(o);
    sds query = objectGetVal(c->argv[2]);
    size_t matched_len;
    void *payload;
    if (!raxFindLongestPrefix(radix->index,
                              (unsigned char *)query,
                              sdslen(query),
                              &matched_len,
                              &payload)) {
        addReplyNull(c);
        return;
    }
    radixReplyMatch(c, c->argv[2], matched_len, payload, reply_mode, value_mode, fields, numfields);
}

static int radixCollectMatch(size_t path_len, void *data, void *context) {
    radixMatchList *matches = context;
    if (path_len > matches->max_path_len) return 0;
    if (matches->len == matches->cap) {
        if (matches->cap < matches->limit) {
            size_t new_cap = matches->cap ? matches->cap * 2 : 8;
            if (new_cap > matches->limit) new_cap = matches->limit;
            matches->cap = new_cap;
            matches->items = zrealloc(matches->items, matches->cap * sizeof(*matches->items));
        } else {
            /* COUNT retains the deepest matches in this circular buffer. */
            matches->items[matches->head] = (radixMatch){path_len, data};
            matches->head = (matches->head + 1) % matches->cap;
            return 1;
        }
    }
    size_t index = (matches->head + matches->len) % matches->cap;
    matches->items[index] = (radixMatch){path_len, data};
    matches->len++;
    return 1;
}

void raxprefixesCommand(client *c) {
    radixReplyMode reply_mode;
    radixValueMode value_mode;
    robj **fields;
    long numfields;
    long long count, maxlen;
    if (radixParseMatchOptions(c, 3, 1, 1, &reply_mode, &value_mode, &fields, &numfields, &count, &maxlen) !=
        C_OK)
        return;

    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) return;
    if (o == NULL) {
        addReply(c, shared.emptyarray);
        return;
    }
    radixObject *radix = objectGetVal(o);
    sds query = objectGetVal(c->argv[2]);
    radixMatchList matches = {
        .items = NULL,
        .len = 0,
        .cap = 0,
        .head = 0,
        .limit = count == -1 ? SIZE_MAX : (size_t)count,
        .max_path_len = maxlen == -1 ? SIZE_MAX : (size_t)maxlen,
    };
    raxForEachPrefix(radix->index, (unsigned char *)query, sdslen(query), radixCollectMatch, &matches);
    addReplyArrayLen(c, matches.len);
    for (size_t i = 0; i < matches.len; i++) {
        size_t index = (matches.head + i) % matches.cap;
        radixReplyMatch(c,
                        c->argv[2],
                        matches.items[index].path_len,
                        matches.items[index].payload,
                        reply_mode,
                        value_mode,
                        fields,
                        numfields);
    }
    zfree(matches.items);
}

static int radixPathHasPrefix(const unsigned char *path,
                              size_t path_len,
                              const unsigned char *prefix,
                              size_t prefix_len) {
    return path_len >= prefix_len && memcmp(path, prefix, prefix_len) == 0;
}

void raxdelprefixCommand(client *c) {
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) return;
    if (o == NULL) {
        addReply(c, shared.czero);
        return;
    }
    radixObject *radix = objectGetVal(o);
    sds prefix = objectGetVal(c->argv[2]);
    size_t prefix_len = sdslen(prefix);
    if (prefix_len == 0) {
        long long deleted = raxSize(radix->index);
        if (deleted) {
            rax *empty = raxNew();
            if (empty == NULL) {
                addReplyError(c, "failed to allocate empty radix tree");
                return;
            }
            raxFreeWithCallback(radix->index, freeRadixPayload);
            radix->index = empty;
            radix->num_fields = 0;
            signalModifiedKey(c, c->db, c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_RADIX, "raxdelprefix", c->argv[1], c->db->id);
            server.dirty += deleted;
        }
        addReplyLongLong(c, deleted);
        return;
    }

    long long deleted = 0;
    while (1) {
        sds paths[RADIX_DELETE_CHUNK_SIZE];
        size_t chunk_len = 0;
        raxIterator iter;
        raxStart(&iter, radix->index);
        raxSeek(&iter, ">=", (unsigned char *)prefix, prefix_len);
        while (chunk_len < RADIX_DELETE_CHUNK_SIZE && raxNext(&iter) &&
               radixPathHasPrefix(iter.key, iter.key_len, (unsigned char *)prefix, prefix_len)) {
            paths[chunk_len++] = sdsnewlen(iter.key, iter.key_len);
        }
        raxStop(&iter);
        if (chunk_len == 0) break;

        for (size_t i = 0; i < chunk_len; i++) {
            void *payload = NULL;
            serverAssert(raxRemove(radix->index, (unsigned char *)paths[i], sdslen(paths[i]), &payload));
            radix->num_fields -= hashTypeLength(payload);
            decrRefCount(payload);
            sdsfree(paths[i]);
        }
        deleted += chunk_len;
    }
    if (deleted) {
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_RADIX, "raxdelprefix", c->argv[1], c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c, deleted);
}

static int radixHexDigit(unsigned char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static sds radixDecodeCursor(client *c, robj *cursor, int *initial) {
    sds encoded = objectGetVal(cursor);
    size_t len = sdslen(encoded);
    if (len == 1 && encoded[0] == '0') {
        *initial = 1;
        return sdsempty();
    }
    *initial = 0;
    if (len < 2 || encoded[0] != '1' || encoded[1] != ':' || ((len - 2) & 1)) goto invalid;
    sds decoded = sdsnewlen(NULL, (len - 2) / 2);
    for (size_t i = 2; i < len; i += 2) {
        int high = radixHexDigit(encoded[i]);
        int low = radixHexDigit(encoded[i + 1]);
        if (high < 0 || low < 0) {
            sdsfree(decoded);
            goto invalid;
        }
        decoded[(i - 2) / 2] = (high << 4) | low;
    }
    return decoded;

invalid:
    addReplyError(c, "invalid cursor");
    return NULL;
}

static sds radixEncodeCursor(const unsigned char *path, size_t len) {
    static const char hex[] = "0123456789abcdef";
    sds cursor = sdsnewlen(NULL, 2 + len * 2);
    cursor[0] = '1';
    cursor[1] = ':';
    for (size_t i = 0; i < len; i++) {
        cursor[2 + i * 2] = hex[path[i] >> 4];
        cursor[3 + i * 2] = hex[path[i] & 0xf];
    }
    return cursor;
}

void raxscanCommand(client *c) {
    int withvalues = 0;
    int count_seen = 0;
    long count = 10;
    robj *prefix_arg = NULL;
    for (int i = 3; i < c->argc;) {
        char *arg = objectGetVal(c->argv[i]);
        if (!strcasecmp(arg, "prefix") && i + 1 < c->argc && prefix_arg == NULL) {
            prefix_arg = c->argv[i + 1];
            i += 2;
        } else if (!strcasecmp(arg, "count") && i + 1 < c->argc && !count_seen) {
            if (getRangeLongFromObjectOrReply(c, c->argv[i + 1], 1, LONG_MAX, &count, NULL) != C_OK) return;
            count_seen = 1;
            i += 2;
        } else if (!strcasecmp(arg, "withvalues") && !withvalues) {
            withvalues = 1;
            i++;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    int initial;
    sds previous = radixDecodeCursor(c, c->argv[2], &initial);
    if (previous == NULL) return;
    sds prefix = prefix_arg ? objectGetVal(prefix_arg) : NULL;
    size_t prefix_len = prefix ? sdslen(prefix) : 0;
    if (!initial && prefix &&
        !radixPathHasPrefix((unsigned char *)previous, sdslen(previous), (unsigned char *)prefix, prefix_len)) {
        sdsfree(previous);
        addReplyError(c, "cursor does not belong to the requested prefix");
        return;
    }

    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) {
        sdsfree(previous);
        return;
    }
    if (o == NULL) {
        sdsfree(previous);
        addReplyArrayLen(c, 2);
        addReplyBulkCBuffer(c, "0", 1);
        addReply(c, shared.emptyarray);
        return;
    }

    radixObject *radix = objectGetVal(o);
    raxIterator iter;
    raxStart(&iter, radix->index);
    if (!initial)
        raxSeek(&iter, ">", (unsigned char *)previous, sdslen(previous));
    else if (prefix)
        raxSeek(&iter, ">=", (unsigned char *)prefix, prefix_len);
    else
        raxSeek(&iter, "^", NULL, 0);

    radixScanEntry *entries = NULL;
    size_t returned = 0, cap = 0;
    while (returned < (size_t)count && raxNext(&iter)) {
        if (prefix && !radixPathHasPrefix(iter.key, iter.key_len, (unsigned char *)prefix, prefix_len)) break;
        if (returned == cap) {
            cap = cap ? cap * 2 : 16;
            if (cap > (size_t)count) cap = count;
            entries = zrealloc(entries, cap * sizeof(*entries));
        }
        entries[returned].path = sdsnewlen(iter.key, iter.key_len);
        entries[returned].payload = iter.data;
        returned++;
    }
    /* Stopping exactly at COUNT does not mean there is more to scan, so look
     * one entry ahead instead of making the client discover the end of the
     * traversal with an extra empty call. */
    int has_more = 0;
    if (returned == (size_t)count && raxNext(&iter))
        has_more = !prefix || radixPathHasPrefix(iter.key, iter.key_len, (unsigned char *)prefix, prefix_len);
    sds next_cursor = has_more ? radixEncodeCursor((unsigned char *)entries[returned - 1].path,
                                                   sdslen(entries[returned - 1].path))
                               : sdsnew("0");
    raxStop(&iter);
    sdsfree(previous);

    addReplyArrayLen(c, 2);
    addReplyBulkSds(c, next_cursor);
    addReplyArrayLen(c, returned);
    for (size_t i = 0; i < returned; i++) {
        if (withvalues) addReplyArrayLen(c, 2);
        addReplyBulkCBuffer(c, entries[i].path, sdslen(entries[i].path));
        if (withvalues) radixReplyPayload(c, entries[i].payload);
        sdsfree(entries[i].path);
    }
    zfree(entries);
}

void raxcardCommand(client *c) {
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_RADIX)) return;
    if (o == NULL)
        addReply(c, shared.czero);
    else
        addReplyLongLong(c, raxSize(((radixObject *)objectGetVal(o))->index));
}

int rewriteRadixObject(rio *r, robj *key, robj *o) {
    radixObject *radix = objectGetVal(o);
    raxIterator paths;

    if (raxSize(radix->index) == 0) {
        /* As with XADD MAXLEN 0 for an empty stream, create a temporary
         * logical path and remove it again to reconstruct an empty Radix. */
        if (!rioWriteBulkCount(r, '*', 7) || !rioWriteBulkString(r, "RAXSET", 6) ||
            !rioWriteBulkObject(r, key) || !rioWriteBulkString(r, "", 0) ||
            !rioWriteBulkString(r, "FIELDS", 6) || !rioWriteBulkLongLong(r, 1) ||
            !rioWriteBulkString(r, "", 0) || !rioWriteBulkString(r, "", 0) ||
            !rioWriteBulkCount(r, '*', 3) || !rioWriteBulkString(r, "RAXDEL", 6) ||
            !rioWriteBulkObject(r, key) || !rioWriteBulkString(r, "", 0))
            return 0;
        return 1;
    }

    raxStart(&paths, radix->index);
    raxSeek(&paths, "^", NULL, 0);
    while (raxNext(&paths)) {
        hashTypeIterator fields;
        hashTypeInitIterator(paths.data, &fields);
        while (hashTypeNext(&fields) != C_ERR) {
            sds field = hashTypeCurrentObjectNewSds(&fields, OBJ_HASH_FIELD);
            sds value = hashTypeCurrentObjectNewSds(&fields, OBJ_HASH_VALUE);
            int ok = rioWriteBulkCount(r, '*', 7) && rioWriteBulkString(r, "RAXSET", 6) &&
                     rioWriteBulkObject(r, key) && rioWriteBulkString(r, (char *)paths.key, paths.key_len) &&
                     rioWriteBulkString(r, "FIELDS", 6) && rioWriteBulkLongLong(r, 1) &&
                     rioWriteBulkString(r, field, sdslen(field)) && rioWriteBulkString(r, value, sdslen(value));
            sdsfree(field);
            sdsfree(value);
            if (!ok) {
                hashTypeResetIterator(&fields);
                raxStop(&paths);
                return 0;
            }
        }
        hashTypeResetIterator(&fields);
    }
    raxStop(&paths);
    return 1;
}
