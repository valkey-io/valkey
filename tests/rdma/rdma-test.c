/* ==========================================================================
 * rdma-test.c - a simple test client for Valkey Over RDMA (Linux only)
 * --------------------------------------------------------------------------
 * Copyright (C) 2021-2024  zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under BSD 3-Clause, License 1 of the COPYING file in
 * the top-level directory.
 * ==========================================================================
 */

#ifndef __linux__    /* currently RDMA is only supported on Linux */

#error "BUILD ERROR: RDMA is only supported on Linux"

#else /* __linux__ */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <rdma/rdma_cma.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct valkeyRdmaFeature {
    /* defined as following Opcodes */
    uint16_t opcode;
    /* select features */
    uint16_t select;
    uint8_t rsvd[20];
    /* feature bits */
    uint64_t features;
} valkeyRdmaFeature;

typedef struct valkeyRdmaKeepalive {
    /* defined as following Opcodes */
    uint16_t opcode;
    uint8_t rsvd[30];
} valkeyRdmaKeepalive;

typedef struct valkeyRdmaMemory {
    /* defined as following Opcodes */
    uint16_t opcode;
    uint8_t rsvd[14];
    /* address of a transfer buffer which is used to receive remote streaming data,
     * aka 'RX buffer address'. The remote side should use this as 'TX buffer address' */
    uint64_t addr;
    /* length of the 'RX buffer' */
    uint32_t length;
    /* the RDMA remote key of 'RX buffer' */
    uint32_t key;
} valkeyRdmaMemory;

typedef union valkeyRdmaCmd {
    valkeyRdmaFeature feature;
    valkeyRdmaKeepalive keepalive;
    valkeyRdmaMemory memory;
} valkeyRdmaCmd;

typedef enum valkeyRdmaOpcode {
    GetServerFeature = 0,
    SetClientFeature = 1,
    Keepalive = 2,
    RegisterXferMemory = 3,
} valkeyRdmaOpcode;

#define MAX_THREADS 32
#define UNUSED(x) (void)(x)
#define MIN(a, b) (a) < (b) ? a : b
#define VALKEY_RDMA_MAX_WQE 1024
#define VALKEY_RDMA_DEFAULT_RX_LEN  (1024*1024)
#define VALKEY_RDMA_INVALID_OPCODE 0xffff

typedef struct RdmaContext {
    struct rdma_cm_id *cm_id;
    struct rdma_event_channel *cm_channel;
    struct ibv_comp_channel *comp_channel;
    struct ibv_cq *cq;
    struct ibv_pd *pd;
    bool connected;

    /* TX */
    char *tx_addr;
    uint32_t tx_length;
    uint32_t tx_offset;
    uint32_t tx_key;
    char *send_buf;
    uint32_t send_length;
    uint32_t send_ops;
    struct ibv_mr *send_mr;

    /* RX */
    uint32_t rx_offset;
    char *recv_buf;
    unsigned int recv_length;
    unsigned int recv_offset;
    struct ibv_mr *recv_mr;

    /* CMD 0 ~ VALKEY_RDMA_MAX_WQE for recv buffer
     * VALKEY_RDMA_MAX_WQE ~ 2 * VALKEY_RDMA_MAX_WQE -1 for send buffer */
    valkeyRdmaCmd *cmd_buf;
    struct ibv_mr *cmd_mr;
} RdmaContext;

static int valkeySetFdBlocking(int fd, int blocking) {
    int flags;

    flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        return -1;
    }

    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    return fcntl(fd, F_SETFL, flags);
}

#define rdmaFatal(msg)                                          \
    do {                                                        \
        fprintf(stderr, "%s:%d %s\n", __func__, __LINE__, msg); \
        assert(0);                                              \
    } while (0)

