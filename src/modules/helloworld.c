/* Helloworld module -- A few examples of the Modules API in the form
 * of commands showing how to accomplish common tasks.
 *
 * This module does not do anything useful, if not for a few commands. The
 * examples are designed in order to show the API.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2016, Redis Ltd.
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

#include "../valkeymodule.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

/* HELLO.SIMPLE is among the simplest commands you can implement.
 * It just returns the currently selected DB id, a functionality which is
 * missing in the server. The command uses two important API calls: one to
 * fetch the currently selected DB, the other in order to send the client
 * an integer reply as response. */
int HelloSimple_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModule_ReplyWithLongLong(ctx, ValkeyModule_GetSelectedDb(ctx));
    return VALKEYMODULE_OK;
}

/* HELLO.PUSH.NATIVE re-implements RPUSH, and shows the low level modules API
 * where you can "open" keys, make low level operations, create new keys by
 * pushing elements into non-existing keys, and so forth.
 *
 * You'll find this command to be roughly as fast as the actual RPUSH
 * command. */
int HelloPushNative_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);

    ValkeyModule_ListPush(key, VALKEYMODULE_LIST_TAIL, argv[2]);
    size_t newlen = ValkeyModule_ValueLength(key);
    ValkeyModule_CloseKey(key);
    ValkeyModule_ReplyWithLongLong(ctx, newlen);
    return VALKEYMODULE_OK;
}

/* HELLO.PUSH.CALL implements RPUSH using an higher level approach, calling
 * a command instead of working with the key in a low level way. This
 * approach is useful when you need to call commands that are not
 * available as low level APIs, or when you don't need the maximum speed
 * possible but instead prefer implementation simplicity. */
int HelloPushCall_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);

    ValkeyModuleCallReply *reply;

    reply = ValkeyModule_Call(ctx, "RPUSH", "ss", argv[1], argv[2]);
    long long len = ValkeyModule_CallReplyInteger(reply);
    ValkeyModule_FreeCallReply(reply);
    ValkeyModule_ReplyWithLongLong(ctx, len);
    return VALKEYMODULE_OK;
}

/* HELLO.PUSH.CALL2
 * This is exactly as HELLO.PUSH.CALL, but shows how we can reply to the
 * client using directly a reply object that Call() returned. */
int HelloPushCall2_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 3) return ValkeyModule_WrongArity(ctx);

    ValkeyModuleCallReply *reply;

    reply = ValkeyModule_Call(ctx, "RPUSH", "ss", argv[1], argv[2]);
    ValkeyModule_ReplyWithCallReply(ctx, reply);
    ValkeyModule_FreeCallReply(reply);
    return VALKEYMODULE_OK;
}

/* HELLO.LIST.SUM.LEN returns the total length of all the items inside
 * a list, by using the high level Call() API.
 * This command is an example of the array reply access. */
int HelloListSumLen_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    ValkeyModuleCallReply *reply;

    reply = ValkeyModule_Call(ctx, "LRANGE", "sll", argv[1], (long long)0, (long long)-1);
    size_t strlen = 0;
    size_t items = ValkeyModule_CallReplyLength(reply);
    size_t j;
    for (j = 0; j < items; j++) {
        ValkeyModuleCallReply *ele = ValkeyModule_CallReplyArrayElement(reply, j);
        strlen += ValkeyModule_CallReplyLength(ele);
    }
    ValkeyModule_FreeCallReply(reply);
    ValkeyModule_ReplyWithLongLong(ctx, strlen);
    return VALKEYMODULE_OK;
}

/* HELLO.LIST.SPLICE srclist dstlist count
 * Moves 'count' elements from the tail of 'srclist' to the head of
 * 'dstlist'. If less than count elements are available, it moves as much
 * elements as possible. */
