#include "server.h"
#include "serverassert.h"
#include "entry.h"

/*-----------------------------------------------------------------------------
 * Entry Implementation
 *----------------------------------------------------------------------------*/

/* There are 3 different formats for the "entry".  In all cases, the "entry" pointer points into the
 * allocation and is identical to the "field" sds pointer.
 *
 * Type 1: Field sds type is an SDS_TYPE_5
 *     With this type, both the key and value are embedded in the entry.  Expiration is not allowed
 *     as the SDS_TYPE_5 (on field) doesn't contain any aux bits to encode the existence of an
 *     expiration.  Extra padding is included in the value to the size of the physical block.
 *
 *             entry
 *               |
 *     +---------V------------+----------------------------+
 *     |       Field          |      Value                 |
 *     | sdshdr5 | "foo" \0   | sdshdr8 "bar" \0 (padding) |
 *     +---------+------------+----------------------------+
 *
 *     Identified by: field sds type is SDS_TYPE_5
 *
 *
 * Type 2: Field sds type is an SDS_TYPE_8 type
 *     With this type, both the key and value are embedded.  Extra bits in the sdshdr8 (on field)
 *     are used to encode aux flags which may indicate the presence of an optional expiration.
 *     Extra padding is included in the value to the size of the physical block.
 *
 *                            entry
 *                              |
 *     +--------------+---------V------------+----------------------------+
 *     | Expire (opt) |       Field          |      Value                 |
 *     |  long long   | sdshdr8 | "foo" \0   | sdshdr8 "bar" \0 (padding) |
 *     +--------------+---------+------------+----------------------------+
 *
 *     Identified by: sds type is SDS_TYPE_8  AND  has embedded value
 *
 *
 * Type 3: Value is an sds, referenced by pointer
 *     With this type, the key is embedded, and the value is an sds, referenced by pointer.  Extra
 *     bits in the sdshdr8(+) are used to encode aux flags which indicate the presence of a value by
 *     pointer.  An aux bit may indicate the presence of an optional expiration.  Note that the
 *     "field" is not padded, so there's no direct way to identify the length of the allocation.
 *
 *                                             entry
 *                                               |
 *     +--------------+---------------+----------V----------+--------+
 *     | Expire (opt) |     Value     |        Field        | / / / /|
 *     |  long long   | sds (pointer) | sdshdr8+ | "foo" \0 |/ / / / |
 *     +--------------+-------+-------+----------+----------+--------+
 *                            |
 *                            +-> sds value
 *
 *
 * Type 4: Value is an stringRef, referenced by pointer
 *     With this type, the key is embedded, and the value is a stringRef, referenced by pointer.  Extra
 *     bits in the sdshdr8(+) are used to encode aux flags which indicate the presence of a value by
 *     pointer.  An aux bit may indicate the presence of an optional expiration.  Note that the
 *     "field" is not padded, so there's no direct way to identify the length of the allocation.
 *     This type is used when the entry's Value holds a buffer but doesn't own it, meaning
 *     freeing the entry doesn't involve freeing of the buffer.
 *
 *                                                   entry
 *                                                     |
 *     +--------------+---------------------+----------V----------+--------+
 *     | Expire (opt) |        Value        |        Field        | / / / /|
 *     |  long long   | stringRef (pointer) | sdshdr8+ | "foo" \0 |/ / / / |
 *     +--------------+----------+----------+----------+----------+--------+
 *                               |
 *                               |
 *                               +-> stringRef value
 *                                              |
 *                                   +----------V-----------+
 *                                   | buffer   | buffer    |
 *                                   | pointer  | length    |
 *                                   +----------+-----------+
 */

enum {
    /* SDS aux flag. If set, it indicates that the entry has TTL metadata set. */
    FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY = 0,
    /* SDS aux flag. If set, it indicates that the entry has an embedded value
     * pointer located in memory before the embedded field. If unset, the entry
     * instead has an embedded value located after the embedded field. */
    FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR = 1,
    /* SDS aux flag.  If set, it indicates that the hash entry has a reference to the value.
     * The hash entry does not own the string reference and will not free it upon
     * entry destruction. The primary usecase is to avoid memory duplication
     * between the core and a module. */
    FIELD_SDS_AUX_BIT_ENTRY_HAS_STRING_REF = 2,
    FIELD_SDS_AUX_BIT_MAX
};
static_assert(FIELD_SDS_AUX_BIT_MAX < sizeof(char) - SDS_TYPE_BITS, "too many sds bits are used for entry metadata");

