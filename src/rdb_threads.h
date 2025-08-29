#ifndef __RDB_THREADS_H__
#define __RDB_THREADS_H__

#include "server.h"
#include "thread_common.h"

/*
 * Threshold for flushing a worker's buffer to the target RIO (File or Replica Socket) (64 KB).
 * If the thread's buffer stores more than this amount of data AFTER an entire key-value pair has been
 * saved we will flush the buffer to the target RIO.
 *
 * Impact on Multi-Thread RDB Save Performance: Above 64 KB the performance of RDB Save is consistent.
 *
 * Impact on Multi-Thread RDB Load Performance: Since the buffer flush size defines the minimum "rdb-data-segment" size,
 * saving with larger values will improve multi-thread load because we can request larger reads from the OS.
 */
#define RDB_WORKER_FLUSH_SIZE_MIN (64 * 1024)           /* Min = 64 KB */
#define RDB_WORKER_FLUSH_SIZE_DEFAULT (4 * 1024 * 1024) /* Default = 4 MB */
#define RDB_WORKER_FLUSH_SIZE_MAX (8 * 1024 * 1024)     /* Max = 8 MB */

/*
 * The maximum memory a single worker thread's buffer can consume
 * before streaming directly to the RIO. This determines the upper bound
 * on memory usage for RDB threads during save operations, which is
 * (capacity * thread_count).
 * Larger values increase memory usage but can improve performance with large keys.
 *
 * Having this limit is essential for preventing "big keys" from consuming an unbounded amount of memory.
 */
#define RDB_WORKER_BUFFER_CAPACITY_LIMIT (16 * 1024 * 1024)

/* The sixe of the RDB Save job queues (min size of JobQueue) */
#define RDB_SAVE_JOB_QUEUE_SIZE 2

/* The size of the job queue for RDB loading threads. */
#define RDB_LOAD_JOB_QUEUE_SIZE 10

/* The size of a batch of keys which is the max number of keys we will insert at a time. */
#define RDB_LOAD_BATCH_SIZE 1024

/* ----- Thread Management Functions ----- */

void initRDBThreads(int per_thread_queue_size);
void killRDBThreads(void);
void drainRDBThreadsQueue(void);
void startRDBThreads(void);
void stopRDBThreads(void);


/* ----- Multi-Threaded RDB Save ----- */

typedef struct RdbSaveThreadArgs RdbSaveThreadArgs;

/* Describes a range buckets in a hashtable for a thread to process. */
typedef struct BucketStride {
    size_t start_index; /* First logical bucket index for this thread */
    size_t stride_size; /* Step size to find the next logical bucket (typically num_worker_threads) */
} BucketStride;

/* Info needed by main thread for reporting save progress*/
typedef struct MainThreadRdbInfo {
    RdbSaveThreadArgs *threadArgs;
    long *last_key_counter;
    long long *info_updated_time;
    char *pname;
} MainThreadRdbInfo;

/* Arguments for a worker thread responsible for saving a portion of a database. */
typedef struct RdbSaveThreadArgs {
    int dbid;                                   /* Database ID being saved. */
    hashtable *ht;                              /* Hashtable to be saved. */
    BucketStride bucket_stride;                 /* Defines what buckets the thread is responsible for. */
    atomic_long keys_processed;                 /* Number of keys processed by this thread. */
    ssize_t bytes_written;                      /* Total bytes written by this thread. */
    rio buf_to_target_rio;                      /* In-memory buffer (with max capacity) for key-value serialization. */
    pthread_mutex_t *rdb_write_mutex;           /* Protects access to the shared RIO. */
    int save_status;                            /* Thread's save status. */
    MainThreadRdbInfo *main_thread_report_info; /* Reporting info (only set for the main thread's arguments). */
} RdbSaveThreadArgs;

ssize_t rdbSaveDbMultiThreaded(rio *rdb, int dbid, long *key_counter, char *pname);


/* ----- Multi-Threaded RDB Load ----- */

/* Global atomic variable to signal a loading error from any worker thread. */
extern _Atomic int rdb_load_thread_error;

/* Represents a key loaded from an RDB file, ready for insertion. */
typedef struct RdbLoadedKey {
    sds key;              /* The key string. */
    robj *val;            /* The key's value object. */
    long long expiretime; /* The expiration timestamp. */
    long long lfu_freq;   /* The LFU frequency. */
    long long lru_idle;   /* The LRU idle time. */
} RdbLoadedKey;

/* Arguments for a worker thread responsible for loading an RDB data segment. */
typedef struct RdbDataSegmentLoadArgs {
    rio *data_segment_rio;
    serverDb *db;
    int dbid;
    int rdbver;
    int rdbflags;
    long long lru_clock;
    long long now;
    pthread_mutex_t *db_insert_mutex; /* Protects access to the database during insertion. */
} RdbDataSegmentLoadArgs;


int offloadRdbDataSegment(
    rio *rdb_main_stream,
    unsigned long segment_size,
    serverDb *db,
    int dbid,
    int rdbver,
    int rdbflags,
    long long lru_clock,
    long long now,
    pthread_mutex_t *db_insert_mutex);

#endif // __RDB_THREADS_H__