static inline long valkeyNowMs(void) {
    struct timeval tv;

    if (gettimeofday(&tv, NULL) < 0)
            return -1;

    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int rdmaPostRecv(RdmaContext *ctx, struct rdma_cm_id *cm_id, valkeyRdmaCmd *cmd) {
    struct ibv_sge sge;
    size_t length = sizeof(valkeyRdmaCmd);
    struct ibv_recv_wr recv_wr, *bad_wr;


    sge.addr = (uint64_t)cmd;
    sge.length = length;
    sge.lkey = ctx->cmd_mr->lkey;

    recv_wr.wr_id = (uint64_t)cmd;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;
    recv_wr.next = NULL;

    if (ibv_post_recv(cm_id->qp, &recv_wr, &bad_wr)) {
        return -1;
    }

    return 0;
}

static void rdmaDestroyIoBuf(RdmaContext *ctx) {
    if (ctx->recv_mr) {
        ibv_dereg_mr(ctx->recv_mr);
        ctx->recv_mr = NULL;
    }

    free(ctx->recv_buf);
    ctx->recv_buf = NULL;

    if (ctx->send_mr) {
        ibv_dereg_mr(ctx->send_mr);
        ctx->send_mr = NULL;
    }

    free(ctx->send_buf);
    ctx->send_buf = NULL;

    if (ctx->cmd_mr) {
        ibv_dereg_mr(ctx->cmd_mr);
        ctx->cmd_mr = NULL;
    }

    free(ctx->cmd_buf);
    ctx->cmd_buf = NULL;
}

static int rdmaSetupIoBuf(RdmaContext *ctx, struct rdma_cm_id *cm_id) {
    int access = IBV_ACCESS_LOCAL_WRITE;
    size_t length = sizeof(valkeyRdmaCmd) * VALKEY_RDMA_MAX_WQE * 2;
    valkeyRdmaCmd *cmd;
    int i;

    /* setup CMD buf & MR */
    ctx->cmd_buf = calloc(length, 1);
    ctx->cmd_mr = ibv_reg_mr(ctx->pd, ctx->cmd_buf, length, access);
    if (!ctx->cmd_mr) {
        rdmaFatal("RDMA: reg recv mr failed");
        goto destroy_iobuf;
    }

    for (i = 0; i < VALKEY_RDMA_MAX_WQE; i++) {
        cmd = ctx->cmd_buf + i;

        if (rdmaPostRecv(ctx, cm_id, cmd) == -1) {
            rdmaFatal("RDMA: post recv failed");
            goto destroy_iobuf;
        }
    }

    for (i = VALKEY_RDMA_MAX_WQE; i < VALKEY_RDMA_MAX_WQE * 2; i++) {
        cmd = ctx->cmd_buf + i;
        cmd->keepalive.opcode = VALKEY_RDMA_INVALID_OPCODE;
    }

    /* setup recv buf & MR */
    access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    length = VALKEY_RDMA_DEFAULT_RX_LEN;
    ctx->recv_buf = calloc(length, 1);
    ctx->recv_length = length;
    ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, length, access);
    if (!ctx->recv_mr) {
        rdmaFatal("RDMA: reg send mr failed");
        goto destroy_iobuf;
    }

    return 0;

destroy_iobuf:
    rdmaDestroyIoBuf(ctx);
    return -1;
}

static int rdmaAdjustSendbuf(RdmaContext *ctx, unsigned int length) {
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    if (length == ctx->send_length) {
        return 0;
    }

    /* try to free old MR & buffer */
    if (ctx->send_length) {
        ibv_dereg_mr(ctx->send_mr);
        free(ctx->send_buf);
        ctx->send_length = 0;
    }

    /* create a new buffer & MR */
    ctx->send_buf = calloc(length, 1);
    ctx->send_length = length;
    ctx->send_mr = ibv_reg_mr(ctx->pd, ctx->send_buf, length, access);
    if (!ctx->send_mr) {
        rdmaFatal("RDMA: reg send buf mr failed");
        free(ctx->send_buf);
        ctx->send_buf = NULL;
        ctx->send_length = 0;
        return -1;
    }

    return 0;
}


