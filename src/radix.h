/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_RADIX_H
#define VALKEY_RADIX_H

#include "rax.h"
#include <stdint.h>

struct serverObject;

typedef struct radixObject {
    rax *index;
    uint64_t num_paths;
    uint64_t num_fields;
} radixObject;

void freeRadixObject(struct serverObject *o);
void dismissRadixObject(struct serverObject *o, size_t size_hint);
struct serverObject *radixTypeDup(struct serverObject *o);

#endif
