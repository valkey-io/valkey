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
#include "vset.h"
#include "server.h"
#include "zmalloc.h"
#include <math.h>
#include <string.h>
#include "entry.h"

/* enumeration of all the possible return values of commands manipulating fields expiration. */
typedef enum {
    /* SDS aux flag. If set, it indicates that the entry has TTL metadata set. */
    EXPIRATION_MODIFICATION_NOT_EXIST = -2,       /* in case the provided object is NULL or the specific field was not found */
    EXPIRATION_MODIFICATION_SUCCESSFUL = 1,       /* if the expiration time was applied or modified */
    EXPIRATION_MODIFICATION_FAILED_CONDITION = 0, /* if the some predefined conditions (e.g hexpire conditional flags) has not been met */
    EXPIRATION_MODIFICATION_FAILED = -1,          /* if apply of the expiration modification failed (e.g hpersist on item without expiration) */
    EXPIRATION_MODIFICATION_EXPIRE_ASAP = 2,      /* if apply of the expiration modification was set to a time in the past (i.e field is immediately expired) */
} expiryModificationResult;

// A vsetGetExpiryFunc
static long long entryGetExpiryVsetFunc(const void *e) {
    return entryGetExpiry((const entry *)e);
}

/*-----------------------------------------------------------------------------
 * Hash type Expiry API
 *----------------------------------------------------------------------------*/

static vset *hashTypeGetVolatileSet(robj *o) {
    serverAssert(o->encoding == OBJ_ENCODING_HASHTABLE);
    vset *set = (vset *)hashtableMetadata(o->ptr);
    return vsetIsValid(set) ? set : NULL;
}

bool hashTypeHasVolatileFields(robj *o) {
    if (o == NULL) return false;
    serverAssert(o->type == OBJ_HASH);
    if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        vset *set = hashTypeGetVolatileSet(o);
        if (set && !vsetIsEmpty(set))
            return true;
    }
    return false;
}

/* make any access to the hash object elements ignore the specific elements expiration.
 * This is mainly in order to be able to access hash elements which are already expired. */
static inline void hashTypeIgnoreTTL(robj *o, bool ignore) {
    if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        /* prevent placing access function if not needed */
        if (!ignore && hashTypeGetVolatileSet(o) == NULL) {
            ignore = true;
        }
        hashtableSetType(o->ptr, ignore ? &hashHashtableType : &hashWithVolatileItemsHashtableType);
    }
}

static vset *hashTypeGetOrcreateVolatileSet(robj *o) {
    serverAssert(o->encoding == OBJ_ENCODING_HASHTABLE);
    vset *set = (vset *)hashtableMetadata(o->ptr);
    if (!vsetIsValid(set)) {
        vsetInit(set);
        /* serves mainly for optimization. Use type which supports access function only when needed. */
        hashTypeIgnoreTTL(o, false);
    }
    return set;
}

void hashTypeFreeVolatileSet(robj *o) {
    vset *set = (vset *)hashtableMetadata(o->ptr);
    if (vsetIsValid(set)) vsetRelease(set);
    /* serves mainly for optimization. by changing the hashtable type we can avoid extra function call in hashtable access */
    hashTypeIgnoreTTL(o, true);
}

void hashTypeTrackEntry(robj *o, entry *entry) {
    vset *set;
    if (hashTypeHasVolatileFields(o)) {
        set = hashTypeGetVolatileSet(o);
    } else {
        set = hashTypeGetOrcreateVolatileSet(o);
    }
    bool added = vsetAddEntry(set, entryGetExpiryVsetFunc, entry);
    serverAssert(added);
}

static void hashTypeUntrackEntry(robj *o, entry *entry) {
    if (!entryHasExpiry(entry)) return;
    vset *set = hashTypeGetVolatileSet(o);
    debugServerAssert(set);
    serverAssert(vsetRemoveEntry(set, entryGetExpiryVsetFunc, entry));
    if (vsetIsEmpty(set)) {
        hashTypeFreeVolatileSet(o);
    }
}

static void hashTypeTrackUpdateEntry(robj *o, entry *old_entry, entry *new_entry, long long old_expiry, long long new_expiry) {
    int old_tracked = (old_entry && old_expiry != EXPIRY_NONE);
    int new_tracked = (new_entry && new_expiry != EXPIRY_NONE);
    /* If entry was not tracked before and not going to be tracked now, we can simply return */
    if (!old_tracked && !new_tracked)
        return;

    vset *set = hashTypeGetOrcreateVolatileSet(o);
    debugServerAssert(!old_tracked || !vsetIsEmpty(set));

    serverAssert(vsetUpdateEntry(set, entryGetExpiryVsetFunc, old_entry, new_entry, old_expiry, new_expiry) == 1);

    if (vsetIsEmpty(set)) {
        hashTypeFreeVolatileSet(o);
    }
}

// This is a hashtableType validateEntry callback
bool hashHashtableTypeValidate(hashtable *ht, void *entryptr) {
    UNUSED(ht);
    entry *entry = entryptr;
    expirationPolicy policy = getExpirationPolicyWithFlags(0);
    if (policy == POLICY_IGNORE_EXPIRE) return true;

    if (!entryIsExpired(entry)) return true;

    return false;
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

/* Higher level function of hashTypeGet*() that returns the hash value
 * associated with the specified field. If the field is found C_OK
 * is returned, otherwise C_ERR. The returned object is returned by
 * reference in either *vstr and *vlen if it's returned in string form,
 * or stored in *vll if it's returned as a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * for C_OK and checking if vll (or vstr) is NULL.
 *
 * If *expiry is populated than the function will also provide the current field expiration time
 * or EXPIRY_NONE in case the field has no expiration time defined. */
int hashTypeGetValue(robj *o, sds field, unsigned char **vstr, unsigned int *vlen, long long *vll, long long *expiry) {
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        *vstr = NULL;
        if (hashTypeGetFromListpack(o, field, vstr, vlen, vll) == 0) {
            if (expiry) *expiry = EXPIRY_NONE;
            return C_OK;
        }
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        void *entry = NULL;
        hashtableFind(o->ptr, field, &entry);
        if (entry) {
            sds value = entryGetValue(entry);
            serverAssert(value != NULL);
            *vstr = (unsigned char *)value;
            *vlen = sdslen(value);
            if (expiry) *expiry = entryGetExpiry(entry);
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
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        if (hashTypeExists(o, field)) {
            if (expiry) *expiry = EXPIRY_NONE;
            return C_OK;
        }
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        void *found_element = NULL;
        if (hashtableFind(o->ptr, field, &found_element)) {
            if (expiry) *expiry = entryGetExpiry(found_element);
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

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll, NULL) == C_ERR) return NULL;
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

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll, NULL) == C_OK) len = vstr ? vlen : sdigits10(vll);

    return len;
}

/* Test if the specified field exists in the given hash. Returns 1 if the field
 * exists, and 0 when it doesn't. */