static int rdmaSendCommand(RdmaContext *ctx, struct rdma_cm_id *cm_id, valkeyRdmaCmd *cmd) {
    struct ibv_send_wr send_wr, *bad_wr;
    struct ibv_sge sge;
    valkeyRdmaCmd *_cmd;
    int i;
    int ret;

    /* find an unused cmd buffer */
    for (i = VALKEY_RDMA_MAX_WQE; i < 2 * VALKEY_RDMA_MAX_WQE; i++) {
        _cmd = ctx->cmd_buf + i;
        if (_cmd->keepalive.opcode == VALKEY_RDMA_INVALID_OPCODE) {
            break;
        }
    }

    assert(i < 2 * VALKEY_RDMA_MAX_WQE);

    memcpy(_cmd, cmd, sizeof(valkeyRdmaCmd));
    sge.addr = (uint64_t)_cmd;
    sge.length = sizeof(valkeyRdmaCmd);
    sge.lkey = ctx->cmd_mr->lkey;

    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.wr_id = (uint64_t)_cmd;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.next = NULL;
    ret = ibv_post_send(cm_id->qp, &send_wr, &bad_wr);
    if (ret) {
        return -1;
    }

    return 0;
}

static int connRdmaRegisterRx(RdmaContext *ctx, struct rdma_cm_id *cm_id) {
    valkeyRdmaCmd cmd = { 0 };

    cmd.memory.opcode = htons(RegisterXferMemory);
    cmd.memory.addr = htobe64((uint64_t)ctx->recv_buf);
    cmd.memory.length = htonl(ctx->recv_length);
    cmd.memory.key = htonl(ctx->recv_mr->rkey);

    ctx->rx_offset = 0;
    ctx->recv_offset = 0;

    return rdmaSendCommand(ctx, cm_id, &cmd);
}

static int connRdmaHandleRecv(RdmaContext *ctx, struct rdma_cm_id *cm_id, valkeyRdmaCmd *cmd, uint32_t byte_len) {
    if (byte_len != sizeof(valkeyRdmaCmd)) {
        rdmaFatal("RDMA: FATAL error, recv corrupted cmd");
        return -1;
    }

    switch (ntohs(cmd->keepalive.opcode)) {
    case RegisterXferMemory:
        ctx->tx_addr = (char *)be64toh(cmd->memory.addr);
        ctx->tx_length = ntohl(cmd->memory.length);
        ctx->tx_key = ntohl(cmd->memory.key);
        ctx->tx_offset = 0;
        rdmaAdjustSendbuf(ctx, ctx->tx_length);
        break;

    case Keepalive:
        break;

    default:
        rdmaFatal("RDMA: FATAL error, unknown cmd");
        return -1;
    }

    return rdmaPostRecv(ctx, cm_id, cmd);
}

static int connRdmaHandleRecvImm(RdmaContext *ctx, struct rdma_cm_id *cm_id, valkeyRdmaCmd *cmd, uint32_t byte_len) {
    assert(byte_len + ctx->rx_offset <= ctx->recv_length);
    ctx->rx_offset += byte_len;

    return rdmaPostRecv(ctx, cm_id, cmd);
}

static int connRdmaHandleSend(valkeyRdmaCmd *cmd) {
    /* mark this cmd has already sent */
    memset(cmd, 0x00, sizeof(*cmd));
    cmd->keepalive.opcode = VALKEY_RDMA_INVALID_OPCODE;

    return 0;
}

static int connRdmaHandleWrite(RdmaContext *ctx, uint32_t byte_len) {
    UNUSED(ctx);
    UNUSED(byte_len);

    return 0;
}

