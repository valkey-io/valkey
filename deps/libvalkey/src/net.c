/* Extracted from anet.c to work properly with Hiredis error reporting.
 *
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2014, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2015, Matt Stancliff <matt at genges dot com>,
 *                     Jan-Erik Rediger <janerik at fnordig dot com>
 *
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

#include "fmacros.h"
#include "win32.h"

#include "net.h"

#include "async.h"
#include "sockcompat.h"
#include "valkey_private.h"

#include <sds.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

void valkeyNetClose(valkeyContext *c) {
    if (c && c->fd != VALKEY_INVALID_FD) {
        close(c->fd);
        c->fd = VALKEY_INVALID_FD;
    }
}

static ssize_t valkeyNetRead(valkeyContext *c, char *buf, size_t bufcap) {
    ssize_t nread = recv(c->fd, buf, bufcap, 0);
    if (nread == -1) {
        if ((errno == EWOULDBLOCK && !(c->flags & VALKEY_BLOCK)) || (errno == EINTR)) {
            /* Try again later */
            return 0;
        } else if (errno == ETIMEDOUT && (c->flags & VALKEY_BLOCK)) {
            /* especially in windows */
            valkeySetError(c, VALKEY_ERR_TIMEOUT, "recv timeout");
            return -1;
        } else {
            valkeySetError(c, VALKEY_ERR_IO, strerror(errno));
            return -1;
        }
    } else if (nread == 0) {
        valkeySetError(c, VALKEY_ERR_EOF, "Server closed the connection");
        return -1;
    } else {
        return nread;
    }
}

static ssize_t valkeyNetWrite(valkeyContext *c) {
    ssize_t nwritten;

    nwritten = send(c->fd, c->obuf, sdslen(c->obuf), 0);
    if (nwritten < 0) {
        if ((errno == EWOULDBLOCK && !(c->flags & VALKEY_BLOCK)) || (errno == EINTR)) {
            /* Try again */
            return 0;
        } else {
            valkeySetError(c, VALKEY_ERR_IO, strerror(errno));
            return -1;
        }
    }

    return nwritten;
}

static void valkeySetErrorFromErrno(valkeyContext *c, int type, const char *prefix) {
    int errorno = errno; /* snprintf() may change errno */
    char buf[128] = {0};
    size_t len = 0;

    if (prefix != NULL)
        len = snprintf(buf, sizeof(buf), "%s: ", prefix);
    strerror_r(errorno, (char *)(buf + len), sizeof(buf) - len);
    valkeySetError(c, type, buf);
}

static int valkeySetReuseAddr(valkeyContext *c) {
    int on = 1;
    if (setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, NULL);
        valkeyNetClose(c);
        return VALKEY_ERR;
    }
    return VALKEY_OK;
}

static int valkeyCreateSocket(valkeyContext *c, int type) {
    valkeyFD s;
    if ((s = socket(type, SOCK_STREAM, 0)) == VALKEY_INVALID_FD) {
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, NULL);
        return VALKEY_ERR;
    }
    c->fd = s;
    if (type == AF_INET) {
        if (valkeySetReuseAddr(c) == VALKEY_ERR) {
            return VALKEY_ERR;
        }
    }
    return VALKEY_OK;
}

static int valkeySetBlocking(valkeyContext *c, int blocking) {
#ifndef _WIN32
    int flags;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(c->fd, F_GETFL)) == -1) {
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, "fcntl(F_GETFL)");
        valkeyNetClose(c);
        return VALKEY_ERR;
    }

    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if (fcntl(c->fd, F_SETFL, flags) == -1) {
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, "fcntl(F_SETFL)");
        valkeyNetClose(c);
        return VALKEY_ERR;
    }
#else
    u_long mode = blocking ? 0 : 1;
    if (ioctl(c->fd, FIONBIO, &mode) == -1) {
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, "ioctl(FIONBIO)");
        valkeyNetClose(c);
        return VALKEY_ERR;
    }
#endif /* _WIN32 */
    return VALKEY_OK;
}

int valkeyKeepAlive(valkeyContext *c, int interval) {
    int val = 1;
    valkeyFD fd = c->fd;

    /* TCP_KEEPALIVE makes no sense with AF_UNIX connections */
    if (c->connection_type == VALKEY_CONN_UNIX)
        return VALKEY_ERR;

#ifndef _WIN32
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1) {
        valkeySetError(c, VALKEY_ERR_OTHER, strerror(errno));
        return VALKEY_ERR;
    }

    val = interval;

