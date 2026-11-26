/* Test module that deterministically holds the _dl_load_lock across a crash. */

#define _GNU_SOURCE
#include "valkeymodule.h"

#define UNUSED(V) ((void)V)

#ifdef __linux__

#include <link.h>
#include <pthread.h>
#include <time.h>

#define HOLD_SECONDS 10

static pthread_barrier_t held_barrier;

/* dl_iterate_phdr callback. */
static int hold_phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    UNUSED(info);
    UNUSED(size);
    UNUSED(data);

    /* Rendezvous with the command. */
    pthread_barrier_wait(&held_barrier);

    struct timespec rem = {.tv_sec = HOLD_SECONDS, .tv_nsec = 0};
    while (nanosleep(&rem, &rem) == -1) {
    }

    return 1;
}

static void *holder_thread(void *arg) {
    UNUSED(arg);
    /* Enters dl_iterate_phdr, which takes _dl_load_lock. */
    dl_iterate_phdr(hold_phdr_cb, NULL);
    return NULL;
}

/* LOADERLOCK.HOLD
 * Spawns a thread that will hold the _dl_load_lock across a crash. */
static int cmd_hold(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    if (pthread_barrier_init(&held_barrier, NULL, 2) != 0) {
        ValkeyModule_ReplyWithError(ctx, "ERR failed to init barrier");
        return VALKEYMODULE_ERR;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, holder_thread, NULL) != 0) {
        pthread_barrier_destroy(&held_barrier);
        ValkeyModule_ReplyWithError(ctx, "ERR failed to create holder thread");
        return VALKEYMODULE_ERR;
    }
    pthread_detach(tid);

    pthread_barrier_wait(&held_barrier);
    pthread_barrier_destroy(&held_barrier);

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    if (ValkeyModule_Init(ctx, "loaderlock", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    if (ValkeyModule_CreateCommand(ctx, "loaderlock.hold", cmd_hold, "", 0, 0, 0) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}

#else /* !__linux__ */

/* Non-Linux: empty module so the build succeeds. The test is Linux-only. */
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    UNUSED(argv);
    UNUSED(argc);

    if (ValkeyModule_Init(ctx, "loaderlock", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    return VALKEYMODULE_OK;
}

#endif /* __linux__ */
