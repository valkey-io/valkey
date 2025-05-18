/*
 * Copyright (c) 2009-2012, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hashtable.h"
#include "rax.h"
#include "sds.h"
#include "volatile_set.h"
#include "server.h"
#include "zmalloc.h"
#include <math.h>
#include <string.h>


sds hashTypeEntryGetField(const hashTypeEntry *entry);
static inline void *hashTypeEntryGetMetadata(const hashTypeEntry *entry);
static inline size_t hashTypeEntryGetMetadataSize(const hashTypeEntry *entry);
static inline hashTypeEntry *hashTypeEntrySetExpiry(hashTypeEntry *entry, long long expiry);
int hashTypeExpireEntry(hashTypeEntry *entry);
int hashTypeExpireRemoveEntry(void *entry);

volatileEntryType hashvolatileEntryType = {
    .entryGetKey = (sds(*)(const void *entry))hashTypeEntryGetField,
    .getExpiry = (long long (*)(const void *entry))hashTypeEntryGetExpiry,
    .expire = hashTypeExpireRemoveEntry,
};


#include <stdbool.h>

/*-----------------------------------------------------------------------------
 * Hash Entry API
 *----------------------------------------------------------------------------*/

/* The hashTypeEntry pointer is the field sds. We encode the entry layout type
 * in the field SDS header. Field type SDS_TYPE_5 doesn't have any spare bits to
 * encode this so we use it only for the first layout type.
 *
 * Entry with embedded value, used for small sizes. The value is stored as
 * SDS_TYPE_8. The field can use any SDS type.
 *
 *     +--------------+---------------+
 *     | field        | value         |
 *     | hdr "foo" \0 | hdr8 "bar" \0 |
 *     +------^-------+---------------+
 *            |
 *            |
 *          entry pointer = field sds
 *
 * Entry with value pointer, used for larger fields and values. The field is SDS
 * type 8 or higher.
 *
 *     +-------+--------------+
 *     | value | field        |
 *     | ptr   | hdr "foo" \0 |
 *     +-------+------^-------+
 *                    |
 *                    |
 *                 entry pointer = field sds
 */

/* The maximum allocation size we want to use for entries with embedded
 * values. */
#define EMBED_VALUE_MAX_ALLOC_SIZE 128

/* SDS aux flag. If set, it indicates that the entry has an embedded value
 * pointer located in memory before the embedded field. If unset, the entry
 * instead has an embedded value located after the embedded field. */
#define FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR 0

/* SDS aux flag. If set, it indicates that the entry has TTL metadata set. */
#define FIELD_SDS_AUX_BIT_ENTRY_HAS_TTL 1

/* SDS aux flag. If set, it indicates that the entry has TTL metadata set. */
#define FIELD_SDS_AUX_BIT_ENTRY_HAS_METADATA 2

static inline bool entryHasValuePtr(const hashTypeEntry *entry) {
    return sdsGetAuxBit(entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR);
}

static inline bool entryHasExpiry(const hashTypeEntry *entry) {
    return sdsGetAuxBit(entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_TTL);
}

static inline bool entryHasMetadata(const hashTypeEntry *entry) {
    return sdsGetAuxBit(entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_METADATA);
}

/* Returns the location of a pointer to a separately allocated value. Only for
 * an entry without an embedded value. */
static sds *hashTypeEntryGetValueRef(const hashTypeEntry *entry) {
    serverAssert(entryHasValuePtr(entry));
    char *field_data = sdsAllocPtr(entry);
    field_data -= sizeof(sds *);
    return (sds *)field_data;
}

/* takes ownership of value, does not take ownership of field */
hashTypeEntry *hashTypeCreateEntry(sds field, sds value, long long expiry, void *metadata, size_t metadata_size) {
    serverAssert(metadata_size <= UCHAR_MAX);
    sds embedded_field_sds;
    size_t expiry_size = (expiry == EXPIRY_NONE) ? 0 : sizeof(long long);
    size_t metadata_header_size = metadata_size ? sizeof(char) : 0;
    size_t field_len = sdslen(field);
    int field_sds_type = sdsReqType(field_len);
    if (field_sds_type == SDS_TYPE_5 && (expiry_size > 0 || metadata_size > 0)) {
        field_sds_type = SDS_TYPE_8;
    }
    size_t field_size = sdsReqSize(field_len, field_sds_type);
    size_t value_len = sdslen(value);
    size_t value_size = sdsReqSize(value_len, SDS_TYPE_8);
    size_t alloc_size = field_size + metadata_size + metadata_header_size + expiry_size;
    bool embed_value = false;
    if (alloc_size + value_size <= EMBED_VALUE_MAX_ALLOC_SIZE) {
        /* Embed field and value. Value is fixed to SDS_TYPE_8. Unused
         * allocation space is recorded in the embedded value's SDS header.
         *
         *     +----------+----------+------+--------------+---------------+
         *     | metadata | metadata | TTL  | field        | value         |
         *     |          |   size   |      | hdr "foo" \0 | hdr8 "bar" \0 |
         *     +----------+----------+------+--------------+---------------+
         */
        embed_value = true;
        alloc_size += value_size;
    } else {
        /* Embed field, but not value. Field must be >= SDS_TYPE_8 to encode to
         * indicate this type of entry.
         *
         *     +----------+----------+------+-------+---------------+
         *     | metadata | metadata | TTL  | value | field         |
         *     |          |   size   |      | ptr   | hdr8 "foo" \0 |
         *     +----------+----------+------+-------+---------------+
         */
        embed_value = false;
        alloc_size += sizeof(sds *);
        if (field_sds_type == SDS_TYPE_5) {
            field_sds_type = SDS_TYPE_8;
            alloc_size -= field_size;
            field_size = sdsReqSize(field_len, field_sds_type);
            alloc_size += field_size;
        }
    }

    /* allocate the buffer */
    size_t buf_size;
    char *buf = zmalloc_usable(alloc_size, &buf_size);

    /* Copy the metadata if exists */
    if (metadata_size) {
        memcpy(buf, metadata, metadata_size);
        buf += metadata_size;
        *buf = (char)(metadata_size & 0xFF);
        buf += sizeof(char);
        buf_size -= sizeof(char);
    }

    /* Set The expiry if exists */
    if (expiry_size) {
        memcpy(buf, &expiry, expiry_size);
        buf += expiry_size;
        buf_size -= expiry_size;
    }

    if (!embed_value) {
        *(sds *)buf = value;
        buf += sizeof(sds *);
        buf_size -= sizeof(sds *);
    } else {
        sdswrite(buf + field_size, buf_size - field_size, SDS_TYPE_8, value, value_len);
        sdsfree(value);
        buf_size -= value_size;
    }
    /* Set the field data */
    embedded_field_sds = sdswrite(buf, field_size, field_sds_type, field, field_len);

    /* Field sds aux bits are zero, which we use for this entry encoding. */
    sdsSetAuxBit(embedded_field_sds, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR, embed_value ? 0 : 1);
    sdsSetAuxBit(embedded_field_sds, FIELD_SDS_AUX_BIT_ENTRY_HAS_TTL, expiry_size > 0 ? 1 : 0);
    sdsSetAuxBit(embedded_field_sds, FIELD_SDS_AUX_BIT_ENTRY_HAS_METADATA, metadata_size > 0 ? 1 : 0);
    serverAssert(sdsGetAuxBit(embedded_field_sds, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR) == (embed_value ? 0 : 1));
    serverAssert(sdsGetAuxBit(embedded_field_sds, FIELD_SDS_AUX_BIT_ENTRY_HAS_TTL) == (expiry != EXPIRY_NONE));
    serverAssert(sdsGetAuxBit(embedded_field_sds, FIELD_SDS_AUX_BIT_ENTRY_HAS_METADATA) == (metadata_size > 0 ? 1 : 0));

    return (void *)embedded_field_sds;
}

/* The entry pointer is the field sds, but that's an implementation detail. */
sds hashTypeEntryGetField(const hashTypeEntry *entry) {
    return (sds)entry;
}

sds hashTypeEntryGetValue(const hashTypeEntry *entry) {
    if (entryHasValuePtr(entry)) {
        return *hashTypeEntryGetValueRef(entry);
    } else {
        /* Skip field content, field null terminator and value sds8 hdr. */
        size_t offset = sdslen(entry) + 1 + sdsHdrSize(SDS_TYPE_8);
        serverAssert((char *)entry + offset);

        return (char *)entry + offset;
    }
}

