#ifndef TUNNEL_H
#define TUNNEL_H

#include "sds.h"

typedef struct connection connection;

/* State machine for the tunnel session lifecycle. */
typedef enum {
    TUNNEL_STATE_CONNECTING,  /* Initial state, upstream connection in progress. */
    TUNNEL_STATE_ESTABLISHED, /* Both connections are up, data transfer is active. */
    TUNNEL_STATE_CLOSING      /* Marked for asynchronous cleanup. */
} tunnelState;

typedef struct tunnelPipe {
    connection *read_conn;
    connection *write_conn;
    sds buffer; /* Use a dynamic sds buffer instead of a fixed-size array. */
} tunnelPipe;

typedef struct tunnelSession {
    tunnelPipe up_pipe;   /* 'up' reads from downstream and writes to upstream. */
    tunnelPipe down_pipe; /* 'down' reads from upstream and writes to downstream. */
    sds host;
    int port;
    tunnelState state;    /* The current state of the session. */
    mstime_t creation_time;
} tunnelSession;

/* Public API */
void establishTunnelOrClose(connection *conn);
void freeTunnelsInAsyncFreeQueue(void);
void abortAllTunnelSessions(void);
void tunnelsCron(void);

#endif /* TUNNEL_H */