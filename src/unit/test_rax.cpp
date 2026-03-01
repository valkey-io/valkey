/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Rax -- A radix tree implementation.
 *
 * Copyright (c) 2017-2018, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "generated_wrappers.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>

extern "C" {
#include "mt19937-64.c"
#include "rax.h"
#include "util.h"

extern bool accurate;
extern bool large_memory;
uint16_t crc16(const char *buf, int len); /* From crc16.c */
}

/* ---------------------------------------------------------------------------
 * Simple hash table implementation, no rehashing, just chaining. This is
 * used in order to test the radix tree implementation against something that
 * will always "tell the truth" :-) */

/* This is huge but we want it fast enough without rehashing needed. */
#define HT_TABLE_SIZE 100000
typedef struct htNode {
    uint64_t keylen;
    unsigned char *key;
    void *data;
    struct htNode *next;
} htNode;

typedef struct ht {
    uint64_t numele;
    htNode *table[HT_TABLE_SIZE];
} testHashtable;

/* Create a new hash table. */
static testHashtable *htNew(void) {
    testHashtable *ht = (testHashtable *)zcalloc(sizeof(*ht));
    ht->numele = 0;
    return ht;
}

/* djb2 hash function. */
static uint32_t htHash(unsigned char *s, size_t len) {
    uint32_t hash = 5381;
    for (size_t i = 0; i < len; i++) hash = hash * 33 + s[i];
    return hash % HT_TABLE_SIZE;
}

/* Low level hash table lookup function. */
static htNode *htRawLookup(testHashtable *t, unsigned char *s, size_t len, uint32_t *hash, htNode ***parentlink) {
    uint32_t h = htHash(s, len);
    if (hash) *hash = h;
    htNode *n = t->table[h];
    if (parentlink) *parentlink = &t->table[h];
    while (n) {
        if (n->keylen == len && memcmp(n->key, s, len) == 0) return n;
        if (parentlink) *parentlink = &n->next;
        n = n->next;
    }
    return nullptr;
}

/* Add an element to the hash table, return 1 if the element is new,
 * 0 if it existed and the value was updated to the new one. */
static int htAdd(testHashtable *t, unsigned char *s, size_t len, void *data) {
    uint32_t hash;
    htNode *n = htRawLookup(t, s, len, &hash, nullptr);

    if (!n) {
        n = (htNode *)zmalloc(sizeof(*n));
        n->key = (unsigned char *)zmalloc(len);
        memcpy(n->key, s, len);
        n->keylen = len;
        n->data = data;
        n->next = t->table[hash];
        t->table[hash] = n;
        t->numele++;
        return 1;
    } else {
        n->data = data;
        return 0;
    }
}

/* Remove the specified element, returns 1 on success, 0 if the element
 * was not there already. */
static int htRem(testHashtable *t, unsigned char *s, size_t len) {
    htNode **parentlink;
    htNode *n = htRawLookup(t, s, len, nullptr, &parentlink);

    if (!n) return 0;
    *parentlink = n->next;
    zfree(n->key);
    zfree(n);
    t->numele--;
    return 1;
}

static void *htNotFound = (void *)(char *)"ht-not-found";

/* Find an element inside the hash table. Returns htNotFound if the
 * element is not there, otherwise returns the associated value. */
static void *htFind(testHashtable *t, unsigned char *s, size_t len) {
    htNode *n = htRawLookup(t, s, len, nullptr, nullptr);
    if (!n) return htNotFound;
    return n->data;
}

/* Free the whole hash table including all the linked nodes. */
static void htFree(testHashtable *ht) {
    for (int j = 0; j < HT_TABLE_SIZE; j++) {
        htNode *next = ht->table[j];
        while (next) {
            htNode *this_node = next;
            next = this_node->next;
            zfree(this_node->key);
            zfree(this_node);
        }
    }
    zfree(ht);
}

/* --------------------------------------------------------------------------
 * Utility functions to generate keys, check time usage and so forth.
 * -------------------------------------------------------------------------*/

/* This is a simple Feistel network in order to turn every possible
 * uint32_t input into another "randomly" looking uint32_t. It is a
 * one to one map so there are no repetitions. */
static uint32_t int2int(uint32_t input) {
    uint16_t l = input & 0xffff;
    uint16_t r = input >> 16;
    for (int i = 0; i < 8; i++) {
        uint16_t nl = r;
        uint16_t F = (((r * 31) + (r >> 5) + 7 * 371) ^ r) & 0xffff;
        r = l ^ F;
        l = nl;
    }
    return ((uint32_t)r << 16) | l;
}

/* Turn an uint32_t integer into an alphanumerical key and return its
 * length. This function is used in order to generate keys that have
 * a large charset, so that the radix tree can be tested with many
 * children per node. */
static size_t int2alphakey(char *s, size_t maxlen, uint32_t i) {
    const char *set = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                      "abcdefghijklmnopqrstuvwxyz"
                      "0123456789";
    const size_t setlen = 62;

    if (maxlen == 0) return 0;
    maxlen--; /* Space for null term char. */
    size_t len = 0;
    while (len < maxlen) {
        s[len++] = set[i % setlen];
        i /= setlen;
        if (i == 0) break;
    }
    s[len] = '\0';
    return len;
}