void hashTypeEntrySetValue(const hashTypeEntry *entry, sds value) {
    if (entryHasValuePtr(entry)) {
        sds *value_ref = hashTypeEntryGetValueRef(entry);
        sdsfree(*value_ref);
        *value_ref = value;
    } else {
        /* Skip field content, field null terminator and value sds8 hdr. */
        sds old_value = hashTypeEntryGetValue(entry);
        sdswrite(sdsAllocPtr(old_value), sdsAllocSize(old_value), SDS_TYPE_8, value, sdslen(value));
        sdsfree(value);
    }
}

/* Returns the address of the entry allocation. */
static void *hashTypeEntryAllocPtr(const hashTypeEntry *entry) {
    char *buf = sdsAllocPtr(entry);
    if (entryHasValuePtr(entry)) buf -= sizeof(sds *);
    if (entryHasExpiry(entry)) buf -= sizeof(long long);
    if (entryHasMetadata(entry)) buf -= (hashTypeEntryGetMetadataSize(entry) + sizeof(char));
    return buf;
}

/* Frees previous value, takes ownership of new value, returns entry (may be
 * reallocated). */
static hashTypeEntry *hashTypeEntryUpdate(hashTypeEntry *entry, sds value, long long expiry) {
    sds field = (sds)entry;
    bool update_value = value ? true : false;
    long long ttl = hashTypeEntryGetExpiry(entry);
    bool update_expiry = (expiry != ttl) ? true : false;
    if (update_expiry) ttl = expiry;
    value = update_value ? value : hashTypeEntryGetValue(entry);

    size_t field_size = sdsHdrSize(sdsType(field)) + sdsalloc(field) + 1;
    size_t value_len = sdslen(value);
    size_t value_size = sdsReqSize(value_len, SDS_TYPE_8);
    size_t expiry_size = ttl != EXPIRY_NONE ? sizeof(ttl) : 0;
    size_t metadata_size = entryHasMetadata(entry) ? hashTypeEntryGetMetadataSize(entry) + sizeof(char) : 0;
    size_t required_size = field_size + value_size + expiry_size + metadata_size;
    size_t current_allocation_size = hashTypeEntryMemUsage(entry);
    bool create_new_entry = (update_expiry && (hashTypeEntryGetExpiry(entry) == EXPIRY_NONE || ttl == EXPIRY_NONE)) ||
                            !(update_value && !entryHasValuePtr(entry) &&
                              required_size <= EMBED_VALUE_MAX_ALLOC_SIZE &&
                              required_size <= current_allocation_size &&
                              required_size >= current_allocation_size * 3 / 4);

    if (!create_new_entry) {
        /* Reuse the allocation if the new value fits and leaves no more than
         * 25% unused space after replacing the value. */
        if (update_expiry)
            hashTypeEntrySetExpiry(entry, ttl);
        if (update_value) {
            hashTypeEntrySetValue(entry, value);
        }
        serverAssert(sdsGetAuxBit(entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_TTL) == (ttl == EXPIRY_NONE ? 0 : 1));
        return entry;

    } else {
        hashTypeEntry *new_entry = hashTypeCreateEntry(hashTypeEntryGetField(entry), value, ttl, hashTypeEntryGetMetadata(entry), metadata_size);
        freeHashTypeEntry(entry);
        return new_entry;
    }
}

/* Returns memory usage of a hashTypeEntry, including all allocations owned by
 * the hashTypeEntry. */
size_t hashTypeEntryMemUsage(hashTypeEntry *entry) {
    size_t mem = 0;

    if (entryHasValuePtr(entry)) {
        /* In case the value is not embedded we might not be able to sum all the allocation sizes since the field
         * header could be too small for holding the real allocation size. */
        mem += zmalloc_usable_size(hashTypeEntryAllocPtr(entry));
    } else {
        mem += sdsReqSize(sdslen(entry), sdsType(entry));
        if (entryHasExpiry(entry)) mem += sizeof(long long);
        if (entryHasMetadata(entry)) mem += (hashTypeEntryGetMetadataSize(entry) + sizeof(char));
    }
    mem += sdsAllocSize(hashTypeEntryGetValue(entry));
    return mem;
}

/**************************************** Entry Expiry API *****************************************/
static inline void *hashTypeEntryGetMetadata(const hashTypeEntry *entry) {
    if (entryHasExpiry(entry))
        return hashTypeEntryAllocPtr(entry);
    return NULL;
}

static inline size_t hashTypeEntryGetMetadataSize(const hashTypeEntry *entry) {
    size_t medadata_size = 0;
    if (!entryHasMetadata(entry)) return medadata_size;

    char *buf = sdsAllocPtr(entry);
    if (entryHasValuePtr(entry)) buf -= sizeof(sds *);
    if (entryHasExpiry(entry)) buf -= sizeof(long long);
    buf -= sizeof(char);
    return (size_t)(*buf);
}

long long hashTypeEntryGetExpiry(const hashTypeEntry *entry) {
    long long expiry = EXPIRY_NONE;
    if (entryHasExpiry(entry)) {
        char *buf = sdsAllocPtr(entry);
        if (entryHasValuePtr(entry)) buf -= sizeof(sds *);
        buf -= sizeof(expiry);
        memcpy(&expiry, buf, sizeof(expiry));
    }
    return expiry;
}

int hashTypeEntryHasExpire(const hashTypeEntry *entry) {
    return entryHasExpiry(entry);
}

static inline hashTypeEntry *hashTypeEntrySetExpiry(hashTypeEntry *entry, long long expiry) {
    if (entryHasExpiry(entry)) {
        char *buf = sdsAllocPtr(entry);
        if (entryHasValuePtr(entry)) buf -= sizeof(sds *);
        buf -= sizeof(expiry);
        memcpy(buf, &expiry, sizeof(expiry));
        return entry;
    }
    hashTypeEntry *new_entry = hashTypeCreateEntry(hashTypeEntryGetField(entry),
                                                   sdsdup(hashTypeEntryGetValue(entry)),
                                                   expiry,
                                                   hashTypeEntryGetMetadata(entry),
                                                   hashTypeEntryGetMetadataSize(entry));
    freeHashTypeEntry(entry);
    return new_entry;
}

static int hashTypeEntryIsExpired(hashTypeEntry *entry) {
    /* Don't expire anything while loading. It will be done later. */
    if (server.loading) return 0;
    if (server.lazy_expire_disabled) return 0;
    if (!timestampIsExpired(hashTypeEntryGetExpiry(entry))) return 0;
    if (server.primary_host == NULL && server.import_mode) {
        if (server.current_client && server.current_client->flag.import_source) return 0;
    }
    return 1;
}
/**************************************** Entry Expiry API - End *****************************************/

/* Defragments a hashtable entry (field-value pair) if needed, using the
 * provided defrag functions. The defrag functions return NULL if the allocation
 * was not moved, otherwise they return a pointer to the new memory location.
 * A separate sds defrag function is needed because of the unique memory layout
 * of sds strings.
 * If the location of the hashTypeEntry changed we return the new location,
 * otherwise we return NULL. */
hashTypeEntry *hashTypeEntryDefrag(hashTypeEntry *entry, void *(*defragfn)(void *), sds (*sdsdefragfn)(sds)) {
    if (entryHasValuePtr(entry)) {
        sds *value_ref = hashTypeEntryGetValueRef(entry);
        sds new_value = sdsdefragfn(*value_ref);
        if (new_value) *value_ref = new_value;
    }
    char *allocation = hashTypeEntryAllocPtr(entry);
    char *new_allocation = defragfn(allocation);
    if (new_allocation != NULL) {
        /* Return the same offset into the new allocation as the entry's offset
         * in the old allocation. */
        return new_allocation + ((char *)entry - allocation);
    }
    return NULL;
}

/* Used for releasing memory to OS to avoid unnecessary CoW. Called when we've
 * forked and memory won't be used again. See zmadvise_dontneed() */
void dismissHashTypeEntry(hashTypeEntry *entry) {
    /* Only dismiss values memory since the field size usually is small. */
    if (entryHasValuePtr(entry)) {
        dismissSds(*hashTypeEntryGetValueRef(entry));
    }
}

void freeHashTypeEntry(hashTypeEntry *entry) {
    if (entryHasValuePtr(entry)) {
        sdsfree(*hashTypeEntryGetValueRef(entry));
    }
    zfree(hashTypeEntryAllocPtr(entry));
}

/*-----------------------------------------------------------------------------
 * Hash type Expiry API
 *----------------------------------------------------------------------------*/

static volatile_set *hashTypeGetVolatileSet(robj *o) {
    serverAssert(o->encoding == OBJ_ENCODING_HASHTABLE);
    return *(volatile_set **)hashtableMetadata(o->ptr);
}

int hashTypeHasVolatileElements(robj *o) {
    return o->encoding == OBJ_ENCODING_HASHTABLE && hashTypeGetVolatileSet(o);
}

