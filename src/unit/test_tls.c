#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ae.h"
#include "server.h"
#include "connection.h"
#include "testhelp.h"

/* We need internal TLS flags and structures. 
 * Since they are private to tls.c, we re-declare what we need here.
 */
#define TLS_CONN_FLAG_WRITE_WANT_READ (1<<2)
#define TLS_CONN_FLAG_READ_WANT_WRITE (1<<3)

typedef struct tls_connection {
    connection c;
    int flags;
    void *ssl;
    char *ssl_error;
    void *pending_list_node;
    size_t last_failed_write_data_len;
} tls_connection;

extern ConnectionType CT_TLS;
void updateSSLEvent(tls_connection *conn);

#define TEST_ASSERT(cond) test_cond(#cond, cond)

int test_tlsBusyLoop(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

#if defined(USE_OPENSSL)
    server.el = aeCreateEventLoop(1024);
    TEST_ASSERT(server.el != NULL);

    int fds[2];
    TEST_ASSERT(pipe(fds) == 0);
    
    /* Create fake TLS connection */
    tls_connection *conn = zcalloc(sizeof(tls_connection));
    conn->c.type = &CT_TLS;
    conn->c.fd = fds[0];
    conn->c.state = CONN_STATE_CONNECTED;

    /* Step 1: Simulate write handler facing WANT_READ.
     * WANT_READ flag is set, registering AE_READABLE on the event loop. */
    connSetWriteHandler(&conn->c, (ConnectionCallbackFunc) 0xdeadbeef);
    conn->flags |= TLS_CONN_FLAG_WRITE_WANT_READ;
    updateSSLEvent(conn);

    int mask = aeGetFileEvents(server.el, conn->c.fd);
    TEST_ASSERT((mask & AE_READABLE) != 0);

    /* Step 2: High level app removes the write handler */
    connSetWriteHandler(&conn->c, NULL);

    /* BUG VERIFICATION:
     * Removing the write handler should end up unregistering AE_READABLE 
     * since there is no longer interest in the write wait. */
    mask = aeGetFileEvents(server.el, conn->c.fd);
    TEST_ASSERT((mask & AE_READABLE) == 0);

    /* Step 3: Same for read handler facing WANT_WRITE */
    connSetReadHandler(&conn->c, (ConnectionCallbackFunc) 0xdeadbeef);
    conn->flags |= TLS_CONN_FLAG_READ_WANT_WRITE;
    updateSSLEvent(conn);

    mask = aeGetFileEvents(server.el, conn->c.fd);
    TEST_ASSERT((mask & AE_WRITABLE) != 0);

    connSetReadHandler(&conn->c, NULL);
    mask = aeGetFileEvents(server.el, conn->c.fd);
    TEST_ASSERT((mask & AE_WRITABLE) == 0);

    /* Cleanup */
    aeDeleteFileEvent(server.el, conn->c.fd, AE_READABLE);
    aeDeleteFileEvent(server.el, conn->c.fd, AE_WRITABLE);
    close(fds[0]);
    close(fds[1]);
    zfree(conn);
    aeDeleteEventLoop(server.el);
    server.el = NULL;
#else
    /* Test skipped when OpenSSL is not enabled */
    TEST_ASSERT(1);
#endif

    return 0;
}

int __failed_tests = 0;
int __test_num = 0;

int main(int argc, char **argv) {
    int result = test_tlsBusyLoop(argc, argv, 0);
    test_report();
    return result;
}
