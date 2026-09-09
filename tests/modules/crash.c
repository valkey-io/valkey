#include "valkeymodule.h"

#include <strings.h>
#include <sys/mman.h>

#define UNUSED(V) ((void) V)

void assertCrash(ValkeyModuleInfoCtx *ctx, int for_crash_report) {
    UNUSED(ctx);
    UNUSED(for_crash_report);
    ValkeyModule_Assert(0);
}

void segfaultCrash(ValkeyModuleInfoCtx *ctx, int for_crash_report) {
    UNUSED(ctx);
    UNUSED(for_crash_report);
    /* Compiler gives warnings about writing to a random address
     * e.g "*((char*)-1) = 'x';". As a workaround, we map a read-only area
     * and try to write there to trigger segmentation fault. */
    char *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *p = 'x';
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);
    if (ValkeyModule_Init(ctx,"infocrash",1,VALKEYMODULE_APIVER_1)
            == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    ValkeyModule_Assert(argc == 1);
    if (!strcasecmp(ValkeyModule_StringPtrLen(argv[0], NULL), "segfault")) {
        if (ValkeyModule_RegisterInfoFunc(ctx, segfaultCrash) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    } else if(!strcasecmp(ValkeyModule_StringPtrLen(argv[0], NULL), "assert")) {
        if (ValkeyModule_RegisterInfoFunc(ctx, assertCrash) == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    } else {
        return VALKEYMODULE_ERR;
    }

    return VALKEYMODULE_OK;
}