size_t hashTypeNumVolatileElements(robj *o) {
    if (hashTypeHasVolatileElements(o)) {
        return volatileSetNumEntries(hashTypeGetVolatileSet(o));
    }
    return 0;
}

static volatile_set *
hashTypeGetOrcreateVolatileSet(robj *o) {
    serverAssert(o->encoding == OBJ_ENCODING_HASHTABLE);
    volatile_set **volatile_set_ref = hashtableMetadata(o->ptr);
    if (*volatile_set_ref == NULL)
        *volatile_set_ref = createVolatileSet(&hashvolatileEntryType);
    return *volatile_set_ref;
}

void hashTypeTrackEntry(robj *o, void *entry) {
    volatile_set *set = hashTypeGetOrcreateVolatileSet(o);
    serverAssert(volatileSetAddEntry(set, entry, hashTypeEntryGetExpiry(entry)));
}

static void hashTypeUntrackEntry(robj *o, void *entry) {
    if (!hashTypeEntryHasExpire(entry)) return;
    volatile_set *set = hashTypeGetVolatileSet(o);
    debugServerAssert(set);
    serverAssert(volatileSetRemoveEntry(set, entry, hashTypeEntryGetExpiry(entry)));
    if (volatileSetNumEntries(set) == 0) {
        freeVolatileSet(set);
        volatile_set **volatile_set_ref = hashtableMetadata(o->ptr);
        *volatile_set_ref = NULL;
    }
}

static void hashTypeTrackUpdateEntry(robj *o, void *old_entry, void *new_entry, long long old_expiry, long long new_expiry) {
    if (old_expiry == EXPIRY_NONE && new_expiry == EXPIRY_NONE)
        return;
    else if (!new_entry || new_expiry == EXPIRY_NONE)
        hashTypeUntrackEntry(o, old_entry);
    else if (!old_entry || old_expiry == EXPIRY_NONE)
        hashTypeTrackEntry(o, new_entry);
    else {
        volatile_set *set = hashTypeGetVolatileSet(o);
        debugServerAssert(set);
        serverAssert(volatileSetUpdateEntry(set, old_entry, new_entry, old_expiry, new_expiry) == C_OK);
    }
}

void hashTypePropagateDeletion(serverDb *db, sds key, void *entry) {
    robj *argv[3];
    sds field = (sds)entry;
    argv[0] = shared.hdel;
    argv[1] = createStringObject(key, sdslen(key));
    argv[2] = createStringObject(field, sdslen(field));
    incrRefCount(argv[0]);

    /* If the primary decided to delete a key we must propagate it to replicas no matter what.
     * Even if module executed a command without asking for propagation. */
    int prev_replication_allowed = server.replication_allowed;
    server.replication_allowed = 1;
    alsoPropagate(db->id, argv, 3, PROPAGATE_AOF | PROPAGATE_REPL);
    server.replication_allowed = prev_replication_allowed;

    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
    decrRefCount(argv[2]);
}

int hashTypeExpireEntry(void *entry) {
    serverAssert(server.access_context.key && server.access_context.db);
    robj keyobj;
    sds key = objectGetKey(server.access_context.key);
    serverAssert(key);
    initStaticStringObject(keyobj, key);
    notifyKeyspaceEvent(NOTIFY_EXPIRED, "hexpired", &keyobj, server.access_context.db->id);
    serverLog(LL_NOTICE, "expiring entry %s of key %s", (sds)entry, key);
    hashTypePropagateDeletion(server.access_context.db, key, entry);
    return 1;
}

int hashTypeExpireRemoveEntry(void *entry) {
    serverAssert(server.access_context.key && server.access_context.db);
    hashTypeExpireEntry(entry);
    return hashTypeDelete(server.access_context.key, (sds)entry);
}

hashtableElementAccessState hashHashtableTypeAccess(hashtable *ht, void *entry) {
    UNUSED(ht);

    int delete_expired = 0;
    if (!canExpireWithFlags(0, &delete_expired)) return ELEMENT_VALID;

    if ((server.access_context.flags & OBJ_ACCESS_IGNORE_TTL) || !hashTypeEntryIsExpired(entry)) return ELEMENT_VALID;

    if (!delete_expired) return ELEMENT_INVALID;

    if (server.access_context.flags == OBJ_ACCESS_NONE) return ELEMENT_INVALID;

    robj *o = server.access_context.key;
    serverDb *db = server.access_context.db;
    serverAssert(o && db);

    hashTypeUntrackEntry(o, entry);
    hashTypeExpireEntry(entry);
    freeHashTypeEntry(entry);
    return ELEMENT_DELETE;
}

void hashTypeSetAccessContext(robj *o, serverDb *db) {
    setAccessContext(o, db);
}

void hashTypeResetAccessContext(void) {
    robj keyobj;
    robj *o = server.access_context.key;
    serverDb *db = server.access_context.db;
    serverAssert(!o || o->type == OBJ_HASH);
    resetAccessContext();
    if (o) {
        if (hashTypeLength(o) == 0) {
            initStaticStringObject(keyobj, objectGetKey(o));
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", &keyobj, db->id);
            dbDelete(db, &keyobj);
        }
    }
}

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/

/* Check the length of a number of objects to see if we need to convert a
 * listpack to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
void hashTypeTryConversion(robj *o, robj **argv, int start, int end) {
    int i;
    size_t sum = 0;

    if (o->encoding != OBJ_ENCODING_LISTPACK) return;

    /* We guess that most of the values in the input are unique, so
     * if there are enough arguments we create a pre-sized hash, which
     * might over allocate memory if there are duplicates. */
    size_t new_fields = (end - start + 1) / 2;
    if (new_fields > server.hash_max_listpack_entries) {
        hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
        hashtableExpand(o->ptr, new_fields);
        return;
    }

    for (i = start; i <= end; i++) {
        if (!sdsEncodedObject(argv[i])) continue;
        size_t len = sdslen(argv[i]->ptr);
        if (len > server.hash_max_listpack_value) {
            hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
            return;
        }
        sum += len;
    }
    if (!lpSafeToAdd(o->ptr, sum)) hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
}

/* Get the value from a listpack encoded hash, identified by field.
 * Returns -1 when the field cannot be found. */
int hashTypeGetFromListpack(robj *o, sds field, unsigned char **vstr, unsigned int *vlen, long long *vll) {
    unsigned char *zl, *fptr = NULL, *vptr = NULL;

    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK);

    zl = o->ptr;
    fptr = lpFirst(zl);
    if (fptr != NULL) {
        fptr = lpFind(zl, fptr, (unsigned char *)field, sdslen(field), 1);
        if (fptr != NULL) {
            /* Grab pointer to the value (fptr points to the field) */
            vptr = lpNext(zl, fptr);
            serverAssert(vptr != NULL);
        }
    }

    if (vptr != NULL) {
        *vstr = lpGetValue(vptr, vlen, vll);
        return 0;
    }

    return -1;
}

/* Get the value from a hash table encoded hash, identified by field.
 * Returns NULL when the field cannot be found, otherwise the SDS value
 * is returned. */
sds hashTypeGetFromHashTable(robj *o, sds field) {
    serverAssert(o->encoding == OBJ_ENCODING_HASHTABLE);
    void *found_element = NULL;
    hashtableFind(o->ptr, field, &found_element);
    if (found_element)
        return hashTypeEntryGetValue(found_element);
    else
        return NULL;
}

/* Higher level function of hashTypeGet*() that returns the hash value
 * associated with the specified field. If the field is found C_OK
 * is returned, otherwise C_ERR. The returned object is returned by
 * reference in either *vstr and *vlen if it's returned in string form,
 * or stored in *vll if it's returned as a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * for C_OK and checking if vll (or vstr) is NULL. */
int hashTypeGetValue(robj *o, sds field, unsigned char **vstr, unsigned int *vlen, long long *vll) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        *vstr = NULL;
        if (hashTypeGetFromListpack(o, field, vstr, vlen, vll) == 0) return C_OK;
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        sds value = hashTypeGetFromHashTable(o, field);
        if (value != NULL) {
            *vstr = (unsigned char *)value;
            *vlen = sdslen(value);
            return C_OK;
        }
    } else {
        serverPanic("Unknown hash encoding");
    }
    return C_ERR;
}

/* Returns the expiration time associated with the specified field.
 * If the field is found C_OK is returned, otherwise C_ERR.
 * The matching item expiration time is assigned to `expiry` memory location, if specified.
 * In case the item has no assigned expiration time, -1 is returned. */
