
/* define macros for having usleep */
#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include "valkeymodule.h"
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define UNUSED(V) ((void) V)

int child_pid = -1;
int exited_with_code = -1;

void done_handler(int exitcode, int bysignal, void *user_data) {
    child_pid = -1;
    exited_with_code = exitcode;
    assert(user_data==(void*)0xdeadbeef);
    UNUSED(bysignal);
}

int fork_create(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    long long code_to_exit_with;
    long long usleep_us;
    if (argc != 3) {
        ValkeyModule_WrongArity(ctx);
        return VALKEYMODULE_OK;
    }

    if(!RMAPI_FUNC_SUPPORTED(ValkeyModule_Fork)){
        ValkeyModule_ReplyWithError(ctx, "Fork api is not supported in the current valkey version");
        return VALKEYMODULE_OK;
    }

    ValkeyModule_StringToLongLong(argv[1], &code_to_exit_with);
    ValkeyModule_StringToLongLong(argv[2], &usleep_us);
    exited_with_code = -1;
    int fork_child_pid = ValkeyModule_Fork(done_handler, (void*)0xdeadbeef);
    if (fork_child_pid < 0) {
        ValkeyModule_ReplyWithError(ctx, "Fork failed");
        return VALKEYMODULE_OK;
    } else if (fork_child_pid > 0) {
        /* parent */
        child_pid = fork_child_pid;
        ValkeyModule_ReplyWithLongLong(ctx, child_pid);
        return VALKEYMODULE_OK;
    }

    /* child */
    ValkeyModule_Log(ctx, "notice", "fork child started");
    usleep(usleep_us);
    ValkeyModule_Log(ctx, "notice", "fork child exiting");
    ValkeyModule_ExitFromChild(code_to_exit_with);
    /* unreachable */
    return 0;
}

int fork_exitcode(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);
    ValkeyModule_ReplyWithLongLong(ctx, exited_with_code);
    return VALKEYMODULE_OK;
}

int fork_kill(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc)
{
    UNUSED(argv);
    UNUSED(argc);
    if (ValkeyModule_KillForkChild(child_pid) != VALKEYMODULE_OK)
        ValkeyModule_ReplyWithError(ctx, "KillForkChild failed");
    else
        ValkeyModule_ReplyWithLongLong(ctx, 1);
    child_pid = -1;
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);
    if (ValkeyModule_Init(ctx,"fork",1,VALKEYMODULE_APIVER_1)== VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"fork.create", fork_create,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"fork.exitcode", fork_exitcode,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx,"fork.kill", fork_kill,"",0,0,0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}
