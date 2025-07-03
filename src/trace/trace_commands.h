/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* ==========================================================================
 * trace_commands.h - support lttng tracing for commands events.
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
#define LTTNG_UST_TRACEPOINT_PROVIDER valkey_commands

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./trace_commands.h"

#if !defined(__VALKEY_TRACE_COMMANDS_H__) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define __VALKEY_TRACE_COMMANDS_H__

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_ENUM(
    /* Tracepoint provider name */
    valkey_commands,

    /* Tracepoint connection type enum */
    valkey_conn_type_enum,

    /* Tracepoint connection type enum values, Source: ConnectionTypeId */
    LTTNG_UST_TP_ENUM_VALUES(
        lttng_ust_field_enum_value("SOCKET", 1)
        lttng_ust_field_enum_value("UNIX", 2)
        lttng_ust_field_enum_value("TLS", 3)
        lttng_ust_field_enum_value("RDMA", 4)
    )
)

LTTNG_UST_TRACEPOINT_EVENT(
	/* Tracepoint provider name */
	valkey_commands,

	/* Tracepoint name */
	command_call,

	/* Input arguments */
	LTTNG_UST_TP_ARGS(
		int, prot,
		const char *, addr,
		const char *, laddr,
		const char *, name,
		uint64_t, duration
	),

	/* Output event fields */
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_enum(valkey_commands, valkey_conn_type_enum, int, enum_field, prot)
		lttng_ust_field_string(addr, addr)
		lttng_ust_field_string(laddr, laddr)
		lttng_ust_field_string(name, name)
		lttng_ust_field_integer(uint64_t, duration, duration)
	)
)

#define valkey_commands_trace(...) lttng_ust_tracepoint(__VA_ARGS__)

#endif /* __VALKEY_TRACE_COMMANDS_H__ */

#include <lttng/tracepoint-event.h>

#else /* USE_LTTNG */

#ifndef __VALKEY_TRACE_COMMANDS_H__
#define __VALKEY_TRACE_COMMANDS_H__

/* avoid compiler warning on empty source file */
static inline void __valkey_commands_trace(void) {
}

#define valkey_commands_trace(...) \
    do {                           \
    } while (0)

#endif /* __VALKEY_TRACE_COMMANDS_H__ */

#endif /* USE_LTTNG */