int HelloListSplice_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);

    ValkeyModuleKey *srckey = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    ValkeyModuleKey *dstkey = ValkeyModule_OpenKey(ctx, argv[2], VALKEYMODULE_READ | VALKEYMODULE_WRITE);

    /* Src and dst key must be empty or lists. */
    if ((ValkeyModule_KeyType(srckey) != VALKEYMODULE_KEYTYPE_LIST &&
         ValkeyModule_KeyType(srckey) != VALKEYMODULE_KEYTYPE_EMPTY) ||
        (ValkeyModule_KeyType(dstkey) != VALKEYMODULE_KEYTYPE_LIST &&
         ValkeyModule_KeyType(dstkey) != VALKEYMODULE_KEYTYPE_EMPTY)) {
        ValkeyModule_CloseKey(srckey);
        ValkeyModule_CloseKey(dstkey);
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }

    long long count;
    if ((ValkeyModule_StringToLongLong(argv[3], &count) != VALKEYMODULE_OK) || (count < 0)) {
        ValkeyModule_CloseKey(srckey);
        ValkeyModule_CloseKey(dstkey);
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid count");
    }

    while (count-- > 0) {
        ValkeyModuleString *ele;

        ele = ValkeyModule_ListPop(srckey, VALKEYMODULE_LIST_TAIL);
        if (ele == NULL) break;
        ValkeyModule_ListPush(dstkey, VALKEYMODULE_LIST_HEAD, ele);
        ValkeyModule_FreeString(ctx, ele);
    }

    size_t len = ValkeyModule_ValueLength(srckey);
    ValkeyModule_CloseKey(srckey);
    ValkeyModule_CloseKey(dstkey);
    ValkeyModule_ReplyWithLongLong(ctx, len);
    return VALKEYMODULE_OK;
}

/* Like the HELLO.LIST.SPLICE above, but uses automatic memory management
 * in order to avoid freeing stuff. */
int HelloListSpliceAuto_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 4) return ValkeyModule_WrongArity(ctx);

    ValkeyModule_AutoMemory(ctx);

    ValkeyModuleKey *srckey = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    ValkeyModuleKey *dstkey = ValkeyModule_OpenKey(ctx, argv[2], VALKEYMODULE_READ | VALKEYMODULE_WRITE);

    /* Src and dst key must be empty or lists. */
    if ((ValkeyModule_KeyType(srckey) != VALKEYMODULE_KEYTYPE_LIST &&
         ValkeyModule_KeyType(srckey) != VALKEYMODULE_KEYTYPE_EMPTY) ||
        (ValkeyModule_KeyType(dstkey) != VALKEYMODULE_KEYTYPE_LIST &&
         ValkeyModule_KeyType(dstkey) != VALKEYMODULE_KEYTYPE_EMPTY)) {
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }

    long long count;
    if ((ValkeyModule_StringToLongLong(argv[3], &count) != VALKEYMODULE_OK) || (count < 0)) {
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid count");
    }

    while (count-- > 0) {
        ValkeyModuleString *ele;

        ele = ValkeyModule_ListPop(srckey, VALKEYMODULE_LIST_TAIL);
        if (ele == NULL) break;
        ValkeyModule_ListPush(dstkey, VALKEYMODULE_LIST_HEAD, ele);
    }

    size_t len = ValkeyModule_ValueLength(srckey);
    ValkeyModule_ReplyWithLongLong(ctx, len);
    return VALKEYMODULE_OK;
}

/* HELLO.RAND.ARRAY <count>
 * Shows how to generate arrays as commands replies.
 * It just outputs <count> random numbers. */
int HelloRandArray_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    long long count;
    if (ValkeyModule_StringToLongLong(argv[1], &count) != VALKEYMODULE_OK || count < 0)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid count");

    /* To reply with an array, we call ValkeyModule_ReplyWithArray() followed
     * by other "count" calls to other reply functions in order to generate
     * the elements of the array. */
    ValkeyModule_ReplyWithArray(ctx, count);
    while (count--) ValkeyModule_ReplyWithLongLong(ctx, rand());
    return VALKEYMODULE_OK;
}

/* This is a simple command to test replication. Because of the "!" modified
 * in the ValkeyModule_Call() call, the two INCRs get replicated.
 * Also note how the ECHO is replicated in an unexpected position (check
 * comments the function implementation). */
int HelloRepl1_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    ValkeyModule_AutoMemory(ctx);

    /* This will be replicated *after* the two INCR statements, since
     * the Call() replication has precedence, so the actual replication
     * stream will be:
     *
     * MULTI
     * INCR foo
     * INCR bar
     * ECHO c foo
     * EXEC
     */
    ValkeyModule_Replicate(ctx, "ECHO", "c", "foo");

    /* Using the "!" modifier we replicate the command if it
     * modified the dataset in some way. */
    ValkeyModule_Call(ctx, "INCR", "c!", "foo");
    ValkeyModule_Call(ctx, "INCR", "c!", "bar");

    ValkeyModule_ReplyWithLongLong(ctx, 0);

    return VALKEYMODULE_OK;
}