static int connRdmaHandleCq(RdmaContext *ctx) {
    struct rdma_cm_id *cm_id = ctx->cm_id;
    struct ibv_cq *ev_cq = NULL;
    void *ev_ctx = NULL;
    struct ibv_wc wc = {0};
    valkeyRdmaCmd *cmd;
    int ret;

    if (ibv_get_cq_event(ctx->comp_channel, &ev_cq, &ev_ctx) < 0) {
        if (errno != EAGAIN) {
            rdmaFatal("RDMA: get cq event failed");
            return -1;
        }
    } else if (ibv_req_notify_cq(ev_cq, 0)) {
        rdmaFatal("RDMA: notify cq failed");
        return -1;
    }

pollcq:
    ret = ibv_poll_cq(ctx->cq, 1, &wc);
    if (ret < 0) {
        rdmaFatal("RDMA: poll cq failed");
        return -1;
    } else if (ret == 0) {
        return 0;
    }

    ibv_ack_cq_events(ctx->cq, 1);

    if (wc.status != IBV_WC_SUCCESS) {
        rdmaFatal("RDMA: send/recv failed");
        return -1;
    }

    switch (wc.opcode) {
    case IBV_WC_RECV:
        cmd = (valkeyRdmaCmd *)wc.wr_id;
        if (connRdmaHandleRecv(ctx, cm_id, cmd, wc.byte_len) == -1) {
            return -1;
        }

        break;

    case IBV_WC_RECV_RDMA_WITH_IMM:
        cmd = (valkeyRdmaCmd *)wc.wr_id;
        if (connRdmaHandleRecvImm(ctx, cm_id, cmd, ntohl(wc.imm_data)) == -1) {
            return -1;
        }

        break;
    case IBV_WC_RDMA_WRITE:
        if (connRdmaHandleWrite(ctx, wc.byte_len) == -1) {
            return -1;
        }

        break;
    case IBV_WC_SEND:
        cmd = (valkeyRdmaCmd *)wc.wr_id;
        if (connRdmaHandleSend(cmd) == -1) {
            return -1;
        }

        break;
    default:
        rdmaFatal("RDMA: unexpected opcode");
        return -1;
    }

    goto pollcq;

    return 0;
}

static ssize_t valkeyRdmaRead(RdmaContext *ctx, char *buf, size_t data_len) {
    struct rdma_cm_id *cm_id = ctx->cm_id;
    struct pollfd pfd;
    long timed = 1000;
    long start = valkeyNowMs();
    uint32_t toread, remained;

copy:
    if (ctx->recv_offset < ctx->rx_offset) {
        remained = ctx->rx_offset - ctx->recv_offset;
        toread = MIN(remained, data_len);

        memcpy(buf, ctx->recv_buf + ctx->recv_offset, toread);
        ctx->recv_offset += toread;

        if (ctx->recv_offset == ctx->recv_length) {
            connRdmaRegisterRx(ctx, cm_id);
        }

        return toread;
    }

pollcq:
    /* try to poll a CQ firstly */
    if (connRdmaHandleCq(ctx) == -1) {
        return -1;
    }

    if (ctx->recv_offset < ctx->rx_offset) {
        goto copy;
    }

    pfd.fd = ctx->comp_channel->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 1000) < 0) {
        return -1;
    }

    if ((valkeyNowMs() - start) < timed) {
        goto pollcq;
    }

    rdmaFatal("RDMA: read timeout");
    return -1;
}

static ssize_t valkeyRdmaReadFull(RdmaContext *ctx, char *buf, size_t data_len) {
    size_t inbytes = 0;

    do {
        inbytes += valkeyRdmaRead(ctx, buf + inbytes, data_len - inbytes);
    } while (inbytes < data_len);

    return data_len;
}

static size_t connRdmaSend(RdmaContext *ctx, struct rdma_cm_id *cm_id, const void *data, size_t data_len) {
    struct ibv_send_wr send_wr, *bad_wr;
    struct ibv_sge sge;
    uint32_t off = ctx->tx_offset;
    char *addr = ctx->send_buf + off;
    char *remote_addr = ctx->tx_addr + off;
    int ret;

    assert(data_len <= ctx->tx_length);
    memcpy(addr, data, data_len);

    sge.addr = (uint64_t)addr;
    sge.lkey = ctx->send_mr->lkey;
    sge.length = data_len;

    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    send_wr.send_flags = (++ctx->send_ops % VALKEY_RDMA_MAX_WQE) ? 0 : IBV_SEND_SIGNALED;
    send_wr.imm_data = htonl(data_len);
    send_wr.wr.rdma.remote_addr = (uint64_t)remote_addr;
    send_wr.wr.rdma.rkey = ctx->tx_key;
    send_wr.next = NULL;
    ret = ibv_post_send(cm_id->qp, &send_wr, &bad_wr);
    if (ret) {
        return -1;
    }

    ctx->tx_offset += data_len;

    return data_len;
}

