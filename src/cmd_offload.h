/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 * Command offloading and deferred queues for managing command execution ordering
 * in multithreaded environments.
 */

#ifndef __CMD_OFFLOAD_H
#define __CMD_OFFLOAD_H

#include <stddef.h>
#include "adlist.h"

/* Check if a command can be offloaded to IO threads. */
#define CMD_CAN_BE_OFFLOADED(cmd)             \
    ((cmd->flags & CMD_READONLY) &&           \
     !(cmd->flags & CMD_NO_MANDATORY_KEYS) && \
     !(cmd->flags & CMD_MAY_REPLICATE) &&     \
     !(cmd->flags & CMD_BLOCKING))

typedef void (*job_handler)(void *);

struct client;
struct serverCommand;
struct slotQueue;

/* Initialize the deferred queues system */
void initSlotQueues(void);

/* Check if a command can be executed immediately or needs to be deferred */
int canExecuteCommand(struct client *c);

/* Check if a command requires exclusive access to the whole database */
int requiresServerExclusivity(struct serverCommand *cmd, int slot);

/* Check if a command can be offloaded to IO threads */
int canCommandBeOffloaded(int slot, struct serverCommand *cmd);

/* Increment/decrement the reference count for a slot */
void slotQueueIncRef(int slot);
void slotQueueDecRef(int slot);

/* Increment/decrement the exclusive queue reference count */
void exclusiveQueueIncRef(void);
void exclusiveQueueDecRef(void);

/* Get/set the thread id handling a slot */
int getSlotThreadId(int slot);
void setSlotThreadId(int slot, int tid);

/* Remove a client from its pending queue */
void slotQueueRemoveClient(struct client *c);

/* Called when a client is unlinked */
void ioThreadsOnUnlinkClient(struct client *c);

/* Add a deferred job to the thread-local job list (called from IO threads) */
void threadAddDeferredJob(int slot, job_handler handler, size_t data_size, void *data);

/* Initialize/free thread-local deferred jobs list */
void initThreadDeferredJobs(void);
void freeThreadDeferredJobs(void);

/* Add a deferred job to a job list */
void slotQueueAddJob(list *jobs_list, int slot, job_handler handler, size_t data_size, void *data);

/* Dispatch deferred jobs from a job list */
void dispatchSlotQueueJobs(list *jobs_list);

/* Prefetch slot info for better cache performance */
void prefetchSlotQueueInfo(int slot);

/* Check if server cron should be deferred */
int isServerCronDeferred(void);

/* Check if command should be postponed due to busy slot */
int yieldForBusySlot(struct client *c);

/* Command execution in IO thread */
void ioThreadCallCommand(struct client *c);

/* Write response after command execution in IO thread */
void ioThreadWriteAfterCmd(struct client *c);

/* Send command result to main thread, flushing deferred jobs first */
void sendCommandResultToMain(struct client *c);

/* Process batch of completed command jobs (called from main thread) */
void handleCommandJobs(struct client **command_jobs, int command_count);

/* Update offloading throttle and saturation state */
void updateOffloadingThrottle(void);
void updateOffloadingSaturation(void);

#endif /* __CMD_OFFLOAD_H */
