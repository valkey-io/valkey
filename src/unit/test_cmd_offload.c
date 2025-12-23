#include "test_help.h"
#include "../server.h"

static size_t mock_module_count_value = 0;
static inline size_t test_moduleCount(void) {
    return mock_module_count_value;
}
#define moduleCount test_moduleCount

#include "../cmd_offload.c"

void execCommand(client *c);

/* Test fixture state */
typedef struct {
    int cluster_enabled;
    int io_threads_num;
    int active_io_threads_num;
    int io_threads_do_commands_offloading_with_modules;
    int notify_keyspace_events;
    int io_threads_saturated;
    int offload_throttle_pct;
    long long mstime;
} serverState;

static serverState saved_state;
static client *tc1 = NULL; /* Test client 1 */
static client *tc2 = NULL; /* Test client 2 */

static void setup(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    if (!globalExclusiveQueue.pending_clients) {
        initSlotQueues();
    }
    /* Save server state */
    saved_state.cluster_enabled = server.cluster_enabled;
    saved_state.io_threads_num = server.io_threads_num;
    saved_state.active_io_threads_num = server.active_io_threads_num;
    saved_state.io_threads_do_commands_offloading_with_modules = server.io_threads_do_commands_offloading_with_modules;
    saved_state.notify_keyspace_events = server.notify_keyspace_events;
    saved_state.io_threads_saturated = server.io_threads_saturated;
    saved_state.offload_throttle_pct = server.offload_throttle_pct;
    saved_state.mstime = server.mstime;
    /* Create test clients */
    tc1 = zcalloc(sizeof(client));
    tc1->bstate = zcalloc(sizeof(blockingState));
    tc2 = zcalloc(sizeof(client));
    tc2->bstate = zcalloc(sizeof(blockingState));
}

static void teardown(void) {
    /* Restore server state */
    server.cluster_enabled = saved_state.cluster_enabled;
    server.io_threads_num = saved_state.io_threads_num;
    server.active_io_threads_num = saved_state.active_io_threads_num;
    server.io_threads_do_commands_offloading_with_modules = saved_state.io_threads_do_commands_offloading_with_modules;
    server.notify_keyspace_events = saved_state.notify_keyspace_events;
    server.io_threads_saturated = saved_state.io_threads_saturated;
    server.offload_throttle_pct = saved_state.offload_throttle_pct;
    server.mstime = saved_state.mstime;
    /* Free test clients */
    if (tc1) {
        if (tc1->bstate) zfree(tc1->bstate);
        zfree(tc1);
        tc1 = NULL;
    }
    if (tc2) {
        if (tc2->bstate) zfree(tc2->bstate);
        zfree(tc2);
        tc2 = NULL;
    }
    if (globalExclusiveQueue.pending_clients) {
        listRelease(globalExclusiveQueue.pending_clients);
    }
    if (globalExclusiveQueue.deferred_jobs) {
        listRelease(globalExclusiveQueue.deferred_jobs);
    }
    globalExclusiveQueue.pending_clients = NULL;
    globalExclusiveQueue.deferred_jobs = NULL;
}

#define RUN_TEST(body)            \
    do {                          \
        setup(argc, argv, flags); \
        body;                     \
        teardown();               \
        return 0;                 \
    } while (0)

static struct serverCommand mockReadCmd = {.flags = CMD_READONLY};
static struct serverCommand mockWriteCmd = {.flags = CMD_WRITE};
static struct serverCommand mockAdminCmd = {.flags = CMD_ADMIN};
static struct serverCommand mockNoMandatoryKeysCmd = {.flags = CMD_NO_MANDATORY_KEYS};
static struct serverCommand mockTouchesArbitraryKeysCmd = {.flags = CMD_TOUCHES_ARBITRARY_KEYS};
static struct serverCommand mockExecCmd;
static struct serverCommand mockBlockingCmd = {.flags = CMD_READONLY | CMD_BLOCKING};
static struct serverCommand mockMayReplicateCmd = {.flags = CMD_READONLY | CMD_MAY_REPLICATE};
static struct serverCommand mockSlotExclusiveCmd = {.flags = CMD_WRITE};

