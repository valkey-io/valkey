/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "io_threads.h"
#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"
#include "cluster_slot_stats.h"
#include "module.h"

static __thread int thread_id = 0; /* Thread local var */
static pthread_t io_threads[IO_THREADS_MAX_NUM] = {0};
static pthread_mutex_t io_threads_mutex[IO_THREADS_MAX_NUM];
static int cur_epoll_thread = 0;

/******************************************************************************************************
 * Multi-Producer Single-Consumer Queue Implementation
 * This queue allows multiple producer threads to safely enqueue items
 * that will be consumed by a single consumer thread. It's designed for
 * passing messages from IO-threads to the Main-thread.
 *******************************************************************************************************/

/* Multi-Producer Single-Consumer Queue structure
 * Cache line padding (64 bytes) is used to prevent false sharing between counter */
typedef struct IoToMTQueue {
    size_t capacity __attribute__((aligned(64)));                   /* Total queue capacity */
    _Atomic uint64_t producer_counter __attribute__((aligned(64))); /* Shared counter for producers */
    _Atomic uint64_t producer_limit __attribute__((aligned(64)));   /* Upper bound for producers */
    uint64_t consumer_counter __attribute__((aligned(64)));         /* Consumer position */
    volatile uint64_t entries[];                                    /* Flexible array of queue entries */
} IoToMTQueue;

typedef struct MPSCPendingResponse {
    uint64_t value;
    uint64_t counter;
} MPSCPendingResponse;

/* Global queue for completed I/O jobs */
static IoToMTQueue *io_to_mt_queue = NULL;

/* Thread-local queue for values that couldn't be immediately written to the MPSC queue */
static __thread list thread_pending_responses = {0};

/* Default queue size */
#define DEFAULT_MPSC_QUEUE_SIZE_PER_THREAD 1024

/* Creates a new MPSC queue with the specified capacity */
static IoToMTQueue *IoToMTQueueCreate(size_t capacity) {
    IoToMTQueue *queue = (IoToMTQueue *)zcalloc(sizeof(IoToMTQueue) + (sizeof(uint64_t) * capacity));
    queue->capacity = capacity;
    queue->producer_limit = capacity - 1; /* 0 based index */
    return queue;
}

/* Adds an item to the queue (producer operation)
 *
 * This function attempts to add a value to the IO-to-MT thread queue. If the queue
 * is full the value is stored in an unposted list to be tried again later.
 *
 * value - The main value to enqueue in the queue
 * counter - Position in the queue where the value should be written. If 0, a new
 *           position is allocated.
 *
 * Returns:
 *   1 - If the value was successfully added to the queue
 *   0 - If the queue was full and the value couldn't be added
 */
static int IoToMTQueueProduce(uint64_t value, uint64_t counter) {
    IoToMTQueue *q = io_to_mt_queue;
    int first_try = counter == 0;
    /* Get the next producer slot if no slot is given */
    if (first_try) {
        counter = atomic_fetch_add_explicit(&q->producer_counter, 1, memory_order_relaxed);
    }

    /* Calculate the actual index in the ring buffer */
    uint64_t index = counter % q->capacity;

    /* Try to write the value. */
    if (atomic_load_explicit(&q->producer_limit, memory_order_acquire) >= counter) {
        atomic_thread_fence(memory_order_release);
        q->entries[index] = value;
        return 1;
    }

    if (!first_try) return 0;
    /* If the queue is full, store the value to be written later, IO thread should never busy wait for the MT to avoid dead lock */
    MPSCPendingResponse *pending_response = zmalloc(sizeof(MPSCPendingResponse));
    pending_response->value = value;
    pending_response->counter = counter;
    listAddNodeTail(&thread_pending_responses, pending_response);
    return 0;
}

static void handleThreadPendingResponses(void) {
    if (listLength(&thread_pending_responses) == 0) return;
    listIter li;
    listNode *ln;

    listRewind(&thread_pending_responses, &li);
    while ((ln = listNext(&li))) {
        MPSCPendingResponse *pending_response = listNodeValue(ln);
        if (IoToMTQueueProduce(pending_response->value, pending_response->counter) == 0) break;
        listDelNode(&thread_pending_responses, ln);
    }
}

/* Consumes multiple items from the queue (consumer operation)
 * Returns Number of items actually consumed */
static int IoToMTQueueConsumeBatch(int max_items, uint64_t *values) {
    IoToMTQueue *q = io_to_mt_queue;
    int consumed_count = 0;

    /* Try to consume up to max_items */
    for (int i = 0; i < max_items; i++) {
        /* Get the current consumer position */
        size_t index = q->consumer_counter % q->capacity;

        uint64_t val = q->entries[index];

        if (!val) break;

        /* Store the consumed values in the output arrays */
        values[consumed_count] = val;
        consumed_count++;

        /* Clear the entry to mark it as consumed */
        q->entries[index] = 0;

        /* Move to the next position */
        q->consumer_counter++;
    }

    /* If we consumed any items, update the producer limit */
    if (consumed_count > 0) {
        /* Release so that the threads see the NULL assingments */
        atomic_store_explicit(&q->producer_limit, q->producer_limit + consumed_count, memory_order_release);
        /* Acqiure to get the latest thread changes */
        atomic_thread_fence(memory_order_acquire);
    }

    return consumed_count;
}
/* End of IO to MT MSPC queue functions */


/******************************************************************************************************
 * Single-Producer Single-Consumer Queue Implementation
 * This queue allows a single producer thread (main thread) to safely enqueue items
 * that will be consumed by a single consumer thread (IO thread).
 * The queue uses a ring buffer with head and tail pointers to track the producer
 * and consumer positions respectively, ensuring thread-safety through atomic operations.
 *******************************************************************************************************/
/* IO jobs queue functions - Used to send jobs from the main-thread to the IO thread. */
typedef struct iojob {
    job_handler handler;
    void *data;
} iojob;

typedef struct IOJobQueue {
    iojob *ring_buffer;
    size_t size;
    volatile bool pending_epoll_job;
    size_t submitted_jobs_count;                                   /* Number of jobs submitted, accessed by the main-thread only. */
    _Atomic size_t head __attribute__((aligned(CACHE_LINE_SIZE))); /* Next write index for producer (main-thread) */
    _Atomic size_t tail __attribute__((aligned(CACHE_LINE_SIZE))); /* Next read index for consumer  (IO-thread) */
} IOJobQueue;
IOJobQueue io_jobs[IO_THREADS_MAX_NUM] = {0};

/* Initialize the job queue with a specified number of items. */
static void IOJobQueue_init(IOJobQueue *jq, size_t item_count) {
    debugServerAssertWithInfo(NULL, NULL, inMainThread());
    jq->ring_buffer = zcalloc(item_count * sizeof(iojob));
    jq->size = item_count; /* Total number of items */
    jq->head = 0;
    jq->tail = 0;
    jq->submitted_jobs_count = 0;
    jq->pending_epoll_job = false;
}

/* Clean up the job queue and free allocated memory. */
static void IOJobQueue_cleanup(IOJobQueue *jq) {
    debugServerAssertWithInfo(NULL, NULL, inMainThread());
    zfree(jq->ring_buffer);
    memset(jq, 0, sizeof(*jq));
}

