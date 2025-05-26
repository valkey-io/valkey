/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* ==========================================================================
 * trace.c - support generic tracing layers.
 * --------------------------------------------------------------------------
 * Copyright (C) 2025  zhenwei pi <zhenwei.pi@linux.dev>
 * Copyright (C) 2025  zhiqiang li <lizhiqiang.sf@bytedance.com>
 *
 * This work is licensed under BSD 3-Clause, License 1 of the COPYING file in
 * the top-level directory.
 * ==========================================================================
 */

#include "trace.h"
#include <errno.h>

#ifdef USE_LTTNG
pid_t do_fork(void) {
    sigset_t sigset;
    int saved_errno;
    lttng_ust_before_fork(&sigset);
    int childpid = fork();
    saved_errno = errno;
    if (childpid != 0) {
        lttng_ust_after_fork_parent(&sigset);
    } else {
        lttng_ust_after_fork_child(&sigset);
    }
    errno = saved_errno;
    return childpid;
}
#endif
