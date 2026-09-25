/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "test_server_fixture.hpp"

extern "C" {
#include "allocator_defrag.h"
/* Defined in server.c but not exposed in any header (used internally and from main()). */
void initServerConfig(void);
void createSharedObjects(void);
void moduleInitModulesSystem(void);
void evalInit(void);
}

#include <cstring>

/* The one-time server bootstrap below (config, shared objects, module system, eval, event loop)
 * is intentionally never torn down -- it lives for the whole test process. Suppress LeakSanitizer
 * reporting for these allocations so per-test leaks are still caught on sanitizer builds. */
#ifdef VALKEY_ADDRESS_SANITIZER
#include <sanitizer/lsan_interface.h>
#define testLsanDisable() __lsan_disable()
#define testLsanEnable() __lsan_enable()
#else
#define testLsanDisable()
#define testLsanEnable()
#endif

/* allocatorDefragInit() may be called only once and returns -1 when the allocator has no active-
 * defrag support (e.g. libc malloc, which sanitizer and macOS builds use). Cache the result so
 * tests can skip the defrag paths where they'd otherwise hit allocatorShouldDefrag()'s
 * defrag_supported assertion. Sentinel 1 = not yet initialized (allocatorDefragInit returns 0/-1). */
static int g_defrag_init_rc = 1;

void testAllocatorDefragInitOnce(void) {
    if (g_defrag_init_rc == 1) g_defrag_init_rc = allocatorDefragInit();
}

bool defragTestSupported(void) {
    testAllocatorDefragInitOnce();
    return g_defrag_init_rc == 0;
}

void testServerInitMinimal(int dbnum) {
    static int process_inited = 0;
    if (!process_inited) {
        testLsanDisable();
        /* Enable activeDefragAlloc(): allocatorShouldDefrag() asserts defrag_supported. */
        testAllocatorDefragInitOnce();
        /* Populate all config defaults, including active_defrag_* and maxmemory_policy. */
        initServerConfig();
        /* Initialize the modules subsystem: dbAdd() -> notifyKeyspaceEvent() unconditionally calls
         * moduleNotifyKeyspaceEvent(), which dereferences module subscriber structures. This also
         * makes the defragModuleGlobals stage safe for the full-cycle test. Order matches main(). */
        moduleInitModulesSystem();
        createSharedObjects();
        /* Initialize the Lua/eval subsystem so the defragLuaScripts stage's evalScriptsDict()
         * returns a valid (empty) dict instead of dereferencing NULL in the full-cycle test. */
        evalInit();
        /* beginDefragCycle() registers a timer via aeCreateTimeEvent(server.el, ...). */
        server.el = aeCreateEventLoop(128);
        testLsanEnable();
        process_inited = 1;
    }

    /* Deterministic, dependency-light environment for defrag component tests. */
    server.cluster_enabled = 0;
    server.latency_monitor_threshold = 0; /* skip the latency-event dict dependency */
    server.notify_keyspace_events = 0;    /* so dbAdd() does not touch pubsub */
    server.maxmemory = 0;
    /* The cycle scheduler's outer loop tests `getMonotonicUs() <= endtime - active_defrag_cycle_us`
     * on unsigned monotime. Production is safe because endtime is an absolute monotonic time far
     * larger than active_defrag_cycle_us, but a test that passes a small endtime would underflow
     * (endtime < cycle_us -> ~1.8e19), making the budget effectively infinite and the cycle spin
     * forever. Setting cycle_us to 0 removes the subtraction; the per-stage endtime checks still
     * enforce the real bounded-latency (yield) contract. */
    server.active_defrag_cycle_us = 0;

    if (server.db == NULL) {
        testLsanDisable();
        server.dbnum = dbnum;
        server.db = (serverDb **)zcalloc(sizeof(serverDb *) * server.dbnum);
        for (int i = 0; i < server.dbnum; i++) createDatabaseIfNeeded(i);
        server.pubsub_channels =
            kvstoreCreate(&kvstoreChannelHashtableType, 0, KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND);
        server.pubsubshard_channels =
            kvstoreCreate(&kvstoreChannelHashtableType, 0,
                          KVSTORE_ALLOCATE_HASHTABLES_ON_DEMAND | KVSTORE_FREE_EMPTY_HASHTABLES);
        testLsanEnable();
    } else {
        /* The first caller's dbnum wins for the whole process; make a mismatch loud. */
        serverAssert(server.dbnum == dbnum);
    }
}

void testServerAddObjectKey(int dbid, const char *key, robj *val) {
    robj *k = createStringObject(key, strlen(key));
    /* dbAdd() copies the key's sds into the stored value object and takes ownership of the
     * value (via valref); it does not take ownership of the key robj. */
    dbAdd(server.db[dbid], k, &val);
    decrRefCount(k);
}

void testServerAddStringKey(int dbid, const char *key, const char *val) {
    testServerAddObjectKey(dbid, key, createStringObject(val, strlen(val)));
}

void testServerEmptyAllDbs(void) {
    if (server.db == NULL) return;
    for (int i = 0; i < server.dbnum; i++) {
        serverDb *db = server.db[i];
        if (db == NULL) continue;
        /* Entry destructors from the kvstore hashtable types free the stored objects. */
        kvstoreEmpty(db->keys, NULL);
        kvstoreEmpty(db->expires, NULL);
        kvstoreEmpty(db->keys_with_volatile_items, NULL);
    }
}
