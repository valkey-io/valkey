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

#include "server.h"
#include "hashtable.h"
#include "intset.h" /* Compact integer set structure */
#include "expire.h"
#include "listpack.h"
#include "vset.h"

#define SET_ADD_KEEP_EXPIRY (1 << 0)

/* True when the intset plus 'added' members of at most 'len' bytes and 'extra_bytes'
 * of metadata fits the listpack limits; intset elements are budgeted at their widest. */
static bool setTypeIntsetFitsListpack(robj *setobj, size_t added, size_t len, size_t extra_bytes) {
    intset *is = objectGetVal(setobj);
    size_t maxelelen = 0, totsize = 0;
    unsigned long n = intsetLen(is);
    if (n != 0) {
        maxelelen = max(sdigits10(intsetMax(is)), sdigits10(intsetMin(is)));
        totsize = max(lpEstimateBytesRepeatedInteger(intsetMax(is), n), lpEstimateBytesRepeatedInteger(intsetMin(is), n));
    }
    return n + added <= server.set_max_listpack_entries && len <= server.set_max_listpack_value &&
           maxelelen <= server.set_max_listpack_value && lpSafeToAdd(NULL, totsize + len + extra_bytes);
}

/*-----------------------------------------------------------------------------
 * Member expiration
 *----------------------------------------------------------------------------*/

/* Member expiration for the SET type. A listpack set stores one entry per
 * member, so the metadata entry follows the member entry itself.
 *
 * Replicas and AOF loading apply commands under POLICY_IGNORE_EXPIRE, so an
 * expired member hidden here is live there: commands whose effect depends on
 * liveness propagate the members they changed, and copies and encoding
 * conversions carry expired members with their expiry. */

static long long smemberGetExpiryVsetFunc(const void *m) {
    return smemberGetExpiry((const smember *)m);
}

/*-----------------------------------------------------------------------------
 * Hashtable vset
 *----------------------------------------------------------------------------*/

static vset *setTypeVolatileIndex(robj *o) {
    serverAssert(objectGetEncoding(o) == OBJ_ENCODING_HASHTABLE);
    return hashtableMetadata(objectGetVal(o));
}

static vset *setTypeGetVolatileSet(robj *o) {
    vset *set = setTypeVolatileIndex(o);
    return vsetIsValid(set) ? set : NULL;
}

static vset *setTypeGetOrCreateVolatileIndex(robj *o) {
    vset *set = setTypeVolatileIndex(o);
    if (!vsetIsValid(set)) {
        vsetInit(set);
        hashtableSetType(objectGetVal(o), &setWithVolatileMembersHashtableType);
    }
    return set;
}

void setTypeFreeVolatileSet(robj *o) {
    vset *set = hashtableMetadata(objectGetVal(o));
    if (vsetIsValid(set)) vsetRelease(set);
    hashtableSetType(objectGetVal(o), &setHashtableType);
}

void setTypeTrackMember(robj *o, smember *m) {
    vset *set = setTypeGetOrCreateVolatileIndex(o);
    serverAssert(vsetAddEntry(set, smemberGetExpiryVsetFunc, m));
}

/* vsetUpdateEntry dispatches add / remove / move on (old_expiry, new_expiry). */
static void setTypeTrackUpdateMember(robj *o, smember *old, smember *new, mstime_t old_expiry, mstime_t new_expiry) {
    if (old_expiry == EXPIRY_NONE && new_expiry == EXPIRY_NONE) return;
    vset *set = setTypeGetOrCreateVolatileIndex(o);
    serverAssert(vsetUpdateEntry(set, smemberGetExpiryVsetFunc, old, new, old_expiry, new_expiry));
    if (vsetIsEmpty(set)) setTypeFreeVolatileSet(o);
}

/* Call before freeing 'm': vsetRemoveEntry reads its expiry. */
static void setTypeUntrackMember(robj *o, smember *m) {
    if (!smemberHasExpiry(m)) return;
    vset *set = setTypeVolatileIndex(o);
    debugServerAssert(vsetIsValid(set));
    serverAssert(vsetRemoveEntry(set, smemberGetExpiryVsetFunc, m));
    if (vsetIsEmpty(set)) setTypeFreeVolatileSet(o);
}

/* Include unreclaimed expired members when rebuilding the vset. */
static void setTypeTrackVolatileMembers(robj *o) {
    hashtable *ht = objectGetVal(o);
    hashtableIterator iter;
    hashtableInitIterator(&iter, ht, HASHTABLE_ITER_SKIP_VALIDATION);
    void *next;
    while (hashtableNext(&iter, &next)) {
        smember *m = next;
        if (!smemberHasExpiry(m)) continue;
        setTypeTrackMember(o, m);
    }
    hashtableCleanupIterator(&iter);
}

/*-----------------------------------------------------------------------------
 * Listpack helpers
 *----------------------------------------------------------------------------*/

long long setTypeListpackGetExpiry(unsigned char *lp, unsigned char *p) {
    unsigned char *metadata_ptr = lpGetMetadata(lp, p);
    return metadata_ptr ? lpGetMetadataValue(metadata_ptr) : EXPIRY_NONE;
}

static unsigned char *setTypeListpackFind(unsigned char *lp, sds member) {
    unsigned char *p = lpFirst(lp);
    if (p == NULL) return NULL;
    return lpFind(lp, p, (unsigned char *)member, sdslen(member), 0);
}

bool setTypeHasVolatileMembers(robj *o) {
    if (o == NULL) return false;
    int encoding = objectGetEncoding(o);
    if (encoding == OBJ_ENCODING_LISTPACK) return lpIsMetadata(lpStart(objectGetVal(o)));
    if (encoding == OBJ_ENCODING_HASHTABLE) {
        vset *set = setTypeGetVolatileSet(o);
        return set && !vsetIsEmpty(set);
    }
    return false;
}

bool setTypeHasExpiredMembers(robj *o) {
    if (!setTypeHasVolatileMembers(o) || getExpirationPolicyWithFlags(0) == POLICY_IGNORE_EXPIRE) return false;

    if (objectGetEncoding(o) == OBJ_ENCODING_HASHTABLE)
        return vsetHasHidden(setTypeGetVolatileSet(o), smemberGetExpiryVsetFunc, commandTimeSnapshot());

    serverAssert(objectGetEncoding(o) == OBJ_ENCODING_LISTPACK);
    unsigned char *lp = objectGetVal(o);
    for (unsigned char *p = lpFirst(lp); p != NULL; p = lpNext(lp, p)) {
        if (timestampIsExpired(setTypeListpackGetExpiry(lp, p))) return true;
    }
    return false;
}

void setTypeInitVolatileIterator(robj *o, vsetIterator *iter) {
    serverAssert(objectGetEncoding(o) == OBJ_ENCODING_HASHTABLE);
    vsetInitIterator(setTypeGetVolatileSet(o), iter);
}

long long setTypeVolatileCount(robj *o) {
    serverAssert(objectGetType(o) == OBJ_SET);

    switch (objectGetEncoding(o)) {
    case OBJ_ENCODING_LISTPACK: {
        unsigned char *p = lpStart(objectGetVal(o));
        return lpIsMetadata(p) ? lpGetMetadataValue(p) : 0;
    }
    case OBJ_ENCODING_HASHTABLE: {
        vset *set = setTypeGetVolatileSet(o);
        return set ? (long long)vsetSize(set) : 0;
    }
    case OBJ_ENCODING_INTSET: return 0;
    default: serverPanic("Unknown set encoding");
    }
}

int setTypeGetExpiry(robj *o, sds member, mstime_t *expiry) {
    *expiry = EXPIRY_NONE;

    if (objectGetEncoding(o) == OBJ_ENCODING_INTSET) return setTypeIsMember(o, member) ? C_OK : C_ERR;

    if (objectGetEncoding(o) == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = objectGetVal(o);
        unsigned char *p = setTypeListpackFind(lp, member);
        if (p == NULL) return C_ERR;
        long long member_expiry = setTypeListpackGetExpiry(lp, p);
        if (!listpackObjectItemIsValid(member_expiry)) return C_ERR;
        *expiry = member_expiry;
        return C_OK;
    }

    serverAssert(objectGetEncoding(o) == OBJ_ENCODING_HASHTABLE);
    void *found = NULL;
    if (!hashtableFind(objectGetVal(o), member, &found)) return C_ERR;
    *expiry = smemberGetExpiry(found);
    return C_OK;
}

void setTypeIgnoreTTL(robj *o, bool ignore) {
    if (objectGetEncoding(o) == OBJ_ENCODING_LISTPACK) {
        listpackObjectIgnoreTTL(ignore);
        return;
    }
    /* Clearing is done regardless of encoding so that a bracket whose object
     * was converted listpack->hashtable in between cannot leak the flag.
     * Setting is left to the listpack encoding: a hashtable carries its
     * ignore state in its type. */
    if (!ignore) listpackObjectIgnoreTTL(false);
    if (objectGetEncoding(o) != OBJ_ENCODING_HASHTABLE) return;

    if (!ignore && setTypeGetVolatileSet(o) == NULL) ignore = true;
    hashtableSetType(objectGetVal(o), ignore ? &setHashtableType : &setWithVolatileMembersHashtableType);
}

/*-----------------------------------------------------------------------------
 * Setting a member expiry
 *----------------------------------------------------------------------------*/

/* An intset cannot hold metadata; the listpack budget adds the count header and one expiry entry. */
static void setTypeConvertForExpiry(robj *o, size_t extra, size_t memberlen) {
    serverAssert(objectGetEncoding(o) == OBJ_ENCODING_INTSET);
    int enc = setTypeIntsetFitsListpack(o, extra, memberlen, 2 * LP_METADATA_MAX_ENTRY_BYTES)
                  ? OBJ_ENCODING_LISTPACK
                  : OBJ_ENCODING_HASHTABLE;
    setTypeConvertAndExpand(o, enc, intsetLen(objectGetVal(o)) + extra, 1);
}

/* Reallocates the listpack; 'p' is invalid afterwards. */
static void setTypeListpackSetExpiry(robj *o, unsigned char *p, mstime_t expiry) {
    unsigned char *lp = objectGetVal(o);
    unsigned char *metadata_ptr = lpGetMetadata(lp, p);

    if (expiry == EXPIRY_NONE) {
        lp = lpRemoveMetadata(lp, metadata_ptr);
        objectSetVal(o, lp);
        listpackObjectUpdateVolatileCount(o, -1);
        return;
    }

    unsigned char intenc[LP_MAX_INT_ENCODING_LEN];
    uint64_t enclen;
    lpEncodeIntegerGetType(expiry, intenc, &enclen);
    if (metadata_ptr) {
        lp = lpInsertMetadata(lp, intenc, enclen, metadata_ptr, LP_REPLACE, NULL);
    } else {
        lp = lpInsertMetadata(lp, intenc, enclen, p, LP_AFTER, NULL);
    }
    objectSetVal(o, lp);
    if (!metadata_ptr) listpackObjectUpdateVolatileCount(o, 1);
}

expiryModificationResult setTypeSetExpiry(robj *o, sds member, mstime_t expiry, int flags) {
    if (o == NULL) return EXPIRATION_MODIFICATION_NOT_EXIST;

    mstime_t current = EXPIRY_NONE;
    unsigned char *p = NULL;
    void **member_ref = NULL;
    smember *m = NULL;

    if (objectGetEncoding(o) == OBJ_ENCODING_LISTPACK) {
        p = setTypeListpackFind(objectGetVal(o), member);
        if (p == NULL) return EXPIRATION_MODIFICATION_NOT_EXIST;
        current = setTypeListpackGetExpiry(objectGetVal(o), p);
        /* Do not resurrect an expired member; active expiration propagates its SREM. */
        if (!listpackObjectItemIsValid(current)) return EXPIRATION_MODIFICATION_NOT_EXIST;
    } else if (objectGetEncoding(o) == OBJ_ENCODING_HASHTABLE) {
        member_ref = hashtableFindRef(objectGetVal(o), member);
        if (member_ref == NULL) return EXPIRATION_MODIFICATION_NOT_EXIST;
        m = *member_ref;
        current = smemberGetExpiry(m);
    } else if (!setTypeIsMember(o, member)) {
        return EXPIRATION_MODIFICATION_NOT_EXIST;
    }

    /* EXPIRY_NONE is treated as +inf: GT can never beat it, LT always does. */
    if ((flags & EXPIRE_NX) && current != EXPIRY_NONE) return EXPIRATION_MODIFICATION_FAILED_CONDITION;
    if ((flags & EXPIRE_XX) && current == EXPIRY_NONE) return EXPIRATION_MODIFICATION_FAILED_CONDITION;
    if ((flags & EXPIRE_GT) && (current == EXPIRY_NONE || expiry <= current))
        return EXPIRATION_MODIFICATION_FAILED_CONDITION;
    if ((flags & EXPIRE_LT) && current != EXPIRY_NONE && expiry >= current)
        return EXPIRATION_MODIFICATION_FAILED_CONDITION;

    if (expiry == EXPIRY_NONE && current == EXPIRY_NONE) return EXPIRATION_MODIFICATION_FAILED;
    if (expiry != EXPIRY_NONE && checkAlreadyExpired(expiry)) {
        serverAssert(setTypeRemove(o, member));
        return EXPIRATION_MODIFICATION_EXPIRE_ASAP;
    }

    if (objectGetEncoding(o) == OBJ_ENCODING_INTSET) {
        setTypeConvertForExpiry(o, 0, 0);
        if (objectGetEncoding(o) == OBJ_ENCODING_LISTPACK) {
            p = setTypeListpackFind(objectGetVal(o), member);
        } else {
            member_ref = hashtableFindRef(objectGetVal(o), member);
            m = *member_ref;
        }
    }

    if (p != NULL) {
        setTypeListpackSetExpiry(o, p, expiry);
        return EXPIRATION_MODIFICATION_SUCCESSFUL;
    }

    smember *updated = smemberSetExpiry(m, expiry);
    *member_ref = updated;
    setTypeTrackUpdateMember(o, m, updated, current, expiry);
    return EXPIRATION_MODIFICATION_SUCCESSFUL;
}

/*-----------------------------------------------------------------------------
 * Adding a member with an expiry
 *----------------------------------------------------------------------------*/

static void setTypeAddNewWithExpiry(robj *o, sds member, mstime_t expiry) {
    size_t len = sdslen(member);

    if (objectGetEncoding(o) == OBJ_ENCODING_INTSET) setTypeConvertForExpiry(o, 1, len);

    if (objectGetEncoding(o) == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = objectGetVal(o);
        size_t add_bytes = len;
        if (expiry != EXPIRY_NONE) add_bytes += 2 * LP_METADATA_MAX_ENTRY_BYTES;
        if (lpLength(lp) < server.set_max_listpack_entries && len <= server.set_max_listpack_value &&
            lpSafeToAdd(lp, add_bytes)) {
            lp = lpAppend(lp, (unsigned char *)member, len);
            objectSetVal(o, lp);
            if (expiry != EXPIRY_NONE) setTypeListpackSetExpiry(o, lpLast(lp), expiry);
            return;
        }
        setTypeConvertAndExpand(o, OBJ_ENCODING_HASHTABLE, lpLength(lp) + 1, 1);
    }

    serverAssert(objectGetEncoding(o) == OBJ_ENCODING_HASHTABLE);
    smember *m = smemberCreate(member, len, expiry);
    serverAssert(hashtableAdd(objectGetVal(o), m));
    if (expiry != EXPIRY_NONE) setTypeTrackMember(o, m);
}

static void setTypeUpdateHashtableMemberExpiry(robj *o, smember *m, mstime_t current, mstime_t expiry) {
    smember *updated = smemberSetExpiry(m, expiry);
    if (updated != m) serverAssert(hashtableReplaceReallocatedEntry(objectGetVal(o), m, updated));
    setTypeTrackUpdateMember(o, m, updated, current, expiry);
}

