/* This module is used to test the server events hooks API.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2019, Redis Ltd.
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

#include "valkeymodule.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <assert.h>

/* We need to store events to be able to test and see what we got, and we can't
 * store them in the key-space since that would mess up rdb loading (duplicates)
 * and be lost of flushdb. */
ValkeyModuleDict *event_log = NULL;
/* stores all the keys on which we got 'removed' event */
ValkeyModuleDict *removed_event_log = NULL;
/* stores all the subevent on which we got 'removed' event */
ValkeyModuleDict *removed_subevent_type = NULL;
/* stores all the keys on which we got 'removed' event with expiry information */
ValkeyModuleDict *removed_expiry_log = NULL;

typedef struct EventElement {
    long count;
    ValkeyModuleString *last_val_string;
    long last_val_int;
} EventElement;

void LogStringEvent(ValkeyModuleCtx *ctx, const char* keyname, const char* data) {
    EventElement *event = ValkeyModule_DictGetC(event_log, (void*)keyname, strlen(keyname), NULL);
    if (!event) {
        event = ValkeyModule_Alloc(sizeof(EventElement));
        memset(event, 0, sizeof(EventElement));
        ValkeyModule_DictSetC(event_log, (void*)keyname, strlen(keyname), event);
    }
    if (event->last_val_string) ValkeyModule_FreeString(ctx, event->last_val_string);
    event->last_val_string = ValkeyModule_CreateString(ctx, data, strlen(data));
    event->count++;
}

void LogNumericEvent(ValkeyModuleCtx *ctx, const char* keyname, long data) {
    VALKEYMODULE_NOT_USED(ctx);
    EventElement *event = ValkeyModule_DictGetC(event_log, (void*)keyname, strlen(keyname), NULL);
    if (!event) {
        event = ValkeyModule_Alloc(sizeof(EventElement));
        memset(event, 0, sizeof(EventElement));
        ValkeyModule_DictSetC(event_log, (void*)keyname, strlen(keyname), event);
    }
    event->last_val_int = data;
    event->count++;
}

void FreeEvent(ValkeyModuleCtx *ctx, EventElement *event) {
    if (event->last_val_string)
        ValkeyModule_FreeString(ctx, event->last_val_string);
    ValkeyModule_Free(event);
}

int cmdEventCount(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 2){
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    EventElement *event = ValkeyModule_DictGet(event_log, argv[1], NULL);
    ValkeyModule_ReplyWithLongLong(ctx, event? event->count: 0);
    return VALKEYMODULE_OK;
}

int cmdEventLast(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    if (argc != 2){
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    EventElement *event = ValkeyModule_DictGet(event_log, argv[1], NULL);
    if (event && event->last_val_string)
        ValkeyModule_ReplyWithString(ctx, event->last_val_string);
    else if (event)
        ValkeyModule_ReplyWithLongLong(ctx, event->last_val_int);
    else
        ValkeyModule_ReplyWithNull(ctx);
    return VALKEYMODULE_OK;
}

void clearEvents(ValkeyModuleCtx *ctx)
{
    ValkeyModuleString *key;
    EventElement *event;
    ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStart(event_log, "^", NULL);
    while((key = ValkeyModule_DictNext(ctx, iter, (void**)&event)) != NULL) {
        event->count = 0;
        event->last_val_int = 0;
        if (event->last_val_string) ValkeyModule_FreeString(ctx, event->last_val_string);
        event->last_val_string = NULL;
        ValkeyModule_DictDel(event_log, key, NULL);
        ValkeyModule_Free(event);
    }
    ValkeyModule_DictIteratorStop(iter);
}

int cmdEventsClear(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    VALKEYMODULE_NOT_USED(argc);
    VALKEYMODULE_NOT_USED(argv);
    clearEvents(ctx);
    return VALKEYMODULE_OK;
}

/* Client state change callback. */
void clientChangeCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);

    ValkeyModuleClientInfo *ci = data;
    char *keyname = (sub == VALKEYMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED) ?
        "client-connected" : "client-disconnected";
    LogNumericEvent(ctx, keyname, ci->id);
}

void flushdbCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);

    ValkeyModuleFlushInfo *fi = data;
    char *keyname = (sub == VALKEYMODULE_SUBEVENT_FLUSHDB_START) ?
        "flush-start" : "flush-end";
    LogNumericEvent(ctx, keyname, fi->dbnum);
}

void roleChangeCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(data);

    ValkeyModuleReplicationInfo *ri = data;
    char *keyname = (sub == VALKEYMODULE_EVENT_REPLROLECHANGED_NOW_PRIMARY) ?
        "role-master" : "role-replica";
    LogStringEvent(ctx, keyname, ri->primary_host);
}

void replicationChangeCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(data);

    char *keyname = (sub == VALKEYMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE) ?
        "replica-online" : "replica-offline";
    LogNumericEvent(ctx, keyname, 0);
}

void rasterLinkChangeCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(data);

    char *keyname = (sub == VALKEYMODULE_SUBEVENT_PRIMARY_LINK_UP) ?
        "masterlink-up" : "masterlink-down";
    LogNumericEvent(ctx, keyname, 0);
}

void persistenceCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(data);

    char *keyname = NULL;
    switch (sub) {
        case VALKEYMODULE_SUBEVENT_PERSISTENCE_RDB_START: keyname = "persistence-rdb-start"; break;
        case VALKEYMODULE_SUBEVENT_PERSISTENCE_AOF_START: keyname = "persistence-aof-start"; break;
        case VALKEYMODULE_SUBEVENT_PERSISTENCE_SYNC_AOF_START: keyname = "persistence-syncaof-start"; break;
        case VALKEYMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START: keyname = "persistence-syncrdb-start"; break;
        case VALKEYMODULE_SUBEVENT_PERSISTENCE_ENDED: keyname = "persistence-end"; break;
        case VALKEYMODULE_SUBEVENT_PERSISTENCE_FAILED: keyname = "persistence-failed"; break;
    }
    /* modifying the keyspace from the fork child is not an option, using log instead */
    ValkeyModule_Log(ctx, "warning", "module-event-%s", keyname);
    if (sub == VALKEYMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START ||
        sub == VALKEYMODULE_SUBEVENT_PERSISTENCE_SYNC_AOF_START) 
    {
        LogNumericEvent(ctx, keyname, 0);
    }
}

void loadingCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(data);

    char *keyname = NULL;
    switch (sub) {
        case VALKEYMODULE_SUBEVENT_LOADING_RDB_START: keyname = "loading-rdb-start"; break;
        case VALKEYMODULE_SUBEVENT_LOADING_AOF_START: keyname = "loading-aof-start"; break;
        case VALKEYMODULE_SUBEVENT_LOADING_REPL_START: keyname = "loading-repl-start"; break;
        case VALKEYMODULE_SUBEVENT_LOADING_ENDED: keyname = "loading-end"; break;
        case VALKEYMODULE_SUBEVENT_LOADING_FAILED: keyname = "loading-failed"; break;
    }
    LogNumericEvent(ctx, keyname, 0);
}

void loadingProgressCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);

    ValkeyModuleLoadingProgress *ei = data;
    char *keyname = (sub == VALKEYMODULE_SUBEVENT_LOADING_PROGRESS_RDB) ?
        "loading-progress-rdb" : "loading-progress-aof";
    LogNumericEvent(ctx, keyname, ei->progress);
}

void shutdownCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(data);
    VALKEYMODULE_NOT_USED(sub);

    ValkeyModule_Log(ctx, "warning", "module-event-%s", "shutdown");
}

void cronLoopCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(sub);

    ValkeyModuleCronLoop *ei = data;
    LogNumericEvent(ctx, "cron-loop", ei->hz);
}

void moduleChangeCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);

    ValkeyModuleModuleChange *ei = data;
    char *keyname = (sub == VALKEYMODULE_SUBEVENT_MODULE_LOADED) ?
        "module-loaded" : "module-unloaded";
    LogStringEvent(ctx, keyname, ei->module_name);
}

void swapDbCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(sub);

    ValkeyModuleSwapDbInfo *ei = data;
    LogNumericEvent(ctx, "swapdb-first", ei->dbnum_first);
    LogNumericEvent(ctx, "swapdb-second", ei->dbnum_second);
}

void configChangeCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);
    if (sub != VALKEYMODULE_SUBEVENT_CONFIG_CHANGE) {
        return;
    }

    ValkeyModuleConfigChangeV1 *ei = data;
    LogNumericEvent(ctx, "config-change-count", ei->num_changes);
    LogStringEvent(ctx, "config-change-first", ei->config_names[0]);
}

void keyInfoCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data)
{
    VALKEYMODULE_NOT_USED(e);

    ValkeyModuleKeyInfoV1 *ei = data;
    ValkeyModuleKey *kp = ei->key;
    ValkeyModuleString *key = (ValkeyModuleString *) ValkeyModule_GetKeyNameFromModuleKey(kp);
    const char *keyname = ValkeyModule_StringPtrLen(key, NULL);
    ValkeyModuleString *event_keyname = ValkeyModule_CreateStringPrintf(ctx, "key-info-%s", keyname);
    LogStringEvent(ctx, ValkeyModule_StringPtrLen(event_keyname, NULL), keyname);
    ValkeyModule_FreeString(ctx, event_keyname);

    /* Despite getting a key object from the callback, we also try to re-open it
     * to make sure the callback is called before it is actually removed from the keyspace. */
    ValkeyModuleKey *kp_open = ValkeyModule_OpenKey(ctx, key, VALKEYMODULE_READ);
    assert(ValkeyModule_ValueLength(kp) == ValkeyModule_ValueLength(kp_open));
    ValkeyModule_CloseKey(kp_open);

    /* We also try to RM_Call a command that accesses that key, also to make sure it's still in the keyspace. */
    char *size_command = NULL;
    int key_type = ValkeyModule_KeyType(kp);
    if (key_type == VALKEYMODULE_KEYTYPE_STRING) {
        size_command = "STRLEN";
    } else if (key_type == VALKEYMODULE_KEYTYPE_LIST) {
        size_command = "LLEN";
    } else if (key_type == VALKEYMODULE_KEYTYPE_HASH) {
        size_command = "HLEN";
    } else if (key_type == VALKEYMODULE_KEYTYPE_SET) {
        size_command = "SCARD";
    } else if (key_type == VALKEYMODULE_KEYTYPE_ZSET) {
        size_command = "ZCARD";
    } else if (key_type == VALKEYMODULE_KEYTYPE_STREAM) {
        size_command = "XLEN";
    }
    if (size_command != NULL) {
        ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, size_command, "s", key);
        assert(reply != NULL);
        assert(ValkeyModule_ValueLength(kp) == (size_t) ValkeyModule_CallReplyInteger(reply));
        ValkeyModule_FreeCallReply(reply);
    }

    /* Now use the key object we got from the callback for various validations. */
    ValkeyModuleString *prev = ValkeyModule_DictGetC(removed_event_log, (void*)keyname, strlen(keyname), NULL);
    /* We keep object length */
    ValkeyModuleString *v = ValkeyModule_CreateStringPrintf(ctx, "%zd", ValkeyModule_ValueLength(kp));
    /* For string type, we keep value instead of length */
    if (ValkeyModule_KeyType(kp) == VALKEYMODULE_KEYTYPE_STRING) {
        ValkeyModule_FreeString(ctx, v);
        size_t len;
        /* We need to access the string value with ValkeyModule_StringDMA.
         * ValkeyModule_StringDMA may call dbUnshareStringValue to free the origin object,
         * so we also can test it. */
        char *s = ValkeyModule_StringDMA(kp, &len, VALKEYMODULE_READ);
        v = ValkeyModule_CreateString(ctx, s, len);
    }
    ValkeyModule_DictReplaceC(removed_event_log, (void*)keyname, strlen(keyname), v);
    if (prev != NULL) {
        ValkeyModule_FreeString(ctx, prev);
    }

    const char *subevent = "deleted";
    if (sub == VALKEYMODULE_SUBEVENT_KEY_EXPIRED) {
        subevent = "expired";
    } else if (sub == VALKEYMODULE_SUBEVENT_KEY_EVICTED) {
        subevent = "evicted";
    } else if (sub == VALKEYMODULE_SUBEVENT_KEY_OVERWRITTEN) {
        subevent = "overwritten";
    }
    ValkeyModule_DictReplaceC(removed_subevent_type, (void*)keyname, strlen(keyname), (void *)subevent);

    ValkeyModuleString *prevexpire = ValkeyModule_DictGetC(removed_expiry_log, (void*)keyname, strlen(keyname), NULL);
    ValkeyModuleString *expire = ValkeyModule_CreateStringPrintf(ctx, "%lld", ValkeyModule_GetAbsExpire(kp));
    ValkeyModule_DictReplaceC(removed_expiry_log, (void*)keyname, strlen(keyname), (void *)expire);
    if (prevexpire != NULL) {
        ValkeyModule_FreeString(ctx, prevexpire);
    }
}

static int cmdIsKeyRemoved(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    if(argc != 2){
        return ValkeyModule_WrongArity(ctx);
    }

    const char *key  = ValkeyModule_StringPtrLen(argv[1], NULL);

    ValkeyModuleString *value = ValkeyModule_DictGetC(removed_event_log, (void*)key, strlen(key), NULL);

    if (value == NULL) {
        return ValkeyModule_ReplyWithError(ctx, "ERR Key was not removed");
    }

    const char *subevent = ValkeyModule_DictGetC(removed_subevent_type, (void*)key, strlen(key), NULL);
    ValkeyModule_ReplyWithArray(ctx, 2);
    ValkeyModule_ReplyWithString(ctx, value);
    ValkeyModule_ReplyWithSimpleString(ctx, subevent);

    return VALKEYMODULE_OK;
}

