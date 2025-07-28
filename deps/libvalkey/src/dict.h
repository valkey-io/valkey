/* Hash table implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __DICT_H
#define __DICT_H

#include <stdint.h>

#define DICT_OK 0
#define DICT_ERR 1

typedef struct dictEntry dictEntry; /* opaque */

typedef struct dictType {
    uint64_t (*hashFunction)(const void *key);
    void *(*keyDup)(const void *key);
    int (*keyCompare)(const void *key1, const void *key2);
    void (*keyDestructor)(void *key);
    void (*valDestructor)(void *obj);
} dictType;

typedef struct dict {
    dictEntry **table;
    dictType *type;
    unsigned long size;
    unsigned long sizemask;
    unsigned long used;
} dict;

typedef struct dictIterator {
    dict *ht;
    int index;
    dictEntry *entry, *nextEntry;
} dictIterator;

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_SIZE 4

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeEntryVal(ht, entry) \
    if ((ht)->type->valDestructor)  \
    (ht)->type->valDestructor(dictGetVal(entry))

#define dictFreeEntryKey(ht, entry) \
    if ((ht)->type->keyDestructor)  \
    (ht)->type->keyDestructor(dictGetKey(entry))

#define dictCompareHashKeys(ht, key1, key2)   \
    (((ht)->type->keyCompare) ?               \
         (ht)->type->keyCompare(key1, key2) : \
         (key1) == (key2))

#define dictHashKey(ht, key) (ht)->type->hashFunction(key)

#define dictSlots(ht) ((ht)->size)
#define dictSize(ht) ((ht)->used)

/* API */
uint64_t dictGenHashFunction(const unsigned char *buf, int len);
dict *dictCreate(dictType *type);
int dictExpand(dict *ht, unsigned long size);
int dictAdd(dict *ht, void *key, void *val);
int dictReplace(dict *ht, void *key, void *val);
int dictDelete(dict *ht, const void *key);
void dictRelease(dict *ht);
dictEntry *dictFind(dict *ht, const void *key);
void dictSetKey(dict *d, dictEntry *de, void *key);
void dictSetVal(dict *d, dictEntry *de, void *val);
void *dictGetKey(const dictEntry *de);
void *dictGetVal(const dictEntry *de);
void dictInitIterator(dictIterator *iter, dict *ht);
dictEntry *dictNext(dictIterator *iter);

#endif /* __DICT_H */