#if defined(__APPLE__) && defined(__MACH__)
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &val, sizeof(val)) < 0) {
        valkeySetError(c, VALKEY_ERR_OTHER, strerror(errno));
        return VALKEY_ERR;
    }
#else
#if defined(__GLIBC__) && !defined(__FreeBSD_kernel__)
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        valkeySetError(c, VALKEY_ERR_OTHER, strerror(errno));
        return VALKEY_ERR;
    }

    val = interval / 3;
    if (val == 0)
        val = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        valkeySetError(c, VALKEY_ERR_OTHER, strerror(errno));
        return VALKEY_ERR;
    }

    val = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        valkeySetError(c, VALKEY_ERR_OTHER, strerror(errno));
        return VALKEY_ERR;
    }
#endif
#endif
#else
    int res;

    res = win32_valkeyKeepAlive(fd, interval * 1000);
    if (res != 0) {
        valkeySetError(c, VALKEY_ERR_OTHER, strerror(res));
        return VALKEY_ERR;
    }
#endif
    return VALKEY_OK;
}

int valkeySetTcpNoDelay(valkeyContext *c) {
    int yes = 1;
    if (setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, "setsockopt(TCP_NODELAY)");
        valkeyNetClose(c);
        return VALKEY_ERR;
    }
    return VALKEY_OK;
}

int valkeyContextSetTcpUserTimeout(valkeyContext *c, unsigned int timeout) {
    int res;
#ifdef TCP_USER_TIMEOUT
    res = setsockopt(c->fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout, sizeof(timeout));
#else
    res = -1;
    errno = ENOTSUP;
    (void)timeout;
#endif
    if (res == -1) {
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, "setsockopt(TCP_USER_TIMEOUT)");
        valkeyNetClose(c);
        return VALKEY_ERR;
    }
    return VALKEY_OK;
}

static long valkeyPollMillis(void) {
#ifndef _MSC_VER
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec * 1000) + now.tv_nsec / 1000000;
#else
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10;
#endif
}

static int valkeyContextWaitReady(valkeyContext *c, long msec) {
    struct pollfd wfd;
    long end;
    int res;

    if (errno != EINPROGRESS) {
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, NULL);
        valkeyNetClose(c);
        return VALKEY_ERR;
    }

    wfd.fd = c->fd;
    wfd.events = POLLOUT;
    end = msec >= 0 ? valkeyPollMillis() + msec : 0;

    while ((res = poll(&wfd, 1, msec)) <= 0) {
        if (res < 0 && errno != EINTR) {
            valkeySetErrorFromErrno(c, VALKEY_ERR_IO, "poll(2)");
            valkeyNetClose(c);
            return VALKEY_ERR;
        } else if (res == 0 || (msec >= 0 && valkeyPollMillis() >= end)) {
            errno = ETIMEDOUT;
            valkeySetErrorFromErrno(c, VALKEY_ERR_IO, NULL);
            valkeyNetClose(c);
            return VALKEY_ERR;
        } else {
            /* res < 0 && errno == EINTR, try again */
        }
    }

    if (valkeyCheckConnectDone(c, &res) != VALKEY_OK || res == 0) {
        valkeyCheckSocketError(c);
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

int valkeyCheckConnectDone(valkeyContext *c, int *completed) {
    int rc = connect(c->fd, (const struct sockaddr *)c->saddr, c->addrlen);
    if (rc == 0) {
        *completed = 1;
        return VALKEY_OK;
    }
    int error = errno;
    if (error == EINPROGRESS) {
        /* must check error to see if connect failed.  Get the socket error */
        int fail, so_error;
        socklen_t optlen = sizeof(so_error);
        fail = getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen);
        if (fail == 0) {
            if (so_error == 0) {
                /* Socket is connected! */
                *completed = 1;
                return VALKEY_OK;
            }
            /* connection error; */
            errno = so_error;
            error = so_error;
        }
    }
    switch (error) {
    case EISCONN:
        *completed = 1;
        return VALKEY_OK;
    case EALREADY:
    case EWOULDBLOCK:
        *completed = 0;
        return VALKEY_OK;
    default:
        return VALKEY_ERR;
    }
}