static int cmdKeyExpiry(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc){
    if(argc != 2){
        return ValkeyModule_WrongArity(ctx);
    }

    const char* key  = ValkeyModule_StringPtrLen(argv[1], NULL);
    ValkeyModuleString *expire = ValkeyModule_DictGetC(removed_expiry_log, (void*)key, strlen(key), NULL);
    if (expire == NULL) {
        return ValkeyModule_ReplyWithError(ctx, "ERR Key was not removed");
    }
    ValkeyModule_ReplyWithString(ctx, expire);
    return VALKEYMODULE_OK;
}

/* This function must be present on each module. It is used in order to
 * register the commands into the server. */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
#define VerifySubEventSupported(e, s) \
    if (!ValkeyModule_IsSubEventSupported(e, s)) { \
        return VALKEYMODULE_ERR; \
    }

    if (ValkeyModule_Init(ctx,"testhook",1,VALKEYMODULE_APIVER_1)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    /* Example on how to check if a server sub event is supported */
    if (!ValkeyModule_IsSubEventSupported(ValkeyModuleEvent_ReplicationRoleChanged, VALKEYMODULE_EVENT_REPLROLECHANGED_NOW_PRIMARY)) {
        return VALKEYMODULE_ERR;
    }

    /* replication related hooks */
    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_ReplicationRoleChanged, roleChangeCallback);
    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_ReplicaChange, replicationChangeCallback);
    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_PrimaryLinkChange, rasterLinkChangeCallback);

    /* persistence related hooks */
    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_Persistence, persistenceCallback);
    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_Loading, loadingCallback);
    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_LoadingProgress, loadingProgressCallback);

    /* other hooks */
    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_ClientChange, clientChangeCallback);
    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_FlushDB, flushdbCallback);
    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_Shutdown, shutdownCallback);
    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_CronLoop, cronLoopCallback);

    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_ModuleChange, moduleChangeCallback);
    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_SwapDB, swapDbCallback);

    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_Config, configChangeCallback);

    ValkeyModule_SubscribeToServerEvent(ctx,
        ValkeyModuleEvent_Key, keyInfoCallback);

    event_log = ValkeyModule_CreateDict(ctx);
    removed_event_log = ValkeyModule_CreateDict(ctx);
    removed_subevent_type = ValkeyModule_CreateDict(ctx);
    removed_expiry_log = ValkeyModule_CreateDict(ctx);

    if (ValkeyModule_CreateCommand(ctx,"hooks.event_count", cmdEventCount,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"hooks.event_last", cmdEventLast,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"hooks.clear", cmdEventsClear,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"hooks.is_key_removed", cmdIsKeyRemoved,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;
    if (ValkeyModule_CreateCommand(ctx,"hooks.pexpireat", cmdKeyExpiry,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (argc == 1) {
        const char *ptr = ValkeyModule_StringPtrLen(argv[0], NULL);
        if (!strcasecmp(ptr, "noload")) {
            /* This is a hint that we return ERR at the last moment of OnLoad. */
            ValkeyModule_FreeDict(ctx, event_log);
            ValkeyModule_FreeDict(ctx, removed_event_log);
            ValkeyModule_FreeDict(ctx, removed_subevent_type);
            ValkeyModule_FreeDict(ctx, removed_expiry_log);
            return VALKEYMODULE_ERR;
        }
    }

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    clearEvents(ctx);
    ValkeyModule_FreeDict(ctx, event_log);
    event_log = NULL;

    ValkeyModuleDictIter *iter = ValkeyModule_DictIteratorStartC(removed_event_log, "^", NULL, 0);
    char* key;
    size_t keyLen;
    ValkeyModuleString* val;
    while((key = ValkeyModule_DictNextC(iter, &keyLen, (void**)&val))){
        ValkeyModule_FreeString(ctx, val);
    }
    ValkeyModule_FreeDict(ctx, removed_event_log);
    ValkeyModule_DictIteratorStop(iter);
    removed_event_log = NULL;

    ValkeyModule_FreeDict(ctx, removed_subevent_type);
    removed_subevent_type = NULL;

    iter = ValkeyModule_DictIteratorStartC(removed_expiry_log, "^", NULL, 0);
    while((key = ValkeyModule_DictNextC(iter, &keyLen, (void**)&val))){
        ValkeyModule_FreeString(ctx, val);
    }
    ValkeyModule_FreeDict(ctx, removed_expiry_log);
    ValkeyModule_DictIteratorStop(iter);
    removed_expiry_log = NULL;

    return VALKEYMODULE_OK;
}