int hashTypeExists(robj *o, sds field) {
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    return hashTypeGetValue(o, field, &vstr, &vlen, &vll, NULL) == C_OK;
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

        /* We have to ignore the TTL when setting an element. this is mainly in order to be able to update an existing expired
         * entry and not have it remain in the hashtable with the same field/value. */
        hashTypeIgnoreTTL(o, true);
        hashtablePosition position;
        void *existing;
        if (hashtableFindPositionForInsert(ht, field, &position, &existing)) {
            /* does not exist yet */
            entry *entry = entryCreate(field, v, expiry);
            hashtableInsertAtPosition(ht, entry, &position);
            /* In case an expiry is set on the new entry, we need to track it */
            if (expiry != EXPIRY_NONE) {
                hashTypeTrackEntry(o, entry);
            }
        } else {
            /* exists: replace value */
            long long entry_expiry = entryGetExpiry(existing);
            /* It is possible that the entry is already expired. In this case we can override it, but we need to make sure to expire it first
             * and treat it like it did not exist. */
            bool is_expired = timestampIsExpired(entry_expiry);
            if (!is_expired && flags & HASH_SET_KEEP_EXPIRY) {
                /* In case the HASH_SET_KEEP_EXPIRY will force keeping the existing entry expiry. */
                expiry = entry_expiry;
            }
            void *new_entry = entryUpdate(existing, v, expiry);
            if (new_entry != existing) {
                /* It has been reallocated. */
                bool replaced = hashtableReplaceReallocatedEntry(ht, existing, new_entry);
                serverAssert(replaced);
            }

            hashTypeTrackUpdateEntry(o, existing, new_entry, entry_expiry, expiry);

            /* since we are exposed to expired entries, we must NOT reflect them as being "updated" */
            update = is_expired ? 0 : 1;
        }
        hashTypeIgnoreTTL(o, false);
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
 * returns 2 when 'expire' indicate a past Unix time. In this case, if the item exists in the HASH, it will also be expired. */
static expiryModificationResult hashTypeSetExpire(robj *o, sds field, long long expiry, int flag) {
    /* If no object we will return -2 */
    if (o == NULL) return EXPIRATION_MODIFICATION_NOT_EXIST;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;
        /* We do not want to convert to listpack for no good reason.
         * So we first check if the item exists.*/
        if (hashTypeGetFromListpack(o, field, &vstr, &vlen, &vll) < 0) {
            return EXPIRATION_MODIFICATION_NOT_EXIST;
        }
        /* When listpack representation is used, we consider it as infinite TTL,
         * so expire command with gt always fail the GT as well as existence(XX).
         * Else, we already know we are going to set an expiration so we expend to hashtable encoding. */
        if (flag & EXPIRE_XX || flag & EXPIRE_GT) {
            return EXPIRATION_MODIFICATION_FAILED_CONDITION;
        } else {
            hashTypeConvert(o, OBJ_ENCODING_HASHTABLE);
        }
    }

    /* we must be hashtable encoded */
    serverAssert(o->encoding == OBJ_ENCODING_HASHTABLE);

    hashtable *ht = o->ptr;
    void **entry_ref = NULL;
    if ((entry_ref = hashtableFindRef(ht, field))) {
        entry *current_entry = *entry_ref;
        long long current_expire = entryGetExpiry(current_entry);
        if (flag) {
            /* NX option is set, check no current expiry */
            if (flag & EXPIRE_NX) {
                if (current_expire != EXPIRY_NONE) {
                    return EXPIRATION_MODIFICATION_FAILED_CONDITION;
                }
            }

            /* XX option is set, check current expiry */
            if (flag & EXPIRE_XX) {
                if (current_expire == EXPIRY_NONE) {
                    return EXPIRATION_MODIFICATION_FAILED_CONDITION;
                }
            }

            /* GT option is set, check current expiry */
            if (flag & EXPIRE_GT) {
                /* When current_expire is -1, we consider it as infinite TTL,
                 * so expire command with gt always fail the GT. */
                if (expiry <= current_expire || current_expire == EXPIRY_NONE) {
                    return EXPIRATION_MODIFICATION_FAILED_CONDITION;
                }
            }

            /* LT option is set, check current expiry */
            if (flag & EXPIRE_LT) {
                /* When current_expire -1, we consider it as infinite TTL,
                 * so if there is an expiry on the key and it's not less than current, we fail the LT. */
                if (current_expire != EXPIRY_NONE && expiry >= current_expire) {
                    return EXPIRATION_MODIFICATION_FAILED_CONDITION;
                }
            }
        }
        *entry_ref = entrySetExpiry(current_entry, expiry);
        hashTypeTrackUpdateEntry(o, current_entry, *entry_ref, current_expire, expiry);
        return EXPIRATION_MODIFICATION_SUCCESSFUL;
    }
    return EXPIRATION_MODIFICATION_NOT_EXIST; // we did not find anything to do. return -2
}


static expiryModificationResult hashTypePersist(robj *o, sds field) {
    /* NULL object returns -2 */
    if (o == NULL || o->type != OBJ_HASH) return EXPIRATION_MODIFICATION_NOT_EXIST;

    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        if (hashTypeExists(o, field))
            /* When listpack representation is used, All items are without expiry */
            return EXPIRATION_MODIFICATION_FAILED;
        else
            return EXPIRATION_MODIFICATION_NOT_EXIST; // Did not find any element return -2
    }

    hashtable *ht = o->ptr;
    void **entry_ref = NULL;
    if ((entry_ref = hashtableFindRef(ht, field))) {
        entry *current_entry = *entry_ref;
        long long current_expire = entryGetExpiry(current_entry);
        if (current_expire != EXPIRY_NONE) {
            hashTypeUntrackEntry(o, current_entry);
            *entry_ref = entryUpdate(current_entry, NULL, EXPIRY_NONE);
            return EXPIRATION_MODIFICATION_SUCCESSFUL;
        }
        return EXPIRATION_MODIFICATION_FAILED; // If the found element has no expiration set, return -1
    }
    return EXPIRATION_MODIFICATION_NOT_EXIST; // Did not find any element return -2
}

/* Delete an element from a hash.
 * Return true on deleted and false on not found. */
bool hashTypeDelete(robj *o, sds field) {
    bool deleted = false;
    serverAssert(o && o->type == OBJ_HASH);
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
                deleted = true;
            }
        }
    } else if (o->encoding == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = o->ptr;
        void *entry = NULL;
        deleted = hashtablePop(ht, field, &entry);
        if (deleted) {
            hashTypeUntrackEntry(o, entry);
            entryFree(entry);
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
    hi->volatile_items_iter = false;

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
    hi->volatile_items_iter = true;

    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        return;
    } else if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        vsetInitIterator(hashTypeGetVolatileSet(subject), &hi->viter);
    } else {
        serverPanic("Unknown hash encoding");
    }
}

void hashTypeResetIterator(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_HASHTABLE) {
        if (!hi->volatile_items_iter)
            hashtableResetIterator(&hi->iter);
        else
            vsetResetIterator(&hi->viter);
    }
}

/* Move to the next entry in the hash. Return C_OK when the next entry
 * could be found and C_ERR when the iterator reaches the end. */
