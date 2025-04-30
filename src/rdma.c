/*
 * Copyright (c) 2024, zhenwei pi <pizhenwei@bytedance.com>
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
 *   * Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
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

#ifdef __linux__ /* currently RDMA is only supported on Linux */

#define _GNU_SOURCE
#include "rdma.h"

#include "async.h"
#include "valkey.h"
#include "valkey_private.h"
#include "vkutil.h"

#include <arpa/inet.h>
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <rdma/rdma_cma.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static valkeyContextFuncs valkeyContextRdmaFuncs;

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

#define VALKEY_RDMA_MAX_WQE 1024
#define VALKEY_RDMA_DEFAULT_RX_LEN (1024 * 1024)
#define VALKEY_RDMA_INVALID_OPCODE 0xffff

typedef struct RdmaContext {
    struct rdma_cm_id *cm_id;
    struct rdma_event_channel *cm_channel;
    struct ibv_comp_channel *comp_channel;
    struct ibv_cq *cq;
    struct ibv_pd *pd;

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

static int valkeyRdmaCM(valkeyContext *c, long timeout);

static int valkeyRdmaSetFdBlocking(valkeyContext *c, int fd, int blocking) {
    int flags;

    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        valkeySetError(c, VALKEY_ERR_IO, "fcntl(F_GETFL)");
        return VALKEY_ERR;
    }

    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;

    if (fcntl(fd, F_SETFL, flags) == -1) {
        valkeySetError(c, VALKEY_ERR_IO, "fcntl(F_SETFL)");
        return VALKEY_ERR;
    }

    return 0;
}

