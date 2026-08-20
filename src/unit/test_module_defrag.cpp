#include "generated_wrappers.hpp"

extern "C" {
#include "server.h"
#include "module.h"
#include "monotonic.h"

extern list *modules; /* Defined in module.c; the driver iterates this list. */

/* Core-side accessors the global defrag callback uses (non-static in module.c).
 * The ValkeyModuleDefragCtx struct itself is opaque, so tests go through these. */
int VM_DefragShouldStop(ValkeyModuleDefragCtx *ctx);
int VM_DefragCursorSet(ValkeyModuleDefragCtx *ctx, unsigned long cursor);
int VM_DefragCursorGet(ValkeyModuleDefragCtx *ctx, unsigned long *cursor);
}

/* Unit tests for the global module defrag driver (moduleDefragGlobals /
 * moduleDefragGlobalsStart in module.c). These exercise the scheduling logic
 * directly with fake modules and callbacks, without a running defrag cycle:
 *  - a writable cursor is forwarded to the callback;
 *  - a non-zero cursor left by the callback means "more work", zero means done;
 *  - moduleDefragGlobalsStart() clears the per-cycle done flags;
 *  - a module that keeps consuming the deadline does not starve later modules.
 */

namespace {

/* A controllable clock so tests decide exactly when the deadline is hit.
 * getMonotonicUs is a function pointer, so we can point it at this stub. */
static monotime fake_now_us = 0;
static monotime fakeMonotonicUs(void) {
    return fake_now_us;
}

/* Per-callback bookkeeping. Each fake module routes to one of these. */
struct CbState {
    int calls;                /* how many times the callback ran */
    int saw_cursor;           /* callback was given a usable cursor */
    unsigned long stop_after; /* leave cursor non-zero until it reaches this */
    monotime deadline;        /* deadline passed to this round (for advance_clock) */
    int advance_clock;        /* if set, consume the deadline on every call */
};

/* Callback that walks a virtual cursor from 1..stop_after, then resets to 0
 * (done). Mirrors how a real module signals progress via the cursor. */
static void cursorWalkCb(ValkeyModuleDefragCtx *ctx, CbState *st) {
    st->calls++;

    unsigned long cursor = 0;
    if (VM_DefragCursorGet(ctx, &cursor) == VALKEYMODULE_OK) st->saw_cursor = 1;

    if (st->advance_clock) fake_now_us = st->deadline; /* force the deadline to hit */

    if (cursor + 1 >= st->stop_after) {
        VM_DefragCursorSet(ctx, 0); /* done */
    } else {
        VM_DefragCursorSet(ctx, cursor + 1); /* more work */
    }
}

/* The module API stores a single-arg C callback, so route each fake module to
 * its CbState through a small table of trampolines. */
static CbState *g_states[8];
template <int N> static void trampoline(ValkeyModuleDefragCtx *ctx) {
    cursorWalkCb(ctx, g_states[N]);
}

} // namespace

class ModuleDefragTest : public ::testing::Test {
  protected:
    MockValkey mock;
    RealValkey real;
    list *saved_modules = nullptr;

    void SetUp() override {
        memset(&server, 0, sizeof(valkeyServer));
        monotonicInit();
        getMonotonicUs = fakeMonotonicUs;
        fake_now_us = 1000;
        saved_modules = modules;
        modules = listCreate();
        for (auto &s : g_states) s = nullptr;
    }

    void TearDown() override {
        listIter li;
        listNode *ln;
        listRewind(modules, &li);
        while ((ln = listNext(&li)) != nullptr) zfree(listNodeValue(ln));
        listRelease(modules);
        modules = saved_modules;
    }

    /* Register a fake module with the given callback and step state. */
    ValkeyModule *addModule(ValkeyModuleDefragFunc cb, CbState *st, int idx) {
        auto *m = (ValkeyModule *)zcalloc(sizeof(ValkeyModule));
        m->defrag_cb = cb;
        m->defrag_cursor = 0;
        m->defrag_done_this_cycle = 0;
        g_states[idx] = st;
        listAddNodeTail(modules, m);
        return m;
    }
};

