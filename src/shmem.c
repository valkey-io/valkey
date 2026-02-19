/*
 * Shared memory connection type for Valkey
 * 100% userspace communication using POSIX shared memory
 */

#include "server.h"
#include "connection.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <semaphore.h>

#define SHMEM_RING_SIZE (1024 * 1024)  // 1MB ring buffer per direction

typedef struct {
    volatile size_t head;
    volatile size_t tail;
    volatile int closed;
    sem_t sem;
    char data[SHMEM_RING_SIZE];
} shmemRing;

typedef struct {
    shmemRing *tx;  // Transmit ring
    shmemRing *rx;  // Receive ring
} shmemShared;

typedef struct {
    connection conn;
    char *shm_name;
    int shm_fd;
    shmemShared *shared;
    size_t shm_size;
} shmemConnection;

static ConnectionType CT_Shmem;

/* Ring buffer helpers */
static size_t ring_available(shmemRing *ring) {
    size_t head = ring->head;
    size_t tail = ring->tail;
    return (tail >= head) ? (tail - head) : (SHMEM_RING_SIZE - head + tail);
}

static size_t ring_space(shmemRing *ring) {
    return SHMEM_RING_SIZE - ring_available(ring) - 1;
}

static size_t ring_read(shmemRing *ring, void *buf, size_t len) {
    size_t head = ring->head;
    size_t tail = ring->tail;
    size_t avail = (tail >= head) ? (tail - head) : (SHMEM_RING_SIZE - head + tail);
    
    if (avail == 0) return 0;
    if (len > avail) len = avail;
    
    size_t first = SHMEM_RING_SIZE - head;
    if (len <= first) {
        memcpy(buf, ring->data + head, len);
        ring->head = (head + len) % SHMEM_RING_SIZE;
    } else {
        memcpy(buf, ring->data + head, first);
        memcpy((char *)buf + first, ring->data, len - first);
        ring->head = len - first;
    }
    return len;
}

static size_t ring_write(shmemRing *ring, const void *buf, size_t len) {
    size_t head = ring->head;
    size_t tail = ring->tail;
    size_t space = SHMEM_RING_SIZE - ((tail >= head) ? (tail - head) : (SHMEM_RING_SIZE - head + tail)) - 1;
    
    if (space == 0) return 0;
    if (len > space) len = space;
    
    size_t first = SHMEM_RING_SIZE - tail;
    if (len <= first) {
        memcpy(ring->data + tail, buf, len);
        ring->tail = (tail + len) % SHMEM_RING_SIZE;
    } else {
        memcpy(ring->data + tail, buf, first);
        memcpy(ring->data, (const char *)buf + first, len - first);
        ring->tail = len - first;
    }
    sem_post(&ring->sem);
    return len;
}

/* Connection type implementation */
static int connShmemGetType(void) {
    return CONN_TYPE_MAX;  // Use next available type
}

static void connShmemInit(void) {
    // Nothing to initialize
}

static void connShmemCleanup(void) {
    // Nothing to cleanup
}

static connection *connShmemCreate(void) {
    shmemConnection *conn = zcalloc(sizeof(*conn));
    conn->conn.type = &CT_Shmem;
    conn->conn.state = CONN_STATE_NONE;
    conn->shm_fd = -1;
    return (connection *)conn;
}

static void connShmemClose(connection *conn_) {
    shmemConnection *conn = (shmemConnection *)conn_;
    
    if (conn->shared) {
        conn->shared->tx->closed = 1;
        sem_post(&conn->shared->rx->sem);
        munmap(conn->shared, conn->shm_size);
        conn->shared = NULL;
    }
    
    if (conn->shm_fd >= 0) {
        close(conn->shm_fd);
        conn->shm_fd = -1;
    }
    
    if (conn->shm_name) {
        shm_unlink(conn->shm_name);
        sdsfree(conn->shm_name);
        conn->shm_name = NULL;
    }
    
    conn->conn.state = CONN_STATE_CLOSED;
}

static void connShmemShutdown(connection *conn) {
    connShmemClose(conn);
}

static int connShmemConnect(connection *conn_, const char *addr, int port,
                            const char *src, int mp, ConnectionCallbackFunc handler) {
    shmemConnection *conn = (shmemConnection *)conn_;
    
    // addr is the shared memory name
    conn->shm_name = sdsnew(addr);
    conn->shm_size = sizeof(shmemShared);
    
    // Open existing shared memory (server creates it)
    conn->shm_fd = shm_open(addr, O_RDWR, 0666);
    if (conn->shm_fd < 0) return C_ERR;
    
    conn->shared = mmap(NULL, conn->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, conn->shm_fd, 0);
    if (conn->shared == MAP_FAILED) {
        close(conn->shm_fd);
        return C_ERR;
    }
    
    // Client: tx is server's rx, rx is server's tx
    shmemRing *tmp = conn->shared->tx;
    conn->shared->tx = conn->shared->rx;
    conn->shared->rx = tmp;
    
    conn->conn.state = CONN_STATE_CONNECTED;
    if (handler) handler(conn_);
    return C_OK;
}

