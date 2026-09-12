/*
 * connmem.c -- an in-process connection type with no socket underneath.
 *
 * Bytes are exchanged with the embedding host (a browser page or Node
 * script) through per-connection inbox/outbox buffers, and the host drives
 * the event loop explicitly with tv_tick(). Everything above this layer --
 * client creation, RESP parsing, command execution, reply buffering, MULTI,
 * pub/sub, blocking commands, CLIENT LIST -- is the unmodified server.
 *
 * This is the transport behind "try valkey" (valkey-server compiled to
 * WebAssembly). It only exists in the Emscripten build; elsewhere the
 * register function is a no-op so connTypeInitialize() stays uniform.
 *
 * Copyright (c) Valkey Contributors
 * SPDX-License-Identifier: BSD 3-Clause
 */

#include "server.h"
#include "connection.h"
#include "connhelpers.h"

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <unistd.h>

#define MEMCONN_MAX 1024

typedef struct memConnection {
    connection c;
    int id;     /* index into memConns[], handed to the host */
    sds inbox;  /* bytes written by the host, not yet read by the server */
    sds outbox; /* bytes written by the server, not yet read by the host */
    int host_closed;   /* host called tv_close(): next read returns EOF */
    int server_closed; /* server closed it; struct lingers until the host drains the outbox */
} memConnection;

static ConnectionType CT_Mem;
static memConnection *memConns[MEMCONN_MAX];
static int memListenerPipe[2] = {-1, -1};

static memConnection *memConnFromConn(connection *conn) {
    return (memConnection *)conn;
}

static memConnection *memConnById(int id) {
    if (id < 0 || id >= MEMCONN_MAX) return NULL;
    return memConns[id];
}

/* ---------------------------- ConnectionType ---------------------------- */

static int connMemGetType(void) {
    return CONN_TYPE_MEM;
}

static connection *connCreateMem(void) {
    memConnection *mc = zcalloc(sizeof(*mc));
    mc->c.type = &CT_Mem;
    mc->c.fd = -1;
    mc->c.iovcnt = IOV_MAX;
    mc->id = -1;
    mc->inbox = sdsempty();
    mc->outbox = sdsempty();
    return &mc->c;
}

static connection *connCreateAcceptedMem(int fd, void *priv) {
    UNUSED(fd);
    UNUSED(priv);
    connection *conn = connCreateMem();
    memConnection *mc = memConnFromConn(conn);
    for (int i = 0; i < MEMCONN_MAX; i++) {
        if (!memConns[i]) {
            memConns[i] = mc;
            mc->id = i;
            break;
        }
    }
    if (mc->id == -1) {
        conn->state = CONN_STATE_ERROR;
        conn->last_errno = EMFILE;
    } else {
        conn->state = CONN_STATE_ACCEPTING;
    }
    return conn;
}

static void connMemShutdown(connection *conn) {
    memConnFromConn(conn)->host_closed = 1;
}

static void memConnFree(memConnection *mc) {
    if (mc->id >= 0 && memConns[mc->id] == mc) memConns[mc->id] = NULL;
    sdsfree(mc->inbox);
    sdsfree(mc->outbox);
    zfree(mc);
}

static void connMemClose(connection *conn) {
    memConnection *mc = memConnFromConn(conn);

    /* If called from within a handler, schedule the close but keep the
     * connection until the handler returns (same contract as socket.c). */
    if (connHasRefs(conn)) {
        conn->flags |= CONN_FLAG_CLOSE_SCHEDULED;
        return;
    }
    conn->state = CONN_STATE_CLOSED;
    mc->server_closed = 1;
    /* Like a TCP FIN after the final write: bytes the server queued right
     * before closing (e.g. "-ERR Protocol error") must still reach the host.
     * Keep the struct around until tv_read()/tv_pending() drain it. */
    if (sdslen(mc->outbox) > 0 && mc->id >= 0) return;
    memConnFree(mc);
}

static int connMemAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    if (conn->state != CONN_STATE_ACCEPTING) return C_ERR;
    conn->state = CONN_STATE_CONNECTED;
    if (!callHandler(conn, accept_handler)) return C_ERR;
    return C_OK;
}

static int connMemWrite(connection *conn, const void *data, size_t data_len) {
    memConnection *mc = memConnFromConn(conn);
    mc->outbox = sdscatlen(mc->outbox, data, data_len);
    return (int)data_len;
}

static int connMemWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    int total = 0;
    for (int i = 0; i < iovcnt; i++) total += connMemWrite(conn, iov[i].iov_base, iov[i].iov_len);
    return total;
}

static int connMemRead(connection *conn, void *buf, size_t buf_len) {
    memConnection *mc = memConnFromConn(conn);
    size_t avail = sdslen(mc->inbox);
    if (avail == 0) {
        if (mc->host_closed) {
            conn->state = CONN_STATE_CLOSED;
            return 0; /* EOF */
        }
        errno = EAGAIN;
        return -1;
    }
    size_t n = avail < buf_len ? avail : buf_len;
    memcpy(buf, mc->inbox, n);
    sdsrange(mc->inbox, n, -1);
    return (int)n;
}

