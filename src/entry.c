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
 *                        value pointer = value sds
 */

enum {
    /* SDS aux flag. If set, it indicates that the entry has TTL metadata set. */
    FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY = 0,
    /* SDS aux flag. If set, it indicates that the entry has an embedded value
     * pointer located in memory before the embedded field. If unset, the entry
     * instead has an embedded value located after the embedded field. */
    FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR = 1,
    /* SDS aux flag. If set, it indicates that the hash entry's value is a StringViewValue,
     * meaning it's a char* and size_t managed externally by a module.
     * This bit is mutually exclusive with the actual value being a normal sds. */
    FIELD_SDS_AUX_BIT_VIEW_VALUE = 2,
    FIELD_SDS_AUX_BIT_MAX
};
static_assert(FIELD_SDS_AUX_BIT_MAX < sizeof(char) - SDS_TYPE_BITS, "too many sds bits are used for entry metadata");

/* Returns true in case the entry's value is not embedded in the entry.
 * Returns false otherwise. */
static inline bool entryHasValuePtr(const entry *entry) {
    return sdsGetAuxBit(entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR);
}

/* Returns true in case the entry's value is embedded in the entry.
 * Returns false otherwise. */
bool entryHasEmbeddedValue(entry *entry) {
    return (!entryHasValuePtr(entry));
}

/* Returns true in case the entry holds a view of the value.
 * Returns false otherwise. */
