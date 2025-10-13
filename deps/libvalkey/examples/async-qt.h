#ifndef VALKEY_EXAMPLE_QT_H
#define VALKEY_EXAMPLE_QT_H

#include <valkey/adapters/qt.h>

class ExampleQt : public QObject {

    Q_OBJECT

  public:
    ExampleQt(const char *value, QObject *parent = 0)
        : QObject(parent), m_value(value) {}

  signals:
    void finished();

  public slots:
    void run();

  private:
    void finish() { emit finished(); }

  private:
    const char *m_value;
    valkeyAsyncContext *m_ctx;
    ValkeyQtAdapter m_adapter;

    friend void getCallback(valkeyAsyncContext *, void *, void *);
};

#endif /* VALKEY_EXAMPLE_QT_H */
