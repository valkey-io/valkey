/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* ==========================================================================
 * trace_aof.h - support lttng tracing for aof events.
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
#define LTTNG_UST_TRACEPOINT_PROVIDER valkey_aof

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./trace_aof.h"

#if !defined(__VALKEY_TRACE_AOF_H__) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define __VALKEY_TRACE_AOF_H__

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
    valkey_aof,
    valkey_aof_entry_class,
    LTTNG_UST_TP_ARGS(),
    LTTNG_UST_TP_FIELDS()
)

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
    valkey_aof,
    valkey_aof_class,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    ),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(uint64_t, duration, duration)
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_entry_class, valkey_aof, fork_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_class, valkey_aof, fork,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_entry_class, valkey_aof, aof_write_pending_fsync_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_class, valkey_aof, aof_write_pending_fsync,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_entry_class, valkey_aof, aof_write_active_child_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_class, valkey_aof, aof_write_active_child,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_entry_class, valkey_aof, aof_write_alone_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_class, valkey_aof, aof_write_alone,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_entry_class, valkey_aof, aof_write_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_class, valkey_aof, aof_write,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_entry_class, valkey_aof, aof_fsync_always_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_class, valkey_aof, aof_fsync_always,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_entry_class, valkey_aof, aof_fstat_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_class, valkey_aof, aof_fstat,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_entry_class, valkey_aof, aof_rename_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_class, valkey_aof, aof_rename,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_entry_class, valkey_aof, aof_flush_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_aof, valkey_aof_class, valkey_aof, aof_flush,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

#define valkey_aof_trace(...) lttng_ust_tracepoint(__VA_ARGS__)

#endif /* __VALKEY_TRACE_AOF_H__ */

#include <lttng/tracepoint-event.h>

#else /* USE_LTTNG */

#ifndef __VALKEY_TRACE_AOF_H__
#define __VALKEY_TRACE_AOF_H__

/* avoid compiler warning on empty source file */
static inline void __valkey_aof_trace(void) {
}

#define valkey_aof_trace(...) \
    do {                     \
    } while (0)

#endif /* __VALKEY_TRACE_AOF_H__ */

#endif /* USE_LTTNG */