static int IOJobQueue_isFull(IOJobQueue *jq) {
    debugServerAssertWithInfo(NULL, NULL, inMainThread());

    /* Fast path: If submitted jobs are less than the queue size, the queue can't be full */
    if (jq->submitted_jobs_count < (jq->size - 1)) {
        return 0; /* Fast path Submitted jobs are less than the queue size, the queue can't be full. */
    }

    size_t current_head = atomic_load_explicit(&jq->head, memory_order_relaxed);
    /* We don't use memory_order_acquire for the tail due to performance reasons,
     * In the worst case we will just assume wrongly the buffer is full and the main thread will do the job by itself. */
    size_t current_tail = atomic_load_explicit(&jq->tail, memory_order_relaxed);
    size_t next_head = (current_head + 1) % jq->size;
    if (next_head == current_tail) {
        /* Queue is full */
        serverAssert(jq->submitted_jobs_count == jq->size - 1);
        return 1;
    } else {
        /* Queue is not full, update the submitted_jobs_count */
        size_t free_slots = (current_tail >= next_head) ? (current_tail - next_head) : (jq->size - (next_head - current_tail));
        jq->submitted_jobs_count = jq->size - free_slots - 1;
        return 0;
    }
}

/* Attempt to push a new job to the queue from the main thread.
 * the caller must ensure the queue is not full before calling this function. */
static void IOJobQueue_push(IOJobQueue *jq, job_handler handler, void *data) {
    debugServerAssertWithInfo(NULL, NULL, inMainThread());
    /* Assert the queue is not full - should not happen as the caller should check for it before. */
    serverAssert(!IOJobQueue_isFull(jq));

    /* No need to use atomic acquire for the head, as the main thread is the only one that writes to the head index. */
    size_t current_head = atomic_load_explicit(&jq->head, memory_order_relaxed);
    size_t next_head = (current_head + 1) % jq->size;

    /* We store directly the job's fields to avoid allocating a new iojob structure. */
    serverAssert(jq->ring_buffer[current_head].data == NULL);
    serverAssert(jq->ring_buffer[current_head].handler == NULL);
    jq->ring_buffer[current_head].data = data;
    jq->ring_buffer[current_head].handler = handler;

    /* memory_order_release to make sure the data is visible to the consumer (the IO thread). */
    atomic_store_explicit(&jq->head, next_head, memory_order_release);
    jq->submitted_jobs_count++;
}

/* Returns the number of jobs currently available for consumption in the given job queue.
 *
 * This function  ensures memory visibility for the jobs by
 * using a memory acquire fence when there are jobs available. */
static size_t IOJobQueue_availableJobs(const IOJobQueue *jq) {
    debugServerAssertWithInfo(NULL, NULL, !inMainThread());
    /* We use memory_order_acquire to make sure the head and the job's fields are visible to the consumer (IO thread). */
    size_t current_head = atomic_load_explicit(&jq->head, memory_order_acquire);
    size_t current_tail = atomic_load_explicit(&jq->tail, memory_order_relaxed);

    if (current_head >= current_tail) {
        return current_head - current_tail;
    } else {
        return jq->size - (current_tail - current_head);
    }
}

/* Checks if the job Queue is empty.
 * returns 1 if the buffer is currently empty, 0 otherwise.
 * Called by the main-thread only.
 * This function uses relaxed memory order, so the caller need to use an acquire
 * memory fence before calling this function to be sure it has the latest index
 * from the other thread, especially when called repeatedly. */
static int IOJobQueue_isEmpty(const IOJobQueue *jq) {
    size_t current_head = atomic_load_explicit(&jq->head, memory_order_relaxed);
    size_t current_tail = atomic_load_explicit(&jq->tail, memory_order_relaxed);
    return current_head == current_tail;
}

/* Removes the next job from the given job queue by advancing the tail index.
 * Called by the IO thread.
 * The caller must ensure that the queue is not empty before calling this function.
 * This function uses relaxed memory order, so the caller need to use an release memory fence
 * after calling this function to make sure the updated tail is visible to the producer (main thread). */
static void IOJobQueue_removeJob(IOJobQueue *jq) {
    debugServerAssertWithInfo(NULL, NULL, !inMainThread());
    size_t current_tail = atomic_load_explicit(&jq->tail, memory_order_relaxed);
    jq->ring_buffer[current_tail].data = NULL;
    jq->ring_buffer[current_tail].handler = NULL;
    atomic_store_explicit(&jq->tail, (current_tail + 1) % jq->size, memory_order_relaxed);
}

/* Retrieves the next job handler and data from the job queue without removal.
 * Called by the consumer (IO thread). Caller must ensure queue is not empty.*/
static void IOJobQueue_peek(const IOJobQueue *jq, job_handler *handler, void **data) {
    debugServerAssertWithInfo(NULL, NULL, !inMainThread());
    size_t current_tail = atomic_load_explicit(&jq->tail, memory_order_relaxed);
    iojob *job = &jq->ring_buffer[current_tail];
    *handler = job->handler;
    *data = job->data;
}

/* End of IO job queue functions */


/* *********************************************************************************************************************
 * Deferred queues are used to manage command execution ordering in multi-threaded environments.
 * They ensure proper synchronization between commands that access the same slot or require exclusive access.
 * Commands are queued when there are conflicting operations in progress and executed when it's safe to do so.
 *
 * A deferred queue is composed of two lists:
 * 1. A clients list - containing clients blocked waiting for command execution
 * 2. A jobs list - containing general jobs that need to be performed on a given slot or the whole DB without client context
 *
 * The system maintains a deferred queue per slot and a general deferred queue (deferredCmdExclusive) for exclusive
 * access to the whole database.
 **********************************************************************************************************************/
typedef struct deferredQueue {
    int refcount;
    int cur_tid;
    list *deferred_jobs;
    list *pending_clients;
} deferredQueue;

/* Global queues for deferred commands */
deferredQueue deferredCmdExclusive = {0};
deferredQueue slot_use_info[16384] = {0};

typedef struct delayedJob {
    job_handler handler;
    int slot;
    char data[];
} delayedJob;

/* Global thread-local storage for delayed jobs */
static __thread list *thread_delayed_jobs = NULL;

/*
 * executionContext
 * -1: CTX_NONE (no execution context)
 * -2: CTX_EXCLUSIVE (exclusive access to the whole database)
 * >= 0: slot number (indicating we're operating on a specific slot)
 */
#define CTX_NONE -1
#define CTX_EXCLUSIVE -2

/* Global context tracking for deferred queue operations - mainthread only */
static int dq_context = CTX_NONE;

static deferredQueue *getDeferredQueue(int slot) {
    if (slot == -1) {
        return &deferredCmdExclusive;
    } else {
        return &slot_use_info[slot];
    }
}

static int isClientListEmpty(deferredQueue *queue) {
    return queue->pending_clients == NULL || listLength(queue->pending_clients) == 0;
}

static int isJobListEmpty(deferredQueue *queue) {
    return queue->deferred_jobs == NULL || listLength(queue->deferred_jobs) == 0;
}

static int isDqEmpty(deferredQueue *queue) {
    return isJobListEmpty(queue) && isClientListEmpty(queue);
}

/* Check if a deferred queue is available for immediate processing */
static int dqAvailable(deferredQueue *queue) {
    return queue->refcount == 0;
}

/* Returns the thread id handling the given slot, -1 if no thread is handling it */
static int getSlotTid(int slot) {
    if (dqAvailable(getDeferredQueue(slot))) return -1;
    return getDeferredQueue(slot)->cur_tid;
}

static void setSlotTid(int slot, int tid) {
    getDeferredQueue(slot)->cur_tid = tid;
}

