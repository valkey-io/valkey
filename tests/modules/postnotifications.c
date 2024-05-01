/* This module is used to test the server post keyspace jobs API.
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

/* This module allow to verify 'ValkeyModule_AddPostNotificationJob' by registering to 3
 * key space event:
 * * STRINGS - the module register to all strings notifications and set post notification job
 *             that increase a counter indicating how many times the string key was changed.
 *             In addition, it increase another counter that counts the total changes that
 *             was made on all strings keys.
 * * EXPIRED - the module register to expired event and set post notification job that that
 *             counts the total number of expired events.
 * * EVICTED - the module register to evicted event and set post notification job that that
 *             counts the total number of evicted events.
 *
 * In addition, the module register a new command, 'postnotification.async_set', that performs a set
 * command from a background thread. This allows to check the 'ValkeyModule_AddPostNotificationJob' on
 * notifications that was triggered on a background thread. */

#define _BSD_SOURCE
#define _DEFAULT_SOURCE /* For usleep */

#include "valkeymodule.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

static void KeySpace_PostNotificationStringFreePD(void *pd) {
    ValkeyModule_FreeString(NULL, pd);
}

static void KeySpace_PostNotificationReadKey(ValkeyModuleCtx *ctx, void *pd) {
    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "get", "!s", pd);
    ValkeyModule_FreeCallReply(rep);
}

static void KeySpace_PostNotificationString(ValkeyModuleCtx *ctx, void *pd) {
    VALKEYMODULE_NOT_USED(ctx);
    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "incr", "!s", pd);
    ValkeyModule_FreeCallReply(rep);
}

static int KeySpace_NotificationExpired(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key){
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);

    ValkeyModuleString *new_key = ValkeyModule_CreateString(NULL, "expired", 7);
    int res = ValkeyModule_AddPostNotificationJob(ctx, KeySpace_PostNotificationString, new_key, KeySpace_PostNotificationStringFreePD);
    if (res == VALKEYMODULE_ERR) KeySpace_PostNotificationStringFreePD(new_key);
    return VALKEYMODULE_OK;
}

static int KeySpace_NotificationEvicted(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key){
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);
    VALKEYMODULE_NOT_USED(key);

    const char *key_str = ValkeyModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "evicted", 7) == 0) {
        return VALKEYMODULE_OK; /* do not count the evicted key */
    }

    if (strncmp(key_str, "before_evicted", 14) == 0) {
        return VALKEYMODULE_OK; /* do not count the before_evicted key */
    }

    ValkeyModuleString *new_key = ValkeyModule_CreateString(NULL, "evicted", 7);
    int res = ValkeyModule_AddPostNotificationJob(ctx, KeySpace_PostNotificationString, new_key, KeySpace_PostNotificationStringFreePD);
    if (res == VALKEYMODULE_ERR) KeySpace_PostNotificationStringFreePD(new_key);
    return VALKEYMODULE_OK;
}

static int KeySpace_NotificationString(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key){
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);

    const char *key_str = ValkeyModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "string_", 7) != 0) {
        return VALKEYMODULE_OK;
    }

    if (strcmp(key_str, "string_total") == 0) {
        return VALKEYMODULE_OK;
    }

    ValkeyModuleString *new_key;
    if (strncmp(key_str, "string_changed{", 15) == 0) {
        new_key = ValkeyModule_CreateString(NULL, "string_total", 12);
    } else {
        new_key = ValkeyModule_CreateStringPrintf(NULL, "string_changed{%s}", key_str);
    }

    int res = ValkeyModule_AddPostNotificationJob(ctx, KeySpace_PostNotificationString, new_key, KeySpace_PostNotificationStringFreePD);
    if (res == VALKEYMODULE_ERR) KeySpace_PostNotificationStringFreePD(new_key);
    return VALKEYMODULE_OK;
}

static int KeySpace_LazyExpireInsidePostNotificationJob(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key){
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);

    const char *key_str = ValkeyModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "read_", 5) != 0) {
        return VALKEYMODULE_OK;
    }

    ValkeyModuleString *new_key = ValkeyModule_CreateString(NULL, key_str + 5, strlen(key_str) - 5);;
    int res = ValkeyModule_AddPostNotificationJob(ctx, KeySpace_PostNotificationReadKey, new_key, KeySpace_PostNotificationStringFreePD);
    if (res == VALKEYMODULE_ERR) KeySpace_PostNotificationStringFreePD(new_key);
    return VALKEYMODULE_OK;
}

static int KeySpace_NestedNotification(ValkeyModuleCtx *ctx, int type, const char *event, ValkeyModuleString *key){
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(type);
    VALKEYMODULE_NOT_USED(event);

    const char *key_str = ValkeyModule_StringPtrLen(key, NULL);

    if (strncmp(key_str, "write_sync_", 11) != 0) {
        return VALKEYMODULE_OK;
    }

    /* This test was only meant to check VALKEYMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS.
     * In general it is wrong and discourage to perform any writes inside a notification callback.  */
    ValkeyModuleString *new_key = ValkeyModule_CreateString(NULL, key_str + 11, strlen(key_str) - 11);;
    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "set", "!sc", new_key, "1");
    ValkeyModule_FreeCallReply(rep);
    ValkeyModule_FreeString(NULL, new_key);
    return VALKEYMODULE_OK;
}

