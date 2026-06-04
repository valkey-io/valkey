#include "server.h"
#include "tunnel.h"
#include "connection.h"
#include <string.h>

#define PIPE_BUFFER_SIZE (8*1024)  /* 88KB buffer limit, adjust as needed */

/* Forward declarations */
static void upPipeTransfer(connection *conn);
static void downPipeTransfer(connection *conn);

/* Initialize a tunnel pipe, setting connections and creating an empty sds buffer. */
static void initTunnelPipe(tunnelPipe *pipe, connection *read_conn, connection *write_conn) {
    pipe->read_conn = read_conn;
    pipe->write_conn = write_conn;
    pipe->buffer = sdsnewlen(NULL, PIPE_BUFFER_SIZE);
    sdssetlen(pipe->buffer, 0);
}

/* Frees the resources associated with a single tunnel session. */
static void freeTunnelSession(tunnelSession *session) {
    if (session == NULL) return;

    /* Remove from active tunnel_sessions list */
    listNode *ln = listSearchKey(server.active_tunnel_sessions, session);
    if (ln) listDelNode(server.active_tunnel_sessions, ln);

    /* If it’s also in the to-close list, remove from that too */
    if (session->state == TUNNEL_STATE_CLOSING) {
        listNode *ln_close = listSearchKey(server.tunnels_to_close, session);
        if (ln_close) listDelNode(server.tunnels_to_close, ln_close);
    }

    sdsfree(session->host);
    sdsfree(session->up_pipe.buffer);
    sdsfree(session->down_pipe.buffer);
    connClose(session->up_pipe.read_conn);
    connClose(session->up_pipe.write_conn);
    zfree(session);
}

/* Process the async queue of tunnels marked for closing. */
void freeTunnelsInAsyncFreeQueue(void) {
    listIter li;
    listNode *ln;

    listRewind(server.tunnels_to_close, &li);
    while ((ln = listNext(&li)) != NULL) {
        tunnelSession *session = listNodeValue(ln);
        listDelNode(server.tunnels_to_close, ln); /* Remove before freeing */
        freeTunnelSession(session);
    }
}

/* Mark a tunnel for closing and add it to the async cleanup queue. */
static void abortTunnelSession(tunnelSession *session) {
    if (session->state == TUNNEL_STATE_CLOSING) return;

    session->state = TUNNEL_STATE_CLOSING;
    serverAssert(listSearchKey(server.tunnels_to_close, session) == NULL);
    listAddNodeTail(server.tunnels_to_close, session);
}

/* Marks all active tunnel sessions for closing. */
void abortAllTunnelSessions(void) {
    /* Reset tunneling parameters */
    server.tunnel_activation_time = 0;

    listIter li;
    listNode *ln;

    listRewind(server.active_tunnel_sessions, &li);
    while ((ln = listNext(&li)) != NULL) {
        tunnelSession *session = listNodeValue(ln);
        if (session->state != TUNNEL_STATE_CLOSING) {
            abortTunnelSession(session);
        }
    }
}

/* A macro to ensure a connection is valid, otherwise aborts the session. */
#define ENSURE_CONNECTED_OR_RETURN(conn, session)         \
    do {                                                  \
        if (connGetState(conn) != CONN_STATE_CONNECTED) { \
            abortTunnelSession(session);                  \
            return;                                       \
        }                                                 \
    } while (0)

/*
 * Reads data from one connection and writes it to another using an sds buffer.
 * It intelligently sets read/write handlers to avoid busy-waiting.
 */
