/* Test module to verify that module write operations are properly rejected
 * when the server is paused (CLIENT PAUSE WRITE or during CLUSTER FAILOVER).
 *
 * This module uses timer callbacks to execute writes during a pause,
 * reproducing the crash from issue #1921 where module-initiated writes
 * bypass the pause check in processCommand() and hit the assertion in
 * propagateNow().
 *
 * Commands:
 *   PAUSETEST.TIMER_CALL       - schedules a timer that does VM_Call("SET")
 *   PAUSETEST.TIMER_REPLICATE  - schedules a timer that does VM_Replicate
 *   PAUSETEST.TIMER_VERBATIM   - schedules a timer that does VM_ReplicateVerbatim
 *   PAUSETEST.GET_RESULT       - returns the result of the last timer operation
 */

#include "valkeymodule.h"
#include <string.h>

#define UNUSED(V) ((void)V)

/* Store the result of the last timer operation for the test to query. */
static int last_call_result = -1;      /* 0 = success, 1 = error, -1 = not run */
static int last_replicate_result = -1;
static int last_verbatim_result = -1;

/* Timer callback: VM_Call with replication. */
void timerCallHandler(ValkeyModuleCtx *ctx, void *data) {
    UNUSED(data);
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "SET", "cc!", "pause_timer_key", "from_call");
    if (reply == NULL) {
        last_call_result = 1;
        return;
    }
    if (ValkeyModule_CallReplyType(reply) == VALKEYMODULE_REPLY_ERROR) {
        last_call_result = 1;
    } else {
        last_call_result = 0;
    }
    ValkeyModule_FreeCallReply(reply);
}

/* Timer callback: VM_Replicate. */
void timerReplicateHandler(ValkeyModuleCtx *ctx, void *data) {
    UNUSED(data);
    int ret = ValkeyModule_Replicate(ctx, "SET", "cc", "pause_timer_key", "from_replicate");
    last_replicate_result = (ret == VALKEYMODULE_OK) ? 0 : 1;
}

/* Timer callback: VM_ReplicateVerbatim.
 * Tests that ReplicateVerbatim is rejected when replica traffic is paused. */
void timerVerbatimHandler(ValkeyModuleCtx *ctx, void *data) {
    UNUSED(data);
    int ret = ValkeyModule_ReplicateVerbatim(ctx);
    last_verbatim_result = (ret == VALKEYMODULE_OK) ? 0 : 1;
}

/* PAUSETEST.TIMER_CALL [delay_ms] - Schedule a timer that does VM_Call. */
int PauseTestTimerCall_Command(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    long long delay = 100;
    if (argc > 1) ValkeyModule_StringToLongLong(argv[1], &delay);
    last_call_result = -1;
    ValkeyModule_CreateTimer(ctx, delay, timerCallHandler, NULL);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

/* PAUSETEST.TIMER_REPLICATE [delay_ms] - Schedule a timer that does VM_Replicate. */
int PauseTestTimerReplicate_Command(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    long long delay = 100;
    if (argc > 1) ValkeyModule_StringToLongLong(argv[1], &delay);
    last_replicate_result = -1;
    ValkeyModule_CreateTimer(ctx, delay, timerReplicateHandler, NULL);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

/* PAUSETEST.TIMER_VERBATIM [delay_ms] - Schedule a timer that does VM_ReplicateVerbatim. */
int PauseTestTimerVerbatim_Command(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    long long delay = 100;
    if (argc > 1) ValkeyModule_StringToLongLong(argv[1], &delay);
    last_verbatim_result = -1;
    ValkeyModule_CreateTimer(ctx, delay, timerVerbatimHandler, NULL);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

/* PAUSETEST.GET_RESULT <call|replicate|verbatim> - Get the result of the last timer op. */
int PauseTestGetResult_Command(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }
    const char *which = ValkeyModule_StringPtrLen(argv[1], NULL);
    int result;
    if (!strcmp(which, "call")) {
        result = last_call_result;
    } else if (!strcmp(which, "replicate")) {
        result = last_replicate_result;
    } else if (!strcmp(which, "verbatim")) {
        result = last_verbatim_result;
    } else {
        ValkeyModule_ReplyWithError(ctx, "ERR unknown result type");
        return VALKEYMODULE_OK;
    }
    ValkeyModule_ReplyWithLongLong(ctx, result);
    return VALKEYMODULE_OK;
}

/* PAUSETEST.AVOIDTRAFFIC - Check VM_AvoidReplicaTraffic. */
int PauseTestAvoidTraffic_Command(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_ReplyWithLongLong(ctx, ValkeyModule_AvoidReplicaTraffic());
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    if (ValkeyModule_Init(ctx, "pausetest", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "PAUSETEST.TIMER_CALL", PauseTestTimerCall_Command, "write", 0, 0, 0) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "PAUSETEST.TIMER_REPLICATE", PauseTestTimerReplicate_Command, "write", 0, 0,
                                   0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "PAUSETEST.TIMER_VERBATIM", PauseTestTimerVerbatim_Command, "write", 0, 0, 0) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "PAUSETEST.GET_RESULT", PauseTestGetResult_Command, "readonly", 0, 0, 0) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "PAUSETEST.AVOIDTRAFFIC", PauseTestAvoidTraffic_Command, "readonly", 0, 0, 0) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