static void *KeySpace_PostNotificationsAsyncSetInner(void *arg) {
    ValkeyModuleBlockedClient *bc = arg;
    ValkeyModuleCtx *ctx = ValkeyModule_GetThreadSafeContext(bc);
    ValkeyModule_ThreadSafeContextLock(ctx);
    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "set", "!cc", "string_x", "1");
    ValkeyModule_ThreadSafeContextUnlock(ctx);
    ValkeyModule_ReplyWithCallReply(ctx, rep);
    ValkeyModule_FreeCallReply(rep);

    ValkeyModule_UnblockClient(bc, NULL);
    ValkeyModule_FreeThreadSafeContext(ctx);
    return NULL;
}

static int KeySpace_PostNotificationsAsyncSet(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1)
        return ValkeyModule_WrongArity(ctx);

    pthread_t tid;
    ValkeyModuleBlockedClient *bc = ValkeyModule_BlockClient(ctx,NULL,NULL,NULL,0);

    if (pthread_create(&tid,NULL,KeySpace_PostNotificationsAsyncSetInner,bc) != 0) {
        ValkeyModule_AbortBlock(bc);
        return ValkeyModule_ReplyWithError(ctx,"-ERR Can't start thread");
    }
    return VALKEYMODULE_OK;
}

typedef struct KeySpace_EventPostNotificationCtx {
    ValkeyModuleString *triggered_on;
    ValkeyModuleString *new_key;
} KeySpace_EventPostNotificationCtx;

static void KeySpace_ServerEventPostNotificationFree(void *pd) {
    KeySpace_EventPostNotificationCtx *pn_ctx = pd;
    ValkeyModule_FreeString(NULL, pn_ctx->new_key);
    ValkeyModule_FreeString(NULL, pn_ctx->triggered_on);
    ValkeyModule_Free(pn_ctx);
}

static void KeySpace_ServerEventPostNotification(ValkeyModuleCtx *ctx, void *pd) {
    VALKEYMODULE_NOT_USED(ctx);
    KeySpace_EventPostNotificationCtx *pn_ctx = pd;
    ValkeyModuleCallReply* rep = ValkeyModule_Call(ctx, "lpush", "!ss", pn_ctx->new_key, pn_ctx->triggered_on);
    ValkeyModule_FreeCallReply(rep);
}

static void KeySpace_ServerEventCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent eid, uint64_t subevent, void *data) {
    VALKEYMODULE_NOT_USED(eid);
    VALKEYMODULE_NOT_USED(data);
    if (subevent > 3) {
        ValkeyModule_Log(ctx, "warning", "Got an unexpected subevent '%llu'", (unsigned long long)subevent);
        return;
    }
    static const char* events[] = {
            "before_deleted",
            "before_expired",
            "before_evicted",
            "before_overwritten",
    };

    const ValkeyModuleString *key_name = ValkeyModule_GetKeyNameFromModuleKey(((ValkeyModuleKeyInfo*)data)->key);
    const char *key_str = ValkeyModule_StringPtrLen(key_name, NULL);

    for (int i = 0 ; i < 4 ; ++i) {
        const char *event = events[i];
        if (strncmp(key_str, event , strlen(event)) == 0) {
            return; /* don't log any event on our tracking keys */
        }
    }

    KeySpace_EventPostNotificationCtx *pn_ctx = ValkeyModule_Alloc(sizeof(*pn_ctx));
    pn_ctx->triggered_on = ValkeyModule_HoldString(NULL, (ValkeyModuleString*)key_name);
    pn_ctx->new_key = ValkeyModule_CreateString(NULL, events[subevent], strlen(events[subevent]));
    int res = ValkeyModule_AddPostNotificationJob(ctx, KeySpace_ServerEventPostNotification, pn_ctx, KeySpace_ServerEventPostNotificationFree);
    if (res == VALKEYMODULE_ERR) KeySpace_ServerEventPostNotificationFree(pn_ctx);
}

/* This function must be present on each module. It is used in order to
 * register the commands into the server. */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx,"postnotifications",1,VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }

    if (!(ValkeyModule_GetModuleOptionsAll() & VALKEYMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS)) {
        return VALKEYMODULE_ERR;
    }

    int with_key_events = 0;
    if (argc >= 1) {
        const char *arg = ValkeyModule_StringPtrLen(argv[0], 0);
        if (strcmp(arg, "with_key_events") == 0) {
            with_key_events = 1;
        }
    }

    ValkeyModule_SetModuleOptions(ctx, VALKEYMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS);

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_STRING, KeySpace_NotificationString) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_STRING, KeySpace_LazyExpireInsidePostNotificationJob) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_STRING, KeySpace_NestedNotification) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_EXPIRED, KeySpace_NotificationExpired) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if(ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_EVICTED, KeySpace_NotificationEvicted) != VALKEYMODULE_OK){
        return VALKEYMODULE_ERR;
    }

    if (with_key_events) {
        if(ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_Key, KeySpace_ServerEventCallback) != VALKEYMODULE_OK){
            return VALKEYMODULE_ERR;
        }
    }

    if (ValkeyModule_CreateCommand(ctx, "postnotification.async_set", KeySpace_PostNotificationsAsyncSet,
                                      "write", 0, 0, 0) == VALKEYMODULE_ERR){
        return VALKEYMODULE_ERR;
    }

    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    VALKEYMODULE_NOT_USED(ctx);
    return VALKEYMODULE_OK;
}
