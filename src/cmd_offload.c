/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 * Command offloading and deferred queues for managing command execution ordering
 * in multithreaded environments.
 */

#include "server.h"
#include "cmd_offload.h"
#include "module.h"
#include "cluster.h"
#include "cluster_slot_stats.h"
#include "io_threads.h"

/* IO saturation check constants */
#define IO_SATURATION_CHECK_INTERVAL_MS 100
#define IO_SATURATION_LIMIT 55      /* Adjusted IO pct limit to enter saturation */
#define IO_UNSATURATION_LIMIT 45    /* Avg IO pct to exit saturation */
#define IO_HEADROOM_BUFFER 10       /* Safety buffer for free capacity calc */
#define IO_TOTAL_FREE_THRESHOLD 200 /* Aggregate free capacity to desaturate */
#define FULL_CORE_UTILIZATION 100

/* Throttling Constants */
#define CONTENTION_RATIO_THRESHOLD 0.15
#define RECOVERY_RATIO_THRESHOLD 0.05
#define OFFLOAD_MIN_THROTTLE 5
#define OFFLOAD_THROTTLE_STEP 5

/* Execution context constants */
#define CTX_NONE -1      /* No active execution context. */
#define CTX_EXCLUSIVE -2 /* Global exclusive to the whole database. */

/*
 * Slot queues ensure proper synchronization between commands that access the same
 * slot or require exclusive access. Commands are queued when there are conflicting
 * operations in progress and executed when it's safe to do so.
 *
 * A slot queue is composed of two lists:
 * 1. A clients list - containing clients blocked waiting for command execution
 * 2. A jobs list - containing general jobs that need to be performed on a given slot
 *    or the whole DB without client context
 *
 * The system maintains a queue per slot and a general  queue
 * (globalExclusiveQueue) for exclusive access to the whole database. */
typedef struct slotQueue {
    int refcount;          /* Active operation counter; >0 blocks exclusive commands. */
    int cur_tid;           /* ID of the thread currently executing on this slot. */
    list *deferred_jobs;   /* Internal server tasks (no client context). */
    list *pending_clients; /* Blocked clients waiting for safe access. */
} slotQueue;

typedef struct deferredJob {
    job_handler handler; /* Callback function. */
    int slot;            /* Target DB slot ID, or -1 for global. */
    char data[];         /* Flexible array for variable-length arguments. */
} deferredJob;

/* Global queues for deferred commands */
static slotQueue globalExclusiveQueue = {0};
static slotQueue slotQueues[CLUSTER_SLOTS] = {0};

/* Thread-local storage for deferred jobs from IO threads */
static _Thread_local list *thread_deferred_jobs = NULL;

/* Context tracking (Main thread only) */
static int current_slot_context = CTX_NONE;
static int offload_suspended_by_contention = 0;

static slotQueue *getSlotQueue(int slot) {
    if (slot == -1) {
        return &globalExclusiveQueue;
    }
    serverAssert(slot >= 0 && slot < CLUSTER_SLOTS);
    return &slotQueues[slot];
}

static int isSlotClientsEmpty(slotQueue *queue) {
    return queue->pending_clients == NULL || listLength(queue->pending_clients) == 0;
}

static int isSlotJobsEmpty(slotQueue *queue) {
    return queue->deferred_jobs == NULL || listLength(queue->deferred_jobs) == 0;
}

static int isSlotQueueEmpty(slotQueue *queue) {
    return isSlotJobsEmpty(queue) && isSlotClientsEmpty(queue);
}

/* Check if a slot is available for immediate processing */
static int slotAvailable(slotQueue *queue) {
    return queue->refcount == 0;
}

static int serverExclusivityAvailable(void) {
    return slotAvailable(&globalExclusiveQueue);
}

/* Returns the thread id handling the given slot, -1 if no thread is handling it */
int getSlotThreadId(int slot) {
    slotQueue *sq = getSlotQueue(slot);
    if (slotAvailable(sq)) return -1;
    return sq->cur_tid;
}

