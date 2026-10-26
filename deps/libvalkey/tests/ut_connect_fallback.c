/*
 * Unit test for TCP connect fallback behavior.
 *
 * Verifies that valkeyContextConnectTcp iterates through all addresses in the
 * DNS response when earlier ones fail. We override getaddrinfo() to return three
 * entries for 127.0.0.1 and override connect() to fail with ECONNREFUSED on the
 * first two. The third entry points to a real listening socket and should
 * succeed.
 */

#define _GNU_SOURCE
#include "fmacros.h"

#include "sockcompat.h"
#include "valkey.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int port_listen;

#define PORT_ECONNREFUSED 10000 /* Simulates failed non-blocking connect */
#define PORT_EHOSTUNREACH 10001 /* Simulates immediate routing failure */
static struct addrinfo *make_entry(int port, struct addrinfo *next);

/* --- Overridden system calls --- */

/*
 * Override getaddrinfo(3) to return a crafted address list instead of doing
 * real DNS resolution. Linked before libc due to static library ordering.
 */
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    (void)node;
    (void)service;
    (void)hints;
    struct addrinfo *third = make_entry(port_listen, NULL);
    struct addrinfo *second = make_entry(PORT_EHOSTUNREACH, third);
    struct addrinfo *first = make_entry(PORT_ECONNREFUSED, second);
    *res = first;
    return 0;
}

/* Override freeaddrinfo(3) to match the crafted getaddrinfo above. */
void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res);
        res = next;
    }
}

/*
 * Override connect(2) to simulate connect failures for the fake addresses.
 * Port 10000 returns EINPROGRESS then ECONNREFUSED (failed non-blocking connect).
 * Port 10001 returns EHOSTUNREACH (immediate routing failure).
 * Falls through to the real connect via dlsym(RTLD_NEXT) for other addresses.
 */
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    static int refused_attempts = 0;
    static int (*real_connect)(int, const struct sockaddr *, socklen_t) = NULL;
    if (!real_connect)
        real_connect = dlsym(RTLD_NEXT, "connect");

    assert(addr->sa_family == AF_INET);
    const struct sockaddr_in *sa = (const struct sockaddr_in *)addr;
    int port = ntohs(sa->sin_port);
    if (port == PORT_ECONNREFUSED) {
        errno = (refused_attempts++ == 0) ? EINPROGRESS : ECONNREFUSED;
        return -1;
    }
    if (port == PORT_EHOSTUNREACH) {
        errno = EHOSTUNREACH;
        return -1;
    }
    return real_connect(sockfd, addr, addrlen);
}

/* --- Helpers --- */

/* Build a single addrinfo entry for 127.0.0.1 on the given port. */
static struct addrinfo *make_entry(int port, struct addrinfo *next) {
    struct addrinfo *ai = calloc(1, sizeof(*ai));
    struct sockaddr_in *sa = calloc(1, sizeof(*sa));
    assert(ai && sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    ai->ai_addrlen = sizeof(*sa);
    ai->ai_addr = (struct sockaddr *)sa;
    ai->ai_next = next;
    return ai;
}

/* Create a TCP listener on 127.0.0.1 with an OS-assigned port. */
static int create_listener(int *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 1) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr *)&addr, &len) == 0);
    *out_port = ntohs(addr.sin_port);
    return fd;
}

int main(void) {
    int listen_fd = create_listener(&port_listen);

    printf("Test: connect falls back to next address after ECONNREFUSED\n");

    /* Arguments are ignored by our getaddrinfo override. */
    valkeyContext *c = valkeyConnect("localhost", 0);
    assert(c != NULL);
    if (c->err) {
        fprintf(stderr, "FAIL: %s\n", c->errstr);
        valkeyFree(c);
        close(listen_fd);
        return 1;
    }
    assert(c->err == 0);
    assert(c->errstr[0] == '\0');

    printf("PASS\n");
    valkeyFree(c);
    close(listen_fd);
    return 0;
}
