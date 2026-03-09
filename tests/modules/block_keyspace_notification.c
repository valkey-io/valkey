/* This module is used to test blocking the client during a keyspace event. */
#define _BSD_SOURCE
#define _DEFAULT_SOURCE /* For usleep */

#include "valkeymodule.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define EVENT_LOG_MAX_SIZE 1024

static pthread_mutex_t event_log_mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct KeyspaceEventData {
    ValkeyModuleString *key;
    ValkeyModuleString *event;
} KeyspaceEventData;

typedef struct KeyspaceEventLog {
    KeyspaceEventData *log[EVENT_LOG_MAX_SIZE];
    size_t next_index;
} KeyspaceEventLog;

KeyspaceEventLog *event_log = NULL;
int unloaded = 0;

typedef struct BackgroundThreadData {
    KeyspaceEventData *event;
    ValkeyModuleBlockedClient *bc;
} BackgroundThreadData;

static void *GenericEvent_BackgroundWork(void *arg) {
    BackgroundThreadData *data = (BackgroundThreadData *)arg;
    // Sleep for 1 second
    sleep(1);
    pthread_mutex_lock(&event_log_mutex);
    if (!unloaded && event_log->next_index < EVENT_LOG_MAX_SIZE) {
        event_log->log[event_log->next_index] = data->event;
        event_log->next_index++;
    }
    pthread_mutex_unlock(&event_log_mutex);
    if (data->bc) {
        ValkeyModule_UnblockClient(data->bc, NULL);
    }
    ValkeyModule_Free(data);
    pthread_exit(NULL);
}

static int KeySpace_NotificationGeneric(ValkeyModuleCtx *ctx, int type,
                                        const char *event,
                                        ValkeyModuleString *key) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(type);
    ValkeyModuleString *retained_key = ValkeyModule_HoldString(ctx, key);
    ValkeyModuleBlockedClient *bc =
        ValkeyModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    if (bc == NULL) {
        ValkeyModule_Log(ctx, VALKEYMODULE_LOGLEVEL_NOTICE,
                         "Failed to block for event %s on %s!", event,
                         ValkeyModule_StringPtrLen(key, NULL));
    }
    BackgroundThreadData *data =
        ValkeyModule_Alloc(sizeof(BackgroundThreadData));
    data->bc = bc;
    KeyspaceEventData *event_data =
        ValkeyModule_Alloc(sizeof(KeyspaceEventData));
    event_data->key = retained_key;
    event_data->event = ValkeyModule_CreateString(ctx, event, strlen(event));
    data->event = event_data;
    pthread_t tid;
    pthread_create(&tid, NULL, GenericEvent_BackgroundWork, (void *)data);
    return VALKEYMODULE_OK;
}

static int cmdGetEvents(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                        int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    pthread_mutex_lock(&event_log_mutex);
    ValkeyModule_ReplyWithArray(ctx, event_log->next_index);
    for (size_t i = 0; i < event_log->next_index; i++) {
        ValkeyModule_ReplyWithArray(ctx, 4);
        ValkeyModule_ReplyWithStringBuffer(ctx, "event", 5);
        ValkeyModule_ReplyWithString(ctx, event_log->log[i]->event);
        ValkeyModule_ReplyWithStringBuffer(ctx, "key", 3);
        ValkeyModule_ReplyWithString(ctx, event_log->log[i]->key);
    }
    pthread_mutex_unlock(&event_log_mutex);
    return VALKEYMODULE_OK;
}

static int cmdClearEvents(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                          int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    pthread_mutex_lock(&event_log_mutex);
    for (size_t i = 0; i < event_log->next_index; i++) {
        KeyspaceEventData *data = event_log->log[i];
        ValkeyModule_FreeString(ctx, data->event);
        ValkeyModule_FreeString(ctx, data->key);
        ValkeyModule_Free(data);
    }
    event_log->next_index = 0;
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    pthread_mutex_unlock(&event_log_mutex);
    return VALKEYMODULE_OK;
}

/* This function must be present on each Valkey module. It is used in order to
 * register the commands into the Valkey server. */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv,
                        int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx, "testblockingkeyspacenotif", 1,
                          VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    event_log = ValkeyModule_Alloc(sizeof(KeyspaceEventLog));
    event_log->next_index = 0;
    int keySpaceAll = ValkeyModule_GetKeyspaceNotificationFlagsAll();
    if (!(keySpaceAll & VALKEYMODULE_NOTIFY_LOADED)) {
        // VALKEYMODULE_NOTIFY_LOADED event are not supported we can not start
        return VALKEYMODULE_ERR;
    }
    if (ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_LOADED,
                                               KeySpace_NotificationGeneric) !=
            VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_GENERIC,
                                               KeySpace_NotificationGeneric) !=
            VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_EXPIRED,
                                               KeySpace_NotificationGeneric) !=
            VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_MODULE,
                                               KeySpace_NotificationGeneric) !=
            VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(
            ctx, VALKEYMODULE_NOTIFY_KEY_MISS, KeySpace_NotificationGeneric) !=
            VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_STRING,
                                               KeySpace_NotificationGeneric) !=
            VALKEYMODULE_OK ||
        ValkeyModule_SubscribeToKeyspaceEvents(ctx, VALKEYMODULE_NOTIFY_HASH,
                                               KeySpace_NotificationGeneric) !=
            VALKEYMODULE_OK ||
        ValkeyModule_CreateCommand(ctx, "b_keyspace.events", cmdGetEvents, "",
                                   0, 0, 0) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "b_keyspace.clear", cmdClearEvents, "",
                                   0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    pthread_mutex_lock(&event_log_mutex);
    unloaded = 1;
    for (size_t i = 0; i < event_log->next_index; i++) {
        KeyspaceEventData *data = event_log->log[i];
        ValkeyModule_FreeString(ctx, data->event);
        ValkeyModule_FreeString(ctx, data->key);
        ValkeyModule_Free(data);
    }
    ValkeyModule_Free(event_log);
    pthread_mutex_unlock(&event_log_mutex);
    return VALKEYMODULE_OK;
}
