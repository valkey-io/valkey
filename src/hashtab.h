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

#ifndef HASHTAB_H
#define HASHTAB_H

/* Hash table implementation.
 *
 * This is a cache-friendly hash table implementation. It uses an open
 * addressing scheme with buckets of 64 bytes (one cache line).
 *
 * Terminology:
 *
 * hashtab
 *         An instance of the data structure, a set of elements.
 *
 * key
 *         A key used for looking up an element in the hashtab.
 *
 * element
 *         An element in the hashtab. This may be of the same type as the key,
 *         or a struct containing a key and other fields.
 *
 * type
 *         A struct containing callbacks, such as hash function, key comparison
 *         function and how to get the key in an element.
 */

#include <stddef.h>
#include <stdint.h>

/* --- Opaque types --- */

typedef struct hashtab hashtab;

/* --- Non-opaque types --- */

/* The hashtabType is a set of callbacks for a hashtab. All callbacks are
 * optional. With all callbacks omitted, the hashtab is effectively a set of
 * pointer-sized integers. */
typedef struct {
    /* If the type of an element is not the same as the type of a key used for
     * lookup, this callback needs to return the key within an element. */
    const void *(*elementGetKey)(const void *element);
    /* Hash function. Defaults to hashing the bits in the pointer. */
    uint64_t (*hashFunction)(const void *key);
    /* Compare function, returns 0 if the keys are equal. Defaults to just
     * comparing the pointes for equality. */
    int (*keyCompare)(hashtab *s, const void *key1, const void *key2);
    /* Callback to free an element when it's overwritten or deleted.
     * Optional. */
    void (*elementDestructor)(hashtab *s, void *elem);
    /* Optional callback to control when resizing should be allowed. (Not implemented) */
    int (*resizeAllowed)(size_t moreMem, double usedRatio);
    /* Invoked at the start of rehashing. Both tables are already created. */
    void (*rehashingStarted)(hashtab *s);
    /* Invoked at the end of rehashing. Both tables still exist and are cleaned
     * up after this callback. */
    void (*rehashingCompleted)(hashtab *s);
    /* Allow a hashtab to carry extra caller-defined metadata. The extra memory
     * is initialized to 0. */
    size_t (*getMetadataSize)(void);
    /* Allow the caller to store some data here in the type. It's useful for the
     * rehashingStarted and rehashingCompleted callbacks. */
    void *userdata;
} hashtabType;

typedef enum {
    HASHTAB_RESIZE_ALLOW = 0,
    HASHTAB_RESIZE_AVOID,
    HASHTAB_RESIZE_FORBID,
} hashtabResizePolicy;

typedef void(*hashtabScanFunction)(void *privdata, void *element);

/* TODO: Iterator, stats */

/* Not needed: defrag functions (solved by emit_ref in scan) */

/* TODO: Type flag to disable incremental rehashing. */

/* --- Inline functions --- */

/* --- Prototypes --- */

/* Hash function (global seed) */
void hashtabSetHashFunctionSeed(uint8_t *seed);
uint8_t *hashtabGetHashFunctionSeed(void);
uint64_t hashtabGenHashFunction(const char *buf, size_t len);
uint64_t hashtabGenCaseHashFunction(const char *buf, size_t len);

/* Global resize policy */
void hashtabSetResizePolicy(hashtabResizePolicy policy);

hashtab *hashtabCreate(hashtabType *type);
hashtabType *hashtabGetType(hashtab *s);
void *hashtabMetadata(hashtab *s);
size_t hashtabSize(hashtab *s);
void hashtabPauseAutoShrink(hashtab *s);
void hashtabResumeAutoShrink(hashtab *s);
int hashtabIsRehashing(hashtab *s);

int hashtabExpandIfNeeded(hashtab *s);
int hashtabShrinkIfNeeded(hashtab *s);

int hashtabFind(hashtab *s, const void *key, void **found);
int hashtabAdd(hashtab *s, void *elem);
int hashtabAddRaw(hashtab *s, void *elem, void **existing);
int hashtabReplace(hashtab *s, void *elem);

size_t hashtabScan(hashtab *t, size_t cursor, hashtabScanFunction fn, void *privdata, int emit_ref);
#endif