/* Handlers are stored only; there is no fd to register with ae. The host
 * pump (tv_tick) invokes them when there is work. */
static int connMemSetWriteHandler(connection *conn, ConnectionCallbackFunc func, int barrier) {
    conn->write_handler = func;
    if (barrier)
        conn->flags |= CONN_FLAG_WRITE_BARRIER;
    else
        conn->flags &= ~CONN_FLAG_WRITE_BARRIER;
    return C_OK;
}

static int connMemSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    conn->read_handler = func;
    return C_OK;
}

static const char *connMemGetLastError(connection *conn) {
    return strerror(conn->last_errno);
}

static int connMemAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    memConnection *mc = memConnFromConn(conn);
    snprintf(ip, ip_len, "%s", remote ? "mem" : "wasm");
    if (port) *port = remote ? mc->id : 0;
    return 0;
}

static int connMemIsLocal(connection *conn) {
    UNUSED(conn);
    return 1;
}

/* The listener never fires; it exists so the server sees one listening fd
 * and the accept plumbing (createSocketAcceptHandler) stays uniform. */
static int connMemListen(connListener *listener) {
    if (memListenerPipe[0] == -1 && pipe(memListenerPipe) == -1) return C_ERR;
    listener->fd[0] = memListenerPipe[0];
    listener->count = 1;
    return C_OK;
}

static void connMemCloseListener(connListener *listener) {
    if (listener->count && listener->fd[0] != -1) aeDeleteFileEvent(server.el, listener->fd[0], AE_READABLE);
    listener->count = 0;
}

static void connMemAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(fd);
    UNUSED(privdata);
    UNUSED(mask);
    /* unreachable: nothing ever writes to the listener pipe */
}

static void connMemEventHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    UNUSED(el);
    UNUSED(fd);
    UNUSED(clientData);
    UNUSED(mask);
    /* unreachable: mem connections register no ae file events */
}

static int connMemUnsupported(connection *conn, const char *addr, int port, long long timeout) {
    UNUSED(addr);
    UNUSED(port);
    UNUSED(timeout);
    conn->state = CONN_STATE_ERROR;
    conn->last_errno = ENOTSUP;
    return C_ERR;
}

static int connMemConnect(connection *conn,
                          const char *addr,
                          int port,
                          const char *src_addr,
                          int multipath,
                          ConnectionCallbackFunc connect_handler) {
    UNUSED(src_addr);
    UNUSED(multipath);
    UNUSED(connect_handler);
    return connMemUnsupported(conn, addr, port, 0);
}

static ssize_t connMemSyncUnsupported(connection *conn, char *ptr, ssize_t size, long long timeout) {
    UNUSED(ptr);
    UNUSED(size);
    UNUSED(timeout);
    conn->last_errno = ENOTSUP;
    return -1;
}

static ConnectionType CT_Mem = {
    .get_type = connMemGetType,

    .init = NULL,
    .cleanup = NULL,
    .configure = NULL,

    .ae_handler = connMemEventHandler,
    .accept_handler = connMemAcceptHandler,
    .addr = connMemAddr,
    .is_local = connMemIsLocal,
    .listen = connMemListen,
    .closeListener = connMemCloseListener,

    .conn_create = connCreateMem,
    .conn_create_accepted = connCreateAcceptedMem,
    .shutdown = connMemShutdown,
    .close = connMemClose,

    .connect = connMemConnect,
    .blocking_connect = connMemUnsupported,
    .accept = connMemAccept,

    .write = connMemWrite,
    .writev = connMemWritev,
    .read = connMemRead,
    .set_write_handler = connMemSetWriteHandler,
    .set_read_handler = connMemSetReadHandler,
    .get_last_error = connMemGetLastError,
    .sync_write = connMemSyncUnsupported,
    .sync_read = connMemSyncUnsupported,
    .sync_readline = connMemSyncUnsupported,

    .has_pending_data = NULL,
    .process_pending_data = NULL,
    .postpone_update_state = NULL,
    .update_state = NULL,

    .connIntegrityChecked = NULL,
    .is_closing = NULL,
};

int RegisterConnectionTypeMem(void) {
    return connTypeRegister(&CT_Mem);
}

/* ----------------------------- Host driver API --------------------------- */
/* Exported to JS. Pointers cross the boundary as numbers (BigInt in a
 * memory64 build); the JS side uses Module.HEAPU8 for the byte buffers. */

/* Open a new client connection. Returns its id, or -1. */
EMSCRIPTEN_KEEPALIVE int tv_connect(void) {
    struct ClientFlags flags = {0};
    connection *conn = connCreateAcceptedMem(-1, NULL);
    int id = memConnFromConn(conn)->id;
    if (id == -1) {
        connClose(conn);
        return -1;
    }
    acceptCommonHandler(conn, flags, NULL);
    /* acceptCommonHandler may have closed it (maxclients, ACL...). */
    return memConns[id] == memConnFromConn(conn) ? id : -1;
}

