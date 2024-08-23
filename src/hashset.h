/* Copyright (c) 2024-present, Valkey contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    * Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
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

#ifndef HASHSET_H
#define HASHSET_H

/* Hash Set Implementation.
 *
 * This is a cache friendly hash table implementation. It uses an open
 * addressing scheme with buckets of 64 bytes (one cache line).
 *
 * Functions and types are prefixed by "hashset", macros by "HASHSET".
 *
 * Terminology:
 *
 * hashset
 *         An instance of the data structure, a set of elements.
 *
 * key
 *         A key used for looking up an element in the hashset.
 *
 * element
 *         An element in the hashset. This may be of the same type as the key,
 *         or a struct containing a key and other fields.
 *
 * type
 *         A struct containing callbacks, such as hash function, key comparison
 *         function and how to get the key in an element.
 */

#include <stdint.h>

/* --- Opaque types --- */

typedef struct hashset hashset;

/* --- Non-opaque types --- */

/* The hashsetType is a set of callbacks for a hashset. */
typedef struct {
    /* If the type of an element is not the same as the type of a key used for
     * lookup, this callback needs to return the key within an element. */
    const void *(*elementGetKey)(const void *element);
    uint64_t (*hashFunction)(const void *key);
    int (*keyCompare)(hashset *s, const void *key1, const void *key2);
    void (*elementDestructor)(hashset *s, void *elem);
    int (*resizeAllowed)(size_t moreMem, double usedRatio);
    /* Invoked at the start of rehashing. Both tables are already created. */
    void (*rehashingStarted)(hashset *s);
    /* Invoked at the end of rehashing. Both tables still exist and are cleaned
     * up after this callback. */
    void (*rehashingCompleted)(hashset *s);
    /* Allow a hashset to carry extra caller-defined metadata. The extra memory
     * is initialized to 0. */
    size_t (*getMetadataSize)(void);
    /* Allow the caller to store some data here in the type. It's useful for the
     * rehashingStarted and rehashingCompleted callbacks. */
    void *userdata;
} hashsetType;

typedef enum {
    HASHSET_RESIZE_ALLOW = 0,
    HASHSET_RESIZE_AVOID,
    HASHSET_RESIZE_FORBID,
} hashsetResizePolicy;

/* TODO: Iterator, stats, scan callback typedefs, defrag functions */

/* TODO: Type flag to disable incremental rehashing. */

/* --- Inline functions --- */

/* --- Prototypes --- */

/* Hash function (global seed) */
void hashsetSetHashFunctionSeed(uint8_t *seed);
uint8_t *hashsetGetHashFunctionSeed(void);
uint64_t hashsetGenHashFunction(const char *buf, size_t len);
uint64_t hashsetGenCaseHashFunction(const char *buf, size_t len);

/* Global resize policy */
void hashsetSetResizePolicy(hashsetResizePolicy policy);

hashset *hashsetCreate(hashsetType *type);
hashsetType *hashsetGetType(hashset *s);
void *hashsetMetadata(hashset *s);
size_t hashsetNumBuckets(hashset *s);
size_t hashsetCount(hashset *s);
void hashtablePauseAutoResize(hashset *s);
void hashsetResumeAutoResize(hashset *s);
int hashsetIsRehashing(hashset *s);

int hashsetFind(hashset *s, const void *key, void **found);
int hashsetAdd(hashset *s, void *elem);
int hashsetAddRaw(hashset *s, void *elem, void **existing);
int hashsetReplace(hashset *s, void *elem);

#endif
