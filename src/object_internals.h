/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* object_internals.h - Internal definition of struct serverObject.
 *
 * This header should ONLY be included by files that need direct access to
 * serverObject fields (object.c, defrag.c, etc.). All other code should use
 * the accessor functions declared in server.h.
 *
 * This separation enforces opacity: code that doesn't include this header
 * cannot access robj fields directly, preventing accidental coupling. */

#ifndef OBJECT_INTERNALS_H
#define OBJECT_INTERNALS_H

#include "server.h"

/* The serverObject struct is variable in size. It has several static fields that are always present,
 * followed by several optional variable-sized fields. The static fields are `type` through `refcount`
 * in the struct-defined order:
 *
 *    +------+----------+-----+-----------+-----------+-----------+----------+----
 *    | type | encoding | lru | hasexpire | hasembkey | hasembval | refcount | ...
 *    +------+----------+-----+-----------+-----------+-----------+----------+----
 *
 * The optional variable-sized embedded data has 2 possible layouts. If value is embedded (hasembval == 1)
 *  the `val_ptr` pointer is not used - instead the val data is embedded:
 *
 *    +------+----------+-----+------------+----------+--------+-----------------+---------+------------+
 *    | type | encoding | lru | has* flags | refcount | expire | key_header_size | key sds | value data |
 *    +------+----------+-----+------------+----------+--------+-----------------+---------+------------+
 *                                                      ^        ^                 ^         ^
 *                                                      |        |                 |         |
 *                                                      |        |                 |         +--- present because hasembval == 1
 *                                                      |        |                 |
 *                                                      |        +-----------------+--- present if hasembkey == 1
 *                                                      |
 *                                                      +--- present if hasexpire == 1
 *
 * Otherwise value is not embedded and we use the `val_ptr` pointer:
 *
 *    +------+----------+-----+------------+----------+---------+--------+-----------------+---------+
 *    | type | encoding | lru | has* flags | refcount | val_ptr | expire | key_header_size | key sds |
 *    +------+----------+-----+------------+----------+---------+--------+-----------------+---------+
 *                                                      ^         ^        ^                 ^
 *                                                      |         |        |                 |
 *                                                      |         |        +-----------------+--- present if hasembkey == 1
 *                                                      |         |
 *                                                      |         +--- present if hasexpire == 1
 *                                                      |
 *                                                      +--- present because hasembval == 0
 */

struct serverObject {
    unsigned type : 4;
    unsigned encoding : 4;
    unsigned lru : LRULFU_BITS;
    unsigned hasexpire : 1;
    unsigned hasembkey : 1;
    unsigned hasembval : 1;
    unsigned refcount : OBJ_REFCOUNT_BITS;
    void *val_ptr; /* Not always present. Use objectGetVal(obj) and
                    * objectSetVal(obj, val) instead. */
};
static_assert(sizeof(struct serverObject) <= 8 + sizeof(void *), "unexpected size - verify struct is packed correctly");
static_assert(sizeof(struct serverObject) == sizeof(robjStatic), "robjStatic size must match struct serverObject");

/* Internal-only accessor for setting refcount directly.
 * Normal code should use incrRefCount()/decrRefCount(). */
void objectSetRefcount(robj *o, unsigned int refcount);

#endif /* OBJECT_INTERNALS_H */
