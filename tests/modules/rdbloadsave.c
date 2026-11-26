#include "valkeymodule.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <errno.h>
#include <stdio.h>
#include <strings.h>

/* Sanity tests to verify inputs and return values. */
int sanity(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    ValkeyModuleRdbStream *s = ValkeyModule_RdbStreamCreateFromFile("dbnew.rdb");

    /* NULL stream should fail. */
    if (ValkeyModule_RdbLoad(ctx, NULL, 0) == VALKEYMODULE_OK || errno != EINVAL) {
        ValkeyModule_ReplyWithError(ctx, strerror(errno));
        goto out;
    }

    /* Invalid flags should fail. */
    if (ValkeyModule_RdbLoad(ctx, s, 188) == VALKEYMODULE_OK || errno != EINVAL) {
        ValkeyModule_ReplyWithError(ctx, strerror(errno));
        goto out;
    }

    /* Missing file should fail. */
    if (ValkeyModule_RdbLoad(ctx, s, 0) == VALKEYMODULE_OK || errno != ENOENT) {
        ValkeyModule_ReplyWithError(ctx, strerror(errno));
        goto out;
    }

    /* Save RDB file. */
    if (ValkeyModule_RdbSave(ctx, s, 0) != VALKEYMODULE_OK || errno != 0) {
        ValkeyModule_ReplyWithError(ctx, strerror(errno));
        goto out;
    }

    /* Load the saved RDB file. */
    if (ValkeyModule_RdbLoad(ctx, s, 0) != VALKEYMODULE_OK || errno != 0) {
        ValkeyModule_ReplyWithError(ctx, strerror(errno));
        goto out;
    }

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");

 out:
    ValkeyModule_RdbStreamFree(s);
    return VALKEYMODULE_OK;
}

int cmd_rdbsave(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    size_t len;
    const char *filename = ValkeyModule_StringPtrLen(argv[1], &len);

    char tmp[len + 1];
    memcpy(tmp, filename, len);
    tmp[len] = '\0';

    ValkeyModuleRdbStream *stream = ValkeyModule_RdbStreamCreateFromFile(tmp);

    if (ValkeyModule_RdbSave(ctx, stream, 0) != VALKEYMODULE_OK || errno != 0) {
        ValkeyModule_ReplyWithError(ctx, strerror(errno));
        goto out;
    }

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");

out:
    ValkeyModule_RdbStreamFree(stream);
    return VALKEYMODULE_OK;
}

/* Fork before calling RM_RdbSave(). */
int cmd_rdbsave_fork(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    size_t len;
    const char *filename = ValkeyModule_StringPtrLen(argv[1], &len);

    char tmp[len + 1];
    memcpy(tmp, filename, len);
    tmp[len] = '\0';

    int fork_child_pid = ValkeyModule_Fork(NULL, NULL);
    if (fork_child_pid < 0) {
        ValkeyModule_ReplyWithError(ctx, strerror(errno));
        return VALKEYMODULE_OK;
    } else if (fork_child_pid > 0) {
        /* parent */
        ValkeyModule_ReplyWithSimpleString(ctx, "OK");
        return VALKEYMODULE_OK;
    }

    ValkeyModuleRdbStream *stream = ValkeyModule_RdbStreamCreateFromFile(tmp);

    int ret = 0;
    if (ValkeyModule_RdbSave(ctx, stream, 0) != VALKEYMODULE_OK) {
        ret = errno;
    }
    ValkeyModule_RdbStreamFree(stream);

    ValkeyModule_ExitFromChild(ret);
    return VALKEYMODULE_OK;
}