static ssize_t valkeyRdmaWrite(RdmaContext *ctx, char *buf, size_t data_len) {
    struct rdma_cm_id *cm_id = ctx->cm_id;
    struct pollfd pfd;
    long timed = 1000;
    long start = valkeyNowMs();
    uint32_t towrite, wrote = 0;
    size_t ret;

    /* try to pollcq to */
    goto pollcq;

waitcq:
    pfd.fd = ctx->comp_channel->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 1) < 0) {
        return -1;
    }

pollcq:
    if (connRdmaHandleCq(ctx) == -1) {
        return -1;
    }

    assert(ctx->tx_offset <= ctx->tx_length);
    if (ctx->tx_offset == ctx->tx_length) {
        /* wait a new TX buffer */
        goto waitcq;
    }

    towrite = MIN(ctx->tx_length - ctx->tx_offset, data_len - wrote);
    ret = connRdmaSend(ctx, cm_id, buf + wrote, towrite);
    if (ret == (size_t)-1) {
        return -1;
    }

    wrote += ret;
    if (wrote == data_len) {
        return data_len;
    }

    if ((valkeyNowMs() - start) < timed) {
        goto waitcq;
    }

    rdmaFatal("RDMA: write timeout");

    return -1;
}

static void valkeyRdmaClose(RdmaContext *ctx) {
    struct rdma_cm_id *cm_id = ctx->cm_id;

    connRdmaHandleCq(ctx);
    rdma_disconnect(cm_id);
    ibv_destroy_cq(ctx->cq);
    rdmaDestroyIoBuf(ctx);
    ibv_destroy_qp(cm_id->qp);
    ibv_destroy_comp_channel(ctx->comp_channel);
    ibv_dealloc_pd(ctx->pd);
    rdma_destroy_id(cm_id);

    rdma_destroy_event_channel(ctx->cm_channel);
}

static void valkeyRdmaFree(void *privctx) {
    if (!privctx)
        return;

    free(privctx);
}

static int valkeyRdmaConnect(RdmaContext *ctx, struct rdma_cm_id *cm_id) {
    struct ibv_comp_channel *comp_channel = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_qp_init_attr init_attr = {0};
    struct rdma_conn_param conn_param = {0};

    pd = ibv_alloc_pd(cm_id->verbs);
    if (!pd) {
        rdmaFatal("RDMA: alloc pd failed");
        goto error;
    }

    comp_channel = ibv_create_comp_channel(cm_id->verbs);
    if (!comp_channel) {
        rdmaFatal("RDMA: alloc pd failed");
        goto error;
    }

    if (valkeySetFdBlocking(comp_channel->fd, 0) != 0) {
        rdmaFatal("RDMA: set recv comp channel fd non-block failed");
        goto error;
    }

    cq = ibv_create_cq(cm_id->verbs, VALKEY_RDMA_MAX_WQE * 2, ctx, comp_channel, 0);
    if (!cq) {
        rdmaFatal("RDMA: create send cq failed");
        goto error;
    }

    if (ibv_req_notify_cq(cq, 0)) {
        rdmaFatal("RDMA: notify send cq failed");
        goto error;
    }

    /* create qp with attr */
    init_attr.cap.max_send_wr = VALKEY_RDMA_MAX_WQE;
    init_attr.cap.max_recv_wr = VALKEY_RDMA_MAX_WQE;
    init_attr.cap.max_send_sge = 1;
    init_attr.cap.max_recv_sge = 1;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.send_cq = cq;
    init_attr.recv_cq = cq;
    if (rdma_create_qp(cm_id, pd, &init_attr)) {
        rdmaFatal("RDMA: create qp failed");
        goto error;
    }

    ctx->cm_id = cm_id;
    ctx->comp_channel = comp_channel;
    ctx->cq = cq;
    ctx->pd = pd;

    if (rdmaSetupIoBuf(ctx, cm_id) != 0)
        goto free_qp;

    /* rdma connect with param */
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 7;
    conn_param.rnr_retry_count = 7;
    if (rdma_connect(cm_id, &conn_param)) {
        rdmaFatal("RDMA: connect failed");
        goto destroy_iobuf;
    }

    return 0;

destroy_iobuf:
    rdmaDestroyIoBuf(ctx);
free_qp:
    ibv_destroy_qp(cm_id->qp);
error:
    if (cq)
        ibv_destroy_cq(cq);
    if (pd)
        ibv_dealloc_pd(pd);
    if (comp_channel)
        ibv_destroy_comp_channel(comp_channel);

    return -1;
}