/* The callback is given a usable cursor (the core of the change: previously the
 * global callback got cursor=NULL, so VM_DefragCursorGet returned an error). */
TEST_F(ModuleDefragTest, ForwardsCursor) {
    CbState st = {};
    st.stop_after = 1; /* finishes on first call */
    addModule(trampoline<0>, &st, 0);

    moduleDefragGlobalsStart();
    int more = moduleDefragGlobals(fake_now_us + 1000);

    EXPECT_EQ(st.calls, 1);
    EXPECT_EQ(st.saw_cursor, 1);
    EXPECT_EQ(more, 0); /* module finished, cursor left at 0 */
}

/* A non-zero cursor means "more work" (return 1); resuming continues from the
 * saved cursor until the module zeroes it (return 0). */
TEST_F(ModuleDefragTest, ResumesViaCursorUntilDone) {
    CbState st = {};
    st.stop_after = 3; /* needs 3 calls: cursor 1, 2, then 0 */
    ValkeyModule *m = addModule(trampoline<0>, &st, 0);

    moduleDefragGlobalsStart();

    int more = moduleDefragGlobals(fake_now_us + 1000);
    EXPECT_EQ(more, 1);
    EXPECT_EQ(m->defrag_cursor, 1UL); /* progress persisted on the module */

    more = moduleDefragGlobals(fake_now_us + 1000);
    EXPECT_EQ(more, 1);
    EXPECT_EQ(m->defrag_cursor, 2UL);

    more = moduleDefragGlobals(fake_now_us + 1000);
    EXPECT_EQ(more, 0);               /* done */
    EXPECT_EQ(m->defrag_cursor, 0UL); /* reset */
    EXPECT_EQ(st.calls, 3);
}

/* Once a module reports done it is skipped until moduleDefragGlobalsStart()
 * clears the per-cycle flag for a fresh cycle. */
TEST_F(ModuleDefragTest, DoneResetOnNewCycle) {
    CbState st = {};
    st.stop_after = 1;
    ValkeyModule *m = addModule(trampoline<0>, &st, 0);

    moduleDefragGlobalsStart();
    moduleDefragGlobals(fake_now_us + 1000);
    EXPECT_EQ(m->defrag_done_this_cycle, 1);

    /* Same cycle: a finished module is not called again. */
    moduleDefragGlobals(fake_now_us + 1000);
    EXPECT_EQ(st.calls, 1);

    /* New cycle: the flag is cleared and the module runs again. */
    moduleDefragGlobalsStart();
    EXPECT_EQ(m->defrag_done_this_cycle, 0);
    moduleDefragGlobals(fake_now_us + 1000);
    EXPECT_EQ(st.calls, 2);
}

/* A module that keeps unfinished work and consumes the whole deadline must not
 * prevent later modules from ever being defragged. The driver resumes from the
 * module after the one it stopped on. */
TEST_F(ModuleDefragTest, BusyModuleDoesNotStarveOthers) {
    CbState busy = {};
    busy.stop_after = 1000000; /* never finishes within the test */
    busy.advance_clock = 1;    /* consumes the deadline on every call */
    CbState other = {};
    other.stop_after = 1; /* finishes as soon as it is reached */

    addModule(trampoline<0>, &busy, 0);
    addModule(trampoline<1>, &other, 1);

    moduleDefragGlobalsStart();

    /* Call 1: starts at the busy module, which eats the deadline and breaks out
     * before the other module is reached. */
    fake_now_us = 1000;
    busy.deadline = fake_now_us + 500;
    moduleDefragGlobals(busy.deadline);
    EXPECT_EQ(busy.calls, 1);
    EXPECT_EQ(other.calls, 0);

    /* Call 2: resumes from the module after the busy one, so the other module
     * runs and finishes even though the busy module still has work. */
    fake_now_us = 2000;
    busy.deadline = fake_now_us + 500;
    moduleDefragGlobals(busy.deadline);
    EXPECT_GE(other.calls, 1);
}