void setSlotThreadId(int slot, int tid) {
    getSlotQueue(slot)->cur_tid = tid;
}

void slotQueueIncRef(int slot) {
    getSlotQueue(slot)->refcount++;
}

void exclusiveQueueIncRef(void) {
    globalExclusiveQueue.refcount++;
}

/* Create a new job node. Allocates memory for listNode + deferredJob + data. */
static listNode *createJobNode(int slot, job_handler handler, size_t data_size, void *data) {
    listNode *node = zmalloc(sizeof(listNode) + sizeof(deferredJob) + data_size);
    deferredJob *job = (deferredJob *)(node + 1);

    job->slot = slot;
    job->handler = handler;
    if (data_size) {
        memcpy(job->data, data, data_size);
    }

    node->value = job;
    return node;
}

/* Process a job immediately if possible, otherwise queue it. */
static void processOrAddJob(slotQueue *q, listNode *jobNode) {
    if (q->refcount == 0) {
        deferredJob *job = listNodeValue(jobNode);
        job->handler(job->data);
        zfree(jobNode);
    } else {
        if (q->deferred_jobs == NULL) {
            q->deferred_jobs = listCreate();
        }
        listLinkNodeTail(q->deferred_jobs, jobNode);
    }
}

/* Execute all jobs in the deferred list and clear the list. */
static void processDeferredJobsList(slotQueue *queue) {
    if (!queue->deferred_jobs) return;

    listIter li;
    listNode *ln;
    listRewind(queue->deferred_jobs, &li);

    while ((ln = listNext(&li))) {
        deferredJob *job = listNodeValue(ln);
        listUnlinkNode(queue->deferred_jobs, ln);
        job->handler(job->data);
        zfree(ln);
    }

    if (queue != &globalExclusiveQueue) {
        listRelease(queue->deferred_jobs);
        queue->deferred_jobs = NULL;
    }
}

int requiresServerExclusivity(struct serverCommand *cmd, int slot) {
    /* Implicit server exclusivity:
     * 1. No slot but writes to keyspace.
     * 2. EXEC (may contain mixed slot commands).
     * 3. No mandatory keys / touches arbitrary keys.
     * 4. Admin commands. */
    if (slot == -1 && (cmd->flags & CMD_WRITE)) return 1;
    if (cmd->proc == execCommand) return 1;
    if (cmd->flags & (CMD_NO_MANDATORY_KEYS | CMD_TOUCHES_ARBITRARY_KEYS | CMD_ADMIN)) return 1;
    return 0;
}

static int isSlotExclusiveCmd(struct serverCommand *cmd, int slot) {
    if (CMD_CAN_BE_OFFLOADED(cmd)) return 0;
    if (requiresServerExclusivity(cmd, slot)) return 0; /* It's server-exclusive, not slot-exclusive */
    return 1;
}

/* Process clients waiting on a specific slot (or global queue). */
static void processPendingSlotClients(int slot) {
    slotQueue *queue = getSlotQueue(slot);

    /* Set the queue context based on queue type */
    current_slot_context = (queue == &globalExclusiveQueue) ? CTX_EXCLUSIVE : slot;

    /* Process only a fixed number of clients to avoid infinite iteration.
     * Clients may be added back to the list during processing. */
    size_t len = listLength(queue->pending_clients);
    listNode *ln;

    /* Process clients from the pending list.
     * We need to check if the list still exists during each iteration
     * because commands like CLIENT KILL may remove clients mid-iteration. */
    while (len-- && queue->pending_clients && (ln = listFirst(queue->pending_clients))) {
        client *c = listNodeValue(ln);

        /* stop if the client's command is blocked by current exclusivity constraints */
        if (queue->refcount) {
            int blocked = (current_slot_context == CTX_EXCLUSIVE) ? requiresServerExclusivity(c->cmd, c->slot) : isSlotExclusiveCmd(c->cmd, c->slot);
            if (blocked) break;
        }

        /* Unblock and process */
        listUnlinkNode(queue->pending_clients, ln);
        c->bstate->slot_pending_list = NULL;
        c->flag.blocked = 0;
        server.stat_io_threaded_clients_blocked_on_slot--;
        /* C_ERR indicates the client was freed during command processing.
         * In that case, skip beforeNextClient() as 'c' is no longer valid. */
        if (processPendingCommandAndInputBuffer(c) != C_ERR) {
            beforeNextClient(c);
        }
    }

    /* Clean up the client list if it's empty */
    if (queue != &globalExclusiveQueue && isSlotClientsEmpty(queue)) {
        listRelease(queue->pending_clients);
        queue->pending_clients = NULL;
    }

    /* Reset the queue context */
    current_slot_context = CTX_NONE;
}