int hashTypeGetExpiry(robj *o, sds field, long long *expiry) {
    if (o->encoding == OBJ_ENCODING_LISTPACK && hashTypeExists(o, field)) {
        if (expiry) *expiry = -1;
        return C_OK;
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        void *found_element = NULL;
        if (hashtableFind(o->ptr, field, &found_element)) {
            if (expiry) *expiry = hashTypeEntryGetExpiry(found_element);
            return C_OK;
        }
    } else {
        serverPanic("Unknown hash encoding");
    }
    return C_ERR;
}

/* Like hashTypeGetValue() but returns an Object, which is useful for
 * interaction with the hash type outside t_hash.c.
 * The function returns NULL if the field is not found in the hash. Otherwise
 * a newly allocated string object with the value is returned. */
robj *hashTypeGetValueObject(robj *o, sds field) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_ERR) return NULL;
    if (vstr)
        return createStringObject((char *)vstr, vlen);
    else
        return createStringObjectFromLongLong(vll);
}

/* Higher level function using hashTypeGet*() to return the length of the
 * object associated with the requested field, or 0 if the field does not
 * exist. */
size_t hashTypeGetValueLength(robj *o, sds field) {
    size_t len = 0;
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_OK) len = vstr ? vlen : sdigits10(vll);

    return len;
}

/* Test if the specified field exists in the given hash. Returns 1 if the field
 * exists, and 0 when it doesn't. */
int hashTypeExists(robj *o, sds field) {
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    return hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_OK;
}

/* Add a new field, overwrite the old with the new value if it already exists.
 * Return 0 on insert and 1 on update.
 *
 * By default, the field and value SDS strings are copied if needed, so the
 * caller retains ownership of the strings passed. However this behavior
 * can be effected by passing appropriate flags (possibly bitwise OR-ed):
 *
 * HASH_SET_TAKE_FIELD -- The SDS field ownership passes to the function.
 * HASH_SET_TAKE_VALUE -- The SDS value ownership passes to the function.
 *
 * When the flags are used the caller does not need to release the passed
 * SDS string(s). It's up to the function to use the string to create a new
 * entry or to free the SDS string before returning to the caller.
 *
 * HASH_SET_COPY corresponds to no flags passed, and means the default
 * semantics of copying the values if needed.
 *
 */
int hashTypeSet(robj *o, sds field, sds value, long long expiry, int flags) {
    int update = 0;

    /* Check if the field is too long for listpack, and convert before adding the item.
     * This is needed for HINCRBY* case since in other commands this is handled early by
     * hashTypeTryConversion, so this check will be a NOP. */
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        if (expiry > 0 || sdslen(field) > server.hash_max_listpack_value || sdslen(value) > server.hash_max_listpack_value)
            hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
    }

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl, *fptr, *vptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char *)field, sdslen(field), 1);
            if (fptr != NULL) {
                /* Grab pointer to the value (fptr points to the field) */
                vptr = lpNext(zl, fptr);
                serverAssert(vptr != NULL);
                update = 1;

                /* Replace value */
                zl = lpReplace(zl, &vptr, (unsigned char *)value, sdslen(value));
            }
        }

        if (!update) {
            /* Push new field/value pair onto the tail of the listpack */
            zl = lpAppend(zl, (unsigned char *)field, sdslen(field));
            zl = lpAppend(zl, (unsigned char *)value, sdslen(value));
        }
        o->ptr = zl;

        /* Check if the listpack needs to be converted to a hash table */
        if (hashTypeLength(o) > server.hash_max_listpack_entries) hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = o->ptr;

        sds v;
        if (flags & HASH_SET_TAKE_VALUE) {
            v = value;
            value = NULL;
        } else {
            v = sdsdup(value);
        }
        hashtablePosition position;
        void *existing;
        if (hashtableFindPositionForInsert(ht, field, &position, &existing)) {
            /* does not exist yet */
            hashTypeEntry *entry = hashTypeCreateEntry(field, v, expiry, NULL, 0);
            hashtableInsertAtPosition(ht, entry, &position);
            /* In case an expiry is set on the new entry, we need to track it */
            if (expiry != EXPIRY_NONE) {
                hashTypeTrackEntry(o, entry);
            }
        } else {
            /* exists: replace value */
            long long entry_expiry = hashTypeEntryGetExpiry(existing);

            /* In case the HASH_SET_KEEP_EXPIRY will force keeping the existing entry expiry. */
            if (flags & HASH_SET_KEEP_EXPIRY)
                expiry = entry_expiry;
            void *new_entry = hashTypeEntryUpdate(existing, v, expiry);
            if (new_entry != existing) {
                /* It has been reallocated. */
                int replaced = hashtableReplaceReallocatedEntry(ht, existing, new_entry);
                serverAssert(replaced);
            }
            hashTypeTrackUpdateEntry(o, existing, new_entry, entry_expiry, expiry);

            update = 1;
        }
    } else {
        serverPanic("Unknown hash encoding");
    }

    /* Free SDS strings we did not referenced elsewhere if the flags
     * want this function to be responsible. */
    if (flags & HASH_SET_TAKE_FIELD && field) sdsfree(field);
    if (flags & HASH_SET_TAKE_VALUE && value) sdsfree(value);
    return update;
}

/* Set expiration on the specific HASH object 'o' item indicated by 'field'.
 * returns -2 in case the provided object is NULL or the specific field was not found.
 * returns 0 if the specified flag conditions has not been met.
 * returns 1 if the expiration time was applied.
 * returns 2 when 'expire' indicate a past Unix time. In this case, if the item exists in the HASH, it will also be expired.
 */
int hashTypeSetExpire(robj *o, sds field, long long expiry, int flag) {
    /* If no object we will return -2 */
    if (o == NULL) return -2;

    if (timestampIsExpired(expiry)) {
        /* It is possible that the assigned expiration is set in the past (or zero).
         * In such case we cannot count on the hash object representation to be hashtable, so
         * we operate this on before we check for the encoding. */
        if (hashTypeDelete(o, field)) {
            hashTypeExpireEntry(field);
            return 2;
        } else {
            return -2;
        }
    }

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        /* When listpack representation is used, we consider it as infinite TTL,
         * so expire command with gt always fail the GT as well as existence(XX).
         * Else, we already know we are going to set an expiration so we expend to hashtable encoding. */
        if (flag & EXPIRE_XX || flag & EXPIRE_GT) {
            return 0;
        } else {
            hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
        }
    }

    hashtable *ht = o->ptr;
    void **entry_ref = NULL;
    if ((entry_ref = hashtableFindRef(ht, field))) {
        hashTypeEntry *current_entry = *entry_ref;
        long long current_expire = hashTypeEntryGetExpiry(current_entry);
        if (flag) {
            /* NX option is set, check no current expiry */
            if (flag & EXPIRE_NX) {
                if (current_expire != EXPIRY_NONE) {
                    return 0;
                }
            }

            /* XX option is set, check current expiry */
            if (flag & EXPIRE_XX) {
                if (current_expire == EXPIRY_NONE) {
                    return 0;
                }
            }

            /* GT option is set, check current expiry */
            if (flag & EXPIRE_GT) {
                /* When current_expire is -1, we consider it as infinite TTL,
                 * so expire command with gt always fail the GT. */
                if (expiry <= current_expire || current_expire == EXPIRY_NONE) {
                    return 0;
                }
            }

            /* LT option is set, check current expiry */
            if (flag & EXPIRE_LT) {
                /* When current_expire -1, we consider it as infinite TTL,
                 * so if there is an expiry on the key and it's not less than current, we fail the LT. */
                if (current_expire != EXPIRY_NONE && expiry >= current_expire) {
                    return 0;
                }
            }
        }
        *entry_ref = hashTypeEntrySetExpiry(current_entry, expiry);
        hashTypeTrackUpdateEntry(o, current_entry, *entry_ref, current_expire, expiry);
        return 1;
    }
    return -2; // we did not find anything to do. return -2
}


int hashTypePersist(robj *o, sds field) {
    /* NULL object returns -2 */
    if (o == NULL || o->type != OBJ_HASH) return -2;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        if (hashTypeExists(o, field))
            /* When listpack representation is used, All items are without expiry */
            return -1;
        else
            return -2; // Did not find any element return -2
    }

    hashtable *ht = o->ptr;
    void **entry_ref = NULL;
    if ((entry_ref = hashtableFindRef(ht, field))) {
        hashTypeEntry *current_entry = *entry_ref;
        long long current_expire = hashTypeEntryGetExpiry(current_entry);
        if (current_expire != EXPIRY_NONE) {
            hashTypeUntrackEntry(o, current_entry);
            *entry_ref = hashTypeEntryUpdate(current_entry, hashTypeEntryGetValue(current_entry), EXPIRY_NONE);
            return 1;
        }
        return -1; // If the found element has no expiration set, return -1
    }
    return -2; // Did not find any element return -2
}