int test_getSlotQueue(int argc, char **argv, int flags) {
    RUN_TEST({
        /* slot -1 returns global exclusive queue */
        slotQueue *gq = getSlotQueue(-1);
        TEST_ASSERT(gq == &globalExclusiveQueue);

        slotQueues[0].refcount = 0;
        slotQueues[0].cur_tid = 0;

        TEST_ASSERT(getSlotThreadId(0) == -1); /* Available initially */
        slotQueueIncRef(0);
        setSlotThreadId(0, 5);
        TEST_ASSERT(getSlotThreadId(0) == 5);
        slotQueueDecRef(0);
        TEST_ASSERT(getSlotThreadId(0) == -1); /* Available again */

        /* Test slot at boundary */
        slotQueues[CLUSTER_SLOTS - 1].refcount = 0;
        slotQueues[CLUSTER_SLOTS - 1].cur_tid = 0;

        slotQueueIncRef(CLUSTER_SLOTS - 1);
        setSlotThreadId(CLUSTER_SLOTS - 1, 3);
        TEST_ASSERT(getSlotThreadId(CLUSTER_SLOTS - 1) == 3);
        slotQueueDecRef(CLUSTER_SLOTS - 1);

        /* Test global queue */
        globalExclusiveQueue.refcount = 0;
        exclusiveQueueIncRef();
        setSlotThreadId(-1, 7);
        TEST_ASSERT(getSlotThreadId(-1) == 7);
        globalExclusiveQueue.refcount--; /* Manual decrement to avoid processing jobs */
        TEST_ASSERT(getSlotThreadId(-1) == -1);
    });
}

int test_requiresServerExclusivity(int argc, char **argv, int flags) {
    RUN_TEST({
        TEST_ASSERT(requiresServerExclusivity(&mockWriteCmd, -1) == 1); /* Write with no slot */
        TEST_ASSERT(requiresServerExclusivity(&mockWriteCmd, 0) == 0);  /* Write with slot */
        TEST_ASSERT(requiresServerExclusivity(&mockReadCmd, -1) == 0);  /* Read with no slot */
        TEST_ASSERT(requiresServerExclusivity(&mockAdminCmd, 0) == 1);  /* Admin always */
        TEST_ASSERT(requiresServerExclusivity(&mockAdminCmd, -1) == 1);
        TEST_ASSERT(requiresServerExclusivity(&mockNoMandatoryKeysCmd, 0) == 1);
        TEST_ASSERT(requiresServerExclusivity(&mockTouchesArbitraryKeysCmd, 0) == 1);

        mockExecCmd.proc = execCommand;
        TEST_ASSERT(requiresServerExclusivity(&mockExecCmd, 0) == 1); /* EXEC always */
    });
}

int test_canCommandBeOffloaded(int argc, char **argv, int flags) {
    RUN_TEST({
        server.cluster_enabled = 1;
        server.active_io_threads_num = 4;
        server.io_threads_do_commands_offloading_with_modules = 1;
        server.notify_keyspace_events = 0;

        TEST_ASSERT(canCommandBeOffloaded(-1, &mockReadCmd) == 0); /* slot -1 cannot offload */

        server.cluster_enabled = 0;
        TEST_ASSERT(canCommandBeOffloaded(0, &mockReadCmd) == 0); /* non-cluster mode */

        server.cluster_enabled = 1;
        server.active_io_threads_num = 2;
        TEST_ASSERT(canCommandBeOffloaded(0, &mockReadCmd) == 0); /* not enough threads */

        server.active_io_threads_num = 4;
        server.notify_keyspace_events = NOTIFY_KEY_MISS;
        TEST_ASSERT(canCommandBeOffloaded(0, &mockReadCmd) == 0); /* NOTIFY_KEY_MISS */

        server.notify_keyspace_events = 0;
        TEST_ASSERT(canCommandBeOffloaded(0, &mockBlockingCmd) == 0);     /* blocking cmd */
        TEST_ASSERT(canCommandBeOffloaded(0, &mockMayReplicateCmd) == 0); /* MAY_REPLICATE */
        TEST_ASSERT(canCommandBeOffloaded(0, &mockWriteCmd) == 0);        /* write cmd */
        TEST_ASSERT(canCommandBeOffloaded(0, &mockNoMandatoryKeysCmd) == 0);
        TEST_ASSERT(canCommandBeOffloaded(0, &mockReadCmd) == 1); /* valid read cmd */
    });
}