/* Turn the integer 'i' into a key according to 'mode'.
 * KEY_INT: Just represents the integer as a string.
 * KEY_UNIQUE_ALPHA: Turn it into a random-looking alphanumerical string
 *                   according to the int2alphakey() function, so that
 *                   at every integer is mapped a different string.
 * KEY_RANDOM: Totally random string up to maxlen bytes.
 * KEY_RANDOM_ALPHA: Alphanumerical random string up to maxlen bytes.
 * KEY_RANDOM_SMALL_CSET: Small charset random strings.
 * KEY_CHAIN: 'i' times the character "A". */
#define KEY_INT 0
#define KEY_UNIQUE_ALPHA 1
#define KEY_RANDOM 2
#define KEY_RANDOM_ALPHA 3
#define KEY_RANDOM_SMALL_CSET 4
#define KEY_CHAIN 5
static size_t int2key(char *s, size_t maxlen, uint32_t i, int mode) {
    if (mode == KEY_INT) {
        return snprintf(s, maxlen, "%lu", (unsigned long)i);
    } else if (mode == KEY_UNIQUE_ALPHA) {
        if (maxlen > 16) maxlen = 16;
        i = int2int(i);
        return int2alphakey(s, maxlen, i);
    } else if (mode == KEY_RANDOM) {
        if (maxlen > 16) maxlen = 16;
        int r = genrand64_int64() % maxlen;
        for (int j = 0; j < r; j++) s[j] = genrand64_int64() & 0xff;
        return r;
    } else if (mode == KEY_RANDOM_ALPHA) {
        if (maxlen > 16) maxlen = 16;
        int r = genrand64_int64() % maxlen;
        for (int j = 0; j < r; j++) s[j] = 'A' + genrand64_int64() % ('z' - 'A' + 1);
        return r;
    } else if (mode == KEY_RANDOM_SMALL_CSET) {
        if (maxlen > 16) maxlen = 16;
        int r = genrand64_int64() % maxlen;
        for (int j = 0; j < r; j++) s[j] = 'A' + genrand64_int64() % 4;
        return r;
    } else if (mode == KEY_CHAIN) {
        if (i > maxlen) i = maxlen;
        memset(s, 'A', i);
        return i;
    } else {
        return 0;
    }
}

/* -------------------------------------------------------------------------- */

/* Perform a fuzz test, returns 0 on success, 1 on error. */
int fuzzTest(int keymode, size_t count, double addprob, double remprob) {
    testHashtable *ht = htNew();
    rax *rax_tree = raxNew();

    printf("Fuzz test in mode %d [%zu]: ", keymode, count);
    fflush(stdout);

    /* Perform random operations on both the dictionaries. */
    for (size_t i = 0; i < count; i++) {
        unsigned char key[1024];
        uint32_t keylen;

        /* Insert element. */
        if ((double)genrand64_int64() / RAND_MAX < addprob) {
            keylen = int2key((char *)key, sizeof(key), i, keymode);
            void *val = (void *)(unsigned long)genrand64_int64();
            /* Stress NULL values more often, they use a special encoding. */
            if (!(genrand64_int64() % 100)) val = nullptr;
            int retval1 = htAdd(ht, key, keylen, val);
            int retval2 = raxInsert(rax_tree, key, keylen, val, nullptr);
            if (retval1 != retval2) {
                printf("Fuzz: key insertion reported mismatching value in HT/RAX\n");
                return 1;
            }
        }

        /* Remove element. */
        if ((double)genrand64_int64() / RAND_MAX < remprob) {
            keylen = int2key((char *)key, sizeof(key), i, keymode);
            int retval1 = htRem(ht, key, keylen);
            int retval2 = raxRemove(rax_tree, key, keylen, nullptr);
            if (retval1 != retval2) {
                printf("Fuzz: key deletion of '%.*s' reported mismatching "
                       "value in HT=%d RAX=%d\n",
                       (int)keylen, (char *)key, retval1, retval2);
                return 1;
            }
        }
    }

    /* Check that count matches. */
    if (ht->numele != raxSize(rax_tree)) {
        printf("Fuzz: HT / RAX keys count mismatch: %lu vs %lu\n", (unsigned long)ht->numele,
               (unsigned long)raxSize(rax_tree));
        return 1;
    }
    printf("%lu elements inserted\n", (unsigned long)ht->numele);

    /* Check that elements match. */
    raxIterator iter;
    raxStart(&iter, rax_tree);
    raxSeek(&iter, "^", nullptr, 0);

    size_t numkeys = 0;
    while (raxNext(&iter)) {
        void *val1 = htFind(ht, iter.key, iter.key_len);
        void *val2 = nullptr;
        raxFind(rax_tree, iter.key, iter.key_len, &val2);
        if (val1 != val2) {
            printf("Fuzz: HT=%p, RAX=%p value do not match "
                   "for key %.*s\n",
                   val1, val2, (int)iter.key_len, (char *)iter.key);
            return 1;
        }
        numkeys++;
    }

    /* Check that the iterator reported all the elements. */
    if (ht->numele != numkeys) {
        printf("Fuzz: the iterator reported %lu keys instead of %lu\n", (unsigned long)numkeys,
               (unsigned long)ht->numele);
        return 1;
    }

    raxStop(&iter);
    raxFree(rax_tree);
    htFree(ht);
    return 0;
}

