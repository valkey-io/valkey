/* Support for valkey-large-object. The benchmark is a passive RDMA target.
 *
 * Command-line placeholders, replaced per client:
 *   __efa_rkey__  __efa_addr__  __efa_len__
 * e.g.  -- BLOB.GET k:__rand_int__ __efa_rkey__ __efa_addr__ __efa_len__
 *       -- BLOB.SET k:__rand_int__ __efa_len__ __efa_rkey__ __efa_addr__ __efa_len__
 *
 * Build with BUILD_EFA=yes which requires libfabric via pkg-config.
 *
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_BENCHMARK_EFA_H
#define VALKEY_BENCHMARK_EFA_H

#include <stddef.h>
#include <valkey/valkey.h>
#include "sds.h"

/* A client's registered memory buffer */
typedef struct efaRegion efaRegion;

/* Open the libfabric.
 * * `provider`: "efa-direct" or "tcp"
 * * `bind`: tcp source address or NULL.
 * Exits on failure. */
void efaInit(const char *provider, const char *bind, size_t region_len);

/* Widen each __efa_*__ placeholder in `arg` to its fixed 20-byte form
 * so the RESP length is final before clients copy the template. */
sds efaExpandPlaceholders(sds arg);

/* Allocate and register region_len bytes. Exits on failure. */
efaRegion *efaRegisterRegion(void);
void efaReleaseRegion(efaRegion *region);

/* Overwrite the widened placeholders in `buf` with this region's
 * zero-padded rkey, remote address and length. */
void efaSubstituteRegion(const efaRegion *region, char *buf, size_t len);

/* The benchmark's fabric address as the module's HELLO wants it. */
sds efaLocalAddressHex(void);

/* RESP for `BLOB.HELLO <local-address-hex>`. Allocates *buf with
 * valkeyFormatCommand and returns its length. Caller free()s. */
int efaFormatHello(char **buf);

/* A BLOB.HELLO reply has one hex address per server fabric service. Each is
 * inserted into the local domain's address vector once. This must run, per
 * server, before any transfer is issued. Exits on bad reply. */
void efaHandleHelloReply(const valkeyReply *reply);

#endif
