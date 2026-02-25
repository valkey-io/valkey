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

#ifndef VALKEY_VK_PRIVATE_H
#define VALKEY_VK_PRIVATE_H

#include "win32.h"

#include "valkey.h"
#include "visibility.h"

#include <sds.h>

#include <limits.h>
#include <string.h>

LIBVALKEY_API void valkeySetError(valkeyContext *c, int type, const char *str);

/* Helper function. Convert struct timeval to millisecond. */
static inline int valkeyContextTimeoutMsec(const struct timeval *timeout, long *result) {
    long max_msec = (LONG_MAX - 999) / 1000;
    long msec = INT_MAX;

    /* Only use timeout when not NULL. */
    if (timeout != NULL) {
        if (timeout->tv_usec > 1000000 || timeout->tv_sec > max_msec) {
            *result = msec;
            return VALKEY_ERR;
        }

        msec = (timeout->tv_sec * 1000) + ((timeout->tv_usec + 999) / 1000);

        if (msec < 0 || msec > INT_MAX) {
            msec = INT_MAX;
        }
    }

    *result = msec;
    return VALKEY_OK;
}

/* Get connect timeout of valkeyContext */
static inline int valkeyConnectTimeoutMsec(valkeyContext *c, long *result) {
    const struct timeval *timeout = c->connect_timeout;
    int ret = valkeyContextTimeoutMsec(timeout, result);

    if (ret != VALKEY_OK) {
        valkeySetError(c, VALKEY_ERR_IO, "Invalid timeout specified");
    }

    return ret;
}

/* Get command timeout of valkeyContext */
static inline int valkeyCommandTimeoutMsec(valkeyContext *c, long *result) {
    const struct timeval *timeout = c->command_timeout;
    int ret = valkeyContextTimeoutMsec(timeout, result);

    if (ret != VALKEY_OK) {
        valkeySetError(c, VALKEY_ERR_IO, "Invalid timeout specified");
    }

    return ret;
}

static inline int valkeyContextUpdateConnectTimeout(valkeyContext *c,
                                                    const struct timeval *timeout) {
    /* Same timeval struct, short circuit */
    if (c->connect_timeout == timeout)
        return VALKEY_OK;

    /* Allocate context timeval if we need to */
    if (c->connect_timeout == NULL) {
        c->connect_timeout = vk_malloc(sizeof(*c->connect_timeout));
        if (c->connect_timeout == NULL)
            return VALKEY_ERR;
    }

    memcpy(c->connect_timeout, timeout, sizeof(*c->connect_timeout));
    return VALKEY_OK;
}

static inline int valkeyContextUpdateCommandTimeout(valkeyContext *c,
                                                    const struct timeval *timeout) {
    /* Same timeval struct, short circuit */
    if (c->command_timeout == timeout)
        return VALKEY_OK;

    /* Allocate context timeval if we need to */
    if (c->command_timeout == NULL) {
        c->command_timeout = vk_malloc(sizeof(*c->command_timeout));
        if (c->command_timeout == NULL)
            return VALKEY_ERR;
    }

    memcpy(c->command_timeout, timeout, sizeof(*c->command_timeout));
    return VALKEY_OK;
}

/* Visible although private since required by libvalkey_rdma.so */
LIBVALKEY_API int valkeyContextRegisterFuncs(valkeyContextFuncs *funcs, enum valkeyConnectionType type);
void valkeyContextRegisterTcpFuncs(void);
void valkeyContextRegisterUnixFuncs(void);
void valkeyContextRegisterUserfdFuncs(void);

void valkeyContextSetFuncs(valkeyContext *c);

long long valkeyFormatSdsCommandArgv(sds *target, int argc, const char **argv, const size_t *argvlen);

#endif /* VALKEY_VK_PRIVATE_H */