/* Redis Cluster alike fuzz testing.
 *
 * This test simulates the radix tree usage made by Redis Cluster in order
 * to maintain the hash slot -> keys mapping. The keys are alphanumerical
 * but the first two bytes that are binary (and are the key hashed).
 *
 * In this test there is no comparison with the hash table, the only goal
 * is to crash the radix tree implementation, or to trigger Valgrind
 * warnings. */
int fuzzTestCluster(size_t count, double addprob, double remprob) {
    unsigned char key[128];
    int keylen = 0;
    size_t used_memory_before = zmalloc_used_memory();

    printf("Cluster Fuzz test [keys:%zu keylen:%d]: ", count, keylen);
    fflush(stdout);

    rax *rax_tree = raxNew();

    /* This is our template to generate keys. The first two bytes will
     * be replaced with the binary redis cluster hash slot. */
    keylen = snprintf((char *)key, sizeof(key), "__geocode:2e68e5df3624");
    const char *cset = "0123456789abcdef";

    for (unsigned long j = 0; j < count; j++) {
        /* Generate a random key by altering our template key. */

        /* With a given probability, let's use a common prefix so that there
         * is a subset of keys that have an higher percentage of probability
         * of being hit again and again. */
        size_t commonprefix = genrand64_int64() & 0xf;
        if (commonprefix == 0) memcpy(key + 10, "2e68e5", 6);

        /* Alter a random char in the key. */
        int pos = 10 + genrand64_int64() % 12;
        key[pos] = cset[genrand64_int64() % 16];

        /* Compute the Redis Cluster hash slot to set the first two
         * binary bytes of the key. */
        int hashslot = crc16((char *)key, keylen) & 0x3FFF;
        key[0] = (hashslot >> 8) & 0xff;
        key[1] = hashslot & 0xff;

        /* Insert element. */
        if ((double)genrand64_int64() / RAND_MAX < addprob) {
            raxInsert(rax_tree, key, keylen, nullptr, nullptr);
            EXPECT_EQ(raxAllocSize(rax_tree) + used_memory_before, zmalloc_used_memory());
        }

        /* Remove element. */
        if ((double)genrand64_int64() / RAND_MAX < remprob) {
            raxRemove(rax_tree, key, keylen, nullptr);
            EXPECT_EQ(raxAllocSize(rax_tree) + used_memory_before, zmalloc_used_memory());
        }
    }
    size_t finalkeys = raxSize(rax_tree);
    raxFree(rax_tree);
    printf("ok with %zu final keys\n", finalkeys);
    return 0;
}

/* Iterator fuzz testing. Compared the items returned by the Rax iterator with
 * a C implementation obtained by sorting the inserted strings in a linear
 * array. */
typedef struct arrayItem {
    unsigned char *key;
    size_t key_len;
} arrayItem;

/* Utility functions used with qsort() in order to sort the array of strings
 * in the same way Rax sorts keys (which is, lexicographically considering
 * every byte an unsigned integer. */
int compareAB(const unsigned char *keya, size_t lena, const unsigned char *keyb, size_t lenb) {
    size_t minlen = (lena <= lenb) ? lena : lenb;
    int retval = memcmp(keya, keyb, minlen);
    if (lena == lenb || retval != 0) return retval;
    return (lena > lenb) ? 1 : -1;
}

int compareArrayItems(const void *aptr, const void *bptr) {
    const arrayItem *a = (const arrayItem *)aptr;
    const arrayItem *b = (const arrayItem *)bptr;
    return compareAB(a->key, a->key_len, b->key, b->key_len);
}

/* Seek an element in the array, returning the seek index (the index inside the
 * array). If the seek is not possible (== operator and key not found or empty
 * array) -1 is returned. */
int arraySeek(arrayItem *array, int count, unsigned char *key, size_t len, const char *op) {
    if (count == 0) return -1;
    if (op[0] == '^') return 0;
    if (op[0] == '$') return count - 1;

    int eq = 0, lt = 0, gt = 0;
    if (op[1] == '=') eq = 1;
    if (op[0] == '<') lt = 1;
    if (op[0] == '>') gt = 1;

    int i;
    for (i = 0; i < count; i++) {
        int cmp = compareAB(array[i].key, array[i].key_len, key, len);
        if (eq && !cmp) return i;
        if (cmp > 0 && gt) return i;
        if (cmp >= 0 && lt) {
            i--;
            break;
        }
    }
    if (lt && i == count) return count - 1;
    if (i < 0 || i >= count) return -1;
    return i;
}