int test_slotQueueRefCounting(int argc, char **argv, int flags) {
    RUN_TEST({
        TEST_ASSERT(getSlotThreadId(100) == -1); /* Available */
        slotQueueIncRef(100);
        setSlotThreadId(100, 2);
        TEST_ASSERT(getSlotThreadId(100) == 2);
        slotQueueIncRef(100);
        slotQueueDecRef(100);
        TEST_ASSERT(getSlotThreadId(100) == 2); /* Still busy */
        slotQueueDecRef(100);
        TEST_ASSERT(getSlotThreadId(100) == -1); /* Available again */
    });
}

static int job_executed = 0;
static void testJobHandler(void *data) {
    int *val = (int *)data;
    job_executed = *val;
}

int test_threadDeferredJobs(int argc, char **argv, int flags) {
    RUN_TEST({
        initThreadDeferredJobs();

        int test_val = 42;
        threadAddDeferredJob(5, testJobHandler, sizeof(int), &test_val);

        TEST_ASSERT(thread_deferred_jobs != NULL);
        TEST_ASSERT(listLength(thread_deferred_jobs) == 1);

        freeThreadDeferredJobs();
        TEST_ASSERT(thread_deferred_jobs == NULL);
    });
}

int test_slotQueueAddJobAndDispatch(int argc, char **argv, int flags) {
    RUN_TEST({
        job_executed = 0;
        int test_val = 99;

        /* Global job (slot -1) executes immediately */
        list *jobs = listCreate();
        slotQueueAddJob(jobs, -1, testJobHandler, sizeof(int), &test_val);
        TEST_ASSERT(listLength(jobs) == 1);
        dispatchSlotQueueJobs(jobs);
        TEST_ASSERT(job_executed == 99);

        /* Slot job when slot available executes immediately */
        job_executed = 0;
        jobs = listCreate();
        slotQueueAddJob(jobs, 50, testJobHandler, sizeof(int), &test_val);
        dispatchSlotQueueJobs(jobs);
        TEST_ASSERT(job_executed == 99);

        /* Slot job when slot busy gets queued */
        job_executed = 0;
        slotQueueIncRef(50);
        jobs = listCreate();
        slotQueueAddJob(jobs, 50, testJobHandler, sizeof(int), &test_val);
        dispatchSlotQueueJobs(jobs);
        TEST_ASSERT(job_executed == 0);

        /* Release slot - job executes */
        slotQueueDecRef(50);
        TEST_ASSERT(job_executed == 99);
    });
}

int test_canExecuteCommand(int argc, char **argv, int flags) {
    RUN_TEST({
        tc1->cmd = &mockReadCmd;
        tc1->slot = 0;

        /* Non-cluster mode always allows execution */
        server.cluster_enabled = 0;
        server.io_threads_num = 4;
        TEST_ASSERT(canExecuteCommand(tc1) == 1);

        /* Single IO thread always allows execution */
        server.cluster_enabled = 1;
        server.io_threads_num = 1;
        TEST_ASSERT(canExecuteCommand(tc1) == 1);
    });
}

