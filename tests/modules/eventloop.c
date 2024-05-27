/* This module contains four tests :
 * 1- test.sanity    : Basic tests for argument validation mostly.
 * 2- test.sendbytes : Creates a pipe and registers its fds to the event loop,
 *                     one end of the pipe for read events and the other end for
 *                     the write events. On writable event, data is written. On
 *                     readable event data is read. Repeated until all data is
 *                     received.
 * 3- test.iteration : A test for BEFORE_SLEEP and AFTER_SLEEP callbacks.
 *                     Counters are incremented each time these events are
 *                     fired. They should be equal and increment monotonically.
 * 4- test.oneshot   : Test for oneshot API
 */

#include "valkeymodule.h"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <errno.h>

int fds[2];
long long buf_size;
char *src;
long long src_offset;
char *dst;
long long dst_offset;

ValkeyModuleBlockedClient *bc;
ValkeyModuleCtx *reply_ctx;

void onReadable(int fd, void *user_data, int mask) {
    VALKEYMODULE_NOT_USED(mask);

    ValkeyModule_Assert(strcmp(user_data, "userdataread") == 0);

    while (1) {
        int rd = read(fd, dst + dst_offset, buf_size - dst_offset);
        if (rd <= 0)
            return;
        dst_offset += rd;

        /* Received all bytes */
        if (dst_offset == buf_size) {
            if (memcmp(src, dst, buf_size) == 0)
                ValkeyModule_ReplyWithSimpleString(reply_ctx, "OK");
            else
                ValkeyModule_ReplyWithError(reply_ctx, "ERR bytes mismatch");

            ValkeyModule_EventLoopDel(fds[0], VALKEYMODULE_EVENTLOOP_READABLE);
            ValkeyModule_EventLoopDel(fds[1], VALKEYMODULE_EVENTLOOP_WRITABLE);
            ValkeyModule_Free(src);
            ValkeyModule_Free(dst);
            close(fds[0]);
            close(fds[1]);

            ValkeyModule_FreeThreadSafeContext(reply_ctx);
            ValkeyModule_UnblockClient(bc, NULL);
            return;
        }
    };
}

void onWritable(int fd, void *user_data, int mask) {
    VALKEYMODULE_NOT_USED(user_data);
    VALKEYMODULE_NOT_USED(mask);

    ValkeyModule_Assert(strcmp(user_data, "userdatawrite") == 0);

    while (1) {
        /* Check if we sent all data */
        if (src_offset >= buf_size)
            return;
        int written = write(fd, src + src_offset, buf_size - src_offset);
        if (written <= 0) {
            return;
        }

        src_offset += written;
    };
}

/* Create a pipe(), register pipe fds to the event loop and send/receive data
 * using them. */
