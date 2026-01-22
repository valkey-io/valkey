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
    /* Tracepoint class provider name */
    valkey_cluster,

    /* Tracepoint class name */
    valkey_cluster_class,

    /* List of tracepoint arguments (input) */
    LTTNG_UST_TP_ARGS(
      uint64_t, duration
    ),

    /* List of fields of eventual event (output) */
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer(uint64_t, duration, duration)
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    /* Name of the tracepoint class provider */
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_open,

    /* List of tracepoint arguments (input) */
    LTTNG_UST_TP_ARGS(
      uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    /* Name of the tracepoint class provider */
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_write,

    /* List of tracepoint arguments (input) */
    LTTNG_UST_TP_ARGS(
      uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    /* Name of the tracepoint class provider */
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_fsync,

    /* List of tracepoint arguments (input) */
    LTTNG_UST_TP_ARGS(
      uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    /* Name of the tracepoint class provider */
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_rename,

    /* List of tracepoint arguments (input) */
    LTTNG_UST_TP_ARGS(
      uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    /* Name of the tracepoint class provider */
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_dir_fsync,

    /* List of tracepoint arguments (input) */
    LTTNG_UST_TP_ARGS(
      uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    /* Name of the tracepoint class provider */
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_close,

    /* List of tracepoint arguments (input) */
    LTTNG_UST_TP_ARGS(
      uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    /* Name of the tracepoint class provider */
    valkey_cluster, valkey_cluster_class, valkey_cluster, cluster_config_unlink,

    /* List of tracepoint arguments (input) */
    LTTNG_UST_TP_ARGS(
      uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    /* Name of the tracepoint class provider */
    valkey_cluster, valkey_cluster_class, valkey_cluster, fork,

    /* List of tracepoint arguments (input) */
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