static int setTypeAddWithExpiry(robj *o, sds member, mstime_t expiry, int flags, bool *replaced_expired, bool *ttl_changed) {
    if (expiry == EXPIRY_NONE && !setTypeHasVolatileMembers(o)) return setTypeAdd(o, member);

    if (objectGetEncoding(o) == OBJ_ENCODING_HASHTABLE) {
        hashtable *ht = objectGetVal(o);
        hashtablePosition position;
        void *existing;
        setTypeIgnoreTTL(o, true);
        if (hashtableFindPositionForInsert(ht, member, &position, &existing)) {
            smember *m = smemberCreate(member, sdslen(member), expiry);
            hashtableInsertAtPosition(ht, m, &position);
            if (expiry != EXPIRY_NONE) setTypeTrackMember(o, m);
            setTypeIgnoreTTL(o, false);
            return 1;
        }
        smember *m = existing;
        mstime_t current = smemberGetExpiry(m);
        bool expired = getExpirationPolicyWithFlags(0) != POLICY_IGNORE_EXPIRE && smemberIsExpired(m);
        if ((expired || !(flags & SET_ADD_KEEP_EXPIRY)) && current != expiry) {
            setTypeUpdateHashtableMemberExpiry(o, m, current, expiry);
            if (!expired && ttl_changed) *ttl_changed = true;
        }
        setTypeIgnoreTTL(o, false);
        if (expired && replaced_expired) *replaced_expired = true;
        return expired;
    }

    if (objectGetEncoding(o) == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = objectGetVal(o);
        unsigned char *p = setTypeListpackFind(lp, member);
        if (p != NULL) {
            mstime_t current = setTypeListpackGetExpiry(lp, p);
            bool expired = !listpackObjectItemIsValid(current);
            if ((expired || !(flags & SET_ADD_KEEP_EXPIRY)) && current != expiry) {
                setTypeListpackSetExpiry(o, p, expiry);
                if (!expired && ttl_changed) *ttl_changed = true;
            }
            if (expired && replaced_expired) *replaced_expired = true;
            return expired;
        }
        setTypeAddNewWithExpiry(o, member, expiry);
        return 1;
    }

    /* Intsets cannot carry TTLs. Convert only when an existing member's TTL changes. */
    if (setTypeIsMember(o, member)) {
        if (!(flags & SET_ADD_KEEP_EXPIRY) && expiry != EXPIRY_NONE) {
            serverAssert(setTypeSetExpiry(o, member, expiry, 0) == EXPIRATION_MODIFICATION_SUCCESSFUL);
            if (ttl_changed) *ttl_changed = true;
        }
        return 0;
    }
    setTypeAddNewWithExpiry(o, member, expiry);
    return 1;
}

/*-----------------------------------------------------------------------------
 * Active expiration
 *----------------------------------------------------------------------------*/

typedef struct {
    robj *o;
    robj **members;
    size_t nmembers;
} setExpiryContext;

static int setTypeExpireMember(void *entry, void *c) {
    setExpiryContext *ctx = c;
    smember *m = entry;
    serverAssert(hashtablePop(objectGetVal(ctx->o), m, NULL));
    if (ctx->members) ctx->members[ctx->nmembers++] = createStringObjectFromSds(m);
    smemberFree(m);
    return 1;
}

size_t setTypeDeleteExpiredMembers(robj *o, mstime_t now, unsigned long max_members, robj **out_members) {
    if (!setTypeHasVolatileMembers(o)) return 0;

    if (objectGetEncoding(o) == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = objectGetVal(o);
        unsigned char *p = lpFirst(lp);
        size_t expired = 0;
        unsigned char intbuf[LP_INTBUF_SIZE];

        while (p != NULL && expired < max_members) {
            mstime_t expiry = setTypeListpackGetExpiry(lp, p);
            if (expiry != EXPIRY_NONE && expiry <= now) {
                if (out_members) {
                    int64_t len;
                    unsigned char *str = lpGet(p, &len, intbuf);
                    out_members[expired] = createStringObject((char *)str, len);
                }
                lp = lpDeleteRangeWithEntry(lp, &p, 1);
                objectSetVal(o, lp);
                expired++;
                continue;
            }
            p = lpNext(lp, p);
        }

        listpackObjectUpdateVolatileCount(o, -(long)expired);
        server.stat_expiredsetmembers += expired;
        return expired;
    }

    serverAssert(objectGetEncoding(o) == OBJ_ENCODING_HASHTABLE);
    vset *set = setTypeGetVolatileSet(o);

    /* The pops must see the expired members they remove. */
    setTypeIgnoreTTL(o, true);
    setExpiryContext ctx = {.o = o, .members = out_members, .nmembers = 0};
    size_t expired = vsetRemoveExpired(set, smemberGetExpiryVsetFunc, setTypeExpireMember, now, max_members, &ctx);
    serverAssert(ctx.nmembers <= max_members);
    if (vsetIsEmpty(set))
        setTypeFreeVolatileSet(o);
    else
        setTypeIgnoreTTL(o, false);
    server.stat_expiredsetmembers += expired;
    return expired;
}

/*-----------------------------------------------------------------------------
 * Defrag
 *----------------------------------------------------------------------------*/

typedef struct {
    robj *o;
    void *(*defragfn)(void *);
} setDefragMemberCtx;

static void defragSetMemberCallback(void *privdata, void *element_ref) {
    setDefragMemberCtx *ctx = privdata;
    smember **member_ref = element_ref;
    smember *m = *member_ref;
    smember *new_m = smemberDefrag(m, ctx->defragfn);
    if (new_m == NULL) return;
    if (smemberHasExpiry(new_m)) {
        mstime_t expiry = smemberGetExpiry(new_m);
        setTypeTrackUpdateMember(ctx->o, m, new_m, expiry, expiry);
    }
    *member_ref = new_m;
}

size_t setTypeScanDefrag(robj *o, size_t cursor, void *(*defragfn)(void *)) {
    /* Only one object is defragged at a time, so one static cursor state is enough. */
    static struct setDefragCursor {
        size_t cursor;
        bool in_vset_phase;
    } state;
    struct setDefragCursor *st = (struct setDefragCursor *)cursor;

    if (st == NULL) {
        hashtable *relocated = hashtableDefragTables(objectGetVal(o), defragfn);
        if (relocated) objectSetVal(o, relocated);
        st = &state;
        st->cursor = 0;
        st->in_vset_phase = false;
    }

    if (!st->in_vset_phase) {
        setDefragMemberCtx ctx = {.o = o, .defragfn = defragfn};
        st->cursor = hashtableScanDefrag(objectGetVal(o), st->cursor, defragSetMemberCallback, &ctx, defragfn,
                                         HASHTABLE_SCAN_EMIT_REF);
        if (st->cursor == 0) {
            if (setTypeGetVolatileSet(o) == NULL) return 0;
            st->in_vset_phase = true;
        }
    } else {
        vset *set = setTypeGetVolatileSet(o);
        /* SPERSIST, SREM, replacement or recreation can release the volatile-member
         * index between deferred active-defrag steps; resuming is then done. */
        if (set == NULL) return 0;
        st->cursor = vsetScanDefrag(set, st->cursor, defragfn);
        if (st->cursor == 0) return 0;
    }
    return (size_t)st;
}

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum, robj *dstkey, int op);

bool setTypeListpackIsValidAt(unsigned char *lp, unsigned char *p) {
    /* A listpack with no volatile members carries no metadata header, so every
     * entry is live without decoding it for an expiry tag. */
    return !lpIsMetadata(lpStart(lp)) || listpackObjectItemIsValid(setTypeListpackGetExpiry(lp, p));
}

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise, a listpack
 * or a regular hash table.
 *
 * The size hint indicates approximately how many items will be added which is
 * used to determine the initial representation. */
robj *setTypeCreate(sds value, size_t size_hint) {
    if (isSdsRepresentableAsLongLong(value, NULL) == C_OK && size_hint <= server.set_max_intset_entries)
        return createIntsetObject();
    if (size_hint <= server.set_max_listpack_entries) return createSetListpackObject();

    /* We may oversize the set by using the hint if the hint is not accurate,
     * but we will assume this is acceptable to maximize performance. */
    robj *o = createSetObject();
    hashtableExpand(objectGetVal(o), size_hint);
    return o;
}

/* Check if the existing set should be converted to another encoding based off the
 * the size hint. */
void setTypeMaybeConvert(robj *set, size_t size_hint) {
    if ((set->encoding == OBJ_ENCODING_LISTPACK && size_hint > server.set_max_listpack_entries) ||
        (set->encoding == OBJ_ENCODING_INTSET && size_hint > server.set_max_intset_entries)) {
        setTypeConvertAndExpand(set, OBJ_ENCODING_HASHTABLE, size_hint, 1);
    }
}

/* Return the maximum number of entries to store in an intset. */
static size_t intsetMaxEntries(void) {
    size_t max_entries = server.set_max_intset_entries;
    /* limit to 1G entries due to intset internals. */
    if (max_entries >= 1 << 30) max_entries = 1 << 30;
    return max_entries;
}

/* Converts intset to HT if it contains too many entries. */
static void maybeConvertIntset(robj *subject) {
    serverAssert(subject->encoding == OBJ_ENCODING_INTSET);
    if (intsetLen(objectGetVal(subject)) > intsetMaxEntries()) setTypeConvert(subject, OBJ_ENCODING_HASHTABLE);
}

/* When you know all set elements are integers, call this to convert the set to
 * an intset. No conversion happens if the set contains too many entries for an
 * intset. */
static void maybeConvertToIntset(robj *set) {
    if (set->encoding == OBJ_ENCODING_INTSET) return;  /* already intset */
    if (setTypeSize(set) > intsetMaxEntries()) return; /* can't use intset */
    intset *is = intsetNew();
    char *str;
    size_t len;
    int64_t llval;
    setTypeIterator *si = setTypeInitIterator(set);
    while (setTypeNext(si, &str, &len, &llval) != -1) {
        if (str) {
            /* If the element is returned as a string, we may be able to convert
             * it to integer. This happens for OBJ_ENCODING_HASHTABLE. */
            serverAssert(string2ll(str, len, (long long *)&llval));
        }
        uint8_t success = 0;
        is = intsetAdd(is, llval, &success);
        serverAssert(success);
    }
    setTypeReleaseIterator(si);
    freeSetObject(set); /* frees the internals but not robj itself */
    objectSetVal(set, is);
    set->encoding = OBJ_ENCODING_INTSET;
}

/* Add the specified sds value into a set.
 *
 * If the value was already member of the set, nothing is done and 0 is
 * returned; otherwise, the new element is added and 1 is returned. */
int setTypeAdd(robj *subject, sds value) {
    return setTypeAddAux(subject, value, sdslen(value), 0, 1);
}

