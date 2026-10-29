/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _SMEMBER_H_
#define _SMEMBER_H_

#include "fmacros.h"
#include "sds.h"
#include "util.h"
#include <stdbool.h>

/* An smember is an sds that may carry an expiration prefix. Always free it with
 * smemberFree(). */
typedef char smember;

smember *smemberCreate(const char *str, size_t len, mstime_t expiry);

/* Returns EXPIRY_NONE when no expiration is set. */
mstime_t smemberGetExpiry(const smember *m);

bool smemberHasExpiry(const smember *m);

/* May reallocate; use the returned pointer. */
smember *smemberSetExpiry(smember *m, mstime_t expiry);

/* Uses commandTimeSnapshot(). */
bool smemberIsExpired(const smember *m);

void smemberFree(smember *m);

size_t smemberMemUsage(const smember *m);

/* Returns NULL when not moved. */
smember *smemberDefrag(smember *m, void *(*defragfn)(void *));

#endif /* _SMEMBER_H_ */
