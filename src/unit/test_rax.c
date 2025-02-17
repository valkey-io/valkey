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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>

#include "../rax.c"
#include "../mt19937-64.c"
#include "test_help.h"

/* This test verifies that raxRemove correctly handles compression when two keys
 * share a common prefix. Upon deletion of one key, rax attempts to recompress
 * the structure back to its original form for other key. Historically, there was
 * a crash when deleting one key because rax would attempt to recompress the
 * structure without checking the 512MB size limit.
 *
 * This test is disabled by default because it uses a lot of memory. */
int test_raxRecompressHugeKey(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);

    if (!(flags & UNIT_TEST_LARGE_MEMORY)) return 0;

    rax *rt = raxNew();

    /* Insert small keys */
    char small_key[32];
    const char *small_prefix = ",({5oM}";
    int i;
    for (i = 1; i <= 20; i++) {
        snprintf(small_key, sizeof(small_key), "%s%d", small_prefix, i);
        size_t keylen = strlen(small_key);
        raxInsert(rt, (unsigned char *)small_key, keylen, (void *)(long)i, NULL);
    }

    /* Insert large key exceeding compressed node size limit */
    size_t max_keylen = ((1 << 29) - 1) + 100; // Compressed node limit + overflow
    const char *large_prefix = ",({ABC}";
    unsigned char *large_key = zmalloc(max_keylen + strlen(large_prefix));
    if (!large_key) {
        fprintf(stderr, "Failed to allocate memory for large key\n");
        raxFree(rt);
        return 1;
    }

    memcpy(large_key, large_prefix, strlen(large_prefix));
    memset(large_key + strlen(large_prefix), '1', max_keylen);
    raxInsert(rt, large_key, max_keylen + strlen(large_prefix), NULL, NULL);

    /* Remove small keys to trigger recompression crash in raxRemove() */
    for (i = 20; i >= 1; i--) {
        snprintf(small_key, sizeof(small_key), "%s%d", small_prefix, i);
        size_t keylen = strlen(small_key);
        raxRemove(rt, (unsigned char *)small_key, keylen, NULL);
    }

    zfree(large_key);
    raxFree(rt);
    return 0;
}
