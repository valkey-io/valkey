#include "server.h"
#include "tunnel.h"
#include "connection.h"
#include <string.h>

static int isTunnelClosed(tunnelSession *session) {
     return !session || !session->downstream_client ||
          session->downstream_client->flag.close_asap ||
          !session->upstream_client ||
          session->upstream_client->flag.close_asap;
}

void freeTunnelSession(tunnelSession *session) {
    if (session == NULL) return;
    --server.stat_active_tunnel_sessions;
    sdsfree(session->host);
    if (session->downstream_client) {
        freeClientAsync(session->downstream_client);
        session->downstream_client->tunnel_session = NULL;
    }
    freeClientAsync(session->upstream_client);
    session->upstream_client->tunnel_session = NULL;
    sdsfree(session->cmd);
    zfree(session);
}

static void abortTunnel(tunnelSession *session) {
    freeClientAsync(session->downstream_client);
    session->downstream_client = NULL;
}

#define ENSURE_CONNECTED_OR_RETURN(conn, session)         \
    do {                                                  \
        if (connGetState(conn) != CONN_STATE_CONNECTED) { \
            abortTunnel(session);                         \
            return;                                       \
        }                                                 \
    } while (0)

/* Forward data from one end of the pipe to the other. */
static void pipeTransfer(tunnelSession *session, tunnelPipe *pipe, ConnectionCallbackFunc callback) {
    if (isTunnelClosed(session)) return;

    do {
        if (pipe->buffer_len == 0) {
            ssize_t nread = connRead(pipe->read_conn, pipe->buffer, TUNNEL_BUFSIZE);
            if (nread <= 0) {
                ENSURE_CONNECTED_OR_RETURN(pipe->read_conn, session);
                break;
            }
            pipe->buffer_len = nread;
        }
        if (pipe->buffer_pos != pipe->buffer_len) {
            pipe->buffer_pos += connWrite(pipe->write_conn, pipe->buffer + pipe->buffer_pos, pipe->buffer_len - pipe->buffer_pos);
            ENSURE_CONNECTED_OR_RETURN(pipe->write_conn, session);
            if (pipe->buffer_pos != pipe->buffer_len) break;
        }
        pipe->buffer_len = 0;
        pipe->buffer_pos = 0;
    } while (1);

    if (pipe->buffer_len == 0) {
        connSetWriteHandler(pipe->write_conn, NULL);
        connSetReadHandler(pipe->read_conn, callback);
        return;
    }
    connSetWriteHandler(pipe->write_conn, callback);
    connSetReadHandler(pipe->read_conn, NULL);
}

static void upPipeTransfer(connection *conn) {
    tunnelSession *session = ((client *)connGetPrivateData(conn))->tunnel_session;
    pipeTransfer(session, &session->up_pipe, upPipeTransfer);
}

static void downPipeTransfer(connection *conn) {
    tunnelSession *session = ((client *)connGetPrivateData(conn))->tunnel_session;
    pipeTransfer(session, &session->down_pipe, downPipeTransfer);
}

static void initTunnelPipe(tunnelPipe *pipe, connection *read_conn, connection *write_conn) {
    pipe->read_conn = read_conn;
    pipe->write_conn = write_conn;
    pipe->buffer_pos = 0;
    pipe->buffer_len = 0;
}

static ConnectionType *connTypeOfTunneling(void) {
    if (server.tls_replication) {
        return connectionTypeTls();
    }
    return connectionTypeTcp();
}

static const char ok_str[] = "+OK\r\n";
static const size_t ok_str_len = sizeof(ok_str) - 1; // 5
static const char queued_str[] = "+QUEUED\r\n";
static const size_t queued_str_len = sizeof(queued_str) - 1; // 9

static int isReplyError(const char *buf, size_t buf_len) {
    return (buf_len > 0 && buf[0] == '-');
}

static int isEntireReplyReceived(size_t buf_len, size_t len) {
    return buf_len >= len;
}

static int isExpectedReply(const char *buf, const char *str, size_t len) {
    return (memcmp(buf, str, len) == 0);
}

static size_t expectedReplyLength(int multi_cnt) {
    size_t ret = ok_str_len + multi_cnt * queued_str_len;
    return multi_cnt ? ret + ok_str_len : ret;
}

static int verifyReadReply(tunnelSession *session, const char *buf, size_t buf_offset, size_t buf_len, const char *expected_str, size_t expected_str_len) {
    if (buf_offset >= buf_len) return 0;
    buf += buf_offset;
    buf_len -= buf_offset;
    if (isReplyError(buf, buf_len)) {
        abortTunnel(session);
        return 0;
    }
    if (!isEntireReplyReceived(buf_len, expected_str_len)) return 0;

    if (!isExpectedReply(buf, expected_str, expected_str_len)) {
        abortTunnel(session);
        return 0;
    }
    return 1;
}