/* Delete an element from a hash.
 * Return 1 on deleted and 0 on not found. */
int hashTypeDelete(robj *o, sds field) {
    int deleted = 0;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl, *fptr;

        zl = o->ptr;
        fptr = lpFirst(zl);
        if (fptr != NULL) {
            fptr = lpFind(zl, fptr, (unsigned char *)field, sdslen(field), 1);
            if (fptr != NULL) {
                /* Delete both field and value. */
                zl = lpDeleteRangeWithEntry(zl, &fptr, 2);
                o->ptr = zl;
                deleted = 1;
            }
        }
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = o->ptr;
        void *entry = NULL;
        deleted = hashtablePop(ht, field, &entry);
        if (deleted) {
            hashTypeUntrackEntry(o, entry);
        }
    } else {
        serverPanic("Unknown hash encoding");
    }
    return deleted;
}

/* Return the number of elements in a hash. */
unsigned long hashTypeLength(const robj *o) {
    switch (o->encoding) {
    case OBJ_ENCODING_LISTPACK:
        return lpLength(o->ptr) / 2;
    case OBJ_ENCODING_HASHTABLE:
        return hashtableSize((const hashtable *)o->ptr);
    default:
        serverPanic("Unknown hash encoding");
        return ULONG_MAX;
    }
}

void hashTypeInitIterator(robj *subject, hashTypeIterator *hi) {
    hi->subject = subject;
    hi->encoding = subject->encoding;
    hi->volatile_items = 0;

    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        hi->fptr = NULL;
        hi->vptr = NULL;
    } else if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtableInitIterator(&hi->iter, subject->ptr, 0);
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void hashTypeInitVolatileIterator(robj *subject, hashTypeIterator *hi) {
    hi->subject = subject;
    hi->encoding = subject->encoding;
    hi->volatile_items = 1;

    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        return;
    } else if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        volatileSetStart(hashTypeGetVolatileSet(subject), &hi->viter);
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void hashTypeResetIterator(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        if (!hi->volatile_items)
            hashtableResetIterator(&hi->iter);
        else
            volatileSetReset(&hi->viter);
    }
}

/* Move to the next entry in the hash. Return C_OK when the next entry
 * could be found and C_ERR when the iterator reaches the end. */
int hashTypeNext(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl;
        unsigned char *fptr, *vptr;

        /* listpack encoding does not have volatile items, so return as iteration end */
        if (hi->volatile_items) return C_ERR;

        zl = hi->subject->ptr;
        fptr = hi->fptr;
        vptr = hi->vptr;

        if (fptr == NULL) {
            /* Initialize cursor */
            serverAssert(vptr == NULL);
            fptr = lpFirst(zl);
        } else {
            /* Advance cursor */
            serverAssert(vptr != NULL);
            fptr = lpNext(zl, vptr);
        }
        if (fptr == NULL) return C_ERR;

        /* Grab pointer to the value (fptr points to the field) */
        vptr = lpNext(zl, fptr);
        serverAssert(vptr != NULL);

        /* fptr, vptr now point to the first or next pair */
        hi->fptr = fptr;
        hi->vptr = vptr;
    } else if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        if (!hi->volatile_items) {
            if (!hashtableNext(&hi->iter, &hi->next)) return C_ERR;
        } else {
            if (!volatileSetNext(&hi->viter, &hi->next)) return C_ERR;
        }
    } else {
        serverPanic("Unknown hash encoding");
    }
    return C_OK;
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a listpack. Prototype is similar to `hashTypeGetFromListpack`. */
void hashTypeCurrentFromListpack(hashTypeIterator *hi,
                                 int what,
                                 unsigned char **vstr,
                                 unsigned int *vlen,
                                 long long *vll) {
    serverAssert(hi->encoding == OBJ_ENCODING_LISTPACK);

    if (what & OBJ_HASH_FIELD) {
        *vstr = lpGetValue(hi->fptr, vlen, vll);
    } else {
        *vstr = lpGetValue(hi->vptr, vlen, vll);
    }
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a hash table. Prototype is similar to
 * `hashTypeGetFromHashTable`. */
sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what) {
    serverAssert(hi->encoding == OBJ_ENCODING_HASHTABLE);

    if (what & OBJ_HASH_FIELD) {
        return hashTypeEntryGetField(hi->next);
    } else {
        return hashTypeEntryGetValue(hi->next);
    }
}

/* Higher level function of hashTypeCurrent*() that returns the hash value
 * at current iterator position.
 *
 * The returned element is returned by reference in either *vstr and *vlen if
 * it's returned in string form, or stored in *vll if it's returned as
 * a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * type checking if vstr == NULL. */
static void hashTypeCurrentObject(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        *vstr = NULL;
        hashTypeCurrentFromListpack(hi, what, vstr, vlen, vll);
    } else if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        sds ele = hashTypeCurrentFromHashTable(hi, what);
        *vstr = (unsigned char *)ele;
        *vlen = sdslen(ele);
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* Return the field or value at the current iterator position as a new
 * SDS string. */
sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    hashTypeCurrentObject(hi, what, &vstr, &vlen, &vll);
    if (vstr) return sdsnewlen(vstr, vlen);
    return sdsfromlonglong(vll);
}

robj *hashTypeLookupWriteOrCreate(client *c, robj *key) {
    robj *o = lookupKeyWrite(c->db, key);
    if (checkType(c, o, OBJ_HASH)) return NULL;

    if (o == NULL) {
        o = createHashObject();
        dbAdd(c->db, key, &o);
    }
    return o;
}


void hashTypeConvertListpack(robj *o, int enc) {
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK);

    if (enc == OBJ_ENCODING_LISTPACK) {
        /* Nothing to do... */

    } else if (enc == OBJ_ENCODING_HASHTABLE) {
        hashTypeIterator hi;

        hashtable *ht = hashtableCreate(&hashHashtableType);

        /* Presize the hashtable to avoid rehashing */
        hashtableExpand(ht, hashTypeLength(o));

        hashTypeInitIterator(o, &hi);
        while (hashTypeNext(&hi) != C_ERR) {
            sds field = hashTypeCurrentObjectNewSds(&hi, OBJ_HASH_FIELD);
            sds value = hashTypeCurrentObjectNewSds(&hi, OBJ_HASH_VALUE);
            hashTypeEntry *entry = hashTypeCreateEntry(field, value, -1, NULL, 0);
            sdsfree(field);
            if (!hashtableAdd(ht, entry)) {
                freeHashTypeEntry(entry);
                hashTypeResetIterator(&hi); /* Needed for gcc ASAN */
                serverLogHexDump(LL_WARNING, "listpack with dup elements dump", o->ptr, lpBytes(o->ptr));
                serverPanic("Listpack corruption detected");
            }
        }
        hashTypeResetIterator(&hi);
        zfree(o->ptr);
        o->encoding = OBJ_ENCODING_HASHTABLE;
        o->ptr = ht;
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void hashTypeConvert(robj *o, int enc) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        hashTypeConvertListpack(o, enc);
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        serverPanic("Not implemented");
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* This is a helper function for the COPY command.
 * Duplicate a hash object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *hashTypeDup(robj *o) {
    robj *hobj;
    hashTypeIterator hi;

    serverAssert(o->type == OBJ_HASH);

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = o->ptr;
        size_t sz = lpBytes(zl);
        unsigned char *new_zl = zmalloc(sz);
        memcpy(new_zl, zl, sz);
        hobj = createObject(OBJ_HASH, new_zl);
        hobj->encoding = OBJ_ENCODING_LISTPACK;
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = hashtableCreate(&hashHashtableType);
        hashtableExpand(ht, hashtableSize((const hashtable *)o->ptr));

        hashTypeInitIterator(o, &hi);
        while (hashTypeNext(&hi) != C_ERR) {
            /* Extract a field-value pair from an original hash object.*/
            sds field = hashTypeCurrentFromHashTable(&hi, OBJ_HASH_FIELD);
            sds value = hashTypeCurrentFromHashTable(&hi, OBJ_HASH_VALUE);

            /* Add a field-value pair to a new hash object. */
            hashTypeEntry *entry = hashTypeCreateEntry(field, sdsdup(value), -1, NULL, 0);
            hashtableAdd(ht, entry);
        }
        hashTypeResetIterator(&hi);
        hobj = createObject(OBJ_HASH, ht);
        hobj->encoding = OBJ_ENCODING_HASHTABLE;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return hobj;
}

/* Create a new sds string from the listpack entry. */
sds hashSdsFromListpackEntry(listpackEntry *e) {
    return e->sval ? sdsnewlen(e->sval, e->slen) : sdsfromlonglong(e->lval);
}

/* Reply with bulk string from the listpack entry. */
void hashReplyFromListpackEntry(client *c, listpackEntry *e) {
    if (e->sval)
        addReplyBulkCBuffer(c, e->sval, e->slen);
    else
        addReplyBulkLongLong(c, e->lval);
}

/* Return random element from a non empty hash.
 * 'field' and 'val' will be set to hold the element.
 * The memory in them is not to be freed or modified by the caller.
 * 'val' can be NULL in which case it's not extracted. */
static void hashTypeRandomElement(robj *hashobj, unsigned long hashsize, listpackEntry *field, listpackEntry *val) {
    if (hashobj->encoding == OBJ_ENCODING_HASHTABLE) {
        void *entry = NULL;

        while (!entry) {
            hashtableFairRandomEntry(hashobj->ptr, &entry);
            sds sds_field = hashTypeEntryGetField(entry);
            field->sval = (unsigned char *)sds_field;
            field->slen = sdslen(sds_field);
            if (val) {
                hashTypeEntry *hash_entry = entry;
                sds sds_val = hashTypeEntryGetValue(hash_entry);
                val->sval = (unsigned char *)sds_val;
                val->slen =
                    sdslen(sds_val);
            }
        }
    } else if (hashobj->encoding == OBJ_ENCODING_LISTPACK) {
        lpRandomPair(hashobj->ptr, hashsize, field, val);
    } else {
        serverPanic("Unknown hash encoding");
    }
}


/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

void hsetnxCommand(client *c) {
    robj *o;
    if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;
    hashTypeSetAccessContext(o, c->db);
    if (hashTypeExists(o, c->argv[2]->ptr)) {
        addReply(c, shared.czero);
    } else {
        hashTypeTryConversion(o, c->argv, 2, 3);
        hashTypeSet(o, c->argv[2]->ptr, c->argv[3]->ptr, EXPIRY_NONE, HASH_SET_COPY | HASH_SET_KEEP_EXPIRY);
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_HASH, "hset", c->argv[1], c->db->id);
        server.dirty++;
        addReply(c, shared.cone);
    }
    hashTypeResetAccessContext();
}