static int valkeyRdmaEstablished(RdmaContext *ctx, struct rdma_cm_id *cm_id) {

    /* it's time to tell redis we have already connected */
    ctx->connected = true;

    return connRdmaRegisterRx(ctx, cm_id);
}

static int valkeyRdmaCM(RdmaContext *ctx, int timeout) {
    struct rdma_cm_event *event;
    char errorstr[128];
    int ret = -1;

    while (rdma_get_cm_event(ctx->cm_channel, &event) == 0) {
        /* printf("GET RDMA CM EVENT: %s\n", rdma_event_str(event->event)); */
        switch (event->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
            if (timeout < 0 || timeout > 100)
                timeout = 100; /* at most 100ms to resolve route */
            ret = rdma_resolve_route(event->id, timeout);
            if (ret) {
                rdmaFatal("RDMA: route resolve failed");
            }
            break;
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
            ret = valkeyRdmaConnect(ctx, event->id);
            break;
        case RDMA_CM_EVENT_ESTABLISHED:
            ret = valkeyRdmaEstablished(ctx, event->id);
            break;
        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
            ret = -1;
            rdmaFatal("RDMA: connect timeout");
            break;
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
        case RDMA_CM_EVENT_DISCONNECTED:
        case RDMA_CM_EVENT_ADDR_CHANGE:
        default:
            snprintf(errorstr, sizeof(errorstr), "RDMA: connect failed - %s", rdma_event_str(event->event));
            rdmaFatal(errorstr);
            ret = -1;
            break;
        }

        rdma_ack_cm_event(event);
    }

    return ret;
}

static int valkeyRdmaWaitConn(RdmaContext *ctx, long timeout) {
    int timed;
    struct pollfd pfd;
    long now = valkeyNowMs();
    long start = now;

    while (now - start < timeout) {
        timed = (int)(timeout - (now - start));

        pfd.fd = ctx->cm_channel->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, timed) < 0) {
            return -1;
        }

        if (valkeyRdmaCM(ctx, timed) == -1) {
            return -1;
        }

        if (ctx->connected) {
            return 0;
        }

        now = valkeyNowMs();
    }

    return -1;
}

static RdmaContext *valkeyContextConnectRdma(const char *addr, int port, int timeout) {
    int ret;
    char _port[6];  /* strlen("65535"); */
    struct addrinfo hints, *servinfo = NULL, *p;
    RdmaContext *ctx = NULL;
    struct sockaddr_storage saddr;
    long start = valkeyNowMs(), timed;

    snprintf(_port, 6, "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if ((ret = getaddrinfo(addr, _port, &hints, &servinfo)) != 0) {
         hints.ai_family = AF_INET6;
         if ((ret = getaddrinfo(addr, _port, &hints, &servinfo)) != 0) {
            rdmaFatal(gai_strerror(ret));
            return NULL;
        }
    }

    ctx = calloc(sizeof(RdmaContext), 1);
    if (!ctx) {
        rdmaFatal("Out of memory");
        goto free_rdma;
    }

    ctx->cm_channel = rdma_create_event_channel();
    if (!ctx->cm_channel) {
        rdmaFatal("RDMA: create event channel failed");
        goto free_rdma;
    }

    if (rdma_create_id(ctx->cm_channel, &ctx->cm_id, (void *)ctx, RDMA_PS_TCP)) {
        rdmaFatal("RDMA: create id failed");
        goto free_rdma;
    }

    if ((valkeySetFdBlocking(ctx->cm_channel->fd, 0) != 0)) {
        rdmaFatal("RDMA: set cm channel fd non-block failed");
        goto free_rdma;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        if (p->ai_family == PF_INET) {
                memcpy(&saddr, p->ai_addr, sizeof(struct sockaddr_in));
                ((struct sockaddr_in *)&saddr)->sin_port = htons(port);
        } else if (p->ai_family == PF_INET6) {
                memcpy(&saddr, p->ai_addr, sizeof(struct sockaddr_in6));
                ((struct sockaddr_in6 *)&saddr)->sin6_port = htons(port);
        } else {
            rdmaFatal("RDMA: unsupported family");
            goto free_rdma;
        }

        /* resolve addr as most 100ms */
        if (rdma_resolve_addr(ctx->cm_id, NULL, (struct sockaddr *)&saddr, 100)) {
            continue;
        }

        timed = timeout - (valkeyNowMs() - start);
        if ((valkeyRdmaWaitConn(ctx, timed) == 0) && ctx->connected) {
            ret = 0;
            goto end;
        }
    }

    if ((!ctx->connected) && (p == NULL)) {
        rdmaFatal("RDMA: resolve failed");
    }

free_rdma:
    if (ctx->cm_id) {
        rdma_destroy_id(ctx->cm_id);
    }
    if (ctx->cm_channel) {
        rdma_destroy_event_channel(ctx->cm_channel);
    }

    if (ctx) {
        free(ctx);
    }

end:
    if(servinfo) {
        freeaddrinfo(servinfo);
    }

    return ctx;
}

