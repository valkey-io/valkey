#include "valkeymodule.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory.h>
#include <errno.h>

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

    return VALKEYMODULE_OK;
}
