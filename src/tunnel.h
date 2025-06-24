#ifndef TUNNEL_H
#define TUNNEL_H

#include "sds.h"

typedef struct connection connection;
typedef struct client client;

#define TUNNEL_BUFSIZE 4096

typedef struct tunnelPipe {
    connection *read_conn;
    connection *write_conn;
    char buffer[TUNNEL_BUFSIZE];
    size_t buffer_pos;
    size_t buffer_len;
} tunnelPipe;

typedef struct tunnelSession {
    client *downstream_client;
    client *upstream_client;
    tunnelPipe up_pipe;   /* 'up' reads from downstream and writes to upstream */
    tunnelPipe down_pipe; /* 'down' reads from upstream and writes to downstream */
    sds host;
    int port;
    sds cmd;
    int multi_cnt;
} tunnelSession;

void establishTunnelOrClose(client *c);
void freeTunnelSession(tunnelSession *tunnel_session);

#endif /* TUNNEL_H */