static int port = 6379;
static char *host = NULL;
static int minkeys = 128;
static int maxkeys = 8192;
static int keysize = 1024 + 1; /* for '\0' terminator */

struct test_kv_pair {
    char key[32]; /* "THREAD01-000001" */
    char *value;
};

static void *test_routine(void *arg) {
    pid_t tid = gettid();
    RdmaContext *ctx;
    struct test_kv_pair *kv_pairs = NULL, *kv_pair;
    int keys;

    ctx = valkeyContextConnectRdma(host, port, 1000);
    if (!ctx) {
         rdmaFatal("RDMA connect failed");
    }

    int bufsize = keysize + 128;
    char *inbuf = malloc(bufsize);
    char *outbuf = malloc(bufsize);
    int inbytes, outbytes;

    /* # round 1, test PING */
    char *pingcmd = "*1\r\n$4\r\nPING\r\n";
    char *pingresp = "+PONG\r\n";

    valkeyRdmaWrite(ctx, pingcmd, strlen(pingcmd));
    inbytes = valkeyRdmaReadFull(ctx, inbuf, strlen(pingresp));
    assert(!strncmp(pingresp, inbuf, inbytes));
    printf("Valkey Over RDMA test thread[%d] PING/PONG [OK]\n", tid);

    /* prepare random KV for SET/GET */
    keys = random() % (maxkeys - minkeys) + minkeys;
    kv_pairs = calloc(sizeof(struct test_kv_pair), keys);

    for (int i = 0; i < keys; i++) {
        kv_pair = &kv_pairs[i];
        snprintf(kv_pair->key, sizeof(kv_pair->key) - 1, "THREAD%02d-%06d", tid, i);
        kv_pair->value = calloc(keysize, 1);
        for (int k = 0; k < keysize - 1; k++) {
            kv_pair->value[k] = 'A' + random() % 26; /* generate upper case string */
        }
    }
    printf("Valkey Over RDMA test thread[%d] prepare %d KVs [OK]\n", tid, keys);

    /* # round 2, test SET */
    char *okresp = "+OK\r\n";

    for (int i = 0; i < keys; i++) {
        kv_pair = &kv_pairs[i];
        /* build SET command */
        outbytes = sprintf(outbuf, "*3\r\n$3\r\nSET\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n",
                           strlen(kv_pair->key), kv_pair->key,
                           strlen(kv_pair->value), kv_pair->value);
        valkeyRdmaWrite(ctx, outbuf, outbytes);
        inbytes = valkeyRdmaReadFull(ctx, inbuf, strlen(okresp));
        assert(!strncmp("+OK\r\n", inbuf, inbytes));
    }
    printf("Valkey Over RDMA test thread[%d] SET %d KVs [OK]\n", tid, keys);

    /* # round 3, test BGSAVE, to avoid "-ERR Background save already", run BGSAVE only once */
    char *bgsavecmd = "*1\r\n$6\r\nBGSAVE\r\n";
    char *bgsaveresp = "+Background saving started\r\n";
    static int bgsaved;

    if (!__atomic_fetch_add(&bgsaved, 1, __ATOMIC_SEQ_CST)) {
        valkeyRdmaWrite(ctx, bgsavecmd, strlen(bgsavecmd));
        inbytes = valkeyRdmaReadFull(ctx, inbuf, strlen(bgsaveresp));
        assert(!strncmp(bgsaveresp, inbuf, inbytes));
        printf("Valkey Over RDMA test thread[%d] BGSAVE [OK]\n", tid);
    }

    /* # round 4, test GET. also verify all the value already set */
    char *getrespprex = "$1024\r\n";
    int getrespprexlen = strlen(getrespprex);

    for (int i = 0; i < keys; i++) {
        kv_pair = &kv_pairs[i];
        /* build GET command */
        outbytes = sprintf(outbuf, "*2\r\n$3\r\nGET\r\n$%ld\r\n%s\r\n",
                           strlen(kv_pair->key), kv_pair->key);
        valkeyRdmaWrite(ctx, outbuf, outbytes);
        inbytes = valkeyRdmaReadFull(ctx, inbuf, getrespprexlen + strlen(kv_pair->value) + 2);
        assert(!strncmp(getrespprex, inbuf, getrespprexlen));
        assert(!strncmp(kv_pair->value, inbuf + getrespprexlen, strlen(kv_pair->value)));
    }
    printf("Valkey Over RDMA test thread[%d] GET %d KVs [OK]\n", tid, keys);

    return NULL;
}

