/* This module is used to test the server keyspace events API.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2020, Meir Shpilraien <meir at redislabs dot com>
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

#define _BSD_SOURCE
#define _DEFAULT_SOURCE /* For usleep */

#include "valkeymodule.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

ustime_t cached_time = 0;

/** stores all the keys on which we got 'loaded' keyspace notification **/
ValkeyModuleDict *loaded_event_log = NULL;
/** stores all the keys on which we got 'module' keyspace notification **/
ValkeyModuleDict *module_event_log = NULL;

/** Counts how many deleted KSN we got on keys with a prefix of "count_dels_" **/
static size_t dels = 0;

static int KeySpace_NotificationLoaded(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key){
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(type);

    if(strcmp(event, "loaded") == 0){
        const char* keyName = ValkeyModule_StringPtrLen(key, NULL);
        int nokey;
        ValkeyModule_DictGetC(loaded_event_log, (void*)keyName, strlen(keyName), &nokey);
        if(nokey){
            ValkeyModule_DictSetC(loaded_event_log, (void*)keyName, strlen(keyName), ValkeyModule_HoldString(ctx, key));
        }
    }

    return VALKEYMODULE_OK;
}

static int KeySpace_NotificationGeneric(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(type);
    const char *key_str = ValkeyModule_StringPtrLen(key, NULL);
    if (strncmp(key_str, "count_dels_", 11) == 0 && strcmp(event, "del") == 0) {
        if (ValkeyModule_GetContextFlags(ctx) & VALKEYMODULE_CTX_FLAGS_PRIMARY) {
            dels++;
            ValkeyModule_Replicate(ctx, "keyspace.incr_dels", "");
        }
        return VALKEYMODULE_OK;
    }
    if (cached_time) {
        ValkeyModule_Assert(cached_time == ValkeyModule_CachedMicroseconds());
        usleep(1);
        ValkeyModule_Assert(cached_time != ValkeyModule_Microseconds());
    }

    if (strcmp(event, "del") == 0) {
        ValkeyModuleString *copykey = ValkeyModule_CreateStringPrintf(ctx, "%s_copy", ValkeyModule_StringPtrLen(key, NULL));
        ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "DEL", "s!", copykey);
        ValkeyModule_FreeString(ctx, copykey);
        ValkeyModule_FreeCallReply(rep);

        int ctx_flags = ValkeyModule_GetContextFlags(ctx);
        if (ctx_flags & VALKEYMODULE_CTX_FLAGS_LUA) {
            ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "INCR", "c", "lua");
            ValkeyModule_FreeCallReply(rep);
        }
        if (ctx_flags & VALKEYMODULE_CTX_FLAGS_MULTI) {
            ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "INCR", "c", "multi");
            ValkeyModule_FreeCallReply(rep);
        }
    }

    return VALKEYMODULE_OK;
}

static int KeySpace_NotificationExpired(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "INCR", "c!", "testkeyspace:expired");
    ValkeyModule_FreeCallReply(rep);

    return VALKEYMODULE_OK;
}

/* This key miss notification handler is performing a write command inside the notification callback.
 * Notice, it is discourage and currently wrong to perform a write command inside key miss event.
 * It can cause read commands to be replicated to the replica/aof. This test is here temporary (for coverage and
 * verification that it's not crashing). */
static int KeySpace_NotificationModuleKeyMiss(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);

    int flags = ValkeyModule_GetContextFlags(ctx);
    if (!(flags & VALKEYMODULE_CTX_FLAGS_PRIMARY)) {
        return VALKEYMODULE_OK; // ignore the event on replica
    }

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "incr", "!c", "missed");
    ValkeyModule_FreeCallReply(rep);

    return VALKEYMODULE_OK;
}

static int KeySpace_NotificationModuleString(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    ValkeyModuleKey *valkey_key = ValkeyModule_OpenKey(ctx, key, VALKEYMODULE_READ);

    size_t len = 0;
    /* ValkeyModule_StringDMA could change the data format and cause the old robj to be freed.
     * This code verifies that such format change will not cause any crashes.*/
    char *data = ValkeyModule_StringDMA(valkey_key, &len, VALKEYMODULE_READ);
    int res = strncmp(data, "dummy", 5);
    VALKEYMODULE_NOT_USED(res);

    ValkeyModule_CloseKey(valkey_key);

    return VALKEYMODULE_OK;
}