/* Add member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was added and 0 if it was already a member. */
int setTypeAddAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    debugServerAssert(!setTypeHasVolatileMembers(set));
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (set->encoding == OBJ_ENCODING_INTSET) {
            uint8_t success = 0;
            objectSetVal(set, intsetAdd(objectGetVal(set), llval, &success));
            if (success) maybeConvertIntset(set);
            return success;
        }
        /* Convert int to string. */
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    serverAssert(str);
    if (set->encoding == OBJ_ENCODING_HASHTABLE) {
        /* Avoid duping the string if it is an sds string. */
        sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
        hashtable *ht = objectGetVal(set);
        hashtablePosition position;
        if (hashtableFindPositionForInsert(ht, sdsval, &position, NULL)) {
            /* Key doesn't already exist in the set. Add it but dup the key. */
            if (sdsval == str) sdsval = sdsdup(sdsval);
            hashtableInsertAtPosition(ht, sdsval, &position);
            return 1;
        } else if (sdsval != str) {
            /* String is already a member. Free our temporary sds copy. */
            sdsfree(sdsval);
            return 0;
        }
    } else if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = objectGetVal(set);
        unsigned char *p = lpFirst(lp);
        if (p != NULL) p = lpFind(lp, p, (unsigned char *)str, len, 0);
        if (p == NULL) {
            /* Not found.  */
            if (lpLength(lp) < server.set_max_listpack_entries && len <= server.set_max_listpack_value &&
                lpSafeToAdd(lp, len)) {
                if (str == tmpbuf) {
                    /* This came in as integer so we can avoid parsing it again.
                     * TODO: Create and use lpFindInteger; don't go via string. */
                    lp = lpAppendInteger(lp, llval);
                } else {
                    lp = lpAppend(lp, (unsigned char *)str, len);
                }
                objectSetVal(set, lp);
            } else {
                /* Size limit is reached. Convert to hashtable and add. */
                setTypeConvertAndExpand(set, OBJ_ENCODING_HASHTABLE, lpLength(lp) + 1, 1);
                serverAssert(hashtableAdd(objectGetVal(set), sdsnewlen(str, len)));
            }
            return 1;
        }
    } else if (set->encoding == OBJ_ENCODING_INTSET) {
        long long value;
        if (string2ll(str, len, &value)) {
            uint8_t success = 0;
            objectSetVal(set, intsetAdd(objectGetVal(set), value, &success));
            if (success) {
                maybeConvertIntset(set);
                return 1;
            }
        } else {
            if (setTypeIntsetFitsListpack(set, true, len, 0)) {
                setTypeConvertAndExpand(set, OBJ_ENCODING_LISTPACK, intsetLen(objectGetVal(set)) + 1, 1);
                unsigned char *lp = objectGetVal(set);
                lp = lpAppend(lp, (unsigned char *)str, len);
                lp = lpShrinkToFit(lp);
                objectSetVal(set, lp);
                return 1;
            } else {
                setTypeConvertAndExpand(set, OBJ_ENCODING_HASHTABLE, intsetLen(objectGetVal(set)) + 1, 1);
                /* The set *was* an intset and this value is not integer
                 * encodable, so hashtableAdd should always work. */
                serverAssert(hashtableAdd(objectGetVal(set), sdsnewlen(str, len)));
                return 1;
            }
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* Add a member taken from another set, as setTypeAddAux() takes it. A member
 * without an expiry is inserted raw, which needs no sds of its own, as long as
 * no earlier member brought a TTL into 'set': setTypeAddAux() does not maintain
 * the volatile member index. */
static void setTypeAddMovedMember(robj *set, char *str, size_t len, int64_t llval, int str_is_sds, mstime_t expiry) {
    if (expiry == EXPIRY_NONE && !setTypeHasVolatileMembers(set)) {
        setTypeAddAux(set, str, len, llval, str_is_sds);
        return;
    }
    sds member = str_is_sds ? (sds)str : (str ? sdsnewlen(str, len) : sdsfromlonglong(llval));
    setTypeAddWithExpiry(set, member, expiry, 0, NULL, NULL);
    if (member != str) sdsfree(member);
}

/* Deletes a value provided as an sds string from the set. Returns 1 if the
 * value was deleted and 0 if it was not a member of the set. */
int setTypeRemove(robj *setobj, sds value) {
    return setTypeRemoveAux(setobj, value, sdslen(value), 0, 1);
}

/* Remove a member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was deleted and 0 if it was not a member of the set. */
int setTypeRemoveAux(robj *setobj, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (setobj->encoding == OBJ_ENCODING_INTSET) {
            int success;
            objectSetVal(setobj, intsetRemove(objectGetVal(setobj), llval, &success));
            return success;
        }
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (setobj->encoding == OBJ_ENCODING_HASHTABLE) {
        sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
        void *popped;
        int deleted = hashtablePop(objectGetVal(setobj), sdsval, &popped);
        if (deleted) {
            setTypeUntrackMember(setobj, popped);
            smemberFree(popped);
        }
        if (sdsval != str) sdsfree(sdsval); /* free temp copy */
        return deleted;
    } else if (setobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = objectGetVal(setobj);
        unsigned char *p = lpFirst(lp);
        if (p == NULL) return 0;
        p = lpFind(lp, p, (unsigned char *)str, len, 0);
        if (p != NULL) {
            mstime_t expiry = setTypeListpackGetExpiry(lp, p);
            if (!listpackObjectItemIsValid(expiry)) return 0;
            lp = lpDeleteRangeWithEntry(lp, &p, 1);
            objectSetVal(setobj, lp);
            if (expiry != EXPIRY_NONE) listpackObjectUpdateVolatileCount(setobj, -1);
            return 1;
        }
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        long long llval;
        if (string2ll(str, len, &llval)) {
            int success;
            objectSetVal(setobj, intsetRemove(objectGetVal(setobj), llval, &success));
            if (success) return 1;
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* Check if an sds string is a member of the set. Returns 1 if the value is a
 * member of the set and 0 if it isn't. */
int setTypeIsMember(robj *subject, sds value) {
    return setTypeIsMemberAux(subject, value, sdslen(value), 0, 1);
}

/* Membership checking optimized for the different encodings. The value can be
 * provided as an sds string (indicated by passing str_is_sds = 1), as string
 * and length (str_is_sds = 0) or as an integer in which case str is set to NULL
 * and llval is provided instead.
 *
 * Returns 1 if the value is a member of the set and 0 if it isn't. */
int setTypeIsMemberAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (set->encoding == OBJ_ENCODING_INTSET) return intsetFind(objectGetVal(set), llval);
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = objectGetVal(set);
        unsigned char *p = lpFirst(lp);
        if (p == NULL) return 0;
        p = lpFind(lp, p, (unsigned char *)str, len, 0);
        return p && setTypeListpackIsValidAt(lp, p);
    } else if (set->encoding == OBJ_ENCODING_INTSET) {
        long long llval;
        return string2ll(str, len, &llval) && intsetFind(objectGetVal(set), llval);
    } else if (set->encoding == OBJ_ENCODING_HASHTABLE && str_is_sds) {
        return hashtableFind(objectGetVal(set), (sds)str, NULL);
    } else if (set->encoding == OBJ_ENCODING_HASHTABLE) {
        sds sdsval = sdsnewlen(str, len);
        int result = hashtableFind(objectGetVal(set), sdsval, NULL);
        sdsfree(sdsval);
        return result;
    } else {
        serverPanic("Unknown set encoding");
    }
}

setTypeIterator *setTypeInitIterator(robj *subject) {
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = subject->encoding;
    if (si->encoding == OBJ_ENCODING_HASHTABLE) {
        si->hashtable_iterator = hashtableCreateIterator(objectGetVal(subject), 0);
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        si->ii = 0;
    } else if (si->encoding == OBJ_ENCODING_LISTPACK) {
        si->lpi = NULL;
    } else {
        serverPanic("Unknown set encoding");
    }
    return si;
}

void setTypeReleaseIterator(setTypeIterator *si) {
    if (si->encoding == OBJ_ENCODING_HASHTABLE) hashtableReleaseIterator(si->hashtable_iterator);
    zfree(si);
}

/* Move to the next entry in the set. Returns the object at the current
 * position, as a string or as an integer.
 *
 * Since set elements can be internally be stored as SDS strings, char buffers or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointers
 * (str and len) or (llele) depending on whether the value is stored as a string
 * or as an integer internally.
 *
 * If OBJ_ENCODING_HASHTABLE is returned, then str points to an sds string and can be
 * used as such. If OBJ_ENCODING_INTSET, then llele is populated and str is
 * pointed to NULL. If OBJ_ENCODING_LISTPACK is returned, the value can be
 * either a string or an integer. If *str is not NULL, then str and len are
 * populated with the string content and length. Otherwise, llele populated with
 * an integer value.
 *
 * Note that str, len and llele pointers should all be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused.
 *
 * When there are no more elements -1 is returned. */
int setTypeNext(setTypeIterator *si, char **str, size_t *len, int64_t *llele) {
    if (si->encoding == OBJ_ENCODING_HASHTABLE) {
        void *next;
        if (!hashtableNext(si->hashtable_iterator, &next)) return -1;
        *str = next;
        *len = sdslen(*str);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        if (!intsetGet(objectGetVal(si->subject), si->ii++, llele)) return -1;
        *str = NULL;
    } else if (si->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = objectGetVal(si->subject);
        unsigned char *lpi = si->lpi;
        if (lpi == NULL) {
            lpi = lpFirst(lp);
        } else {
            lpi = lpNext(lp, lpi);
        }
        while (lpi != NULL && !setTypeListpackIsValidAt(lp, lpi)) lpi = lpNext(lp, lpi);
        if (lpi == NULL) return -1;
        si->lpi = lpi;
        unsigned int l;
        *str = (char *)lpGetValue(lpi, &l, (long long *)llele);
        *len = (size_t)l;
    } else {
        serverPanic("Wrong set encoding in setTypeNext");
    }
    return si->encoding;
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new SDS
 * strings. So if you don't retain a pointer to this object you should call
 * sdsfree() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue. */
sds setTypeNextObject(setTypeIterator *si) {
    int64_t intele;
    char *str;
    size_t len;

    if (setTypeNext(si, &str, &len, &intele) == -1) return NULL;
    if (str != NULL) return sdsnewlen(str, len);
    return sdsfromlonglong(intele);
}

mstime_t setTypeCurrentExpiry(setTypeIterator *si, const char *str) {
    switch (si->encoding) {
    case OBJ_ENCODING_HASHTABLE: return smemberGetExpiry((const smember *)str);
    case OBJ_ENCODING_LISTPACK: return setTypeListpackGetExpiry(objectGetVal(si->subject), si->lpi);
    default: return EXPIRY_NONE;
    }
}

/*-----------------------------------------------------------------------------
 * Drawing live members
 *----------------------------------------------------------------------------*/

/* setTypeSize() counts hidden members, which no reply may hold. A set that may
 * hide one is served in up to three steps, each used only when the previous one
 * cannot finish the request:
 *
 * 1. When every member of a hashtable set carries a TTL, its expiry index holds
 *    them all, and a walk of the newest buckets collects the live members when
 *    there are few of them (setLiveIndexCap()). The bucket keys prove most
 *    buckets wholly live or wholly hidden, so an all-hidden set is usually
 *    recognised without reading a member.
 * 2. Otherwise the plain set's sampler draws and rejects a pick that lands on a
 *    hidden member, costing 1 / (1 - h) picks for a hidden fraction h. The
 *    picks a command may reject grow with its request and are capped at about
 *    half a pass over the set.
 * 3. The draws still owed come from one pass over the members, which knows how
 *    many are live (the members without a TTL plus the index's live ones): a
 *    rank walk for a few draws, selection sampling for distinct members.
 *
 * A listpack has no O(1) random access and its plain-set samplers walk it, so a
 * listpack holding a hidden member is served by single validating passes. A set
 * that provably hides nothing takes the plain set's paths. */
typedef struct {
    robj *set;
    bool hides;           /* A stored member may be hidden in this execution context. */
    unsigned long budget; /* Picks the command may still reject. */
} setLiveSampler;

#define SET_LIVE_INDEX_MAX 512 /* Live members step 1 collects before deferring to sampling. */
#define SET_RANK_WALK_MAX 64   /* Draws one rank walk serves; more draw from the collected live members. */

/* False proves that no member is hidden in this execution context. Reads at
 * most one vector of the expiry index, or walks a listpack. */
static bool setTypeMayHideMembers(robj *o) {
    if (!setTypeHasVolatileMembers(o) || getExpirationPolicyWithFlags(0) == POLICY_IGNORE_EXPIRE) return false;
    if (objectGetEncoding(o) == OBJ_ENCODING_HASHTABLE)
        return vsetMayHaveHidden(setTypeGetVolatileSet(o), smemberGetExpiryVsetFunc, commandTimeSnapshot());
    return setTypeHasExpiredMembers(o);
}

static void setLiveSamplerInit(setLiveSampler *s, robj *set, unsigned long draws) {
    s->set = set;
    s->hides = setTypeMayHideMembers(set);
    unsigned long by_request = draws > ULONG_MAX / 16 ? ULONG_MAX : 32 + 8 * draws;
    unsigned long by_size = 32 + setTypeSize(set) / 128;
    s->budget = by_request < by_size ? by_request : by_size;
}

/* Step 1 collects at most this many: few enough to keep on the stack, and fewer
 * than half the set, past which step 2's draws are the cheaper route. */
static size_t setLiveIndexCap(robj *set) {
    size_t half = setTypeSize(set) / 2;
    return half < SET_LIVE_INDEX_MAX ? half : SET_LIVE_INDEX_MAX;
}

/* Step 1: the live members of a hashtable set whose members all carry a TTL,
 * when at most 'cap' are live. Returns how many, or -1 when the index cannot
 * answer: a member has no TTL, or more than 'cap' are live. The pointers borrow
 * the set's members. */
static long setTypeCollectLiveIndexed(robj *set, smember **out, size_t cap) {
    if (set->encoding != OBJ_ENCODING_HASHTABLE) return -1;
    vset *index = setTypeGetVolatileSet(set);
    if (!index || vsetSize(index) != setTypeSize(set)) return -1;
    size_t live = vsetCollectLive(index, smemberGetExpiryVsetFunc, commandTimeSnapshot(), cap, (void **)out);
    return live > cap ? -1 : (long)live;
}

/* The exact number of live members of a hashtable set: those without a TTL and
 * the index's live ones. */
static unsigned long setTypeLiveCount(robj *set) {
    vset *index = setTypeGetVolatileSet(set);
    if (!index) return setTypeSize(set);
    unsigned long persistent = setTypeSize(set) - vsetSize(index);
    return persistent + vsetCountLive(index, smemberGetExpiryVsetFunc, commandTimeSnapshot());
}

/* Draw a live member of a hashtable set, or NULL once the budget is spent. */
static smember *setLiveSamplerDraw(setLiveSampler *s) {
    serverAssert(s->set->encoding == OBJ_ENCODING_HASHTABLE);
    void *entry = NULL;
    /* Bracketed: under the validating type hashtableFairRandomEntry loops until it has sampled
     * enough live members, forever when fewer remain than its sample size. */
    setTypeIgnoreTTL(s->set, true);
    while (hashtableFairRandomEntry(objectGetVal(s->set), &entry)) {
        if (!s->hides || !smemberIsExpired(entry)) break;
        entry = NULL;
        if (s->budget == 0) break;
        s->budget--;
    }
    setTypeIgnoreTTL(s->set, false);
    return entry;
}

/* Collect the live members in one pass. The entries borrow the set's storage. */
static listpackEntry *setTypeCollectLive(robj *set, unsigned long *count) {
    char *str = NULL;
    size_t len = 0;
    int64_t llele = 0;
    unsigned long cap = 16, held = 0;
    listpackEntry *buf = zmalloc(sizeof(*buf) * cap);
    setTypeIterator *si = setTypeInitIterator(set);
    while (setTypeNext(si, &str, &len, &llele) != -1) {
        if (held == cap) {
            cap *= 2;
            buf = zrealloc(buf, sizeof(*buf) * cap);
        }
        buf[held++] = (listpackEntry){.sval = (unsigned char *)str, .slen = len, .lval = llele};
    }
    setTypeReleaseIterator(si);
    *count = held;
    return buf;
}

typedef struct {
    unsigned long rank;
    unsigned int draw;
} setRankDraw;

static int setRankDrawCompare(const void *a, const void *b) {
    unsigned long ra = ((const setRankDraw *)a)->rank, rb = ((const setRankDraw *)b)->rank;
    return (ra > rb) - (ra < rb);
}

/* Step 3 for draws with replacement: 'n' uniform ranks over the 'live' members
 * of a hashtable set, resolved by one walk that stops at the highest rank. The
 * pointers borrow the set's members. */
static void setTypePickLiveByRank(robj *set, unsigned long live, unsigned int n, smember **out) {
    setRankDraw draws[SET_RANK_WALK_MAX];
    serverAssert(set->encoding == OBJ_ENCODING_HASHTABLE && live > 0 && n <= SET_RANK_WALK_MAX);
    for (unsigned int i = 0; i < n; i++) draws[i] = (setRankDraw){.rank = (unsigned long)rand() % live, .draw = i};
    qsort(draws, n, sizeof(draws[0]), setRankDrawCompare);
    hashtableIterator iter;
    hashtableInitIterator(&iter, objectGetVal(set), 0);
    unsigned long seen = 0;
    unsigned int resolved = 0;
    void *next;
    while (resolved < n && hashtableNext(&iter, &next)) {
        while (resolved < n && draws[resolved].rank == seen) out[draws[resolved++].draw] = next;
        seen++;
    }
    hashtableCleanupIterator(&iter);
    serverAssert(resolved == n);
}

static int setTypeHashtableMemberResult(void *entry, char **str, size_t *len, int64_t *llele) {
    *str = entry;
    *len = sdslen((sds)entry);
    *llele = -123456789; /* Not needed. Defensive. */
    return OBJ_ENCODING_HASHTABLE;
}

/* Pick uniformly among the live members in one pass. Returns -1 when none is live. */
static int setTypePickLiveByPass(robj *set, char **str, size_t *len, int64_t *llele) {
    char *s = NULL;
    size_t l = 0;
    int64_t v = 0;
    int encoding, picked = -1;
    unsigned long seen = 0;
    setTypeIterator *si = setTypeInitIterator(set);
    while ((encoding = setTypeNext(si, &s, &l, &v)) != -1) {
        if ((unsigned long)rand() % ++seen != 0) continue;
        *str = s;
        *len = l;
        *llele = v;
        picked = encoding;
    }
    setTypeReleaseIterator(si);
    return picked;
}

/* Return a uniformly selected live member, or -1 if none is live. */
static int setLiveSamplerPick(setLiveSampler *s, char **str, size_t *len, int64_t *llele) {
    robj *set = s->set;
    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = objectGetVal(set);
        unsigned char *p = lpSeek(lp, rand() % lpLength(lp));
        if (setTypeListpackIsValidAt(lp, p)) {
            unsigned int l;
            *str = (char *)lpGetValue(p, &l, (long long *)llele);
            *len = (size_t)l;
            return OBJ_ENCODING_LISTPACK;
        }
        return setTypePickLiveByPass(set, str, len, llele);
    }

    serverAssert(set->encoding == OBJ_ENCODING_HASHTABLE);
    smember *indexed[SET_LIVE_INDEX_MAX];
    long held = setTypeCollectLiveIndexed(set, indexed, setLiveIndexCap(set));
    if (held == 0) return -1;
    smember *m = held > 0 ? indexed[rand() % held] : setLiveSamplerDraw(s);
    if (m == NULL) {
        unsigned long live = setTypeLiveCount(set);
        if (live == 0) return -1;
        setTypePickLiveByRank(set, live, 1, &m);
    }
    return setTypeHashtableMemberResult(m, str, len, llele);
}

/* Return random element from a non empty set.
 * The returned element can be an int64_t value if the set is encoded
 * as an "intset" blob of integers, or a string.
 *
 * The caller provides three pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and can be used by the caller to check if the
 * int64_t pointer or the str and len pointers were populated, as for
 * setTypeNext. If OBJ_ENCODING_HASHTABLE is returned, str is pointed to a
 * string which is actually an sds string and it can be used as such.
 *
 * Note that both the str, len and llele pointers should be passed and cannot
 * be NULL. If str is set to NULL, the value is an integer stored in llele.
 *
 * Returns -1 when the set has volatile members and none of them is live. */
int setTypeRandomElement(robj *setobj, char **str, size_t *len, int64_t *llele) {
    bool volatile_set = setTypeHasVolatileMembers(setobj);
    if (volatile_set) {
        setLiveSampler sampler;
        setLiveSamplerInit(&sampler, setobj, 1);
        if (sampler.hides) return setLiveSamplerPick(&sampler, str, len, llele);
    }
    if (setobj->encoding == OBJ_ENCODING_HASHTABLE) {
        void *entry = NULL;
        /* Nothing is hidden, so the sampler need not read every sampled member's expiry. */
        if (volatile_set) setTypeIgnoreTTL(setobj, true);
        hashtableFairRandomEntry(objectGetVal(setobj), &entry);
        if (volatile_set) setTypeIgnoreTTL(setobj, false);
        *str = entry;
        *len = sdslen(*str);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        *llele = intsetRandom(objectGetVal(setobj));
        *str = NULL; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = objectGetVal(setobj);
        int r = rand() % lpLength(lp);
        unsigned char *p = lpSeek(lp, r);
        unsigned int l;
        *str = (char *)lpGetValue(p, &l, (long long *)llele);
        *len = (size_t)l;
    } else {
        serverPanic("Unknown set encoding");
    }
    return setobj->encoding;
}

/* Pops a random element and returns it as an object, or NULL when every
 * member is expired. */
robj *setTypePopRandom(robj *set) {
    robj *obj;
    if (set->encoding == OBJ_ENCODING_LISTPACK && !setTypeHasVolatileMembers(set)) {
        /* Find random and delete it without re-seeking the listpack. */
        unsigned int i = 0;
        unsigned char *p = lpNextRandom(objectGetVal(set), lpFirst(objectGetVal(set)), &i, 1, 0);
        unsigned int len = 0; /* initialize to silence warning */
        long long llele = 0;  /* initialize to silence warning */
        char *str = (char *)lpGetValue(p, &len, &llele);
        if (str)
            obj = createStringObject(str, len);
        else
            obj = createStringObjectFromLongLong(llele);
        objectSetVal(set, lpDelete(objectGetVal(set), p, NULL));
    } else {
        char *str;
        size_t len = 0;
        int64_t llele = 0;
        int encoding = setTypeRandomElement(set, &str, &len, &llele);
        if (encoding == -1) return NULL;
        if (str)
            obj = createStringObject(str, len);
        else
            obj = createStringObjectFromLongLong(llele);
        setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HASHTABLE);
    }
    return obj;
}

unsigned long setTypeSize(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_HASHTABLE) {
        return hashtableSize((const hashtable *)objectGetVal(subject));
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        return intsetLen((const intset *)objectGetVal(subject));
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        return lpLength((unsigned char *)objectGetVal(subject));
    } else {
        serverPanic("Unknown set encoding");
    }
}

/* Convert the set to specified encoding. The resulting hashtable (when converting
 * to a hash table) is presized to hold the number of elements in the original
 * set. */
void setTypeConvert(robj *setobj, int enc) {
    setTypeConvertAndExpand(setobj, enc, setTypeSize(setobj), 1);
}

/* Converts a set to the specified encoding, pre-sizing it for 'cap' elements.
 * The 'panic' argument controls whether to panic on OOM (panic=1) or return
 * C_ERR on OOM (panic=0). If panic=1 is given, this function always returns
 * C_OK. */
int setTypeConvertAndExpand(robj *setobj, int enc, unsigned long cap, int panic) {
    setTypeIterator *si;
    serverAssertWithInfo(NULL, setobj, setobj->type == OBJ_SET && setobj->encoding != enc);

    if (enc == OBJ_ENCODING_HASHTABLE) {
        /* A half-built listpack does not have the aggregate header yet. */
        bool has_volatile = false;
        hashtable *ht = hashtableCreate(&setHashtableType);

        /* Presize the hashtable to avoid rehashing */
        if (panic) {
            hashtableExpand(ht, cap);
        } else if (!hashtableTryExpand(ht, cap)) {
            hashtableRelease(ht);
            return C_ERR;
        }

        /* Expired members are carried too: a conversion changes only the encoding. */
        setTypeIgnoreTTL(setobj, true);
        char *str;
        size_t len;
        int64_t llele;
        char tmpbuf[LONG_STR_SIZE];
        si = setTypeInitIterator(setobj);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            mstime_t expiry = setTypeCurrentExpiry(si, str);
            if (expiry != EXPIRY_NONE) has_volatile = true;
            if (str == NULL) {
                len = ll2string(tmpbuf, sizeof(tmpbuf), llele);
                str = tmpbuf;
            }
            serverAssert(hashtableAdd(ht, smemberCreate(str, len, expiry)));
        }
        setTypeReleaseIterator(si);
        setTypeIgnoreTTL(setobj, false);

        freeSetObject(setobj); /* frees the internals but not setobj itself */
        setobj->encoding = OBJ_ENCODING_HASHTABLE;
        objectSetVal(setobj, ht);
        if (has_volatile) setTypeTrackVolatileMembers(setobj);
    } else if (enc == OBJ_ENCODING_LISTPACK) {
        /* Preallocate the minimum two bytes per element (enc/value + backlen) */
        size_t estcap = cap * 2;
        if (setobj->encoding == OBJ_ENCODING_INTSET && setTypeSize(setobj) > 0) {
            /* If we're converting from intset, we have a better estimate. */
            size_t s1 = lpEstimateBytesRepeatedInteger(intsetMin(objectGetVal(setobj)), cap);
            size_t s2 = lpEstimateBytesRepeatedInteger(intsetMax(objectGetVal(setobj)), cap);
            estcap = max(s1, s2);
        }
        unsigned char *lp = lpNew(estcap);
        char *str;
        size_t len;
        int64_t llele;
        si = setTypeInitIterator(setobj);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            if (str != NULL)
                lp = lpAppend(lp, (unsigned char *)str, len);
            else
                lp = lpAppendInteger(lp, llele);
        }
        setTypeReleaseIterator(si);

        freeSetObject(setobj); /* frees the internals but not setobj itself */
        setobj->encoding = OBJ_ENCODING_LISTPACK;
        objectSetVal(setobj, lp);
    } else {
        serverPanic("Unsupported set conversion");
    }
    return C_OK;
}

