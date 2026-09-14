/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_STORAGE_MOCK_H
#define VALKEY_STORAGE_MOCK_H

#include "storage.h"

/* The in-memory reference engine, registered under the name "mock". */
const storageEngine *storageMockEngine(void);

#endif /* VALKEY_STORAGE_MOCK_H */
