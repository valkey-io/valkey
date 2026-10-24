/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __HASHTABLE_HELPERS_H__
#define __HASHTABLE_HELPERS_H__

/*
 * Important: include fmacros.h before any system header
 * so feature-test macros like _FILE_OFFSET_BITS are in effect
 * when glibc's <features.h> is first seen (transitively via
 * headers like <stdint.h>). This prevents 32-bit off_t on
 * 32-bit builds due to include-order.
 */
#include "fmacros.h"

#include <stddef.h>
#include <stdint.h>

/* Reusable callbacks for hashtableType and dictType. These depend only on sds,
 * adlist, zmalloc and hashtable, not on server.h, so valkey-cli and
 * valkey-benchmark link them too. */

/* Hash functions */
uint64_t dictSdsHash(const void *key);
uint64_t dictSdsCaseHash(const void *key);
uint64_t dictCStrHash(const void *key);
uint64_t dictCStrCaseHash(const void *key);
uint64_t hashtablePointerHash(const void *key);

/* Hash functions using the configurable seed (hash-seed config) */
void setConfigurableHashSeed(uint8_t *seed);
uint8_t *getConfigurableHashSeed(void);
uint64_t genHashFunctionConfigurableSeed(const char *buf, size_t len);
uint64_t sdsHashConfigurableSeed(const void *key);

/* Key compare functions */
int dictSdsKeyCompare(const void *key1, const void *key2);
int dictSdsKeyCaseCompare(const void *key1, const void *key2);
int dictCStrKeyCompare(const void *key1, const void *key2);

/* Destructors */
void dictVanillaFree(void *val);
void dictSdsDestructor(void *val);
void dictListDestructor(void *val);
void dictHashtableDestructor(void *val);
void dictEntryDestructorSdsKey(void *entry);
void dictEntryDestructorSdsKeyValue(void *entry);
void dictEntryDestructorSdsKeyListValue(void *entry);
void dictEntryDestructorSdsKeyHeapValue(void *entry);

#endif /* __HASHTABLE_HELPERS_H__ */