static int rdmaPostRecv(RdmaContext *ctx, struct rdma_cm_id *cm_id, valkeyRdmaCmd *cmd) {
    struct ibv_sge sge;
    size_t length = sizeof(valkeyRdmaCmd);
    struct ibv_recv_wr recv_wr, *bad_wr;

    sge.addr = (uint64_t)(uintptr_t)cmd;
    sge.length = length;
    sge.lkey = ctx->cmd_mr->lkey;

    recv_wr.wr_id = (uint64_t)cmd;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;
    recv_wr.next = NULL;

    if (ibv_post_recv(cm_id->qp, &recv_wr, &bad_wr)) {
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

static void rdmaDestroyIoBuf(RdmaContext *ctx) {
    if (ctx->recv_mr) {
        ibv_dereg_mr(ctx->recv_mr);
        ctx->recv_mr = NULL;
    }

    vk_free(ctx->recv_buf);
    ctx->recv_buf = NULL;

    if (ctx->send_mr) {
        ibv_dereg_mr(ctx->send_mr);
        ctx->send_mr = NULL;
    }

    vk_free(ctx->send_buf);
    ctx->send_buf = NULL;

    if (ctx->cmd_mr) {
        ibv_dereg_mr(ctx->cmd_mr);
        ctx->cmd_mr = NULL;
    }

    vk_free(ctx->cmd_buf);
    ctx->cmd_buf = NULL;
}

static int rdmaSetupIoBuf(valkeyContext *c, RdmaContext *ctx, struct rdma_cm_id *cm_id) {
    int access = IBV_ACCESS_LOCAL_WRITE;
    size_t length = sizeof(valkeyRdmaCmd) * VALKEY_RDMA_MAX_WQE * 2;
    valkeyRdmaCmd *cmd;
    int i;

    /* setup CMD buf & MR */
    ctx->cmd_buf = vk_calloc(length, 1);
    ctx->cmd_mr = ibv_reg_mr(ctx->pd, ctx->cmd_buf, length, access);
    if (!ctx->cmd_mr) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: reg recv mr failed");
        goto destroy_iobuf;
    }

    for (i = 0; i < VALKEY_RDMA_MAX_WQE; i++) {
        cmd = ctx->cmd_buf + i;

        if (rdmaPostRecv(ctx, cm_id, cmd) == VALKEY_ERR) {
            valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: post recv failed");
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
    ctx->recv_buf = vk_calloc(length, 1);
    ctx->recv_length = length;
    ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, length, access);
    if (!ctx->recv_mr) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: reg send mr failed");
        goto destroy_iobuf;
    }

    return VALKEY_OK;

destroy_iobuf:
    rdmaDestroyIoBuf(ctx);
    return VALKEY_ERR;
}

static int rdmaAdjustSendbuf(valkeyContext *c, RdmaContext *ctx, unsigned int length) {
    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    if (length == ctx->send_length) {
        return VALKEY_OK;
    }

    /* try to free old MR & buffer */
    if (ctx->send_length) {
        ibv_dereg_mr(ctx->send_mr);
        vk_free(ctx->send_buf);
        ctx->send_length = 0;
    }

    /* create a new buffer & MR */
    ctx->send_buf = vk_calloc(length, 1);
    ctx->send_length = length;
    ctx->send_mr = ibv_reg_mr(ctx->pd, ctx->send_buf, length, access);
    if (!ctx->send_mr) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: reg send buf mr failed");
        vk_free(ctx->send_buf);
        ctx->send_buf = NULL;
        ctx->send_length = 0;
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

static int rdmaSendCommand(valkeyContext *c, struct rdma_cm_id *cm_id, valkeyRdmaCmd *cmd) {
    RdmaContext *ctx = c->privctx;
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

    if (i >= 2 * VALKEY_RDMA_MAX_WQE) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: no empty command buffers");
        return VALKEY_ERR;
    }

    memcpy(_cmd, cmd, sizeof(valkeyRdmaCmd));
    sge.addr = (uint64_t)(uintptr_t)_cmd;
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
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: failed to send command buffers");
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

static int connRdmaRegisterRx(valkeyContext *c, struct rdma_cm_id *cm_id) {
    RdmaContext *ctx = c->privctx;
    valkeyRdmaCmd cmd = {0};

    cmd.memory.opcode = htons(RegisterXferMemory);
    cmd.memory.addr = htobe64((uint64_t)ctx->recv_buf);
    cmd.memory.length = htonl(ctx->recv_length);
    cmd.memory.key = htonl(ctx->recv_mr->rkey);

    ctx->rx_offset = 0;
    ctx->recv_offset = 0;

    return rdmaSendCommand(c, cm_id, &cmd);
}

static int connRdmaHandleRecv(valkeyContext *c, RdmaContext *ctx, struct rdma_cm_id *cm_id, valkeyRdmaCmd *cmd, uint32_t byte_len) {
    if (byte_len != sizeof(valkeyRdmaCmd)) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: FATAL error, recv corrupted cmd");
        return VALKEY_ERR;
    }

    switch (ntohs(cmd->keepalive.opcode)) {
    case RegisterXferMemory:
        ctx->tx_addr = (char *)be64toh(cmd->memory.addr);
        ctx->tx_length = ntohl(cmd->memory.length);
        ctx->tx_key = ntohl(cmd->memory.key);
        ctx->tx_offset = 0;
        rdmaAdjustSendbuf(c, ctx, ctx->tx_length);
        break;

    case Keepalive:
        break;

    default:
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: FATAL error, unknown cmd");
        return VALKEY_ERR;
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

    return VALKEY_OK;
}

static int connRdmaHandleWrite(VALKEY_UNUSED RdmaContext *ctx, uint32_t VALKEY_UNUSED byte_len) {

    return VALKEY_OK;
}

static int connRdmaHandleCq(valkeyContext *c) {
    RdmaContext *ctx = c->privctx;
    struct rdma_cm_id *cm_id = ctx->cm_id;
    struct ibv_cq *ev_cq = NULL;
    void *ev_ctx = NULL;
    struct ibv_wc wc = {0};
    valkeyRdmaCmd *cmd;
    int ret;

    if (ibv_get_cq_event(ctx->comp_channel, &ev_cq, &ev_ctx) < 0) {
        if (errno != EAGAIN) {
            valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: get cq event failed");
            return VALKEY_ERR;
        }
    } else if (ibv_req_notify_cq(ev_cq, 0)) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: notify cq failed");
        return VALKEY_ERR;
    }

pollcq:
    ret = ibv_poll_cq(ctx->cq, 1, &wc);
    if (ret < 0) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: poll cq failed");
        return VALKEY_ERR;
    } else if (ret == 0) {
        return VALKEY_OK;
    }

    ibv_ack_cq_events(ctx->cq, 1);

    if (wc.status != IBV_WC_SUCCESS) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: send/recv failed");
        return VALKEY_ERR;
    }

    switch (wc.opcode) {
    case IBV_WC_RECV:
        cmd = (valkeyRdmaCmd *)(uintptr_t)wc.wr_id;
        if (connRdmaHandleRecv(c, ctx, cm_id, cmd, wc.byte_len) == VALKEY_ERR) {
            return VALKEY_ERR;
        }

        break;

    case IBV_WC_RECV_RDMA_WITH_IMM:
        cmd = (valkeyRdmaCmd *)(uintptr_t)wc.wr_id;
        if (connRdmaHandleRecvImm(ctx, cm_id, cmd, ntohl(wc.imm_data)) == VALKEY_ERR) {
            return VALKEY_ERR;
        }

        break;
    case IBV_WC_RDMA_WRITE:
        if (connRdmaHandleWrite(ctx, wc.byte_len) == VALKEY_ERR) {
            return VALKEY_ERR;
        }

        break;
    case IBV_WC_SEND:
        cmd = (valkeyRdmaCmd *)(uintptr_t)wc.wr_id;
        if (connRdmaHandleSend(cmd) == VALKEY_ERR) {
            return VALKEY_ERR;
        }

        break;
    default:
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: unexpected opcode");
        return VALKEY_ERR;
    }

    goto pollcq;

    return VALKEY_OK;
}