int hashTypeNext(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl;
        unsigned char *fptr, *vptr;

        /* listpack encoding does not have volatile items, so return as iteration end */
        if (hi->volatile_items_iter) return C_ERR;

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
        if (!hi->volatile_items_iter) {
            if (!hashtableNext(&hi->iter, &hi->next)) return C_ERR;
        } else {
            if (!vsetNext(&hi->viter, &hi->next)) return C_ERR;
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
        return entryGetField(hi->next);
    } else {
        return entryGetValue(hi->next);
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
            entry *entry = entryCreate(field, value, EXPIRY_NONE);
            sdsfree(field);
            if (!hashtableAdd(ht, entry)) {
                entryFree(entry);
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
        hobj = createObject(OBJ_HASH, ht);
        hobj->encoding = OBJ_ENCODING_HASHTABLE;

        hashTypeInitIterator(o, &hi);
        while (hashTypeNext(&hi) != C_ERR) {
            /* Extract a field-value pair from an original hash object.*/
            sds field = hashTypeCurrentFromHashTable(&hi, OBJ_HASH_FIELD);
            sds value = hashTypeCurrentFromHashTable(&hi, OBJ_HASH_VALUE);
            long long expiry = entryGetExpiry(hi.next);
            /* Add a field-value pair to a new hash object. */
            entry *entry = entryCreate(field, sdsdup(value), expiry);
            hashtableAdd(ht, entry);
            if (expiry != EXPIRY_NONE)
                hashTypeTrackEntry(hobj, entry);
        }
        hashTypeResetIterator(&hi);
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
        void *e = NULL;
        int maxtries = 100;
        hashTypeIgnoreTTL(hashobj, true);
        while (!e) {
            hashtableFairRandomEntry(hashobj->ptr, &e);
            if (entryIsExpired(e) && --maxtries) {
                e = NULL;
                continue;
            } else if (maxtries == 0) {
                /* in case we will not be able to locate an entry which is not expired, we will just not return any
                 * result. An alternative would have been that we end up returning an expired entry. */
                field->sval = NULL;
                if (val) val->sval = NULL;
                break;
            }
            sds sds_field = entryGetField(e);
            field->sval = (unsigned char *)sds_field;
            field->slen = sdslen(sds_field);
            if (val) {
                entry *hash_entry = e;
                sds sds_val = entryGetValue(hash_entry);
                val->sval = (unsigned char *)sds_val;
                val->slen =
                    sdslen(sds_val);
            }
        }
        hashTypeIgnoreTTL(hashobj, false);
    } else if (hashobj->encoding == OBJ_ENCODING_LISTPACK) {
        lpRandomPair(hashobj->ptr, hashsize, field, val);
    } else {
        serverPanic("Unknown hash encoding");
    }
}


/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

void hincrbyCommand(client *c) {
    long long value, incr, oldvalue;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;
    long long expiry = EXPIRY_NONE;
    if (getLongLongFromObjectOrReply(c, c->argv[3], &incr, NULL) != C_OK) return;
    if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;
    if (hashTypeGetValue(o, c->argv[2]->ptr, &vstr, &vlen, &value, &expiry) == C_OK) {
        if (vstr) {
            if (string2ll((char *)vstr, vlen, &value) == 0) {
                addReplyError(c, "hash value is not an integer");
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
        return;
    }
    value += incr;
    new = sdsfromlonglong(value);
    hashTypeSet(o, c->argv[2]->ptr, new, expiry, HASH_SET_TAKE_VALUE);
    signalModifiedKey(c, c->db, c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH, "hincrby", c->argv[1], c->db->id);
    server.dirty++;
    addReplyLongLong(c, value);
}

void hincrbyfloatCommand(client *c) {
    long double value, incr;
    long long ll;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;
    long long expiry = EXPIRY_NONE;

    if (getLongDoubleFromObjectOrReply(c, c->argv[3], &incr, NULL) != C_OK) return;
    if (isnan(incr) || isinf(incr)) {
        addReplyError(c, "value is NaN or Infinity");
        return;
    }
    if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;

    if (hashTypeGetValue(o, c->argv[2]->ptr, &vstr, &vlen, &ll, &expiry) == C_OK) {
        if (vstr) {
            if (string2ld((char *)vstr, vlen, &value) == 0) {
                addReplyError(c, "hash value is not a float");
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
        return;
    }

    char buf[MAX_LONG_DOUBLE_CHARS];
    int len = ld2string(buf, sizeof(buf), value, LD_STR_HUMAN);
    new = sdsnewlen(buf, len);
    hashTypeSet(o, c->argv[2]->ptr, new, expiry, HASH_SET_TAKE_VALUE);
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
}

static void addHashFieldToReply(client *c, robj *o, sds field) {
    if (o == NULL) {
        addReplyNull(c);
        return;
    }

    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    if (hashTypeGetValue(o, field, &vstr, &vlen, &vll, NULL) == C_OK) {
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
    addHashFieldToReply(c, o, c->argv[2]->ptr);
}

void hmgetCommand(client *c) {
    robj *o;
    int i;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HMGET should respond with a series of null bulks. */
    o = lookupKeyRead(c->db, c->argv[1]);

    if (checkType(c, o, OBJ_HASH)) return;

    addReplyArrayLen(c, c->argc - 2);
    for (i = 2; i < c->argc; i++) {
        addHashFieldToReply(c, o, c->argv[i]->ptr);
    }
    if (o && hashTypeLength(o) == 0) {
        dbDelete(c->db, c->argv[1]);
    }
}

void hdelCommand(client *c) {
    robj *o;
    int j, deleted = 0;
    bool keyremoved = false;

    if ((o = lookupKeyWriteOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_HASH)) return;

    bool hash_volatile_items = hashTypeHasVolatileFields(o);
    for (j = 2; j < c->argc; j++) {
        if (hashTypeDelete(o, c->argv[j]->ptr)) {
            deleted++;
            if (hashTypeLength(o) == 0) {
                if (hash_volatile_items) dbUntrackKeyWithVolatileItems(c->db, o);
                dbDelete(c->db, c->argv[1]); /* Please note that this will also remove the tracking from the kvstore */
                keyremoved = true;
                break;
            }
        }
    }
    if (deleted) {
        if (!keyremoved && hash_volatile_items != hashTypeHasVolatileFields(o)) {
            dbUpdateObjectWithVolatileItemsTracking(c->db, o);
        }
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_HASH, "hdel", c->argv[1], c->db->id);
        if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c, deleted);
}

void hgetdelCommand(client *c) {
    /* argv: [0]=HGETDEL, [1]=key, [2]=FIELDS, [3]=numfields, [4...]=fields */
    int fields_index = 4;
    int i, deleted = 0;
    long long num_fields = 0;
    bool keyremoved = false;

    if (getLongLongFromObjectOrReply(c, c->argv[fields_index - 1], &num_fields, NULL) != C_OK) return;

    /* Check that the parsed fields number matches the real provided number of fields */
    if (!num_fields || num_fields != (c->argc - fields_index)) {
        addReplyError(c, "numfields should be greater than 0 and match the provided number of fields");
        return;
    }

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HGETDEL should respond with a series of null bulks. */
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_HASH)) return;

    bool hash_volatile_items = hashTypeHasVolatileFields(o);

    /* Reply with array of values and delete at the same time */
    addReplyArrayLen(c, num_fields);
    for (i = fields_index; i < c->argc; i++) {
        addHashFieldToReply(c, o, c->argv[i]->ptr);

        /* If hash doesn't exist, continue as already replied with NULL */
        if (o == NULL) continue;
        if (hashTypeDelete(o, c->argv[i]->ptr)) {
            deleted++;
            if (hashTypeLength(o) == 0) {
                if (hash_volatile_items) dbUntrackKeyWithVolatileItems(c->db, o);
                dbDelete(c->db, c->argv[1]);
                keyremoved = true;
                o = NULL;
            }
        }
    }

    if (deleted) {
        if (!keyremoved && hash_volatile_items != hashTypeHasVolatileFields(o)) {
            dbUpdateObjectWithVolatileItemsTracking(c->db, o);
        }
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_HASH, "hdel", c->argv[1], c->db->id);
        if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty += deleted;
    }
}

void hlenCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_HASH)) return;

    addReplyLongLong(c, hashTypeLength(o));
}

void hstrlenCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_HASH)) return;
    addReplyLongLong(c, hashTypeGetValueLength(o, c->argv[2]->ptr));
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

void hsetnxCommand(client *c) {
    robj *o;
    if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) return;
    if (hashTypeExists(o, c->argv[2]->ptr)) {
        addReply(c, shared.czero);
    } else {
        hashTypeTryConversion(o, c->argv, 2, 3);
        bool has_volatile_fields = hashTypeHasVolatileFields(o);
        hashTypeSet(o, c->argv[2]->ptr, c->argv[3]->ptr, EXPIRY_NONE, HASH_SET_COPY | HASH_SET_KEEP_EXPIRY);
        if (has_volatile_fields != hashTypeHasVolatileFields(o)) {
            dbUpdateObjectWithVolatileItemsTracking(c->db, o);
        }
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_HASH, "hset", c->argv[1], c->db->id);
        server.dirty++;
        addReply(c, shared.cone);
    }
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
    bool has_volatile_fields = hashTypeHasVolatileFields(o);
    for (i = 2; i < c->argc; i += 2) {
        created += !hashTypeSet(o, c->argv[i]->ptr, c->argv[i + 1]->ptr, EXPIRY_NONE, HASH_SET_COPY);
    }
    if (has_volatile_fields != hashTypeHasVolatileFields(o)) {
        dbUpdateObjectWithVolatileItemsTracking(c->db, o);
    }
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
}

/* High-Level Algorithm of HSETEX Command:
 *
 * - Parse arguments and options:
 *   Parses optional flags such as NX, XX, FNX, FXX, KEEPTTL, and expiration time options.
 *   Ensures the number of specified fields matches the actual provided key-value pairs.
 *
 * - Check object existence conditions:
 *   Depending on NX/XX flags, verifies whether the hash key must or must not exist.
 *   Exits early with a zero reply if conditions aren't met.
 *
 * - Create the hash object if needed:
 *   If the key does not exist and creation is permitted, allocates a new hash.
 *
 * - Handle expiration logic:
 *   Computes the expiry time (relative or absolute).
 *   If the expiration is in the past, the command proceeds to delete the relevant fields.
 *
 * - Enforce per-field conditions:
 *   If FNX (field must not exist) or FXX (field must exist) flags are set,
 *   ensures all fields satisfy these conditions before proceeding.
 *
 * - Apply changes:
 *   Either deletes expired fields or sets fields with optional expiration.
 *
 * - Clean up and notify:
 *   Deletes the key if the hash becomes empty.
 *   Emits keyspace notifications for changes (see below).
 *   Modifies the command vector for AOF propagation if necessary.
 *
 *
 * Return Value:
 * - Returns integer 1 if all fields were successfully updated or deleted.
 * - Returns integer 0 if no fields were updated due to condition failures.
 *
 *
 * Keyspace Notifications (if enabled):
 * - "hset"      — Emitted when fields are added or updated.
 * - "hexpire"   — Emitted when expiration is set on fields.
 * - "hexpired"  — Emitted when fields are immediately expired and deleted.
 * - "del"       — Emitted if the entire key is removed (empty hash).
 *
 *
 * Client Reply:
 * - Integer reply: 1 if all changes succeeded, 0 if no changes occurred. */
void hsetexCommand(client *c) {
    robj *o;
    robj *expire = NULL;
    robj *comparison = NULL;
    int unit = UNIT_SECONDS;
    int flags = ARGS_NO_FLAGS;
    int fields_index = 0;
    long long num_fields = 0;
    long long when = EXPIRY_NONE;
    int i = 0;
    int set_flags = HASH_SET_COPY, set_expired = 0;
    int changes = 0;
    robj **new_argv = NULL;
    int new_argc = 0;
    int need_rewrite_argv = 0;

    for (; fields_index < c->argc - 1; fields_index++) {
        if (!strcasecmp(c->argv[fields_index]->ptr, "fields")) {
            /* checking optional flags */
            if (parseExtendedCommandArgumentsOrReply(c, &flags, &unit, &expire, &comparison, COMMAND_HSET, fields_index++) != C_OK) return;
            if (getLongLongFromObjectOrReply(c, c->argv[fields_index++], &num_fields, NULL) != C_OK) return;
            break;
        }
    }
    /* Check that the parsed fields number matches the real provided number of fields */
    if (!num_fields || num_fields > LLONG_MAX / 2 || (num_fields * 2) != (c->argc - fields_index)) {
        addReplyError(c, "numfields should be greater than 0 and match the provided number of fields");
        return;
    }

    o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_HASH))
        return;

    if (flags & (ARGS_SET_NX | ARGS_SET_XX | ARGS_SET_FNX | ARGS_SET_FXX | ARGS_EX | ARGS_PX | ARGS_EXAT)) {
        need_rewrite_argv = 1;
    }

    /* Check NX/XX key-level conditions before creating a new object */
    if (((flags & ARGS_SET_NX) && o != NULL) ||
        ((flags & ARGS_SET_XX) && o == NULL)) {
        addReply(c, shared.czero);
        return;
    }

    /* Handle parsing and calculating the expiration time. */
    if (flags & ARGS_KEEPTTL)
        set_flags |= HASH_SET_KEEP_EXPIRY;
    else if (expire) {
        long long basetime = (flags & (ARGS_EXAT | ARGS_PXAT)) ? 0 : commandTimeSnapshot();

        if (convertExpireArgumentToUnixTime(c, expire, basetime, unit, &when) == C_ERR)
            return;

        if (checkAlreadyExpired(when)) {
            set_expired = 1;
        }
    }

    /* Check FNX/FXX field-level conditions */
    if (flags & (ARGS_SET_FNX | ARGS_SET_FXX)) {
        if (o) {
            /* Key exists: check fields normally */
            for (i = fields_index; i < c->argc; i += 2) {
                if (((flags & ARGS_SET_FNX) && hashTypeExists(o, c->argv[i]->ptr)) ||
                    ((flags & ARGS_SET_FXX) && !hashTypeExists(o, c->argv[i]->ptr))) {
                    addReply(c, shared.czero);
                    return;
                }
            }
        } else {
            /* Key does not exist */
            if (flags & ARGS_SET_FXX) {
                /* Any FXX fails because no fields exist */
                addReply(c, shared.czero);
                return;
            }
            /* FNX automatically passes if key doesn't exist, nothing to check */
        }
    }

    if (o == NULL) {
        o = createHashObject();
        dbAdd(c->db, c->argv[1], &o);
    }

    bool has_volatile_fields = hashTypeHasVolatileFields(o);

    /* Prepare a new argv when rewriting the command. If set_expired is true,
     * all expired fields will be deleted. Otherwise, if rewriting is needed due to NX/XX/FNX/FXX flags,
     * copy the command, key, and optional arguments, skipping the NX/XX/FNX/FXX flags. */
    if (set_expired) {
        new_argv = zmalloc(sizeof(robj *) * (num_fields + 2));
        new_argv[new_argc++] = shared.hdel;
        incrRefCount(shared.hdel);
        new_argv[new_argc++] = c->argv[1];
        incrRefCount(c->argv[1]);
    } else if (need_rewrite_argv) {
        /* We use new_argv for rewrite */
        new_argv = zmalloc(sizeof(robj *) * c->argc);
        // Copy optional args (skip NX/XX/FNX/FXX)
        for (int i = 0; i < fields_index; i++) {
            if (strcmp(c->argv[i]->ptr, "NX") &&
                strcmp(c->argv[i]->ptr, "XX") &&
                strcmp(c->argv[i]->ptr, "FNX") &&
                strcmp(c->argv[i]->ptr, "FXX")) {
                /* Propagate as HSETEX Key Value PXAT millisecond-timestamp if there is
                 * EX/PX/EXAT flag. */
                if (expire && !(flags & ARGS_PXAT) && c->argv[i + 1] == expire) {
                    robj *milliseconds_obj = createStringObjectFromLongLong(when);
                    new_argv[new_argc++] = shared.pxat;
                    new_argv[new_argc++] = milliseconds_obj;
                    i++; // skip the original expire argument
                } else {
                    new_argv[new_argc++] = c->argv[i];
                    incrRefCount(c->argv[i]);
                }
            }
        }
    }

    for (i = fields_index; i < c->argc; i += 2) {
        if (set_expired) {
            if (hashTypeDelete(o, c->argv[i]->ptr)) {
                new_argv[new_argc++] = c->argv[i];
                incrRefCount(c->argv[i]);
                /* we treat this case exactly as active expiration. */
                server.stat_expiredfields++;
                changes++;
            }
        } else {
            hashTypeSet(o, c->argv[i]->ptr, c->argv[i + 1]->ptr, when, set_flags);
            changes++;
            if (need_rewrite_argv) {
                new_argv[new_argc++] = c->argv[i];
                incrRefCount(c->argv[i]);
                new_argv[new_argc++] = c->argv[i + 1];
                incrRefCount(c->argv[i + 1]);
            }
        }
    }


    if (changes) {
        if (has_volatile_fields != hashTypeHasVolatileFields(o)) {
            dbUpdateObjectWithVolatileItemsTracking(c->db, o);
        }
        if (set_expired) {
            replaceClientCommandVector(c, new_argc, new_argv);
            /* We would like to reduce the number of hexpired events in case there are potential many expired fields. */
            notifyKeyspaceEvent(NOTIFY_HASH, "hexpired", c->argv[1], c->db->id);
        } else {
            notifyKeyspaceEvent(NOTIFY_HASH, "hset", c->argv[1], c->db->id);
            if (need_rewrite_argv) {
                replaceClientCommandVector(c, new_argc, new_argv);
            }
            if (expire) {
                notifyKeyspaceEvent(NOTIFY_HASH, "hexpire", c->argv[1], c->db->id);
            }
        }
        signalModifiedKey(c, c->db, c->argv[1]);
        /* Delete the object in case it was left empty */
        if (hashTypeLength(o) == 0) {
            dbDelete(c->db, c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        }
        server.dirty += changes;
    } else {
        /* If no changes were done we still need to free the new argv array and the refcount of the first argument. */
        if (set_expired)
            decrRefCount(c->argv[1]);
        if (new_argv) zfree(new_argv);
    }
    addReplyLongLong(c, changes == num_fields ? 1 : 0);
}

