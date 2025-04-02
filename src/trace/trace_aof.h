/* ==========================================================================
 * trace_aof.h - support lttng tracing for aof events.
 * --------------------------------------------------------------------------
 * Copyright (C) 2025  zhenwei pi <pizhenwei@bytedance.com>
 * Copyright (C) 2025  zhiqiang li <lizhiqiang.sf@bytedance.com>
 *
 * This work is licensed under BSD 3-Clause, License 1 of the COPYING file in
 * the top-level directory.
 * ==========================================================================
 */
/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef USE_LTTNG

#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER valkey_aof

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./trace_aof.h"

#if !defined(__VALKEY_TRACE_AOF_H__) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define __VALKEY_TRACE_AOF_H__

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_EVENT(
	/* Tracepoint provider name */
	valkey_aof,

	/* Tracepoint name */
	latency,

	/* Input arguments */
	LTTNG_UST_TP_ARGS(
		const char *, event,
		uint64_t, duration
	),

	/* Output event fields */
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_string(event, event)
		lttng_ust_field_integer(uint64_t, duration, duration)
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