/* There are two FD(s) in use:
 * - fd of CM channel: handle CM event. Return error on Disconnected.
 * - fd of completion channel: handle CQ event.
 * Return OK on CQ event ready, then CQ event should be handled outside.
 */
static int valkeyRdmaPollCqCm(valkeyContext *c, long timed) {
#define VALKEY_RDMA_POLLFD_CM 0
#define VALKEY_RDMA_POLLFD_CQ 1
#define VALKEY_RDMA_POLLFD_MAX 2
    struct pollfd pfd[VALKEY_RDMA_POLLFD_MAX];
    RdmaContext *ctx = c->privctx;
    long now = vk_msec_now();
    int ret;

    if (now >= timed) {
        valkeySetError(c, VALKEY_ERR_IO, "RDMA: IO timeout");
        return VALKEY_ERR;
    }

    /* pfd[0] for CM event */
    pfd[VALKEY_RDMA_POLLFD_CM].fd = ctx->cm_channel->fd;
    pfd[VALKEY_RDMA_POLLFD_CM].events = POLLIN;
    pfd[VALKEY_RDMA_POLLFD_CM].revents = 0;

    /* pfd[1] for CQ event */
    pfd[VALKEY_RDMA_POLLFD_CQ].fd = ctx->comp_channel->fd;
    pfd[VALKEY_RDMA_POLLFD_CQ].events = POLLIN;
    pfd[VALKEY_RDMA_POLLFD_CQ].revents = 0;
    ret = poll(pfd, VALKEY_RDMA_POLLFD_MAX, timed - now);
    if (ret < 0) {
        valkeySetError(c, VALKEY_ERR_IO, "RDMA: Poll CQ/CM failed");
        return VALKEY_ERR;
    } else if (ret == 0) {
        valkeySetError(c, VALKEY_ERR_IO, "Resource temporarily unavailable");
        return VALKEY_ERR;
    }

    if (pfd[VALKEY_RDMA_POLLFD_CM].revents & POLLIN) {
        valkeyRdmaCM(c, 0);
        if (!(c->flags & VALKEY_CONNECTED)) {
            valkeySetError(c, VALKEY_ERR_EOF, "Server closed the connection");
            return VALKEY_ERR;
        }
    }

    return VALKEY_OK;
}

static ssize_t valkeyRdmaRead(valkeyContext *c, char *buf, size_t bufcap) {
    RdmaContext *ctx = c->privctx;
    struct rdma_cm_id *cm_id = ctx->cm_id;
    long timed, end;
    uint32_t toread, remained;

    if (valkeyCommandTimeoutMsec(c, &timed)) {
        return VALKEY_ERR;
    }

    end = vk_msec_now() + timed;

pollcq:
    /* try to poll a CQ first */
    if (connRdmaHandleCq(c) == VALKEY_ERR) {
        return VALKEY_ERR;
    }

    if (ctx->recv_offset < ctx->rx_offset) {
        remained = ctx->rx_offset - ctx->recv_offset;
        toread = valkeyMin(remained, bufcap);

        memcpy(buf, ctx->recv_buf + ctx->recv_offset, toread);
        ctx->recv_offset += toread;

        if (ctx->recv_offset == ctx->recv_length) {
            connRdmaRegisterRx(c, cm_id);
        }

        return toread;
    }

    if (valkeyRdmaPollCqCm(c, end) == VALKEY_OK) {
        goto pollcq;
    } else {
        return VALKEY_ERR;
    }
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

    sge.addr = (uint64_t)(uintptr_t)addr;
    sge.lkey = ctx->send_mr->lkey;
    sge.length = data_len;

    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    send_wr.send_flags = (++ctx->send_ops % VALKEY_RDMA_MAX_WQE) ? 0 : IBV_SEND_SIGNALED;
    send_wr.imm_data = htonl(data_len);
    send_wr.wr.rdma.remote_addr = (uint64_t)(uintptr_t)remote_addr;
    send_wr.wr.rdma.rkey = ctx->tx_key;
    send_wr.next = NULL;
    ret = ibv_post_send(cm_id->qp, &send_wr, &bad_wr);
    if (ret) {
        return VALKEY_ERR;
    }

    ctx->tx_offset += data_len;

    return data_len;
}

