#include "server.h"
#include "zmalloc.h"
#include <assert.h>
#include <stdarg.h>

/* Forward declarations from module.h to avoid pulling in full module internals
 * which has header dependency issues when included before server.h */
void moduleNotifyKeyspaceEvent(int type, const char *event, robj *key, int dbid);

/*================================= Internal Data structures ======================== */

/* Internal structure used to track replication offset and arguments needed in
 * executing task when offset has been acked by required number of replicas. */
typedef struct taskWaitingAck {
    int type; /* Task type */
    int64_t offset;
    void **argv;
} taskWaitingAck;

// Internal structure used to define all handlers for a task type
typedef struct taskWaitingAckType {
    taskWaitingAck *(*create_task)(va_list);
    void (*destroy_task)(void *);
    void (*execute_task)(const taskWaitingAck *);
    void (*on_client_destroy)(void *);
} taskWaitingAckType;

static taskWaitingAckType taskTypes[POST_COMMIT_TASK_TYPE_MAX];

/*================================= Keyspace Notify Task ===================== */

// Create the keyspace notify task.
static taskWaitingAck *createKeyspaceNotifyTask(va_list ap) {
    int argc = 4; /* 4 arguments for notify function: type, event, key, dbid */
    taskWaitingAck *task = zcalloc(sizeof(taskWaitingAck));
    task->argv = zmalloc(argc * sizeof(void *));
    for (int i = 0; i < argc; i++) {
        task->argv[i] = va_arg(ap, void *);
    }

    /* Copy the event string (argv[1]) because the caller (especially modules)
     * may free the original string after this function returns. */
    char *event = (char *)task->argv[1];
    if (event) {
        task->argv[1] = zstrdup(event);
    }

    // Increase reference count to avoid the key from being deleted
    robj *key = (robj *)task->argv[2];
    if (key) {
        incrRefCount(key);
    }
    return task;
}

// Destroy the keyspace notify task.
static void destroyKeyspaceNotifyTask(void *ptr) {
    taskWaitingAck *task = (taskWaitingAck *)ptr;
    // Free the copied event string (argv[1])
    if (task->argv[1]) {
        zfree(task->argv[1]);
    }
    if (task->argv[2]) {
        robj *key = (robj *)task->argv[2];
        decrRefCount(key);
    }
    zfree(task->argv);
    zfree(task);
}

// Execute the keyspace notify task.
static void executeKeyspaceNotifyTask(const taskWaitingAck *task) {
    notifyKeyspaceEvent((int)(intptr_t)task->argv[0],
                        (char *)task->argv[1],
                        (robj *)task->argv[2],
                        (int)(intptr_t)task->argv[3]);
}

/*================================= Key Invalidation Task ==================== */

// Create the key invalidation task.
static taskWaitingAck *createKeyInvalidationTask(va_list ap) {
    /* A key invalidation task has 2 arguments:
     * 1. client* which generated the modification on the key
     * 2. serverObject* that is modified */
    int argc = 2;
    taskWaitingAck *task = zcalloc(sizeof(taskWaitingAck));
    task->argv = zmalloc(argc * sizeof(void *));
    for (int i = 0; i < argc; i++) {
        task->argv[i] = va_arg(ap, void *);
    }

    // Track the pending notification task in the referenced client
    client *c = (client *)task->argv[0];
    if (c != NULL) {
        listAddNodeTail(c->reply_blocking_state.pending_notify_tasks, task);
    }

    // Increase reference count to avoid the key from being deleted
    robj *key = (robj *)task->argv[1];
    if (key) {
        incrRefCount(key);
    }
    return task;
}

// Destroy the key invalidation task.
static void destroyKeyInvalidationTask(void *ptr) {
    taskWaitingAck *task = (taskWaitingAck *)ptr;
    /* Remove the current task from the list of pending tasks for the client.
     * The tasks are tracked in FIFO order so we only need to look at the first one. */
    client *c = (client *)task->argv[0];
    if (c != NULL) {
        serverAssert(listLength(c->reply_blocking_state.pending_notify_tasks) > 0);
        listNode *first = listFirst(c->reply_blocking_state.pending_notify_tasks);
        serverAssert(task == (taskWaitingAck *)listNodeValue(first));
        listDelNode(c->reply_blocking_state.pending_notify_tasks, first);
    }

    // Decrement the refcount for the key
    if (task->argv[1]) {
        robj *key = (robj *)task->argv[1];
        decrRefCount(key);
    }
    zfree(task->argv);
    zfree(task);
}

// De-reference the client argument from the key invalidation task
static void destroyClientForKeyInvalidationTask(void *task_ptr) {
    taskWaitingAck *task = (taskWaitingAck *)task_ptr;
    // The first argument is the client pointer
    task->argv[0] = NULL;
}