void hsetCommand(client *c) {
    int i, created = 0;
    robj *o;

    if ((c->argc % 2) == 1) {
        addReplyErrorArity(c);
        return;
    }

    if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;
    hashTypeTryConversion(o, c->argv, 2, c->argc - 1);

    hashTypeSetAccessContext(o, c->db);
    for (i = 2; i < c->argc; i += 2) created += !hashTypeSet(o, c->argv[i]->ptr, c->argv[i + 1]->ptr, EXPIRY_NONE, HASH_SET_COPY | HASH_SET_KEEP_EXPIRY);

    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH, "hset", c->argv[1], c->db->id);
    server.dirty += (c->argc - 2) / 2;

    /* HMSET (deprecated) and HSET return value is different. */
    char *cmdname = c->argv[0]->ptr;
    if (cmdname[1] == 's' || cmdname[1] == 'S') {
        /* HSET */
        addReplyLongLong(c, created);
    } else {
        /* HMSET */
        addReply(c, shared.ok);
    }

    hashTypeResetAccessContext();
}

void hincrbyCommand(client *c) {
    long long value, incr, oldvalue;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if (getLongLongFromObjectOrReply(c, c->argv[3], &incr, NULL) != C_OK) return;
    if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;
    hashTypeSetAccessContext(o, c->db);
    if (hashTypeGetValue(o, c->argv[2]->ptr, &vstr, &vlen, &value) == C_OK) {
        if (vstr) {
            if (string2ll((char *)vstr, vlen, &value) == 0) {
                addReplyError(c, "hash value is not an integer");
                hashTypeResetAccessContext();
                return;
            }
        } /* Else hashTypeGetValue() already stored it into &value */
    } else {
        value = 0;
    }

    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN - oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX - oldvalue))) {
        addReplyError(c, "increment or decrement would overflow");
        hashTypeResetAccessContext();
        return;
    }
    value += incr;
    new = sdsfromlonglong(value);
    hashTypeSet(o, c->argv[2]->ptr, new, EXPIRY_NONE, HASH_SET_TAKE_VALUE | HASH_SET_KEEP_EXPIRY);
    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH, "hincrby", c->argv[1], c->db->id);
    server.dirty++;
    addReplyLongLong(c, value);
    hashTypeResetAccessContext();
}

void hincrbyfloatCommand(client *c) {
    long double value, incr;
    long long ll;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if (getLongDoubleFromObjectOrReply(c, c->argv[3], &incr, NULL) != C_OK) return;
    if (isnan(incr) || isinf(incr)) {
        addReplyError(c, "value is NaN or Infinity");
        return;
    }
    if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;

    hashTypeSetAccessContext(o, c->db);

    if (hashTypeGetValue(o, c->argv[2]->ptr, &vstr, &vlen, &ll) == C_OK) {
        if (vstr) {
            if (string2ld((char *)vstr, vlen, &value) == 0) {
                addReplyError(c, "hash value is not a float");
                hashTypeResetAccessContext();
                return;
            }
        } else {
            value = (long double)ll;
        }
    } else {
        value = 0;
    }

    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c, "increment would produce NaN or Infinity");
        hashTypeResetAccessContext();
        return;
    }

    char buf[MAX_LONG_DOUBLE_CHARS];
    int len = ld2string(buf, sizeof(buf), value, LD_STR_HUMAN);
    new = sdsnewlen(buf, len);
    hashTypeSet(o, c->argv[2]->ptr, new, EXPIRY_NONE, HASH_SET_TAKE_VALUE | HASH_SET_KEEP_EXPIRY);
    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH, "hincrbyfloat", c->argv[1], c->db->id);
    server.dirty++;
    addReplyBulkCBuffer(c, buf, len);

    /* Always replicate HINCRBYFLOAT as an HSET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    robj *newobj;
    newobj = createRawStringObject(buf, len);
    rewriteClientCommandArgument(c, 0, shared.hset);
    rewriteClientCommandArgument(c, 3, newobj);
    decrRefCount(newobj);
    hashTypeResetAccessContext();
}

static void addHashFieldToReply(client *c, robj *o, sds field) {
    if (o == NULL) {
        addReplyNull(c);
        return;
    }

    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll) == C_OK) {
        if (vstr) {
            addReplyBulkCBuffer(c, vstr, vlen);
        } else {
            addReplyBulkLongLong(c, vll);
        }
    } else {
        addReplyNull(c);
    }
}

void hgetCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.null[c->resp])) == NULL || checkType(c, o, OBJ_HASH)) return;
    hashTypeSetAccessContext(o, c->db);
    addHashFieldToReply(c, o, c->argv[2]->ptr);

    if (hashTypeLength(o) == 0) {
        dbDelete(c->db, c->argv[1]);
    }
    hashTypeResetAccessContext();
}

void hmgetCommand(client *c) {
    robj *o;
    int i;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HMGET should respond with a series of null bulks. */
    o = lookupKeyRead(c->db, c->argv[1]);

    if (checkType(c, o, OBJ_HASH)) return;

    hashTypeSetAccessContext(o, c->db);

    addReplyArrayLen(c, c->argc - 2);
    for (i = 2; i < c->argc; i++) {
        addHashFieldToReply(c, o, c->argv[i]->ptr);
    }
    if (o && hashTypeLength(o) == 0) {
        dbDelete(c->db, c->argv[1]);
    }

    hashTypeResetAccessContext();
}

void hdelCommand(client *c) {
    robj *o;
    int j, deleted = 0;

    if ((o = lookupKeyWriteOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_HASH)) return;
    hashTypeSetAccessContext(o, c->db);
    for (j = 2; j < c->argc; j++) {
        if (hashTypeDelete(o, c->argv[j]->ptr)) {
            deleted++;
            if (hashTypeLength(o) == 0) {
                break;
            }
        }
    }
    if (deleted) {
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_HASH, "hdel", c->argv[1], c->db->id);
        server.dirty += deleted;
    }
    hashTypeResetAccessContext();
    addReplyLongLong(c, deleted);
}

void hlenCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_HASH)) return;

    addReplyLongLong(c, hashTypeLength(o));
}

void hstrlenCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_HASH)) return;
    hashTypeSetAccessContext(o, c->db);
    addReplyLongLong(c, hashTypeGetValueLength(o, c->argv[2]->ptr));
    hashTypeResetAccessContext();
}

