/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hashtable_helpers.h"
#include "adlist.h"
#include "dict.h"
#include "sds.h"
#include "util.h"
#include "zmalloc.h"

#include <string.h>
#include <strings.h>

void dictVanillaFree(void *val) {
    zfree(val);
}

void dictListDestructor(void *val) {
    listRelease((list *)val);
}

void dictHashtableDestructor(void *val) {
    hashtableRelease((hashtable *)val);
}

/* Returns 1 when keys match */
int dictSdsKeyCompare(const void *key1, const void *key2) {
    int l1, l2;
    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* A case-insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. Works
 * for both sds and null terminated string keys. */
int dictSdsKeyCaseCompare(const void *key1, const void *key2) {
    return strcasecmp(key1, key2) == 0;
}

void dictSdsDestructor(void *val) {
    sdsfree(val);
}

uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction(key, sdslen(key));
}

/* Hash function using a configurable seed (set via hash-seed config).
 * Used for data hashtables (keys, sets, zsets, hashes) where deterministic
 * iteration order across cluster nodes is needed. */
static uint8_t configurable_hash_seed[16];

extern uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);

void setConfigurableHashSeed(uint8_t *seed) {
    memcpy(configurable_hash_seed, seed, sizeof(configurable_hash_seed));
}

uint8_t *getConfigurableHashSeed(void) {
    return configurable_hash_seed;
}

uint64_t genHashFunctionConfigurableSeed(const char *buf, size_t len) {
    return siphash((const uint8_t *)buf, len, configurable_hash_seed);
}

uint64_t sdsHashConfigurableSeed(const void *key) {
    return genHashFunctionConfigurableSeed(key, sdslen(key));
}

uint64_t dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction(key, sdslen(key));
}

/* Dict hash function for null terminated string */
uint64_t dictCStrHash(const void *key) {
    return dictGenHashFunction(key, strlen(key));
}

/* Dict hash function for null terminated string */
uint64_t dictCStrCaseHash(const void *key) {
    return dictGenCaseHashFunction(key, strlen((char *)key));
}

/* Hash function for tables keyed by a pointer value. Cheaper than the default,
 * which runs siphash over the pointer's bytes. */
uint64_t hashtablePointerHash(const void *key) {
    return wangHash64((uint64_t)(uintptr_t)key);
}

/* Dict compare function for null terminated string */
int dictCStrKeyCompare(const void *key1, const void *key2) {
    return strcmp(key1, key2) == 0;
}

/* Entry destructor that frees SDS key and the dictEntry itself */
void dictEntryDestructorSdsKey(void *entry) {
    dictEntry *de = entry;
    dictSdsDestructor(dictGetKey(de));
    zfree(de);
}

void dictEntryDestructorSdsKeyValue(void *entry) {
    dictEntry *de = entry;
    dictSdsDestructor(dictGetKey(de));
    dictSdsDestructor(dictGetVal(de));
    zfree(de);
}

/* Entry destructor that frees SDS key, list value, and the dictEntry itself */
void dictEntryDestructorSdsKeyListValue(void *entry) {
    dictEntry *de = entry;
    dictSdsDestructor(dictGetKey(de));
    dictListDestructor(dictGetVal(de));
    zfree(de);
}

/* Entry destructor that frees SDS key, heap pointer value, and the dictEntry itself */
void dictEntryDestructorSdsKeyHeapValue(void *entry) {
    dictEntry *de = entry;
    dictSdsDestructor(dictGetKey(de));
    zfree(dictGetVal(de));
    zfree(de);
}
