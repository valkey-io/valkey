#ifndef POST_COMMIT_TASK_H
#define POST_COMMIT_TASK_H

/* Include feature-test macros early so _FILE_OFFSET_BITS=64 is defined
 * before any system headers, ensuring off_t is 64-bit on 32-bit builds. */
#include "fmacros.h"

#include <stdbool.h>
#include <inttypes.h>

struct client;
struct serverDb;
struct serverObject;
struct list;

/* Define the supported task types for deferred work that executes
 * after reply-blocking has been confirmed (replica ACK). */
typedef enum {
    POST_COMMIT_KEYSPACE_NOTIFY_TASK = 0, /* KEYSPACE NOTIFY task */
    POST_COMMIT_KEY_INVALIDATION_TASK,    /* Key invalidation task for client side caching */
    POST_COMMIT_FLUSH_INVALIDATION_TASK,  /* FLUSH invalidation task for client side caching */
    POST_COMMIT_TASK_TYPE_MAX             /* Max task type */
} postCommitTaskType;

/* Initialize the task type registry (create/destroy/execute handlers).
 * Must be called before any task registration. */
void initTaskTypes(void);

/* Register a deferred task for execution after the current replication
 * offset is acknowledged by reply-blocking providers. The task is created
 * from the variadic arguments based on the given task type.
 *
 * Returns true if the task was successfully registered, false otherwise. */
bool replyBlockingRegisterPostCommitTask(int type, ...);

// Find and execute all deferred tasks whose offset <= consensus_ack_offset.
void executeDeferredTasksForAck(long long consensus_ack_offset);

/* Move pending tasks (registered during command execution before the
 * replication offset was known) to the official tasks list, setting
 * their offset to server.primary_repl_offset. */
void certifyPendingDeferredTasks(void);

/* Notify the task system that a client is being destroyed so that
 * any tasks referencing it can de-reference the client pointer.
 * Iterates all tasks in the given pending_notify_tasks list. */
void postCommitTaskNotifyClientDestroy(struct list *pending_notify_tasks);

/* Custom processing whenever a key gets modified. Invoked from signalModifiedKey().
 *
 * Return true if no further processing are required in signalModifiedKey() such
 * as some async tasks are created which need some time to finish, false otherwise. */
bool replyBlockingSignalModifiedKey(struct client *c, struct serverDb *db, struct serverObject *key);

/* Custom processing whenever a FLUSH happens. Invoked from signalFlushedDb().
 *
 * Return true if no further processing are required in signalFlushedDb() such
 * as some async tasks are created which need some time to finish, false otherwise. */
bool replyBlockingSignalFlushedDb(int dbid);

/* Initialize the task lists in the reply-blocking structure.
 * Called from replyBlockingInit(). */
void postCommitTaskInitLists(void);

// Release (free) all task lists. Called from replyBlockingCleanup().
void postCommitTaskCleanupLists(void);

// Empty (but don't free) all task lists. Called during primary state reset.
void postCommitTaskEmptyLists(void);

#endif /* POST_COMMIT_TASK_H */