int test_yieldForBusySlot(int argc, char **argv, int flags) {
    RUN_TEST({
        tc1->slot = 200;

        /* Slot available - no yield */
        TEST_ASSERT(yieldForBusySlot(tc1) == 0);

        /* Slot busy - should yield */
        slotQueueIncRef(200);
        server.stat_offload_blocked = 0;
        server.stat_io_threaded_clients_blocked_on_slot = 0;
        server.stat_io_threaded_clients_blocked_total = 0;
        TEST_ASSERT(yieldForBusySlot(tc1) == 1);
        TEST_ASSERT(tc1->flag.blocked == 1);
        TEST_ASSERT(server.stat_io_threaded_clients_blocked_on_slot == 1);

        slotQueueRemoveClient(tc1);
        slotQueueDecRef(200);
    });
}

int test_slotQueueRemoveClient(int argc, char **argv, int flags) {
    RUN_TEST({
        /* Remove client not in any queue - early return */
        tc1->bstate->slot_pending_list = NULL;
        slotQueueRemoveClient(tc1);

        /* Add client to busy slot queue */
        tc1->slot = 300;
        slotQueueIncRef(300);
        server.stat_io_threaded_clients_blocked_on_slot = 0;
        yieldForBusySlot(tc1);
        TEST_ASSERT(tc1->flag.blocked == 1);
        TEST_ASSERT(tc1->bstate->slot_pending_list != NULL);

        /* Remove client from queue */
        slotQueueRemoveClient(tc1);
        TEST_ASSERT(tc1->flag.blocked == 0);
        TEST_ASSERT(tc1->bstate->slot_pending_list == NULL);
        TEST_ASSERT(server.stat_io_threaded_clients_blocked_on_slot == 0);

        slotQueueDecRef(300);
    });
}

int test_ioThreadsOnUnlinkClient(int argc, char **argv, int flags) {
    RUN_TEST({
        tc1->slot = 400;

        /* Client not in any queue */
        ioThreadsOnUnlinkClient(tc1);
        TEST_ASSERT(tc1->bstate->slot_pending_list == NULL);

        /* Add client to queue then unlink */
        slotQueueIncRef(400);
        yieldForBusySlot(tc1);
        TEST_ASSERT(tc1->bstate->slot_pending_list != NULL);

        ioThreadsOnUnlinkClient(tc1);
        TEST_ASSERT(tc1->bstate->slot_pending_list == NULL);
        TEST_ASSERT(tc1->flag.blocked == 0);

        slotQueueDecRef(400);

        /* Test with NULL bstate */
        zfree(tc1->bstate);
        tc1->bstate = NULL;
        ioThreadsOnUnlinkClient(tc1);
    });
}

int test_isServerCronDeferred(int argc, char **argv, int flags) {
    RUN_TEST({
        /* Non-cluster mode - not deferred */
        server.cluster_enabled = 0;
        server.io_threads_num = 4;
        TEST_ASSERT(isServerCronDeferred() == 0);

        /* Single IO thread - not deferred */
        server.cluster_enabled = 1;
        server.io_threads_num = 1;
        TEST_ASSERT(isServerCronDeferred() == 0);

        /* Multiple threads, queue available - not deferred */
        server.io_threads_num = 4;
        TEST_ASSERT(isServerCronDeferred() == 0);

        /* Queue busy - should be deferred */
        exclusiveQueueIncRef();
        TEST_ASSERT(isServerCronDeferred() == 1);
        globalExclusiveQueue.refcount--; /* Manual decrement to avoid processing jobs */
        listNode *ln = listFirst(globalExclusiveQueue.deferred_jobs);
        if (ln) listDelNode(globalExclusiveQueue.deferred_jobs, ln);
    });
}

