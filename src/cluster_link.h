#ifndef CLUSTER_LINK_H
#define CLUSTER_LINK_H

#include "connection.h"
#include "adlist.h"

typedef struct clusterNode clusterNode;

#define RCVBUF_INIT_LEN 1024
#define RCVBUF_MIN_READ_LEN 14
#define RCVBUF_MAX_PREALLOC (1 << 20) /* 1MB */

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
} clusterLink;

/* Cluster link flags */
#define CLUSTER_LINK_EXTENSIONS_SUPPORTED (1 << 0)
#define linkSupportsExtension(link) ((link)->flags & CLUSTER_LINK_EXTENSIONS_SUPPORTED)

clusterLink *createClusterLink(clusterNode *node);
void freeClusterLink(clusterLink *link);
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

struct client;
void clusterCommandLinks(struct client *c);
int clusterLinkDebugCommand(struct client *c);
const char **clusterLinkDebugHelp(void);

#endif /* CLUSTER_LINK_H */