int iteratorFuzzTest(int keymode, size_t count) {
    count = genrand64_int64() % count;
    rax *rax_tree = raxNew();
    arrayItem *array = (arrayItem *)zmalloc(sizeof(arrayItem) * count);

    /* Fill a radix tree and a linear array with some data. */
    unsigned char key[1024];
    size_t j = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t keylen = int2key((char *)key, sizeof(key), i, keymode);
        void *val = (void *)(unsigned long)htHash(key, keylen);

        if (raxInsert(rax_tree, key, keylen, val, nullptr)) {
            array[j].key = (unsigned char *)zmalloc(keylen);
            array[j].key_len = keylen;
            memcpy(array[j].key, key, keylen);
            j++;
        }
    }
    count = raxSize(rax_tree);

    /* Sort the array. */
    qsort(array, count, sizeof(arrayItem), compareArrayItems);

    /* Perform a random seek operation. */
    uint32_t keylen = int2key((char *)key, sizeof(key), genrand64_int64() % (count ? count : 1), keymode);
    raxIterator iter;
    raxStart(&iter, rax_tree);
    const char *seekops[] = {"==", ">=", "<=", ">", "<", "^", "$"};
    const char *seekop = seekops[genrand64_int64() % 7];
    raxSeek(&iter, seekop, key, keylen);
    int seekidx = arraySeek(array, count, key, keylen, seekop);

    int next = genrand64_int64() % 2;
    int iteration = 0;
    while (1) {
        int rax_res;
        int array_res;
        unsigned char *array_key = nullptr;
        size_t array_key_len = 0;

        array_res = (seekidx == -1) ? 0 : 1;
        if (array_res) {
            if (next && seekidx == (signed)count) array_res = 0;
            if (!next && seekidx == -1) array_res = 0;
            if (array_res != 0) {
                array_key = array[seekidx].key;
                array_key_len = array[seekidx].key_len;
            }
        }

        if (next) {
            rax_res = raxNext(&iter);
            if (array_res) seekidx++;
        } else {
            rax_res = raxPrev(&iter);
            if (array_res) seekidx--;
        }

        /* Both the iterators should agree about EOF. */
        if (array_res != rax_res) {
            printf("Iter fuzz: iterators do not agree about EOF "
                   "at iteration %d:  "
                   "array_more=%d rax_more=%d next=%d\n",
                   iteration, array_res, rax_res, next);
            return 1;
        }
        if (array_res == 0) break; /* End of iteration reached. */

        /* Check that the returned keys are the same. */
        if (iter.key_len != array_key_len || memcmp(iter.key, array_key, iter.key_len)) {
            printf("Iter fuzz: returned element %d mismatch\n", iteration);
            printf("SEEKOP was %s\n", seekop);
            if (keymode != KEY_RANDOM) {
                printf("\n");
                printf("BUG SEEKING: %s %.*s\n", seekop, keylen, key);
                printf("%.*s (iter) VS %.*s (array) next=%d idx=%d "
                       "count=%lu keymode=%d\n",
                       (int)iter.key_len, (char *)iter.key,
                       (int)array_key_len, (char *)array_key, next, seekidx,
                       (unsigned long)count, keymode);
                if (count < 500) {
                    printf("\n");
                    for (unsigned int k = 0; k < count; k++) {
                        printf("%d) '%.*s'\n", k, (int)array[k].key_len, array[k].key);
                    }
                }
                exit(1);
            }
            return 1;
        }
        iteration++;
    }

    for (unsigned int i = 0; i < count; i++) zfree(array[i].key);
    zfree(array);
    raxStop(&iter);
    raxFree(rax_tree);
    return 0;
}

/* Test fixture */
class RaxTest : public ::testing::Test {
  protected:
    void SetUp() override {
        /* Seed random generator for tests that need it */
        for (int x = 0; x < 10000; x++) genrand64_int64();
    }
};

/* Test the random walk function. */
TEST_F(RaxTest, raxRandomWalk) {
    size_t used_memory_before = zmalloc_used_memory();

    rax *t = raxNew();
    const char *toadd[] = {"alligator", "alien", "byword",
                           "chromodynamic", "romane", "romanus",
                           "romulus", "rubens", "ruber",
                           "rubicon", "rubicundus", "all",
                           "rub", "by", nullptr};
    bool visited[14] = {false};

    long numele;
    for (numele = 0; toadd[numele] != nullptr; numele++) {
        raxInsert(t, (unsigned char *)toadd[numele], strlen(toadd[numele]),
                  (void *)numele, nullptr);
        EXPECT_EQ(raxAllocSize(t) + used_memory_before, zmalloc_used_memory());
    }

    raxIterator iter;
    raxStart(&iter, t);
    raxSeek(&iter, "^", nullptr, 0);
    int maxloops = 100000;
    while (raxRandomWalk(&iter, 0) && maxloops--) {
        int found = 0;
        for (long i = 0; i < numele; i++) {
            if (visited[i]) {
                found++;
                continue;
            }
            if (strlen(toadd[i]) == iter.key_len && memcmp(toadd[i], iter.key, iter.key_len) == 0) {
                visited[i] = true;
                found++;
            }
        }
        if (found == numele) break;
    }
    EXPECT_NE(maxloops, 0) << "randomWalkTest() is unable to report all the elements after 100k iterations!";
    raxStop(&iter);
    raxFree(t);
}

