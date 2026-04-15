/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"
#include <cstdint>
#include <cstdlib>
#include <unistd.h>

extern "C" {
#include "ae.h"
#include "config.h"
#include "connection.h"
#include "fmacros.h"
#include "server.h"

/* Locally duplicate the opaque tls_connection to manipulate its flags */
#define TLS_CONN_FLAG_WRITE_WANT_READ (1 << 1)

typedef struct fake_tls_connection {
    connection c;
    int flags;
} fake_tls_connection;
}

class TlsEventTest : public ::testing::Test {
  protected:
    void SetUp() override {
        server.logfile = (char *)"";
        server.el = aeCreateEventLoop(1024);
        ASSERT_NE(server.el, nullptr);
        connTypeInitialize();
    }

    void TearDown() override {
        if (server.el) {
            aeDeleteEventLoop(server.el);
            server.el = nullptr;
        }
        connTypeCleanupAll();
    }
};

TEST_F(TlsEventTest, BusyLoopClearance) {
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    ConnectionType *ct = connectionTypeTls();
    if (!ct) {
        close(fds[0]);
        close(fds[1]);
        GTEST_SKIP() << "TLS not supported in this build";
        return;
    }

    fake_tls_connection *conn = (fake_tls_connection *)zcalloc(sizeof(fake_tls_connection));
    conn->c.type = ct;
    conn->c.fd = fds[0];
    conn->c.state = CONN_STATE_CONNECTED;

    /* 1. Set Want Read */
    conn->flags |= TLS_CONN_FLAG_WRITE_WANT_READ;
    conn->c.type->set_write_handler(&conn->c, (ConnectionCallbackFunc)0xdeadbeef, 0);

    int mask = aeGetFileEvents(server.el, conn->c.fd);
    ASSERT_NE(mask & AE_READABLE, 0);

    /* 2. High level clears */
    conn->c.type->set_write_handler(&conn->c, nullptr, 0);

    mask = aeGetFileEvents(server.el, conn->c.fd);
    ASSERT_EQ(mask & AE_READABLE, 0);

    aeDeleteFileEvent(server.el, conn->c.fd, AE_READABLE);
    aeDeleteFileEvent(server.el, conn->c.fd, AE_WRITABLE);
    close(fds[0]);
    close(fds[1]);
    zfree(conn);
}