int test_updateOffloadingThrottle(int argc, char **argv, int flags) {
    RUN_TEST({
        /* High congestion - should decrease throttle */
        server.offload_throttle_pct = 100;
        server.stat_offload_attempts = 100;
        server.stat_offload_blocked = 20; /* 20% > 15% threshold */
        server.mstime = 1000;
        updateOffloadingThrottle();
        server.mstime = 1200;
        updateOffloadingThrottle();
        TEST_ASSERT(server.offload_throttle_pct < 100);

        /* Low congestion - should increase throttle */
        server.offload_throttle_pct = 50;
        server.stat_offload_attempts = 100;
        server.stat_offload_blocked = 2; /* 2% < 5% threshold */
        server.mstime = 1400;
        updateOffloadingThrottle();
        TEST_ASSERT(server.offload_throttle_pct > 50);
    });
}

int test_prefetchSlotQueueInfo(int argc, char **argv, int flags) {
    RUN_TEST({
        prefetchSlotQueueInfo(0);
        prefetchSlotQueueInfo(CLUSTER_SLOTS - 1);
        prefetchSlotQueueInfo(8000);
    });
}

int test_updateOffloadingSaturation(int argc, char **argv, int flags) {
    RUN_TEST({
        /* Reset state - use large mstime to avoid static last_update_time issues */
        server.io_threads_saturated = 0;
        server.offload_throttle_pct = 50;
        server.io_threads_num = 4;
        server.mstime = 1000000;

        /* Test: Skip when not enough threads */
        server.active_io_threads_num = 2;
        updateOffloadingSaturation();
        TEST_ASSERT(server.offload_throttle_pct == 50); /* Unchanged - skipped */

        /* Test: Throttle increases when not saturated (first valid call sets last_update_time) */
        server.active_io_threads_num = 4;
        for (int i = 1; i < 4; i++) {
            atomic_store(&io_threads_stat_io_cpu[i], 20); /* Low IO load */
            atomic_store(&io_threads_stat_cmd_cpu[i], 10);
        }
        updateOffloadingSaturation();
        TEST_ASSERT(server.io_threads_saturated == 0);
        TEST_ASSERT(server.offload_throttle_pct > 50); /* Increased */

        /* Test: Skip due to time interval (called too soon) */
        int prev_throttle = server.offload_throttle_pct;
        server.mstime += 50; /* Only 50ms later, need 100ms */
        updateOffloadingSaturation();
        TEST_ASSERT(server.offload_throttle_pct == prev_throttle); /* Unchanged - skipped */

        /* Test: Becomes saturated with high adjusted IO */
        server.offload_throttle_pct = 50;
        server.io_threads_saturated = 0;
        server.mstime += 200; /* Enough time passed */
        for (int i = 1; i < 4; i++) {
            atomic_store(&io_threads_stat_io_cpu[i], 52); /* High IO - adjusted will be 52*100/90=57 > 55 */
            atomic_store(&io_threads_stat_cmd_cpu[i], 10);
        }
        updateOffloadingSaturation();
        TEST_ASSERT(server.io_threads_saturated == 1);
        TEST_ASSERT(server.offload_throttle_pct < 50); /* Decreased */

        /* Test: Exits saturation when IO drops */
        server.mstime += 200;
        for (int i = 1; i < 4; i++) {
            atomic_store(&io_threads_stat_io_cpu[i], 30); /* Below unsaturation limit */
        }
        updateOffloadingSaturation();
        TEST_ASSERT(server.io_threads_saturated == 0);
    });
}

