/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* ==========================================================================
 * trace.h - support generic tracing layers.
 * --------------------------------------------------------------------------
 * Copyright (C) 2025  zhenwei pi <zhenwei.pi@linux.dev>
 * Copyright (C) 2025  zhiqiang li <lizhiqiang.sf@bytedance.com>
 *
 * This work is licensed under BSD 3-Clause, License 1 of the COPYING file in
 * the top-level directory.
 * ==========================================================================
 */

#if !defined(__VALKEY_TRACE_H__)
#define __VALKEY_TRACE_H__

#include "trace_aof.h"
#include "trace_cluster.h"
#include "trace_server.h"
#include "trace_db.h"
#include "trace_rdb.h"
#include "trace_commands.h"

#ifdef USE_LTTNG
#include <lttng/ust-fork.h>

#define LATENCY_TRACE_SWITCH 1
pid_t do_fork(void);

#define latencyTraceIfNeeded(type, event, var) \
    valkey_##type##_trace(valkey_##type, event, (var));

#else

#define latencyTraceIfNeeded(type, event, var) \
    do {                                       \
    } while (0)
#endif

#endif /* __VALKEY_TRACE_H__ */