/* Increment the reference count of a deferred queue */
static void dqIncr(deferredQueue *queue) {
    queue->refcount++;
}

/* Create a new job with the given handler and data */
static listNode *createJobNode(int slot, job_handler handler, size_t data_size, void *data) {
    /* Allocate memory for job structure plus data using flexible array member */
    listNode *node = zmalloc(sizeof(listNode) + sizeof(delayedJob) + data_size);
    delayedJob *job = (delayedJob *)(node + 1);
    job->slot = slot;
    job->handler = handler;
    if (data_size) {
        memcpy(job->data, data, data_size); /* Copy data to the flexible array member */
    }
    node->value = job;

    return node;
}

/* Process a job immediately or add it to queue based on refcount */
static void processOrAddJob(deferredQueue *q, listNode *jobNode) {
    if (q->refcount == 0) {
        delayedJob *job = listNodeValue(jobNode);
        job->handler(job->data);
        zfree(jobNode);
    } else {
        if (q->deferred_jobs == NULL) {
            q->deferred_jobs = listCreate();
        }
        listLinkNodeTail(q->deferred_jobs, jobNode);
    }
}

/* Returns whether the given command requires exclusive access to the whole database. */
static int isDBExclusiveCmd(struct serverCommand *cmd, int slot) {
    /* If no slot is specified but the client command changes the keyspace, we assume it is an exclusive command */
    if (slot == -1 && (cmd->flags & CMD_WRITE)) return 1;
    /* The exec command can contain commands that may affect the whole database */
    if (cmd->proc == execCommand) return 1;
    /* If no mandatory keys are specified, we can't determine which slot will be accessed */
    if (cmd->flags & CMD_NO_MANDATORY_KEYS) return 1;
    /* Any Admin level command needs full exclusivity as it impacts system-wide behaviour */
    if (cmd->flags & CMD_ADMIN) return 1;
    return 0;
}

/* Returns if the given command requires exclusive access to the given slot. */
static int isSlotExclusiveCmd(struct serverCommand *cmd, int slot) {
    /* No exclusivity required */
    if (cmd->flags & CMD_CAN_BE_OFFLOADED) return 0;

    /* Not slot exclusive rather DB exclusive */
    if (isDBExclusiveCmd(cmd, slot)) return 0;

    return 1;
}

/* Process all jobs in a deferred jobs list and reset the list to empty */
static void processDeferredJobsList(deferredQueue *queue) {
    if (!queue->deferred_jobs) return;

    listIter li;
    listNode *ln;
    listRewind(queue->deferred_jobs, &li);

    while ((ln = listNext(&li))) {
        delayedJob *job = listNodeValue(ln);
        listUnlinkNode(queue->deferred_jobs, ln);
        job->handler(job->data);
        zfree(ln);
    }

    if (queue != &deferredCmdExclusive) {
        listRelease(queue->deferred_jobs);
        queue->deferred_jobs = NULL;
    }
}

/* This function handles clients that were previously deferred for command processing. */
static void dqProcessPendingClients(int slot) {
    deferredQueue *queue = getDeferredQueue(slot);
    /* Set the queue context based on queue type */
    dq_context = (queue == &deferredCmdExclusive) ? CTX_EXCLUSIVE : slot;

    /* Process only a fixed number of clients to avoid infinite iteration.
     * Clients may be added back to the list during processing. */
    size_t len = listLength(queue->pending_clients);
    listNode *ln;

    /* Process clients from the pending list.
     * We need to check if the list still exists during each iteration
     * because commands like CLIENT KILL may remove clients mid-iteration. */
    while (len-- && queue->pending_clients && (ln = listFirst(queue->pending_clients))) {
        client *c = listNodeValue(ln);

        /* Check if we need to wait due to exclusive commands */
        if (queue->refcount) {
            if (dq_context == CTX_EXCLUSIVE) {
                if (isDBExclusiveCmd(c->cmd, c->slot)) break;
            } else {
                if (isSlotExclusiveCmd(c->cmd, c->slot)) break;
            }
        }

        /* Remove client from pending list and mark as unblocked */
        listUnlinkNode(queue->pending_clients, ln);
        c->bstate->slot_pending_list = NULL;
        c->flag.blocked = 0;
        server.stat_io_threaded_clients_blocked_on_slot--;

        if (processPendingCommandAndInputBuffer(c) != C_ERR) {
            beforeNextClient(c);
        }
    }

    /* Clean up the client list if it's empty */
    if (queue != &deferredCmdExclusive && queue->pending_clients && listLength(queue->pending_clients) == 0) {
        listRelease(queue->pending_clients);
        queue->pending_clients = NULL;
    }

    /* Reset the queue context */
    dq_context = CTX_NONE;
}

/* Decrement the reference count and process pending jobs if it reaches zero */
static void dqDecr(int slot) {
    deferredQueue *queue = getDeferredQueue(slot);
    serverAssert(queue->refcount > 0);
    queue->refcount--;
    if (queue->refcount != 0) return;

    /* Process any pending jobs when refcount reaches zero */
    if (queue->deferred_jobs && listLength(queue->deferred_jobs)) {
        processDeferredJobsList(queue);
    }

    /* Process any pending clients */
    if (queue->pending_clients && listLength(queue->pending_clients)) {
        dqProcessPendingClients(slot);
    }
}

/* Add a client to the pending clients list of a deferred queue */
static void dqAddPendingClient(deferredQueue *queue, client *c) {
    /* Create the pending clients list if it doesn't exist */
    if (isClientListEmpty(queue)) {
        queue->pending_clients = listCreate();
    }

    /* Add client to the list and mark the client as blocked */
    initClientBlockingState(c);
    listLinkNodeTail(queue->pending_clients, &c->bstate->pending_client_node);
    c->flag.pending_command = 1;
    c->bstate->slot_pending_list = queue;
    c->bstate->btype = BLOCKED_SLOT;
    c->flag.blocked = 1;
    server.stat_io_threaded_clients_blocked_on_slot++;
    server.stat_io_threaded_clients_blocked_total++;
}

/* Remove a client from the pending clients list of a deferred queue */
static void dqRemoveClient(deferredQueue *queue, client *c) {
    /* Remove client from the list if it exists */
    serverAssert(!isClientListEmpty(queue));
    listUnlinkNode(queue->pending_clients, &c->bstate->pending_client_node);

    /* Clean up empty client list */
    if (queue != &deferredCmdExclusive && listLength(queue->pending_clients) == 0) {
        listRelease(queue->pending_clients);
        queue->pending_clients = NULL;
    }

    /* Reset client state */
    c->flag.pending_command = 0;
    c->flag.blocked = 0;
    c->bstate->slot_pending_list = NULL;
    server.stat_io_threaded_clients_blocked_on_slot--;
}

static void delayedServerCron(void *data) {
    UNUSED(data);
    long long interval = serverCron(server.el, 0, NULL);
    aeCreateTimeEvent(server.el, interval, serverCron, NULL, NULL);
}

/* Add a delayed job to the thread-local job list */
void threadAddDelayedJob(int slot, job_handler handler, size_t data_size, void *data) {
    /* Allocate memory for job structure plus data using flexible array member */
    listNode *job_node = createJobNode(slot, handler, data_size, data);
    listLinkNodeTail(thread_delayed_jobs, job_node);
}

