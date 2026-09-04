/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __VALKEY_TLS_H
#define __VALKEY_TLS_H

#if defined(USE_OPENSSL)
#include <openssl/ssl.h>

#if defined(TLS_NO_GROUPS)
#define VALKEY_TLS_SUPPORTS_GROUPS 0
#elif defined(SSL_CTX_set1_groups_list)
#define VALKEY_TLS_SUPPORTS_GROUPS 1
#define valkeyTlsCtxSetGroupsList(ctx, list) SSL_CTX_set1_groups_list((ctx), (list))
#elif defined(SSL_CTX_set1_curves_list)
#define VALKEY_TLS_SUPPORTS_GROUPS 1
#define valkeyTlsCtxSetGroupsList(ctx, list) SSL_CTX_set1_curves_list((ctx), (list))
#else
#define VALKEY_TLS_SUPPORTS_GROUPS 0
#endif
#else
#define VALKEY_TLS_SUPPORTS_GROUPS 0
#endif

/* TLS reload functions - only available when TLS is built-in, not as a module */
#if defined(USE_OPENSSL) && USE_OPENSSL == 1 /* BUILD_YES */
void tlsReconfigureIfNeeded(void);
void tlsApplyPendingReload(void);
void tlsConfigureAsync(void);
#endif

#endif /* __VALKEY_TLS_H */