static void readSetupCmdsReply(connection *conn) {
    tunnelSession *session = ((client *)connGetPrivateData(conn))->tunnel_session;
    if (isTunnelClosed(session)) return;
    tunnelPipe *pipe = &session->down_pipe;
    ssize_t nread = connRead(pipe->read_conn, pipe->buffer + pipe->buffer_pos, TUNNEL_BUFSIZE - pipe->buffer_pos);
    if (nread <= 0) {
        ENSURE_CONNECTED_OR_RETURN(conn, session);
        return;
    }
    pipe->buffer_pos += nread;
    if (!verifyReadReply(session, pipe->buffer, 0, pipe->buffer_pos, ok_str, ok_str_len)) return;
    if (session->multi_cnt) {
        size_t buf_offset = ok_str_len;
        if (!verifyReadReply(session, pipe->buffer, buf_offset, pipe->buffer_pos, ok_str, ok_str_len)) return;
        buf_offset += ok_str_len;
        for (int j = 0; j < session->multi_cnt; j++) {
            if (!verifyReadReply(session, pipe->buffer, buf_offset, pipe->buffer_pos, queued_str, queued_str_len)) return;
            buf_offset += queued_str_len;
        }
    }
    size_t expected_len = expectedReplyLength(session->multi_cnt);
    if (pipe->buffer_pos == expected_len) {
        pipe->buffer_pos = 0;
    } else {
        pipe->buffer_len = pipe->buffer_pos;
        pipe->buffer_pos = expected_len;
    }
    upPipeTransfer(conn);
    downPipeTransfer(conn);
}
/* Send the pipeline commands upstream to align the connection with the downstream
 * RESP protocol version and the downstream user.
 */
static void upstreamCmds(connection *conn) {
    tunnelSession *session = ((client *)connGetPrivateData(conn))->tunnel_session;
    if (isTunnelClosed(session)) return;

    if (sdslen(session->cmd) > session->up_pipe.buffer_pos) {
        const char *buf = session->cmd + session->up_pipe.buffer_pos;
        size_t len = sdslen(session->cmd) - session->up_pipe.buffer_pos;
        session->up_pipe.buffer_pos += connWrite(session->up_pipe.write_conn, buf, len);
        ENSURE_CONNECTED_OR_RETURN(conn, session);
        if (sdslen(session->cmd) > session->up_pipe.buffer_pos) return;
    }
    sdsfree(session->cmd);
    session->cmd = NULL;
    session->up_pipe.buffer_pos = 0;
    connSetWriteHandler(conn, NULL);
    connSetReadHandler(conn, readSetupCmdsReply);
    readSetupCmdsReply(conn);
}

/* A connect handler that gets called when a connection to the upstream node
 * gets established.
 */
static void connectHandler(connection *conn) {
    tunnelSession *session = ((client *)connGetPrivateData(conn))->tunnel_session;
    ENSURE_CONNECTED_OR_RETURN(conn, session);
    if (isTunnelClosed(session)) return;
    connSetWriteHandler(conn, upstreamCmds);
    upstreamCmds(conn);
}

static sds reconstructCommand(robj **argv, int argc, sds resp) {
    resp = sdscatfmt(resp, "*%i\r\n", argc);

    for (int j = 0; j < argc; j++) {
        robj *arg = argv[j];
        size_t len = sdslen(arg->ptr);
        resp = sdscatfmt(resp, "$%U\r\n", (unsigned long long)len);
        resp = sdscatlen(resp, arg->ptr, len);
        resp = sdscatlen(resp, "\r\n", 2);
    }
    return resp;
}

static sds reconstructClientCommand(client *c, sds resp) {
    if (c->mstate) {
        resp = sdscatlen(resp, "*1\r\n", 4);
        resp = sdscatlen(resp, "$5\r\nMULTI\r\n", 11);
        for (int j = 0; j < c->mstate->count; j++) {
            multiCmd *mc = c->mstate->commands + j;
            resp = reconstructCommand(mc->argv, mc->argc, resp);
        }
    }
    return reconstructCommand(c->argv, c->argc, resp);
}

static sds serializeTunnelCmd(client *c, sds resp) {
    resp = sdscatlen(resp, "*3\r\n", 4);
    resp = sdscatlen(resp, "$6\r\nTUNNEL\r\n", 12);

    size_t len = sdslen(c->user->name);
    resp = sdscatfmt(resp, "$%U\r\n", (unsigned long long)len);
    resp = sdscatlen(resp, c->user->name, len);
    resp = sdscatlen(resp, "\r\n", 2);

    char resp_version[16];
    snprintf(resp_version, sizeof(resp_version), "%d", c->resp);
    len = strlen(resp_version);
    resp = sdscatfmt(resp, "$%U\r\n", (unsigned long long)len);
    resp = sdscatlen(resp, resp_version, len);
    resp = sdscatlen(resp, "\r\n", 2);
    return resp;
}

static sds constructPipelineCommands(client *c) {
    sds resp = sdsempty();
    resp = serializeTunnelCmd(c, resp);
    resp = reconstructClientCommand(c, resp);
    return resp;
}

static tunnelSession *createTunnelSession(client *c, char *host, int port) {
    serverAssert(!c->tunnel_session);
    ++server.stat_tunnel_sessions;
    ++server.stat_active_tunnel_sessions;
    tunnelSession *session = zmalloc(sizeof(tunnelSession));
    connection *conn = connCreate(connTypeOfTunneling());
    session->upstream_client = createClient(conn);
    session->upstream_client->tunnel_session = session;
    connSetPrivateData(conn, session->upstream_client);
    session->host = sdsnew(host);
    session->port = port;
    session->downstream_client = c;
    session->downstream_client->tunnel_session = session;
    initTunnelPipe(&session->up_pipe, c->conn, conn);
    initTunnelPipe(&session->down_pipe, conn, c->conn);
    session->cmd = constructPipelineCommands(c);
    session->multi_cnt = (!c->mstate) ? 0 : c->mstate->count;
    return session;
}

/* Establish a tunnel session between the given client and a upstream host.
 * Close the client incase of an error.
 */
void establishTunnelOrClose(client *c) {
    tunnelSession *session = createTunnelSession(c, server.primary_host, server.primary_port);
    if (connConnect(session->up_pipe.write_conn, session->host, session->port,
                         server.bind_source_addr, 0, connectHandler) == C_ERR) {
        freeTunnelSession(session);
    }
}
