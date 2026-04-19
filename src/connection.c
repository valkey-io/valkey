/* ==========================================================================
 * connection.c - connection layer framework
 * --------------------------------------------------------------------------
 * Copyright (C) 2022  zhenwei pi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ==========================================================================
 */

#include "server.h"
#include "connection.h"

#include <arpa/inet.h>
#include <ifaddrs.h>

static ConnectionType *connTypes[CONN_TYPE_MAX];

int connTypeRegister(ConnectionType *ct) {
    int type = ct->get_type();
    serverAssert(type >= 0 && type < CONN_TYPE_MAX && !connTypes[type]);

    serverLog(LL_VERBOSE, "Connection type %s registering", getConnectionTypeName(type));
    connTypes[type] = ct;

    if (ct->init) {
        ct->init();
    }

    return C_OK;
}

int connTypeInitialize(void) {
    /* currently socket connection type is necessary  */
    serverAssert(RedisRegisterConnectionTypeSocket() == C_OK);

    /* currently unix socket connection type is necessary  */
    serverAssert(RedisRegisterConnectionTypeUnix() == C_OK);

    /* may fail if without BUILD_TLS=yes */
    RedisRegisterConnectionTypeTLS();

    /* may fail if without BUILD_RDMA=yes */
    RegisterConnectionTypeRdma();

    return C_OK;
}

ConnectionType *connectionByType(int type) {
    serverAssert(type >= 0 && type < CONN_TYPE_MAX);

    ConnectionType *ct = connTypes[type];

    if (!ct) {
        serverLog(LL_WARNING, "Missing implement of connection type %s", getConnectionTypeName(type));
    }
    return ct;
}

/* Cache TCP connection type, query it by string once */
ConnectionType *connectionTypeTcp(void) {
    static ConnectionType *ct_tcp = NULL;

    if (ct_tcp != NULL) return ct_tcp;

    ct_tcp = connectionByType(CONN_TYPE_SOCKET);
    serverAssert(ct_tcp != NULL);

    return ct_tcp;
}

/* Cache TLS connection type, query it by string once */
ConnectionType *connectionTypeTls(void) {
    static ConnectionType *ct_tls = NULL;
    static int cached = 0;

    /* Unlike the TCP and Unix connections, the TLS one can be missing
     * So we need the cached pointer to handle NULL correctly too. */
    if (!cached) {
        cached = 1;
        ct_tls = connectionByType(CONN_TYPE_TLS);
    }

    return ct_tls;
}

/* Cache Unix connection type, query it by string once */
ConnectionType *connectionTypeUnix(void) {
    static ConnectionType *ct_unix = NULL;

    if (ct_unix != NULL) return ct_unix;

    ct_unix = connectionByType(CONN_TYPE_UNIX);
    return ct_unix;
}

void connTypeCleanupAll(void) {
    ConnectionType *ct;
    int type;

    for (type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (!ct) continue;

        if (ct->cleanup) ct->cleanup();
    }
}

/* walk all the connection types until has pending data */
int connTypeHasPendingData(void) {
    ConnectionType *ct;
    int type;
    int ret = 0;

    for (type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (ct && ct->has_pending_data && (ret = ct->has_pending_data())) {
            return ret;
        }
    }

    return ret;
}

/* walk all the connection types and process pending data for each connection type */
int connTypeProcessPendingData(void) {
    ConnectionType *ct;
    int type;
    int ret = 0;

    for (type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (ct && ct->process_pending_data) {
            ret += ct->process_pending_data();
        }
    }

    return ret;
}

sds getListensInfoString(sds info) {
    for (int j = 0; j < CONN_TYPE_MAX; j++) {
        connListener *listener = &server.listeners[j];
        if (listener->ct == NULL) continue;

        info = sdscatfmt(info, "listener%i:name=%s", j, getConnectionTypeName(listener->ct->get_type()));
        for (int i = 0; i < listener->count; i++) {
            info = sdscatfmt(info, ",bind=%s", listener->bindaddr[i]);
        }

        if (listener->port) info = sdscatfmt(info, ",port=%i", listener->port);

        info = sdscatfmt(info, "\r\n");
    }

    return info;
}

int stringArrayContains(char **arr, int count, const char *s) {
    if (!arr || !s) return 0;

    for (int i = 0; i < count; i++) {
        if (arr[i] && strcmp(arr[i], s) == 0) {
            return 1;
        }
    }

    return 0;
}

int matchListeningIP(char **bindaddr, int bind_count, const char *ip, sds *local_ips, int local_ip_count) {
    if (!ip || !local_ips) return 0;

    if (stringArrayContains(bindaddr, bind_count, ip)) {
        return 1;
    }

    if (!stringArrayContains(bindaddr, bind_count, "0.0.0.0") &&
        !stringArrayContains(bindaddr, bind_count, "::")) {
        return 0;
    }

    for (int i = 0; i < local_ip_count; i++) {
        if (strcmp(local_ips[i], ip) == 0) {
            return 1;
        }
    }

    return 0;
}

sds *getLocalIPAddresses(int *count_out) {
    sds *ip_array = NULL;
    struct ifaddrs *if_addr_struct;
    int ip_count = 0;

    if (count_out) *count_out = 0;

    if (getifaddrs(&if_addr_struct) == -1) {
        return NULL;
    }

    for (struct ifaddrs *ifa = if_addr_struct; ifa; ifa = ifa->ifa_next) {
        char buf[INET6_ADDRSTRLEN];

        if (!ifa->ifa_addr) continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            if (!inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf)))
                continue;
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)ifa->ifa_addr;
            if (IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr)) continue;
            if (!inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf)))
                continue;
        } else {
            continue;
        }

        ip_array = zrealloc(ip_array, (ip_count + 1) * sizeof(sds));
        ip_array[ip_count++] = sdsnew(buf);
    }

    freeifaddrs(if_addr_struct);

    if (count_out) *count_out = ip_count;

    return ip_array;
}
