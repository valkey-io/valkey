#include "server.h"
#include "tunnel.h"
#include "connection.h"
#include <string.h>

static void freeTunnelSession(tunnelSession *session) {
    if (session == NULL) return;
    --server.stat_active_tunnel_sessions;
    sdsfree(session->host);
    if (session->flag.closed) {
        listNode *ln = listSearchKey(server.tunnels_to_close, session);
        serverAssert(ln != NULL);
        listDelNode(server.tunnels_to_close, ln);
    }
    connClose(session->up_pipe.read_conn);
    connClose(session->up_pipe.write_conn);
    zfree(session);
}

void freeTunnelsInAsyncFreeQueue(void) {
    listIter li;
    listNode *ln;

    listRewind(server.tunnels_to_close, &li);
    while ((ln = listNext(&li)) != NULL) {
        tunnelSession *session = listNodeValue(ln);
        listDelNode(server.tunnels_to_close, ln);
        session->flag.closed = 0;
        freeTunnelSession(session);
    }
}

static void abortTunnelSession(tunnelSession *session) {
    if (session->flag.closed) return;
    session->flag.closed = true;
    serverAssert(listSearchKey(server.tunnels_to_close, session) == NULL);
    listAddNodeTail(server.tunnels_to_close, session);
}

#define ENSURE_CONNECTED_OR_RETURN(conn, session)         \
    do {                                                  \
        if (connGetState(conn) != CONN_STATE_CONNECTED) { \
            abortTunnelSession(session);                  \
            return;                                       \
        }                                                 \
    } while (0)

/* Forward data from one end of the pipe to the other. */
static void pipeTransfer(tunnelSession *session, tunnelPipe *pipe, ConnectionCallbackFunc callback) {
    if (session->flag.closed) return;

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
    tunnelSession *session = connGetPrivateData(conn);
    pipeTransfer(session, &session->up_pipe, upPipeTransfer);
}

static void downPipeTransfer(connection *conn) {
    tunnelSession *session = connGetPrivateData(conn);
    pipeTransfer(session, &session->down_pipe, downPipeTransfer);
}

static void initTunnelPipe(tunnelPipe *pipe, connection *read_conn, connection *write_conn) {
    pipe->read_conn = read_conn;
    pipe->write_conn = write_conn;
    pipe->buffer_pos = 0;
    pipe->buffer_len = 0;
}

/* A connect handler that gets called when a connection to the upstream node
 * gets established.
 */
static void connectHandler(connection *conn) {
    tunnelSession *session = connGetPrivateData(conn);
    ENSURE_CONNECTED_OR_RETURN(conn, session);
    upPipeTransfer(conn);
    downPipeTransfer(conn);
}

static tunnelSession *createTunnelSession(connection *downstream_conn, char *host, int port) {
    ++server.stat_tunnel_sessions;
    ++server.stat_active_tunnel_sessions;
    tunnelSession *session = zmalloc(sizeof(tunnelSession));
    connection *upstream_conn = connCreate(connTypeOfReplication());
    connSetPrivateData(upstream_conn, session);
    connSetPrivateData(downstream_conn, session);
    downstream_conn->state = CONN_STATE_CONNECTED;
    session->host = sdsnew(host);
    session->port = port;
    session->raw_flag = 0;
    initTunnelPipe(&session->up_pipe, downstream_conn, upstream_conn);
    initTunnelPipe(&session->down_pipe, upstream_conn, downstream_conn);
    return session;
}

/* Establish a tunnel session between the given client and a upstream host.
 * Close the client incase of an error.
 */
void establishTunnelOrClose(connection *conn) {
    tunnelSession *session = createTunnelSession(conn, server.primary_host, server.primary_port);
    if (connConnect(session->up_pipe.write_conn, session->host, session->port,
                    server.bind_source_addr, 0, connectHandler) == C_ERR) {
        freeTunnelSession(session);
    }
}
