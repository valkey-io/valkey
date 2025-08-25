#ifndef __RDB_THREADS_H__
#define __RDB_THREADS_H__

#include "server.h"
#include "thread_common.h"
/*
* Default threshold for flushing a worker's buffer to the target RIO (File or Replica Socket) (64 KB).
* If the thread's buffer stores more than this amount of data AFTER an entire key-value pair has been
* saved we will flush the buffer to the target RIO.
*/
#define WORKER_BUFFER_DEFAULT_SIZE 4 * 1024 * (1024)
/*
* The maximum buffer size (256 KB).
* If we exceed this limit during the saving of a single key value pair we 
* will flush the buffer to the target RIO and stream the rest of the key value pair
* directly to the RIO. This prevents "big keys" from consuming an unbounded amount of memory.
*/
#define WORKER_BUFFER_CAPACITY_LIMIT 4 * 1024 * (1024)

/* Minimum size of JobQueue */
#define RDB_SAVE_JOB_QUEUE_SIZE 2

/* The size of the RDB load job queue. */
#define RDB_LOAD_JOB_QUEUE_SIZE 10

/* The size of a batch of keys for loading. */
#define RDB_LOAD_BATCH_SIZE 512

/* The number of locks for managing concurrent DB insertions. */
#define NUM_LOCKS 16


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
    int dbid;                                 /* Database ID being saved. */
    hashtable *ht;                            /* Hashtable to be saved. */
    BucketStride bucket_stride;               /* Defines what buckets the thread is responsible for. */
    atomic_long keys_processed;               /* Number of keys processed by this thread. */
    ssize_t bytes_written;                    /* Total bytes written by this thread. */
    rio buf_to_target_rio;                    /* In-memory buffer (with max capacity) for key-value serialization. */
    pthread_mutex_t *rdb_write_mutex;         /* Protects access to the shared RIO. */
    int save_status;                          /* Thread's save status. */
    MainThreadRdbInfo *main_thread_report_info; /* Reporting info (only set for the main thread's arguments). */
} RdbSaveThreadArgs;

ssize_t rdbSaveDbMultiThreaded(rio *rdb, int dbid, long *key_counter, char *pname);


/* ----- Multi-Threaded RDB Load ----- */

/* Represents a loaded key from an RDB file, ready for insertion into the database. */
typedef struct RdbLoadedKey {
    sds key_sds;
    robj *val_obj;
    long long expiretime;
    long long lfu_freq;
    long long lru_idle;
    long long lru_clock;
    long long now;
} RdbLoadedKey;

/* A buffer for holding a chunk of RDB data. */
typedef struct RdbChunkBuffer {
    rio buffer_rio;
    sds sds_chunk_buf;
} RdbChunkBuffer;

typedef struct RdbLoadThreadContext {
    long long main_keys_delta;      /* Delta for the currently active slot */
    long long volatile_keys_delta;  /* Delta for the currently active slot */
    int       current_slot;         /* The slot these deltas are for */
} RdbLoadThreadContext;

/* Arguments for a worker thread responsible for loading an RDB chunk. */
typedef struct RdbChunkLoadThreadArgs {
    rio *rdb;
    RdbLoadThreadContext *rdb_thread_context;
    serverDb *db;
    int rdbflags;
    int current_dbid;
    int rdbver;
    long long lru_clock;
    long long now;
    pthread_mutex_t *db_insert_mutexes;
    RdbLoadedKey batch_buffers[NUM_LOCKS][RDB_LOAD_BATCH_SIZE];
    int batch_counts[NUM_LOCKS];
} __attribute__((aligned(CACHE_LINE_SIZE))) RdbChunkLoadThreadArgs;

int offloadRDBChunkToThread(
    rio *rdb_main_stream,         
    unsigned long chunk_size,     
    int rdbver,                   
    serverDb *current_db,         
    int rdbflags,                 
    int current_dbid,
    long long lru_clock,
    long long now,
    pthread_mutex_t *db_insert_mutexes,
    RdbLoadThreadContext *rdb_thread_contexts
);


/* Global state variable to signal a loading error. */
extern _Atomic int rdb_load_thread_error;

/* ----- Thread Management Functions ----- */

void initRDBThreads(int per_thread_queue_size);
void killRDBThreads(void);
void drainRDBThreadsQueue(void);
void startRDBThreads(void);
void stopRDBThreads(void);

#endif // __RDB_THREADS_H__