/* This is a helper function for the COPY command.
 * Duplicate a set object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *setTypeDup(robj *o) {
    robj *set;
    setTypeIterator *si;

    serverAssert(objectGetType(o) == OBJ_SET);

    /* Create a new set object that have the same encoding as the original object's encoding */
    if (objectGetEncoding(o) == OBJ_ENCODING_INTSET) {
        intset *is = objectGetVal(o);
        size_t size = intsetBlobLen(is);
        intset *newis = zmalloc(size);
        memcpy(newis, is, size);
        set = createObject(OBJ_SET, newis);
        set->encoding = OBJ_ENCODING_INTSET;
    } else if (objectGetEncoding(o) == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = objectGetVal(o);
        size_t sz = lpBytes(lp);
        unsigned char *new_lp = zmalloc(sz);
        memcpy(new_lp, lp, sz);
        set = createObject(OBJ_SET, new_lp);
        set->encoding = OBJ_ENCODING_LISTPACK;
    } else if (objectGetEncoding(o) == OBJ_ENCODING_HASHTABLE) {
        set = createSetObject();
        hashtable *ht = objectGetVal(o);
        hashtable *dst = objectGetVal(set);
        hashtableExpand(dst, hashtableSize(ht));
        setTypeIgnoreTTL(o, true);
        si = setTypeInitIterator(o);
        char *str;
        size_t len;
        int64_t intobj;
        while (setTypeNext(si, &str, &len, &intobj) != -1) {
            mstime_t expiry = smemberGetExpiry(str);
            smember *m = smemberCreate(str, len, expiry);
            serverAssert(hashtableAdd(dst, m));
            if (expiry != EXPIRY_NONE) setTypeTrackMember(set, m);
        }
        setTypeReleaseIterator(si);
        setTypeIgnoreTTL(o, false);
    } else {
        serverPanic("Unknown set encoding");
    }
    return set;
}

void saddCommand(client *c) {
    robj *set;
    int j, added = 0;

    set = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, set, OBJ_SET)) return;

    if (set == NULL) {
        set = setTypeCreate(objectGetVal(c->argv[2]), c->argc - 2);
        dbAdd(c->db, c->argv[1], &set);
    } else {
        setTypeMaybeConvert(set, c->argc - 2);
    }

    if (setTypeHasVolatileMembers(set)) {
        size_t original_size = setTypeSize(set);
        mstime_t key_expire = objectGetExpire(set);
        size_t num_expired = 0;
        robj **expired_members = NULL;
        for (j = 2; j < c->argc; j++) {
            bool replaced_expired = false;
            if (setTypeAddWithExpiry(set, objectGetVal(c->argv[j]), EXPIRY_NONE, SET_ADD_KEEP_EXPIRY, &replaced_expired, NULL))
                added++;
            if (replaced_expired) {
                if (expired_members == NULL) expired_members = zmalloc(sizeof(robj *) * (c->argc - 2));
                expired_members[num_expired++] = c->argv[j];
                incrRefCount(c->argv[j]);
            }
        }
        if (num_expired > 0) {
            server.stat_expiredsetmembers += num_expired;
            notifyKeyspaceEvent(NOTIFY_SET, "sexpired", c->argv[1], c->db->id);
            size_t idx = 0;
            while (idx < num_expired) {
                idx += propagateItemsDeletion(c->db, set, num_expired - idx, &expired_members[idx], c->slot);
            }
            if (num_expired == original_size && key_expire != EXPIRY_NONE)
                propagateCommandAndKeyExpiration(c, c->argv[1], key_expire);
        }
        zfree(expired_members);
        if (!setTypeHasVolatileMembers(set)) dbUpdateObjectWithVolatileItemsTracking(c->db, set);
    } else {
        for (j = 2; j < c->argc; j++) {
            if (setTypeAdd(set, objectGetVal(c->argv[j]))) added++;
        }
    }
    if (added) {
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_SET, "sadd", c->argv[1], c->db->id);
        server.dirty += added;
    }
    addReplyLongLong(c, added);
}

void sremCommand(client *c) {
    robj *set;
    int j, deleted = 0, keyremoved = 0;

    if ((set = lookupKeyWriteOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, set, OBJ_SET)) return;

    bool was_volatile = setTypeHasVolatileMembers(set);
    robj **new_argv = NULL;
    int new_argc = 0, skipped = 0;
    if (set->encoding == OBJ_ENCODING_HASHTABLE) hashtablePauseAutoShrink(objectGetVal(set));
    for (j = 2; j < c->argc; j++) {
        if (setTypeRemove(set, objectGetVal(c->argv[j]))) {
            if (skipped) {
                if (!new_argv) new_argv = createChangedItemsArgv(c, c->argc, 2, 2, skipped, &new_argc);
                new_argv[new_argc++] = c->argv[j];
                incrRefCount(c->argv[j]);
            }
            deleted++;
            if (setTypeSize(set) == 0) {
                if (was_volatile) dbUntrackKeyWithVolatileItems(c->db, set);
                dbDelete(c->db, c->argv[1]);
                keyremoved = 1;
                break;
            }
        } else if (was_volatile && !skipped) {
            skipped = j;
        }
    }
    if (!keyremoved && set->encoding == OBJ_ENCODING_HASHTABLE) hashtableResumeAutoShrink(objectGetVal(set));

    if (deleted) {
        if (!keyremoved && was_volatile != setTypeHasVolatileMembers(set))
            dbUpdateObjectWithVolatileItemsTracking(c->db, set);
        if (skipped) {
            if (!new_argv) new_argv = createChangedItemsArgv(c, c->argc, 2, 2, skipped, &new_argc);
            new_argv[0] = shared.srem;
            new_argv[1] = c->argv[1];
            incrRefCount(c->argv[1]);
            replaceClientCommandVector(c, new_argc, new_argv);
        }
        signalModifiedKey(c, c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_SET, "srem", c->argv[1], c->db->id);
        if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c, deleted);
}

void smoveCommand(client *c) {
    robj *srcset, *dstset, *ele;
    srcset = lookupKeyWrite(c->db, c->argv[1]);
    dstset = lookupKeyWrite(c->db, c->argv[2]);
    ele = c->argv[3];

    /* If the source key does not exist return 0 */
    if (srcset == NULL) {
        addReply(c, shared.czero);
        return;
    }

    /* If the source key has the wrong type, or the destination key
     * is set and has the wrong type, return with an error. */
    if (checkType(c, srcset, OBJ_SET) || checkType(c, dstset, OBJ_SET)) return;

    /* If srcset and dstset are equal, SMOVE is a no-op */
    if (srcset == dstset) {
        addReply(c, setTypeIsMember(srcset, objectGetVal(ele)) ? shared.cone : shared.czero);
        return;
    }

    /* The member's TTL moves with it. */
    mstime_t expiry = EXPIRY_NONE;
    bool src_volatile = setTypeHasVolatileMembers(srcset);
    bool dst_volatile = setTypeHasVolatileMembers(dstset);
    if (src_volatile) setTypeGetExpiry(srcset, objectGetVal(ele), &expiry);
    unsigned long dst_original_size = dst_volatile ? setTypeSize(dstset) : 0;
    mstime_t dst_key_expire = dst_volatile ? objectGetExpire(dstset) : EXPIRY_NONE;

    /* If the element cannot be removed from the src set, return 0. */
    if (!setTypeRemove(srcset, objectGetVal(ele))) {
        addReply(c, shared.czero);
        return;
    }
    notifyKeyspaceEvent(NOTIFY_SET, "srem", c->argv[1], c->db->id);

    if (src_volatile && !setTypeHasVolatileMembers(srcset)) dbUpdateObjectWithVolatileItemsTracking(c->db, srcset);

    /* Remove the src set from the database when empty */
    if (setTypeSize(srcset) == 0) {
        dbDelete(c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    }

    /* Create the destination set when it doesn't exist */
    if (!dstset) {
        dstset = setTypeCreate(objectGetVal(ele), 1);
        dbAdd(c->db, c->argv[2], &dstset);
    }

    signalModifiedKey(c, c->db, c->argv[1]);
    server.dirty++;

    /* An extra key has changed when ele was successfully added to dstset */
    bool replaced_expired = false;
    int added;
    if (expiry == EXPIRY_NONE && !dst_volatile) {
        added = setTypeAdd(dstset, objectGetVal(ele));
    } else {
        added = setTypeAddWithExpiry(dstset, objectGetVal(ele), expiry, SET_ADD_KEEP_EXPIRY, &replaced_expired, NULL);
        if (replaced_expired) {
            server.stat_expiredsetmembers++;
            notifyKeyspaceEvent(NOTIFY_SET, "sexpired", c->argv[2], c->db->id);
            incrRefCount(ele);
            propagateItemsDeletion(c->db, dstset, 1, &ele, c->slot);
        }
        if (dst_volatile != setTypeHasVolatileMembers(dstset)) dbUpdateObjectWithVolatileItemsTracking(c->db, dstset);
    }
    if (added) {
        server.dirty++;
        signalModifiedKey(c, c->db, c->argv[2]);
        notifyKeyspaceEvent(NOTIFY_SET, "sadd", c->argv[2], c->db->id);
    }
    if (replaced_expired && dst_original_size == 1 && dst_key_expire != EXPIRY_NONE)
        propagateCommandAndKeyExpiration(c, c->argv[2], dst_key_expire);
    addReply(c, shared.cone);
}

#define SMISMEMBER_FIND_BATCH_SIZE 16
static_assert(SMISMEMBER_FIND_BATCH_SIZE <= HASHTABLE_FIND_BATCH_MAX_SIZE,
              "SMISMEMBER batch size exceeds hashtable batch lookup limit");

static void sismemberReply(client *c, robj *set, robj *member) {
    addReply(c, setTypeIsMember(set, objectGetVal(member)) ? shared.cone : shared.czero);
}

static void smismemberReplyWithHashtable(client *c, hashtable *ht, robj **members, size_t count) {
    const void *keys[SMISMEMBER_FIND_BATCH_SIZE];
    void *found_entries[SMISMEMBER_FIND_BATCH_SIZE];
    while (count) {
        size_t batch = count > SMISMEMBER_FIND_BATCH_SIZE ? SMISMEMBER_FIND_BATCH_SIZE : count;

        for (size_t i = 0; i < batch; i++) {
            keys[i] = objectGetVal(members[i]);
        }

        uint32_t result = hashtableFindBatch(ht, (int)batch, keys, found_entries);

        for (size_t i = 0; i < batch; i++) {
            addReply(c, (result >> i) & 1 ? shared.cone : shared.czero);
        }

        members += batch;
        count -= batch;
    }
}

void sismemberCommand(client *c) {
    robj *set;
    int xx = 0;

    if (c->argc == 4 && !strcasecmp(objectGetVal(c->argv[3]), "XX")) {
        xx = 1;
    } else if (c->argc > 3) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    set = lookupKeyRead(c->db, c->argv[1]);
    if (set == NULL) {
        if (xx)
            /* If key doesn't exist and XX is specified, return -1 */
            addReplyLongLong(c, -1);
        else
            /* If key doesn't exist and XX is not specified, return 0 */
            addReply(c, shared.czero);
        return;
    }

    if (checkType(c, set, OBJ_SET)) return;

    sismemberReply(c, set, c->argv[2]);
}

void smismemberCommand(client *c) {
    robj *set;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * sets, where SMISMEMBER should respond with a series of zeros. */
    set = lookupKeyRead(c->db, c->argv[1]);
    if (set == NULL) {
        addReplyArrayLen(c, c->argc - 2);
        for (int j = 2; j < c->argc; j++) {
            addReply(c, shared.czero);
        }
        return;
    }
    if (checkType(c, set, OBJ_SET)) return;

    size_t count = c->argc - 2;
    addReplyArrayLen(c, count);

    /* Prefer hashtable batch lookup to improve performance. */
    if (set->encoding == OBJ_ENCODING_HASHTABLE && count > 1) {
        smismemberReplyWithHashtable(c, objectGetVal(set), c->argv + 2, count);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        sismemberReply(c, set, c->argv[i + 2]);
    }
}

void scardCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, o, OBJ_SET)) return;

    addReplyLongLong(c, setTypeSize(o));
}

