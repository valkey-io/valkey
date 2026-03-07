/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* ==========================================================================
 * trace_cluster.h - support lttng tracing for cluster events.
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
#define LTTNG_UST_TRACEPOINT_PROVIDER valkey_cluster

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./trace_cluster.h"

#if !defined(__VALKEY_TRACE_CLUSTER_H__) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define __VALKEY_TRACE_CLUSTER_H__

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
    valkey_cluster,
    valkey_cluster_entry_class,
    LTTNG_UST_TP_ARGS(),
    LTTNG_UST_TP_FIELDS()
)

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
    valkey_cluster,
    valkey_cluster_class,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    ),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer_nowrite(uint64_t, duration, duration)
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_entry_class, valkey_cluster, cluster_config_open_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_open,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_entry_class, valkey_cluster, cluster_config_write_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_write,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_entry_class, valkey_cluster, cluster_config_fsync_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_fsync,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_entry_class, valkey_cluster, cluster_config_rename_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_rename,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_entry_class, valkey_cluster, cluster_config_dir_fsync_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_dir_fsync,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_entry_class, valkey_cluster, cluster_config_close_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_close,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_entry_class, valkey_cluster, cluster_config_unlink_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_unlink,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_entry_class, valkey_cluster, fork_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_cluster, valkey_cluster_class, valkey_cluster, fork,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

#define valkey_cluster_trace(...) lttng_ust_tracepoint(__VA_ARGS__)

#endif /* __VALKEY_TRACE_CLUSTER_H__ */

#include <lttng/tracepoint-event.h>

#else /* USE_LTTNG */

#ifndef __VALKEY_TRACE_CLUSTER_H__
#define __VALKEY_TRACE_CLUSTER_H__

/* avoid compiler warning on empty source file */
static inline void __valkey_cluster_trace(void) {
}

#define valkey_cluster_trace(...) \
    do {                     \
    } while (0)

#endif /* __VALKEY_TRACE_CLUSTER_H__ */

#endif /* USE_LTTNG */