TEST_F(RaxTest, raxIteratorUnitTests) {
    size_t used_memory_before = zmalloc_used_memory();

    rax *t = raxNew();
    const char *toadd[] = {"alligator", "alien", "byword",
                           "chromodynamic", "romane", "romanus",
                           "romulus", "rubens", "ruber",
                           "rubicon", "rubicundus", "all",
                           "rub", "by", nullptr};

    long items = 0;
    while (toadd[items] != nullptr) items++;

    for (long i = 0; i < items; i++) {
        raxInsert(t, (unsigned char *)toadd[i], strlen(toadd[i]),
                  (void *)i, nullptr);
        EXPECT_EQ(raxAllocSize(t) + used_memory_before, zmalloc_used_memory());
    }

    raxIterator iter;
    raxStart(&iter, t);

    struct {
        const char *seek;
        size_t seeklen;
        const char *seekop;
        const char *expected;
    } tests[] = {/* Seek value. */ /* Expected result. */
                 {"rpxxx", 5, "<=", "romulus"},
                 {"rom", 3, ">=", "romane"},
                 {"rub", 3, ">=", "rub"},
                 {"rub", 3, ">", "rubens"},
                 {"rub", 3, "<", "romulus"},
                 {"rom", 3, ">", "romane"},
                 {"chro", 4, ">", "chromodynamic"},
                 {"chro", 4, "<", "byword"},
                 {"chromz", 6, "<", "chromodynamic"},
                 {"", 0, "^", "alien"},
                 {"zorro", 5, "<=", "rubicundus"},
                 {"zorro", 5, "<", "rubicundus"},
                 {"zorro", 5, "<", "rubicundus"},
                 {"", 0, "$", "rubicundus"},
                 {"ro", 2, ">=", "romane"},
                 {"zo", 2, ">", nullptr},
                 {"zo", 2, "==", nullptr},
                 {"romane", 6, "==", "romane"}};

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        raxSeek(&iter, tests[i].seekop, (unsigned char *)tests[i].seek, tests[i].seeklen);
        int retval = raxNext(&iter);

        if (tests[i].expected != nullptr) {
            if (strlen(tests[i].expected) != iter.key_len ||
                memcmp(tests[i].expected, iter.key, iter.key_len) != 0) {
                printf("Iterator unit test error: test %zu, %s expected, %.*s reported\n",
                       i, tests[i].expected, (int)iter.key_len, (char *)iter.key);
                FAIL();
            }
        } else {
            if (retval != 0) {
                printf("Iterator unit test error: EOF expected in test %zu\n", i);
                FAIL();
            }
        }
    }
    raxStop(&iter);
    raxFree(t);
}

/* Test that raxInsert() / raxTryInsert() overwrite semantic
 * works as expected. */
TEST_F(RaxTest, raxTryInsertUnitTests) {
    rax *t = raxNew();
    raxInsert(t, (unsigned char *)"FOO", 3, (void *)(long)1, nullptr);
    void *old, *val;
    raxTryInsert(t, (unsigned char *)"FOO", 3, (void *)(long)2, &old);
    EXPECT_EQ(old, (void *)(long)1) << "Old value not returned correctly by raxTryInsert()";

    val = nullptr;
    raxFind(t, (unsigned char *)"FOO", 3, &val);
    EXPECT_EQ(val, (void *)(long)1) << "FOO value mismatch: is " << val << " instead of 1";

    raxInsert(t, (unsigned char *)"FOO", 3, (void *)(long)2, nullptr);
    val = nullptr;
    raxFind(t, (unsigned char *)"FOO", 3, &val);
    EXPECT_EQ(val, (void *)(long)2) << "FOO value mismatch: is " << val << " instead of 2";

    raxFree(t);
}

/* Regression test #1: Iterator wrong element returned after seek. */
TEST_F(RaxTest, raxRegressionTest1) {
    rax *rax_tree = raxNew();
    raxInsert(rax_tree, (unsigned char *)"LIKE", 4, (void *)(long)1, nullptr);
    raxInsert(rax_tree, (unsigned char *)"TQ", 2, (void *)(long)2, nullptr);
    raxInsert(rax_tree, (unsigned char *)"B", 1, (void *)(long)3, nullptr);
    raxInsert(rax_tree, (unsigned char *)"FY", 2, (void *)(long)4, nullptr);
    raxInsert(rax_tree, (unsigned char *)"WI", 2, (void *)(long)5, nullptr);

    raxIterator iter;
    raxStart(&iter, rax_tree);
    raxSeek(&iter, ">", (unsigned char *)"FMP", 3);
    if (raxNext(&iter)) {
        char got[64] = {0};
        memcpy(got, iter.key, iter.key_len < 63 ? iter.key_len : 63);
        EXPECT_TRUE(iter.key_len == 2 && memcmp(iter.key, "FY", 2) == 0)
            << "Regression test 1 failed: 'FY' expected, got: '" << got << "'";
    }

    raxStop(&iter);
    raxFree(rax_tree);
}