/* Another command to show replication. In this case, we call
 * ValkeyModule_ReplicateVerbatim() to mean we want just the command to be
 * propagated to replicas / AOF exactly as it was called by the user.
 *
 * This command also shows how to work with string objects.
 * It takes a list, and increments all the elements (that must have
 * a numerical value) by 1, returning the sum of all the elements
 * as reply.
 *
 * Usage: HELLO.REPL2 <list-key> */
int HelloRepl2_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    ValkeyModule_AutoMemory(ctx); /* Use automatic memory management. */
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);

    if (ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_LIST)
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);

    size_t listlen = ValkeyModule_ValueLength(key);
    long long sum = 0;

    /* Rotate and increment. */
    while (listlen--) {
        ValkeyModuleString *ele = ValkeyModule_ListPop(key, VALKEYMODULE_LIST_TAIL);
        long long val;
        if (ValkeyModule_StringToLongLong(ele, &val) != VALKEYMODULE_OK) val = 0;
        val++;
        sum += val;
        ValkeyModuleString *newele = ValkeyModule_CreateStringFromLongLong(ctx, val);
        ValkeyModule_ListPush(key, VALKEYMODULE_LIST_HEAD, newele);
    }
    ValkeyModule_ReplyWithLongLong(ctx, sum);
    ValkeyModule_ReplicateVerbatim(ctx);
    return VALKEYMODULE_OK;
}

/* This is an example of strings DMA access. Given a key containing a string
 * it toggles the case of each character from lower to upper case or the
 * other way around.
 *
 * No automatic memory management is used in this example (for the sake
 * of variety).
 *
 * HELLO.TOGGLE.CASE key */
int HelloToggleCase_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);

    int keytype = ValkeyModule_KeyType(key);
    if (keytype != VALKEYMODULE_KEYTYPE_STRING && keytype != VALKEYMODULE_KEYTYPE_EMPTY) {
        ValkeyModule_CloseKey(key);
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }

    if (keytype == VALKEYMODULE_KEYTYPE_STRING) {
        size_t len, j;
        char *s = ValkeyModule_StringDMA(key, &len, VALKEYMODULE_WRITE);
        for (j = 0; j < len; j++) {
            if (isupper(s[j])) {
                s[j] = tolower(s[j]);
            } else {
                s[j] = toupper(s[j]);
            }
        }
    }

    ValkeyModule_CloseKey(key);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    ValkeyModule_ReplicateVerbatim(ctx);
    return VALKEYMODULE_OK;
}

/* HELLO.MORE.EXPIRE key milliseconds.
 *
 * If the key has already an associated TTL, extends it by "milliseconds"
 * milliseconds. Otherwise no operation is performed. */
int HelloMoreExpire_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    ValkeyModule_AutoMemory(ctx); /* Use automatic memory management. */
    if (argc != 3) return ValkeyModule_WrongArity(ctx);

    mstime_t addms, expire;

    if (ValkeyModule_StringToLongLong(argv[2], &addms) != VALKEYMODULE_OK)
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid expire time");

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    expire = ValkeyModule_GetExpire(key);
    if (expire != VALKEYMODULE_NO_EXPIRE) {
        expire += addms;
        ValkeyModule_SetExpire(key, expire);
    }
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* HELLO.ZSUMRANGE key startscore endscore
 * Return the sum of all the scores elements between startscore and endscore.
 *
 * The computation is performed two times, one time from start to end and
 * another time backward. The two scores, returned as a two element array,
 * should match.*/