/* Add a client to the pending list and mark it as blocked. */
static void slotQueueAddPendingClient(slotQueue *queue, client *c) {
    if (queue->pending_clients == NULL) {
        queue->pending_clients = listCreate();
    }

    server.stat_offload_blocked++;
    initClientBlockingState(c);
    listLinkNodeTail(queue->pending_clients, &c->bstate->pending_client_node);

    c->flag.pending_command = 1;
    c->bstate->slot_pending_list = queue;
    c->bstate->btype = BLOCKED_SLOT;
    c->flag.blocked = 1;

    server.stat_io_threaded_clients_blocked_on_slot++;
    server.stat_io_threaded_clients_blocked_total++;
}

/* Remove a client from the pending clients list and unblock it */
void slotQueueRemoveClient(client *c) {
    slotQueue *queue = c->bstate->slot_pending_list;
    if (!queue) return;

    serverAssert(!isSlotClientsEmpty(queue));
    listUnlinkNode(queue->pending_clients, &c->bstate->pending_client_node);

    if (queue != &globalExclusiveQueue && listLength(queue->pending_clients) == 0) {
        listRelease(queue->pending_clients);
        queue->pending_clients = NULL;
    }

    /* Reset client state */
    c->flag.pending_command = 0;
    c->flag.blocked = 0;
    c->bstate->slot_pending_list = NULL;
    server.stat_io_threaded_clients_blocked_on_slot--;
}

/* Decrement the reference count and process pending jobs if it reaches zero */
void slotQueueDecRef(int slot) {
    slotQueue *queue = getSlotQueue(slot);
    serverAssert(queue->refcount > 0);
    queue->refcount--;

    if (queue->refcount == 0) {
        if (!isSlotJobsEmpty(queue)) processDeferredJobsList(queue);
        if (!isSlotClientsEmpty(queue)) processPendingSlotClients(slot);
    }
}

void exclusiveQueueDecRef(void) {
    slotQueueDecRef(-1);
}

void ioThreadsOnUnlinkClient(client *c) {
    if (c->bstate && c->bstate->slot_pending_list) {
        slotQueueRemoveClient(c);
    }
}

void threadAddDeferredJob(int slot, job_handler handler, size_t data_size, void *data) {
    listNode *job_node = createJobNode(slot, handler, data_size, data);
    listLinkNodeTail(thread_deferred_jobs, job_node);
}

void initThreadDeferredJobs(void) {
    thread_deferred_jobs = listCreate();
}

void freeThreadDeferredJobs(void) {
    if (thread_deferred_jobs) {
        listRelease(thread_deferred_jobs);
        thread_deferred_jobs = NULL;
    }
}

void sendCommandResultToMain(client *c) {
    if (listLength(thread_deferred_jobs)) {
        sendToMainThread(thread_deferred_jobs, JOB_RES_JOBLIST);
        thread_deferred_jobs = listCreate();
    }
    sendToMainThread(c, JOB_RES_COMMAND);
}

