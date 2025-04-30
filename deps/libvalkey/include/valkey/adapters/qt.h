/*-
 * Copyright (C) 2014 Pietro Cerutti <gahr@gahr.ch>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef VALKEY_ADAPTERS_QT_H
#define VALKEY_ADAPTERS_QT_H
#include "../async.h"

#include <QSocketNotifier>

static void ValkeyQtAddRead(void *);
static void ValkeyQtDelRead(void *);
static void ValkeyQtAddWrite(void *);
static void ValkeyQtDelWrite(void *);
static void ValkeyQtCleanup(void *);

class ValkeyQtAdapter : public QObject {

    Q_OBJECT

    friend void ValkeyQtAddRead(void *adapter) {
        ValkeyQtAdapter *a = static_cast<ValkeyQtAdapter *>(adapter);
        a->addRead();
    }

    friend void ValkeyQtDelRead(void *adapter) {
        ValkeyQtAdapter *a = static_cast<ValkeyQtAdapter *>(adapter);
        a->delRead();
    }

    friend void ValkeyQtAddWrite(void *adapter) {
        ValkeyQtAdapter *a = static_cast<ValkeyQtAdapter *>(adapter);
        a->addWrite();
    }

    friend void ValkeyQtDelWrite(void *adapter) {
        ValkeyQtAdapter *a = static_cast<ValkeyQtAdapter *>(adapter);
        a->delWrite();
    }

    friend void ValkeyQtCleanup(void *adapter) {
        ValkeyQtAdapter *a = static_cast<ValkeyQtAdapter *>(adapter);
        a->cleanup();
    }

  public:
    ValkeyQtAdapter(QObject *parent = 0)
        : QObject(parent), m_ctx(0), m_read(0), m_write(0) {}

    ~ValkeyQtAdapter() {
        if (m_ctx != 0) {
            m_ctx->ev.data = NULL;
        }
    }

    int setContext(valkeyAsyncContext *ac) {
        if (ac->ev.data != NULL) {
            return VALKEY_ERR;
        }
        m_ctx = ac;
        m_ctx->ev.data = this;
        m_ctx->ev.addRead = ValkeyQtAddRead;
        m_ctx->ev.delRead = ValkeyQtDelRead;
        m_ctx->ev.addWrite = ValkeyQtAddWrite;
        m_ctx->ev.delWrite = ValkeyQtDelWrite;
        m_ctx->ev.cleanup = ValkeyQtCleanup;
        return VALKEY_OK;
    }

  private:
    void addRead() {
        if (m_read)
            return;
        m_read = new QSocketNotifier(m_ctx->c.fd, QSocketNotifier::Read, 0);
        connect(m_read, SIGNAL(activated(int)), this, SLOT(read()));
    }

    void delRead() {
        if (!m_read)
            return;
        delete m_read;
        m_read = 0;
    }

    void addWrite() {
        if (m_write)
            return;
        m_write = new QSocketNotifier(m_ctx->c.fd, QSocketNotifier::Write, 0);
        connect(m_write, SIGNAL(activated(int)), this, SLOT(write()));
    }

    void delWrite() {
        if (!m_write)
            return;
        delete m_write;
        m_write = 0;
    }

    void cleanup() {
        delRead();
        delWrite();
    }

  private slots:
    void read() { valkeyAsyncHandleRead(m_ctx); }
    void write() { valkeyAsyncHandleWrite(m_ctx); }

  private:
    valkeyAsyncContext *m_ctx;
    QSocketNotifier *m_read;
    QSocketNotifier *m_write;
};

#endif /* VALKEY_ADAPTERS_QT_H */
