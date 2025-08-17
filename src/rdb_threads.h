#ifndef __RDB_THREADS_H__
#define __RDB_THREADS_H__

#include "server.h"
#include "thread_common.h"

/*
* Default threshold for flushing a worker's buffer to the target RIO (File or Replica Socket) (64 KB).
* If the thread's buffer stores more than this amount of data AFTER an entire key-value pair has been
* saved we will flush the buffer to the target RIO.
*/
#define WORKER_BUFFER_DEFAULT_SIZE 64 * (1024)
/*
* The maximum buffer size (256 KB).
* If we exceed this limit during the saving of a single key value pair we 
* will flush the buffer to the target RIO and stream the rest of the key value pair
* directly to the RIO. This prevents "big keys" from consuming an unbounded amount of memory.
*/
#define WORKER_BUFFER_CAPACITY_LIMIT 256 * (1024)

#define RDB_SAVE_JOB_QUEUE_SIZE 2 /* Minimum size of JobQueue */

typedef struct RdbSaveThreadArgs RdbSaveThreadArgs;

/* Describes a range buckets in a hashtable for a thread to process. */
typedef struct BucketStride {
    size_t start_index; // First logical bucket index for this thread
    size_t stride_size; // Step size to find the next logical bucket (typically num_worker_threads)
} BucketStride;

/* Info needed by main thread for reporting save progress*/
typedef struct MainThreadRdbInfo {
    RdbSaveThreadArgs *threadArgs;
    long *last_key_counter;
    long long *info_updated_time;
    char *pname;
} MainThreadRdbInfo;

typedef struct RdbSaveThreadArgs {
    int dbid;                   /* Database ID being saved */
    hashtable *ht;              /* hashtable to be saved */
    BucketStride bucket_stride; /* Defines what buckets in a hashtable the thread is responsible for */
    atomic_long keys_processed;
    ssize_t bytes_written;
    rio buf_to_target_rio;            /* In-memory buffer (with max capacity) for key/val serialization */
    pthread_mutex_t *rdb_write_mutex; /* Protects access to underlying mutex in buf_to_underlying_rio */
    int save_status;
    MainThreadRdbInfo *main_thread_report_info; /* Reporting info (only set for main thread's args) */
} RdbSaveThreadArgs;


void initRDBThreads(int per_thread_queue_size);
void killRDBThreads(void);
void drainRDBThreadsQueue(void);

void startRDBThreads(void);
void stopRDBThreads(void);



ssize_t rdbSaveDbMultiThreaded(rio *rdb, int dbid, long *key_counter, char *pname);

typedef struct RdbChunkLoadThreadArgs {
    int rdbver;
    rio chunk_rio;
    serverDb *db;
    int rdbflags;
    int current_dbid;
    pthread_mutex_t *insert_mutex;
    long long lru_clock;
    long long now;
} RdbChunkLoadThreadArgs;


int offloadRDBChunkToThread(
    rio *rdb_main_stream,         
    unsigned long chunk_size,     
    int rdbver,                   
    serverDb *current_db,         
    int rdbflags,                 
    pthread_mutex_t *db_insert_mutex,
    int current_dbid,
    long long lru_clock,
    long long now
);
extern _Atomic int rdb_load_thread_error;

#endif // __RDB_THREADS_H__