/* Feed bytes from the host into connection 'id' and run its read handler
 * until the server has drained what it is willing to take right now. */
EMSCRIPTEN_KEEPALIVE int tv_write(int id, const uint8_t *buf, int len) {
    memConnection *mc = memConnById(id);
    if (!mc || mc->c.state != CONN_STATE_CONNECTED) return -1;
    mc->inbox = sdscatlen(mc->inbox, buf, len);

    /* Emulate level-triggered readiness: keep calling the handler while it
     * makes progress. A handler that declines to read (blocked client,
     * paused client) leaves the bytes queued for a later tick. */
    while (memConns[id] == mc && mc->c.state == CONN_STATE_CONNECTED && mc->c.read_handler && sdslen(mc->inbox) > 0) {
        size_t before = sdslen(mc->inbox);
        if (!callHandler(&mc->c, mc->c.read_handler)) break; /* closed */
        if (memConns[id] != mc || sdslen(mc->inbox) >= before) break;
    }
    return len;
}

/* Run one iteration of the event loop: beforeSleep (which flushes replies
 * into outboxes), time events (serverCron, expiry, blocked-client timeouts),
 * and any pending mem read/write handlers. Never blocks. */
EMSCRIPTEN_KEEPALIVE void tv_tick(void) {
    for (int i = 0; i < MEMCONN_MAX; i++) {
        memConnection *mc = memConns[i];
        if (!mc || mc->c.state != CONN_STATE_CONNECTED) continue;
        if (mc->c.write_handler && !callHandler(&mc->c, mc->c.write_handler)) continue;
        if (memConns[i] != mc) continue;
        if (mc->c.read_handler && (sdslen(mc->inbox) > 0 || mc->host_closed)) callHandler(&mc->c, mc->c.read_handler);
    }
    aeProcessEvents(server.el, AE_ALL_EVENTS | AE_DONT_WAIT | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP);
}

/* Bytes waiting in connection 'id's outbox (-1 if the connection is gone). */
EMSCRIPTEN_KEEPALIVE int tv_pending(int id) {
    memConnection *mc = memConnById(id);
    if (!mc) return -1;
    if (mc->server_closed && sdslen(mc->outbox) == 0) {
        memConnFree(mc);
        return -1;
    }
    return (int)sdslen(mc->outbox);
}

/* Copy up to 'cap' outbox bytes into buf. Returns the count, -1 if gone. */
EMSCRIPTEN_KEEPALIVE int tv_read(int id, uint8_t *buf, int cap) {
    memConnection *mc = memConnById(id);
    if (!mc) return -1;
    size_t n = sdslen(mc->outbox);
    if ((int)n > cap) n = cap;
    memcpy(buf, mc->outbox, n);
    sdsrange(mc->outbox, n, -1);
    if (mc->server_closed && sdslen(mc->outbox) == 0) memConnFree(mc);
    return (int)n;
}

/* Host hangs up: the server sees EOF on its next read and frees the client. */
EMSCRIPTEN_KEEPALIVE void tv_close(int id) {
    memConnection *mc = memConnById(id);
    if (!mc) return;
    if (mc->server_closed) {
        memConnFree(mc);
        return;
    }
    mc->host_closed = 1;
    if (mc->c.read_handler) callHandler(&mc->c, mc->c.read_handler);
}

/* Split a command line exactly as valkey-cli does (sdssplitargs: quotes,
 * escapes, hex) and return it RESP-encoded in a buffer the host must release
 * with tv_free(); *len_out receives its length (arguments may contain NUL).
 * Returns NULL for unbalanced quotes, and a zero-length buffer for a blank
 * line. */
EMSCRIPTEN_KEEPALIVE char *tv_encode_command(const char *line, int *len_out) {
    int argc = 0;
    sds *argv = sdssplitargs(line, &argc);
    if (!argv) return NULL;
    sds out = sdsempty();
    if (argc > 0) {
        out = sdscatfmt(out, "*%i\r\n", argc);
        for (int i = 0; i < argc; i++) {
            out = sdscatfmt(out, "$%u\r\n", (unsigned long)sdslen(argv[i]));
            out = sdscatsds(out, argv[i]);
            out = sdscatlen(out, "\r\n", 2);
        }
    }
    sdsfreesplitres(argv, argc);
    *len_out = (int)sdslen(out);
    char *ret = zmalloc(sdslen(out) + 1);
    memcpy(ret, out, sdslen(out) + 1);
    sdsfree(out);
    return ret;
}

/* Release a buffer returned by tv_encode_command(). */
EMSCRIPTEN_KEEPALIVE void tv_free(void *p) {
    zfree(p);
}

#else /* !__EMSCRIPTEN__ */

int RegisterConnectionTypeMem(void) {
    return C_ERR;
}

#endif
