#ifndef FUZZER_COMMAND_GENERATOR_H
#define FUZZER_COMMAND_GENERATOR_H

#include <valkey/valkey.h>
#include "sds.h"

typedef struct Command {
    sds *argv;
    int argc;
    int size;
} Command;

/* Fuzzing mode types */
typedef enum {
    NORMAL = 0,
    AGGRESSIVE = 1
} FuzzMode;

int initFuzzer(valkeyContext *ctx, int num_keys, int cluster_mode, FuzzMode fuzz_level);
void cleanupFuzzer(void);
void initThreadClientCtx(FuzzMode fuzz_level);
void resetClientFuzzCtx(void);
void freeClientCtx(void);
Command *generateCmd(void);
void freeCommand(Command *args);
void printCommand(Command *cmd);

#endif /* FUZZER_COMMAND_GENERATOR_H */