void dispatchSlotQueueJobs(list *jobs_list) {
    listIter li;
    listNode *ln;
    listRewind(jobs_list, &li);

    while ((ln = listNext(&li))) {
        deferredJob *job = listNodeValue(ln);
        if (job->slot == -1) {
            /* Immediately execute non-slot-related jobs */
            job->handler(job->data);
            listDelNode(jobs_list, ln);
        } else {
            /* Push slot-specific jobs to their queue */
            listUnlinkNode(jobs_list, ln);
            processOrAddJob(getSlotQueue(job->slot), ln);
        }
        server.stat_deferred_jobs_processed++;
    }
    listRelease(jobs_list);
}

void slotQueueAddJob(list *jobs_list, int slot, job_handler handler, size_t data_size, void *data) {
    listNode *job_node = createJobNode(slot, handler, data_size, data);
    listLinkNodeTail(jobs_list, job_node);
}

/* This function checks various conditions to ensure thread safety when processing commands.
 * Returns 1 if the command can be safely processed immediately.
 * Returns 0 if it cannot, in which case the command is queued to be processed later. */
int canExecuteCommand(client *c) {
    if (!server.cluster_enabled || server.io_threads_num == 1) {
        return 1;
    }

    /* Global Exclusive Check */
    if (requiresServerExclusivity(c->cmd, c->slot)) {
        if (current_slot_context == CTX_EXCLUSIVE) return 1;

        if (serverExclusivityAvailable()) return 1;

        /* We can't execute the command right now */
        slotQueueAddPendingClient(&globalExclusiveQueue, c);
        return 0;
    }

    if (c->slot == -1) return 1;                   /* Non-exclusive, no specific slot */
    if (current_slot_context == c->slot) return 1; /* Already executing in this slot */

    /* Wait if there are exclusive commands pending globally */
    if (current_slot_context == CTX_NONE && !isSlotQueueEmpty(&globalExclusiveQueue)) {
        slotQueueAddPendingClient(&globalExclusiveQueue, c);
        return 0;
    }

    /* Slot-Level Checks */
    slotQueue *q = getSlotQueue(c->slot);

    /* Queue if others are already waiting on this slot (FIFO) */
    if (!isSlotQueueEmpty(q)) {
        slotQueueAddPendingClient(q, c);
        return 0;
    }

    /* Queue write commands if the slot is currently busy with reads */
    if (isSlotExclusiveCmd(c->cmd, c->slot) && !slotAvailable(q)) {
        slotQueueAddPendingClient(q, c);
        return 0;
    }

    return 1;
}

/*  returns if the command should be postponed due to busy slot */
int yieldForBusySlot(client *c) {
    slotQueue *q = getSlotQueue(c->slot);
    if (!slotAvailable(q)) {
        slotQueueAddPendingClient(q, c);
        return 1; /* Postpone the command execution */
    }
    return 0;
}

static void deferServerCron(void *data) {
    UNUSED(data);
    long long interval = serverCron(server.el, 0, NULL);
    aeCreateTimeEvent(server.el, interval, serverCron, NULL, NULL);
}

int isServerCronDeferred(void) {
    if (!server.cluster_enabled || server.io_threads_num == 1) return 0;
    if (slotAvailable(&globalExclusiveQueue)) return 0;

    listNode *job_node = createJobNode(-1, deferServerCron, 0, NULL);
    listLinkNodeTail(globalExclusiveQueue.deferred_jobs, job_node);
    return 1;
}

void initSlotQueues(void) {
    globalExclusiveQueue.pending_clients = listCreate();
    globalExclusiveQueue.deferred_jobs = listCreate();
}

void prefetchSlotQueueInfo(int slot) {
    valkey_prefetch(slotQueues + slot);
}

/* Check if a command can be offloaded to IO threads.
 * Returns 1 if the command can be offloaded, 0 otherwise. */