static int connShmemAccept(connection *conn_, ConnectionCallbackFunc handler) {
    shmemConnection *conn = (shmemConnection *)conn_;
    conn->conn.state = CONN_STATE_CONNECTED;
    if (handler) handler(conn_);
    return C_OK;
}

static int connShmemWrite(connection *conn_, const void *data, size_t len) {
    shmemConnection *conn = (shmemConnection *)conn_;
    if (!conn->shared || conn->shared->tx->closed) return -1;
    
    size_t written = ring_write(conn->shared->tx, data, len);
    return written > 0 ? written : -1;
}

static int connShmemRead(connection *conn_, void *buf, size_t len) {
    shmemConnection *conn = (shmemConnection *)conn_;
    if (!conn->shared) return -1;
    
    if (conn->shared->rx->closed) return 0;
    
    size_t nread = ring_read(conn->shared->rx, buf, len);
    return nread;
}

static int connShmemSetWriteHandler(connection *conn, ConnectionCallbackFunc handler, int barrier) {
    conn->write_handler = handler;
    if (barrier) conn->flags |= CONN_FLAG_WRITE_BARRIER;
    return C_OK;
}

static int connShmemSetReadHandler(connection *conn, ConnectionCallbackFunc handler) {
    conn->read_handler = handler;
    return C_OK;
}

static void connShmemAeHandler(struct aeEventLoop *el, int fd, void *clientData, int mask) {
    // Polling-based handler - check for data
    connection *conn = clientData;
    shmemConnection *sc = (shmemConnection *)conn;
    
    if (!sc->shared) return;
    
    if (mask & AE_READABLE && conn->read_handler) {
        if (ring_available(sc->shared->rx) > 0) {
            conn->read_handler(conn);
        }
    }
    
    if (mask & AE_WRITABLE && conn->write_handler) {
        if (ring_space(sc->shared->tx) > 0) {
            conn->write_handler(conn);
        }
    }
}

static int connShmemHasPendingData(void) {
    return 0;
}

static int connShmemProcessPendingData(void) {
    return 0;
}

static connection *connShmemCreateAccepted(int fd, void *priv) {
    shmemConnection *conn = (shmemConnection *)connShmemCreate();
    
    // fd is actually a pointer to the shared memory name
    char *name = (char *)priv;
    conn->shm_name = sdsnew(name);
    conn->shm_size = sizeof(shmemShared);
    
    conn->shm_fd = shm_open(name, O_RDWR | O_CREAT, 0666);
    if (conn->shm_fd < 0) {
        zfree(conn);
        return NULL;
    }
    
    ftruncate(conn->shm_fd, conn->shm_size);
    
    conn->shared = mmap(NULL, conn->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, conn->shm_fd, 0);
    if (conn->shared == MAP_FAILED) {
        close(conn->shm_fd);
        zfree(conn);
        return NULL;
    }
    
    // Initialize rings
    memset(conn->shared, 0, conn->shm_size);
    sem_init(&conn->shared->tx->sem, 1, 0);
    sem_init(&conn->shared->rx->sem, 1, 0);
    
    conn->conn.state = CONN_STATE_ACCEPTING;
    return (connection *)conn;
}

static int connShmemAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    shmemConnection *sc = (shmemConnection *)conn;
    if (ip) snprintf(ip, ip_len, "shmem:%s", sc->shm_name ? sc->shm_name : "unknown");
    if (port) *port = 0;
    return C_OK;
}

static int connShmemIsLocal(connection *conn) {
    return 1;  // Always local
}

static ConnectionType CT_Shmem = {
    .get_type = connShmemGetType,
    .init = connShmemInit,
    .cleanup = connShmemCleanup,
    .ae_handler = connShmemAeHandler,
    .accept_handler = NULL,
    .addr = connShmemAddr,
    .is_local = connShmemIsLocal,
    .listen = NULL,
    .closeListener = NULL,
    .conn_create = connShmemCreate,
    .conn_create_accepted = connShmemCreateAccepted,
    .close = connShmemClose,
    .shutdown = connShmemShutdown,
    .connect = connShmemConnect,
    .blocking_connect = NULL,
    .accept = connShmemAccept,
    .write = connShmemWrite,
    .writev = NULL,
    .read = connShmemRead,
    .set_write_handler = connShmemSetWriteHandler,
    .set_read_handler = connShmemSetReadHandler,
    .get_last_error = NULL,
    .sync_write = NULL,
    .sync_read = NULL,
    .sync_readline = NULL,
    .has_pending_data = connShmemHasPendingData,
    .process_pending_data = connShmemProcessPendingData,
    .postpone_update_state = NULL,
    .update_state = NULL,
    .get_peer_cert = NULL,
    .get_peer_username = NULL,
    .connIntegrityChecked = NULL,
};

int valkeyRegisterShmemConnectionType(void) {
    return connTypeRegister(&CT_Shmem);
}
