/*
 * Copyright (c) 2021-2024 zhenwei pi <pizhenwei@bytedance.com>
 *
 * Valkey Over RDMA has been supported as experimental feature since Valkey-8.0.
 * It's also supported as an experimental feature by libvalkey,
 * It may be removed or changed in any version.
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

#ifndef VALKEY_RDMA_H
#define VALKEY_RDMA_H

#include "visibility.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Helper macros to initialize options for RDMA.
 * It's ok to reuse TCP options.
 */
#define VALKEY_OPTIONS_SET_RDMA(opts, ip_, port_) \
    do {                                          \
        (opts)->type = VALKEY_CONN_RDMA;          \
        (opts)->endpoint.tcp.ip = ip_;            \
        (opts)->endpoint.tcp.port = port_;        \
    } while (0)

#define VALKEY_OPTIONS_SET_RDMA_WITH_SOURCE_ADDR(opts, ip_, port_, source_addr_) \
    do {                                                                         \
        (opts)->type = VALKEY_CONN_RDMA;                                         \
        (opts)->endpoint.tcp.ip = ip_;                                           \
        (opts)->endpoint.tcp.port = port_;                                       \
        (opts)->endpoint.tcp.source_addr = source_addr_;                         \
    } while (0)

LIBVALKEY_API int valkeyInitiateRdma(void);

#ifdef __cplusplus
}
#endif

#endif /* VALKEY_RDMA_H */
