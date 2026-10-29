/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "serverassert.h"
#include "smember.h"

#define SMEMBER_SDS_AUX_BIT_HAS_EXPIRY 0
static_assert(SMEMBER_SDS_AUX_BIT_HAS_EXPIRY < ZSET_SDS_AUX_BIT_LOOKUP_KEY,
              "smember metadata overlaps the zset lookup-key bit");

bool smemberHasExpiry(const smember *m) {
    return sdsGetAuxBit(m, SMEMBER_SDS_AUX_BIT_HAS_EXPIRY);
}

static char *smemberGetAllocPtr(const smember *m) {
    char *buf = sdsAllocPtr(m);
    if (smemberHasExpiry(m)) buf -= sizeof(mstime_t);
    return buf;
}

static mstime_t *smemberGetExpiryRef(const smember *m) {
    serverAssert(smemberHasExpiry(m));
    return (mstime_t *)smemberGetAllocPtr(m);
}

mstime_t smemberGetExpiry(const smember *m) {
    if (smemberHasExpiry(m)) return *smemberGetExpiryRef(m);
    return EXPIRY_NONE;
}

/* The sds header records only the space that follows the prefix. */
smember *smemberCreate(const char *str, size_t len, mstime_t expiry) {
    if (expiry == EXPIRY_NONE) return sdsnewlen(str, len);

    char type = sdsReqType(len);
    if (type == SDS_TYPE_5) type = SDS_TYPE_8;
    int hdrlen = sdsHdrSize(type);
    size_t req = sizeof(mstime_t) + sdsReqSize(len, type);

    size_t bufsize;
    char *buf = zmalloc_usable(req, &bufsize);
    *(mstime_t *)buf = expiry;
    buf += sizeof(mstime_t);
    bufsize -= sizeof(mstime_t);

    size_t usable = bufsize - hdrlen - 1;
    if (usable > sdsTypeMaxSize(type)) type = sdsReqType(usable);

    smember *m = sdswrite(buf, bufsize, type, str, len);
    sdsSetAuxBit(m, SMEMBER_SDS_AUX_BIT_HAS_EXPIRY, 1);
    return m;
}

smember *smemberSetExpiry(smember *m, mstime_t expiry) {
    if (smemberHasExpiry(m) && expiry != EXPIRY_NONE) {
        *smemberGetExpiryRef(m) = expiry;
        return m;
    }
    smember *new_member = smemberCreate(m, sdslen(m), expiry);
    smemberFree(m);
    return new_member;
}

bool smemberIsExpired(const smember *m) {
    /* timestampIsExpired() treats a negative EXPIRY_NONE as never expired. */
    return timestampIsExpired(smemberGetExpiry(m));
}

void smemberFree(smember *m) {
    zfree_with_size(smemberGetAllocPtr(m), smemberMemUsage(m));
}

size_t smemberMemUsage(const smember *m) {
    size_t mem = sdsAllocSize(m);
    if (smemberHasExpiry(m)) mem += sizeof(mstime_t);
    return mem;
}

smember *smemberDefrag(smember *m, void *(*defragfn)(void *)) {
    char *allocation = smemberGetAllocPtr(m);
    size_t offset = m - allocation;
    char *new_allocation = defragfn(allocation);
    if (new_allocation == NULL) return NULL;
    return new_allocation + offset;
}