static ssize_t valkeyRdmaWrite(valkeyContext *c) {
    RdmaContext *ctx = c->privctx;
    struct rdma_cm_id *cm_id = ctx->cm_id;
    size_t data_len = sdslen(c->obuf);
    long timed, end;
    uint32_t towrite, wrote = 0;
    size_t ret;

    if (valkeyCommandTimeoutMsec(c, &timed)) {
        return VALKEY_ERR;
    }

    end = vk_msec_now() + timed;

pollcq:
    if (connRdmaHandleCq(c) == VALKEY_ERR) {
        return VALKEY_ERR;
    }

    assert(ctx->tx_offset <= ctx->tx_length);
    if (ctx->tx_offset == ctx->tx_length) {
        /* wait a new TX buffer */
        goto waitcq;
    }

    towrite = valkeyMin(ctx->tx_length - ctx->tx_offset, data_len - wrote);
    ret = connRdmaSend(ctx, cm_id, c->obuf + wrote, towrite);
    if (ret == (size_t)VALKEY_ERR) {
        return VALKEY_ERR;
    }

    wrote += ret;
    if (wrote == data_len) {
        return data_len;
    }

waitcq:
    if (valkeyRdmaPollCqCm(c, end) == VALKEY_OK) {
        goto pollcq;
    } else {
        return VALKEY_ERR;
    }
}

/* RDMA has no POLLOUT event supported, so it could't work well with valkey async mechanism */
static void valkeyRdmaAsyncRead(VALKEY_UNUSED valkeyAsyncContext *ac) {
    assert("valkey async mechanism can't work with RDMA" == NULL);
}

static void valkeyRdmaAsyncWrite(VALKEY_UNUSED valkeyAsyncContext *ac) {
    assert("valkey async mechanism can't work with RDMA" == NULL);
}

static void valkeyRdmaClose(valkeyContext *c) {
    RdmaContext *ctx = c->privctx;
    struct rdma_cm_id *cm_id;

    if (!ctx) {
        return; /* connect failed? */
    }

    cm_id = ctx->cm_id;
    connRdmaHandleCq(c);
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

    vk_free(privctx);
}

static int valkeyRdmaConnect(valkeyContext *c, struct rdma_cm_id *cm_id) {
    RdmaContext *ctx = c->privctx;
    struct ibv_comp_channel *comp_channel = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_qp_init_attr init_attr = {0};
    struct rdma_conn_param conn_param = {0};

    pd = ibv_alloc_pd(cm_id->verbs);
    if (!pd) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: alloc pd failed");
        goto error;
    }

    comp_channel = ibv_create_comp_channel(cm_id->verbs);
    if (!comp_channel) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: alloc comp channel failed");
        goto error;
    }

    if (valkeyRdmaSetFdBlocking(c, comp_channel->fd, 0) != VALKEY_OK) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: set recv comp channel fd non-block failed");
        goto error;
    }

    cq = ibv_create_cq(cm_id->verbs, VALKEY_RDMA_MAX_WQE * 2, ctx, comp_channel, 0);
    if (!cq) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: create send cq failed");
        goto error;
    }

    if (ibv_req_notify_cq(cq, 0)) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: notify send cq failed");
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
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: create qp failed");
        goto error;
    }

    ctx->cm_id = cm_id;
    ctx->comp_channel = comp_channel;
    ctx->cq = cq;
    ctx->pd = pd;

    if (rdmaSetupIoBuf(c, ctx, cm_id) != VALKEY_OK)
        goto free_qp;

    /* rdma connect with param */
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 7;
    conn_param.rnr_retry_count = 7;
    if (rdma_connect(cm_id, &conn_param)) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: connect failed");
        goto destroy_iobuf;
    }

    return VALKEY_OK;

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

    return VALKEY_ERR;
}