/* High-Level Algorithm of HGETEX Command:
 *
 * - Parses the command for optional arguments, including expiration options,
 *   persistence flags, and the list of hash fields to retrieve.
 *
 * - Verifies that the number of fields specified matches the actual arguments,
 *   and ensures the key exists and is a valid hash type.
 *
 * - Computes the expiration behavior:
 *   - If `PERSIST` is provided, removes the expiration from the fields.
 *   - If an expiration time is specified, calculates it relative or absolute.
 *     - If already expired, deletes the fields immediately.
 *     - Otherwise, schedules new expiration timestamps.
 *
 * - Retrieves and replies with the values for each requested field.
 *
 * - For each field:
 *   - If expiration is due: deletes the field.
 *   - If an expiry is scheduled: updates the field's expiration timestamp.
 *   - If persisting: clears the field's expiration.
 *
 * - If any changes were made (deletes, expires, or persists):
 *   - Rewrites the command vector (for AOF and replication) using HDEL, HPEXPIREAT, or HPERSIST.
 *   - Issues keyspace notifications accordingly.
 *   - If the hash becomes empty as a result, deletes the key and notifies.
 *
 *
 * Return Value:
 * - Always replies with an array of values for the requested fields (including NULLs for missing fields).
 *
 *
 * Keyspace Notifications (if enabled):
 * - "hexpire"   — When expiration is added to hash fields.
 * - "hexpired"  — When fields are immediately expired and deleted.
 * - "hpersist"  — When expiration is removed from fields.
 * - "del"       — If the hash becomes empty and is removed entirely. */
