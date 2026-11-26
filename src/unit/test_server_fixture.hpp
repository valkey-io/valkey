/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Reusable minimal in-process server bootstrap for component tests.
 *
 * Valkey has no in-process "boot a real server" test tier -- the integration tests spawn a real
 * valkey-server process, and existing gtests are low-level (they never initialize server.db).
 * Component tests for subsystems that operate on live server state (e.g. active defrag) need
 * enough of the server stood up in-process to exercise the real code paths.
 *
 * testServerInitMinimal() brings up: allocator-defrag support, config defaults, shared objects,
 * the modules and eval subsystems (initialized empty, so stages/notifications that touch them are
 * safe no-ops), an event loop, `dbnum` databases (with their keys/expires/keys_with_volatile_items
 * kvstores), and the pubsub kvstores. That is sufficient to drive active-defrag's kvstore stages
 * and the full defrag cycle over a real server.db. It deliberately omits networking, cluster,
 * threads, and background jobs.
 */

#ifndef __TEST_SERVER_FIXTURE_HPP
#define __TEST_SERVER_FIXTURE_HPP

/* Must precede server.h: provides the harness C++ prelude (fmacros, _Atomic handling, gtest). */
#include "generated_wrappers.hpp"

extern "C" {
#include "server.h"
}

/* Bring up minimal server state (idempotent per process). `dbnum` databases are created. */
void testServerInitMinimal(int dbnum);

/* Add a real string key/value to database `dbid` via the normal dbAdd() path. */
void testServerAddStringKey(int dbid, const char *key, const char *val);

/* Add an arbitrary (typed) value object under `key` in database `dbid` via the normal dbAdd()
 * path. Ownership of `val` passes to the database (as with dbAdd); the key name is copied. */
void testServerAddObjectKey(int dbid, const char *key, robj *val);

/* Free all keys in all databases. Safe for keys without TTLs / volatile items (the common
 * case for these tests). Call between tests to avoid cross-test contamination. */
void testServerEmptyAllDbs(void);

/* Initialize allocator-defrag support, once per process (idempotent). allocatorDefragInit()
 * asserts if called twice, so ALL test suites needing it must init through this -- never call
 * allocatorDefragInit() directly. Safe to call from any suite's SetUpTestSuite(). */
void testAllocatorDefragInitOnce(void);

/* True if active defrag is available (allocator has defrag support, i.e. jemalloc); initializes
 * on first use. Sanitizer and macOS builds use libc malloc, where the defrag scan would hit
 * allocatorShouldDefrag()'s defrag_supported assertion -- defrag-exercising tests should
 * GTEST_SKIP() when this is false. */
bool defragTestSupported(void);

#endif /* __TEST_SERVER_FIXTURE_HPP */
