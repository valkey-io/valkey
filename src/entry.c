#include <stdbool.h>
#include "server.h"
#include "serverassert.h"
#include "entry.h"

#include <stdbool.h>

/*-----------------------------------------------------------------------------
 * Entry API
 *----------------------------------------------------------------------------*/

/* The entry pointer is the field sds. We encode the entry layout type
 * in the field SDS header. Field type SDS_TYPE_5 doesn't have any spare bits to
 * encode this so we use it only for the first layout type.
 *
 * Entry with embedded value, used for small sizes. The value is stored as
 * SDS_TYPE_8. The field can use any SDS type.
 *
 * Entry can also have expiration timestamp, which is the UNIX timestamp for it to be expired.
 * For aligned fast access, we keep the expiry timestamp prior to the start of the sds header.
 *
 *     +--------------+--------------+---------------+
 *     | Expiration   | field        | value         |
 *     | 1234567890LL | hdr "foo" \0 | hdr8 "bar" \0 |
 *     +--------------+--------------+---------------+
 *
 * Entry with value pointer, used for larger fields and values. The field is SDS
 * type 8 or higher.
 *
 *     +--------------+-------+--------------+
 *     | Expiration   | value | field        |
 *     | 1234567890LL | ptr   | hdr "foo" \0 |
 *     +--------------+---^---+--------------+
 *                        |
 *                        |
 *                        entry pointer = value sds
 */

/* SDS aux flag. If set, it indicates that the entry has TTL metadata set. */
#define FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY 0

/* SDS aux flag. If set, it indicates that the entry has an embedded value
 * pointer located in memory before the embedded field. If unset, the entry
 * instead has an embedded value located after the embedded field. */
#define FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR 2

/* Returns true in case the entry's value is not embedded in the entry.
 * Returns false otherwise. */
static inline bool entryHasValuePtr(const entry *entry) {
    return sdsGetAuxBit(entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR);
}

/* Returns true in case the entry has expiration timestamp.
 * Returns false otherwise. */
bool entryHasExpiry(const entry *entry) {
    return sdsGetAuxBit(entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY);
}

/* The entry pointer is the field sds, but that's an implementation detail. */
sds entryGetField(const entry *entry) {
    return (sds)entry;
}

/* Returns the location of a pointer to a separately allocated value. Only for
 * an entry without an embedded value. */
static sds *entryGetValueRef(const entry *entry) {
    serverAssert(entryHasValuePtr(entry));
    char *field_data = sdsAllocPtr(entry);
    field_data -= sizeof(sds *);
    return (sds *)field_data;
}

/* Returns the sds of the entry's value. */
sds entryGetValue(const entry *entry) {
    if (entryHasValuePtr(entry)) {
        return *entryGetValueRef(entry);
    } else {
        /* Skip field content, field null terminator and value sds8 hdr. */
        size_t offset = sdslen(entry) + 1 + sdsHdrSize(SDS_TYPE_8);
        return (char *)entry + offset;
    }
}

/* Modify the value of this entry and return a pointer to the (potentially new) entry.
 * The value is taken by the function and cannot be reused after this function returns. */
entry *entrySetValue(entry *e, sds value) {
    if (entryHasValuePtr(e)) {
        sds *value_ref = entryGetValueRef(e);
        sdsfree(*value_ref);
        *value_ref = value;
        return e;
    } else {
        entry *new_entry = entryUpdate(e, value, entryGetExpiry(e));
        return new_entry;
    }
}

/* Returns the address of the entry allocation. */
void *entryAllocPtr(const entry *entry) {
    char *buf = sdsAllocPtr(entry);
    if (entryHasValuePtr(entry)) buf -= sizeof(sds *);
    if (entryHasExpiry(entry)) buf -= sizeof(long long);
    return buf;
}

bool entryHasEmbeddedValue(entry *entry) {
    return (entryGetValue(entry) && !entryHasValuePtr(entry));
}

/**************************************** Entry Expiry API *****************************************/

/* Returns the entry expiration timestamp.
 * In case this entry has no expiration time, will return EXPIRE_NONE. */
long long entryGetExpiry(const entry *entry) {
    long long expiry = EXPIRY_NONE;
    if (entryHasExpiry(entry)) {
        char *buf = sdsAllocPtr(entry);
        debugServerAssert((((uintptr_t)buf & 0x7) == 0)); /* Test that the allocation is indeed 8 bytes aligned */
        if (entryHasValuePtr(entry)) buf -= sizeof(sds);
        buf -= sizeof(long long);
        expiry = *(long long *)buf;
    }
    return expiry;
}

