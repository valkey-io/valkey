/* A module whose global defrag callback never finishes: it consumes the whole
 * deadline on every invocation and always leaves a non-zero cursor. Used
 * together with defragtest to check that such a module does not starve the
 * global defrag callbacks of other modules (see defrag.tcl).
 */

#include "valkeymodule.h"

/* Number of times our global defrag callback was invoked. Exposed via INFO so
 * the test can confirm the busy module actually ran. */
unsigned long long busy_calls = 0;

/* Invocations that observed a cleared (zero) cursor, i.e. the start of a fresh pass. Since this
 * module stores a non-zero cursor on every call and never finishes, the only way it can see a
 * zero cursor again is if something discarded the saved one. That makes this a direct probe for
 * the cursor being cleared when a defrag cycle terminates abnormally. */
unsigned long long busy_fresh_starts = 0;

static void defragBusyGlobal(ValkeyModuleDefragCtx *ctx) {
    busy_calls++;

    unsigned long cursor = 0;
    if (ValkeyModule_DefragCursorGet(ctx, &cursor) == VALKEYMODULE_OK && cursor == 0) {
        busy_fresh_starts++;
    }

    /* Burn the rest of the deadline, then report we still have work by leaving
     * a non-zero cursor. This models a module that never drains within a
     * single defrag cycle. */
    while (!ValkeyModule_DefragShouldStop(ctx)) {
        /* spin until the deadline is reached */
    }
    ValkeyModule_DefragCursorSet(ctx, 1);
}

static void BusyInfo(ValkeyModuleInfoCtx *ctx, int for_crash_report) {
    VALKEYMODULE_NOT_USED(for_crash_report);
    ValkeyModule_InfoAddSection(ctx, "stats");
    ValkeyModule_InfoAddFieldULongLong(ctx, "busy_calls", busy_calls);
    ValkeyModule_InfoAddFieldULongLong(ctx, "busy_fresh_starts", busy_fresh_starts);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "defragglobalbusy", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    ValkeyModule_RegisterInfoFunc(ctx, BusyInfo);
    ValkeyModule_RegisterDefragFunc(ctx, defragBusyGlobal);

    return VALKEYMODULE_OK;
}
