/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* ==========================================================================
 * trace_server.h - support lttng tracing for server events.
 * --------------------------------------------------------------------------
 * Copyright (C) 2025  zhenwei pi <zhenwei.pi@linux.dev>
 * Copyright (C) 2025  zhiqiang li <lizhiqiang.sf@bytedance.com>
 *
 * This work is licensed under BSD 3-Clause, License 1 of the COPYING file in
 * the top-level directory.
 * ==========================================================================
 */

#ifdef USE_LTTNG

#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER valkey_server

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./trace_server.h"

#if !defined(__VALKEY_TRACE_SERVER_H__) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define __VALKEY_TRACE_SERVER_H__

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
    valkey_server,
    valkey_server_entry_class,
    LTTNG_UST_TP_ARGS(),
    LTTNG_UST_TP_FIELDS()
)

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
    valkey_server,
    valkey_server_class,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    ),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(uint64_t, duration, duration)
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_entry_class, valkey_server, command_unblocking_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_class, valkey_server, command_unblocking,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_entry_class, valkey_server, while_blocked_cron_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_class, valkey_server, while_blocked_cron,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_entry_class, valkey_server, eventloop_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_class, valkey_server, eventloop,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_entry_class, valkey_server, eventloop_cron_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_class, valkey_server, eventloop_cron,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_entry_class, valkey_server, module_acquire_gil_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_class, valkey_server, module_acquire_gil,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_entry_class, valkey_server, command_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_class, valkey_server, command,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_entry_class, valkey_server, fast_command_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_server, valkey_server_class, valkey_server, fast_command,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

#define valkey_server_trace(...) lttng_ust_tracepoint(__VA_ARGS__)

#endif /* __VALKEY_TRACE_SERVER_H__ */

#include <lttng/tracepoint-event.h>

#else /* USE_LTTNG */

#ifndef __VALKEY_TRACE_SERVER_H__
#define __VALKEY_TRACE_SERVER_H__

/* avoid compiler warning on empty source file */
static inline void __valkey_server_trace(void) {
}

#define valkey_server_trace(...) \
    do {                     \
    } while (0)

#endif /* __VALKEY_TRACE_SERVER_H__ */

#endif /* USE_LTTNG */