static int valkeyRdmaEstablished(valkeyContext *c, struct rdma_cm_id *cm_id) {
    RdmaContext *ctx = c->privctx;

    /* it's time to tell redis we have already connected */
    c->flags |= VALKEY_CONNECTED;
    c->funcs = &valkeyContextRdmaFuncs;
    c->fd = ctx->comp_channel->fd;

    return connRdmaRegisterRx(c, cm_id);
}

static int valkeyRdmaCM(valkeyContext *c, long timeout) {
    RdmaContext *ctx = c->privctx;
    struct rdma_cm_event *event;
    char errorstr[128];
    int ret = VALKEY_ERR;

    while (rdma_get_cm_event(ctx->cm_channel, &event) == 0) {
        switch (event->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
            if (timeout < 0 || timeout > 100)
                timeout = 100; /* at most 100ms to resolve route */
            ret = rdma_resolve_route(event->id, timeout);
            if (ret) {
                valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: route resolve failed on");
            }
            break;
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
            ret = valkeyRdmaConnect(c, event->id);
            break;
        case RDMA_CM_EVENT_ESTABLISHED:
            ret = valkeyRdmaEstablished(c, event->id);
            break;
        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
            ret = VALKEY_ERR;
            valkeySetError(c, VALKEY_ERR_TIMEOUT, "RDMA: connect timeout");
            break;
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
        case RDMA_CM_EVENT_DISCONNECTED:
            c->flags &= ~VALKEY_CONNECTED;
            break;
        case RDMA_CM_EVENT_ADDR_CHANGE:
        default:
            snprintf(errorstr, sizeof(errorstr), "RDMA: connect failed - %s", rdma_event_str(event->event));
            valkeySetError(c, VALKEY_ERR_OTHER, errorstr);
            ret = VALKEY_ERR;
            break;
        }

        rdma_ack_cm_event(event);
    }

    return ret;
}

static int valkeyRdmaWaitConn(valkeyContext *c, long timeout) {
    struct pollfd pfd;
    long now, end;
    RdmaContext *ctx = c->privctx;

    assert(timeout >= 0);
    end = vk_msec_now() + timeout;

    while (1) {
        now = vk_msec_now();
        if (now >= end) {
            break;
        }

        pfd.fd = ctx->cm_channel->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, end - now) < 0) {
            return VALKEY_ERR;
        }

        if (valkeyRdmaCM(c, end - now) == VALKEY_ERR) {
            return VALKEY_ERR;
        }

        if (c->flags & VALKEY_CONNECTED) {
            return VALKEY_OK;
        }
    }

    return VALKEY_ERR;
}