/* Modify the expiration time of this entry and return a pointer to the (potentially new) entry. */
entry *entrySetExpiry(entry *e, long long expiry) {
    if (entryHasExpiry(e)) {
        char *buf = sdsAllocPtr(e);
        if (entryHasValuePtr(e)) buf -= sizeof(sds *);
        buf -= sizeof(expiry);
        memcpy(buf, &expiry, sizeof(expiry));
        return e;
    }
    entry *new_entry = entryUpdate(e, NULL, expiry);
    return new_entry;
}

/* Return true in case the entry has assigned expiration or false otherwise. */
bool entryIsExpired(entry *entry) {
    if (!timestampIsExpired(entryGetExpiry(entry))) return false;
    return true;
}
/**************************************** Entry Expiry API - End *****************************************/

void entryFree(entry *entry) {
    if (entryHasValuePtr(entry)) {
        sdsfree(entryGetValue(entry));
    }
    zfree(entryAllocPtr(entry));
}

/* Takes ownership of value. does not take ownership of field */
entry *entryCreate(const_sds field, sds value, long long expiry) {
    sds embedded_field_sds;
    size_t expiry_size = (expiry == EXPIRY_NONE) ? 0 : sizeof(long long);
    size_t field_len = sdslen(field);
    int field_sds_type = sdsReqType(field_len);
    if (field_sds_type == SDS_TYPE_5 && (expiry_size > 0)) {
        field_sds_type = SDS_TYPE_8;
    }
    size_t field_size = sdsReqSize(field_len, field_sds_type);
    size_t value_len = value ? sdslen(value) : 0;
    size_t embedded_value_size = value ? sdsReqSize(value_len, SDS_TYPE_8) : 0;
    size_t alloc_size = field_size + expiry_size;
    bool embed_value = false;
    if (value) {
        if (alloc_size + embedded_value_size <= EMBED_VALUE_MAX_ALLOC_SIZE) {
            /* Embed field and value. Value is fixed to SDS_TYPE_8. Unused
             * allocation space is recorded in the embedded value's SDS header.
             *
             *     +------+--------------+---------------+
             *     | TTL  | field        | value         |
             *     |      | hdr "foo" \0 | hdr8 "bar" \0 |
             *     +------+--------------+---------------+
             */
            embed_value = true;
            alloc_size += embedded_value_size;
        } else {
            /* Embed field, but not value. Field must be >= SDS_TYPE_8 to encode to
             * indicate this type of entry.
             *
             *     +------+-------+---------------+
             *     | TTL  | value | field         |
             *     |      | ptr   | hdr8 "foo" \0 |
             *     +------+-------+---------------+
             */
            embed_value = false;
            alloc_size += sizeof(sds);
            if (field_sds_type == SDS_TYPE_5) {
                field_sds_type = SDS_TYPE_8;
                alloc_size -= field_size;
                field_size = sdsReqSize(field_len, field_sds_type);
                alloc_size += field_size;
            }
        }
    }
    /* allocate the buffer */
    size_t buf_size;
    char *buf = zmalloc_usable(alloc_size, &buf_size);

    /* Set The expiry if exists */
    if (expiry_size) {
        memcpy(buf, &expiry, expiry_size);
        buf += expiry_size;
        buf_size -= expiry_size;
    }
    if (value) {
        if (!embed_value) {
            *(sds *)buf = value;
            buf += sizeof(sds);
            buf_size -= sizeof(sds);
        } else {
            sdswrite(buf + field_size, buf_size - field_size, SDS_TYPE_8, value, value_len);
            sdsfree(value);
            buf_size -= embedded_value_size;
        }
    }
    /* Set the field data */
    embedded_field_sds = sdswrite(buf, field_size, field_sds_type, field, field_len);

    /* Field sds aux bits are zero, which we use for this entry encoding. */
    sdsSetAuxBit(embedded_field_sds, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR, embed_value ? 0 : 1);
    sdsSetAuxBit(embedded_field_sds, FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY, expiry_size > 0 ? 1 : 0);
    return (void *)embedded_field_sds;
}

/* Modify the entry's value and/or expiration time.
 * In case the provided value is NULL, will use the existing value. */