int HelloZsumRange_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    double score_start, score_end;
    if (argc != 4) return ValkeyModule_WrongArity(ctx);

    if (ValkeyModule_StringToDouble(argv[2], &score_start) != VALKEYMODULE_OK ||
        ValkeyModule_StringToDouble(argv[3], &score_end) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid range");
    }

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    if (ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_ZSET) {
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }

    double scoresum_a = 0;
    double scoresum_b = 0;

    ValkeyModule_ZsetFirstInScoreRange(key, score_start, score_end, 0, 0);
    while (!ValkeyModule_ZsetRangeEndReached(key)) {
        double score;
        ValkeyModuleString *ele = ValkeyModule_ZsetRangeCurrentElement(key, &score);
        ValkeyModule_FreeString(ctx, ele);
        scoresum_a += score;
        ValkeyModule_ZsetRangeNext(key);
    }
    ValkeyModule_ZsetRangeStop(key);

    ValkeyModule_ZsetLastInScoreRange(key, score_start, score_end, 0, 0);
    while (!ValkeyModule_ZsetRangeEndReached(key)) {
        double score;
        ValkeyModuleString *ele = ValkeyModule_ZsetRangeCurrentElement(key, &score);
        ValkeyModule_FreeString(ctx, ele);
        scoresum_b += score;
        ValkeyModule_ZsetRangePrev(key);
    }

    ValkeyModule_ZsetRangeStop(key);

    ValkeyModule_CloseKey(key);

    ValkeyModule_ReplyWithArray(ctx, 2);
    ValkeyModule_ReplyWithDouble(ctx, scoresum_a);
    ValkeyModule_ReplyWithDouble(ctx, scoresum_b);
    return VALKEYMODULE_OK;
}

/* HELLO.LEXRANGE key min_lex max_lex min_age max_age
 * This command expects a sorted set stored at key in the following form:
 * - All the elements have score 0.
 * - Elements are pairs of "<name>:<age>", for example "Anna:52".
 * The command will return all the sorted set items that are lexicographically
 * between the specified range (using the same format as ZRANGEBYLEX)
 * and having an age between min_age and max_age. */
int HelloLexRange_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    ValkeyModule_AutoMemory(ctx); /* Use automatic memory management. */

    if (argc != 6) return ValkeyModule_WrongArity(ctx);

    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    if (ValkeyModule_KeyType(key) != VALKEYMODULE_KEYTYPE_ZSET) {
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }

    if (ValkeyModule_ZsetFirstInLexRange(key, argv[2], argv[3]) != VALKEYMODULE_OK) {
        return ValkeyModule_ReplyWithError(ctx, "invalid range");
    }

    int arraylen = 0;
    ValkeyModule_ReplyWithArray(ctx, VALKEYMODULE_POSTPONED_LEN);
    while (!ValkeyModule_ZsetRangeEndReached(key)) {
        double score;
        ValkeyModuleString *ele = ValkeyModule_ZsetRangeCurrentElement(key, &score);
        ValkeyModule_ReplyWithString(ctx, ele);
        ValkeyModule_FreeString(ctx, ele);
        ValkeyModule_ZsetRangeNext(key);
        arraylen++;
    }
    ValkeyModule_ZsetRangeStop(key);
    ValkeyModule_ReplySetArrayLength(ctx, arraylen);
    ValkeyModule_CloseKey(key);
    return VALKEYMODULE_OK;
}

/* HELLO.HCOPY key srcfield dstfield
 * This is just an example command that sets the hash field dstfield to the
 * same value of srcfield. If srcfield does not exist no operation is
 * performed.
 *
 * The command returns 1 if the copy is performed (srcfield exists) otherwise
 * 0 is returned. */
int HelloHCopy_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    ValkeyModule_AutoMemory(ctx); /* Use automatic memory management. */

    if (argc != 4) return ValkeyModule_WrongArity(ctx);
    ValkeyModuleKey *key = ValkeyModule_OpenKey(ctx, argv[1], VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    int type = ValkeyModule_KeyType(key);
    if (type != VALKEYMODULE_KEYTYPE_HASH && type != VALKEYMODULE_KEYTYPE_EMPTY) {
        return ValkeyModule_ReplyWithError(ctx, VALKEYMODULE_ERRORMSG_WRONGTYPE);
    }

    /* Get the old field value. */
    ValkeyModuleString *oldval;
    ValkeyModule_HashGet(key, VALKEYMODULE_HASH_NONE, argv[2], &oldval, NULL);
    if (oldval) {
        ValkeyModule_HashSet(key, VALKEYMODULE_HASH_NONE, argv[3], oldval, NULL);
    }
    ValkeyModule_ReplyWithLongLong(ctx, oldval != NULL);
    return VALKEYMODULE_OK;
}