/* The entry pointer is the field sds, but that's an implementation detail. */
sds entryGetField(const entry *entry) {
    return (sds)entry;
}

/* Returns true in case the entry's value is not embedded in the entry.
 * Returns false otherwise. */
static bool entryHasValuePtr(const entry *entry) {
    return sdsGetAuxBit(entryGetField(entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR);
}

/* Returns true in case the entry's value is embedded in the entry.
 * Returns false otherwise. */
bool entryHasEmbeddedValue(const entry *entry) {
    return (!entryHasValuePtr(entry));
}

/* Returns true in case the entry holds a reference of the value.
 * Returns false otherwise. */
bool entryHasStringRef(const entry *entry) {
    return entryHasValuePtr(entry) && sdsGetAuxBit(entryGetField(entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_STRING_REF);
}
/* Returns true in case the entry has expiration timestamp.
 * Returns false otherwise. */
bool entryHasExpiry(const entry *entry) {
    return sdsGetAuxBit(entryGetField(entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY);
}

/* Returns the location of a pointer to a separately allocated value. Only for
 * an entry without an embedded value. */
static void **entryGetValueRef(const entry *entry) {
    serverAssert(entryHasValuePtr(entry));
    char *field_data = sdsAllocPtr(entryGetField(entry));
    field_data -= sizeof(void *);
    return (void **)field_data;
}

static sds *entryGetSdsValueRef(const entry *entry) {
    return (sds *)entryGetValueRef(entry);
}

static stringRef *entryGetStringRefRef(const entry *entry) {
    serverAssert(entryHasStringRef(entry));
    return (stringRef *)*entryGetValueRef(entry);
}

/* Returns the entry's value. */
char *entryGetValue(const entry *entry, size_t *len) {
    if (entryHasEmbeddedValue(entry)) {
        /* Skip field content, field null terminator and value sds8 hdr. */
        size_t offset = sdslen(entryGetField(entry)) + 1 + sdsHdrSize(SDS_TYPE_8);
        sds value = (char *)entry + offset;
        if (len) *len = sdslen(value);
        return value;
    }
    if (entryHasStringRef(entry)) {
        stringRef *string_ref = entryGetStringRefRef(entry);
        if (!string_ref) return NULL;
        if (len) *len = string_ref->len;
        return (char *)string_ref->buf;
    }
    sds *value_ref = entryGetSdsValueRef(entry);
    if (len) *len = sdslen(*value_ref);
    return *value_ref;
}

/* Frees the entry's non-embedded value.
 * If the value is a string reference (stringRef), only the entry's pointer
 * is freed, as the underlying string is not owned by this entry.
 * Otherwise, the value is a standard SDS and is fully freed. */
static void entryFreeValuePtr(entry *entry) {
    serverAssert(entryHasValuePtr(entry));
    void **value_ref = entryGetValueRef(entry);
    if (entryHasStringRef(entry)) {
        zfree(*value_ref);
    } else {
        sdsfree(*value_ref);
    }
    *value_ref = NULL;
}

static void entrySetValueSds(entry *e, sds value) {
    serverAssert(entryHasValuePtr(e));
    entryFreeValuePtr(e);
    if (entryHasStringRef(e)) sdsSetAuxBit(entryGetField(e), FIELD_SDS_AUX_BIT_ENTRY_HAS_STRING_REF, 0);
    sds *value_ref = entryGetSdsValueRef(e);
    *value_ref = value;
}
/* Returns the address of the entry allocation. */
static void *entryGetAllocPtr(const entry *entry) {
    char *buf = sdsAllocPtr(entryGetField(entry));
    if (entryHasValuePtr(entry)) buf -= sizeof(void *);
    if (entryHasExpiry(entry)) buf -= sizeof(long long);
    return buf;
}

/**************************************** Entry Expiry API *****************************************/
/* Returns the location of a pointer to the expiry */
static long long *entryGetExpiryRef(const entry *entry) {
    debugServerAssert(entryHasExpiry(entry));
    char *buf = entryGetAllocPtr(entry);
    return (long long *)buf;
}

/* Returns the entry expiration timestamp.
 * In case this entry has no expiration time, will return EXPIRE_NONE. */
long long entryGetExpiry(const entry *entry) {
    if (entryHasExpiry(entry)) return *entryGetExpiryRef(entry);
    return EXPIRY_NONE;
}

/* Modify the expiration time of this entry and return a pointer to the (potentially new) entry. */
entry *entrySetExpiry(entry *e, long long expiry) {
    if (expiry != EXPIRY_NONE && entryHasExpiry(e)) {
        *entryGetExpiryRef(e) = expiry;
        return e;
    }
    if (entryHasStringRef(e)) {
        stringRef *value = entryGetStringRefRef(e);
        return entryUpdateAsStringRef(e, value->buf, value->len, expiry);
    }
    return entryUpdate(e, NULL, expiry);
}

/* Return true in case the entry has assigned expiration or false otherwise. */
bool entryIsExpired(entry *entry) {
    return timestampIsExpired(entryGetExpiry(entry));
}
/**************************************** Entry Expiry API - End *****************************************/
void entryFree(entry *entry) {
    if (entryHasValuePtr(entry)) entryFreeValuePtr(entry);
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

/* Serialize the content of the entry into an allocated buffer buf.
 * Note that this function will take ownership of the value so user should not assume it is valid after this call. */
static entry *entryConstruct(size_t alloc_size,
                             const_sds field,
                             sds sds_value,
                             stringRef *stringref_value,
                             long long expiry,
                             bool embed_value,
                             int embedded_field_sds_type,
                             size_t expiry_size,
                             size_t embedded_value_sds_size,
                             size_t embedded_field_sds_size) {
    serverAssert((sds_value == NULL && stringref_value == NULL && embed_value) || (sds_value != NULL && stringref_value == NULL) || (sds_value == NULL && stringref_value != NULL && !embed_value));
    size_t buf_size;
    /* allocate the buffer */
    char *buf = zmalloc_usable(alloc_size, &buf_size);

    /* Set The expiry if exists */
    if (expiry_size) {
        *(long long *)buf = expiry;
        buf += expiry_size;
        buf_size -= expiry_size;
    }
    if (!embed_value) {
        *(void **)buf = sds_value ? (void *)sds_value : (void *)stringref_value;
        buf += sizeof(void *);
        buf_size -= sizeof(void *);
    } else if (sds_value) {
        sdswrite(buf + embedded_field_sds_size, buf_size - embedded_field_sds_size, SDS_TYPE_8, sds_value, sdslen(sds_value));
        sdsfree(sds_value);
        buf_size -= embedded_value_sds_size;
    }
    /* Set the field data.  When we write the field into the buffer, the entry pointer is the returned
     * sds (after the sds header). */
    entry *new_entry = (entry *)sdswrite(buf, embedded_field_sds_size, embedded_field_sds_type, field, sdslen(field));

    /* Field sds aux bits are zero, which we use for this entry encoding. */
    sdsSetAuxBit(entryGetField(new_entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR, embed_value ? 0 : 1);
    sdsSetAuxBit(entryGetField(new_entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY, expiry_size > 0 ? 1 : 0);

    /* Check that the new entry was built correctly */
    debugServerAssert(sdsGetAuxBit(entryGetField(new_entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR) == (embed_value ? 0 : 1));
    debugServerAssert(sdsGetAuxBit(entryGetField(new_entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY) == (expiry_size > 0 ? 1 : 0));
    return new_entry;
}

/* Takes ownership of value. does not take ownership of field */
entry *entryCreate(const_sds field, sds value, long long expiry) {
    bool embed_value = false;
    int embedded_field_sds_type;
    size_t expiry_size, embedded_value_sds_size, embedded_field_sds_size;
    size_t value_len = value ? sdslen(value) : SIZE_MAX;
    size_t alloc_size = entryReqSize(sdslen(field), value_len, expiry, &embed_value, &embedded_field_sds_type, &embedded_field_sds_size, &expiry_size, &embedded_value_sds_size);
    return entryConstruct(alloc_size, field, value, NULL, expiry, embed_value, embedded_field_sds_type, expiry_size, embedded_value_sds_size, embedded_field_sds_size);
}

/* Sets the entry's value to a string reference object.
 * The reference points to the provided `buf` but does not assume ownership.
 * It is assumed that an external mechanism will handle releasing any memory which
 * may have been associated with value->buf */
entry *entryUpdateAsStringRef(entry *e, const char *buf, size_t len, long long expiry) {
    long long entry_expiry = entryGetExpiry(e);
    // Check for toggling expiration
    bool expiry_add_remove = (expiry != entry_expiry) && (entry_expiry == EXPIRY_NONE || expiry == EXPIRY_NONE);
    if (entryHasValuePtr(e) && !expiry_add_remove) {
        if (entryHasStringRef(e)) {
            stringRef *value = entryGetStringRefRef(e);
            value->buf = buf;
            value->len = len;
        } else {
            stringRef *value = zmalloc(sizeof(stringRef));
            value->buf = buf;
            value->len = len;
            sds *value_ref = entryGetSdsValueRef(e);
            sdsfree(*value_ref);
            *value_ref = (sds)value;
            sdsSetAuxBit(entryGetField(e), FIELD_SDS_AUX_BIT_ENTRY_HAS_STRING_REF, 1);
        }
        if (expiry != EXPIRY_NONE) *entryGetExpiryRef(e) = expiry;
        return e;
    }
    stringRef *value = zmalloc(sizeof(stringRef));
    value->buf = buf;
    value->len = len;
    sds field = entryGetField(e);
    size_t field_size = sdsReqSize(sdslen(field), SDS_TYPE_8);
    size_t alloc_size = field_size + sizeof(void *);
    alloc_size += (expiry == EXPIRY_NONE) ? 0 : sizeof(expiry);

    size_t expiry_size = 0;
    if (expiry != EXPIRY_NONE) expiry_size = sizeof(expiry);
    entry *new_entry = entryConstruct(alloc_size, field, NULL, value, expiry, false, SDS_TYPE_8, expiry_size, sizeof(value), field_size);
    entryFree(e);

    sdsSetAuxBit(entryGetField(new_entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_STRING_REF, 1);
    return new_entry;
}
/* Modify the entry's value and/or expiration time.
 * In case the provided value is NULL, will use the existing value.
 * Note that the value ownership is moved to this function and the caller should assume the
 * value is no longer usable after calling this function. */
entry *entryUpdate(entry *e, sds value, long long expiry) {
    sds field = entryGetField(e);
    entry *new_entry = NULL;

    /* Update just the expiry field, no value change, of a string ref entry */
    if (entryHasStringRef(e) && !value) {
        stringRef *value = entryGetStringRefRef(e);
        return entryUpdateAsStringRef(e, value->buf, value->len, expiry);
    }
    bool update_value = value ? true : false;
    long long curr_expiration_time = entryGetExpiry(e);
    bool update_expiry = (expiry != curr_expiration_time) ? true : false;
    /* Just a sanity check. If nothing changes, lets just return */
    if (!update_value && !update_expiry) return e;
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
    size_t current_embedded_allocation_size = entryHasEmbeddedValue(e) ? entryMemUsage(e) : 0;

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
            if (entryHasEmbeddedValue(e)) {
                /* Skip field content, field null terminator and value sds8 hdr. */
                char *old_value = entryGetValue(e, NULL);
                /* We are using the same entry memory in order to store a potentially new value.
                 * In such cases the old value alloc was adjusted to the real buffer size part it was embedded to.
                 * Since we can potentially write here a smaller value, which requires less allocation space, we would like to
                 * inherit the old value memory allocation size. */
                size_t value_size = sdsHdrSize(SDS_TYPE_8) + sdsalloc(old_value) + 1;
                sdswrite(sdsAllocPtr(old_value), value_size, SDS_TYPE_8, value, sdslen(value));
                sdsfree(value);
            } else {
                entrySetValueSds(e, value);
            }
        }
        new_entry = e;

    } else {
        if (!update_value) {
            /* Check if the value can be reused. In case the original entry value is
             * embedded WE WILL HAVE TO DUPLICATE IT if not we have to duplicate it,
             * remove it from the original entry since we are going to delete it. */
            if (entryHasEmbeddedValue(e)) {
                value = sdsdup(value);
            } else {
                void **value_ref = entryGetValueRef(e);
                *value_ref = NULL;
            }
        }
        /* allocate the buffer for a new entry */
        new_entry = entryConstruct(required_entry_size, field, value, NULL, expiry, embed_value, embedded_field_sds_type, expiry_size, embedded_value_size, embedded_field_size);
        entryFree(e);
    }
    /* Check that the new entry was built correctly */
    debugServerAssert(sdsGetAuxBit(entryGetField(new_entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_VALUE_PTR) == (embed_value ? 0 : 1));
    debugServerAssert(sdsGetAuxBit(entryGetField(new_entry), FIELD_SDS_AUX_BIT_ENTRY_HAS_EXPIRY) == (expiry_size > 0 ? 1 : 0));
    serverAssert(new_entry);
    return new_entry;
}

/* Returns memory usage of a entry, including all allocations owned by
 * the entry. */
size_t entryMemUsage(entry *entry) {
    size_t mem = 0;

    if (entryHasEmbeddedValue(entry)) {
        mem += sdsReqSize(sdslen(entryGetField(entry)), sdsType(entryGetField(entry)));
        if (entryHasExpiry(entry)) mem += sizeof(long long);
    } else {
        /* In case the value is not embedded we might not be able to sum all the allocation sizes since the field
         * header could be too small for holding the real allocation size. */
        mem += zmalloc_usable_size(entryGetAllocPtr(entry));
    }
    if (entryHasStringRef(entry)) {
        mem += zmalloc_usable_size(entryGetStringRefRef(entry));
    } else {
        mem += sdsAllocSize((sds)entryGetValue(entry, NULL));
    }
    return mem;
}

/* Defragments a hashtable entry (field-value pair) if needed, using the
 * provided defrag functions. The defrag functions return NULL if the allocation
 * was not moved, otherwise they return a pointer to the new memory location.
 * A separate sds defrag function is needed because of the unique memory layout
 * of sds strings.
 * If the location of the entry changed we return the new location,
 * otherwise we return NULL. */
entry *entryDefrag(entry *e, void *(*defragfn)(void *), sds (*sdsdefragfn)(sds)) {
    if (entryHasStringRef(e)) {
        stringRef **value_ref = (stringRef **)entryGetValueRef(e);
        stringRef *new_value = defragfn(*value_ref);
        if (new_value) *value_ref = new_value;
    } else if (entryHasValuePtr(e)) {
        sds *value_ref = (sds *)entryGetValueRef(e);
        sds new_value = sdsdefragfn(*value_ref);
        if (new_value) *value_ref = new_value;
    }
    char *allocation = entryGetAllocPtr(e);
    char *new_allocation = defragfn(allocation);
    if (new_allocation != NULL) {
        /* Return the same offset into the new allocation as the entry's offset
         * in the old allocation. */
        int entry_pointer_offset = (char *)e - allocation;
        return (entry *)(new_allocation + entry_pointer_offset);
    }
    return NULL;
}

/* Used for releasing memory to OS to avoid unnecessary CoW. Called when we've
 * forked and memory won't be used again. See zmadvise_dontneed() */
void entryDismissMemory(entry *entry) {
    /* Only dismiss values memory since the field size usually is small. */
    if (entryHasValuePtr(entry) && !entryHasStringRef(entry)) dismissSds(*entryGetValueRef(entry));
}