int valkeyCheckSocketError(valkeyContext *c) {
    int err = 0, errno_saved = errno;
    socklen_t errlen = sizeof(err);

    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) {
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, "getsockopt(SO_ERROR)");
        return VALKEY_ERR;
    }

    if (err == 0) {
        err = errno_saved;
    }

    if (err) {
        errno = err;
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, NULL);
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

int valkeyTcpSetTimeout(valkeyContext *c, const struct timeval tv) {
    const void *to_ptr = &tv;
    size_t to_sz = sizeof(tv);

    if (setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, to_ptr, to_sz) == -1) {
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, "setsockopt(SO_RCVTIMEO)");
        return VALKEY_ERR;
    }
    if (setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, to_ptr, to_sz) == -1) {
        valkeySetErrorFromErrno(c, VALKEY_ERR_IO, "setsockopt(SO_SNDTIMEO)");
        return VALKEY_ERR;
    }
    return VALKEY_OK;
}

#ifdef IPPROTO_MPTCP
int valkeyHasMptcp(void) {
    return 1;
}

/* XXX: Until glibc 2.41, getaddrinfo with hints.ai_protocol of IPPROTO_MPTCP leads error.
 * Use hints.ai_protocol IPPROTO_IP (0) or IPPROTO_TCP (6) to resolve address and overwrite
 * it when MPTCP is enabled.
 * Ref: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/tools/testing/selftests/net/mptcp/mptcp_connect.c
 *      https://sourceware.org/git/?p=glibc.git;a=commit;h=a8e9022e0f829d44a818c642fc85b3bfbd26a514
 */
static int valkeyTcpGetProtocol(int is_mptcp_enabled) {
    return is_mptcp_enabled ? IPPROTO_MPTCP : IPPROTO_TCP;
}

#else
int valkeyHasMptcp(void) {
    return 0;
}

static int valkeyTcpGetProtocol(int is_mptcp_enabled) {
    assert(!is_mptcp_enabled);
    (void)is_mptcp_enabled; /* Suppress unused warning when NDEBUG is defined. */
    return IPPROTO_TCP;
}
#endif /* IPPROTO_MPTCP */