/* HELLO.LEFTPAD str len ch
 * This is an implementation of the infamous LEFTPAD function, that
 * was at the center of an issue with the npm modules system in March 2016.
 *
 * LEFTPAD is a good example of using a Modules API called
 * "pool allocator", that was a famous way to allocate memory in yet another
 * open source project, the Apache web server.
 *
 * The concept is very simple: there is memory that is useful to allocate
 * only in the context of serving a request, and must be freed anyway when
 * the callback implementing the command returns. So in that case the module
 * does not need to retain a reference to these allocations, it is just
 * required to free the memory before returning. When this is the case the
 * module can call ValkeyModule_PoolAlloc() instead, that works like malloc()
 * but will automatically free the memory when the module callback returns.
 *
 * Note that PoolAlloc() does not necessarily require AutoMemory to be
 * active. */
int HelloLeftPad_ValkeyCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    ValkeyModule_AutoMemory(ctx); /* Use automatic memory management. */
    long long padlen;

    if (argc != 4) return ValkeyModule_WrongArity(ctx);

    if ((ValkeyModule_StringToLongLong(argv[2], &padlen) != VALKEYMODULE_OK) || (padlen < 0)) {
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid padding length");
    }
    size_t strlen, chlen;
    const char *str = ValkeyModule_StringPtrLen(argv[1], &strlen);
    const char *ch = ValkeyModule_StringPtrLen(argv[3], &chlen);

    /* If the string is already larger than the target len, just return
     * the string itself. */
    if (strlen >= (size_t)padlen) return ValkeyModule_ReplyWithString(ctx, argv[1]);

    /* Padding must be a single character in this simple implementation. */
    if (chlen != 1) return ValkeyModule_ReplyWithError(ctx, "ERR padding must be a single char");

    /* Here we use our pool allocator, for our throw-away allocation. */
    padlen -= strlen;
    char *buf = ValkeyModule_PoolAlloc(ctx, padlen + strlen);
    for (long long j = 0; j < padlen; j++) buf[j] = *ch;
    memcpy(buf + padlen, str, strlen);

    ValkeyModule_ReplyWithStringBuffer(ctx, buf, padlen + strlen);
    return VALKEYMODULE_OK;
}

/* This function must be present on each module. It is used in order to
 * register the commands into the server. */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (ValkeyModule_Init(ctx, "helloworld", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    /* Log the list of parameters passing loading the module. */
    for (int j = 0; j < argc; j++) {
        const char *s = ValkeyModule_StringPtrLen(argv[j], NULL);
        printf("Module loaded with ARGV[%d] = %s\n", j, s);
    }

    if (ValkeyModule_CreateCommand(ctx, "hello.simple", HelloSimple_ValkeyCommand, "readonly", 0, 0, 0) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.push.native", HelloPushNative_ValkeyCommand, "write deny-oom", 1, 1,
                                   1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.push.call", HelloPushCall_ValkeyCommand, "write deny-oom", 1, 1, 1) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.push.call2", HelloPushCall2_ValkeyCommand, "write deny-oom", 1, 1, 1) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.list.sum.len", HelloListSumLen_ValkeyCommand, "readonly", 1, 1, 1) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.list.splice", HelloListSplice_ValkeyCommand, "write deny-oom", 1, 2,
                                   1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.list.splice.auto", HelloListSpliceAuto_ValkeyCommand, "write deny-oom",
                                   1, 2, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.rand.array", HelloRandArray_ValkeyCommand, "readonly", 0, 0, 0) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.repl1", HelloRepl1_ValkeyCommand, "write", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.repl2", HelloRepl2_ValkeyCommand, "write", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.toggle.case", HelloToggleCase_ValkeyCommand, "write", 1, 1, 1) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.more.expire", HelloMoreExpire_ValkeyCommand, "write", 1, 1, 1) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.zsumrange", HelloZsumRange_ValkeyCommand, "readonly", 1, 1, 1) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.lexrange", HelloLexRange_ValkeyCommand, "readonly", 1, 1, 1) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.hcopy", HelloHCopy_ValkeyCommand, "write deny-oom", 1, 1, 1) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "hello.leftpad", HelloLeftPad_ValkeyCommand, "", 1, 1, 1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