static void KeySpace_PostNotificationStringFreePD(void *pd) {
    ValkeyModule_FreeString(NULL, pd);
}

static void KeySpace_PostNotificationString(ValkeyModuleCtx *ctx, void *pd) {
    VALKEYMODULE_NOT_USED(ctx);
    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "incr", "!s", pd);
    ValkeyModule_FreeCallReply(rep);
}

static int KeySpace_NotificationModuleStringPostNotificationJob(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);

    const char *key_str = ValkeyModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "string1_", 8) != 0) {
        return VALKEYMODULE_OK;
    }

    ValkeyModuleString *new_key = ValkeyModule_CreateStringPrintf(NULL, "string_changed{%s}", key_str);
    ValkeyModule_AddPostNotificationJob(ctx, KeySpace_PostNotificationString, new_key, KeySpace_PostNotificationStringFreePD);
    return VALKEYMODULE_OK;
}

static int KeySpace_NotificationModule(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);

    const char* keyName = ValkeyModule_StringPtrLen(key, NULL);
    int nokey;
    ValkeyModule_DictGetC(module_event_log, (void*)keyName, strlen(keyName), &nokey);
    if(nokey){
        ValkeyModule_DictSetC(module_event_log, (void*)keyName, strlen(keyName), ValkeyModule_HoldString(ctx, key));
    }
    return VALKEYMODULE_OK;
}

static int cmdNotify(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    if(argc != 2){
        return ValkeyModule_WrongArity(ctx);
    }

    ValkeyModule_NotifyKeyspaceEvent(ctx, VALKEYMODULE_NOTIFY_MODULE, "notify", argv[1]);
    ValkeyModule_ReplyWithNull(ctx);
    return VALKEYMODULE_OK;
}

static int cmdIsModuleKeyNotified(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    if(argc != 2){
        return ValkeyModule_WrongArity(ctx);
    }

    const char* key  = ValkeyModule_StringPtrLen(argv[1], NULL);

    int nokey;
    ValkeyModuleString* keyStr = ValkeyModule_DictGetC(module_event_log, (void*)key, strlen(key), &nokey);

    ValkeyModule_ReplyWithArray(ctx, 2);
    ValkeyModule_ReplyWithLongLong(ctx, !nokey);
    if(nokey){
        ValkeyModule_ReplyWithNull(ctx);
    }else{
        ValkeyModule_ReplyWithString(ctx, keyStr);
    }
    return VALKEYMODULE_OK;
}

static int cmdIsKeyLoaded(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    if(argc != 2){
        return ValkeyModule_WrongArity(ctx);
    }

    const char* key  = ValkeyModule_StringPtrLen(argv[1], NULL);

    int nokey;
    ValkeyModuleString* keyStr = ValkeyModule_DictGetC(loaded_event_log, (void*)key, strlen(key), &nokey);

    ValkeyModule_ReplyWithArray(ctx, 2);
    ValkeyModule_ReplyWithLongLong(ctx, !nokey);
    if(nokey){
        ValkeyModule_ReplyWithNull(ctx);
    }else{
        ValkeyModule_ReplyWithString(ctx, keyStr);
    }
    return VALKEYMODULE_OK;
}

static int cmdDelKeyCopy(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2)
        return ValkeyModule_WrongArity(ctx);

    cached_time = ValkeyModule_CachedMicroseconds();

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "DEL", "s!", argv[1]);
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }
    cached_time = 0;
    return VALKEYMODULE_OK;
}

/* Call INCR and propagate using RM_Call with `!`. */
static int cmdIncrCase1(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2)
        return ValkeyModule_WrongArity(ctx);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "INCR", "s!", argv[1]);
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }
    return VALKEYMODULE_OK;
}

/* Call INCR and propagate using RM_Replicate. */
static int cmdIncrCase2(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2)
        return ValkeyModule_WrongArity(ctx);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "INCR", "s", argv[1]);
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }
    ValkeyModule_Replicate(ctx, "INCR", "s", argv[1]);
    return VALKEYMODULE_OK;
}

