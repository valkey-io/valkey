/* ==========================================================================
 * trace_aof.c - support lttng tracing for aof events.
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

#define LTTNG_UST_TRACEPOINT_CREATE_PROBES
#define LTTNG_UST_TRACEPOINT_DEFINE

#include "trace_aof.h"