int valkeyContextConnectTcp(valkeyContext *c, const valkeyOptions *options) {
    const struct timeval *timeout = options->connect_timeout;
    const char *addr = options->endpoint.tcp.ip;
    const char *source_addr = options->endpoint.tcp.source_addr;
    int port = options->endpoint.tcp.port;
    valkeyFD s;
    int rv, n;
    char _port[6]; /* strlen("65535"); */
    struct addrinfo hints, *servinfo, *bservinfo, *p, *b;
    int blocking = (c->flags & VALKEY_BLOCK);
    int reuseaddr = (c->flags & VALKEY_REUSEADDR);
    int reuses = 0;
    long timeout_msec = -1;

    servinfo = NULL;
    c->connection_type = VALKEY_CONN_TCP;
    c->tcp.port = port;

    /* We need to take possession of the passed parameters
     * to make them reusable for a reconnect.
     * We also carefully check we don't free data we already own,
     * as in the case of the reconnect method.
     *
     * This is a bit ugly, but atleast it works and doesn't leak memory.
     **/
    if (c->tcp.host != addr) {
        vk_free(c->tcp.host);

        c->tcp.host = vk_strdup(addr);
        if (c->tcp.host == NULL)
            goto oom;
    }

    if (timeout) {
        if (valkeyContextUpdateConnectTimeout(c, timeout) == VALKEY_ERR)
            goto oom;
    } else {
        vk_free(c->connect_timeout);
        c->connect_timeout = NULL;
    }

    if (valkeyConnectTimeoutMsec(c, &timeout_msec) != VALKEY_OK) {
        goto error;
    }

    if (source_addr == NULL) {
        vk_free(c->tcp.source_addr);
        c->tcp.source_addr = NULL;
    } else if (c->tcp.source_addr != source_addr) {
        vk_free(c->tcp.source_addr);
        c->tcp.source_addr = vk_strdup(source_addr);
    }

    snprintf(_port, 6, "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* DNS lookup. To use dual stack, set both flags to prefer both IPv4 and
     * IPv6. By default, for historical reasons, we try IPv4 first and then we
     * try IPv6 only if no IPv4 address was found. */
    if (c->flags & VALKEY_PREFER_IPV6 && c->flags & VALKEY_PREFER_IPV4)
        hints.ai_family = AF_UNSPEC;
    else if (c->flags & VALKEY_PREFER_IPV6)
        hints.ai_family = AF_INET6;
    else
        hints.ai_family = AF_INET;

    rv = getaddrinfo(c->tcp.host, _port, &hints, &servinfo);
    if (rv != 0 && hints.ai_family != AF_UNSPEC) {
        /* Try again with the other IP version. */
        hints.ai_family = (hints.ai_family == AF_INET) ? AF_INET6 : AF_INET;
        rv = getaddrinfo(c->tcp.host, _port, &hints, &servinfo);
    }
    if (rv != 0) {
        valkeySetError(c, VALKEY_ERR_OTHER, gai_strerror(rv));
        return VALKEY_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
    addrretry:
        if ((s = socket(p->ai_family, p->ai_socktype, valkeyTcpGetProtocol(c->flags & VALKEY_MPTCP))) == VALKEY_INVALID_FD)
            continue;

        c->fd = s;
        if (valkeySetBlocking(c, 0) != VALKEY_OK)
            goto error;
        if (c->tcp.source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
            if ((rv = getaddrinfo(c->tcp.source_addr, NULL, &hints, &bservinfo)) != 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Can't get addr: %s", gai_strerror(rv));
                valkeySetError(c, VALKEY_ERR_OTHER, buf);
                goto error;
            }

            if (reuseaddr) {
                n = 1;
                if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&n,
                               sizeof(n)) < 0) {
                    freeaddrinfo(bservinfo);
                    goto error;
                }
            }

            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s, b->ai_addr, b->ai_addrlen) != -1) {
                    bound = 1;
                    break;
                }
            }
            freeaddrinfo(bservinfo);
            if (!bound) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Can't bind socket: %s", strerror(errno));
                valkeySetError(c, VALKEY_ERR_OTHER, buf);
                goto error;
            }
        }

        /* For repeat connection */
        vk_free(c->saddr);
        c->saddr = vk_malloc(p->ai_addrlen);
        if (c->saddr == NULL)
            goto oom;

        memcpy(c->saddr, p->ai_addr, p->ai_addrlen);
        c->addrlen = p->ai_addrlen;

        if (connect(s, p->ai_addr, p->ai_addrlen) == -1) {
            if (errno == EHOSTUNREACH) {
                valkeyNetClose(c);
                continue;
            } else if (errno == EINPROGRESS) {
                if (blocking) {
                    goto wait_for_ready;
                }
                /* This is ok.
                 * Note that even when it's in blocking mode, we unset blocking
                 * for `connect()`
                 */
            } else if (errno == EADDRNOTAVAIL && reuseaddr) {
                if (++reuses >= VALKEY_CONNECT_RETRIES) {
                    goto error;
                } else {
                    valkeyNetClose(c);
                    goto addrretry;
                }
            } else {
            wait_for_ready:
                if (valkeyContextWaitReady(c, timeout_msec) != VALKEY_OK)
                    goto error;
                if (valkeySetTcpNoDelay(c) != VALKEY_OK)
                    goto error;
            }
        }
        if (blocking && valkeySetBlocking(c, 1) != VALKEY_OK)
            goto error;

        c->flags |= VALKEY_CONNECTED;
        rv = VALKEY_OK;
        goto end;
    }
    if (p == NULL) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Can't create socket: %s", strerror(errno));
        valkeySetError(c, VALKEY_ERR_OTHER, buf);
        goto error;
    }

oom:
    valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
error:
    rv = VALKEY_ERR;
end:
    if (servinfo) {
        freeaddrinfo(servinfo);
    }

    return rv; // Need to return VALKEY_OK if alright
}