int sendbytes(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    if (ValkeyModule_StringToLongLong(argv[1], &buf_size) != VALKEYMODULE_OK ||
        buf_size == 0) {
        ValkeyModule_ReplyWithError(ctx, "Invalid integer value");
        return VALKEYMODULE_OK;
    }

    bc = ValkeyModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    reply_ctx = ValkeyModule_GetThreadSafeContext(bc);

    /* Allocate source buffer and write some random data */
    src = ValkeyModule_Calloc(1,buf_size);
    src_offset = 0;
    memset(src, rand() % 0xFF, buf_size);
    memcpy(src, "randomtestdata", strlen("randomtestdata"));

    dst = ValkeyModule_Calloc(1,buf_size);
    dst_offset = 0;

    /* Create a pipe and register it to the event loop. */
    if (pipe(fds) < 0) return VALKEYMODULE_ERR;
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) return VALKEYMODULE_ERR;
    if (fcntl(fds[1], F_SETFL, O_NONBLOCK) < 0) return VALKEYMODULE_ERR;

    if (ValkeyModule_EventLoopAdd(fds[0], VALKEYMODULE_EVENTLOOP_READABLE,
        onReadable, "userdataread") != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
    if (ValkeyModule_EventLoopAdd(fds[1], VALKEYMODULE_EVENTLOOP_WRITABLE,
        onWritable, "userdatawrite") != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
    return VALKEYMODULE_OK;
}

int sanity(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (pipe(fds) < 0) return VALKEYMODULE_ERR;

    if (ValkeyModule_EventLoopAdd(fds[0], 9999999, onReadable, NULL)
        == VALKEYMODULE_OK || errno != EINVAL) {
        ValkeyModule_ReplyWithError(ctx, "ERR non-existing event type should fail");
        goto out;
    }
    if (ValkeyModule_EventLoopAdd(-1, VALKEYMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        == VALKEYMODULE_OK || errno != ERANGE) {
        ValkeyModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (ValkeyModule_EventLoopAdd(99999999, VALKEYMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        == VALKEYMODULE_OK || errno != ERANGE) {
        ValkeyModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (ValkeyModule_EventLoopAdd(fds[0], VALKEYMODULE_EVENTLOOP_READABLE, NULL, NULL)
        == VALKEYMODULE_OK || errno != EINVAL) {
        ValkeyModule_ReplyWithError(ctx, "ERR null callback should fail");
        goto out;
    }
    if (ValkeyModule_EventLoopAdd(fds[0], 9999999, onReadable, NULL)
        == VALKEYMODULE_OK || errno != EINVAL) {
        ValkeyModule_ReplyWithError(ctx, "ERR non-existing event type should fail");
        goto out;
    }
    if (ValkeyModule_EventLoopDel(fds[0], VALKEYMODULE_EVENTLOOP_READABLE)
        != VALKEYMODULE_OK || errno != 0) {
        ValkeyModule_ReplyWithError(ctx, "ERR del on non-registered fd should not fail");
        goto out;
    }
    if (ValkeyModule_EventLoopDel(fds[0], 9999999) == VALKEYMODULE_OK ||
        errno != EINVAL) {
        ValkeyModule_ReplyWithError(ctx, "ERR non-existing event type should fail");
        goto out;
    }
    if (ValkeyModule_EventLoopDel(-1, VALKEYMODULE_EVENTLOOP_READABLE)
        == VALKEYMODULE_OK || errno != ERANGE) {
        ValkeyModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (ValkeyModule_EventLoopDel(99999999, VALKEYMODULE_EVENTLOOP_READABLE)
        == VALKEYMODULE_OK || errno != ERANGE) {
        ValkeyModule_ReplyWithError(ctx, "ERR out of range fd should fail");
        goto out;
    }
    if (ValkeyModule_EventLoopAdd(fds[0], VALKEYMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        != VALKEYMODULE_OK || errno != 0) {
        ValkeyModule_ReplyWithError(ctx, "ERR Add failed");
        goto out;
    }
    if (ValkeyModule_EventLoopAdd(fds[0], VALKEYMODULE_EVENTLOOP_READABLE, onReadable, NULL)
        != VALKEYMODULE_OK || errno != 0) {
        ValkeyModule_ReplyWithError(ctx, "ERR Adding same fd twice failed");
        goto out;
    }
    if (ValkeyModule_EventLoopDel(fds[0], VALKEYMODULE_EVENTLOOP_READABLE)
        != VALKEYMODULE_OK || errno != 0) {
        ValkeyModule_ReplyWithError(ctx, "ERR Del failed");
        goto out;
    }
    if (ValkeyModule_EventLoopAddOneShot(NULL, NULL) == VALKEYMODULE_OK || errno != EINVAL) {
        ValkeyModule_ReplyWithError(ctx, "ERR null callback should fail");
        goto out;
    }

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
out:
    close(fds[0]);
    close(fds[1]);
    return VALKEYMODULE_OK;
}

static long long beforeSleepCount;
static long long afterSleepCount;

int iteration(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    /* On each event loop iteration, eventloopCallback() is called. We increment
     * beforeSleepCount and afterSleepCount, so these two should be equal.
     * We reply with iteration count, caller can test if iteration count
     * increments monotonically */
    ValkeyModule_Assert(beforeSleepCount == afterSleepCount);
    ValkeyModule_ReplyWithLongLong(ctx, beforeSleepCount);
    return VALKEYMODULE_OK;
}

void oneshotCallback(void* arg)
{
    ValkeyModule_Assert(strcmp(arg, "userdata") == 0);
    ValkeyModule_ReplyWithSimpleString(reply_ctx, "OK");
    ValkeyModule_FreeThreadSafeContext(reply_ctx);
    ValkeyModule_UnblockClient(bc, NULL);
}

int oneshot(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    bc = ValkeyModule_BlockClient(ctx, NULL, NULL, NULL, 0);
    reply_ctx = ValkeyModule_GetThreadSafeContext(bc);

    if (ValkeyModule_EventLoopAddOneShot(oneshotCallback, "userdata") != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "ERR oneshot failed");
        ValkeyModule_FreeThreadSafeContext(reply_ctx);
        ValkeyModule_UnblockClient(bc, NULL);
    }
    return VALKEYMODULE_OK;
}

void eventloopCallback(struct ValkeyModuleCtx *ctx, ValkeyModuleEvent eid, uint64_t subevent, void *data) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(eid);
    VALKEYMODULE_NOT_USED(subevent);
    VALKEYMODULE_NOT_USED(data);

    ValkeyModule_Assert(eid.id == VALKEYMODULE_EVENT_EVENTLOOP);
    if (subevent == VALKEYMODULE_SUBEVENT_EVENTLOOP_BEFORE_SLEEP)
        beforeSleepCount++;
    else if (subevent == VALKEYMODULE_SUBEVENT_EVENTLOOP_AFTER_SLEEP)
        afterSleepCount++;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx,"eventloop",1,VALKEYMODULE_APIVER_1)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    /* Test basics. */
    if (ValkeyModule_CreateCommand(ctx, "test.sanity", sanity, "", 0, 0, 0)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    /* Register a command to create a pipe() and send data through it by using
     * event loop API. */
    if (ValkeyModule_CreateCommand(ctx, "test.sendbytes", sendbytes, "", 0, 0, 0)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    /* Register a command to return event loop iteration count. */
    if (ValkeyModule_CreateCommand(ctx, "test.iteration", iteration, "", 0, 0, 0)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.oneshot", oneshot, "", 0, 0, 0)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    if (ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_EventLoop,
        eventloopCallback) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