/* Regression test #2: Crash when mixing NULL and not NULL values. */
TEST_F(RaxTest, raxRegressionTest2) {
    rax *rt = raxNew();
    raxInsert(rt, (unsigned char *)"a", 1, (void *)100, nullptr);
    raxInsert(rt, (unsigned char *)"ab", 2, (void *)101, nullptr);
    raxInsert(rt, (unsigned char *)"abc", 3, nullptr, nullptr);
    raxInsert(rt, (unsigned char *)"abcd", 4, nullptr, nullptr);
    raxInsert(rt, (unsigned char *)"abc", 3, (void *)102, nullptr);
    raxFree(rt);
}

/* Regression test #3: Wrong access at node value in raxRemoveChild()
 * when iskey == 1 and isnull == 1: the memmove() was performed including
 * the value length regardless of the fact there was no actual value.
 *
 * Note that this test always returns success but will trigger a
 * Valgrind error. */
TEST_F(RaxTest, raxRegressionTest3) {
    rax *rt = raxNew();
    raxInsert(rt, (unsigned char *)"D", 1, (void *)1, nullptr);
    raxInsert(rt, (unsigned char *)"", 0, nullptr, nullptr);
    raxRemove(rt, (unsigned char *)"D", 1, nullptr);
    raxFree(rt);
}

/* Regression test #4: Github issue #8, iterator does not populate the
 * data field after seek in case of exact match. The test case is looks odd
 * because it is quite indirect: Seeking "^" will result into seeking
 * the element >= "", and since we just added "" an exact match happens,
 * however we are using the original one from the bug report, since this
 * is quite odd and may later protect against different bugs related to
 * storing and fetching the empty string key. */
TEST_F(RaxTest, raxRegressionTest4) {
    rax *rt = raxNew();
    raxIterator iter;
    raxInsert(rt, (unsigned char *)"", 0, (void *)-1, nullptr);
    void *val = nullptr;
    raxFind(rt, (unsigned char *)"", 0, &val);
    EXPECT_EQ(val, (void *)-1) << "Regression test 4 failed. Key value mismatch in raxFind()";

    raxStart(&iter, rt);
    raxSeek(&iter, "^", nullptr, 0);
    raxNext(&iter);
    EXPECT_EQ(iter.data, (void *)-1) << "Regression test 4 failed. Key value mismatch in raxNext()";

    raxStop(&iter);
    raxFree(rt);
}

/* Less than seek bug when stopping in the middle of a compressed node. */
TEST_F(RaxTest, raxRegressionTest5) {
    rax *rax_tree = raxNew();

    raxInsert(rax_tree, (unsigned char *)"b", 1, (void *)(long)1, nullptr);
    raxInsert(rax_tree, (unsigned char *)"by", 2, (void *)(long)2, nullptr);
    raxInsert(rax_tree, (unsigned char *)"byword", 6, (void *)(long)3, nullptr);

    raxInsert(rax_tree, (unsigned char *)"f", 1, (void *)(long)4, nullptr);
    raxInsert(rax_tree, (unsigned char *)"foobar", 6, (void *)(long)5, nullptr);
    raxInsert(rax_tree, (unsigned char *)"foobar123", 9, (void *)(long)6, nullptr);

    raxIterator ri;
    raxStart(&ri, rax_tree);

    raxSeek(&ri, "<", (unsigned char *)"foo", 3);
    raxNext(&ri);
    EXPECT_TRUE(ri.key_len == 1 && ri.key[0] == 'f') << "Regression test 5 failed. Key value mismatch in raxNext()";

    raxStop(&ri);
    raxFree(rax_tree);
}

/* Seek may not populate iterator data. See issue #25. */
TEST_F(RaxTest, raxRegressionTest6) {
    rax *rax_tree = raxNew();

    const char *key1 = "172.17.141.2/adminguide/v5.0/";
    const char *key2 = "172.17.141.2/adminguide/v5.0/entitlements-configure.html";
    const char *seekpoint = "172.17.141.2/adminguide/v5.0/entitlements";

    raxInsert(rax_tree, (unsigned char *)key1, strlen(key1), (void *)(long)1234, nullptr);
    raxInsert(rax_tree, (unsigned char *)key2, strlen(key2), (void *)(long)5678, nullptr);

    raxIterator ri;
    raxStart(&ri, rax_tree);
    raxSeek(&ri, "<=", (unsigned char *)seekpoint, strlen(seekpoint));
    raxPrev(&ri);
    EXPECT_EQ((long)((intptr_t)ri.data), 1234L) << "Regression test 6 failed. Key data not populated.";

    raxStop(&ri);
    raxFree(rax_tree);
}

/* This is a benchmark test for rax performance.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=RaxTest.DISABLED_raxBenchmark --gtest_also_run_disabled_tests
 */