int test_canExecuteCommandAdvanced(int argc, char **argv, int flags) {
    RUN_TEST({
        server.cluster_enabled = 1;
        server.io_threads_num = 4;
        server.stat_offload_blocked = 0;
        server.stat_io_threaded_clients_blocked_on_slot = 0;
        server.stat_io_threaded_clients_blocked_total = 0;

        /* Server-exclusive command when exclusivity available */
        tc1->cmd = &mockAdminCmd;
        tc1->slot = 0;
        TEST_ASSERT(canExecuteCommand(tc1) == 1);

        /* Server-exclusive command when exclusivity NOT available */
        exclusiveQueueIncRef();
        tc1->cmd = &mockAdminCmd;
        tc1->slot = 0;
        server.stat_offload_blocked = 0;
        TEST_ASSERT(canExecuteCommand(tc1) == 0);
        TEST_ASSERT(tc1->flag.blocked == 1);
        slotQueueRemoveClient(tc1);
        exclusiveQueueDecRef();

        /* Non-exclusive command with no slot */
        tc1->cmd = &mockReadCmd;
        tc1->slot = -1;
        TEST_ASSERT(canExecuteCommand(tc1) == 1);

        /* Command blocked by global exclusive queue */
        exclusiveQueueIncRef();
        tc2->cmd = &mockAdminCmd;
        tc2->slot = 0;
        canExecuteCommand(tc2); /* Adds tc2 to global queue */

        tc1->cmd = &mockReadCmd;
        tc1->slot = 100;
        TEST_ASSERT(canExecuteCommand(tc1) == 0);
        slotQueueRemoveClient(tc1);
        slotQueueRemoveClient(tc2);
        exclusiveQueueDecRef();

        /* Slot-exclusive command when slot is busy */
        slotQueueIncRef(500);
        tc1->cmd = &mockSlotExclusiveCmd;
        tc1->slot = 500;
        TEST_ASSERT(canExecuteCommand(tc1) == 0);
        slotQueueRemoveClient(tc1);
        slotQueueDecRef(500);

        /* Command in same slot context */
        current_slot_context = 123;
        tc1->cmd = &mockReadCmd;
        tc1->slot = 123;
        TEST_ASSERT(canExecuteCommand(tc1) == 1);
        current_slot_context = CTX_NONE;

        /* Server-exclusive command in CTX_EXCLUSIVE context */
        current_slot_context = CTX_EXCLUSIVE;
        tc1->cmd = &mockAdminCmd;
        tc1->slot = 0;
        TEST_ASSERT(canExecuteCommand(tc1) == 1);
        current_slot_context = CTX_NONE;

        /* Command queued when slot queue is not empty (FIFO) */
        slotQueueIncRef(600);
        tc2->slot = 600;
        yieldForBusySlot(tc2);

        tc1->cmd = &mockReadCmd;
        tc1->slot = 600;
        TEST_ASSERT(canExecuteCommand(tc1) == 0);
        slotQueueRemoveClient(tc1);
        slotQueueRemoveClient(tc2);
        slotQueueDecRef(600);
    });
}

int test_isSlotExclusiveCmd(int argc, char **argv, int flags) {
    RUN_TEST({
        TEST_ASSERT(isSlotExclusiveCmd(&mockReadCmd, 0) == 0);
        TEST_ASSERT(isSlotExclusiveCmd(&mockAdminCmd, 0) == 0);
        TEST_ASSERT(isSlotExclusiveCmd(&mockSlotExclusiveCmd, 0) == 1);
        TEST_ASSERT(isSlotExclusiveCmd(&mockBlockingCmd, 0) == 1);
    });
}

static int job_counter = 0;
static int job_data_received = 0;
static void countingJobHandler(void *data) {
    UNUSED(data);
    job_counter++;
}

static void dataJobHandler(void *data) {
    int *val = (int *)data;
    job_data_received = *val;
}

