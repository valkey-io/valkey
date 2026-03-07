/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* ==========================================================================
 * trace_rdb.h - support lttng tracing for rdb events.
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
#define LTTNG_UST_TRACEPOINT_PROVIDER valkey_rdb

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./trace_rdb.h"

#if !defined(__VALKEY_TRACE_SYS_H__) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define __VALKEY_TRACE_SYS_H__

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
    valkey_rdb,
    valkey_rdb_entry_class,
    LTTNG_UST_TP_ARGS(),
    LTTNG_UST_TP_FIELDS()
)

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
    valkey_rdb,
    valkey_rdb_class,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    ),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer_nowrite(uint64_t, duration, duration)
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_rdb, valkey_rdb_entry_class, valkey_rdb, fork_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_rdb, valkey_rdb_class, valkey_rdb, fork,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_rdb, valkey_rdb_entry_class, valkey_rdb, rdb_unlink_temp_file_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_rdb, valkey_rdb_class, valkey_rdb, rdb_unlink_temp_file,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

#define valkey_rdb_trace(...) lttng_ust_tracepoint(__VA_ARGS__)

#endif /* __VALKEY_TRACE_SYS_H__ */

#include <lttng/tracepoint-event.h>

#else /* USE_LTTNG */

#ifndef __VALKEY_TRACE_SYS_H__
#define __VALKEY_TRACE_SYS_H__

/* avoid compiler warning on empty source file */
static inline void __valkey_rdb_trace(void) {
}

#define valkey_rdb_trace(...) \
    do {                     \
    } while (0)

#endif /* __VALKEY_TRACE_SYS_H__ */

#endif /* USE_LTTNG */
