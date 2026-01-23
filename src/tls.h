/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __VALKEY_TLS_H
#define __VALKEY_TLS_H

/* TLS reload functions - only available when TLS is built-in, not as a module */
#if defined(USE_OPENSSL) && USE_OPENSSL == 1 /* BUILD_YES */
void tlsReconfigureIfNeeded(void);
void tlsApplyPendingReload(void);
void tlsConfigureAsync(void);
#endif

#endif /* __VALKEY_TLS_H */