// Execute the key invalidation task.
static void executeKeyInvalidationTask(const taskWaitingAck *task) {
    trackingInvalidateKey((client *)task->argv[0], (robj *)task->argv[1], 1);
}

/*================================= Flush Invalidation Task ================== */

// Create the flush invalidation task.
static taskWaitingAck *createFlushInvalidationTask(va_list ap) {
    // Flush invalidation task has database ID as argument
    int argc = 1;
    taskWaitingAck *task = zcalloc(sizeof(taskWaitingAck));
    task->argv = zmalloc(argc * sizeof(void *));
    for (int i = 0; i < argc; i++) {
        task->argv[i] = va_arg(ap, void *);
    }
    return task;
}

// Destroy the flush invalidation task.
static void destroyFlushInvalidationTask(void *ptr) {
    taskWaitingAck *task = (taskWaitingAck *)ptr;
    zfree(task->argv);
    zfree(task);
}

// Execute the flush invalidation task.
static void executeFlushInvalidationTask(const taskWaitingAck *task) {
    bool is_flush_all = (bool)task->argv[0];
    /* Use DBID -1 for FLUSHALL, otherwise use 0 for DBID.
     * Note: This assumes the OSS Redis code below doesn't operate on the actual
     * DBID besides differentiating between FLUSHDB and FLUSHALL. */
    trackingInvalidateKeysOnFlush(is_flush_all ? -1 : 0);
}

/*================================= Default callback ========================= */

// Default callback on client destroy doing no-op
static void destroyClientDefaultCallback(void *task) {
    UNUSED(task);
    return;
}

/*================================= Task Type Registry ======================= */

void initTaskTypes(void) {
    taskTypes[POST_COMMIT_KEYSPACE_NOTIFY_TASK] = (taskWaitingAckType){
        createKeyspaceNotifyTask,
        destroyKeyspaceNotifyTask,
        executeKeyspaceNotifyTask,
        destroyClientDefaultCallback};
    taskTypes[POST_COMMIT_KEY_INVALIDATION_TASK] = (taskWaitingAckType){
        createKeyInvalidationTask,
        destroyKeyInvalidationTask,
        executeKeyInvalidationTask,
        destroyClientForKeyInvalidationTask};
    taskTypes[POST_COMMIT_FLUSH_INVALIDATION_TASK] = (taskWaitingAckType){
        createFlushInvalidationTask,
        destroyFlushInvalidationTask,
        executeFlushInvalidationTask,
        destroyClientDefaultCallback};
}

/*================================= Task Registration ======================== */

/* Create task based on the given task type and arguments, and append the new
 * task to the end of the linkedlist of the pending tasks of that task type.
 *
 * Note that at this point in time, we might not know about the replication
 * offset we want to configure this task with so we put it onto a pending list.
 * And at a later point in time, when we know the replication offset, we would
 * set it and move the task to the official tasks list. */
bool replyBlockingRegisterPostCommitTask(int type, ...) {
    // Check reply-blocking is active and the type is valid
    if (!isPrimaryReplyBlockingEnabled() || type < 0 || type >= POST_COMMIT_TASK_TYPE_MAX) {
        return false;
    }

    va_list ap;
    bool return_code = false;
    va_start(ap, type);
    taskWaitingAck *task = taskTypes[type].create_task(ap);
    if (task) {
        task->type = type;
        if (server.current_client != NULL) {
            /* Here the notification is triggered by an incoming client request when we
             * don't yet know the actual replication offset after command is applied,
             * so we need to put it onto a pending tasks list. */
            listAddNodeTail(server.reply_blocking.pending_tasks_waiting_ack[type], task);
        } else {
            /* This notification is triggered from a background job such as
             * active expiry or eviction outside of a regular client command.
             * The replication offset is already updated so we use it directly. */
            task->offset = server.primary_repl_offset;
            listAddNodeTail(server.reply_blocking.tasks_waiting_ack[type], task);
        }
        return_code = true;
    }
    va_end(ap);
    return return_code;
}

/*================================= Signal Handlers ========================== */

bool replyBlockingSignalModifiedKey(struct client *c, struct serverDb *db, struct serverObject *key) {
    if (!isPrimaryReplyBlockingEnabled()) return false;

    /* Background writes (expiry/eviction) call signalModifiedKey with a NULL
     * client and have no argv; track them here. Client-command keys are handled
     * by the command's own offset computation. */
    if (c == NULL) trackBackgroundModifiedKey(db, key);

    // Defer key invalidation messages until the reply-blocking providers acknowledge.
    return replyBlockingRegisterPostCommitTask(POST_COMMIT_KEY_INVALIDATION_TASK,
                                               (void *)c, (void *)key);
}