TEST_F(RaxTest, DISABLED_raxBenchmark) {
    size_t used_memory_before = zmalloc_used_memory();

    for (int mode = 0; mode < 2; mode++) {
        printf("Benchmark with %s keys:\n", (mode == 0) ? "integer" : "alphanumerical");
        rax *t = raxNew();
        long long start = ustime();
        for (int i = 0; i < 5000000; i++) {
            char buf[64];
            int len = int2key(buf, sizeof(buf), i, mode);
            raxInsert(t, (unsigned char *)buf, len, (void *)(long)i, nullptr);
            EXPECT_EQ(raxAllocSize(t) + used_memory_before, zmalloc_used_memory());
        }
        printf("Insert: %f\n", (double)(ustime() - start) / 1000000);
        printf("%llu total nodes\n", (unsigned long long)t->numnodes);
        printf("%llu total elements\n", (unsigned long long)t->numele);

        start = ustime();
        for (int i = 0; i < 5000000; i++) {
            char buf[64];
            int len = int2key(buf, sizeof(buf), i, mode);
            void *data;
            EXPECT_TRUE(raxFind(t, (unsigned char *)buf, len, &data) &&
                        data == (void *)(long)i)
                << "Issue with " << buf << ": " << data << " instead of " << (void *)(long)i;
        }
        printf("Linear lookup: %f\n", (double)(ustime() - start) / 1000000);

        start = ustime();
        for (int i = 0; i < 5000000; i++) {
            char buf[64];
            int r = genrand64_int64() % 5000000;
            int len = int2key(buf, sizeof(buf), r, mode);
            void *data;
            EXPECT_TRUE(raxFind(t, (unsigned char *)buf, len, &data) &&
                        data == (void *)(long)r)
                << "Issue with " << buf << ": " << data << " instead of " << (void *)(long)r;
        }
        printf("Random lookup: %f\n", (double)(ustime() - start) / 1000000);

        start = ustime();
        for (int i = 0; i < 5000000; i++) {
            char buf[64];
            int len = int2key(buf, sizeof(buf), i, mode);
            buf[i % len] = '!'; /* "!" is never set into keys. */
            EXPECT_FALSE(raxFind(t, (unsigned char *)buf, len, nullptr)) << "Lookup should have failed";
        }
        printf("Failed lookup: %f\n", (double)(ustime() - start) / 1000000);

        start = ustime();
        raxIterator ri;
        raxStart(&ri, t);
        raxSeek(&ri, "^", nullptr, 0);
        int iter = 0;
        while (raxNext(&ri)) iter++;
        EXPECT_EQ(iter, 5000000) << "Iteration is incomplete";
        raxStop(&ri);
        printf("Full iteration: %f\n", (double)(ustime() - start) / 1000000);

        start = ustime();
        for (int i = 0; i < 5000000; i++) {
            char buf[64];
            int len = int2key(buf, sizeof(buf), i, mode);
            int retval = raxRemove(t, (unsigned char *)buf, len, nullptr);
            EXPECT_EQ(retval, 1);
            EXPECT_EQ(raxAllocSize(t) + used_memory_before, zmalloc_used_memory());
        }
        printf("Deletion: %f\n", (double)(ustime() - start) / 1000000);

        printf("%llu total nodes\n", (unsigned long long)t->numnodes);
        printf("%llu total elements\n", (unsigned long long)t->numele);
        raxFree(t);
    }
}

/* Compressed nodes can only hold (2^29)-1 characters, so it is important
 * to test for keys bigger than this amount, in order to make sure that
 * the code to handle this edge case works as expected.
 *
 * This test is disabled by default because it uses a lot of memory. */
TEST_F(RaxTest, DISABLED_raxHugeKey) {
    if (!large_memory) GTEST_SKIP() << "Skipping large memory test";
    size_t max_keylen = ((1 << 29) - 1) + 100;
    unsigned char *key = (unsigned char *)zmalloc(max_keylen);
    if (key == nullptr) {
        fprintf(stderr, "Sorry, not enough memory to execute --hugekey test.");
        GTEST_SKIP();
    }

    memset(key, 'a', max_keylen);
    key[10] = 'X';
    key[max_keylen - 1] = 'Y';
    rax *rax_tree = raxNew();
    int retval = raxInsert(rax_tree, (unsigned char *)"aaabbb", 6, (void *)5678L, nullptr);
    if (retval == 0 && errno == ENOMEM) {
        fprintf(stderr, "Sorry, not enough memory to execute --hugekey test.");
        zfree(key);
        raxFree(rax_tree);
        GTEST_SKIP();
    }
    retval = raxInsert(rax_tree, key, max_keylen, (void *)1234L, nullptr);
    if (retval == 0 && errno == ENOMEM) {
        fprintf(stderr, "Sorry, not enough memory to execute --hugekey test.");
        zfree(key);
        raxFree(rax_tree);
        GTEST_SKIP();
    }
    void *value1, *value2;
    int found1 = raxFind(rax_tree, (unsigned char *)"aaabbb", 6, &value1);
    int found2 = raxFind(rax_tree, key, max_keylen, &value2);
    zfree(key);
    EXPECT_TRUE(found1 && found2) << "Huge key test failed on elementhood";
    EXPECT_TRUE(value1 == (void *)5678L && value2 == (void *)1234L)
        << "Huge key test failed";
    raxFree(rax_tree);
}