int isServerCronDelayed(void) {
    if (!server.cluster_enabled || server.io_threads_num == 1) {
        return 0;
    }

    if (dqAvailable(&deferredCmdExclusive)) return 0;

    listNode *job_node = createJobNode(-1, delayedServerCron, 0, NULL);
    listLinkNodeTail(deferredCmdExclusive.deferred_jobs, job_node);
    return 1;
}

/* Dispatch delayed jobs based on their type */
static void dispatchThreadDeferredJobs(list *jobs_list) {
    listIter li;
    listNode *ln;
    listRewind(jobs_list, &li);

    while ((ln = listNext(&li))) {
        delayedJob *job = listNodeValue(ln);
        if (job->slot == -1) {
            job->handler(job->data);
            listDelNode(jobs_list, ln);
        } else {
            listUnlinkNode(jobs_list, ln);
            processOrAddJob(getDeferredQueue(job->slot), ln);
        }
        server.stat_delayed_jobs_processed++;
    }

    listRelease(jobs_list);
}

/* This function checks various conditions to ensure thread safety when processing commands
 * returns 1 if the command can be safely processed, 0 if not, in which case the command is queued to be process later */
int postponeClientCommand(client *c) {
    if (!server.cluster_enabled || server.io_threads_num == 1) {
        return 1;
    }

    /* An exclusive command can be processed either when processing the exclusive deferered queue
     * or in immediate mode if there are no read commands executed in queues*/
    if (isDBExclusiveCmd(c->cmd, c->slot)) {
        if (dq_context == CTX_EXCLUSIVE) return 1;

        if (dqAvailable(&deferredCmdExclusive)) return 1;

        /* We can't execute the command */
        dqAddPendingClient(&deferredCmdExclusive, c);
        return 0;
    }

    if (c->slot == -1) return 1; /* Can process non exclusive command without slot */

    if (dq_context == c->slot) return 1; /* Already in slot context */

    /* Immediate non exclusive commands are queued whenenever there are exclusive commands waiting */
    if (dq_context == CTX_NONE && !isDqEmpty(&deferredCmdExclusive)) {
        dqAddPendingClient(&deferredCmdExclusive, c);
        return 0;
    }

    /* Queue client if there are pending commands for this slot */
    deferredQueue *q = getDeferredQueue(c->slot);
    if (!isDqEmpty(q)) {
        dqAddPendingClient(q, c);
        return 0;
    }

    /* Queue write commands if there are active reads */
    if (isSlotExclusiveCmd(c->cmd, c->slot) && !dqAvailable(q)) {
        dqAddPendingClient(q, c);
        return 0;
    }

    return 1; /* No pending commands for the slot, can process immediately */
}

static void prefetchSlotPendingInfo(int slot) {
    __builtin_prefetch(slot_use_info + slot);
}

/* End of IO deferred queue functions */

int inMainThread(void) {
    return thread_id == 0;
}

/* Drains the I/O threads queue by waiting for all jobs to be processed.
 * This function must be called from the main thread. */
void drainIOThreadsQueue(void) {
    serverAssert(inMainThread());
    for (int i = 1; i < IO_THREADS_MAX_NUM; i++) { /* No need to drain thread 0, which is the main thread. */
        IOJobQueue *jq = &io_jobs[i];
        while (!IOJobQueue_isEmpty(jq) || jq->pending_epoll_job) {
            /* memory barrier acquire to get the latest job queue state */
            atomic_thread_fence(memory_order_acquire);
        }
    }
}

/* Returns if there is an IO operation in progress for the given client. */
int clientIOInProgress(client *c) {
    return c->io_read_state != CLIENT_IDLE || c->io_write_state != CLIENT_IDLE || c->io_command_state != CLIENT_IDLE;
}

/* Wait until the IO-thread is done with the client */
void waitForClientIO(client *c) {
    /* No need to wait if the client was not offloaded to the IO thread. */
    if (c->io_read_state == CLIENT_IDLE && c->io_write_state == CLIENT_IDLE && c->io_command_state == CLIENT_IDLE) {
        return;
    }

    /* Wait for read operation to complete if pending. */
    while (c->io_read_state == CLIENT_PENDING_IO) {
        atomic_thread_fence(memory_order_acquire);
    }

    /* Wait for write operation to complete if pending. */
    while (c->io_write_state == CLIENT_PENDING_IO) {
        atomic_thread_fence(memory_order_acquire);
    }

    /* Wait for command operation to complete if pending. */
    while (c->io_command_state == CLIENT_PENDING_IO) {
        atomic_thread_fence(memory_order_acquire);
    }

    /* Final memory barrier to ensure all changes are visible */
    atomic_thread_fence(memory_order_acquire);
}

static int getPendingIOThreadsJobs(void) {
    return server.stat_io_writes_pending + server.stat_io_reads_pending + server.stat_io_commands_pending;
}

/** Adjusts the number of active I/O threads based on the current event load.
 * If increase_only is non-zero, only allows increasing the number of threads.*/
void adjustIOThreadsByEventLoad(int numevents, int increase_only) {
    if (server.io_threads_num == 1) return; /* All I/O is being done by the main thread. */
    debugServerAssertWithInfo(NULL, NULL, server.io_threads_num > 1);

    int target_threads = 0;
    if (server.events_per_io_thread == 0) {
        /* When events_per_io_thread is set to 0, we offload all events to the IO threads.
         * This is used mainly for testing purposes. */
        if (getPendingIOThreadsJobs() > 0) {
            target_threads = server.io_threads_num;
        } else {
            target_threads = numevents + 1;
        }
    } else {
        target_threads = numevents / server.events_per_io_thread;
    }

    target_threads = max(1, min(target_threads, server.io_threads_num));

    if (target_threads == server.active_io_threads_num) return;

    if (target_threads < server.active_io_threads_num) {
        if (increase_only) return;

        int threads_to_deactivate_num = server.active_io_threads_num - target_threads;
        for (int i = 0; i < threads_to_deactivate_num; i++) {
            int tid = server.active_io_threads_num - 1;
            IOJobQueue *jq = &io_jobs[tid];
            /* We can't lock the thread if it may have pending jobs */
            if (!IOJobQueue_isEmpty(jq) || jq->pending_epoll_job) return;
            pthread_mutex_lock(&io_threads_mutex[tid]);
            server.active_io_threads_num--;
        }
    } else {
        int threads_to_activate_num = target_threads - server.active_io_threads_num;
        for (int i = 0; i < threads_to_activate_num; i++) {
            pthread_mutex_unlock(&io_threads_mutex[server.active_io_threads_num]);
            server.active_io_threads_num++;
        }
    }
}

/* This function performs polling on the given event loop and updates the server's
 * IO fired events count and poll state. */