/* Handle the "SPOP key <count>" variant. The normal version of the
 * command is handled by the spopCommand() function itself. */

/* How many times bigger should be the set compared to the remaining size
 * for us to use the "create new set" strategy? Read later in the
 * implementation for more info. */
#define SPOP_MOVE_STRATEGY_MUL 5

/* Pop a live member of a hashtable set: remove it, reply with it and replicate
 * it as part of an SREM batch. */
static void spopHashtableMember(client *c,
                                robj *set,
                                smember *m,
                                robj **propargv,
                                unsigned long *propindex,
                                unsigned long batchsize) {
    robj *member = createStringObject(m, sdslen(m));
    serverAssert(setTypeRemove(set, objectGetVal(member)));
    addReplyBulk(c, member);
    propargv[(*propindex)++] = member;
    if (*propindex == 2 + batchsize) {
        alsoPropagate(c->db->id, propargv, *propindex, PROPAGATE_AOF | PROPAGATE_REPL, c->slot);
        for (unsigned long j = 2; j < *propindex; j++) decrRefCount(propargv[j]);
        *propindex = 2;
    }
}

/* Step 3 for SPOP: pops 'count' of the 'live' members of a hashtable set by
 * selection sampling (a live member is taken with probability still-to-pop /
 * still-unseen) in one walk that removes each as it is returned, so no working
 * memory depends on the count. The hidden members stay. */
static unsigned long spopLiveSample(client *c,
                                    robj *set,
                                    unsigned long live,
                                    unsigned long count,
                                    robj **propargv,
                                    unsigned long *propindex,
                                    unsigned long batchsize) {
    if (count > live) count = live;
    unsigned long popped = 0, seen = 0;
    hashtableIterator iter;
    hashtableInitIterator(&iter, objectGetVal(set), HASHTABLE_ITER_SAFE);
    void *next;
    while (popped < count && hashtableNext(&iter, &next)) {
        if ((unsigned long)rand() % (live - seen++) >= count - popped) continue;
        spopHashtableMember(c, set, next, propargv, propindex, batchsize);
        popped++;
    }
    hashtableCleanupIterator(&iter);
    serverAssert(popped == count);
    return popped;
}

/* SPOP with a count over a hashtable set that may hide a member. Returns the
 * number popped: min(count, live members). */
static unsigned long spopHashtableLive(client *c,
                                       robj *set,
                                       setLiveSampler *s,
                                       unsigned long count,
                                       robj **propargv,
                                       unsigned long *propindex,
                                       unsigned long batchsize) {
    unsigned long size = setTypeSize(set), popped = 0;
    smember *indexed[SET_LIVE_INDEX_MAX];
    long held = setTypeCollectLiveIndexed(set, indexed, setLiveIndexCap(set));
    if (held >= 0) {
        /* A partial Fisher-Yates over the live members the index named. */
        unsigned long n = (unsigned long)held;
        unsigned long want = count < n ? count : n;
        for (unsigned long i = 0; i < want; i++) {
            unsigned long j = i + (unsigned long)rand() % (n - i);
            smember *m = indexed[j];
            indexed[j] = indexed[i];
            spopHashtableMember(c, set, m, propargv, propindex, batchsize);
        }
        return want;
    }
    /* CASE 2 territory: the pop is small next to the set, so draws are cheap. */
    if (count < size && (size - count) * SPOP_MOVE_STRATEGY_MUL > count) {
        smember *m;
        while (popped < count && (m = setLiveSamplerDraw(s)) != NULL) {
            spopHashtableMember(c, set, m, propargv, propindex, batchsize);
            popped++;
        }
    }
    if (popped < count) popped += spopLiveSample(c, set, setTypeLiveCount(set), count - popped, propargv, propindex, batchsize);
    return popped;
}

/* setTypeSize() counts hidden members, so a set that may hide one pops at most
 * its live members and keeps the hidden ones. */
void spopWithCountCommand(client *c) {
    long l;
    unsigned long count, size;
    robj *set;

    /* Get the count argument */
    if (getPositiveLongFromObjectOrReply(c, c->argv[2], &l, NULL) != C_OK) return;
    count = (unsigned long)l;

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set. Otherwise, return nil */
    if ((set = lookupKeyWriteOrReply(c, c->argv[1], shared.emptyset[c->resp])) == NULL || checkType(c, set, OBJ_SET))
        return;
    size = setTypeSize(set);

    /* If count is zero, serve an empty set ASAP to avoid special
     * cases later. */
    if (count == 0) {
        addReply(c, shared.emptyset[c->resp]);
        return;
    }

    bool volatile_set = setTypeHasVolatileMembers(set);
    setLiveSampler sampler;
    setLiveSamplerInit(&sampler, set, count);

    /* CASE 1:
     * The number of requested elements is greater than or equal to
     * the number of elements inside the set: simply return the whole set.
     * A hidden member keeps the set, so its live members are popped below
     * instead; looking for one costs no more than this reply. */
    if (count >= size && (!sampler.hides || !setTypeHasExpiredMembers(set))) {
        /* Generate an SPOP keyspace notification */
        notifyKeyspaceEvent(NOTIFY_SET, "spop", c->argv[1], c->db->id);
        server.dirty += size;

        /* We just return the entire set */
        sunionDiffGenericCommand(c, c->argv + 1, 1, NULL, SET_OP_UNION);

        /* Delete the set as it is now empty */
        dbDelete(c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);

        /* todo: Move the spop notification to be executed after the command logic. */

        /* Propagate this command as a DEL or UNLINK operation */
        robj *aux = server.lazyfree_lazy_server_del ? shared.unlink : shared.del;
        rewriteClientCommandVector(c, 2, aux, c->argv[1]);
        signalModifiedKey(c, c->db, c->argv[1]);
        return;
    }

    /* Case 2 and 3 require to replicate SPOP as a set of SREM commands.
     * Prepare our replication argument vector. Also send the array length
     * which is common to both the code paths. */
    unsigned long batchsize = count > 1024 ? 1024 : count;
    robj **propargv = zmalloc(sizeof(robj *) * (2 + batchsize));
    propargv[0] = shared.srem;
    propargv[1] = c->argv[1];
    unsigned long propindex = 2;
    void *replylen = NULL;
    /* A set that hides members may pop none, so it notifies after the pops. */
    if (sampler.hides) {
        replylen = addReplyDeferredLen(c);
    } else {
        notifyKeyspaceEvent(NOTIFY_SET, "spop", c->argv[1], c->db->id);
        addReplySetLen(c, count);
    }
    unsigned long popped = 0;
    bool set_replaced = false;

    /* Common iteration vars. */
    char *str;
    size_t len;
    int64_t llele;
    unsigned long remaining = count < size ? size - count : 0; /* Elements left after SPOP. */

    /* If we are here, the number of requested elements is less than the
     * number of elements inside the set, or the set may hide a member.
     * Use two different strategies.
     *
     * CASE 2: The number of elements to return is small compared to the
     * set size. We can just extract random elements and return them to
     * the set. */
    if (remaining * SPOP_MOVE_STRATEGY_MUL > count && set->encoding == OBJ_ENCODING_LISTPACK && !sampler.hides) {
        /* Specialized case for listpack. Traverse it only once. */
        unsigned char *lp = objectGetVal(set);
        unsigned char *p = lpFirst(lp);
        unsigned int index = 0;
        unsigned long volatile_deleted = 0;
        unsigned char **ps = zmalloc(sizeof(char *) * count);
        for (unsigned long i = 0; i < count; i++) {
            p = lpNextRandom(lp, p, &index, count - i, 0);
            unsigned int len;
            str = (char *)lpGetValue(p, &len, (long long *)&llele);
            if (setTypeListpackGetExpiry(lp, p) != EXPIRY_NONE) volatile_deleted++;

            if (str) {
                addReplyBulkCBuffer(c, str, len);
                propargv[propindex++] = createStringObject(str, len);
            } else {
                addReplyBulkLongLong(c, llele);
                propargv[propindex++] = createStringObjectFromLongLong(llele);
            }
            /* Replicate/AOF this command as an SREM operation */
            if (propindex == 2 + batchsize) {
                alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL, c->slot);
                for (unsigned long j = 2; j < propindex; j++) {
                    decrRefCount(propargv[j]);
                }
                propindex = 2;
            }

            /* Store pointer for later deletion and move to next. */
            ps[i] = p;
            p = lpNext(lp, p);
            index++;
        }
        lp = lpBatchDelete(lp, ps, count);
        zfree(ps);
        objectSetVal(set, lp);
        if (volatile_deleted) listpackObjectUpdateVolatileCount(set, -(long)volatile_deleted);
        popped = count;
    } else if (sampler.hides && set->encoding == OBJ_ENCODING_LISTPACK) {
        /* One traversal collects the live entries; selection sampling then keeps
         * them in listpack order, which lpBatchDelete requires. */
        unsigned char *lp = objectGetVal(set);
        unsigned char **live = zmalloc(sizeof(*live) * lpLength(lp));
        unsigned long live_count = 0;
        for (unsigned char *p = lpFirst(lp); p != NULL; p = lpNext(lp, p)) {
            if (setTypeListpackIsValidAt(lp, p)) live[live_count++] = p;
        }
        unsigned long to_pop = count < live_count ? count : live_count;
        unsigned long volatile_deleted = 0;
        for (unsigned long i = 0; i < live_count && popped < to_pop; i++) {
            if ((unsigned long)rand() % (live_count - i) >= to_pop - popped) continue;
            unsigned char *p = live[i];
            unsigned int entrylen;
            str = (char *)lpGetValue(p, &entrylen, (long long *)&llele);
            if (setTypeListpackGetExpiry(lp, p) != EXPIRY_NONE) volatile_deleted++;
            if (str) {
                addReplyBulkCBuffer(c, str, entrylen);
                propargv[propindex++] = createStringObject(str, entrylen);
            } else {
                addReplyBulkLongLong(c, llele);
                propargv[propindex++] = createStringObjectFromLongLong(llele);
            }
            live[popped++] = p;
            if (propindex == 2 + batchsize) {
                alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL, c->slot);
                for (unsigned long j = 2; j < propindex; j++) decrRefCount(propargv[j]);
                propindex = 2;
            }
        }
        lp = lpBatchDelete(lp, live, popped);
        zfree(live);
        objectSetVal(set, lp);
        if (volatile_deleted) listpackObjectUpdateVolatileCount(set, -(long)volatile_deleted);
    } else if (sampler.hides) {
        popped = spopHashtableLive(c, set, &sampler, count, propargv, &propindex, batchsize);
    } else if (remaining * SPOP_MOVE_STRATEGY_MUL > count) {
        for (unsigned long i = 0; i < count; i++) {
            propargv[propindex] = setTypePopRandom(set);
            addReplyBulk(c, propargv[propindex]);
            propindex++;
            /* Replicate/AOF this command as an SREM operation */
            if (propindex == 2 + batchsize) {
                alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL, c->slot);
                for (unsigned long j = 2; j < propindex; j++) {
                    decrRefCount(propargv[j]);
                }
                propindex = 2;
            }
        }
        popped = count;
    } else {
        /* CASE 3: The number of elements to return is very big, approaching
         * the size of the set itself. After some time extracting random elements
         * from such a set becomes computationally expensive, so we use
         * a different strategy, we extract random elements that we don't
         * want to return (the elements that will remain part of the set),
         * creating a new set as we do this (that will be stored as the original
         * set). Then we return the elements left in the original set and
         * release it. */
        robj *newset = NULL;

        /* Create a new set with just the remaining elements. */
        if (set->encoding == OBJ_ENCODING_LISTPACK) {
            /* Specialized case for listpack. Traverse it only once. */
            newset = createSetListpackObject();
            unsigned char *lp = objectGetVal(set);
            unsigned char *p = lpFirst(lp);
            unsigned int index = 0;
            unsigned char **ps = zmalloc(sizeof(char *) * remaining);
            for (unsigned long i = 0; i < remaining; i++) {
                p = lpNextRandom(lp, p, &index, remaining - i, 0);
                unsigned int len;
                str = (char *)lpGetValue(p, &len, (long long *)&llele);
                setTypeAddMovedMember(newset, str, len, llele, 0, setTypeListpackGetExpiry(lp, p));
                ps[i] = p;
                p = lpNext(lp, p);
                index++;
            }
            lp = lpBatchDelete(lp, ps, remaining);
            zfree(ps);
            objectSetVal(set, lp);
        } else {
            while (remaining--) {
                int encoding = setTypeRandomElement(set, &str, &len, &llele);
                if (!newset) newset = str ? createSetListpackObject() : createIntsetObject();
                mstime_t expiry = encoding == OBJ_ENCODING_HASHTABLE ? smemberGetExpiry((smember *)str) : EXPIRY_NONE;
                setTypeAddMovedMember(newset, str, len, llele, encoding == OBJ_ENCODING_HASHTABLE, expiry);
                setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HASHTABLE);
            }
        }
        /* Transfer the old set to the client. */
        setTypeIterator *si;
        si = setTypeInitIterator(set);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            if (str == NULL) {
                addReplyBulkLongLong(c, llele);
                propargv[propindex++] = createStringObjectFromLongLong(llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
                propargv[propindex++] = createStringObject(str, len);
            }
            popped++;
            /* Replicate/AOF this command as an SREM operation */
            if (propindex == 2 + batchsize) {
                alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL, c->slot);
                for (unsigned long i = 2; i < propindex; i++) {
                    decrRefCount(propargv[i]);
                }
                propindex = 2;
            }
        }
        setTypeReleaseIterator(si);

        /* Assign the new set as the key value. */
        if (volatile_set) dbUntrackKeyWithVolatileItems(c->db, set);
        dbReplaceValue(c->db, c->argv[1], &newset);
        set_replaced = true;
    }
    bool set_emptied = !set_replaced && setTypeSize(set) == 0;
    if (!set_replaced && volatile_set && !setTypeHasVolatileMembers(set))
        dbUpdateObjectWithVolatileItemsTracking(c->db, set);
    if (set_emptied) dbDelete(c->db, c->argv[1]);
    if (replylen) setDeferredSetLen(c, replylen, popped);
    server.dirty += popped;

    /* Replicate/AOF the remaining elements as an SREM operation */
    if (propindex != 2) {
        alsoPropagate(c->db->id, propargv, propindex, PROPAGATE_AOF | PROPAGATE_REPL, c->slot);
        for (unsigned long i = 2; i < propindex; i++) {
            decrRefCount(propargv[i]);
        }
        propindex = 2;
    }
    zfree(propargv);

    /* Don't propagate the command itself even if we incremented the
     * dirty counter. We don't want to propagate an SPOP command since
     * we propagated the command as a set of SREMs operations using
     * the alsoPropagate() API. */
    preventCommandPropagation(c);
    if (popped) {
        if (sampler.hides) notifyKeyspaceEvent(NOTIFY_SET, "spop", c->argv[1], c->db->id);
        if (set_emptied) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        signalModifiedKey(c, c->db, c->argv[1]);
    }
}