/* This is a fuzz test for rax data structure.
 * To run this test explicitly, use:
 *   ./src/unit/valkey-unit-gtests --gtest_filter=RaxTest.DISABLED_raxFuzz --gtest_also_run_disabled_tests
 */
TEST_F(RaxTest, DISABLED_raxFuzz) {
    if (!accurate) GTEST_SKIP() << "Skipping accurate test";

    int errors = 0;

    init_genrand64(1234);

    for (int i = 0; i < 10; i++) {
        double alpha = (double)genrand64_int64() / RAND_MAX;
        double beta = 1 - alpha;
        if (fuzzTestCluster(genrand64_int64() % 100000000, alpha, beta)) errors++;
    }

    for (int i = 0; i < 10; i++) {
        double alpha = (double)genrand64_int64() / RAND_MAX;
        double beta = 1 - alpha;
        if (fuzzTest(KEY_INT, genrand64_int64() % 10000, alpha, beta)) errors++;
        if (fuzzTest(KEY_UNIQUE_ALPHA, genrand64_int64() % 10000, alpha, beta)) errors++;
        if (fuzzTest(KEY_RANDOM, genrand64_int64() % 10000, alpha, beta)) errors++;
        if (fuzzTest(KEY_RANDOM_ALPHA, genrand64_int64() % 10000, alpha, beta)) errors++;
        if (fuzzTest(KEY_RANDOM_SMALL_CSET, genrand64_int64() % 10000, alpha, beta)) errors++;
    }

    size_t numops = 100000, cycles = 3;
    while (cycles--) {
        if (fuzzTest(KEY_INT, numops, .7, .3)) errors++;
        if (fuzzTest(KEY_UNIQUE_ALPHA, numops, .7, .3)) errors++;
        if (fuzzTest(KEY_RANDOM, numops, .7, .3)) errors++;
        if (fuzzTest(KEY_RANDOM_ALPHA, numops, .7, .3)) errors++;
        if (fuzzTest(KEY_RANDOM_SMALL_CSET, numops, .7, .3)) errors++;
        numops *= 10;
    }

    if (fuzzTest(KEY_CHAIN, 1000, .7, .3)) errors++;
    printf("Iterator fuzz test: ");
    fflush(stdout);
    for (int i = 0; i < 100000; i++) {
        if (iteratorFuzzTest(KEY_INT, 100)) errors++;
        if (iteratorFuzzTest(KEY_UNIQUE_ALPHA, 100)) errors++;
        if (iteratorFuzzTest(KEY_RANDOM_ALPHA, 1000)) errors++;
        if (iteratorFuzzTest(KEY_RANDOM, 1000)) errors++;
        if (i && !(i % 100)) {
            printf(".");
            if (!(i % 1000)) {
                printf("%d%% done", i / 1000);
            }
            fflush(stdout);
        }
    }
    printf("\n");

    if (errors) {
        printf("!!! WARNING !!!: %d errors found\n", errors);
    } else {
        printf("OK! \\o/\n");
    }
    EXPECT_EQ(errors, 0);
}

/* This test verifies that raxRemove correctly handles compression when two keys
 * share a common prefix. Upon deletion of one key, rax attempts to recompress
 * the structure back to its original form for other key. Historically, there was
 * a crash when deleting one key because rax would attempt to recompress the
 * structure without checking the 512MB size limit.
 *
 * This test is disabled by default because it uses a lot of memory. */
TEST_F(RaxTest, DISABLED_raxRecompressHugeKey) {
    if (!large_memory) GTEST_SKIP() << "Skipping large memory test";
    rax *rt = raxNew();

    /* Insert small keys */
    char small_key[32];
    const char *small_prefix = ",({5oM}";
    int i;
    for (i = 1; i <= 20; i++) {
        snprintf(small_key, sizeof(small_key), "%s%d", small_prefix, i);
        size_t keylen = strlen(small_key);
        raxInsert(rt, (unsigned char *)small_key, keylen,
                  (void *)(long)i, nullptr);
    }

    /* Insert large key exceeding compressed node size limit */
    size_t max_keylen = ((1 << 29) - 1) + 100; // Compressed node limit + overflow
    const char *large_prefix = ",({ABC}";
    unsigned char *large_key = (unsigned char *)zmalloc(max_keylen + strlen(large_prefix));
    ASSERT_NE(large_key, nullptr) << "Failed to allocate memory for large key";

    memcpy(large_key, large_prefix, strlen(large_prefix));
    memset(large_key + strlen(large_prefix), '1', max_keylen);
    raxInsert(rt, large_key, max_keylen + strlen(large_prefix), nullptr, nullptr);

    /* Remove small keys to trigger recompression crash in raxRemove() */
    for (i = 20; i >= 1; i--) {
        snprintf(small_key, sizeof(small_key), "%s%d", small_prefix, i);
        size_t keylen = strlen(small_key);
        raxRemove(rt, (unsigned char *)small_key, keylen, nullptr);
    }

    zfree(large_key);
    raxFree(rt);
}
