/*
 * Copyright (c) 2024, zhenwei pi <pizhenwei@bytedance.com>
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
 *   * Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
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

#include "valkey_private.h"

#include <assert.h>

#ifdef VALKEY_USE_THREADS

#if defined(__unix__) || defined(__APPLE__) || defined(__sun)
#define VALKEY_PTHREADS_ONCE 1
#elif defined(_WIN32)
#define VALKEY_WINDOWS_ONCE 1
#else
#error "No call-once implementation available on this platform"
#endif

#endif // VALKEY_USE_THREADS

static void vkRegisterFuncs(void) {
    valkeyContextRegisterTcpFuncs();
    valkeyContextRegisterUnixFuncs();
    valkeyContextRegisterUserfdFuncs();
}

#if VALKEY_PTHREADS_ONCE
#include <pthread.h>

static void vkRegisterFuncsOnce(void) {
    static pthread_once_t flag = PTHREAD_ONCE_INIT;
    pthread_once(&flag, vkRegisterFuncs);
}

#elif VALKEY_WINDOWS_ONCE

#include <windows.h>

/* Windows uses a different signature for the init function */
static BOOL CALLBACK
vkRegisterFuncsWrapper(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    vkRegisterFuncs();
    return TRUE;
}

static void vkRegisterFuncsOnce(void) {
    static INIT_ONCE flag = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&flag, vkRegisterFuncsWrapper, NULL, NULL);
}

#endif

static valkeyContextFuncs *valkeyContextFuncsArray[VALKEY_CONN_MAX];

int valkeyContextRegisterFuncs(valkeyContextFuncs *funcs, enum valkeyConnectionType type) {
    assert(type < VALKEY_CONN_MAX);
    assert(!valkeyContextFuncsArray[type]);

    valkeyContextFuncsArray[type] = funcs;
    return VALKEY_OK;
}

void valkeyContextSetFuncs(valkeyContext *c) {
#ifdef VALKEY_USE_THREADS
    vkRegisterFuncsOnce();
#else
    static int registered = 0;

    if (!registered) {
        registered = 1;
        vkRegisterFuncs();
    }
#endif

    assert(c->connection_type < VALKEY_CONN_MAX);
    assert(!c->funcs);
    c->funcs = valkeyContextFuncsArray[c->connection_type];
    assert(c->funcs != NULL);
}
