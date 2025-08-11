#ifndef THREAD_COMMON_H
#define THREAD_COMMON_H

#include "server.h"


/* thread-local integer to store each thread's unique ID.
 * The main thread will have ID 0. Other worker threads will be assigned
 * unique, non-zero IDs by their respective thread pool managers (see io_threads.c and rdb_threads.c).*/
extern __thread int thread_id;


/* Returns 1 (true) if it's the main thread, 0 (false) otherwise. */
int inMainThread(void);

/* Returns 0 for the main thread, and the assigned ID for worker threads. */
int getThreadID(void);


/* --- Job Queue Definitions --- Used to send jobs from the main-thread to the worker threads (IO Thread or RDB Thread).*/
typedef void (*job_handler)(void *);

typedef struct job {
    job_handler handler;
    void *data;
} job;

typedef struct JobQueue {
    job *ring_buffer;
    size_t size;
    _Atomic size_t head __attribute__((aligned(CACHE_LINE_SIZE))); /* Next write index for producer (main-thread) */
    _Atomic size_t tail __attribute__((aligned(CACHE_LINE_SIZE))); /* Next read index for consumer (Thread) */
} JobQueue;

void JobQueue_init(JobQueue *jq, size_t item_count);
void JobQueue_cleanup(JobQueue *jq);
int JobQueue_isFull(const JobQueue *jq);
void JobQueue_push(JobQueue *jq, job_handler handler, void *data);
size_t JobQueue_availableJobs(const JobQueue *jq);
int JobQueue_isEmpty(const JobQueue *jq);
void JobQueue_removeJob(JobQueue *jq);
void JobQueue_peek(const JobQueue *jq, job_handler *handler, void **data);

#endif /* THREAD_COMMON_H */
