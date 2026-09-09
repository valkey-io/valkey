/*
 * Copyright (c) 2015 Дмитрий Бахвалов (Dmitry Bakhvalov)
 *
 * Permission for license update:
 *   https://github.com/redis/hiredis/issues/1271#issuecomment-2258225227
 *
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

#ifndef VALKEY_ADAPTERS_MACOSX_H
#define VALKEY_ADAPTERS_MACOSX_H

#include "../async.h"
#include "../cluster.h"
#include "../valkey.h"

#include <CoreFoundation/CoreFoundation.h>

typedef struct {
    valkeyAsyncContext *context;
    CFSocketRef socketRef;
    CFRunLoopSourceRef sourceRef;
} ValkeyRunLoop;

static int freeValkeyRunLoop(ValkeyRunLoop *valkeyRunLoop) {
    if (valkeyRunLoop != NULL) {
        if (valkeyRunLoop->sourceRef != NULL) {
            CFRunLoopSourceInvalidate(valkeyRunLoop->sourceRef);
            CFRelease(valkeyRunLoop->sourceRef);
        }
        if (valkeyRunLoop->socketRef != NULL) {
            CFSocketInvalidate(valkeyRunLoop->socketRef);
            CFRelease(valkeyRunLoop->socketRef);
        }
        vk_free(valkeyRunLoop);
    }
    return VALKEY_ERR;
}

static void valkeyMacOSAddRead(void *privdata) {
    ValkeyRunLoop *valkeyRunLoop = (ValkeyRunLoop *)privdata;
    CFSocketEnableCallBacks(valkeyRunLoop->socketRef, kCFSocketReadCallBack);
}

static void valkeyMacOSDelRead(void *privdata) {
    ValkeyRunLoop *valkeyRunLoop = (ValkeyRunLoop *)privdata;
    CFSocketDisableCallBacks(valkeyRunLoop->socketRef, kCFSocketReadCallBack);
}

static void valkeyMacOSAddWrite(void *privdata) {
    ValkeyRunLoop *valkeyRunLoop = (ValkeyRunLoop *)privdata;
    CFSocketEnableCallBacks(valkeyRunLoop->socketRef, kCFSocketWriteCallBack);
}

static void valkeyMacOSDelWrite(void *privdata) {
    ValkeyRunLoop *valkeyRunLoop = (ValkeyRunLoop *)privdata;
    CFSocketDisableCallBacks(valkeyRunLoop->socketRef, kCFSocketWriteCallBack);
}

static void valkeyMacOSCleanup(void *privdata) {
    ValkeyRunLoop *valkeyRunLoop = (ValkeyRunLoop *)privdata;
    freeValkeyRunLoop(valkeyRunLoop);
}

static void valkeyMacOSAsyncCallback(CFSocketRef __unused s, CFSocketCallBackType callbackType, CFDataRef __unused address, const void __unused *data, void *info) {
    valkeyAsyncContext *context = (valkeyAsyncContext *)info;

    switch (callbackType) {
    case kCFSocketReadCallBack:
        valkeyAsyncHandleRead(context);
        break;

    case kCFSocketWriteCallBack:
        valkeyAsyncHandleWrite(context);
        break;

    default:
        break;
    }
}

static int valkeyMacOSAttach(valkeyAsyncContext *valkeyAsyncCtx, CFRunLoopRef runLoop) {
    valkeyContext *valkeyCtx = &(valkeyAsyncCtx->c);

    /* Nothing should be attached when something is already attached */
    if (valkeyAsyncCtx->ev.data != NULL)
        return VALKEY_ERR;

    ValkeyRunLoop *valkeyRunLoop = (ValkeyRunLoop *)vk_calloc(1, sizeof(ValkeyRunLoop));
    if (valkeyRunLoop == NULL)
        return VALKEY_ERR;

    /* Setup valkey stuff */
    valkeyRunLoop->context = valkeyAsyncCtx;

    valkeyAsyncCtx->ev.addRead = valkeyMacOSAddRead;
    valkeyAsyncCtx->ev.delRead = valkeyMacOSDelRead;
    valkeyAsyncCtx->ev.addWrite = valkeyMacOSAddWrite;
    valkeyAsyncCtx->ev.delWrite = valkeyMacOSDelWrite;
    valkeyAsyncCtx->ev.cleanup = valkeyMacOSCleanup;
    valkeyAsyncCtx->ev.data = valkeyRunLoop;

    /* Initialize and install read/write events */
    CFSocketContext socketCtx = {0, valkeyAsyncCtx, NULL, NULL, NULL};

    valkeyRunLoop->socketRef = CFSocketCreateWithNative(NULL, valkeyCtx->fd,
                                                        kCFSocketReadCallBack | kCFSocketWriteCallBack,
                                                        valkeyMacOSAsyncCallback,
                                                        &socketCtx);
    if (!valkeyRunLoop->socketRef)
        return freeValkeyRunLoop(valkeyRunLoop);

    valkeyRunLoop->sourceRef = CFSocketCreateRunLoopSource(NULL, valkeyRunLoop->socketRef, 0);
    if (!valkeyRunLoop->sourceRef)
        return freeValkeyRunLoop(valkeyRunLoop);

    CFRunLoopAddSource(runLoop, valkeyRunLoop->sourceRef, kCFRunLoopDefaultMode);

    return VALKEY_OK;
}

/* Internal adapter function with correct function signature. */
static int valkeyMacOSAttachAdapter(valkeyAsyncContext *ac, void *loop) {
    return valkeyMacOSAttach(ac, (CFRunLoopRef)loop);
}

VALKEY_UNUSED
static int valkeyClusterOptionsUseMacOS(valkeyClusterOptions *options,
                                        CFRunLoopRef loop) {
    if (options == NULL || loop == NULL) {
        return VALKEY_ERR;
    }

    options->attach_fn = valkeyMacOSAttachAdapter;
    options->attach_data = loop;
    return VALKEY_OK;
}

#endif /* VALKEY_ADAPTERS_MACOSX_H */