static void addHashIteratorCursorToReply(writePreparedClient *wpc, hashTypeIterator *hi, int what) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromListpack(hi, what, &vstr, &vlen, &vll);
        if (vstr)
            addWritePreparedReplyBulkCBuffer(wpc, vstr, vlen);
        else
            addWritePreparedReplyBulkLongLong(wpc, vll);
    } else if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        sds value = hashTypeCurrentFromHashTable(hi, what);
        addWritePreparedReplyBulkCBuffer(wpc, value, sdslen(value));
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void genericHgetallCommand(client *c, int flags) {
    robj *o;
    hashTypeIterator hi;
    int count = 0;

    robj *emptyResp = (flags & OBJ_HASH_FIELD && flags & OBJ_HASH_VALUE) ? shared.emptymap[c->resp] : shared.emptyarray;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], emptyResp)) == NULL || checkType(c, o, OBJ_HASH)) return;

    writePreparedClient *wpc = prepareClientForFutureWrites(c);
    if (!wpc) return;
    /* We return a map if the user requested fields and values, like in the
     * HGETALL case. Otherwise to use a flat array makes more sense. */
    void *replylen = addReplyDeferredLen(c);
    hashTypeInitIterator(o, &hi);
    while (hashTypeNext(&hi) != C_ERR) {
        if (flags & OBJ_HASH_FIELD) {
            addHashIteratorCursorToReply(wpc, &hi, OBJ_HASH_FIELD);
            count++;
        }
        if (flags & OBJ_HASH_VALUE) {
            addHashIteratorCursorToReply(wpc, &hi, OBJ_HASH_VALUE);
            count++;
        }
    }

    hashTypeResetIterator(&hi);
    /* Make sure we returned the right number of elements. */
    if (flags & OBJ_HASH_FIELD && flags & OBJ_HASH_VALUE) {
        setDeferredMapLen(c, replylen, count /= 2);
        count /= 2;
    } else {
        setDeferredArrayLen(c, replylen, count);
    }
}

void hkeysCommand(client *c) {
    genericHgetallCommand(c, OBJ_HASH_FIELD);
}

void hvalsCommand(client *c) {
    genericHgetallCommand(c, OBJ_HASH_VALUE);
}

void hgetallCommand(client *c) {
    genericHgetallCommand(c, OBJ_HASH_FIELD | OBJ_HASH_VALUE);
}

void hexistsCommand(client *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_HASH)) return;
    hashTypeSetAccessContext(o, c->db);
    addReply(c, hashTypeExists(o, c->argv[2]->ptr) ? shared.cone : shared.czero);
    hashTypeResetAccessContext();
}

void hscanCommand(client *c) {
    robj *o;
    unsigned long long cursor;

    if (parseScanCursorOrReply(c, c->argv[2]->ptr, &cursor) == C_ERR) return;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.emptyscan)) == NULL || checkType(c, o, OBJ_HASH)) return;
    scanGenericCommand(c, o, cursor);
}

static void hrandfieldReplyWithListpack(writePreparedClient *wpc, unsigned int count, listpackEntry *fields, listpackEntry *vals) {
    client *c = (client *)wpc;
    for (unsigned long i = 0; i < count; i++) {
        if (vals && c->resp > 2) addWritePreparedReplyArrayLen(wpc, 2);
        if (fields[i].sval)
            addWritePreparedReplyBulkCBuffer(wpc, fields[i].sval, fields[i].slen);
        else
            addWritePreparedReplyBulkLongLong(wpc, fields[i].lval);
        if (vals) {
            if (vals[i].sval)
                addWritePreparedReplyBulkCBuffer(wpc, vals[i].sval, vals[i].slen);
            else
                addWritePreparedReplyBulkLongLong(wpc, vals[i].lval);
        }
    }
}


void hexpireGenericCommand(client *c, long long basetime, int unit) {
    robj *key = c->argv[1], *param = c->argv[2];
    long long when; /* unix time in milliseconds when the key will expire. */
    int flag = 0;
    int fields_index = 3;
    long long num_fields = 0;
    int i, result = 0;

    for (; fields_index < c->argc; fields_index++) {
        if (!strcasecmp(c->argv[fields_index]->ptr, "fields")) {
            /* checking optional flags */
            if (parseExtendedExpireArgumentsOrReply(c, &flag, fields_index++) != C_OK) return;
            if (getLongLongFromObjectOrReply(c, c->argv[fields_index++], &num_fields, NULL) != C_OK) return;
            break;
        }
    }

    if (getLongLongFromObjectOrReply(c, param, &when, NULL) != C_OK) return;

    /* HEXPIRE allows negative numbers, but we can at least detect an
     * overflow by either unit conversion or basetime addition. */
    if (unit == UNIT_SECONDS) {
        if (when > LLONG_MAX / 1000 || when < LLONG_MIN / 1000) {
            addReplyErrorExpireTime(c);
            return;
        }
        when *= 1000;
    }

    if (when > LLONG_MAX - basetime) {
        addReplyErrorExpireTime(c);
        return;
    }
    when += basetime;

    robj *obj = lookupKeyWrite(c->db, key);

    /* Non HASH type return simple error */
    if (checkType(c, obj, OBJ_HASH)) {
        return;
    }

    hashTypeSetAccessContext(obj, c->db);

    /* From this point we would return array reply */
    addReplyArrayLen(c, num_fields);

    for (i = fields_index; i < c->argc; i++) {
        result = hashTypeSetExpire(obj, c->argv[i]->ptr, when, flag);
        server.dirty += (result > 0 ? 1 : 0); // in case there was a change increment the dirty
        addReplyLongLong(c, result);
    }
    notifyKeyspaceEvent(NOTIFY_HASH, "hexpire", c->argv[1], c->db->id);

    hashTypeResetAccessContext();
}

void hexpireCommand(client *c) {
    hexpireGenericCommand(c, commandTimeSnapshot(), UNIT_SECONDS);
}

void hexpireAtCommand(client *c) {
    hexpireGenericCommand(c, 0, UNIT_SECONDS);
}

void hpexpireCommand(client *c) {
    hexpireGenericCommand(c, commandTimeSnapshot(), UNIT_MILLISECONDS);
}

void hpexpireAtCommand(client *c) {
    hexpireGenericCommand(c, 0, UNIT_MILLISECONDS);
}

void hpersistCommand(client *c) {
    int fields_index = 4, result = 0;
    long long num_fields = 0;

    if (getLongLongFromObjectOrReply(c, c->argv[fields_index - 1], &num_fields, NULL) != C_OK) return;

    if (num_fields > c->argc - 4) num_fields = c->argc - 4; // Potential user error, but we would like to make effort to comply with the request.

    /* From this point we would return array reply */
    addReplyArrayLen(c, num_fields);

    robj *hash = lookupKeyWrite(c->db, c->argv[1]);

    hashTypeSetAccessContext(hash, c->db);

    for (; fields_index < 4 + num_fields; fields_index++) {
        result = hashTypePersist(hash, c->argv[fields_index]->ptr);
        server.dirty += (result > 0 ? 1 : 0); // in case there was a change increment the dirty
        addReplyLongLong(c, result);
    }

    hashTypeResetAccessContext();
}

void httlGenericCommand(client *c, long long basetime, int unit) {
    int fields_index = 4;
    long long num_fields = 0, result = -2;

    if (getLongLongFromObjectOrReply(c, c->argv[fields_index - 1], &num_fields, NULL) != C_OK) return;

    if (num_fields > c->argc - 4) num_fields = c->argc - 4; // Potential user error, but we would like to make effort to comply with the request.

    robj *hash = lookupKeyRead(c->db, c->argv[1]);

    /* From this point we would return array reply */
    addReplyArrayLen(c, num_fields);

    hashTypeSetAccessContext(hash, c->db);

    for (; fields_index < 4 + num_fields; fields_index++) {
        if (!hash || hashTypeGetExpiry(hash, c->argv[fields_index]->ptr, &result) == C_ERR) {
            addReplyLongLong(c, -2);
        } else if (result == EXPIRY_NONE) {
            addReplyLongLong(c, -1);
        } else {
            result = result - basetime;
            if (result < 0) result = 0;
            addReplyLongLong(c, unit == UNIT_MILLISECONDS ? result : ((result + 500) / 1000));
        }
    }

    hashTypeResetAccessContext();
}

void httlCommand(client *c) {
    httlGenericCommand(c, commandTimeSnapshot(), UNIT_SECONDS);
}

void hpttlCommand(client *c) {
    httlGenericCommand(c, commandTimeSnapshot(), UNIT_MILLISECONDS);
}

void hexpiretimeCommand(client *c) {
    httlGenericCommand(c, 0, UNIT_SECONDS);
}