bool replyBlockingSignalFlushedDb(int dbid) {
    // Defer flush invalidation messages until the reply-blocking providers acknowledge.
    return replyBlockingRegisterPostCommitTask(POST_COMMIT_FLUSH_INVALIDATION_TASK,
                                               (void *)(intptr_t)(dbid == -1));
}

/*================================= Task Execution =========================== */

// Find and execute deferred tasks when 'consensus_ack_offset' is acked.
void executeDeferredTasksForAck(const long long consensus_ack_offset) {
    listIter li;
    listNode *ln;
    struct reply_blocking_t *rb_state = &server.reply_blocking;

    /* Mark that we are re-firing deferred tasks. A keyspace-notify task
     * re-enters notifyKeyspaceEvent(), which checks this flag to skip the
     * first-pass work (inline module notify + re-registration) and go straight
     * to publishing the client pub/sub message. Save/restore rather than a bare
     * false so a future re-entrant task type can't clear it prematurely. */
    bool prev_in_post_commit_task_execution = rb_state->in_post_commit_task_execution;
    rb_state->in_post_commit_task_execution = true;

    for (int i = 0; i < POST_COMMIT_TASK_TYPE_MAX; i++) {
        listRewind(rb_state->tasks_waiting_ack[i], &li);
        while ((ln = listNext(&li))) {
            taskWaitingAck *task = listNodeValue(ln);
            if (task->offset <= consensus_ack_offset) {
                taskTypes[i].execute_task(task);
                listDelNode(rb_state->tasks_waiting_ack[i], ln);
            } else {
                break;
            }
        }
    }

    rb_state->in_post_commit_task_execution = prev_in_post_commit_task_execution;
}

// Move pending deferred tasks to the official list with the current replication offset.
void certifyPendingDeferredTasks(void) {
    listIter li;
    listNode *ln;
    for (int i = 0; i < POST_COMMIT_TASK_TYPE_MAX; i++) {
        /* Splice pending tasks into a local list so module callbacks that
         * register new deferred tasks don't corrupt our iteration. */
        list *pending = server.reply_blocking.pending_tasks_waiting_ack[i];
        if (listLength(pending) == 0) continue;
        list local;
        memset(&local, 0, sizeof(local));
        listJoin(&local, pending);
        listRewind(&local, &li);
        while ((ln = listNext(&li))) {
            taskWaitingAck *task = listNodeValue(ln);
            serverAssert(task->offset == 0);
            task->offset = server.primary_repl_offset;
        }
        listJoin(server.reply_blocking.tasks_waiting_ack[i], &local);
    }
}

/*================================= Client Lifecycle ========================= */

/* Notify the task system that a client is being destroyed so that
 * any tasks referencing it can de-reference the client pointer. */
void postCommitTaskNotifyClientDestroy(struct list *pending_notify_tasks) {
    listIter li;
    listNode *ln;
    listRewind(pending_notify_tasks, &li);
    while ((ln = listNext(&li))) {
        taskWaitingAck *task = (taskWaitingAck *)listNodeValue(ln);
        if (task) {
            taskTypes[task->type].on_client_destroy(task);
        }
    }
}

/*================================= Init / Cleanup =========================== */

/* Initialize the task lists in the reply-blocking structure.
 * Called from replyBlockingInit(). */
void postCommitTaskInitLists(void) {
    for (int i = 0; i < POST_COMMIT_TASK_TYPE_MAX; i++) {
        server.reply_blocking.tasks_waiting_ack[i] = listCreate();
        server.reply_blocking.pending_tasks_waiting_ack[i] = listCreate();
        listSetFreeMethod(server.reply_blocking.tasks_waiting_ack[i],
                          taskTypes[i].destroy_task);
        listSetFreeMethod(server.reply_blocking.pending_tasks_waiting_ack[i],
                          taskTypes[i].destroy_task);
    }
}

// Release (free) all task lists. Called from replyBlockingCleanup().
void postCommitTaskCleanupLists(void) {
    for (int i = 0; i < POST_COMMIT_TASK_TYPE_MAX; i++) {
        listRelease(server.reply_blocking.tasks_waiting_ack[i]);
        server.reply_blocking.tasks_waiting_ack[i] = NULL;
        listRelease(server.reply_blocking.pending_tasks_waiting_ack[i]);
        server.reply_blocking.pending_tasks_waiting_ack[i] = NULL;
    }
}

// Empty (but don't free) all task lists. Called during primary state reset.
void postCommitTaskEmptyLists(void) {
    for (int i = 0; i < POST_COMMIT_TASK_TYPE_MAX; i++) {
        listEmpty(server.reply_blocking.tasks_waiting_ack[i]);
        listEmpty(server.reply_blocking.pending_tasks_waiting_ack[i]);
    }
}