void hgetexCommand(client *c) {
    robj *o;
    robj *expire = NULL;
    robj *comparison = NULL;
    int unit = UNIT_SECONDS;
    int flags = ARGS_NO_FLAGS;
    int fields_index = 0;
    long long num_fields = -1;
    long long when = EXPIRY_NONE;
    int i = 0;
    int set_expiry = 0, set_expired = 0, persist = 0;
    int changes = 0;
    robj **new_argv = NULL;
    robj *milliseconds_obj = NULL, *numitems_obj = NULL;
    int new_argc = 0;
    int milliseconds_index = -1, numitems_index = -1;

    for (; fields_index < c->argc - 1; fields_index++) {
        if (!strcasecmp(c->argv[fields_index]->ptr, "fields")) {
            /* checking optional flags */
            if (parseExtendedCommandArgumentsOrReply(c, &flags, &unit, &expire, &comparison, COMMAND_HGET, fields_index++) != C_OK) return;
            if (getLongLongFromObjectOrReply(c, c->argv[fields_index++], &num_fields, NULL) != C_OK) return;
            break;
        }
    }

    /* Check that the parsed fields number matches the real provided number of fields */
    if (!num_fields || num_fields != (c->argc - fields_index)) {
        addReplyError(c, "numfields should be greater than 0 and match the provided number of fields");
        return;
    }

    o = lookupKeyRead(c->db, c->argv[1]);

    if (checkType(c, o, OBJ_HASH)) return;

    /* Check if the hash object has volatile fields, used for active-expiry tracking */
    bool has_volatile_fields = hashTypeHasVolatileFields(o);

    /* Handle parsing and calculating the expiration time. */
    if (flags & ARGS_PERSIST) {
        persist = 1;
    } else if (expire) {
        long long basetime = (flags & (ARGS_EXAT | ARGS_PXAT)) ? 0 : commandTimeSnapshot();

        if (convertExpireArgumentToUnixTime(c, expire, basetime, unit, &when) == C_ERR)
            return;

        if (checkAlreadyExpired(when)) {
            set_expired = 1;
            when = 0;
        } else {
            set_expiry = 1;
        }
    }

    initDeferredReplyBuffer(c);

    addReplyArrayLen(c, num_fields);
    /* This command is never propagated as is. It is either propagated as HDEL, HPEXPIREAT or PERSIST.
     * This why it doesn't need special handling in feedAppendOnlyFile to convert relative expire time to absolute one. */
    if (set_expiry || set_expired || persist) {
        /* allocate a new client argv for replicating the command. */
        new_argv = zmalloc(sizeof(robj *) * (num_fields + 5));
        if (set_expired)
            new_argv[new_argc++] = shared.hdel;
        else if (persist)
            new_argv[new_argc++] = shared.hpersist;
        else
            new_argv[new_argc++] = shared.hpexpireat;

        new_argv[new_argc++] = c->argv[1];
        incrRefCount(c->argv[1]);

        if (set_expiry) {
            new_argv[new_argc++] = NULL; // placeholder for the expiration time
            milliseconds_index = new_argc - 1;
        }

        if (set_expiry || persist) {
            new_argv[new_argc++] = shared.fields;
            new_argv[new_argc++] = NULL; // placeholder for the number of objects
            numitems_index = new_argc - 1;
        }
    }
    for (i = fields_index; i < c->argc; i++) {
        bool changed = false;
        addHashFieldToReply(c, o, c->argv[i]->ptr);
        if (o && set_expired) {
            changed = hashTypeDelete(o, c->argv[i]->ptr);
            /* we treat this case exactly as active expiration. */
            if (changed) server.stat_expiredfields++;
        } else if (set_expiry) {
            changed = hashTypeSetExpire(o, c->argv[i]->ptr, when, 0) == EXPIRATION_MODIFICATION_SUCCESSFUL;
        } else if (persist) {
            changed = hashTypePersist(o, c->argv[i]->ptr) == EXPIRATION_MODIFICATION_SUCCESSFUL;
        }
        if (changed) {
            changes++;
            new_argv[new_argc++] = c->argv[i];
            incrRefCount(c->argv[i]);
        }
    }

    /* rewrite the command vector and persist in case there are changes.
     * Also notify keyspace notifications and signal the key was changed. */
    if (changes) {
        if (milliseconds_index > 0) {
            milliseconds_obj = createStringObjectFromLongLong(when);
            new_argv[milliseconds_index] = milliseconds_obj;
            incrRefCount(milliseconds_obj);
        }
        if (numitems_index > 0) {
            numitems_obj = createStringObjectFromLongLong(changes);
            new_argv[numitems_index] = numitems_obj;
            incrRefCount(numitems_obj);
        }
        replaceClientCommandVector(c, new_argc, new_argv);
        if (set_expired)
            notifyKeyspaceEvent(NOTIFY_HASH, "hexpired", c->argv[1], c->db->id);
        else
            notifyKeyspaceEvent(NOTIFY_HASH, set_expiry ? "hexpire" : "hpersist", c->argv[1], c->db->id);
        if (milliseconds_obj) decrRefCount(milliseconds_obj);
        if (numitems_obj) decrRefCount(numitems_obj);

        server.dirty += changes;
        signalModifiedKey(c, c->db, c->argv[1]);

        if (has_volatile_fields != hashTypeHasVolatileFields(o)) {
            dbUpdateObjectWithVolatileItemsTracking(c->db, o);
        }

        /* Delete the object in case it was left empty */
        if (hashTypeLength(o) == 0) {
            dbDelete(c->db, c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        }
    } else {
        /* If no changes were done we still need to free the new argv array and the refcount of the first argument. */
        if (set_expiry || set_expired || persist) {
            decrRefCount(c->argv[1]);
        }
        if (new_argv) zfree(new_argv);
    }

    commitDeferredReplyBuffer(c, 1);
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
    addReply(c, hashTypeExists(o, c->argv[2]->ptr) ? shared.cone : shared.czero);
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


/* High-Level Algorithm of hexpireGenericCommand (used by HEXPIRE, HPEXPIRE, HEXPIREAT, HPEXPIREAT):
 *
 * - Parses optional flags and the number of hash fields to apply expiration to.
 *
 * - Converts the given expiration time (relative or absolute) into a Unix timestamp.
 *
 * - Determines if the given timestamp is already expired:
 *   - If so, immediately deletes the specified hash fields.
 *   - If not, updates their expiration metadata.
 *
 * - Responds with an array of integers:
 *   - 1 if the expiration was set.
 *   - 0 if it was unchanged (due to provided condition check failing).
 *   - -2 if the field does not exist or the hash is empty.
 *   - 2 if the field was immediately expired and deleted due to provided expiration is 0 or in the past.
 *
 * - If fields were deleted due to expiration:
 *   - Rewrites the command as HDEL for replication/AOF.
 *   - Emits a "hexpired" keyspace event.
 *
 * - If expiration was newly set:
 *   - May rewrite the command as HPEXPIREAT if needed.
 *   - Emits a "hexpire" keyspace event.
 *
 * - If the hash becomes empty after deletions:
 *   - Deletes the hash key.
 *   - Emits a "del" event for the key.
 *
 * Return Value:
 * - An array of integers corresponding to the result for each field.
 *
 * Keyspace Notifications (if enabled):
 * - "hexpired" — when fields are immediately expired and deleted.
 * - "hexpire"  — when fields receive new expiration timestamps.
 * - "del"      — when the hash key becomes empty and is removed. */
void hexpireGenericCommand(client *c, long long basetime, int unit) {
    robj *key = c->argv[1], *param = c->argv[2];
    long long when; /* unix time in milliseconds when the key will expire. */
    int flag = 0;
    int fields_index = 3;
    long long num_fields = 0;
    int i, expired = 0, updated = 0;
    int set_expired = 0;
    robj **new_argv = NULL;
    int new_argc = 0;

    for (; fields_index < c->argc - 1; fields_index++) {
        if (!strcasecmp(c->argv[fields_index]->ptr, "fields")) {
            /* checking optional flags */
            if (parseExtendedExpireArgumentsOrReply(c, &flag, fields_index++) != C_OK) return;
            if (getLongLongFromObjectOrReply(c, c->argv[fields_index++], &num_fields, NULL) != C_OK) return;
            break;
        }
    }

    /* Check that the parsed fields number matches the real provided number of fields */
    if (!num_fields || num_fields != (c->argc - fields_index)) {
        addReplyError(c, "numfields should be greater than 0 and match the provided number of fields");
        return;
    }

    if (convertExpireArgumentToUnixTime(c, param, basetime, unit, &when) == C_ERR)
        return;

    if (checkAlreadyExpired(when))
        set_expired = 1;

    robj *obj = lookupKeyWrite(c->db, key);

    /* Non HASH type return simple error */
    if (checkType(c, obj, OBJ_HASH)) {
        return;
    }

    bool has_volatile_fields = hashTypeHasVolatileFields(obj);

    /* From this point we would return array reply */
    addReplyArrayLen(c, num_fields);

    /* In case we are expiring all the elements prepare a new argv since we are going to delete all the expired fields. */
    if (set_expired) {
        new_argv = zmalloc(sizeof(robj *) * (num_fields + 3));
        new_argv[new_argc++] = shared.hdel;
        incrRefCount(shared.hdel);
        new_argv[new_argc++] = c->argv[1];
        incrRefCount(c->argv[1]);
    }

    for (i = 0; i < num_fields; i++) {
        expiryModificationResult result = EXPIRATION_MODIFICATION_NOT_EXIST;
        if (set_expired) {
            if (obj && hashTypeDelete(obj, c->argv[fields_index + i]->ptr)) {
                /* In case we deleted the field, add it to the new hdel command vector. */
                new_argv[new_argc++] = c->argv[fields_index + i];
                incrRefCount(c->argv[fields_index + i]);
                result = EXPIRATION_MODIFICATION_EXPIRE_ASAP;
                /* we treat this case exactly as active expiration. */
                server.stat_expiredfields++;
                expired++;
            }
        } else {
            result = hashTypeSetExpire(obj, c->argv[fields_index + i]->ptr, when, flag);
            if (result == EXPIRATION_MODIFICATION_SUCCESSFUL) updated++;
        }
        addReplyLongLong(c, result);
    }

    if (expired || updated) {
        if (has_volatile_fields != hashTypeHasVolatileFields(obj)) {
            dbUpdateObjectWithVolatileItemsTracking(c->db, obj);
        }
        if (expired) {
            replaceClientCommandVector(c, new_argc, new_argv);
            /* We would like to reduce the number of hexpired events in case there are potential many expired fields. */
            notifyKeyspaceEvent(NOTIFY_HASH, "hexpired", c->argv[1], c->db->id);
        } else if (updated) {
            /* Propagate as HPEXPIREAT millisecond-timestamp
             * Only rewrite the command arg if not already HPEXPIREAT */
            if (c->cmd->proc != hpexpireatCommand) {
                rewriteClientCommandArgument(c, 0, shared.hpexpireat);
            }

            /* Avoid creating a string object when it's the same as argv[2] parameter  */
            if (basetime != 0 || unit == UNIT_SECONDS) {
                robj *when_obj = createStringObjectFromLongLong(when);
                rewriteClientCommandArgument(c, 2, when_obj);
                decrRefCount(when_obj);
            }
            notifyKeyspaceEvent(NOTIFY_HASH, "hexpire", c->argv[1], c->db->id);
        }
        server.dirty += (expired + updated); // in case there was a change increment the dirty
        signalModifiedKey(c, c->db, c->argv[1]);
        /* Delete the object in case it was left empty */
        if (hashTypeLength(obj) == 0) {
            dbDelete(c->db, c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        }
    }
}

void hexpireCommand(client *c) {
    hexpireGenericCommand(c, commandTimeSnapshot(), UNIT_SECONDS);
}

void hexpireatCommand(client *c) {
    hexpireGenericCommand(c, 0, UNIT_SECONDS);
}

void hpexpireCommand(client *c) {
    hexpireGenericCommand(c, commandTimeSnapshot(), UNIT_MILLISECONDS);
}

void hpexpireatCommand(client *c) {
    hexpireGenericCommand(c, 0, UNIT_MILLISECONDS);
}

/* High-Level Algorithm of HPERSIST Command:
 *
 * - Expects a key and a list of hash fields whose expiration metadata should be removed.
 * - Validates that the number of provided fields matches the declared count.
 *
 * - For each specified field attempts to remove any existing expiration.
 * - Replies to the client with an array of integers, each representing the result of persistence for one field:
 *   - 1 if the expiration for the field was removed.
 *   - -1 if the field exists, but has no expiration time set.
 *   - -2 if the field does not exist or the hash is empty.
 *
 * - If any expirations were removed:
 *   - Marks the key as modified (for replication/AOF consistency).
 *   - Emits a "hpersist" keyspace notification.
 *
 * Keyspace Notifications (if enabled):
 * - "hpersist" — emitted once if any field had its expiration removed. */
void hpersistCommand(client *c) {
    int fields_index = 4, result = 0, changes = 0;
    long long num_fields = 0;

    if (getLongLongFromObjectOrReply(c, c->argv[fields_index - 1], &num_fields, NULL) != C_OK) return;

    /* Check that the parsed fields number matches the real provided number of fields */
    if (!num_fields || num_fields != (c->argc - fields_index)) {
        addReplyError(c, "numfields should be greater than 0 and match the provided number of fields");
        return;
    }

    /* From this point we would return array reply */
    addReplyArrayLen(c, num_fields);

    robj *hash = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, hash, OBJ_HASH))
        return;

    bool has_volatile_fields = hashTypeHasVolatileFields(hash);

    for (int i = 0; i < num_fields; i++, fields_index++) {
        result = hashTypePersist(hash, c->argv[fields_index]->ptr);
        if (result == EXPIRATION_MODIFICATION_SUCCESSFUL) {
            server.dirty++;
            changes++;
        }
        addReplyLongLong(c, result);
    }
    if (changes) {
        if (has_volatile_fields != hashTypeHasVolatileFields(hash)) {
            dbUpdateObjectWithVolatileItemsTracking(c->db, hash);
        }
        notifyKeyspaceEvent(NOTIFY_HASH, "hpersist", c->argv[1], c->db->id);
        signalModifiedKey(c, c->db, c->argv[1]);
    }
}

/* High-Level Algorithm of HTTL / HPTTL / HEXPIRETIME / HPEXPIRETIME Commands:
 *
 * - These commands return the remaining time to live (TTL) or absolute expiry time
 *   of one or more fields in a hash.
 *
 * - HTTL / HPTTL:
 *   - Return relative TTL of each field (in seconds or milliseconds).
 *   - TTL is computed as the difference between current time and expiry time.
 *
 * - HEXPIRETIME / HPEXPIRETIME:
 *   - Return the absolute Unix time at which each field will expire
 *     (in seconds or milliseconds, depending on the variant).
 *
 * For each field requested:
 *   - If the field or hash does not exist: reply with -2.
 *   - If the field exists but has no expiration: reply with -1.
 *   - If the field has an expiration:
 *     - HTTL / HPTTL: reply with remaining TTL (clamped at 0 if negative).
 *     - HEXPIRETIME / HPEXPIRETIME: reply with the absolute expiry time.
 *
 * Return Value:
 * - An array of integers, one per field:
 *   - -2 = hash or field does not exist.
 *   - -1 = field exists but has no expiration.
 *   - >=0 = TTL or expiry time, depending on the command variant.
 *
 * Keyspace Notifications:
 * - None emitted; this command is read-only. */
void httlGenericCommand(client *c, long long basetime, int unit) {
    int fields_index = 4;
    long long num_fields = 0, result = -2;

    if (getLongLongFromObjectOrReply(c, c->argv[fields_index - 1], &num_fields, NULL) != C_OK) return;

    /* Check that the parsed fields number matches the real provided number of fields */
    if (!num_fields || num_fields != (c->argc - fields_index)) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    robj *hash = lookupKeyRead(c->db, c->argv[1]);

    if (checkType(c, hash, OBJ_HASH)) return;

    /* From this point we would return array reply */
    addReplyArrayLen(c, num_fields);

    for (int i = 0; i < num_fields; i++) {
        if (!hash || hashTypeGetExpiry(hash, c->argv[fields_index + i]->ptr, &result) == C_ERR) {
            addReplyLongLong(c, -2);
        } else if (result == EXPIRY_NONE) {
            addReplyLongLong(c, -1);
        } else {
            result = result - basetime;
            if (result < 0) result = 0;
            addReplyLongLong(c, unit == UNIT_MILLISECONDS ? result : ((result + 500) / 1000));
        }
    }
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

    void *replylen = addReplyDeferredLen(c);
    unsigned long reply_size = 0;

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. This case is the only one that also needs to return the
     * elements in random order. */
    if (!uniq || count == 1) {
        if (hash->encoding == OBJ_ENCODING_HASHTABLE) {
            while (count--) {
                listpackEntry field, value;
                hashTypeRandomElement(hash, size, &field, &value);

                /* In case we were unable to locate random element, it is probably because there is no such element
                 * since all elements are expired. */
                if (!field.sval) break;

                if (withvalues && c->resp > 2) addWritePreparedReplyArrayLen(wpc, 2);
                addWritePreparedReplyBulkCBuffer(wpc, field.sval, field.slen);
                if (withvalues) addWritePreparedReplyBulkCBuffer(wpc, value.sval, value.slen);
                if (c->flag.close_asap) break;
                reply_size++;
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
                reply_size += sample_count;
                lpRandomPairs(hash->ptr, sample_count, fields, vals);
                hrandfieldReplyWithListpack(wpc, sample_count, fields, vals);
                if (c->flag.close_asap) break;
            }
            zfree(fields);
            zfree(vals);
        }
        goto set_deferred_response;
    }

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
            reply_size++;
        }
        hashTypeResetIterator(&hi);

        goto set_deferred_response;
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
        reply_size = count < size ? count : size;
        listpackEntry *fields, *vals = NULL;
        fields = zmalloc(sizeof(listpackEntry) * count);
        if (withvalues) vals = zmalloc(sizeof(listpackEntry) * count);
        serverAssert(lpRandomPairsUnique(hash->ptr, count, fields, vals) == count);
        hrandfieldReplyWithListpack(wpc, count, fields, vals);
        zfree(fields);
        zfree(vals);
        goto set_deferred_response;
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
            reply_size++;
        }
        serverAssert(hashtableSize(ht) == reply_size);
        hashtableResetIterator(&iter);

        /* Remove random elements to reach the right count. */
        while (reply_size > count) {
            void *element;
            hashtableFairRandomEntry(ht, &element);
            hashtableDelete(ht, element);
            reply_size--;
        }

        /* Reply with what's in the temporary hashtable and release memory */
        hashtableInitIterator(&iter, ht, 0);
        void *next;
        while (hashtableNext(&iter, &next)) {
            sds field = entryGetField(next);
            sds value = entryGetValue(next);
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

            /* In case we were unable to locate random element, it is probably because there is no such element
             * since all elements are expired. */
            if (!field.sval) break;

            /* Try to add the object to the hashtable. If expired, stop adding (there are probably non left).
             * If it already exists free it, otherwise increment the number of objects we have
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
        reply_size = added;
    }

set_deferred_response:
    /* Set the reply count, RESP3 responds with nested array, RESP2 with flat one. */
    if (withvalues && c->resp == 2)
        setDeferredArrayLen(c, replylen, reply_size * 2);
    else
        setDeferredArrayLen(c, replylen, reply_size);
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

/* Context structure for tracking expiry operations on hash fields. */
typedef struct {
    robj *key;              /* the hash object */
    unsigned long n_fields; /* number of entries processed */
    robj **fields;          /* array of expired entries to replicate later */
} expiryContext;

/* Callback for popping expired entries from the volatile set.
 * Deletes the entry from the hash table and tracks it in the expiry context.
 * Returns 1 if deleted, 0 if nothing to do. */
static int hashTypeExpireEntry(void *entry, void *c) {
    expiryContext *ctx = c;
    robj *o = ctx->key;
    serverAssert(o->encoding == OBJ_ENCODING_HASHTABLE && hashtableSize(o->ptr) > 0);
    hashtable *ht = o->ptr;
    void *entry_ptr = NULL;
    bool deleted = hashtablePop(ht, entry, &entry_ptr);
    if (deleted) {
        if (ctx->fields)
            ctx->fields[ctx->n_fields++] = createStringObjectFromSds(entryGetField(entry));
        server.stat_expiredfields++;
        entryFree(entry);
        return 1;
    }
    return 0;
}

/* Extract expired entries from a hash object's volatile set.
 * Returns number of expired entries, populates `out_entries`. */
size_t hashTypeDeleteExpiredFields(robj *o, mstime_t now, unsigned long max_fields, robj **out_entries) {
    serverAssert(o->encoding == OBJ_ENCODING_HASHTABLE);

    vset *vset = hashTypeGetVolatileSet(o);
    if (!vset) {
        return 0;
    }

    serverAssert(!vsetIsEmpty(vset));
    /* skip TTL checks temporarily (to allow hashtable pops) */
    hashTypeIgnoreTTL(o, true);
    expiryContext ctx = {.key = o, .fields = out_entries, .n_fields = 0};
    size_t expired = vsetRemoveExpired(vset, entryGetExpiryVsetFunc, hashTypeExpireEntry, now, max_fields, &ctx);
    serverAssert(ctx.n_fields <= max_fields);
    if (vsetIsEmpty(vset)) {
        hashTypeFreeVolatileSet(o);
    } else {
        hashTypeIgnoreTTL(o, false);
    }
    return expired;
}

/* Hashtable scan callback for hash datatype */
static void defragHashTypeEntry(void *privdata, void *element_ref) {
    entry **entry_ref = (entry **)element_ref;
    entry *old_entry = *entry_ref;

    entry *new_entry = entryDefrag(old_entry, activeDefragAlloc, activeDefragSds);
    if (new_entry) {
        long long expiry = entryGetExpiry(new_entry);
        /* In case the entry is tracked we need to update it in the volatile set */
        if (expiry != EXPIRY_NONE) {
            // We don't need to pass the db because db-level tracking isn't going to change for this update.
            hashTypeTrackUpdateEntry(privdata, old_entry, new_entry, expiry, expiry);
        }
        *entry_ref = new_entry;
    }
}

size_t hashTypeScanDefrag(robj *ob, size_t cursor, void *(*defragAllocfn)(void *)) {
    if (ob->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *newzl;
        if ((newzl = activeDefragAlloc(ob->ptr))) ob->ptr = newzl;
        return 0;
    }
    serverAssert(ob->encoding == OBJ_ENCODING_HASHTABLE);
    static struct volatileSetCursor {
        size_t cursor;
        bool is_vsetDefrag;
    } volaSetIter;
    static struct volatileSetCursor *vset_cursor = NULL;

    vset_cursor = (struct volatileSetCursor *)cursor;

    if (!vset_cursor) {
        /* New object scan */
        hashtable *ht = ob->ptr;
        /* defrag the hashtable struct and tables */
        hashtable *new_hashtable = hashtableDefragTables(ht, defragAllocfn);
        if (new_hashtable) ob->ptr = new_hashtable;
        vset_cursor = &volaSetIter;
        vset_cursor->cursor = 0;
        vset_cursor->is_vsetDefrag = false;
    }

    if (!vset_cursor->is_vsetDefrag) {
        hashtable *ht = ob->ptr;
        vset_cursor->cursor = hashtableScanDefrag(ht, vset_cursor->cursor, defragHashTypeEntry, ob,
                                                  defragAllocfn,
                                                  HASHTABLE_SCAN_EMIT_REF);
        if (vset_cursor->cursor == 0) {
            if (hashTypeHasVolatileFields(ob)) {
                /* We're done scanning the hash table, continue to defrag the volatile set only if there's one. */
                vset_cursor->is_vsetDefrag = true;
            } else {
                /* We're done with this object. */
                return 0;
            }
        }
    } else {
        /* We're already defraging volatile set. */
        vset *vset = hashTypeGetVolatileSet(ob);
        vset_cursor->cursor = vsetScanDefrag(vset, vset_cursor->cursor, activeDefragAlloc);
        if (vset_cursor->cursor == 0) {
            /* We're done with this hash object. */
            return 0;
        }
    }
    return (long)vset_cursor;
}