int canCommandBeOffloaded(int slot, struct serverCommand *cmd) {
    if (slot == -1) return 0;
    if (!server.cluster_enabled) {
        return 0; /* Avoid offloading commands in non cluster mode. */
    }

    if (server.active_io_threads_num <= 2) {
        return 0; /* Not enough IO threads to offload to. */
    }

    if (moduleCount() > 0 && !server.io_threads_do_commands_offloading_with_modules) return 0;
    if (!CMD_CAN_BE_OFFLOADED(cmd)) return 0;

    if (server.notify_keyspace_events & NOTIFY_KEY_MISS) {
        return 0; /* Avoid offloading commands when NOTIFY_KEY_MISS is enabled as IO threads can't handle it. */
    }

    return 1;
}

int _writeToClient(client *c);

void ioThreadCallCommand(client *c) {
    c->flag.executing_command = 1;
    current_client = c;
    executing_client = c;
    cmd_time_snapshot = server.mstime;

    monotime monotonic_start = getMonotonicUs();
    c->cmd->proc(c);
    c->flag.executing_command = 0;

    c->duration = getMonotonicUs() - monotonic_start;
    c->io_command_state = CLIENT_COMPLETED_IO;
}

void ioThreadWriteAfterCmd(client *c) {
    c->nwritten = 0;
    c->write_flags = 0;

    c->io_last_reply_block = listLast(c->reply);
    if (c->io_last_reply_block) {
        c->io_last_bufpos = ((clientReplyBlock *)listNodeValue(c->io_last_reply_block))->used;
    } else {
        c->io_last_bufpos = (size_t)c->bufpos;
    }

    _writeToClient(c);
    c->io_write_state = CLIENT_COMPLETED_IO;
}

static void processClientIOCommandDone(client *c) {
    serverAssert(c->io_command_state == CLIENT_COMPLETED_IO);
    c->io_command_state = CLIENT_IDLE;

    if (c->flag.close_after_command) {
        c->flag.close_after_command = 0;
        c->flag.close_after_reply = 1;
    }

    /* Stats & Logging */
    struct serverCommand *real_cmd = c->realcmd;
    real_cmd->calls++;
    real_cmd->microseconds += c->duration;
    c->commands_processed++;
    server.stat_numcommands++;

    char *latency_event = (real_cmd->flags & CMD_FAST) ? "fast-command" : "command";
    latencyAddSampleIfNeeded(latency_event, c->duration / 1000);
    if (server.latency_tracking_enabled) {
        updateCommandLatencyHistogram(&(real_cmd->latency_histogram), c->duration * 1000);
    }

    commandlogPushCurrentCommand(c, real_cmd);

    if (!(c->cmd->flags & (CMD_SKIP_MONITOR | CMD_ADMIN))) {
        robj **argv = c->original_argv ? c->original_argv : c->argv;
        int argc = c->original_argv ? c->original_argc : c->argc;
        replicationFeedMonitors(c, server.monitors, c->db->id, argv, argc);
    }

    clusterSlotStatsAddCpuDuration(c, c->duration);
    clusterSlotStatsAddNetworkBytesOutForUserClient(c);

    if (c->flag.tracking && !c->flag.tracking_bcast) {
        trackingRememberKeys(c, c);
    }

    c->duration = 0;

    processClientIOWriteDone(c);

    if (!c->flag.close_asap) {
        commandProcessed(c);
    }
}

void handleCommandJobs(client **command_jobs, int command_count) {
    server.stat_io_commands_pending -= command_count;
    serverAssert(server.stat_io_commands_pending >= 0);

    /* Prefetch pass for cache efficiency */
    for (int i = 0; i < command_count; i++) {
        client *c = command_jobs[i];
        for (int j = 0; j < c->argc; j++) valkey_prefetch(c->argv[j]);
        prefetchSlotQueueInfo(c->slot);
    }

    /* Processing pass */
    for (int i = 0; i < command_count; i++) {
        client *c = command_jobs[i];
        int slot = c->slot;
        processClientIOCommandDone(c);
        slotQueueDecRef(slot);
        slotQueueDecRef(-1);
        server.stat_io_commands_processed++;
    }
}

