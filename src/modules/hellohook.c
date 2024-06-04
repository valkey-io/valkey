/* Server hooks API example
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2019, Salvatore Sanfilippo <antirez at gmail dot com>
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

/* Client state change callback. */
void clientChangeCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(e);

    ValkeyModuleClientInfo *ci = data;
    printf("Client %s event for client #%llu %s:%d\n",
           (sub == VALKEYMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED) ? "connection" : "disconnection",
           (unsigned long long)ci->id, ci->addr, ci->port);
}

void flushdbCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(e);

    ValkeyModuleFlushInfo *fi = data;
    if (sub == VALKEYMODULE_SUBEVENT_FLUSHDB_START) {
        if (fi->dbnum != -1) {
            ValkeyModuleCallReply *reply;
            reply = ValkeyModule_Call(ctx, "DBSIZE", "");
            long long numkeys = ValkeyModule_CallReplyInteger(reply);
            printf("FLUSHDB event of database %d started (%lld keys in DB)\n", fi->dbnum, numkeys);
            ValkeyModule_FreeCallReply(reply);
        } else {
            printf("FLUSHALL event started\n");
        }
    } else {
        if (fi->dbnum != -1) {
            printf("FLUSHDB event of database %d ended\n", fi->dbnum);
        } else {
            printf("FLUSHALL event ended\n");
        }
    }
}

/* This function must be present on each module. It is used in order to
 * register the commands into the server. */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "hellohook", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_ClientChange, clientChangeCallback);
    ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_FlushDB, flushdbCallback);
    return VALKEYMODULE_OK;
}
