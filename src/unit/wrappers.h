/**
 * wrappers.h - Function Wrapper Declarations for GoogleTest Unit Tests
 *
 * PURPOSE:
 * This file declares C function wrappers that enable mocking of Valkey C functions
 * in GoogleTest unit tests. It bridges C code with gtest infrastructure.
 *
 * HOW IT WORKS:
 * 1. Declare wrapper functions with __wrap_ prefix (e.g., __wrap_mstime for mstime())
 * 2. generate-wrappers.py parses this file and auto-generates TWO files:
 *    - generated_wrappers.hpp (MockValkey class with MOCK_METHOD macros)
 *    - generated_wrappers.cpp (wrapper implementations that delegate to MockValkey)
 * 3. Build system uses --wrap linker flags to redirect calls: mstime() -> __wrap_mstime()
 * 4. GoogleTest can mock these wrappers to control behavior and verify calls
 *
 * RULES:
 * - All wrapper functions MUST be prefixed with __wrap_
 * - Function signatures MUST exactly match the original C function
 * - DO NOT wrap variadic functions (functions with ...) - GoogleTest doesn't support them
 * - Each wrapper becomes mockable in gtest via the auto-generated MockValkey class
 *
 * WORKFLOW:
 * wrappers.h -> generate-wrappers.py -> [generated_wrappers.hpp + generated_wrappers.cpp]
 *                                        -> linked with gtest
 *
 * See: wrapper_util.py, generate-wrappers.py
 */

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <sched.h>
#ifdef __cplusplus
extern "C" {
#endif

#ifndef VALKEY_WRAPPERS_H
#define VALKEY_WRAPPERS_H
// C/C++ cross-compatibility definitions:
// Some C keywords or built-in types (e.g., _Atomic, _Bool) are not
// recognized or have different meanings in C++. To allow C headers
// to be included in C++ code without errors, we redefine them appropriately.
#define _Atomic(type) alignas(sizeof(type)) type /* Preserve alignment in C++ builds */
#define _Alignas alignas                         /* Replace C _Alignas with C++ alignas */
#define _Bool bool                               /* Replace C _Bool with C++ bool */
#define typename _typename                       /* Avoid conflict with C++ 'typename' keyword */
#define protected protected_                     /* Avoid conflict with C++ 'protected' keyword */

#include "ae.h"
#include "compression.h"
#include "server.h"
#include "stat_calc.h"
#include "throttle.h"
#include "throttle_token_bucket.h"

/**
 * The list of wrapper methods defined.  Each wrapper method must
 * conform to the same naming conventions (i.e. prefix with a
 * '__wrap_') and have its method signature match the overridden
 * method exactly.
 *
 * Important: Please read the README.md file for guidelines about mocking. Your
 * usage of mocking will not be approved if it doesn't follow the guidelines.
 *
 * Note: You should NOT wrap variable argument functions (i.e have "...")
 *       See: https://github.com/google/googletest/blob/master/googlemock/docs/gmock_faq.md#can-i-mock-a-variadic-function
 *       Example: serverLog(int level, const char *fmt, ...) should NOT be mocked.
 */
long long __wrap_aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds, aeTimeProc *proc, void *clientData, aeEventFinalizerProc *finalizerProc);
int __wrap_aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
size_t __wrap_getClientOutputBufferMemoryUsage(client *c);
int __wrap_getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level);
void __wrap_queueClientForReprocessing(client *c);
int __wrap_freeClient(client *c);
ssize_t __wrap_streamDecompressorFeed(streamDecompressor *decompressor, uint8_t *output, size_t output_capacity, const uint8_t *input, size_t input_len, size_t *input_consumed);
void __wrap_zmadvise_dontneed(void *ptr, size_t size_hint);
int __wrap_processPendingCommandAndInputBuffer(client *c);
void __wrap_beforeNextClient(client *c);

void __wrap_blockClientInUseOnKeys(client *c, int nKeys, robj **keys);
void __wrap_unblockClientsInUseOnKey(robj *key);

int __wrap_ACLCheckAllUserCommandPerm(user *u, struct serverCommand *cmd, robj **argv, int argc, int dbid, int *idxptr);

size_t __wrap_hashtableScan(hashtable *ht, size_t cursor, hashtableScanFunction fn, void *privdata);
bool __wrap_hashtableScanHasPassedKey(hashtable *ht, const void *key, size_t cursor);

/* Throttler mocks */
throttler *__wrap_throttle_register(throttleCriteriaProc *criteria_proc, void *priv_data, const char *metrics_name);
void __wrap_throttle_deregister(throttler *t);
double __wrap_throttle_adjustRate(throttler *t, double multiplier);
void __wrap_throttle_getMetrics(const char *metrics_name, throttleMetrics *metrics);
long __wrap_throttle_getGuardrailSecs(throttler *t);

/* Token bucket mocks */
bool __wrap_tokenBucket_tryConsume(tokenBucket *bucket, double tokens, bool force_consume);

/* Statcalc mocks */
double __wrap_tpsCalculator_averageTps(tpsCalculator *calc);
double __wrap_trendCalculator_changePerSecShortTerm(trendCalculator *calc);

#undef protected
#undef _Bool
#undef typename

#endif
#ifdef __cplusplus
}
#endif