void hpexpiretimeCommand(client *c) {
    httlGenericCommand(c, 0, UNIT_MILLISECONDS);
}

/* How many times bigger should be the hash compared to the requested size
 * for us to not use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define HRANDFIELD_SUB_STRATEGY_MUL 3

/* If client is trying to ask for a very large number of random elements,
 * queuing may consume an unlimited amount of memory, so we want to limit
 * the number of randoms per time. */
#define HRANDFIELD_RANDOM_SAMPLE_LIMIT 1000

void hrandfieldWithCountCommand(client *c, long l, int withvalues) {
    unsigned long count, size;
    int uniq = 1;
    robj *hash;

    if ((hash = lookupKeyReadOrReply(c, c->argv[1], shared.emptyarray)) == NULL || checkType(c, hash, OBJ_HASH)) return;
    size = hashTypeLength(hash);

    if (l >= 0) {
        count = (unsigned long)l;
    } else {
        count = -l;
        uniq = 0;
    }

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        addReply(c, shared.emptyarray);
        return;
    }

    writePreparedClient *wpc = prepareClientForFutureWrites(c);
    if (!wpc) return;

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. This case is the only one that also needs to return the
     * elements in random order. */
    if (!uniq || count == 1) {
        if (withvalues && c->resp == 2)
            addWritePreparedReplyArrayLen(wpc, count * 2);
        else
            addWritePreparedReplyArrayLen(wpc, count);
        if (hash->encoding == OBJ_ENCODING_HASHTABLE) {
            while (count && hashtableSize(hash->ptr) > 0) {
                void *entry;
                hashtableFairRandomEntry(hash->ptr, &entry);
                count--;
                sds field = hashTypeEntryGetField(entry);
                sds value = hashTypeEntryGetValue(entry);
                if (withvalues && c->resp > 2) addWritePreparedReplyArrayLen(wpc, 2);
                addWritePreparedReplyBulkCBuffer(wpc, field, sdslen(field));
                if (withvalues) addWritePreparedReplyBulkCBuffer(wpc, value, sdslen(value));
                if (c->flag.close_asap) break;
            }
        } else if (hash->encoding == OBJ_ENCODING_LISTPACK) {
            listpackEntry *fields, *vals = NULL;
            unsigned long limit, sample_count;

            limit = count > HRANDFIELD_RANDOM_SAMPLE_LIMIT ? HRANDFIELD_RANDOM_SAMPLE_LIMIT : count;
            fields = zmalloc(sizeof(listpackEntry) * limit);
            if (withvalues) vals = zmalloc(sizeof(listpackEntry) * limit);
            while (count) {
                sample_count = count > limit ? limit : count;
                count -= sample_count;
                lpRandomPairs(hash->ptr, sample_count, fields, vals);
                hrandfieldReplyWithListpack(wpc, sample_count, fields, vals);
                if (c->flag.close_asap) break;
            }
            zfree(fields);
            zfree(vals);
        }
        return;
    }

    /* Initiate reply count, RESP3 responds with nested array, RESP2 with flat one. */
    long reply_size = count < size ? count : size;
    if (withvalues && c->resp == 2)
        addWritePreparedReplyArrayLen(wpc, reply_size * 2);
    else
        addWritePreparedReplyArrayLen(wpc, reply_size);

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the hash: simply return the whole hash. */
    if (count >= size) {
        hashTypeIterator hi;
        hashTypeInitIterator(hash, &hi);
        while (hashTypeNext(&hi) != C_ERR) {
            if (withvalues && c->resp > 2) addWritePreparedReplyArrayLen(wpc, 2);
            addHashIteratorCursorToReply(wpc, &hi, OBJ_HASH_FIELD);
            if (withvalues) addHashIteratorCursorToReply(wpc, &hi, OBJ_HASH_VALUE);
        }
        hashTypeResetIterator(&hi);
        return;
    }

    /* CASE 2.5 listpack only. Sampling unique elements, in non-random order.
     * Listpack encoded hashes are meant to be relatively small, so
     * HRANDFIELD_SUB_STRATEGY_MUL isn't necessary and we rather not make
     * copies of the entries. Instead, we emit them directly to the output
     * buffer.
     *
     * And it is inefficient to repeatedly pick one random element from a
     * listpack in CASE 4. So we use this instead. */
    if (hash->encoding == OBJ_ENCODING_LISTPACK) {
        listpackEntry *fields, *vals = NULL;
        fields = zmalloc(sizeof(listpackEntry) * count);
        if (withvalues) vals = zmalloc(sizeof(listpackEntry) * count);
        serverAssert(lpRandomPairsUnique(hash->ptr, count, fields, vals) == count);
        hrandfieldReplyWithListpack(wpc, count, fields, vals);
        zfree(fields);
        zfree(vals);
        return;
    }
    /* CASE 3:
     * The number of elements inside the hash is not greater than
     * HRANDFIELD_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a hash from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requested elements is just
     * a bit less than the number of elements in the hash, the natural approach
     * used into CASE 4 is highly inefficient. */
    if (count * HRANDFIELD_SUB_STRATEGY_MUL > size) {
        /* Hashtable encoding (generic implementation) */
        hashtable *ht = hashtableCreate(&sdsReplyHashtableType);
        hashtableExpand(ht, size);
        hashtableIterator iter;
        hashtableInitIterator(&iter, hash->ptr, 0);
        void *entry;

        /* Add all the elements into the temporary hashtable. */
        while (hashtableNext(&iter, &entry)) {
            int res = hashtableAdd(ht, entry);
            serverAssert(res);
        }
        serverAssert(hashtableSize(ht) == size);
        hashtableResetIterator(&iter);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            void *element;
            hashtableFairRandomEntry(ht, &element);
            hashtableDelete(ht, element);
            size--;
        }

        /* Reply with what's in the temporary hashtable and release memory */
        hashtableInitIterator(&iter, ht, 0);
        void *next;
        while (hashtableNext(&iter, &next)) {
            sds field = hashTypeEntryGetField(next);
            sds value = hashTypeEntryGetValue(next);
            if (withvalues && c->resp > 2) addWritePreparedReplyArrayLen(wpc, 2);
            addWritePreparedReplyBulkCBuffer(wpc, field, sdslen(field));
            if (withvalues) addWritePreparedReplyBulkCBuffer(wpc, value, sdslen(value));
        }

        hashtableResetIterator(&iter);
        hashtableRelease(ht);
    }

    /* CASE 4: We have a big hash compared to the requested number of elements.
     * In this case we can simply get random elements from the hash and add
     * to the temporary hash, trying to eventually get enough unique elements
     * to reach the specified count. */
    else {
        /* Hashtable encoding (generic implementation) */
        unsigned long added = 0;
        listpackEntry field, value;
        hashtable *ht = hashtableCreate(&setHashtableType);
        hashtableExpand(ht, count);
        while (added < count) {
            hashTypeRandomElement(hash, size, &field, withvalues ? &value : NULL);

            /* Try to add the object to the hashtable. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result hashtable. */
            sds sfield = hashSdsFromListpackEntry(&field);
            if (!hashtableAdd(ht, sfield)) {
                sdsfree(sfield);
                continue;
            }
            added++;

            /* We can reply right away, so that we don't need to store the value in the dict. */
            if (withvalues && c->resp > 2) addWritePreparedReplyArrayLen(wpc, 2);
            hashReplyFromListpackEntry(c, &field);
            if (withvalues) hashReplyFromListpackEntry(c, &value);
        }

        /* Release memory */
        hashtableRelease(ht);
    }
}

/* HRANDFIELD key [<count> [WITHVALUES]] */
void hrandfieldCommand(client *c) {
    long l;
    int withvalues = 0;
    robj *hash;
    listpackEntry ele;

    if (c->argc >= 3) {
        if (getRangeLongFromObjectOrReply(c, c->argv[2], -LONG_MAX, LONG_MAX, &l, NULL) != C_OK) return;
        if (c->argc > 4 || (c->argc == 4 && strcasecmp(c->argv[3]->ptr, "withvalues"))) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        } else if (c->argc == 4) {
            withvalues = 1;
            if (l < -LONG_MAX / 2 || l > LONG_MAX / 2) {
                addReplyError(c, "value is out of range");
                return;
            }
        }
        hrandfieldWithCountCommand(c, l, withvalues);

        return;
    }

    /* Handle variant without <count> argument. Reply with simple bulk string */
    if ((hash = lookupKeyReadOrReply(c, c->argv[1], shared.null[c->resp])) == NULL || checkType(c, hash, OBJ_HASH)) {
        return;
    }
    hashTypeRandomElement(hash, hashTypeLength(hash), &ele, NULL);
    hashReplyFromListpackEntry(c, &ele);
}