entry *entryUpdate(entry *e, sds value, long long expiry) {
    sds field = (sds)e;

    bool update_value = value ? true : false;
    long long expiration_time = entryGetExpiry(e);
    bool update_expiry = (expiry != expiration_time) ? true : false;
    if (!update_value && !update_expiry)
        return e;
    expiration_time = expiry;
    value = update_value ? value : entryGetValue(e);
    size_t expiry_size = (expiration_time != EXPIRY_NONE) ? sizeof(expiration_time) : 0;
    int field_sds_type = sdsReqType(sdslen(field));
    if (field_sds_type == SDS_TYPE_5 && (expiry_size > 0)) {
        field_sds_type = SDS_TYPE_8;
    }
    size_t field_size = sdsHdrSize(field_sds_type) + sdsalloc(field) + 1;
    size_t value_len = value ? sdslen(value) : 0;
    size_t embedded_value_size = value ? sdsReqSize(value_len, SDS_TYPE_8) : 0;

    size_t required_embedded_size = field_size + embedded_value_size + expiry_size;
    size_t current_embedded_allocation_size = entryHasValuePtr(e) ? 0 : entryMemUsage(e);
    /* // We will create a new entry in the following cases:
     * 1. In the case were we add or remove expiration.
     * 2. in the case were we are NOT migrating from an embedded entry to an embedded entry with ~the same size. */
    bool create_new_entry = (update_expiry && (entryGetExpiry(e) == EXPIRY_NONE || expiration_time == EXPIRY_NONE)) ||
                            !(update_value && !entryHasValuePtr(e) &&
                              required_embedded_size <= EMBED_VALUE_MAX_ALLOC_SIZE &&
                              required_embedded_size <= current_embedded_allocation_size &&
                              required_embedded_size >= current_embedded_allocation_size * 3 / 4);

    if (!create_new_entry) {
        /* In this case we are sure we do not have to allocate new entry, so expiry must already be set. */
        if (update_expiry) {
            serverAssert(entryHasExpiry(e));
            char *buf = sdsAllocPtr(e);
            if (entryHasValuePtr(e)) buf -= sizeof(sds *);
            buf -= sizeof(expiry);
            memcpy(buf, &expiry, sizeof(expiry));
        }
        /* In this case we are sure we do not have to allocate new entry, so value must already be set or we have enough room to embed it. */
        if (update_value) {
            if (entryHasValuePtr(e)) {
                sds *value_ref = entryGetValueRef(e);
                sdsfree(*value_ref);
                *value_ref = value;
            } else {
                /* Skip field content, field null terminator and value sds8 hdr. */
                sds old_value = entryGetValue(e);
                /* We are using the same entry memory in order to store a potentially new value.
                 * In such cases the old value alloc was adjusted to the real buffer size part it was embedded to.
                 * since we can potentially write here a smaller value, which requires less allocation space, we would like to
                 * inherit the old value memory allocation size. */
                size_t value_size = sdsHdrSize(SDS_TYPE_8) + sdsalloc(old_value) + 1;
                sdswrite(sdsAllocPtr(old_value), value_size, SDS_TYPE_8, value, sdslen(value));
                sdsfree(value);
            }
        }
        return e;

    } else {
        if (!update_value) {
            /* Check if the value can be reused. */
            int value_was_embedded = !entryHasValuePtr(e);
            /* In case the original entry value is embedded WE WILL HAVE TO DUPLICATE IT */
            if (value_was_embedded)
                value = sdsdup(value);
            /* if not we have to duplicate it, remove it from the original entry since we are going to delete it.*/
            else {
                sds *value_ref = entryGetValueRef(e);
                *value_ref = NULL;
            }
        }
    }

    entry *new_entry = entryCreate(entryGetField(e), value, expiration_time);
    if (new_entry != e)
        entryFree(e);
    return new_entry;
}

/* Returns memory usage of a entry, including all allocations owned by
 * the entry. */
size_t entryMemUsage(entry *entry) {
    size_t mem = 0;

    if (entryHasValuePtr(entry)) {
        /* In case the value is not embedded we might not be able to sum all the allocation sizes since the field
         * header could be too small for holding the real allocation size. */
        mem += zmalloc_usable_size(entryAllocPtr(entry));
    } else {
        mem += sdsReqSize(sdslen(entry), sdsType(entry));
        if (entryHasExpiry(entry)) mem += sizeof(long long);
    }
    mem += sdsAllocSize(entryGetValue(entry));
    return mem;
}

/* Defragments a hashtable entry (field-value pair) if needed, using the
 * provided defrag functions. The defrag functions return NULL if the allocation
 * was not moved, otherwise they return a pointer to the new memory location.
 * A separate sds defrag function is needed because of the unique memory layout
 * of sds strings.
 * If the location of the entry changed we return the new location,
 * otherwise we return NULL. */
entry *entryDefrag(entry *entry, void *(*defragfn)(void *), sds (*sdsdefragfn)(sds)) {
    if (entryHasValuePtr(entry)) {
        sds *value_ref = entryGetValueRef(entry);
        sds new_value = sdsdefragfn(*value_ref);
        if (new_value) *value_ref = new_value;
    }
    char *allocation = entryAllocPtr(entry);
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
void entryDismissMemory(entry *entry) {
    /* Only dismiss values memory since the field size usually is small. */
    if (entryHasValuePtr(entry)) {
        dismissSds(*entryGetValueRef(entry));
    }
}
