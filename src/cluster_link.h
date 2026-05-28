#ifndef CLUSTER_LINK_H
#define CLUSTER_LINK_H

#include "connection.h"
#include "adlist.h"

typedef struct clusterNode clusterNode;

#define RCVBUF_INIT_LEN 1024
#define RCVBUF_MIN_READ_LEN 14
#define RCVBUF_MAX_PREALLOC (1 << 20) /* 1MB */

/* I/O state for threaded cluster bus offload. */
typedef enum {
    CLUSTER_LINK_IO_IDLE = 0,
    CLUSTER_LINK_IO_PENDING,
} clusterLinkIOState;

/* Result codes for cluster I/O jobs. */
typedef enum {
    CLUSTER_IO_OK = 0,
    CLUSTER_IO_BAD_HEADER,
    CLUSTER_IO_BAD_LENGTH,
    CLUSTER_IO_READ_ERROR,
    CLUSTER_IO_EOF,
    CLUSTER_IO_WRITE_ERROR,
} clusterIOResult;

/* How many times per node_timeout we attempt to reconnect to each node. */
#define NODE_CONNECTION_RETRIES_PER_TIMEOUT 10

/* A refcounted block of bytes queued for sending on a cluster link.
 * The data member is uint64_t to ensure alignment for protocol
 * implementations that cast it to message structs with uint64_t fields. */
typedef struct {
    size_t totlen; /* Total allocation size including this header */
    size_t len;    /* Bytes of data to send (<= totlen - sizeof(header)) */
    int refcount;
    uint64_t data[];
} clusterMsgSendBlock;

/* clusterLink encapsulates everything needed to talk with a remote node. */
typedef struct clusterLink {
    mstime_t ctime;                        /* Link creation time */
    connection *conn;                      /* Connection to remote node */
    list *send_msg_queue;                  /* List of clusterSendBlock */
    size_t head_msg_send_offset;           /* Bytes already sent of head block */
    unsigned long long send_msg_queue_mem; /* Memory in bytes used by queue */
    char *rcvbuf;                          /* Packet reception buffer */
    size_t rcvbuf_len;                     /* Used size of rcvbuf */
    size_t rcvbuf_alloc;                   /* Allocated size of rcvbuf */
    clusterNode *node;                     /* Node related to this link */
    int inbound;                           /* 1 if inbound link */
    int flags;                             /* CLUSTER_LINK_... */

    /* Threaded I/O state (main-thread owned, except where noted) */
    int io_read_state;         /* clusterLinkIOState: read job state */
    int io_write_state;        /* clusterLinkIOState: write job state */
    int async_close;           /* 1 if teardown requested while jobs in flight */
    int io_refs;               /* Count of in-flight I/O jobs */
    clusterIOResult io_result; /* Result code from last I/O job (written by I/O thread).
                                * Shared by read/write jobs because they are mutually exclusive per link. */

    /* Async write snapshot/result */
    listNode *io_last_send_block; /* Last queue node visible to current write job */
    size_t io_head_offset;        /* Snapshot/result offset into queue head */
    int io_nodes_sent;            /* Number of fully-sent head nodes (set by I/O thread) */

    /* Pre-dispatch rcvbuf_alloc for memory accounting on completion */
    size_t rcvbuf_alloc_at_dispatch; /* rcvbuf_alloc when read job was dispatched */

    /* Async read framing/result */
    size_t io_complete_bytes;   /* Bytes at the start of rcvbuf framed as complete packets */
    size_t io_complete_packets; /* Number of complete packets in io_complete_bytes */

    /* Read/write fairness */
    int io_read_deferred; /* Read skipped while a write was in flight; next write dispatch yields */
} clusterLink;

/* Cluster link flags */
#define CLUSTER_LINK_EXTENSIONS_SUPPORTED (1 << 0)
#define linkSupportsExtension(link) ((link)->flags & CLUSTER_LINK_EXTENSIONS_SUPPORTED)

clusterLink *createClusterLink(clusterNode *node);
int freeClusterLink(clusterLink *link);
void clusterMsgSendBlockDecrRefCount(void *ptr);
clusterMsgSendBlock *clusterAllocMsgSendBlock(uint32_t msglen);
void setClusterNodeToInboundClusterLink(clusterNode *node, clusterLink *link);
void clusterWriteHandler(connection *conn);

char *clusterLinkGetNodeName(clusterLink *link);
char *clusterLinkGetHumanNodeName(clusterLink *link);
int nodeIp2String(char *buf, clusterLink *link, char *announced_ip);
void clusterLinkSendBlock(clusterLink *link, clusterMsgSendBlock *msgblock);
int freeClusterLinkOnBufferLimitReached(clusterLink *link);

void clusterReadHandler(connection *conn);
void clusterLinkConnectHandler(connection *conn);
void clusterConnectNodes(void);
void clusterListenerInit(void);
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);

/* Cluster I/O completion handlers called from processIOThreadsResponses().
 * For read/write, the tagged pointer is the clusterLink* itself.
 * For accept, the tagged pointer is the connection* (no clusterLink exists yet). */
void clusterHandleReadCompletion(clusterLink *link);
void clusterHandleWriteCompletion(clusterLink *link);
void clusterHandleAcceptCompletion(connection *conn);
void clusterConnAcceptHandler(connection *conn);
void clusterReadJob(clusterLink *link);
void clusterWriteJob(clusterLink *link);
void clusterAcceptJob(connection *conn);
void clusterFindCompletePackets(char *rcvbuf,
                                size_t rcvbuf_len,
                                size_t *complete_bytes,
                                size_t *complete_packets,
                                clusterIOResult *result);

void testOnlyFreeClusterLinkOnBufferLimitReached(clusterLink *link);

struct client;
void clusterCommandLinks(struct client *c);
int clusterLinkDebugCommand(struct client *c);

#endif /* CLUSTER_LINK_H */