void spopCommand(client *c) {
    robj *set, *ele;

    if (c->argc == 3) {
        spopWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set */
    if ((set = lookupKeyWriteOrReply(c, c->argv[1], shared.null[c->resp])) == NULL || checkType(c, set, OBJ_SET))
        return;

    bool was_volatile = setTypeHasVolatileMembers(set);
    ele = setTypePopRandom(set);
    if (ele == NULL) {
        addReply(c, shared.null[c->resp]);
        return;
    }
    if (was_volatile && !setTypeHasVolatileMembers(set)) dbUpdateObjectWithVolatileItemsTracking(c->db, set);

    notifyKeyspaceEvent(NOTIFY_SET, "spop", c->argv[1], c->db->id);

    /* Replicate/AOF this command as an SREM operation */
    rewriteClientCommandVector(c, 3, shared.srem, c->argv[1], ele);

    /* Add the element to the reply */
    addReplyBulk(c, ele);
    decrRefCount(ele);

    /* Delete the set if it's empty */
    if (setTypeSize(set) == 0) {
        dbDelete(c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    }

    /* Set has been modified */
    signalModifiedKey(c, c->db, c->argv[1]);
    server.dirty++;
}

/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info. */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3

/* If client is trying to ask for a very large number of random elements,
 * queuing may consume an unlimited amount of memory, so we want to limit
 * the number of randoms per time. */
#define SRANDFIELD_RANDOM_SAMPLE_LIMIT 1000

/* SRANDMEMBER with a negative count, or a count of 1, over a set that may hide
 * a member. Command time is frozen, so the live members cannot change between
 * picks: the reply is empty only when no member is live. */
static void srandmemberDrawLive(client *c, setLiveSampler *s, unsigned long count) {
    robj *set = s->set;

    /* One draw is a scalar pick, which reads one listpack position unless it
     * lands on a hidden member. */
    if (count == 1) {
        char *str;
        size_t len;
        int64_t llele;
        if (setLiveSamplerPick(s, &str, &len, &llele) == -1) {
            addReply(c, shared.emptyarray);
            return;
        }
        addReplyArrayLen(c, 1);
        if (str)
            addReplyBulkCBuffer(c, str, len);
        else
            addReplyBulkLongLong(c, llele);
        return;
    }

    listpackEntry *live = NULL; /* Once collected, what the remaining picks draw from. */
    unsigned long live_count = 0;
    if (set->encoding == OBJ_ENCODING_HASHTABLE) {
        smember *picks[SET_LIVE_INDEX_MAX];
        long held = setTypeCollectLiveIndexed(set, picks, setLiveIndexCap(set));
        if (held == 0) {
            addReply(c, shared.emptyarray);
            return;
        }
        if (held > 0) {
            addReplyArrayLen(c, count);
            while (count--) {
                smember *m = picks[rand() % held];
                addReplyBulkCBuffer(c, m, sdslen(m));
                if (c->flag.close_asap) break;
            }
            return;
        }

        unsigned long emitted = 0;
        smember *m;
        while (emitted < count && (m = setLiveSamplerDraw(s)) != NULL) {
            if (emitted++ == 0) addReplyArrayLen(c, count);
            addReplyBulkCBuffer(c, m, sdslen(m));
            if (c->flag.close_asap) return;
        }
        if (emitted == count) return;

        /* The budget is spent: the remaining picks come from one pass. */
        unsigned long total = setTypeLiveCount(set);
        if (total == 0) {
            serverAssert(emitted == 0);
            addReply(c, shared.emptyarray);
            return;
        }
        if (emitted == 0) addReplyArrayLen(c, count);
        count -= emitted;
        if (count <= SET_RANK_WALK_MAX) {
            setTypePickLiveByRank(set, total, (unsigned int)count, picks);
            for (unsigned long i = 0; i < count; i++) addReplyBulkCBuffer(c, picks[i], sdslen(picks[i]));
            return;
        }
        live = setTypeCollectLive(set, &live_count);
        serverAssert(live_count == total);
    } else {
        /* A listpack draw is a pass in any case: one pass collects every pick's source. */
        live = setTypeCollectLive(set, &live_count);
        if (live_count == 0) {
            zfree(live);
            addReply(c, shared.emptyarray);
            return;
        }
        addReplyArrayLen(c, count);
    }

    while (count--) {
        listpackEntry pick = live[rand() % live_count];
        if (pick.sval)
            addReplyBulkCBuffer(c, pick.sval, pick.slen);
        else
            addReplyBulkLongLong(c, pick.lval);
        if (c->flag.close_asap) break;
    }
    zfree(live);
}

/* Step 3 for SRANDMEMBER with a positive count: 'count' of the 'live' members
 * of a hashtable set by selection sampling, in one walk and without working
 * memory. */
static void srandmemberReplyLiveSample(client *c, robj *set, unsigned long live, unsigned long count) {
    if (count > live) count = live;
    addReplyArrayLen(c, count);
    unsigned long seen = 0;
    hashtableIterator iter;
    hashtableInitIterator(&iter, objectGetVal(set), 0);
    void *next;
    while (count && hashtableNext(&iter, &next)) {
        if ((unsigned long)rand() % (live - seen++) >= count) continue;
        addReplyBulkCBuffer(c, next, sdslen(next));
        count--;
    }
    hashtableCleanupIterator(&iter);
    serverAssert(count == 0);
}

/* Free a CASE 3/4 auxiliary table that is not sent: it owns its sds entries. */
static void srandmemberReleaseAux(hashtable *ht) {
    hashtableIterator iter;
    hashtableInitIterator(&iter, ht, 0);
    void *element;
    while (hashtableNext(&iter, &element)) sdsfree((sds)element);
    hashtableCleanupIterator(&iter);
    hashtableRelease(ht);
}

/* handle the "SRANDMEMBER key <count>" variant. The normal version of the
 * command is handled by the srandmemberCommand() function itself. */
void srandmemberWithCountCommand(client *c) {
    long l;
    unsigned long count, size;
    int uniq = 1;
    robj *set;
    char *str;
    size_t len;
    int64_t llele;

    if (getRangeLongFromObjectOrReply(c, c->argv[2], -LONG_MAX, LONG_MAX, &l, NULL) != C_OK) return;
    if (l >= 0) {
        count = (unsigned long)l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        count = -l;
        uniq = 0;
    }

    if ((set = lookupKeyReadOrReply(c, c->argv[1], shared.emptyarray)) == NULL || checkType(c, set, OBJ_SET)) return;

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) {
        addReply(c, shared.emptyarray);
        return;
    }

    size = setTypeSize(set);
    setLiveSampler sampler;
    setLiveSamplerInit(&sampler, set, count);

    /* CASE 1: The count was negative, so the extraction method is just:
     * "return N random elements" sampling the whole set every time.
     * This case is trivial and can be served without auxiliary data
     * structures. This case is the only one that also needs to return the
     * elements in random order. */
    if (!uniq || count == 1) {
        if (sampler.hides) {
            srandmemberDrawLive(c, &sampler, count);
            return;
        }
        addReplyArrayLen(c, count);

        if (set->encoding == OBJ_ENCODING_LISTPACK && count > 1) {
            /* Specialized case for listpack, traversing it only once. */
            unsigned long limit, sample_count;
            limit = count > SRANDFIELD_RANDOM_SAMPLE_LIMIT ? SRANDFIELD_RANDOM_SAMPLE_LIMIT : count;
            listpackEntry *entries = zmalloc(limit * sizeof(listpackEntry));
            while (count) {
                sample_count = count > limit ? limit : count;
                count -= sample_count;
                lpRandomEntries(objectGetVal(set), sample_count, entries);
                for (unsigned long i = 0; i < sample_count; i++) {
                    if (entries[i].sval)
                        addReplyBulkCBuffer(c, entries[i].sval, entries[i].slen);
                    else
                        addReplyBulkLongLong(c, entries[i].lval);
                }
                if (c->flag.close_asap) break;
            }
            zfree(entries);
            return;
        }

        while (count--) {
            setTypeRandomElement(set, &str, &len, &llele);
            if (str == NULL) {
                addReplyBulkLongLong(c, llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            if (c->flag.close_asap) break;
        }
        return;
    }

    /* A hashtable set that may hide a member: the index answers when it holds
     * every member and few of them are live, whatever the count. */
    if (sampler.hides && set->encoding == OBJ_ENCODING_HASHTABLE) {
        smember *indexed[SET_LIVE_INDEX_MAX];
        long held = setTypeCollectLiveIndexed(set, indexed, setLiveIndexCap(set));
        if (held >= 0) {
            /* A partial Fisher-Yates over the live members the index named. */
            unsigned long n = (unsigned long)held;
            unsigned long want = count < n ? count : n;
            addReplyArrayLen(c, want);
            for (unsigned long i = 0; i < want; i++) {
                unsigned long j = i + (unsigned long)rand() % (n - i);
                smember *m = indexed[j];
                indexed[j] = indexed[i];
                addReplyBulkCBuffer(c, m, sdslen(m));
            }
            return;
        }
    }

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set, less any
     * hidden member. */
    if (count >= size) {
        setTypeIterator *si;
        void *replylen = NULL;
        if (sampler.hides)
            replylen = addReplyDeferredLen(c);
        else
            addReplyArrayLen(c, size);
        unsigned long emitted = 0;
        si = setTypeInitIterator(set);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            if (str == NULL) {
                addReplyBulkLongLong(c, llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            emitted++;
        }
        setTypeReleaseIterator(si);
        if (replylen)
            setDeferredArrayLen(c, replylen, emitted);
        else
            serverAssert(emitted == size);
        return;
    }

    /* CASE 2.5 listpack only. Sampling unique elements, in non-random order.
     * Listpack encoded sets are meant to be relatively small, so
     * SRANDMEMBER_SUB_STRATEGY_MUL isn't necessary and we rather not make
     * copies of the entries. Instead, we emit them directly to the output
     * buffer.
     *
     * And it is inefficient to repeatedly pick one random element from a
     * listpack in CASE 4. So we use this instead. */
    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        if (sampler.hides) {
            /* A partial Fisher-Yates over the live entries, collected in one pass. */
            unsigned long live_count;
            listpackEntry *live = setTypeCollectLive(set, &live_count);
            unsigned long needed = count < live_count ? count : live_count;
            addReplyArrayLen(c, needed);
            for (unsigned long i = 0; i < needed; i++) {
                unsigned long j = i + (unsigned long)rand() % (live_count - i);
                listpackEntry selected = live[j];
                live[j] = live[i];
                live[i] = selected;
                if (selected.sval)
                    addReplyBulkCBuffer(c, selected.sval, selected.slen);
                else
                    addReplyBulkLongLong(c, selected.lval);
            }
            zfree(live);
            return;
        }
        unsigned char *lp = objectGetVal(set);
        unsigned char *p = lpFirst(lp);
        unsigned int i = 0;
        addReplyArrayLen(c, count);
        while (count) {
            p = lpNextRandom(lp, p, &i, count--, 0);
            unsigned int len;
            str = (char *)lpGetValue(p, &len, (long long *)&llele);
            if (str == NULL) {
                addReplyBulkLongLong(c, llele);
            } else {
                addReplyBulkCBuffer(c, str, len);
            }
            p = lpNext(lp, p);
            i++;
        }
        return;
    }

    /* CASE 3 over a set that may hide a member: one pass selects the reply
     * from the live members. */
    if (sampler.hides && count * SRANDMEMBER_SUB_STRATEGY_MUL > size) {
        srandmemberReplyLiveSample(c, set, setTypeLiveCount(set), count);
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary hashtable. */
    hashtable *ht = hashtableCreate(&sdsReplyHashtableType);

    /* CASE 3:
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * This is done because if the number of requested elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 4 is highly inefficient. */
    if (count * SRANDMEMBER_SUB_STRATEGY_MUL > size) {
        setTypeIterator *si;

        /* Add all the elements into the temporary hashtable. */
        si = setTypeInitIterator(set);
        hashtableExpand(ht, size);
        while (setTypeNext(si, &str, &len, &llele) != -1) {
            if (str == NULL) {
                serverAssert(hashtableAdd(ht, (void *)sdsfromlonglong(llele)));
            } else {
                serverAssert(hashtableAdd(ht, (void *)sdsnewlen(str, len)));
            }
        }
        setTypeReleaseIterator(si);
        serverAssert(hashtableSize(ht) == size);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            void *element;
            hashtableFairRandomEntry(ht, &element);
            hashtableDelete(ht, element);
            sdsfree((sds)element);
            size--;
        }
    }

    /* CASE 4: We have a big set compared to the requested number of elements.
     * In this case we can simply get random elements from the set and add
     * to the temporary set, trying to eventually get enough unique elements
     * to reach the specified count. */
    else {
        unsigned long added = 0;
        sds sdsele;

        hashtableExpand(ht, count);
        while (added < count) {
            if (sampler.hides) {
                smember *m = setLiveSamplerDraw(&sampler);
                if (m == NULL) {
                    /* The budget is spent: one pass selects the whole reply instead. */
                    srandmemberReleaseAux(ht);
                    srandmemberReplyLiveSample(c, set, setTypeLiveCount(set), count);
                    return;
                }
                str = m;
                len = sdslen(m);
            } else {
                setTypeRandomElement(set, &str, &len, &llele);
            }
            if (str == NULL) {
                sdsele = sdsfromlonglong(llele);
            } else {
                sdsele = sdsnewlen(str, len);
            }
            /* Try to add the object to the dictionary. If it already exists
             * free it; otherwise, increment the number of objects we have
             * in the result dictionary. */
            if (hashtableAdd(ht, sdsele))
                added++;
            else
                sdsfree(sdsele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    {
        hashtableIterator iter;
        hashtableInitIterator(&iter, ht, 0);

        addReplyArrayLen(c, count);
        serverAssert(count == hashtableSize(ht));
        void *element;
        while (hashtableNext(&iter, &element)) addReplyBulkSds(c, (sds)element);
        hashtableCleanupIterator(&iter);
        hashtableRelease(ht);
    }
}

/* SRANDMEMBER <key> [<count>] */
void srandmemberCommand(client *c) {
    robj *set;
    char *str;
    size_t len;
    int64_t llele;

    if (c->argc == 3) {
        srandmemberWithCountCommand(c);
        return;
    } else if (c->argc > 3) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    /* Handle variant without <count> argument. Reply with simple bulk string */
    if ((set = lookupKeyReadOrReply(c, c->argv[1], shared.null[c->resp])) == NULL || checkType(c, set, OBJ_SET)) return;

    if (setTypeRandomElement(set, &str, &len, &llele) == -1) {
        addReply(c, shared.null[c->resp]);
        return;
    }
    if (str == NULL) {
        addReplyBulkLongLong(c, llele);
    } else {
        addReplyBulkCBuffer(c, str, len);
    }
}

int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    if (setTypeSize(*(robj **)s1) > setTypeSize(*(robj **)s2)) return 1;
    if (setTypeSize(*(robj **)s1) < setTypeSize(*(robj **)s2)) return -1;
    return 0;
}

/* This is used by SDIFF and in this case we can receive NULL that should
 * be handled as empty sets. */
int qsortCompareSetsByRevCardinality(const void *s1, const void *s2) {
    robj *o1 = *(robj **)s1, *o2 = *(robj **)s2;
    unsigned long first = o1 ? setTypeSize(o1) : 0;
    unsigned long second = o2 ? setTypeSize(o2) : 0;

    if (first < second) return 1;
    if (first > second) return -1;
    return 0;
}

/* SINTER / SMEMBERS / SINTERSTORE / SINTERCARD
 *
 * 'cardinality_only' work for SINTERCARD, only return the cardinality
 * with minimum processing and memory overheads.
 *
 * 'limit' work for SINTERCARD, stop searching after reaching the limit.
 * Passing a 0 means unlimited.
 */
void sinterGenericCommand(client *c,
                          robj **setkeys,
                          unsigned long setnum,
                          robj *dstkey,
                          int cardinality_only,
                          unsigned long limit) {
    robj **sets = zmalloc(sizeof(robj *) * setnum);
    setTypeIterator *si;
    robj *dstset = NULL;
    char *str;
    size_t len;
    int64_t intobj;
    void *replylen = NULL;
    unsigned long j, cardinality = 0;
    int encoding, empty = 0;
    int volatile_source = 0;

    for (j = 0; j < setnum; j++) {
        robj *setobj = lookupKeyRead(c->db, setkeys[j]);
        if (!setobj) {
            /* A NULL is considered an empty set */
            empty += 1;
            sets[j] = NULL;
            continue;
        }
        if (checkType(c, setobj, OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
        if (dstkey && setTypeHasExpiredMembers(setobj)) volatile_source = 1;
    }

    /* Set intersection with an empty set always results in an empty set.
     * Return ASAP if there is an empty set. */
    if (empty > 0) {
        zfree(sets);
        if (dstkey) {
            if (dbDelete(c->db, dstkey)) {
                signalModifiedKey(c, c->db, dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC, "del", dstkey, c->db->id);
                server.dirty++;
            }
            addReply(c, shared.czero);
        } else if (cardinality_only) {
            addReplyLongLong(c, cardinality);
        } else {
            addReply(c, shared.emptyset[c->resp]);
        }
        return;
    }

    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    qsort(sets, setnum, sizeof(robj *), qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (dstkey) {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        if (sets[0]->encoding == OBJ_ENCODING_INTSET) {
            /* The first set is an intset, so the result is an intset too. The
             * elements are inserted in ascending order which is efficient in an
             * intset. */
            dstset = createIntsetObject();
        } else if (sets[0]->encoding == OBJ_ENCODING_LISTPACK) {
            /* To avoid many reallocs, we estimate that the result is a listpack
             * of approximately the same size as the first set. Then we shrink
             * it or possibly convert it to intset in the end. */
            unsigned char *lp = lpNew(lpBytes(objectGetVal(sets[0])));
            dstset = createObject(OBJ_SET, lp);
            dstset->encoding = OBJ_ENCODING_LISTPACK;
        } else {
            /* We start off with a listpack, since it's more efficient to append
             * to than an intset. Later we can convert it to intset or a
             * hashtable. */
            dstset = createSetListpackObject();
        }
    } else if (!cardinality_only) {
        replylen = addReplyDeferredLen(c);
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    int only_integers = 1;
    si = setTypeInitIterator(sets[0]);
    while ((encoding = setTypeNext(si, &str, &len, &intobj)) != -1) {
        for (j = 1; j < setnum; j++) {
            if (sets[j] == sets[0]) continue;
            if (!setTypeIsMemberAux(sets[j], str, len, intobj, encoding == OBJ_ENCODING_HASHTABLE)) break;
        }

        /* Only take action when all sets contain the member */
        if (j == setnum) {
            if (cardinality_only) {
                cardinality++;

                /* We stop the searching after reaching the limit. */
                if (limit && cardinality >= limit) break;
            } else if (!dstkey) {
                if (str != NULL)
                    addReplyBulkCBuffer(c, str, len);
                else
                    addReplyBulkLongLong(c, intobj);
                cardinality++;
            } else {
                if (str && only_integers) {
                    /* It may be an integer although we got it as a string. */
                    if (encoding == OBJ_ENCODING_HASHTABLE && string2ll(str, len, (long long *)&intobj)) {
                        if (dstset->encoding == OBJ_ENCODING_LISTPACK || dstset->encoding == OBJ_ENCODING_INTSET) {
                            /* Adding it as an integer is more efficient. */
                            str = NULL;
                        }
                    } else {
                        /* It's not an integer */
                        only_integers = 0;
                    }
                }
                setTypeAddAux(dstset, str, len, intobj, encoding == OBJ_ENCODING_HASHTABLE);
            }
        }
    }
    setTypeReleaseIterator(si);

    if (cardinality_only) {
        addReplyLongLong(c, cardinality);
    } else if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        if (setTypeSize(dstset) > 0) {
            if (only_integers) maybeConvertToIntset(dstset);
            if (dstset->encoding == OBJ_ENCODING_LISTPACK) {
                /* We allocated too much memory when we created it to avoid
                 * frequent reallocs. Therefore, we shrink it now. */
                objectSetVal(dstset, lpShrinkToFit(objectGetVal(dstset)));
            }
            setKey(c, c->db, dstkey, &dstset, 0);
            notifyKeyspaceEvent(NOTIFY_SET, "sinterstore", dstkey, c->db->id);
            server.dirty++;
            addReplyLongLong(c, setTypeSize(dstset));
            if (volatile_source) propagateStoreAsEffects(c, dstkey, dstset);
        } else {
            if (dbDelete(c->db, dstkey)) {
                server.dirty++;
                signalModifiedKey(c, c->db, dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC, "del", dstkey, c->db->id);
            }
            /* The replica sees the hidden members as live, so an absent
             * destination must be deleted there too. */
            if (volatile_source) propagateStoreAsEffects(c, dstkey, NULL);
            addReply(c, shared.czero);
            decrRefCount(dstset);
        }
    } else {
        setDeferredSetLen(c, replylen, cardinality);
    }
    zfree(sets);
}

/* SINTER key [key ...] */
void sinterCommand(client *c) {
    sinterGenericCommand(c, c->argv + 1, c->argc - 1, NULL, 0, 0);
}

/* SINTERCARD numkeys key [key ...] [LIMIT limit] */
void sinterCardCommand(client *c) {
    long j;
    long numkeys = 0; /* Number of keys. */
    long limit = 0;   /* 0 means not limit. */

    if (getRangeLongFromObjectOrReply(c, c->argv[1], 1, LONG_MAX, &numkeys, "numkeys should be greater than 0") != C_OK)
        return;
    if (numkeys > (c->argc - 2)) {
        addReplyError(c, "Number of keys can't be greater than number of args");
        return;
    }

    for (j = 2 + numkeys; j < c->argc; j++) {
        char *opt = objectGetVal(c->argv[j]);
        int moreargs = (c->argc - 1) - j;

        if (!strcasecmp(opt, "LIMIT") && moreargs) {
            j++;
            if (getPositiveLongFromObjectOrReply(c, c->argv[j], &limit, "LIMIT can't be negative") != C_OK) return;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    sinterGenericCommand(c, c->argv + 2, numkeys, NULL, 1, limit);
}

/* SINTERSTORE destination key [key ...] */
void sinterstoreCommand(client *c) {
    sinterGenericCommand(c, c->argv + 2, c->argc - 2, c->argv[1], 0, 0);
}

void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum, robj *dstkey, int op) {
    robj **sets = zmalloc(sizeof(robj *) * setnum);
    setTypeIterator *si;
    robj *dstset = NULL;
    int dstset_encoding = OBJ_ENCODING_INTSET;
    char *str;
    size_t len;
    int64_t llval;
    int encoding;
    int j, cardinality = 0;
    int diff_algo = 1;
    int sameset = 0;
    int volatile_source = 0;

    for (j = 0; j < setnum; j++) {
        robj *setobj = lookupKeyRead(c->db, setkeys[j]);
        if (!setobj) {
            sets[j] = NULL;
            continue;
        }
        if (checkType(c, setobj, OBJ_SET)) {
            zfree(sets);
            return;
        }
        /* For a SET's encoding, according to the factory method setTypeCreate(), currently have 3 types:
         * 1. OBJ_ENCODING_INTSET
         * 2. OBJ_ENCODING_LISTPACK
         * 3. OBJ_ENCODING_HASHTABLE
         * 'dstset_encoding' is used to determine which kind of encoding to use when initialize 'dstset'.
         *
         * If all sets are all OBJ_ENCODING_INTSET encoding or 'dstkey' is not null, keep 'dstset'
         * OBJ_ENCODING_INTSET encoding when initialize. Otherwise, it is not efficient to create the 'dstset'
         * from intset and then convert to listpack or hashtable.
         *
         * If one of the set is OBJ_ENCODING_LISTPACK, let's set 'dstset' to hashtable default encoding,
         * the hashtable is more efficient when find and compare than the listpack. The corresponding
         * time complexity are O(1) vs O(n). */
        if (!dstkey && dstset_encoding == OBJ_ENCODING_INTSET &&
            (setobj->encoding == OBJ_ENCODING_LISTPACK || setobj->encoding == OBJ_ENCODING_HASHTABLE)) {
            dstset_encoding = OBJ_ENCODING_HASHTABLE;
        }
        sets[j] = setobj;
        if (dstkey && setTypeHasExpiredMembers(setobj)) volatile_source = 1;
        if (j > 0 && sets[0] == sets[j]) {
            sameset = 1;
        }
    }

    /* Select what DIFF algorithm to use.
     *
     * Algorithm 1 is O(N*M) where N is the size of the element first set
     * and M the total number of sets.
     *
     * Algorithm 2 is O(N) where N is the total number of elements in all
     * the sets.
     *
     * We compute what is the best bet with the current input here. */
    if (op == SET_OP_DIFF && sets[0] && !sameset) {
        long long algo_one_work = 0, algo_two_work = 0;

        for (j = 0; j < setnum; j++) {
            if (sets[j] == NULL) continue;

            algo_one_work += setTypeSize(sets[0]);
            algo_two_work += setTypeSize(sets[j]);
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. */
        algo_one_work /= 2;
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;

        if (diff_algo == 1 && setnum > 1) {
            /* With algorithm 1 it is better to order the sets to subtract
             * by decreasing size, so that we are more likely to find
             * duplicated elements ASAP. */
            qsort(sets + 1, setnum - 1, sizeof(robj *), qsortCompareSetsByRevCardinality);
        }
    }

    /* We need a temp set object to store our union/diff. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE/SDIFFSTORE operation) then
     * this set object will be the resulting object to set into the target key*/
    if (dstset_encoding == OBJ_ENCODING_INTSET) {
        dstset = createIntsetObject();
    } else {
        dstset = createSetObject();
    }

    if (op == SET_OP_UNION) {
        /* Union is trivial, just add every element of every set to the
         * temporary set. */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* nonexistent keys are like empty sets */

            si = setTypeInitIterator(sets[j]);
            while ((encoding = setTypeNext(si, &str, &len, &llval)) != -1) {
                cardinality += setTypeAddAux(dstset, str, len, llval, encoding == OBJ_ENCODING_HASHTABLE);
            }
            setTypeReleaseIterator(si);
        }
    } else if (op == SET_OP_DIFF && sameset) {
        /* At least one of the sets is the same one (same key) as the first one, result must be empty. */
    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 1) {
        /* DIFF Algorithm 1:
         *
         * We perform the diff by iterating all the elements of the first set,
         * and only adding it to the target set if the element does not exist
         * into all the other sets.
         *
         * This way we perform at max N*M operations, where N is the size of
         * the first set, and M the number of sets. */
        si = setTypeInitIterator(sets[0]);
        while ((encoding = setTypeNext(si, &str, &len, &llval)) != -1) {
            for (j = 1; j < setnum; j++) {
                if (!sets[j]) continue;        /* no key is an empty set. */
                if (sets[j] == sets[0]) break; /* same set! */
                if (setTypeIsMemberAux(sets[j], str, len, llval, encoding == OBJ_ENCODING_HASHTABLE)) break;
            }
            if (j == setnum) {
                /* There is no other set with this element. Add it. */
                cardinality += setTypeAddAux(dstset, str, len, llval, encoding == OBJ_ENCODING_HASHTABLE);
            }
        }
        setTypeReleaseIterator(si);
    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 2) {
        /* DIFF Algorithm 2:
         *
         * Add all the elements of the first set to the auxiliary set.
         * Then remove all the elements of all the next sets from it.
         *
         * This is O(N) where N is the sum of all the elements in every
         * set. */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* nonexistent keys are like empty sets */

            si = setTypeInitIterator(sets[j]);
            while ((encoding = setTypeNext(si, &str, &len, &llval)) != -1) {
                if (j == 0) {
                    cardinality += setTypeAddAux(dstset, str, len, llval, encoding == OBJ_ENCODING_HASHTABLE);
                } else {
                    cardinality -= setTypeRemoveAux(dstset, str, len, llval, encoding == OBJ_ENCODING_HASHTABLE);
                }
            }
            setTypeReleaseIterator(si);

            /* Exit if result set is empty as any additional removal
             * of elements will have no effect. */
            if (cardinality == 0) break;
        }
    }

    /* Output the content of the resulting set, if not in STORE mode */
    if (!dstkey) {
        addReplySetLen(c, cardinality);
        si = setTypeInitIterator(dstset);
        while (setTypeNext(si, &str, &len, &llval) != -1) {
            if (str)
                addReplyBulkCBuffer(c, str, len);
            else
                addReplyBulkLongLong(c, llval);
        }
        setTypeReleaseIterator(si);
        server.lazyfree_lazy_server_del ? freeObjAsync(NULL, dstset, -1) : decrRefCount(dstset);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        if (setTypeSize(dstset) > 0) {
            setKey(c, c->db, dstkey, &dstset, 0);
            notifyKeyspaceEvent(NOTIFY_SET, op == SET_OP_UNION ? "sunionstore" : "sdiffstore", dstkey, c->db->id);
            server.dirty++;
            addReplyLongLong(c, setTypeSize(dstset));
            if (volatile_source) propagateStoreAsEffects(c, dstkey, dstset);
        } else {
            if (dbDelete(c->db, dstkey)) {
                server.dirty++;
                signalModifiedKey(c, c->db, dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC, "del", dstkey, c->db->id);
            }
            /* The replica sees the hidden members as live, so an absent
             * destination must be deleted there too. */
            if (volatile_source) propagateStoreAsEffects(c, dstkey, NULL);
            addReply(c, shared.czero);
            decrRefCount(dstset);
        }
    }
    zfree(sets);
}

/* SUNION key [key ...] */
void sunionCommand(client *c) {
    sunionDiffGenericCommand(c, c->argv + 1, c->argc - 1, NULL, SET_OP_UNION);
}

/* SUNIONSTORE destination key [key ...] */
void sunionstoreCommand(client *c) {
    sunionDiffGenericCommand(c, c->argv + 2, c->argc - 2, c->argv[1], SET_OP_UNION);
}

/* SDIFF key [key ...] */
void sdiffCommand(client *c) {
    sunionDiffGenericCommand(c, c->argv + 1, c->argc - 1, NULL, SET_OP_DIFF);
}

/* SDIFFSTORE destination key [key ...] */
void sdiffstoreCommand(client *c) {
    sunionDiffGenericCommand(c, c->argv + 2, c->argc - 2, c->argv[1], SET_OP_DIFF);
}

void sscanCommand(client *c) {
    robj *set;
    unsigned long long cursor;

    if (parseScanCursorOrReply(c, objectGetVal(c->argv[2]), &cursor) == C_ERR) return;
    if ((set = lookupKeyReadOrReply(c, c->argv[1], shared.emptyscan)) == NULL || checkType(c, set, OBJ_SET)) return;
    scanGenericCommand(c, set, cursor);
}

static const char *nummembers_err = "nummembers should be greater than 0 and match the provided number of members";

static void sexpireGenericCommand(client *c, mstime_t basetime, int unit) {
    robj *key = c->argv[1], *param = c->argv[2];
    mstime_t when;
    int flag = 0, members_index = 3;
    long long num_members = 0;
    int i, expired = 0, updated = 0;
    robj **new_argv = NULL;
    int new_argc = 0, skipped = 0;

    for (; members_index < c->argc - 1; members_index++) {
        if (!strcasecmp(objectGetVal(c->argv[members_index]), "members")) {
            if (parseExtendedExpireArgumentsOrReply(c, &flag, members_index++) != C_OK) return;
            if (getLongLongFromObjectOrReply(c, c->argv[members_index++], &num_members, NULL) != C_OK) return;
            break;
        }
    }

    if (!num_members || num_members != (c->argc - members_index)) {
        addReplyError(c, nummembers_err);
        return;
    }

    if (convertExpireArgumentToUnixTime(c, param, basetime, unit, &when) == C_ERR)
        return;

    robj *obj = lookupKeyWrite(c->db, key);

    if (checkType(c, obj, OBJ_SET)) return;

    bool has_volatile_members = setTypeHasVolatileMembers(obj);

    initDeferredReplyBuffer(c);

    addReplyArrayLen(c, num_members);

    for (i = 0; i < num_members; i++) {
        expiryModificationResult result = setTypeSetExpiry(obj, objectGetVal(c->argv[members_index + i]), when, flag);
        if (result == EXPIRATION_MODIFICATION_SUCCESSFUL) {
            if (skipped) {
                if (!new_argv)
                    new_argv = createChangedItemsArgv(c, c->argc, members_index, members_index, skipped, &new_argc);
                new_argv[new_argc++] = c->argv[members_index + i];
                incrRefCount(c->argv[members_index + i]);
            }
            updated++;
        } else if (result == EXPIRATION_MODIFICATION_EXPIRE_ASAP) {
            if (new_argv == NULL) {
                new_argv = zmalloc(sizeof(robj *) * (num_members + 2));
                new_argv[new_argc++] = shared.srem;
                new_argv[new_argc++] = c->argv[1];
                incrRefCount(c->argv[1]);
            }
            new_argv[new_argc++] = c->argv[members_index + i];
            incrRefCount(c->argv[members_index + i]);
            server.stat_expiredsetmembers++;
            expired++;
        } else if (has_volatile_members && result == EXPIRATION_MODIFICATION_NOT_EXIST && !skipped) {
            skipped = members_index + i;
        }
        addReplyLongLong(c, result);
    }

    if (expired || updated) {
        if (has_volatile_members != setTypeHasVolatileMembers(obj)) {
            dbUpdateObjectWithVolatileItemsTracking(c->db, obj);
        }
        if (expired) {
            replaceClientCommandVector(c, new_argc, new_argv);
            notifyKeyspaceEvent(NOTIFY_SET, "sexpired", c->argv[1], c->db->id);
        } else if (updated) {
            if (skipped) {
                if (!new_argv)
                    new_argv = createChangedItemsArgv(c, c->argc, members_index, members_index, skipped, &new_argc);
                for (i = 0; i < members_index - 1; i++) {
                    new_argv[i] = c->argv[i];
                    incrRefCount(c->argv[i]);
                }
                new_argv[members_index - 1] = createStringObjectFromLongLong(new_argc - members_index);
                replaceClientCommandVector(c, new_argc, new_argv);
            }
            if (c->cmd->proc != spexpireatCommand) rewriteClientCommandArgument(c, 0, shared.spexpireat);

            /* Reuse argv[2] when it already contains the absolute millisecond time. */
            if (basetime != 0 || unit == UNIT_SECONDS) {
                robj *when_obj = createStringObjectFromLongLong(when);
                rewriteClientCommandArgument(c, 2, when_obj);
                decrRefCount(when_obj);
            }
            notifyKeyspaceEvent(NOTIFY_SET, "sexpire", c->argv[1], c->db->id);
        }
        server.dirty += expired + updated;
        signalModifiedKey(c, c->db, c->argv[1]);
        if (setTypeSize(obj) == 0) {
            dbDelete(c->db, c->argv[1]);
            notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
        }
    }

    commitDeferredReplyBuffer(c, 1);
}

void sexpireCommand(client *c) {
    sexpireGenericCommand(c, commandTimeSnapshot(), UNIT_SECONDS);
}

void sexpireatCommand(client *c) {
    sexpireGenericCommand(c, 0, UNIT_SECONDS);
}

void spexpireCommand(client *c) {
    sexpireGenericCommand(c, commandTimeSnapshot(), UNIT_MILLISECONDS);
}

void spexpireatCommand(client *c) {
    sexpireGenericCommand(c, 0, UNIT_MILLISECONDS);
}

void spersistCommand(client *c) {
    int members_index = 4, changes = 0;
    long long num_members = 0;

    if (strcasecmp(objectGetVal(c->argv[members_index - 2]), "members")) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    if (getLongLongFromObjectOrReply(c, c->argv[members_index - 1], &num_members, NULL) != C_OK) return;

    if (!num_members || num_members != (c->argc - members_index)) {
        addReplyError(c, nummembers_err);
        return;
    }

    robj *set = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, set, OBJ_SET)) return;

    initDeferredReplyBuffer(c);

    addReplyArrayLen(c, num_members);

    bool has_volatile_members = setTypeHasVolatileMembers(set);
    robj **new_argv = NULL;
    int new_argc = 0, skipped = 0;
    int first_member = members_index;

    for (int i = 0; i < num_members; i++, members_index++) {
        expiryModificationResult result = setTypeSetExpiry(set, objectGetVal(c->argv[members_index]), EXPIRY_NONE, 0);
        if (result == EXPIRATION_MODIFICATION_SUCCESSFUL) {
            if (skipped) {
                if (!new_argv)
                    new_argv = createChangedItemsArgv(c, c->argc, first_member, first_member, skipped, &new_argc);
                new_argv[new_argc++] = c->argv[members_index];
                incrRefCount(c->argv[members_index]);
            }
            server.dirty++;
            changes++;
        } else if (has_volatile_members && result == EXPIRATION_MODIFICATION_NOT_EXIST && !skipped) {
            skipped = members_index;
        }
        addReplyLongLong(c, result);
    }
    if (changes) {
        if (has_volatile_members != setTypeHasVolatileMembers(set)) {
            dbUpdateObjectWithVolatileItemsTracking(c->db, set);
        }
        if (skipped) {
            if (!new_argv) new_argv = createChangedItemsArgv(c, c->argc, first_member, first_member, skipped, &new_argc);
            for (int i = 0; i < first_member - 1; i++) {
                new_argv[i] = c->argv[i];
                incrRefCount(c->argv[i]);
            }
            new_argv[first_member - 1] = createStringObjectFromLongLong(new_argc - first_member);
            replaceClientCommandVector(c, new_argc, new_argv);
        }
        notifyKeyspaceEvent(NOTIFY_SET, "spersist", c->argv[1], c->db->id);
        signalModifiedKey(c, c->db, c->argv[1]);
    }

    commitDeferredReplyBuffer(c, 1);
}

static void sttlGenericCommand(client *c, mstime_t basetime, int unit) {
    int members_index = 4;
    long long num_members = 0;

    if (strcasecmp(objectGetVal(c->argv[members_index - 2]), "members")) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    if (getLongLongFromObjectOrReply(c, c->argv[members_index - 1], &num_members, NULL) != C_OK) return;

    /* A syntax error, as HTTL replies; the hash and set write commands reply the count error. */
    if (!num_members || num_members != (c->argc - members_index)) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    robj *set = lookupKeyRead(c->db, c->argv[1]);

    if (checkType(c, set, OBJ_SET)) return;

    addReplyArrayLen(c, num_members);

    for (int i = 0; i < num_members; i++) {
        mstime_t result;
        if (!set || setTypeGetExpiry(set, objectGetVal(c->argv[members_index + i]), &result) == C_ERR) {
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

void sttlCommand(client *c) {
    sttlGenericCommand(c, commandTimeSnapshot(), UNIT_SECONDS);
}

void spttlCommand(client *c) {
    sttlGenericCommand(c, commandTimeSnapshot(), UNIT_MILLISECONDS);
}

void sexpiretimeCommand(client *c) {
    sttlGenericCommand(c, 0, UNIT_SECONDS);
}

void spexpiretimeCommand(client *c) {
    sttlGenericCommand(c, 0, UNIT_MILLISECONDS);
}

/* Scan only the options: the key and members may equal option names. */
static void rewriteSaddexCommand(client *c, int members_index, int flags, robj *expire, mstime_t when) {
    UNUSED(flags);
    robj **new_argv = zmalloc(sizeof(robj *) * (c->argc + 2));
    int new_argc = 0;
    int options_end = members_index - 2;

    for (int i = 0; i < 2; i++) {
        new_argv[new_argc++] = c->argv[i];
        incrRefCount(c->argv[i]);
    }

    /* Replay the resolved expiration once, as an absolute PXAT. A repeated unit
     * (parseExtendedCommandArgumentsOrReply accepts e.g. "EX 100 EX 200") must not
     * leave a stale option-value pair beside the rewritten one, so every
     * EX/PX/EXAT/PXAT pair is dropped and a single PXAT emitted. NX/XX/MNX/MXX are
     * the resolved condition and are not replayed. */
    if (expire) {
        new_argv[new_argc++] = shared.pxat;
        new_argv[new_argc++] = createStringObjectFromLongLong(when);
    }

    for (int i = 2; i < options_end; i++) {
        char *opt = objectGetVal(c->argv[i]);
        if (!strcasecmp(opt, "NX") || !strcasecmp(opt, "XX") || !strcasecmp(opt, "MNX") || !strcasecmp(opt, "MXX")) {
            continue;
        }
        if (!strcasecmp(opt, "EX") || !strcasecmp(opt, "PX") || !strcasecmp(opt, "EXAT") || !strcasecmp(opt, "PXAT")) {
            i++; /* drop the unit and its value */
            continue;
        }
        /* Any remaining option (e.g. KEEPTTL) is a single token kept as-is. */
        new_argv[new_argc++] = c->argv[i];
        incrRefCount(c->argv[i]);
    }

    for (int i = options_end; i < c->argc; i++) {
        new_argv[new_argc++] = c->argv[i];
        incrRefCount(c->argv[i]);
    }
    replaceClientCommandVector(c, new_argc, new_argv);
}

void saddexCommand(client *c) {
    robj *o;
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = ARGS_NO_FLAGS;
    long long num_members = 0;
    mstime_t when = EXPIRY_NONE;
    int i, changes = 0, num_expired = 0;
    int set_expired = 0;
    robj **new_argv = NULL;
    int new_argc = 0;
    robj **expired_members = NULL;

    int members_index = 2;
    for (; members_index < c->argc - 1; members_index++) {
        if (!strcasecmp(objectGetVal(c->argv[members_index]), "members")) {
            if (parseExtendedCommandArgumentsOrReply(c, COMMAND_SADDEX, 2, members_index++, &flags, &unit, NULL,
                                                     &expire, NULL, NULL, NULL, NULL) != C_OK)
                return;
            if (getLongLongFromObjectOrReply(c, c->argv[members_index++], &num_members, NULL) != C_OK) return;
            break;
        }
    }

    if (!num_members || num_members != (c->argc - members_index)) {
        addReplyError(c, nummembers_err);
        return;
    }

    o = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c, o, OBJ_SET)) return;

    if (((flags & ARGS_SET_NX) && o != NULL) ||
        ((flags & ARGS_SET_XX) && o == NULL)) {
        addReply(c, shared.czero);
        return;
    }

    if (expire) {
        mstime_t basetime = (flags & (ARGS_EXAT | ARGS_PXAT)) ? 0 : commandTimeSnapshot();

        if (convertExpireArgumentToUnixTime(c, expire, basetime, unit, &when) == C_ERR)
            return;

        if (checkAlreadyExpired(when)) set_expired = 1;
    }

    if (flags & (ARGS_SET_FNX | ARGS_SET_FXX)) {
        if (o) {
            for (i = members_index; i < c->argc; i++) {
                if (((flags & ARGS_SET_FNX) && setTypeIsMember(o, objectGetVal(c->argv[i]))) ||
                    ((flags & ARGS_SET_FXX) && !setTypeIsMember(o, objectGetVal(c->argv[i])))) {
                    addReply(c, shared.czero);
                    return;
                }
            }
        } else if (flags & ARGS_SET_FXX) {
            addReply(c, shared.czero);
            return;
        }
    }

    if (o == NULL) {
        /* As HSETEX does, a past expiration still creates the key, so the call notifies like any
         * other and the empty-set check below deletes it again. */
        o = set_expired ? createSetListpackObject() : setTypeCreate(objectGetVal(c->argv[members_index]), num_members);
        dbAdd(c->db, c->argv[1], &o);
    }

    bool has_volatile_members = setTypeHasVolatileMembers(o);
    size_t original_size = setTypeSize(o);
    mstime_t key_expire = objectGetExpire(o);

    if (set_expired) {
        setTypeIgnoreTTL(o, true);
        for (i = members_index; i < c->argc; i++) {
            if (setTypeRemove(o, objectGetVal(c->argv[i]))) {
                if (new_argv == NULL) {
                    new_argv = zmalloc(sizeof(robj *) * (num_members + 2));
                    new_argv[new_argc++] = shared.srem;
                    new_argv[new_argc++] = c->argv[1];
                    incrRefCount(c->argv[1]);
                }
                new_argv[new_argc++] = c->argv[i];
                incrRefCount(c->argv[i]);
                server.stat_expiredsetmembers++;
                changes++;
            }
        }
        setTypeIgnoreTTL(o, false);

        if (changes) {
            if (has_volatile_members != setTypeHasVolatileMembers(o)) {
                dbUpdateObjectWithVolatileItemsTracking(c->db, o);
            }
            replaceClientCommandVector(c, new_argc, new_argv);
            signalModifiedKey(c, c->db, c->argv[1]);
            server.dirty += changes;
        }
    } else {
        int add_flags = (flags & ARGS_KEEPTTL) ? SET_ADD_KEEP_EXPIRY : 0;
        for (i = members_index; i < c->argc; i++) {
            bool replaced_expired = false;
            bool ttl_changed = false;
            if (setTypeAddWithExpiry(o, objectGetVal(c->argv[i]), when, add_flags, &replaced_expired, &ttl_changed))
                changes++;
            if (ttl_changed) changes++;

            if (replaced_expired) {
                if (flags & ARGS_KEEPTTL) {
                    if (expired_members == NULL) expired_members = zmalloc(sizeof(robj *) * num_members);
                    expired_members[num_expired] = c->argv[i];
                    incrRefCount(c->argv[i]);
                }
                num_expired++;
            }
        }
        if (changes) {
            if (has_volatile_members != setTypeHasVolatileMembers(o)) {
                dbUpdateObjectWithVolatileItemsTracking(c->db, o);
            }

            if (num_expired) {
                server.stat_expiredsetmembers += num_expired;
                notifyKeyspaceEvent(NOTIFY_SET, "sexpired", c->argv[1], c->db->id);
                if (expired_members != NULL) {
                    int idx = 0;
                    while (idx < num_expired) {
                        idx += propagateItemsDeletion(c->db, o, num_expired - idx, &expired_members[idx], c->slot);
                    }
                    zfree(expired_members);
                }
            }

            if (flags & (ARGS_SET_NX | ARGS_SET_XX | ARGS_SET_FNX | ARGS_SET_FXX | ARGS_EX | ARGS_PX | ARGS_EXAT)) {
                rewriteSaddexCommand(c, members_index, flags, expire, when);
            }
            if ((flags & ARGS_KEEPTTL) && num_expired == (int)original_size && key_expire != EXPIRY_NONE)
                propagateCommandAndKeyExpiration(c, c->argv[1], key_expire);

            signalModifiedKey(c, c->db, c->argv[1]);
            server.dirty += changes;
        }
    }

    /* As for HSETEX, a call that passed its checks notifies whether or not it changed the set:
     * sadd always, sexpire when it carries an expiration time and sexpired when that time has
     * passed. No spersist: HSETEX emits no hpersist when it clears a TTL. */
    notifyKeyspaceEvent(NOTIFY_SET, "sadd", c->argv[1], c->db->id);
    if (expire) notifyKeyspaceEvent(NOTIFY_SET, "sexpire", c->argv[1], c->db->id);
    if (set_expired) notifyKeyspaceEvent(NOTIFY_SET, "sexpired", c->argv[1], c->db->id);

    if (setTypeSize(o) == 0) {
        dbDelete(c->db, c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", c->argv[1], c->db->id);
    }
    /* As for HSETEX: 1 once the conditions held and every member was applied, even when
     * nothing changed; only a failed NX, XX, MNX or MXX condition replies 0. */
    addReply(c, shared.cone);
}