/* Call INCR and propagate using RM_ReplicateVerbatim. */
static int cmdIncrCase3(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2)
        return ValkeyModule_WrongArity(ctx);

    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "INCR", "s", argv[1]);
    if (!rep) {
        ValkeyModule_ReplyWithError(ctx, "NULL reply returned");
    } else {
        ValkeyModule_ReplyWithCallReply(ctx, rep);
        ValkeyModule_FreeCallReply(rep);
    }
    ValkeyModule_ReplicateVerbatim(ctx);
    return VALKEYMODULE_OK;
}

static int cmdIncrDels(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    dels++;
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

static int cmdGetDels(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    return ValkeyModule_ReplyWithLongLong(ctx, dels);
}

/* This function must be present on each module. It is used in order to
 * register the commands into the server. */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (ValkeyModule_Init(ctx,"testkeyspace",1,VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }

    loaded_event_log = ValkeyModule_CreateDict(ctx);
    module_event_log = ValkeyModule_CreateDict(ctx);

    int keySpaceAll = ValkeyModule_GetKeyspaceNotificationFlagsAll();

    if (!(keySpaceAll & VALKEYMODULE_NOTIFY_LOADED)) {
        // VALKEYMODULE_NOTIFY_LOADED event are not supported we can not start
        return VALKEYMODULE_ERR;
    }

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_LOADED, KeySpace_NotificationLoaded) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_GENERIC, KeySpace_NotificationGeneric) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_EXPIRED, KeySpace_NotificationExpired) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_MODULE, KeySpace_NotificationModule) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_KEY_MISS, KeySpace_NotificationModuleKeyMiss) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_STRING, KeySpace_NotificationModuleString) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_STRING, KeySpace_NotificationModuleStringPostNotificationJob) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx,"keyspace.notify", cmdNotify,"",0,0,0) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx,"keyspace.is_module_key_notified", cmdIsModuleKeyNotified,"",0,0,0) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx,"keyspace.is_key_loaded", cmdIsKeyLoaded,"",0,0,0) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "keyspace.del_key_copy", cmdDelKeyCopy,
                                  "write", 0, 0, 0) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }
    
    if (ValkeyModule_CreateCommand(ctx, "keyspace.incr_case1", cmdIncrCase1,
                                  "write", 0, 0, 0) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }
    
    if (ValkeyModule_CreateCommand(ctx, "keyspace.incr_case2", cmdIncrCase2,
                                  "write", 0, 0, 0) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }
    
    if (ValkeyModule_CreateCommand(ctx, "keyspace.incr_case3", cmdIncrCase3,
                                  "write", 0, 0, 0) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "keyspace.incr_dels", cmdIncrDels,
                                  "write", 0, 0, 0) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }

    if (ValkeyModule_CreateCommand(ctx, "keyspace.get_dels", cmdGetDels,
                                  "readonly", 0, 0, 0) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }

    if (argc == 1) {
        const char *ptr = ValkeyModule_StringPtrLen(argv[0], NULL);
        if (!strcasecmp(ptr, "noload")) {
            /* This is a hint that we return ERR at the last moment of OnLoad. */
            ValkeyModule_FreeDict(ctx, loaded_event_log);
            ValkeyModule_FreeDict(ctx, module_event_log);
            return VALKEYMODULE_ERR;
        }
    }

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(loaded_event_log, "^", NULL, 0);
    char* key;
    size_t keyLen;
    ValkeyModuleString* val;
    while((key = ValkeyModule_DictNextC(iter, &keyLen, (void**)&val))){
        ValkeyModule_FreeString(ctx, val);
    }
    ValkeyModule_FreeDict(ctx, loaded_event_log);
    ValkeyModule_DictIteratorStop(iter);
    loaded_event_log = NULL;

    iter = ValkeyModule_DictIteratorStartC(module_event_log, "^", NULL, 0);
    while((key = ValkeyModule_DictNextC(iter, &keyLen, (void**)&val))){
        ValkeyModule_FreeString(ctx, val);
    }
    ValkeyModule_FreeDict(ctx, module_event_log);
    ValkeyModule_DictIteratorStop(iter);
    module_event_log = NULL;

    return VALKEYMODULE_OK;
}
