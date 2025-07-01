#ifndef TUNNEL_H
#define TUNNEL_H

#include "sds.h"

typedef struct connection connection;

#define TUNNEL_BUFSIZE 4096

typedef struct tunnelPipe {
    connection *read_conn;
    connection *write_conn;
    char buffer[TUNNEL_BUFSIZE];
    size_t buffer_pos;
    size_t buffer_len;
} tunnelPipe;

typedef struct TunnelFlags {
    uint64_t closed : 1; /* Tunnel session is marked for close */
} TunnelFlags;

typedef struct tunnelSession {
    tunnelPipe up_pipe;   /* 'up' reads from downstream and writes to upstream */
    tunnelPipe down_pipe; /* 'down' reads from upstream and writes to downstream */
    sds host;
    int port;
    union {
        uint64_t raw_flag;
        struct TunnelFlags flag;
    };
} tunnelSession;

void establishTunnelOrClose(connection *conn);
void freeTunnelsInAsyncFreeQueue(void);

#endif /* TUNNEL_H */