/* Throttle control loop - adjusts offload_throttle_pct based on contention */
void updateOffloadingThrottle(void) {
    static long long last_update_time = 0;
    long long now = server.mstime;

    if (now - last_update_time < 100) return; /* Rate limit to 100ms */
    last_update_time = now;

    size_t attempts = server.stat_offload_attempts;
    size_t blocked = server.stat_offload_blocked;

    server.stat_offload_attempts = 0;
    server.stat_offload_blocked = 0;

    if (attempts == 0) return;

    double ratio = (double)blocked / attempts;

    if (ratio > CONTENTION_RATIO_THRESHOLD) {
        /* Multiplicative decrease */
        server.offload_throttle_pct /= 2;
        if (server.offload_throttle_pct < OFFLOAD_MIN_THROTTLE) {
            server.offload_throttle_pct = OFFLOAD_MIN_THROTTLE;
        }
        offload_suspended_by_contention = 1;
    } else if (ratio < RECOVERY_RATIO_THRESHOLD) {
        /* Additive increase */
        server.offload_throttle_pct += OFFLOAD_THROTTLE_STEP;
        if (server.offload_throttle_pct > 100) {
            server.offload_throttle_pct = 100;
        }
        offload_suspended_by_contention = 0;
    }
}

/* Check if saturation state should be updated */
static int shouldSkipSaturationUpdate(void) {
    static long long last_update_time = 0;
    if (server.active_io_threads_num <= 2) return 1;
    if (server.mstime - last_update_time < IO_SATURATION_CHECK_INTERVAL_MS) return 1;
    last_update_time = server.mstime;
    return 0;
}

/* Check saturation and adjust throttle */
void updateOffloadingSaturation(void) {
    if (shouldSkipSaturationUpdate()) return;

    int avg_io_pct = getAverageThreadStat(io_threads_stat_io_cpu, server.active_io_threads_num);
    int active = server.active_io_threads_num;

    /* Check Becoming Saturated */
    if (!server.io_threads_saturated && active == server.io_threads_num) {
        int avg_cmd_pct = getAverageThreadStat(io_threads_stat_cmd_cpu, active);
        int cmd_total = avg_cmd_pct * (active - 1);

        /* Only consider saturation if we aren't utilizing full core power on commands */
        if (cmd_total < FULL_CORE_UTILIZATION) {
            /* Heuristic: Estimate IO load if offloading were disabled */
            int adjusted_io_pct = (avg_io_pct * FULL_CORE_UTILIZATION) /
                                  (FULL_CORE_UTILIZATION - avg_cmd_pct);
            if (adjusted_io_pct > IO_SATURATION_LIMIT) {
                server.io_threads_saturated = 1;
            }
        }
    }

    /* Check No Longer Saturated */
    if (server.io_threads_saturated) {
        int avg_free_capacity = FULL_CORE_UTILIZATION - avg_io_pct - IO_HEADROOM_BUFFER;
        if (avg_free_capacity < 0) avg_free_capacity = 0;
        int total_free = (active - 1) * avg_free_capacity;

        if (avg_io_pct < IO_UNSATURATION_LIMIT || total_free > IO_TOTAL_FREE_THRESHOLD) {
            server.io_threads_saturated = 0;
        }
    }

    /* Apply Throttle Adjustments */
    if (server.io_threads_saturated) {
        server.offload_throttle_pct /= 2;
        if (server.offload_throttle_pct < OFFLOAD_MIN_THROTTLE) {
            server.offload_throttle_pct = OFFLOAD_MIN_THROTTLE;
        }
    } else if (!offload_suspended_by_contention) {
        server.offload_throttle_pct += OFFLOAD_THROTTLE_STEP;
        if (server.offload_throttle_pct > 100) {
            server.offload_throttle_pct = 100;
        }
    }
}
