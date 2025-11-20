/* Extracted from anet.c to work properly with Hiredis error reporting.
 *
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2014, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2015, Matt Stancliff <matt at genges dot com>,
 *                     Jan-Erik Rediger <janerik at fnordig dot com>
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

#ifndef VALKEY_NET_H
#define VALKEY_NET_H

#include "valkey.h"
#include "visibility.h"

LIBVALKEY_API void valkeyNetClose(valkeyContext *c);

LIBVALKEY_API int valkeyHasMptcp(void);
LIBVALKEY_API int valkeyCheckSocketError(valkeyContext *c);
LIBVALKEY_API int valkeyTcpSetTimeout(valkeyContext *c, const struct timeval tv);
LIBVALKEY_API int valkeyContextConnectTcp(valkeyContext *c, const valkeyOptions *options);
LIBVALKEY_API int valkeyKeepAlive(valkeyContext *c, int interval);
LIBVALKEY_API int valkeyCheckConnectDone(valkeyContext *c, int *completed);

LIBVALKEY_API int valkeySetTcpNoDelay(valkeyContext *c);
LIBVALKEY_API int valkeyContextSetTcpUserTimeout(valkeyContext *c, unsigned int timeout);

#endif /* VALKEY_NET_H */