static void IOThreadPoll(IOJobQueue *jq) {
    atomic_thread_fence(memory_order_acquire); /* Acquire the updated epoll struct */
    serverAssert(server.io_poll_state == AE_IO_STATE_POLL);

    struct timeval tvp = {0, 0};
    int num_events = aePoll(server.el, &tvp);

    server.io_ae_fired_events = num_events;
    jq->pending_epoll_job = false;
    atomic_store_explicit(&server.io_poll_state, AE_IO_STATE_DONE, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}

/* Define a cleanup function that will clean all thread resources */
void cleanupThreadResources(void *dummy) {
    UNUSED(dummy);

    handleThreadPendingResponses();

    /* Free the shared query buffer */
    freeSharedQueryBuf();

    /* Free the delayed jobs list if it exists */
    if (thread_delayed_jobs) {
        listRelease(thread_delayed_jobs);
        thread_delayed_jobs = NULL;
    }

    /* Clean any other thread-specific resources here */
    /* Reset thread state variables */
    setCurrentClient(NULL);
    setExecutingClient(NULL);
}

static void *IOThreadMain(void *myid) {
    /* The ID is the thread ID number (from 1 to server.io_threads_num-1). ID 0 is the main thread. */
    long id = (long)myid;
    char thdname[32];

    snprintf(thdname, sizeof(thdname), "io_thd_%ld", id);
    valkey_set_thread_title(thdname);
    serverSetCpuAffinity(server.server_cpulist);
    initSharedQueryBuf();
    setCurrentClient(NULL);
    setExecutingClient(NULL);
    thread_delayed_jobs = listCreate();
    pthread_cleanup_push(cleanupThreadResources, NULL);

    thread_id = (int)id;
    size_t jobs_to_process = 0;
    IOJobQueue *jq = &io_jobs[id];
    while (1) {
        /* Cancellation point so that pthread_cancel() from main thread is honored. */
        pthread_testcancel();

        /* Handle unposted responses if exist*/
        handleThreadPendingResponses();

        /* Wait for jobs */
        for (int j = 0; j < 1000000; j++) {
            jobs_to_process = IOJobQueue_availableJobs(jq);
            if (jobs_to_process || jq->pending_epoll_job) break;
        }

        if (jq->pending_epoll_job) {
            IOThreadPoll(jq);
        }

        /* Give the main thread a chance to stop this thread. */
        if (jobs_to_process == 0 && listLength(&thread_pending_responses) == 0) {
            pthread_mutex_lock(&io_threads_mutex[id]);
            pthread_mutex_unlock(&io_threads_mutex[id]);
            continue;
        }

        for (size_t j = 0; j < jobs_to_process; j++) {
            job_handler handler;
            void *data;
            /* We keep the job in the queue until it's processed. This ensures that if the main thread checks
             * and finds the queue empty, it can be certain that the IO thread is not currently handling any job. */
            IOJobQueue_peek(jq, &handler, &data);
            handler(data);
            /* Remove the job after it was processed */
            IOJobQueue_removeJob(jq);
            if (jq->pending_epoll_job) {
                IOThreadPoll(jq);
            }
        }

        /* Memory barrier to make sure the main thread sees the updated tail index.
         * We do it once per loop and not per tail-update for optimization reasons.
         * As the main-thread main concern is to check if the queue is empty, it's enough to do it once at the end. */
        atomic_thread_fence(memory_order_release);
    }
    pthread_cleanup_pop(0);
    return NULL;
}

#define IO_JOB_QUEUE_SIZE 2048
static void createIOThread(int id) {
    serverAssert(server.io_threads_num > 0);
    serverAssert(id > 0 && id < server.io_threads_num);

    pthread_t tid;
    pthread_mutex_init(&io_threads_mutex[id], NULL);
    IOJobQueue_init(&io_jobs[id], IO_JOB_QUEUE_SIZE);
    pthread_mutex_lock(&io_threads_mutex[id]); /* Thread will be stopped. */
    if (pthread_create(&tid, NULL, IOThreadMain, (void *)(long)id) != 0) {
        serverLog(LL_WARNING, "Fatal: Can't initialize IO thread, pthread_create failed with: %s", strerror(errno));
        exit(1);
    }
    io_threads[id] = tid;
}

/* Terminates the IO thread specified by id.
 * Called on server shutdown */
static void shutdownIOThread(int id) {
    int err;
    pthread_t tid = io_threads[id];
    if (tid == pthread_self()) return;
    if (tid == 0) return;

    pthread_cancel(tid);

    if ((err = pthread_join(tid, NULL)) != 0) {
        serverLog(LL_WARNING, "IO thread(tid:%lu) can not be joined: %s", (unsigned long)tid, strerror(err));
    } else {
        serverLog(LL_NOTICE, "IO thread(tid:%lu) terminated", (unsigned long)tid);
    }
    pthread_mutex_destroy(&io_threads_mutex[id]);
    IOJobQueue_cleanup(&io_jobs[id]);
}

void killIOThreads(void) {
    for (int j = 1; j < server.io_threads_num; j++) { /* We don't kill thread 0, which is the main thread. */
        shutdownIOThread(j);
    }
}

int updateIOThreads(const char **err) {
    serverAssert(inMainThread());
    UNUSED(err);
    int prev_threads_num = 1;
    for (int i = IO_THREADS_MAX_NUM - 1; i > 0; i--) {
        if (io_threads[i]) {
            prev_threads_num = i + 1;
            break;
        }
    }
    if (prev_threads_num == server.io_threads_num) return 1;

    serverLog(LL_NOTICE, "Changing number of IO threads from %d to %d.", prev_threads_num, server.io_threads_num);
    drainIOThreadsQueue();
    /* Set active threads to 1, will be adjusted based on workload later. */
    for (int i = 1; i < server.active_io_threads_num; i++) {
        pthread_mutex_lock(&io_threads_mutex[i]);
    }
    server.active_io_threads_num = 1;

    // Create new threads.
    if (server.io_threads_num > prev_threads_num) {
        prefetchCommandsBatchInit();
        for (int i = prev_threads_num; i < server.io_threads_num; i++) {
            createIOThread(i);
        }
    }
    // Decrease the number of threads.
    else {
        for (int i = prev_threads_num - 1; i >= server.io_threads_num; i--) {
            // Unblock inactive thread.
            pthread_mutex_unlock(&io_threads_mutex[i]);
            shutdownIOThread(i);
            io_threads[i] = 0;
        }
    }
    return 1;
}

/* Initialize the data structures needed for I/O threads. */
void initIOThreads(void) {
    server.active_io_threads_num = 1; /* We start with threads not active. */
    server.io_poll_state = AE_IO_STATE_NONE;
    server.io_ae_fired_events = 0;

    /* Don't spawn any thread if the user selected a single thread:
     * we'll handle I/O directly from the main thread. */
    if (server.io_threads_num == 1) return;

    serverAssert(server.io_threads_num <= IO_THREADS_MAX_NUM);

    prefetchCommandsBatchInit();
    size_t io_to_mt_queue_size = (server.io_threads_num - 1) * DEFAULT_MPSC_QUEUE_SIZE_PER_THREAD;
    io_to_mt_queue = IoToMTQueueCreate(io_to_mt_queue_size);
    thread_delayed_jobs = listCreate();
    deferredCmdExclusive.pending_clients = listCreate();
    deferredCmdExclusive.deferred_jobs = listCreate();

    /* Spawn and initialize the I/O threads. */
    for (int i = 1; i < server.io_threads_num; i++) {
        createIOThread(i);
    }
}

/*
 * This function is called when a client is closed but still has pending IO jobs.
 * It tracks the client in a dictionary in order to know to ignore its pending jobs.
 */
void ioThreadsOnUnlinkClient(client *c) {
    if (c->bstate && c->bstate->slot_pending_list) {
        dqRemoveClient(c->bstate->slot_pending_list, c);
        c->bstate->slot_pending_list = NULL;
    }
}

/*  returns C_OK if the command is postpone due to busy slot */
static int isCommandPostpone(client *c) {
    deferredQueue *q = getDeferredQueue(c->slot);
    if (!dqAvailable(q)) {
        dqAddPendingClient(q, c);
        return C_OK; /* Postpone the command execution */
    }
    return C_ERR;
}

int trySendProcessCommandToIOThreads(client *c) {
    if (server.active_io_threads_num == 1) {
        return C_ERR; /* No IO threads to offload to. */
    }

    if (!server.io_threads_do_commands_offloading) {
        return C_ERR; /* Command offloading is disabled. */
    }

    /* Check if modules are loaded and module offloading is disabled */
    if (moduleCount() > 0 && !server.io_threads_do_commands_offloading_with_modules) {
        return C_ERR; /* Modules are loaded and module command offloading is disabled. */
    }

    if (!(c->cmd->flags & CMD_CAN_BE_OFFLOADED)) {
        return C_ERR;
    }

    if (!server.cluster_enabled) {
        return C_ERR; /* Avoid offloading commands in non cluster mode. */
    }

    if (server.notify_keyspace_events & NOTIFY_KEY_MISS) {
        return C_ERR; /* Avoid offloading commands when NOTIFY_KEY_MISS is enabled. */
    }

    if (c->io_read_state != CLIENT_IDLE || c->io_command_state != CLIENT_IDLE || c->io_write_state != CLIENT_IDLE) {
        /* isCommandPostpone returns C_OK if the client should be postponed and will be offloaded later */
        return isCommandPostpone(c);
    }

    /* Do not offload if the client uses pipeline commands */
    if (c->querybuf != NULL && sdslen(c->querybuf) > c->qb_pos) {
        return isCommandPostpone(c);
    }

    /* Do not offload if it is possible the main-thread will write at the same time to the client's COB */
    if (getClientType(c) != CLIENT_TYPE_NORMAL) {
        return isCommandPostpone(c);
    }

    serverAssert(c->slot != -1);

    /* Find the IO thread that is responsible for the slot. */
    int tid = getSlotTid(c->slot);
    if (tid == -1 || tid >= server.active_io_threads_num) {
        tid = (c->slot % (server.active_io_threads_num - 1)) + 1;
        setSlotTid(c->slot, tid);
    }

    IOJobQueue *jq = &io_jobs[tid];
    if (IOJobQueue_isFull(jq)) return isCommandPostpone(c);

    c->io_command_state = CLIENT_PENDING_IO;
    c->io_write_state = CLIENT_PENDING_IO; /* The thread may write the command's result */
    dqIncr(getDeferredQueue(c->slot));
    dqIncr(&deferredCmdExclusive);
    /* Setting current client to NULL to avoid accessing it after it was sent to IO */
    setCurrentClient(NULL);
    setExecutingClient(NULL);
    IOJobQueue_push(&io_jobs[tid], ioThreadProcessCommand, c);

    server.stat_io_commands_pending++;
    return C_OK;
}

int trySendReadToIOThreads(client *c) {
    if (server.active_io_threads_num <= 1) return C_ERR;
    /* If IO thread is already reading, return C_OK to make sure the main thread will not handle it. */
    if (c->io_read_state != CLIENT_IDLE) return C_OK;
    if (c->io_write_state != CLIENT_IDLE) return C_OK;
    /* For simplicity, don't offload replica clients reads as read traffic from replica is negligible */
    if (getClientType(c) == CLIENT_TYPE_REPLICA) return C_ERR;
    /* With Lua debug client we may call connWrite directly in the main thread */
    if (c->flag.lua_debug) return C_ERR;
    /* For simplicity let the main-thread handle the blocked clients */
    if (c->flag.blocked || c->flag.unblocked) return C_ERR;
    if (c->flag.close_asap) return C_ERR;
    size_t tid = (c->id % (server.active_io_threads_num - 1)) + 1;

    IOJobQueue *jq = &io_jobs[tid];
    if (IOJobQueue_isFull(jq)) return C_ERR;

    c->cur_tid = tid;
    c->read_flags = canParseCommand(c) ? 0 : READ_FLAGS_DONT_PARSE;
    c->read_flags |= authRequired(c) ? READ_FLAGS_AUTH_REQUIRED : 0;
    c->read_flags |= c->flag.primary ? READ_FLAGS_PRIMARY : 0;

    c->io_read_state = CLIENT_PENDING_IO;
    connSetPostponeUpdateState(c->conn, 1);
    IOJobQueue_push(jq, ioThreadReadQueryFromClient, c);
    server.stat_io_reads_pending++;
    c->flag.pending_read = 1;
    return C_OK;
}

/* This function attempts to offload the client's write to an I/O thread.
 * Returns C_OK if the client's writes were successfully offloaded to an I/O thread,
 * or C_ERR if the client is not eligible for offloading. */
int trySendWriteToIOThreads(client *c) {
    if (server.active_io_threads_num <= 1) return C_ERR;
    /* The I/O thread is already writing for this client. */
    if (c->io_write_state != CLIENT_IDLE) return C_OK;
    /* Nothing to write */
    if (!clientHasPendingReplies(c)) return C_ERR;
    /* For simplicity, avoid offloading non-online replicas */
    if (getClientType(c) == CLIENT_TYPE_REPLICA && c->repl_data->repl_state != REPLICA_STATE_ONLINE) return C_ERR;
    /* We can't offload debugged clients as the main-thread may read at the same time  */
    if (c->flag.lua_debug) return C_ERR;

    size_t tid = (c->id % (server.active_io_threads_num - 1)) + 1;
    /* Handle case where client has a pending IO read job on a different thread:
     * 1. A read job is still pending (io_read_state == CLIENT_PENDING_IO)
     * 2. The pending job is on a different thread (c->cur_tid != tid)
     *
     * This situation can occur if active_io_threads_num increased since the
     * original job assignment. In this case, we keep the job on its current
     * thread to ensure the same thread handles the client's I/O operations. */
    if (c->io_read_state == CLIENT_PENDING_IO && c->cur_tid != (uint8_t)tid) tid = c->cur_tid;

    IOJobQueue *jq = &io_jobs[tid];
    if (IOJobQueue_isFull(jq)) return C_ERR;

    c->cur_tid = tid;
    if (c->flag.pending_write) {
        listUnlinkNode(server.clients_pending_write, &c->clients_pending_write_node);
        c->flag.pending_write = 0;
    }

    int is_replica = getClientType(c) == CLIENT_TYPE_REPLICA;
    if (is_replica) {
        c->io_last_reply_block = listLast(server.repl_buffer_blocks);
        replBufBlock *o = listNodeValue(c->io_last_reply_block);
        c->io_last_bufpos = o->used;
    } else {
        /* Save the last block of the reply list to io_last_reply_block and the used
         * position to io_last_bufpos. The I/O thread will write only up to
         * io_last_bufpos, regardless of the c->bufpos value. This is to prevent I/O
         * threads from reading data that might be invalid in their local CPU cache. */
        c->io_last_reply_block = listLast(c->reply);
        if (c->io_last_reply_block) {
            clientReplyBlock *block = (clientReplyBlock *)listNodeValue(c->io_last_reply_block);
            c->io_last_bufpos = block->used;
            /* If buffer is encoded force new header */
            if (block->flag.buf_encoded) block->last_header = NULL;
        } else {
            c->io_last_bufpos = (size_t)c->bufpos;
            /* If buffer is encoded force new header */
            if (c->flag.buf_encoded) c->last_header = NULL;
        }
    }

    serverAssert(c->bufpos > 0 || c->io_last_bufpos > 0 || is_replica);

    /* The main-thread will update the client state after the I/O thread completes the write. */
    connSetPostponeUpdateState(c->conn, 1);
    c->write_flags = is_replica ? WRITE_FLAGS_IS_REPLICA : 0;
    c->io_write_state = CLIENT_PENDING_IO;

    IOJobQueue_push(jq, ioThreadWriteToClient, c);
    server.stat_io_writes_pending++;
    return C_OK;
}

/* Internal function to free the client's argv in an IO thread. */
static void IOThreadFreeArgv(void *data) {
    robj **argv = (robj **)data;
    int last_arg = 0;
    for (int i = 0;; i++) {
        robj *o = argv[i];
        if (o == NULL) {
            continue;
        }

        /* The main-thread set the refcount to 0 to indicate that this is the last argument to free */
        if (o->refcount == 0) {
            last_arg = 1;
            o->refcount = 1;
        }

        decrRefCount(o);

        if (last_arg) {
            break;
        }
    }

    zfree(argv);
}

/* This function attempts to offload the client's argv to an IO thread.
 * Returns C_OK if the client's argv were successfully offloaded to an IO thread,
 * C_ERR otherwise. */
int tryOffloadFreeArgvToIOThreads(client *c, int argc, robj **argv) {
    if (server.active_io_threads_num <= 1 || argc == 0) {
        return C_ERR;
    }

    size_t tid = (c->id % (server.active_io_threads_num - 1)) + 1;

    IOJobQueue *jq = &io_jobs[tid];
    if (IOJobQueue_isFull(jq)) {
        return C_ERR;
    }

    int last_arg_to_free = -1;

    /* Prepare the argv */
    for (int j = 0; j < argc; j++) {
        if (argv[j]->refcount > 1) {
            decrRefCount(argv[j]);
            /* Set argv[j] to NULL to avoid double free */
            argv[j] = NULL;
        } else {
            last_arg_to_free = j;
        }
    }

    /* If no argv to free, free the argv array at the main thread */
    if (last_arg_to_free == -1) {
        zfree(argv);
        return C_OK;
    }

    /* We set the refcount of the last arg to free to 0 to indicate that
     * this is the last argument to free. With this approach, we don't need to
     * send the argc to the IO thread and we can send just the argv ptr. */
    argv[last_arg_to_free]->refcount = 0;

    /* Must succeed as we checked the free space before. */
    IOJobQueue_push(jq, IOThreadFreeArgv, argv);
    c->argv = NULL;
    c->argc = 0;

    return C_OK;
}

/* This function attempts to offload the free of an object to an IO thread.
 * Returns C_OK if the object was successfully offloaded to an IO thread,
 * C_ERR otherwise.*/
int tryOffloadFreeObjToIOThreads(robj *obj) {
    if (server.active_io_threads_num <= 1) {
        return C_ERR;
    }

    if (obj->refcount > 1) return C_ERR;

    if (obj->encoding != OBJ_ENCODING_RAW || obj->type != OBJ_STRING) return C_ERR;

    /* We select the thread ID in a round-robin fashion. */
    size_t tid = (server.stat_io_freed_objects % (server.active_io_threads_num - 1)) + 1;

    IOJobQueue *jq = &io_jobs[tid];
    if (IOJobQueue_isFull(jq)) {
        return C_ERR;
    }

    /* We offload only the free of the ptr that may be allocated by the I/O thread.
     * The object itself was allocated by the main thread and will be freed by the main thread. */
    IOJobQueue_push(jq, sdsfreeVoid, obj->ptr);
    obj->ptr = NULL;
    decrRefCount(obj);

    server.stat_io_freed_objects++;
    return C_OK;
}

/* This function retrieves the results of the IO Thread poll.
 * returns the number of fired events if the IO thread has finished processing poll events, 0 otherwise. */
static int getIOThreadPollResults(aeEventLoop *eventLoop) {
    int io_state = atomic_load_explicit(&server.io_poll_state, memory_order_acquire);
    if (io_state == AE_IO_STATE_POLL) {
        /* IO thread is still processing poll events. */
        return 0;
    }

    /* IO thread is done processing poll events. */
    serverAssert(io_state == AE_IO_STATE_DONE);
    server.stat_poll_processed_by_io_threads++;
    server.io_poll_state = AE_IO_STATE_NONE;

    /* Remove the custom poll proc. */
    aeSetCustomPollProc(eventLoop, NULL);
    aeSetPollProtect(eventLoop, 0);
    return server.io_ae_fired_events;
}

void trySendPollJobToIOThreads(void) {
    if (server.active_io_threads_num <= 1) {
        return;
    }

    /* If there are no pending jobs, let the main thread do the poll-wait by itself. */
    if (getPendingIOThreadsJobs() == 0) {
        return;
    }

    /* If the IO thread is already processing poll events, don't send another job. */
    if (server.io_poll_state != AE_IO_STATE_NONE) {
        return;
    }

    cur_epoll_thread = ((cur_epoll_thread) % (server.active_io_threads_num - 1)) + 1;
    IOJobQueue *jq = &io_jobs[cur_epoll_thread];
    aeSetCustomPollProc(server.el, getIOThreadPollResults);
    aeSetPollProtect(server.el, 1);
    atomic_store_explicit(&server.io_poll_state, AE_IO_STATE_POLL, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    jq->pending_epoll_job = true;
}

static void ioThreadAccept(void *data) {
    client *c = (client *)data;
    connAccept(c->conn, NULL);
    atomic_thread_fence(memory_order_release);
    c->io_read_state = CLIENT_COMPLETED_IO;
    threadRespond(c, R_READ);
}

/*
 * Attempts to offload an Accept operation (currently used for TLS accept) for a client
 * connection to I/O threads.
 *
 * Returns:
 *   C_OK  - If the accept operation was successfully queued for processing
 *   C_ERR - If the connection is not eligible for offloading
 *
 * Parameters:
 *   conn - The connection object to perform the accept operation on
 */
int trySendAcceptToIOThreads(connection *conn) {
    if (server.io_threads_num <= 1) {
        return C_ERR;
    }

    if (!(conn->flags & CONN_FLAG_ALLOW_ACCEPT_OFFLOAD)) {
        return C_ERR;
    }

    client *c = connGetPrivateData(conn);
    if (c->io_read_state != CLIENT_IDLE) {
        return C_OK;
    }

    if (server.active_io_threads_num <= 1) {
        return C_ERR;
    }

    size_t thread_id = (c->id % (server.active_io_threads_num - 1)) + 1;
    IOJobQueue *job_queue = &io_jobs[thread_id];

    if (IOJobQueue_isFull(job_queue)) {
        return C_ERR;
    }

    c->io_read_state = CLIENT_PENDING_IO;
    c->flag.pending_read = 1;
    connSetPostponeUpdateState(c->conn, 1);
    server.stat_io_reads_pending++;
    server.stat_io_accept_offloaded++;
    IOJobQueue_push(job_queue, ioThreadAccept, c);

    return C_OK;
}

#define JOB_BATCH_SIZE (16)
#define JOB_TYPE_MASK (7)         /* Lower 3 bits for job type */
#define CLIENT_PTR_MASK (~0x7ULL) /* Upper bits for client pointer */

static inline jobResponseType getJobResponseType(uint64_t jobData) {
    jobResponseType type = (jobResponseType)(jobData & JOB_TYPE_MASK);
    if (type >= R_LAST) {
        serverPanic("Invalid job type: %d", type);
    }
    return type;
}

static inline void *getJobData(uint64_t jobData) {
    return (void *)(jobData & CLIENT_PTR_MASK);
}

/* Function to handle read jobs */
static void handleReadJobs(client **read_jobs, int read_count) {
    server.stat_io_reads_pending -= read_count;
    serverAssert(server.stat_io_reads_pending >= 0);

    /* First pass: prefetch cluster slots for all clients */
    for (int i = 0; i < read_count; i++) {
        client *c = read_jobs[i];
        __builtin_prefetch(&(server.cluster->slots[c->slot]));
        __builtin_prefetch(&(server.cluster->migrating_slots_to[c->slot]));
        __builtin_prefetch(&(server.cluster->importing_slots_from[c->slot]));
        prefetchSlotPendingInfo(c->slot);
    }

    /* Second pass: process each client */
    for (int i = 0; i < read_count; i++) {
        client *c = read_jobs[i];
        processClientIOReadsDone(c);
        server.stat_io_reads_processed++;
    }

    /* Process commands in batch if we processed any reads */
    processClientsCommandsBatch();
}

/* Function to handle write jobs */
static void handleWriteJobs(client **write_jobs, int write_count) {
    server.stat_io_writes_pending -= write_count;
    serverAssert(server.stat_io_writes_pending >= 0);

    for (int i = 0; i < write_count; i++) {
        client *c = write_jobs[i];
        server.stat_io_writes_processed++;
        processClientIOWriteDone(c, 1);
    }
}

static void threadRespondJobList(void) {
    if (listLength(thread_delayed_jobs) == 0) return;

    IoToMTQueueProduce((uint64_t)thread_delayed_jobs | (uint64_t)R_JOBLIST, 0);
    thread_delayed_jobs = listCreate();
}

void threadRespond(client *c, jobResponseType r) {
    if (r == R_COMMAND) {
        /* Make sure to send first the deferred jobs list */
        threadRespondJobList();
    }

    IoToMTQueueProduce((uint64_t)c | (uint64_t)r, 0);
}

static void processClientIOCommandDone(client *c) {
    serverAssert(c->io_command_state == CLIENT_COMPLETED_IO);
    c->io_command_state = CLIENT_IDLE;

    if (c->flag.close_after_command) {
        c->flag.close_after_command = 0;
        c->flag.close_after_reply = 1;
    }

    struct serverCommand *real_cmd = c->realcmd;

    /* Command stats */
    real_cmd->calls++;
    real_cmd->microseconds += c->duration;
    c->commands_processed++;
    server.stat_numcommands++;

    /* Latency stats */
    char *latency_event = (real_cmd->flags & CMD_FAST) ? "fast-command" : "command";
    latencyAddSampleIfNeeded(latency_event, c->duration / 1000);
    if (server.latency_tracking_enabled)
        updateCommandLatencyHistogram(&(real_cmd->latency_histogram), c->duration * 1000);

    /* Command log */
    commandlogPushCurrentCommand(c, real_cmd);

    /* Monitor */
    if (!(c->cmd->flags & (CMD_SKIP_MONITOR | CMD_ADMIN))) {
        robj **argv = c->original_argv ? c->original_argv : c->argv;
        int argc = c->original_argv ? c->original_argc : c->argc;
        replicationFeedMonitors(c, server.monitors, c->db->id, argv, argc);
    }

    /* Cluster stats */
    clusterSlotStatsAddCpuDuration(c, c->duration);
    clusterSlotStatsAddNetworkBytesOutForUserClient(c);

    /* Tracking */
    if (c->flag.tracking && !c->flag.tracking_bcast) {
        trackingRememberKeys(c, c);
    }

    c->duration = 0;

    processClientIOWriteDone(c, 1); /* The Worker thread does 2 things: 1. process the command , 2. Writes the results. */
    if (c->flag.close_asap) {
        return;
    }

    commandProcessed(c);

    /* Update the client's memory to include output buffer growth following the
     * processed command. */
    if (c->conn) updateClientMemUsageAndBucket(c);

    if (clientHasPendingReplies(c) && trySendWriteToIOThreads(c) == C_ERR) {
        putClientInPendingWriteQueue(c);
    }

    beforeNextClient(c);
}

/* Function to handle command jobs */
static void handleCommandJobs(client **command_jobs, int command_count) {
    server.stat_io_commands_pending -= command_count;

    /* First pass: prefetch data for all command jobs */
    for (int i = 0; i < command_count; i++) {
        client *c = command_jobs[i];

        for (int j = 0; j < c->argc; j++) {
            __builtin_prefetch(c->argv[j]);
        }
        prefetchSlotPendingInfo(c->slot);
    }

    /* Second pass: process each command */
    for (int i = 0; i < command_count; i++) {
        client *c = command_jobs[i];
        int slot = c->slot;
        processClientIOCommandDone(c);
        dqDecr(slot);
        dqDecr(-1);
        server.stat_io_commands_processed++;
    }
}

int processIOThreadsResponses(void) {
    if (io_to_mt_queue == NULL) return 0;

    /* Quick check if any pending operations exist */
    if (getPendingIOThreadsJobs() == 0) return 0;

    int total_processed = 0;
    uint64_t jobs[JOB_BATCH_SIZE];
    client *read_jobs[JOB_BATCH_SIZE];
    client *write_jobs[JOB_BATCH_SIZE];
    client *command_jobs[JOB_BATCH_SIZE];

    /* Loop until we consume all pending jobs */
    while (1) {
        int received_responses = 0;
        int dequeued_count = 0;
        int read_count = 0;
        int write_count = 0;
        int command_count = 0;

        /* Try to dequeue JOB_BATCH_SIZE */
        while (received_responses < JOB_BATCH_SIZE) {
            dequeued_count = IoToMTQueueConsumeBatch(JOB_BATCH_SIZE - received_responses, jobs);

            /* Stop if we can't get more jobs from the queue. */
            if (dequeued_count == 0) break;

            received_responses += dequeued_count;
            total_processed += dequeued_count;

            /* Prefetch the jobs data */
            for (int i = 0; i < dequeued_count; i++) {
                if (getJobResponseType(jobs[i]) == R_JOBLIST) continue;
                client *c = getJobData(jobs[i]);
                /* Always prefetch the client pointer */
                __builtin_prefetch(c);
                __builtin_prefetch(&c->slot);
            }

            for (int i = 0; i < dequeued_count; i++) {
                jobResponseType job_type = getJobResponseType(jobs[i]);
                if (job_type == R_JOBLIST) {
                    dispatchThreadDeferredJobs((list *)getJobData(jobs[i]));
                    continue;
                }
                client *c = getJobData(jobs[i]);
                if (job_type == R_READ) {
                    serverAssert(c->io_read_state == CLIENT_COMPLETED_IO);
                    read_jobs[read_count++] = c;
                } else if (job_type == R_WRITE) {
                    serverAssert(c->io_write_state == CLIENT_COMPLETED_IO);
                    write_jobs[write_count++] = c;
                } else if (job_type == R_COMMAND) {
                    serverAssert(c->io_write_state == CLIENT_COMPLETED_IO);
                    serverAssert(c->io_command_state == CLIENT_COMPLETED_IO);
                    command_jobs[command_count++] = c;
                } else {
                    serverPanic("Unknown job type %d", job_type);
                }
            }
        }

        if (read_count) handleReadJobs(read_jobs, read_count);
        if (write_count) handleWriteJobs(write_jobs, write_count);
        if (command_count) handleCommandJobs(command_jobs, command_count);

        /* If the queue was empty at the last try - don't try again */
        if (dequeued_count == 0) return total_processed;
    }
}