static int valkeyContextConnectRdma(valkeyContext *c, const valkeyOptions *options) {
    const struct timeval *timeout = options->connect_timeout;
    const char *addr = options->endpoint.tcp.ip;
    const char *source_addr = options->endpoint.tcp.source_addr;
    int port = options->endpoint.tcp.port;
    int ret;
    char _port[6]; /* strlen("65535"); */
    struct rdma_addrinfo hints = {0}, *addrinfo = NULL;
    long timeout_msec = -1;
    RdmaContext *ctx = NULL;
    long start = vk_msec_now(), timed;

    c->connection_type = VALKEY_CONN_RDMA;
    c->tcp.port = port;
    c->flags &= ~VALKEY_CONNECTED;

    if (port < 0 || port > UINT16_MAX) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: Port number must be between 0-65535");
        return VALKEY_ERR;
    }

    if (c->tcp.host != addr) {
        vk_free(c->tcp.host);

        c->tcp.host = vk_strdup(addr);
        if (c->tcp.host == NULL) {
            valkeySetError(c, VALKEY_ERR_OOM, "RDMA: Out of memory");
            return VALKEY_ERR;
        }
    }

    if (source_addr == NULL) {
        vk_free(c->tcp.source_addr);
        c->tcp.source_addr = NULL;
    } else if (c->tcp.source_addr != source_addr) {
        vk_free(c->tcp.source_addr);
        c->tcp.source_addr = vk_strdup(source_addr);
        if (c->tcp.source_addr == NULL) {
            valkeySetError(c, VALKEY_ERR_OOM, "RDMA: Out of memory");
            return VALKEY_ERR;
        }
    }

    if (timeout) {
        if (valkeyContextUpdateConnectTimeout(c, timeout) == VALKEY_ERR) {
            return VALKEY_ERR;
        }
    } else {
        vk_free(c->connect_timeout);
        c->connect_timeout = NULL;
    }

    if (valkeyConnectTimeoutMsec(c, &timeout_msec) != VALKEY_OK) {
        return VALKEY_ERR;
    } else if (timeout_msec == -1) {
        timeout_msec = INT_MAX;
    }

    ctx = vk_calloc(sizeof(RdmaContext), 1);
    if (!ctx) {
        valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    c->privctx = ctx;

    ctx->cm_channel = rdma_create_event_channel();
    if (!ctx->cm_channel) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: create event channel failed");
        goto error;
    }

    if (rdma_create_id(ctx->cm_channel, &ctx->cm_id, (void *)ctx, RDMA_PS_TCP)) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: create id failed");
        goto error;
    }

    if ((valkeyRdmaSetFdBlocking(c, ctx->cm_channel->fd, 0) != VALKEY_OK)) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: set cm channel fd non-block failed");
        goto error;
    }

    if (c->tcp.source_addr) {
        /* bind local address with a random port if user specify one. */
        snprintf(_port, sizeof(_port), "%d", 0);
        hints.ai_flags = RAI_PASSIVE;
        hints.ai_port_space = RDMA_PS_TCP;
        if (rdma_getaddrinfo(addr, _port, &hints, &addrinfo)) {
            valkeySetError(c, VALKEY_ERR_PROTOCOL, "RDMA: failed to getaddrinfo for local side");
            goto error;
        }

        if (rdma_bind_addr(ctx->cm_id, addrinfo->ai_src_addr)) {
            valkeySetError(c, VALKEY_ERR_PROTOCOL, "RDMA: failed to bind local address");
            goto error;
        }

        memset(&hints, 0x00, sizeof(hints));
        rdma_freeaddrinfo(addrinfo);
        addrinfo = NULL;
    }

    /* resolve remote address & port by RDMA style */
    snprintf(_port, sizeof(_port), "%d", port);
    hints.ai_port_space = RDMA_PS_TCP;
    if (rdma_getaddrinfo(addr, _port, &hints, &addrinfo)) {
        valkeySetError(c, VALKEY_ERR_PROTOCOL, "RDMA: failed to getaddrinfo");
        goto error;
    }

    timed = timeout_msec - (vk_msec_now() - start);
    if (rdma_resolve_addr(ctx->cm_id, NULL, (struct sockaddr *)addrinfo->ai_dst_addr, timed)) {
        valkeySetError(c, VALKEY_ERR_OTHER, "RDMA: failed to resolve");
        goto error;
    }

    timed = vk_msec_now() - start;
    if (timed >= timeout_msec) {
        valkeySetError(c, VALKEY_ERR_TIMEOUT, "RDMA: resolving timeout");
        goto error;
    }

    if ((valkeyRdmaWaitConn(c, timeout_msec - timed) == VALKEY_OK) && (c->flags & VALKEY_CONNECTED)) {
        ret = VALKEY_OK;
        goto end;
    }

error:
    ret = VALKEY_ERR;
    if (ctx) {
        if (ctx->cm_id) {
            rdma_destroy_id(ctx->cm_id);
        }
        if (ctx->cm_channel) {
            rdma_destroy_event_channel(ctx->cm_channel);
        }

        vk_free(ctx);
        c->privctx = NULL;
    }

end:
    if (addrinfo) {
        rdma_freeaddrinfo(addrinfo);
    }

    return ret;
}

/* tv has already been updated into @c successfully, do nothing here */
static int valkeyRdmaSetTimeout(VALKEY_UNUSED valkeyContext *c, VALKEY_UNUSED const struct timeval tv) {
    return VALKEY_OK;
}

static valkeyContextFuncs valkeyContextRdmaFuncs = {
    .connect = valkeyContextConnectRdma,
    .close = valkeyRdmaClose,
    .free_privctx = valkeyRdmaFree,
    .async_read = valkeyRdmaAsyncRead,
    .async_write = valkeyRdmaAsyncWrite,
    .read = valkeyRdmaRead,
    .write = valkeyRdmaWrite,
    .set_timeout = valkeyRdmaSetTimeout};

int valkeyInitiateRdma(void) {
    valkeyContextRegisterFuncs(&valkeyContextRdmaFuncs, VALKEY_CONN_RDMA);

    return VALKEY_OK;
}

#else /* __linux__ */

#error "BUILD ERROR: RDMA is only supported on linux"

#endif /* __linux__ */
