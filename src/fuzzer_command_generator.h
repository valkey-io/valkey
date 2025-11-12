#ifndef FUZZER_COMMAND_GENERATOR_H
#define FUZZER_COMMAND_GENERATOR_H

#include <valkey/valkey.h>
#include "sds.h"

typedef struct FuzzerCommand {
    sds *argv;
    int argc;
    int size;
} FuzzerCommand;

/* Fuzzing mode types */
typedef enum {
    FUZZ_MODE_MALFORMED_COMMANDS = (1 << 0),
    FUZZ_MODE_CONFIG_COMMANDS = (1 << 1)
} FuzzMode;

int initFuzzer(valkeyContext *ctx, int num_keys, int cluster_mode, int fuzz_flags);
void cleanupFuzzer(void);
void initThreadClientCtx(int fuzz_flags);
void resetClientFuzzCtx(void);
void freeClientCtx(void);
FuzzerCommand *generateCmd(void);
void freeCommand(FuzzerCommand *args);
char *printCommand(FuzzerCommand *cmd);

#endif /* FUZZER_COMMAND_GENERATOR_H */