static int valkeyContextConnectUnix(valkeyContext *c, const valkeyOptions *options) {
#ifndef _WIN32
    const struct timeval *timeout = options->connect_timeout;
    const char *path = options->endpoint.unix_socket;
    int blocking = (c->flags & VALKEY_BLOCK);
    struct sockaddr_un *sa;
    long timeout_msec = -1;

    if (valkeyCreateSocket(c, AF_UNIX) < 0)
        return VALKEY_ERR;
    if (valkeySetBlocking(c, 0) != VALKEY_OK)
        return VALKEY_ERR;

    c->connection_type = VALKEY_CONN_UNIX;
    if (c->unix_sock.path != path) {
        vk_free(c->unix_sock.path);

        c->unix_sock.path = vk_strdup(path);
        if (c->unix_sock.path == NULL)
            goto oom;
    }

    if (timeout) {
        if (valkeyContextUpdateConnectTimeout(c, timeout) == VALKEY_ERR)
            goto oom;
    } else {
        vk_free(c->connect_timeout);
        c->connect_timeout = NULL;
    }

    if (valkeyConnectTimeoutMsec(c, &timeout_msec) != VALKEY_OK)
        return VALKEY_ERR;

    /* Don't leak sockaddr if we're reconnecting */
    if (c->saddr)
        vk_free(c->saddr);

    sa = (struct sockaddr_un *)(c->saddr = vk_malloc(sizeof(struct sockaddr_un)));
    if (sa == NULL)
        goto oom;

    c->addrlen = sizeof(struct sockaddr_un);
    sa->sun_family = AF_UNIX;
    strncpy(sa->sun_path, path, sizeof(sa->sun_path) - 1);
    if (connect(c->fd, (struct sockaddr *)sa, sizeof(*sa)) == -1) {
        if (errno == EINPROGRESS && !blocking) {
            /* This is ok. */
        } else {
            if (valkeyContextWaitReady(c, timeout_msec) != VALKEY_OK)
                return VALKEY_ERR;
        }
    }

    /* Reset socket to be blocking after connect(2). */
    if (blocking && valkeySetBlocking(c, 1) != VALKEY_OK)
        return VALKEY_ERR;

    c->flags |= VALKEY_CONNECTED;
    return VALKEY_OK;
#else
    /* We currently do not support Unix sockets for Windows. */
    /* TODO(m): https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/ */
    errno = EPROTONOSUPPORT;
    return VALKEY_ERR;
#endif /* _WIN32 */
oom:
    valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
    return VALKEY_ERR;
}

static valkeyContextFuncs valkeyContextTcpFuncs = {
    .connect = valkeyContextConnectTcp,
    .close = valkeyNetClose,
    .free_privctx = NULL,
    .async_read = valkeyAsyncRead,
    .async_write = valkeyAsyncWrite,
    .read = valkeyNetRead,
    .write = valkeyNetWrite,
    .set_timeout = valkeyTcpSetTimeout,
};

void valkeyContextRegisterTcpFuncs(void) {
    valkeyContextRegisterFuncs(&valkeyContextTcpFuncs, VALKEY_CONN_TCP);
}

static valkeyContextFuncs valkeyContextUnixFuncs = {
    .connect = valkeyContextConnectUnix,
    .close = valkeyNetClose,
    .free_privctx = NULL,
    .async_read = valkeyAsyncRead,
    .async_write = valkeyAsyncWrite,
    .read = valkeyNetRead,
    .write = valkeyNetWrite,
    .set_timeout = valkeyTcpSetTimeout,
};

void valkeyContextRegisterUnixFuncs(void) {
    valkeyContextRegisterFuncs(&valkeyContextUnixFuncs, VALKEY_CONN_UNIX);
}

static int valkeyContextConnectUserfd(valkeyContext *c, const valkeyOptions *options) {
    c->fd = options->endpoint.fd;
    c->flags |= VALKEY_CONNECTED;

    return VALKEY_OK;
}

static valkeyContextFuncs valkeyContextUserfdFuncs = {
    .connect = valkeyContextConnectUserfd,
    .close = valkeyNetClose,
    .free_privctx = NULL,
    .async_read = valkeyAsyncRead,
    .async_write = valkeyAsyncWrite,
    .read = valkeyNetRead,
    .write = valkeyNetWrite,
    .set_timeout = valkeyTcpSetTimeout,
};

void valkeyContextRegisterUserfdFuncs(void) {
    valkeyContextRegisterFuncs(&valkeyContextUserfdFuncs, VALKEY_CONN_USERFD);
}