void usage(char *proc) {
    printf("%s usage:\n", proc);
    printf("\t--help/-H\n");
    printf("\t--host/-h HOSTADDR\n");
    printf("\t--port/-p PORT\n");
    printf("\t--maxkeys/-M MAXKEYS\n");
    printf("\t--minkeys/-M MINKEYS\n");
    printf("\t--thread/-t THREADS\n");
}

int main(int argc, char *argv[])
{
    int c, args;
    int nr_threads = 0;
    pthread_t threads[MAX_THREADS];

    static struct option long_opts[] = {
        { "help", no_argument, NULL, 'H' },
        { "host", required_argument, NULL, 'h' },
        { "port", required_argument, NULL, 'p' },
        { "maxkeys", required_argument, NULL, 'M' },
        { "minkeys", required_argument, NULL, 'm' },
        { "thread", required_argument, NULL, 't' },
    };
    static char *short_opts = "Hh:p:t:M:m:";

    while (1) {
        c = getopt_long(argc, argv, short_opts, long_opts, &args);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'h':
            host = optarg;
            break;

        case 'p':
            port = atoi(optarg);
            if (port <= 0 || port > 65535) {
                rdmaFatal("invalid port");
            }
            break;

        case 't':
            nr_threads = atoi(optarg);
            if (nr_threads < 0 || nr_threads > MAX_THREADS) {
                rdmaFatal("--threads/-t is expected as [0, 32]");
            }
            break;

        case 'M':
            maxkeys = atoi(optarg);
            break;

        case 'm':
            minkeys = atoi(optarg);
            break;

        case 'H':
            usage(argv[0]);
            exit(0);

        default:
            usage(argv[0]);
            exit(-1); /* this is not considered as success, to avoid auto-test workaround */
        }
    }

    if (!host) {
         rdmaFatal("missing --host/-H");
    }

    if (minkeys > maxkeys) {
         rdmaFatal("minkeys should less than maxkeys");
    }

    /* To make the test randomly */
    srandom(time(NULL) ^ getpid());

    /* main thread mode */
    if (!nr_threads) {
        printf("Test a single client in main thread ...\n");
        test_routine(NULL);

        return 0;
    }

    /* multi threads mode */
    for (int i = 0; i < nr_threads; i++) {
        assert(!pthread_create(&threads[i], NULL, test_routine, NULL));
    }

    for (int i = 0; i < nr_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("Valkey Over RDMA test [OK]\n");

    return 0;
}

#endif   /* __linux__ */