int cmd_rdbload(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    size_t len;
    const char *filename = ValkeyModule_StringPtrLen(argv[1], &len);

    char tmp[len + 1];
    memcpy(tmp, filename, len);
    tmp[len] = '\0';

    ValkeyModuleRdbStream *stream = ValkeyModule_RdbStreamCreateFromFile(tmp);

    if (ValkeyModule_RdbLoad(ctx, stream, 0) != VALKEYMODULE_OK || errno != 0) {
        ValkeyModule_RdbStreamFree(stream);
        ValkeyModule_ReplyWithError(ctx, strerror(errno));
        return VALKEYMODULE_OK;
    }

    ValkeyModule_RdbStreamFree(stream);
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

/* A file backed stream for VM_RdbStreamCreateFromCallbacks(). Saving writes
 * to a temp file that is renamed on commit, like an upload that only becomes
 * visible once completed. */
typedef struct {
    FILE *fp;
    char path[256];
    char tmppath[300];
    long long fail_write_after; /* -1 to disable */
    long long read_limit;       /* -1 to disable */
    long long nbytes;
    ValkeyModuleCtx *ctx;
    int fail_close, fail_flush, yield_write, nested, fork, timer, aof_on;
    int closed; /* The close callback was called. */
} cbStream;

static long long io_calls = 0;
static int last_commit = -1;
static char timer_result[128] = "none";
static cbStream timer_stream;
static char nested_result[128] = "none";

/* Tries to save and load from the callbacks of a save or a load. */
static void cbNested(cbStream *s) {
    ValkeyModuleRdbStream *stream = ValkeyModule_RdbStreamCreateFromFile("nested.rdb");
    int save = ValkeyModule_RdbSave(s->ctx, stream, 0);
    int save_errno = errno;
    ValkeyModule_RdbStreamFree(stream);
    stream = ValkeyModule_RdbStreamCreateFromFile("nested.rdb");
    int load = ValkeyModule_RdbLoad(s->ctx, stream, 0);
    int load_errno = errno;
    ValkeyModule_RdbStreamFree(stream);
    snprintf(nested_result, sizeof(nested_result), "%s %s", save == VALKEYMODULE_OK ? "OK" : strerror(save_errno),
             load == VALKEYMODULE_OK ? "OK" : strerror(load_errno));
}

static int fileExists(const char *path, const char *suffix) {
    char name[sizeof(((cbStream *)0)->path) + 16];
    snprintf(name, sizeof(name), "%s%s", path, suffix);
    return access(name, F_OK) == 0;
}

static int cbWrite(void *ctx, const void *buf, size_t len) {
    cbStream *s = ctx;
    io_calls++;
    if (s->fail_write_after >= 0 && s->nbytes + (long long)len > s->fail_write_after) return VALKEYMODULE_ERR;
    if (s->yield_write && s->nbytes == 0) {
        /* Try to process events, like a module waiting for a remote storage.
         * Tells the test it started with <path>.started, and waits for
         * <path>.go, so that the test can send a command meanwhile. */
        char started[sizeof(s->path) + 16];
        snprintf(started, sizeof(started), "%s.started", s->path);
        FILE *fp = fopen(started, "w");
        if (fp) fclose(fp);
        for (int i = 0; i < 10000 && !fileExists(s->path, ".go"); i++) {
            usleep(1000);
            ValkeyModule_Yield(s->ctx, VALKEYMODULE_YIELD_FLAG_CLIENTS, NULL);
        }
    }
    if (s->nested && s->nbytes == 0) cbNested(s);
    if (fwrite(buf, len, 1, s->fp) != 1) return VALKEYMODULE_ERR;
    s->nbytes += len;
    return VALKEYMODULE_OK;
}

static ssize_t cbRead(void *ctx, void *buf, size_t len) {
    cbStream *s = ctx;
    io_calls++;
    if (s->nested && s->nbytes == 0) cbNested(s);
    if (s->read_limit >= 0 && s->nbytes + (long long)len > s->read_limit) len = s->read_limit - s->nbytes;
    size_t nread = fread(buf, 1, len, s->fp);
    if (nread == 0 && ferror(s->fp)) return -1;
    s->nbytes += nread;
    return nread;
}

static int cbFlush(void *ctx) {
    cbStream *s = ctx;
    if (s->fail_flush) return VALKEYMODULE_ERR;
    return fflush(s->fp) == 0 ? VALKEYMODULE_OK : VALKEYMODULE_ERR;
}

static int cbClose(void *ctx, int commit) {
    cbStream *s = ctx;
    last_commit = commit;
    s->closed = 1;
    if (s->aof_on) {
        /* Enable the AOF once the load is done, before VM_RdbLoad() returns. */
        ValkeyModuleCallReply *reply = ValkeyModule_Call(s->ctx, "CONFIG", "ccc", "SET", "appendonly", "yes");
        if (reply) ValkeyModule_FreeCallReply(reply);
    }
    fclose(s->fp);
    if (s->tmppath[0] == '\0') return VALKEYMODULE_OK; /* Read stream. */
    if (!commit || s->fail_close) {
        unlink(s->tmppath);
        return commit ? VALKEYMODULE_ERR : VALKEYMODULE_OK;
    }
    return rename(s->tmppath, s->path) == 0 ? VALKEYMODULE_OK : VALKEYMODULE_ERR;
}

static const ValkeyModuleIOCallbacks write_callbacks = {
    .version = VALKEYMODULE_IO_CALLBACKS_VERSION,
    .write_func = cbWrite,
    .flush_func = cbFlush,
    .close_func = cbClose,
};

static const ValkeyModuleIOCallbacks read_callbacks = {
    .version = VALKEYMODULE_IO_CALLBACKS_VERSION,
    .read_func = cbRead,
    .close_func = cbClose,
};

/* Parses "<path> [failwrite <n>] [failflush] [failclose] [yieldwrite] [nested] [limit <n>] [fork]
 * [timer] [aofon]". */
static int cbStreamInit(ValkeyModuleCtx *ctx, cbStream *s, ValkeyModuleString **argv, int argc) {
    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    last_commit = -1; /* Set by the close callback of this stream. */
    snprintf(nested_result, sizeof(nested_result), "none");
    s->fail_write_after = -1;
    s->read_limit = -1;
    snprintf(s->path, sizeof(s->path), "%s", ValkeyModule_StringPtrLen(argv[1], NULL));

    for (int i = 2; i < argc; i++) {
        const char *opt = ValkeyModule_StringPtrLen(argv[i], NULL);
        long long n = 0;
        if (!strcasecmp(opt, "failclose")) {
            s->fail_close = 1;
        } else if (!strcasecmp(opt, "failflush")) {
            s->fail_flush = 1;
        } else if (!strcasecmp(opt, "yieldwrite")) {
            s->yield_write = 1;
        } else if (!strcasecmp(opt, "nested")) {
            s->nested = 1;
        } else if (!strcasecmp(opt, "fork")) {
            s->fork = 1;
        } else if (!strcasecmp(opt, "timer")) {
            s->timer = 1;
        } else if (!strcasecmp(opt, "aofon")) {
            s->aof_on = 1;
        } else if (i + 1 < argc && ValkeyModule_StringToLongLong(argv[i + 1], &n) == VALKEYMODULE_OK &&
                   (!strcasecmp(opt, "failwrite") || !strcasecmp(opt, "limit"))) {
            if (!strcasecmp(opt, "failwrite")) s->fail_write_after = n;
            else s->read_limit = n;
            i++;
        } else {
            ValkeyModule_ReplyWithError(ctx, "ERR syntax error");
            return VALKEYMODULE_ERR;
        }
    }
    return VALKEYMODULE_OK;
}

/* The close callback isn't called when the call fails before using the
 * stream, e.g. with EINVAL or EBUSY, so the file is released here instead. */
static void cbRelease(cbStream *s) {
    if (s->closed || !s->fp) return;
    int saved_errno = errno;
    fclose(s->fp);
    s->fp = NULL;
    if (s->tmppath[0]) unlink(s->tmppath);
    errno = saved_errno;
}

static int cbSave(ValkeyModuleCtx *ctx, cbStream *s) {
    snprintf(s->tmppath, sizeof(s->tmppath), "%s.tmp", s->path);
    if ((s->fp = fopen(s->tmppath, "w")) == NULL) return VALKEYMODULE_ERR;
    ValkeyModuleRdbStream *stream = ValkeyModule_RdbStreamCreateFromCallbacks(&write_callbacks, s);
    int ret = ValkeyModule_RdbSave(ctx, stream, 0);
    ValkeyModule_RdbStreamFree(stream);
    cbRelease(s);
    return ret;
}

static int cbLoad(ValkeyModuleCtx *ctx, cbStream *s) {
    if ((s->fp = fopen(s->path, "r")) == NULL) return VALKEYMODULE_ERR;
    ValkeyModuleRdbStream *stream = ValkeyModule_RdbStreamCreateFromCallbacks(&read_callbacks, s);
    int ret = ValkeyModule_RdbLoad(ctx, stream, 0);
    ValkeyModule_RdbStreamFree(stream);
    cbRelease(s);
    return ret;
}

/* Loading from a timer runs without a current client. */
static void cbLoadTimer(ValkeyModuleCtx *ctx, void *data) {
    VALKEYMODULE_NOT_USED(data);
    timer_stream.ctx = ctx; /* The ctx of the command that armed the timer is gone. */
    int ret = cbLoad(ctx, &timer_stream);
    snprintf(timer_result, sizeof(timer_result), "%s", ret == VALKEYMODULE_OK ? "OK" : strerror(errno));
}

/* Sanity tests for VM_RdbStreamCreateFromCallbacks(). */
int sanity_cb(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    ValkeyModuleIOCallbacks bad = read_callbacks;
    bad.version = VALKEYMODULE_IO_CALLBACKS_VERSION + 1;
    errno = 0;
    if (ValkeyModule_RdbStreamCreateFromCallbacks(NULL, NULL) || errno != EINVAL ||
        ValkeyModule_RdbStreamCreateFromCallbacks(&bad, NULL)) {
        return ValkeyModule_ReplyWithError(ctx, "ERR invalid callbacks accepted");
    }

    /* A stream without a write callback can't be saved to, and one without a
     * read callback can't be loaded from. */
    ValkeyModuleRdbStream *rs = ValkeyModule_RdbStreamCreateFromCallbacks(&read_callbacks, NULL);
    ValkeyModuleRdbStream *ws = ValkeyModule_RdbStreamCreateFromCallbacks(&write_callbacks, NULL);
    int ok = ValkeyModule_RdbSave(ctx, rs, 0) == VALKEYMODULE_ERR && errno == EINVAL &&
             ValkeyModule_RdbLoad(ctx, ws, 0) == VALKEYMODULE_ERR && errno == EINVAL;
    ValkeyModule_RdbStreamFree(rs);
    ValkeyModule_RdbStreamFree(ws);
    if (!ok) return ValkeyModule_ReplyWithError(ctx, "ERR missing callback accepted");
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* test.rdbsave_cb <path> [failwrite <n>] [failflush] [failclose] [yieldwrite] [nested] [fork] */
int cmd_rdbsave_cb(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 2) return ValkeyModule_WrongArity(ctx);
    cbStream s;
    if (cbStreamInit(ctx, &s, argv, argc) != VALKEYMODULE_OK) return VALKEYMODULE_OK;

    if (s.fork) {
        int pid = ValkeyModule_Fork(NULL, NULL);
        if (pid < 0) return ValkeyModule_ReplyWithError(ctx, strerror(errno));
        if (pid == 0) ValkeyModule_ExitFromChild(cbSave(ctx, &s) == VALKEYMODULE_OK ? 0 : 1);
        return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    }
    if (cbSave(ctx, &s) != VALKEYMODULE_OK) return ValkeyModule_ReplyWithError(ctx, strerror(errno));
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* test.rdbload_cb <path> [limit <n>] [timer] */
int cmd_rdbload_cb(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc < 2) return ValkeyModule_WrongArity(ctx);
    cbStream s;
    if (cbStreamInit(ctx, &s, argv, argc) != VALKEYMODULE_OK) return VALKEYMODULE_OK;

    if (s.timer) {
        timer_stream = s;
        snprintf(timer_result, sizeof(timer_result), "pending");
        ValkeyModule_CreateTimer(ctx, 1, cbLoadTimer, NULL);
        return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    }
    if (cbLoad(ctx, &s) != VALKEYMODULE_OK) return ValkeyModule_ReplyWithError(ctx, strerror(errno));
    return ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}

/* test.cb_stat io_calls|last_commit|nested_result|timer_result. Reading io_calls resets it. */
int cmd_cb_stat(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (argc != 2) return ValkeyModule_WrongArity(ctx);
    const char *name = ValkeyModule_StringPtrLen(argv[1], NULL);
    if (!strcasecmp(name, "io_calls")) {
        long long calls = io_calls;
        io_calls = 0;
        return ValkeyModule_ReplyWithLongLong(ctx, calls);
    }
    if (!strcasecmp(name, "last_commit")) return ValkeyModule_ReplyWithLongLong(ctx, last_commit);
    if (!strcasecmp(name, "nested_result")) return ValkeyModule_ReplyWithSimpleString(ctx, nested_result);
    return ValkeyModule_ReplyWithSimpleString(ctx, timer_result);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "rdbloadsave", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.sanity", sanity, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.rdbsave", cmd_rdbsave, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.rdbsave_fork", cmd_rdbsave_fork, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.rdbload", cmd_rdbload, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.sanity_cb", sanity_cb, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.rdbsave_cb", cmd_rdbsave_cb, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.rdbload_cb", cmd_rdbload_cb, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "test.cb_stat", cmd_cb_stat, "allow-loading", 0, 0, 0) ==
        VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
