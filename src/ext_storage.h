/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Data tiering engine-side entry points.
 *
 * Everything here exists only when BUILD_EXT_STORAGE=yes, which defines
 * USE_EXT_STORAGE. The startup check in server.c compiles either way and
 * refuses to start when ext-storage-enabled is set on a binary built without
 * tiering support. */

#ifndef VALKEY_EXT_STORAGE_H
#define VALKEY_EXT_STORAGE_H

#ifdef USE_EXT_STORAGE

#include "sds.h"
#include "storage/storage.h"

/* Registers built-in storage engines before modules load. */
void extStorageRegisterBuiltinEngines(void);

/* Resolves the configured storage engine and opens it.
 *
 * Called after modules load so that module-provided engines are available.
 * Failure is fatal. A node configured to tier but unable to open its engine
 * must not accept writes it cannot store. */
void extStorageInit(void);

/* Closes the storage engine. Safe to call when nothing is open. */
void extStorageDeinit(void);

/* Returns true when a storage engine is open and ready. */
int extStorageIsActive(void);

/* Appends the ext_storage INFO section to the given sds. */
sds extStorageInfoString(sds info);

/* Returns the serialization callbacks passed to the storage engine.
 * Exposed so tests can drive an engine directly. */
const storageSerdes *extStorageSerdes(void);

/* Returns the active storage engine type, or NULL. Exposed for tests. */
const storageEngine *extStorageEngine(void);

/* Returns the active storage engine context, or NULL. Exposed for tests. */
void *extStorageEngineCtx(void);

/* Test-only: clears the registered engine so the next registration succeeds.
 * Allows test fixtures to isolate engine registration. */
void extStorageTestResetEngine(void);

#endif /* USE_EXT_STORAGE */

#endif /* VALKEY_EXT_STORAGE_H */
