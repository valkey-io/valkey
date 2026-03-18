#ifndef DURABLE_TASK_H
#define DURABLE_TASK_H

#include <stdbool.h>
#include <inttypes.h>

struct client;
struct serverDb;
struct serverObject;
struct list;

/**
 * Define the supported task types for deferred work that executes
 * after durability has been confirmed (replica ACK).
 */
typedef enum {
    DURABLE_KEYSPACE_NOTIFY_TASK = 0, /* KEYSPACE NOTIFY task */
    DURABLE_KEY_INVALIDATION_TASK,    /* Key invalidation task for client side caching */
    DURABLE_FLUSH_INVALIDATION_TASK,  /* FLUSH invalidation task for client side caching */
    DURABLE_TASK_TYPE_MAX             /* Max task type */
} durableTaskType;

/**
 * Initialize the task type registry (create/destroy/execute handlers).
 * Must be called before any task registration.
 */
void initTaskTypes(void);

/**
 * Register a deferred task for execution after the current replication
 * offset is acknowledged by durability providers. The task is created
 * from the variadic arguments based on the given task type.
 *
 * Returns true if the task was successfully registered, false otherwise.
 */
bool durabilityRegisterDeferredTask(int type, ...);

/**
 * Find and execute all deferred tasks whose offset <= consensus_ack_offset.
 */
void executeDeferredTasksForAck(long long consensus_ack_offset);

/**
 * Move pending tasks (registered during command execution before the
 * replication offset was known) to the official tasks list, setting
 * their offset to server.primary_repl_offset.
 */
void certifyPendingDeferredTasks(void);

/**
 * Notify the task system that a client is being destroyed so that
 * any tasks referencing it can de-reference the client pointer.
 * Iterates all tasks in the given pending_notify_tasks list.
 */
void durableTaskNotifyClientDestroy(struct list *pending_notify_tasks);

/**
 * Custom processing whenever a key gets modified. Invoked from signalModifiedKey().
 *
 * Return true if no further processing are required in signalModifiedKey() such
 * as some async tasks are created which need some time to finish, false otherwise.
 */
bool durabilitySignalModifiedKey(struct client *c, struct serverDb *db, struct serverObject *key);

/**
 * Custom processing whenever a FLUSH happens. Invoked from signalFlushedDb().
 *
 * Return true if no further processing are required in signalFlushedDb() such
 * as some async tasks are created which need some time to finish, false otherwise.
 */
bool durabilitySignalFlushedDb(int dbid);

/**
 * Initialize the task lists in the durability structure.
 * Called from durabilityInit().
 */
void durableTaskInitLists(void);

/**
 * Release (free) all task lists. Called from durabilityCleanup().
 */
void durableTaskCleanupLists(void);

/**
 * Empty (but don't free) all task lists. Called during primary state reset.
 */
void durableTaskEmptyLists(void);

#endif /* DURABLE_TASK_H */