int test_createJobNodeAndProcessOrAddJob(int argc, char **argv, int flags) {
    RUN_TEST({
        job_counter = 0;
        job_data_received = 0;

        slotQueue *q = getSlotQueue(600);

        /* Job with data executes immediately when slot available */
        int test_data = 12345;
        listNode *node = createJobNode(600, dataJobHandler, sizeof(int), &test_data);
        processOrAddJob(q, node);
        TEST_ASSERT(job_data_received == 12345);

        /* Job executes immediately when slot available */
        node = createJobNode(600, countingJobHandler, 0, NULL);
        processOrAddJob(q, node);
        TEST_ASSERT(job_counter == 1);

        /* Job queued when slot busy */
        slotQueueIncRef(600);
        node = createJobNode(600, countingJobHandler, 0, NULL);
        processOrAddJob(q, node);
        TEST_ASSERT(job_counter == 1);
        TEST_ASSERT(listLength(q->deferred_jobs) == 1);

        /* Release slot - job executes */
        slotQueueDecRef(600);
        TEST_ASSERT(job_counter == 2);
    });
}

int test_processDeferredJobsList(int argc, char **argv, int flags) {
    RUN_TEST({
        job_counter = 0;

        /* NULL deferred_jobs - early return */
        slotQueue *sq_null = getSlotQueue(650);
        sq_null->deferred_jobs = NULL;
        processDeferredJobsList(sq_null);
        TEST_ASSERT(job_counter == 0);

        /* Global queue - list not released after processing */
        slotQueue *gq = getSlotQueue(-1);
        listNode *node1 = createJobNode(-1, countingJobHandler, 0, NULL);
        listLinkNodeTail(gq->deferred_jobs, node1);
        processDeferredJobsList(gq);
        TEST_ASSERT(job_counter == 1);
        TEST_ASSERT(gq->deferred_jobs != NULL);

        /* Slot queue - list released after processing */
        slotQueue *sq = getSlotQueue(700);
        sq->deferred_jobs = listCreate();
        listNode *node2 = createJobNode(700, countingJobHandler, 0, NULL);
        listLinkNodeTail(sq->deferred_jobs, node2);
        processDeferredJobsList(sq);
        TEST_ASSERT(job_counter == 2);
        TEST_ASSERT(sq->deferred_jobs == NULL);
    });
}

int test_slotQueueRemoveClientFromGlobalQueue(int argc, char **argv, int flags) {
    RUN_TEST({
        server.cluster_enabled = 1;
        server.io_threads_num = 4;

        globalExclusiveQueue.refcount = 1;
        tc1->cmd = &mockAdminCmd;
        tc1->slot = 0;
        server.stat_offload_blocked = 0;
        server.stat_io_threaded_clients_blocked_on_slot = 0;
        server.stat_io_threaded_clients_blocked_total = 0;
        TEST_ASSERT(canExecuteCommand(tc1) == 0);
        TEST_ASSERT(tc1->flag.blocked == 1);

        slotQueueRemoveClient(tc1);
        TEST_ASSERT(tc1->flag.blocked == 0);
        TEST_ASSERT(globalExclusiveQueue.pending_clients != NULL);

        globalExclusiveQueue.refcount = 0;
    });
}

int test_updateOffloadingThrottleBoundaries(int argc, char **argv, int flags) {
    RUN_TEST({
        /* Throttle doesn't go below minimum */
        server.offload_throttle_pct = 6;
        server.stat_offload_attempts = 100;
        server.stat_offload_blocked = 50;
        server.mstime = 2000;
        updateOffloadingThrottle();
        server.mstime = 2200;
        updateOffloadingThrottle();
        TEST_ASSERT(server.offload_throttle_pct >= 5);

        /* Throttle doesn't go above 100 */
        server.offload_throttle_pct = 98;
        server.stat_offload_attempts = 100;
        server.stat_offload_blocked = 1;
        server.mstime = 2400;
        updateOffloadingThrottle();
        server.mstime = 2600;
        updateOffloadingThrottle();
        TEST_ASSERT(server.offload_throttle_pct <= 100);

        /* No update when attempts is 0 */
        server.offload_throttle_pct = 50;
        server.stat_offload_attempts = 0;
        server.stat_offload_blocked = 0;
        server.mstime = 2800;
        updateOffloadingThrottle();
        TEST_ASSERT(server.offload_throttle_pct == 50);
    });
}
