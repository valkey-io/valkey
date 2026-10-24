/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_CGROUP_H
#define VALKEY_CGROUP_H

#include <limits.h>

/* ULLONG_MAX denotes an unlimited or unavailable memory controller. A zero
 * limit is a real limit, not a synonym for unlimited. Return 0 on success,
 * 1 if cgroups are unavailable, or -1 on a read/parse failure. */
int cgroupGetMemoryLimit(unsigned long long *limit);
int cgroupReadMemoryLimit(const char *membership_file, const char *mounts_file, unsigned long long *limit);

#endif