static void pipeTransfer(tunnelSession *session, tunnelPipe *pipe, ConnectionCallbackFunc callback) {
    if (session->state == TUNNEL_STATE_CLOSING) return;

    /* Read from read_conn if our buffer has space */
    if (sdsavail(pipe->buffer) > 0) {
        ssize_t nread = connRead(pipe->read_conn,
                                 pipe->buffer + sdslen(pipe->buffer),
                                 sdsavail(pipe->buffer));

        if (nread > 0) {
            sdsIncrLen(pipe->buffer, nread);
        } else if (nread <= 0) {
            ENSURE_CONNECTED_OR_RETURN(pipe->read_conn, session);
        }
    }

    /* Write data from our buffer to the write_conn. */
    if (sdslen(pipe->buffer) > 0) {
        ssize_t nwritten = connWrite(pipe->write_conn, pipe->buffer, sdslen(pipe->buffer));
        if (nwritten > 0) {
            sdsrange(pipe->buffer, nwritten, -1); /* Trim written bytes */
        }
        ENSURE_CONNECTED_OR_RETURN(pipe->write_conn, session);
    }

    connSetReadHandler(pipe->read_conn, (sdsavail(pipe->buffer) > 0) ? callback : NULL);
    connSetWriteHandler(pipe->write_conn, (sdslen(pipe->buffer) > 0) ? callback : NULL);
}

/* Callback for transferring data from downstream to upstream.*/
static void upPipeTransfer(connection *conn) {
    tunnelSession *session = connGetPrivateData(conn);
    pipeTransfer(session, &session->up_pipe, upPipeTransfer);
}

/* Callback for transferring data from upstream to downstream. */
static void downPipeTransfer(connection *conn) {
    tunnelSession *session = connGetPrivateData(conn);
    pipeTransfer(session, &session->down_pipe, downPipeTransfer);
}

/*
 * The connect handler called when the upstream connection is established.
 * It transitions the tunnel state and starts the data transfer.
 */
static void connectHandler(connection *conn) {
    tunnelSession *session = connGetPrivateData(conn);
    if (session->state == TUNNEL_STATE_CLOSING) return; /* Aborted while connecting */

    ENSURE_CONNECTED_OR_RETURN(conn, session);
    session->state = TUNNEL_STATE_ESTABLISHED; /* Transition to established */

    /* Start data transfer in both directions. */
    upPipeTransfer(session->up_pipe.read_conn);
    downPipeTransfer(session->down_pipe.read_conn);
}

/* Creates a new tunnel session object and initializes its pipes. */
static tunnelSession *createTunnelSession(connection *downstream_conn, char *host, int port) {
    ++server.stat_tunnel_sessions;
    tunnelSession *session = zmalloc(sizeof(tunnelSession));
    connection *upstream_conn = connCreate(connTypeOfReplication());

    connSetPrivateData(upstream_conn, session);
    connSetPrivateData(downstream_conn, session);
    downstream_conn->state = CONN_STATE_CONNECTED;

    session->host = sdsnew(host);
    session->port = port;
    session->state = TUNNEL_STATE_CONNECTING; /* Initial state */
    session->creation_time = mstime();

    initTunnelPipe(&session->up_pipe, downstream_conn, upstream_conn);
    initTunnelPipe(&session->down_pipe, upstream_conn, downstream_conn);
    listAddNodeTail(server.active_tunnel_sessions, session);
    return session;
}

/*
 * Entry point: Establishes a tunnel session for a client connection.
 * Closes the client connection if the tunnel cannot be established.
 */
void establishTunnelOrClose(connection *conn) {
    tunnelSession *session = createTunnelSession(conn, server.primary_host, server.primary_port);
    if (connConnect(session->up_pipe.write_conn, session->host, session->port,
                    server.bind_source_addr, 0, connectHandler) == C_ERR) {
        freeTunnelSession(session);
    }
}

void tunnelsCron(void) {
    listIter li;
    listNode *ln;
    mstime_t now = mstime();
    mstime_t timeout_ms = server.tunnel_timeout * 1000;

    listRewind(server.active_tunnel_sessions, &li);
    while ((ln = listNext(&li)) != NULL) {
        tunnelSession *session = listNodeValue(ln);
        if (session->state == TUNNEL_STATE_CLOSING) continue;

        mstime_t elapsed = now - session->creation_time;

        if (elapsed <= timeout_ms) break;

        serverLog(LL_VERBOSE,
            "Tunneling session to %s:%d has timed out after %lld seconds. Aborting.",
            session->host, session->port, elapsed / 1000);

        abortTunnelSession(session);
    }
}