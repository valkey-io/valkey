/*
 * Copyright (c) 2015-2017, Ieshen Zheng <ieshen.zheng at 163 dot com>
 * Copyright (c) 2020, Nick <heronr1 at gmail dot com>
 * Copyright (c) 2020-2021, Bjorn Svensson <bjorn.a.svensson at est dot tech>
 * Copyright (c) 2021, Red Hat
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef VALKEY_CLUSTER_H
#define VALKEY_CLUSTER_H

#include "async.h"
#include "valkey.h"
#include "visibility.h"

#define VALKEYCLUSTER_SLOTS 16384

#define VALKEY_ROLE_UNKNOWN 0
#define VALKEY_ROLE_PRIMARY 1
#define VALKEY_ROLE_REPLICA 2

/* Events, for event_callback in valkeyClusterOptions. */
#define VALKEYCLUSTER_EVENT_SLOTMAP_UPDATED 1
#define VALKEYCLUSTER_EVENT_READY 2
#define VALKEYCLUSTER_EVENT_FREE_CONTEXT 3

#ifdef __cplusplus
extern "C" {
#endif

struct dict;
struct hilist;
struct valkeyClusterAsyncContext;
struct valkeyTLSContext;

typedef void(valkeyClusterCallbackFn)(struct valkeyClusterAsyncContext *,
                                      void *, void *);
typedef struct valkeyClusterNode {
    char *name;
    char *addr;
    char *host;
    uint16_t port;
    uint8_t role;
    uint8_t pad;
    int failure_count; /* consecutive failing attempts in async */
    valkeyContext *con;
    valkeyAsyncContext *acon;
    int64_t lastConnectionAttempt; /* Timestamp */
    struct hilist *slots;
    struct hilist *replicas;
} valkeyClusterNode;

typedef struct cluster_slot {
    uint32_t start;
    uint32_t end;
    valkeyClusterNode *node; /* Owner of slot region. */
} cluster_slot;

/* Context for accessing a Valkey Cluster */
typedef struct valkeyClusterContext {
    int err;          /* Error flags, 0 when there is no error */
    char errstr[128]; /* String representation of error when applicable */

    /* Configurations */
    int options;                     /* Client configuration */
    int flags;                       /* Config and state flags */
    struct timeval *connect_timeout; /* TCP connect timeout */
    struct timeval *command_timeout; /* Receive and send timeout */
    int max_retry_count;             /* Allowed retry attempts */
    char *username;                  /* Authenticate using user */
    char *password;                  /* Authentication password */
    int select_db;

    struct dict *nodes;        /* Known valkeyClusterNode's */
    uint64_t route_version;    /* Increased when the node lookup table changes */
    valkeyClusterNode **table; /* valkeyClusterNode lookup table */

    struct hilist *requests; /* Outstanding commands (Pipelining) */

    int retry_count;       /* Current number of failing attempts */
    int need_update_route; /* Indicator for valkeyClusterReset() (Pipel.) */

    void *tls; /* Pointer to a valkeyTLSContext when using TLS. */
    int (*tls_init_fn)(struct valkeyContext *, struct valkeyTLSContext *);

    void (*on_connect)(const struct valkeyContext *c, int status);
    void (*event_callback)(const struct valkeyClusterContext *cc, int event,
                           void *privdata);
    void *event_privdata;

} valkeyClusterContext;

/* Context for accessing a Valkey Cluster asynchronously */
typedef struct valkeyClusterAsyncContext {
    /* Hold the regular context. */
    valkeyClusterContext cc;

    int err;      /* Error flag, 0 when there is no error,
                   * a copy of cc->err for convenience. */
    char *errstr; /* String representation of error when applicable,
                   * always pointing to cc->errstr[] */

    int64_t lastSlotmapUpdateAttempt; /* Timestamp */

    /* Attach function for an async library. */
    int (*attach_fn)(valkeyAsyncContext *ac, void *attach_data);
    void *attach_data;

    /* Called when either the connection is terminated due to an error or per
     * user request. The status is set accordingly (VALKEY_OK, VALKEY_ERR). */
    valkeyDisconnectCallback *onDisconnect;

    /* Called when the first write event was received. */
    valkeyConnectCallback *onConnect;

} valkeyClusterAsyncContext;

/* --- Opaque types --- */

/* 72 bytes needed when using Valkey's dict. */
typedef uint64_t valkeyClusterNodeIterator[9];

/* --- Configuration options --- */

/* Enable slotmap updates using the command CLUSTER NODES.
 * Default is the CLUSTER SLOTS command. */
#define VALKEY_OPT_USE_CLUSTER_NODES 0x1000
/* Enable parsing of replica nodes. Currently not used, but the
 * information is added to its primary node structure. */
#define VALKEY_OPT_USE_REPLICAS 0x2000
/* Use a blocking slotmap update after an initial async connect. */
#define VALKEY_OPT_BLOCKING_INITIAL_UPDATE 0x4000

typedef struct {
    const char *initial_nodes;             /* Initial cluster node address(es). */
    int options;                           /* Bit field of VALKEY_OPT_xxx */
    const struct timeval *connect_timeout; /* Timeout value for connect, no timeout if NULL. */
    const struct timeval *command_timeout; /* Timeout value for commands, no timeout if NULL. */
    const char *username;                  /* Authentication username. */
    const char *password;                  /* Authentication password. */
    int max_retry;                         /* Allowed retry attempts. */

    /* Select a logical database after a successful connect.
     * Default 0, i.e. the SELECT command is not sent. */
    int select_db;

    /* Common callbacks. */

    /* A hook to get notified when certain events occur. The `event` is set to
     * VALKEYCLUSTER_EVENT_SLOTMAP_UPDATED when the slot mapping has been updated;
     * VALKEYCLUSTER_EVENT_READY when the slot mapping has been fetched for the first
     * time and the client is ready to accept commands;
     * VALKEYCLUSTER_EVENT_FREE_CONTEXT when the cluster context is being freed. */
    void (*event_callback)(const struct valkeyClusterContext *cc, int event, void *privdata);
    void *event_privdata;

    /* Synchronous API callbacks. */

    /* A hook for connect and reconnect attempts, e.g. for applying additional
     * socket options. This is called just after connect, before TLS handshake and
     * Valkey authentication.
     *
     * On successful connection, `status` is set to `VALKEY_OK` and the file
     * descriptor can be accessed as `c->fd` to apply socket options.
     *
     * On failed connection attempt, this callback is called with `status` set to
     * `VALKEY_ERR`. The `err` field in the `valkeyContext` can be used to find out
     * the cause of the error. */
    void (*connect_callback)(const valkeyContext *c, int status);

    /* Asynchronous API callbacks. */

    /* A hook for asynchronous connect or reconnect attempts.
     *
     * On successful connection, `status` is set to `VALKEY_OK`.
     * On failed connection attempt, this callback is called with `status` set to
     * `VALKEY_ERR`. The `err` field in the `valkeyAsyncContext` can be used to
     * find out the cause of the error. */
    void (*async_connect_callback)(struct valkeyAsyncContext *, int status);

    /* A hook for asynchronous disconnections.
     * Called when either a connection is terminated due to an error or per
     * user request. The status is set accordingly (VALKEY_OK, VALKEY_ERR). */
    void (*async_disconnect_callback)(const struct valkeyAsyncContext *, int status);

    /* Event engine attach function, initiated using a engine specific helper. */
    int (*attach_fn)(valkeyAsyncContext *ac, void *attach_data);
    void *attach_data;

    /* TLS context, initiated using valkeyCreateTLSContext. */
    void *tls;
    int (*tls_init_fn)(struct valkeyContext *, struct valkeyTLSContext *);
} valkeyClusterOptions;

/* --- Synchronous API --- */

LIBVALKEY_API valkeyClusterContext *valkeyClusterConnectWithOptions(const valkeyClusterOptions *options);
LIBVALKEY_API valkeyClusterContext *valkeyClusterConnect(const char *addrs);
LIBVALKEY_API valkeyClusterContext *valkeyClusterConnectWithTimeout(const char *addrs, const struct timeval tv);
LIBVALKEY_API void valkeyClusterFree(valkeyClusterContext *cc);

/* Options configurable in runtime. */
LIBVALKEY_API int valkeyClusterSetOptionTimeout(valkeyClusterContext *cc, const struct timeval tv);

/* Blocking
 * The following functions will block for a reply, or return NULL if there was
 * an error in performing the command.
 */

/* Variadic commands (like printf) */
LIBVALKEY_API void *valkeyClusterCommand(valkeyClusterContext *cc, const char *format, ...);
LIBVALKEY_API void *valkeyClusterCommandToNode(valkeyClusterContext *cc,
                                               valkeyClusterNode *node, const char *format,
                                               ...);
/* Variadic using va_list */
LIBVALKEY_API void *valkeyClustervCommand(valkeyClusterContext *cc, const char *format,
                                          va_list ap);
LIBVALKEY_API void *valkeyClustervCommandToNode(valkeyClusterContext *cc,
                                                valkeyClusterNode *node, const char *format,
                                                va_list ap);
/* Using argc and argv */
LIBVALKEY_API void *valkeyClusterCommandArgv(valkeyClusterContext *cc, int argc,
                                             const char **argv, const size_t *argvlen);
/* Send a Valkey protocol encoded string */
LIBVALKEY_API void *valkeyClusterFormattedCommand(valkeyClusterContext *cc, char *cmd,
                                                  int len);

/* Pipelining
 * The following functions will write a command to the output buffer.
 * A call to `valkeyClusterGetReply()` will flush all commands in the output
 * buffer and read until it has a reply from the first command in the buffer.
 */

/* Variadic commands (like printf) */
LIBVALKEY_API int valkeyClusterAppendCommand(valkeyClusterContext *cc, const char *format,
                                             ...);
LIBVALKEY_API int valkeyClusterAppendCommandToNode(valkeyClusterContext *cc,
                                                   valkeyClusterNode *node,
                                                   const char *format, ...);
/* Variadic using va_list */
LIBVALKEY_API int valkeyClustervAppendCommand(valkeyClusterContext *cc, const char *format,
                                              va_list ap);
LIBVALKEY_API int valkeyClustervAppendCommandToNode(valkeyClusterContext *cc,
                                                    valkeyClusterNode *node,
                                                    const char *format, va_list ap);
/* Using argc and argv */
LIBVALKEY_API int valkeyClusterAppendCommandArgv(valkeyClusterContext *cc, int argc,
                                                 const char **argv, const size_t *argvlen);
/* Use a Valkey protocol encoded string as command */
LIBVALKEY_API int valkeyClusterAppendFormattedCommand(valkeyClusterContext *cc, char *cmd,
                                                      int len);
/* Flush output buffer and return first reply */
LIBVALKEY_API int valkeyClusterGetReply(valkeyClusterContext *cc, void **reply);

/* Reset context after a performed pipelining */
LIBVALKEY_API void valkeyClusterReset(valkeyClusterContext *cc);

/* Update the slotmap by querying any node. */
LIBVALKEY_API int valkeyClusterUpdateSlotmap(valkeyClusterContext *cc);

/* Get the valkeyContext used for communication with a given node.
 * Connects or reconnects to the node if necessary. */
LIBVALKEY_API valkeyContext *valkeyClusterGetValkeyContext(valkeyClusterContext *cc,
                                                           valkeyClusterNode *node);

/* --- Asynchronous API --- */

LIBVALKEY_API valkeyClusterAsyncContext *valkeyClusterAsyncConnectWithOptions(const valkeyClusterOptions *options);
LIBVALKEY_API void valkeyClusterAsyncDisconnect(valkeyClusterAsyncContext *acc);
LIBVALKEY_API void valkeyClusterAsyncFree(valkeyClusterAsyncContext *acc);

/* Commands */
LIBVALKEY_API int valkeyClusterAsyncCommand(valkeyClusterAsyncContext *acc,
                                            valkeyClusterCallbackFn *fn, void *privdata,
                                            const char *format, ...);
LIBVALKEY_API int valkeyClusterAsyncCommandToNode(valkeyClusterAsyncContext *acc,
                                                  valkeyClusterNode *node,
                                                  valkeyClusterCallbackFn *fn, void *privdata,
                                                  const char *format, ...);
LIBVALKEY_API int valkeyClustervAsyncCommand(valkeyClusterAsyncContext *acc,
                                             valkeyClusterCallbackFn *fn, void *privdata,
                                             const char *format, va_list ap);
LIBVALKEY_API int valkeyClusterAsyncCommandArgv(valkeyClusterAsyncContext *acc,
                                                valkeyClusterCallbackFn *fn, void *privdata,
                                                int argc, const char **argv,
                                                const size_t *argvlen);
LIBVALKEY_API int valkeyClusterAsyncCommandArgvToNode(valkeyClusterAsyncContext *acc,
                                                      valkeyClusterNode *node,
                                                      valkeyClusterCallbackFn *fn,
                                                      void *privdata, int argc,
                                                      const char **argv,
                                                      const size_t *argvlen);

/* Use a Valkey protocol encoded string as command */
LIBVALKEY_API int valkeyClusterAsyncFormattedCommand(valkeyClusterAsyncContext *acc,
                                                     valkeyClusterCallbackFn *fn,
                                                     void *privdata, char *cmd, int len);
LIBVALKEY_API int valkeyClusterAsyncFormattedCommandToNode(valkeyClusterAsyncContext *acc,
                                                           valkeyClusterNode *node,
                                                           valkeyClusterCallbackFn *fn,
                                                           void *privdata, char *cmd,
                                                           int len);

/* Get the valkeyAsyncContext used for communication with a given node.
 * Connects or reconnects to the node if necessary. */
LIBVALKEY_API valkeyAsyncContext *valkeyClusterGetValkeyAsyncContext(valkeyClusterAsyncContext *acc,
                                                                     valkeyClusterNode *node);

/* Cluster node iterator functions */
LIBVALKEY_API void valkeyClusterInitNodeIterator(valkeyClusterNodeIterator *iter,
                                                 valkeyClusterContext *cc);
LIBVALKEY_API valkeyClusterNode *valkeyClusterNodeNext(valkeyClusterNodeIterator *iter);

/* Helper functions */
LIBVALKEY_API unsigned int valkeyClusterGetSlotByKey(char *key);
LIBVALKEY_API valkeyClusterNode *valkeyClusterGetNodeByKey(valkeyClusterContext *cc,
                                                           char *key);

#ifdef __cplusplus
}
#endif

#endif /* VALKEY_CLUSTER_H */
