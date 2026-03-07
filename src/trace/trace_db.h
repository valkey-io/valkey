/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* ==========================================================================
 * trace_db.h - support lttng tracing for db events.
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
#define LTTNG_UST_TRACEPOINT_PROVIDER valkey_db

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./trace_db.h"

#if !defined(__VALKEY_TRACE_DB_H__) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define __VALKEY_TRACE_DB_H__

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
    valkey_db,
    valkey_db_entry_class,
    LTTNG_UST_TP_ARGS(),
    LTTNG_UST_TP_FIELDS()
)

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
    valkey_db,
    valkey_db_class,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    ),
    LTTNG_UST_TP_FIELDS(
        lttng_ust_field_integer_nowrite(uint64_t, duration, duration)
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_entry_class, valkey_db, expire_del_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_class, valkey_db, expire_del,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_entry_class, valkey_db, active_defrag_cycle_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_class, valkey_db, active_defrag_cycle,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_entry_class, valkey_db, eviction_del_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_class, valkey_db, eviction_del,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_entry_class, valkey_db, eviction_lazyfree_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_class, valkey_db, eviction_lazyfree,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_entry_class, valkey_db, eviction_cycle_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_class, valkey_db, eviction_cycle,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_entry_class, valkey_db, expire_cycle_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_class, valkey_db, expire_cycle,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_entry_class, valkey_db, expire_cycle_keys_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_class, valkey_db, expire_cycle_keys,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_entry_class, valkey_db, expire_cycle_fields_entry,
    LTTNG_UST_TP_ARGS()
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
    valkey_db, valkey_db_class, valkey_db, expire_cycle_fields,
    LTTNG_UST_TP_ARGS(
        uint64_t, duration
    )
)

#define valkey_db_trace(...) lttng_ust_tracepoint(__VA_ARGS__)

#endif /* __VALKEY_TRACE_DB_H__ */

#include <lttng/tracepoint-event.h>

#else /* USE_LTTNG */

#ifndef __VALKEY_TRACE_DB_H__
#define __VALKEY_TRACE_DB_H__

/* avoid compiler warning on empty source file */
static inline void __valkey_db_trace(void) {
}

#define valkey_db_trace(...) \
    do {                     \
    } while (0)

#endif /* __VALKEY_TRACE_DB_H__ */

#endif /* USE_LTTNG */
