/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A fake connection for unit tests.
 *
 * Provides a `connection` that never touches a socket: writes land in an
 * in-memory buffer, reads are served from a caller-supplied byte array, and
 * handler/state calls are recorded so tests can assert on them.
 *
 * Everything here is `inline` rather than `static inline` on purpose: the unit
 * tests build with -Wall -Wextra -Werror, and a `static` helper that a given
 * translation unit happens not to use would trip -Wunused-function.
 */

#ifndef FAKE_CONNECTION_HPP
#define FAKE_CONNECTION_HPP

#include <cerrno>
#include <climits>
#include <cstring>

extern "C" {
#include "connection.h"
#include "zmalloc.h"
}

typedef struct fakeConnection {
    connection conn;

    /* Write sink. Writes are clamped to buf_size; set error to fail them. */
    int error;
    char *buffer;
    size_t buf_size;
    size_t written;

    /* Read source. Once read_data is drained, the next read reports EAGAIN,
     * or EOF / a hard error if the corresponding flag is set. A hard error
     * moves the connection out of CONN_STATE_CONNECTED, which is how callers
     * tell a real error from EAGAIN. */
    unsigned char *read_data;
    size_t read_len;
    size_t read_pos;
    int eof;
    int fail_read;

    /* Recorded calls, for tests that assert on connection lifecycle. */
    int close_calls;
    int postpone_state;
    int update_calls;
} fakeConnection;

inline int fakeConnGetType(void) {
    return CONN_TYPE_SOCKET;
}

inline int fakeConnWrite(connection *conn, const void *data, size_t size) {
    fakeConnection *fc = (fakeConnection *)conn;
    if (fc->error) return -1;

    size_t to_write = size;
    if (fc->written + to_write > fc->buf_size) {
        to_write = fc->buf_size - fc->written;
    }
    memcpy(fc->buffer + fc->written, data, to_write);
    fc->written += to_write;
    return (int)to_write;
}

inline int fakeConnWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    fakeConnection *fc = (fakeConnection *)conn;
    if (fc->error) return -1;

    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        size_t to_write = iov[i].iov_len;
        if (fc->written + to_write > fc->buf_size) {
            to_write = fc->buf_size - fc->written;
        }
        if (to_write == 0) break;

        memcpy(fc->buffer + fc->written, iov[i].iov_base, to_write);
        fc->written += to_write;
        total += to_write;
    }
    return (int)total;
}

inline int fakeConnRead(connection *conn, void *buf, size_t len) {
    fakeConnection *fc = (fakeConnection *)conn;
    if (fc->error) return -1;
    if (fc->read_pos >= fc->read_len) {
        if (fc->eof) return 0;
        if (fc->fail_read) {
            conn->state = CONN_STATE_ERROR;
            return -1;
        }
        errno = EAGAIN;
        return -1;
    }
    size_t avail = fc->read_len - fc->read_pos;
    size_t n = (len < avail) ? len : avail;
    memcpy(buf, fc->read_data + fc->read_pos, n);
    fc->read_pos += n;
    return (int)n;
}

inline int fakeConnSetWriteHandler(connection *conn, ConnectionCallbackFunc handler, int barrier) {
    UNUSED(barrier);
    conn->write_handler = handler;
    return C_OK;
}

inline int fakeConnSetReadHandler(connection *conn, ConnectionCallbackFunc handler) {
    conn->read_handler = handler;
    return C_OK;
}

inline void fakeConnPostponeUpdateState(connection *conn, int val) {
    ((fakeConnection *)conn)->postpone_state = val;
}

inline void fakeConnUpdateState(connection *conn) {
    ((fakeConnection *)conn)->update_calls++;
}

inline void fakeConnClose(connection *conn) {
    ((fakeConnection *)conn)->close_calls++;
    conn->state = CONN_STATE_CLOSED;
}

/* Complete the "handshake" immediately. Set error on the fake connection first
 * to simulate a failed accept instead. */
inline int fakeConnAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    if (((fakeConnection *)conn)->error) {
        conn->state = CONN_STATE_ERROR;
        return C_ERR;
    }
    conn->state = CONN_STATE_CONNECTED;
    if (accept_handler) accept_handler(conn);
    return C_OK;
}

/* The one ConnectionType shared by every fake connection. A function-local
 * static keeps a single instance without each fixture having to initialize it
 * from SetUpTestSuite(). Fields are assigned by name because designated
 * initializers need C++20. */
inline ConnectionType *fakeConnType(void) {
    static ConnectionType ct;
    static bool initialized = false;
    if (!initialized) {
        memset(&ct, 0, sizeof(ct));
        ct.get_type = fakeConnGetType;
        ct.close = fakeConnClose;
        ct.accept = fakeConnAccept;
        ct.write = fakeConnWrite;
        ct.writev = fakeConnWritev;
        ct.read = fakeConnRead;
        ct.set_write_handler = fakeConnSetWriteHandler;
        ct.set_read_handler = fakeConnSetReadHandler;
        ct.postpone_update_state = fakeConnPostponeUpdateState;
        ct.update_state = fakeConnUpdateState;
        initialized = true;
    }
    return &ct;
}

/* Create a fake connection. If write_cap is non-zero a write buffer of that
 * size is allocated, so tests that only read can pass 0. */
inline fakeConnection *connCreateFake(size_t write_cap = 0) {
    fakeConnection *fc = (fakeConnection *)zcalloc(sizeof(fakeConnection));
    fc->conn.type = fakeConnType();
    fc->conn.fd = -1;
    fc->conn.iovcnt = IOV_MAX;
    if (write_cap > 0) {
        fc->buffer = (char *)zmalloc(write_cap);
        fc->buf_size = write_cap;
    }
    return fc;
}

/* Point the connection's read side at a copy of the given bytes. */
inline void fakeConnSetReadData(fakeConnection *fc, const void *data, size_t len) {
    zfree(fc->read_data);
    fc->read_data = (unsigned char *)zmalloc(len);
    memcpy(fc->read_data, data, len);
    fc->read_len = len;
    fc->read_pos = 0;
}

inline void connFreeFake(fakeConnection *fc) {
    if (fc == NULL) return;
    zfree(fc->buffer);
    zfree(fc->read_data);
    zfree(fc);
}

#endif /* FAKE_CONNECTION_HPP */