bool entryIsStringViewValue(const entry *e) {
    return sdsGetAuxBit(e, FIELD_SDS_AUX_BIT_VIEW_VALUE);
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

/* Create an entry for a view value.
 * The 'value' (buf and len) is not owned by hash but keeps just a view of the
 * buffer. */
entry *createStringViewEntry(sds field, const char *buf, size_t len) {
    StringViewValue *ext_value = zmalloc(sizeof(StringViewValue));
    ext_value->buf = buf;
    ext_value->len = len;

    size_t field_len = sdslen(field);
    char field_sds_type = sdsReqType(field_len);
    if (field_sds_type == SDS_TYPE_5) field_sds_type = SDS_TYPE_8; // Ensure we can set aux bits
    size_t field_size = sdsReqSize(field_len, field_sds_type);

    size_t alloc_size = sizeof(void *) + field_size; // Store pointer to StringViewValue
    char *alloc_buf = zmalloc(alloc_size);

    *(void **)alloc_buf = ext_value; // Store the pointer to our struct

    sds embedded_field_sds = sdswrite(alloc_buf + sizeof(void *), field_size, field_sds_type, field, field_len);

    sdsSetAuxBit(embedded_field_sds, FIELD_SDS_AUX_BIT_VIEW_VALUE, 1);          // Mark as view value
    serverAssert(entryIsStringViewValue(embedded_field_sds));

    return (entry *)embedded_field_sds;
}
/* Returns the location of a pointer to a separately allocated value. Only for
 * an entry without an embedded value. */
static sds *entryGetValueRef(const entry *entry) {
    serverAssert(entryHasValuePtr(entry));
    char *field_data = sdsAllocPtr(entry);
    field_data -= sizeof(sds);
    return (sds *)field_data;
}

StringViewValue *entryGetViewValueRef(const entry *entry) {
    serverAssert(entryIsStringViewValue(entry));
    char *field_data = sdsAllocPtr(entry);
    field_data -= sizeof(StringViewValue);
    return (StringViewValue *)field_data;
}

/* Returns the entry's value. */
char *entryGetValue(const entry *entry, size_t *len) {
    if (entryIsStringViewValue(entry)) {
        serverAssert(entryHasValuePtr(entry)); // StringView must use value pointer
        StringViewValue *ext_value = entryGetViewValueRef(entry);
        *len = ext_value->len;
        return (char *)ext_value->buf;
    }
    if (entryHasValuePtr(entry)) {
        sds *value = entryGetValueRef(entry);
        *len = sdslen(*value);
        return *value;
    }
    /* Skip field content, field null terminator and value sds8 hdr. */
    size_t offset = sdslen(entry) + 1 + sdsHdrSize(SDS_TYPE_8);
    sds value = (char *)entry + offset;
    *len = sdslen(value);
    return value;
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
void *entryGetAllocPtr(const entry *entry) {
    char *buf = sdsAllocPtr(entry);
    if (entryIsStringViewValue(entry)) {
        buf -= sizeof(StringViewValue *);
    } else if (entryHasValuePtr(entry)) {
        buf -= sizeof(sds);
    }
    if (entryHasExpiry(entry)) buf -= sizeof(long long);
    return buf;
}

/**************************************** Entry Expiry API *****************************************/

/* Returns the entry expiration timestamp.
 * In case this entry has no expiration time, will return EXPIRE_NONE. */
long long entryGetExpiry(const entry *entry) {
    long long expiry = EXPIRY_NONE;
    if (entryHasExpiry(entry)) {
        char *buf = entryGetAllocPtr(entry);
        debugServerAssert((((uintptr_t)buf & 0x7) == 0)); /* Test that the allocation is indeed 8 bytes aligned
                                                           * This is needed since we access the expiry as with pointer casting
                                                           * which require the access to be 8 bytes aligned. */
        expiry = *(long long *)buf;
    }
    return expiry;
}

/* Modify the expiration time of this entry and return a pointer to the (potentially new) entry. */
entry *entrySetExpiry(entry *e, long long expiry) {
    if (entryHasExpiry(e)) {
        char *buf = entryGetAllocPtr(e);
        debugServerAssert((((uintptr_t)buf & 0x7) == 0)); /* Test that the allocation is indeed 8 bytes aligned
                                                           * This is needed since we access the expiry as with pointer casting
                                                           * which require the access to be 8 bytes aligned. */
        *(long long *)buf = expiry;
        return e;
    }
    entry *new_entry = entryUpdate(e, NULL, expiry);
    return new_entry;
}

/* Return true in case the entry has assigned expiration or false otherwise. */
bool entryIsExpired(entry *entry) {
    return timestampIsExpired(entryGetExpiry(entry));
}
/**************************************** Entry Expiry API - End *****************************************/

void entryFree(entry *entry) {
  /*
    if (entryIsStringViewValue(entry)) {
        StringViewValue *ext_value = entryGetViewValueRef(entry);
        zfree(ext_value);
    } else */
    if (entryHasValuePtr(entry)) {
        size_t len;
        sdsfree(entryGetValue(entry, &len));
    }
    /*else if (entryHasValuePtr(entry)) {
        sdsfree(*entryGetValueRef(entry));
    }
    */
    zfree(entryGetAllocPtr(entry));
}

static inline size_t entryReqSize(size_t field_len,
                                  size_t value_len,
                                  long long expiry,
                                  bool *is_value_embedded,
                                  int *field_sds_type,
                                  size_t *field_size,
                                  size_t *expiry_size,
                                  size_t *embedded_value_size) {
    size_t expiry_alloc_size = (expiry == EXPIRY_NONE) ? 0 : sizeof(long long);
    int embedded_field_sds_type = sdsReqType(field_len);
    if (embedded_field_sds_type == SDS_TYPE_5 && (expiry_alloc_size > 0)) {
        embedded_field_sds_type = SDS_TYPE_8;
    }
    size_t field_alloc_size = sdsReqSize(field_len, embedded_field_sds_type);
    size_t embedded_value_alloc_size = value_len != SIZE_MAX ? sdsReqSize(value_len, SDS_TYPE_8) : 0;
    size_t alloc_size = field_alloc_size + expiry_alloc_size;
    bool embed_value = false;
    if (value_len != SIZE_MAX) {
        if (alloc_size + embedded_value_alloc_size <= EMBED_VALUE_MAX_ALLOC_SIZE) {
            /* Embed field and value. Value is fixed to SDS_TYPE_8. Unused
             * allocation space is recorded in the embedded value's SDS header.
             *
             *     +------+--------------+---------------+
             *     | TTL  | field        | value         |
             *     |      | hdr "foo" \0 | hdr8 "bar" \0 |
             *     +------+--------------+---------------+
             */
            embed_value = true;
            alloc_size += embedded_value_alloc_size;
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
            if (embedded_field_sds_type == SDS_TYPE_5) {
                embedded_field_sds_type = SDS_TYPE_8;
                alloc_size -= field_alloc_size;
                field_alloc_size = sdsReqSize(field_len, embedded_field_sds_type);
                alloc_size += field_alloc_size;
            }
        }
    }
    if (expiry_size) *expiry_size = expiry_alloc_size;
    if (field_sds_type) *field_sds_type = embedded_field_sds_type;
    if (field_size) *field_size = field_alloc_size;
    if (embedded_value_size) *embedded_value_size = embedded_value_alloc_size;
    if (is_value_embedded) *is_value_embedded = embed_value;

    return alloc_size;
}

/* Serialize the content of the entry into the provided buffer buf. Make use of the provided arguments provided by a call to entryReqSize.
 * Note that this function will take ownership of the value so user should not assume it is valid after this call. */
static entry *entryWrite(char *buf,
                         size_t buf_size,
                         const_sds field,
                         size_t field_len,
                         sds value,
                         long long expiry,
                         bool embed_value,
                         int embedded_field_sds_type,
                         size_t embedded_field_sds_size,
                         size_t embedded_value_sds_size,
                         size_t expiry_size) {
    /* Set The expiry if exists */
    if (expiry_size) {
        *(long long *)buf = expiry;
        buf += expiry_size;
        buf_size -= expiry_size;
    }
    if (value) {
        if (!embed_value) {
            *(sds *)buf = value;
            buf += sizeof(sds);
            buf_size -= sizeof(sds);
        } else {
            sdswrite(buf + embedded_field_sds_size, buf_size - embedded_field_sds_size, SDS_TYPE_8, value, sdslen(value));
            sdsfree(value);
            buf_size -= embedded_value_sds_size;
        }
    }
    /* Set the field data */
    entry *new_entry = sdswrite(buf, embedded_field_sds_size, embedded_field_sds_type, field, field_len);

    /* Field sds aux bits are zero, which we use for this entry encoding. */
    sdsSetAuxBit(new_entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR, embed_value ? 0 : 1);
    sdsSetAuxBit(new_entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY, expiry_size > 0 ? 1 : 0);

    /* Check that the new entry was built correctly */
    debugServerAssert(sdsGetAuxBit(new_entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR) == (embed_value ? 0 : 1));
    debugServerAssert(sdsGetAuxBit(new_entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY) == (expiry_size > 0 ? 1 : 0));
    return new_entry;
}

/* Takes ownership of value. does not take ownership of field */
entry *entryCreate(const char *field, size_t field_len, sds value, long long expiry) {
    bool embed_value = false;
    int embedded_field_sds_type;
    size_t expiry_size, embedded_value_sds_size, embedded_field_sds_size;
    size_t value_len = value ? sdslen(value) : SIZE_MAX;
    size_t alloc_size = entryReqSize(field_len, value_len, expiry, &embed_value, &embedded_field_sds_type, &embedded_field_sds_size, &expiry_size, &embedded_value_sds_size);
    size_t buf_size;

    /* allocate the buffer */
    char *buf = zmalloc_usable(alloc_size, &buf_size);

    return entryWrite(buf, buf_size, field, field_len, value, expiry, embed_value, embedded_field_sds_type, embedded_field_sds_size, embedded_value_sds_size, expiry_size);
}

/* Modify the entry's value and/or expiration time.
 * In case the provided value is NULL, will use the existing value.
 * Note that the value ownership is moved to this function and the caller should assume the
 * value is no longer usable after calling this function. */
entry *entryUpdate(entry *e, sds value, long long expiry) {
    sds field = (sds)e;
    entry *new_entry = NULL;

    bool update_value = value ? true : false;
    long long curr_expiration_time = entryGetExpiry(e);
    bool update_expiry = (expiry != curr_expiration_time) ? true : false;
    /* Just a sanity check. If nothing changes, lets just return */
    if (!update_value && !update_expiry)
        return e;
    size_t value_len = SIZE_MAX;
    if (value) {
        value_len = sdslen(value);
    } else {
        value = entryGetValue(e, &value_len);
    }
    bool embed_value = false;
    int embedded_field_sds_type;
    size_t expiry_size, embedded_value_size, embedded_field_size;
    size_t required_entry_size = entryReqSize(sdslen(field), value_len, expiry, &embed_value, &embedded_field_sds_type, &embedded_field_size, &expiry_size, &embedded_value_size);
    size_t current_embedded_allocation_size = entryHasValuePtr(e) ? 0 : entryMemUsage(e);

    bool expiry_add_remove = update_expiry && (curr_expiration_time == EXPIRY_NONE || expiry == EXPIRY_NONE); // In case we are toggling expiration
    bool value_change_encoding = update_value && (embed_value != entryHasEmbeddedValue(e));                   // In case we change the way value is embedded or not


    /* We will create a new entry in the following cases:
     * 1. In the case were we add or remove expiration.
     * 2. We change the way value is encoded
     * 3. in the case were we are NOT migrating from an embedded entry to an embedded entry with ~the same size. */
    bool create_new_entry = (expiry_add_remove) || (value_change_encoding) ||
                            (update_value && entryHasEmbeddedValue(e) &&
                             !(required_entry_size <= EMBED_VALUE_MAX_ALLOC_SIZE &&
                               required_entry_size <= current_embedded_allocation_size &&
                               required_entry_size >= current_embedded_allocation_size * 3 / 4));

    if (!create_new_entry) {
        /* In this case we are sure we do not have to allocate new entry, so expiry must already be set. */
        if (update_expiry) {
            serverAssert(entryHasExpiry(e));
            char *buf = entryGetAllocPtr(e);
            *(long long *)buf = expiry;
        }
        /* In this case we are sure we do not have to allocate new entry, so value must already be set or we have enough room to embed it. */
        if (update_value) {
            if (entryHasValuePtr(e)) {
                sds *value_ref = entryGetValueRef(e);
                sdsfree(*value_ref);
                *value_ref = value;
            } else {
                /* Skip field content, field null terminator and value sds8 hdr. */
                size_t len;
                char *old_value = entryGetValue(e, &len);
                /* We are using the same entry memory in order to store a potentially new value.
                 * In such cases the old value alloc was adjusted to the real buffer size part it was embedded to.
                 * Since we can potentially write here a smaller value, which requires less allocation space, we would like to
                 * inherit the old value memory allocation size. */
                size_t value_size = sdsHdrSize(SDS_TYPE_8) + sdsalloc(old_value) + 1;
                sdswrite(sdsAllocPtr(old_value), value_size, SDS_TYPE_8, value, sdslen(value));
                sdsfree(value);
                sdsSetAuxBit(e, FIELD_SDS_AUX_BIT_VIEW_VALUE, 0);
            }
        }
        new_entry = e;

    } else {
        if (!update_value) {
            /* Check if the value can be reused. */
            int value_was_embedded = !entryHasValuePtr(e);
            /* In case the original entry value is embedded WE WILL HAVE TO DUPLICATE IT
             * if not we have to duplicate it, remove it from the original entry since we are going to delete it.*/
            if (value_was_embedded) {
                value = sdsdup(value);
            } else {
                sds *value_ref = entryGetValueRef(e);
                *value_ref = NULL;
            }
        }
        /* allocate the buffer for a new entry */
        size_t buf_size;
        char *buf = zmalloc_usable(required_entry_size, &buf_size);
        new_entry = entryWrite(buf, buf_size, field, sdslen(field), value, expiry, embed_value, embedded_field_sds_type, embedded_field_size, embedded_value_size, expiry_size);
        debugServerAssert(new_entry != e);
        entryFree(e);
    }
    /* Check that the new entry was built correctly */
    debugServerAssert(sdsGetAuxBit(new_entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR) == (embed_value ? 0 : 1));
    debugServerAssert(sdsGetAuxBit(new_entry, FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY) == (expiry_size > 0 ? 1 : 0));
    debugServerAssert(sdsGetAuxBit(new_entry, FIELD_SDS_AUX_BIT_VIEW_VALUE) == 0);
    serverAssert(new_entry);
    return new_entry;
}

/* Returns memory usage of a entry, including all allocations owned by
 * the entry. */
size_t entryMemUsage(entry *entry) {
    if (entryIsStringViewValue(entry)) {
        size_t mem = zmalloc_usable_size(entryGetAllocPtr(entry));
        StringViewValue *ext_value = entryGetViewValueRef(entry);
        mem += zmalloc_usable_size(ext_value);
        return mem;
    }

    size_t mem = 0;
    if (entryHasValuePtr(entry)) {
        /* In case the value is not embedded we might not be able to sum all the allocation sizes since the field
         * header could be too small for holding the real allocation size. */
        mem += zmalloc_usable_size(entryGetAllocPtr(entry));
    } else {
        mem += sdsReqSize(sdslen(entry), sdsType(entry));
        if (entryHasExpiry(entry)) mem += sizeof(long long);
    }
    size_t len;
    mem += sdsAllocSize((sds)entryGetValue(entry, &len));
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
    if (entryIsStringViewValue(entry)) return NULL;
    if (entryHasValuePtr(entry)) {
        sds *value_ref = entryGetValueRef(entry);
        sds new_value = sdsdefragfn(*value_ref);
        if (new_value) *value_ref = new_value;
    }
    char *allocation = entryGetAllocPtr(entry);
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
    if (entryIsStringViewValue(entry)) return;
    /* Only dismiss values memory since the field size usually is small. */
    if (entryHasValuePtr(entry)) {
        dismissSds(*entryGetValueRef(entry));
    }
}
