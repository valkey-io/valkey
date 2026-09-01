#ifndef VALKEY_COMMANDS_H
#define VALKEY_COMMANDS_H

/* Must be synced with ARG_TYPE_STR and generate-command-code.py */
typedef enum {
    ARG_TYPE_STRING,
    ARG_TYPE_INTEGER,
    ARG_TYPE_DOUBLE,
    ARG_TYPE_KEY, /* A string, but represents a keyname */
    ARG_TYPE_PATTERN,
    ARG_TYPE_UNIX_TIME,
    ARG_TYPE_PURE_TOKEN,
    ARG_TYPE_ONEOF, /* Has subargs */
    ARG_TYPE_BLOCK  /* Has subargs */
} serverCommandArgType;

#define CMD_ARG_NONE (0)
#define CMD_ARG_OPTIONAL (1 << 0)
#define CMD_ARG_MULTIPLE (1 << 1)
#define CMD_ARG_MULTIPLE_TOKEN (1 << 2)

#define COMMAND_GET 0
#define COMMAND_SET 1
#define COMMAND_HGET 2
#define COMMAND_HSET 3
#define COMMAND_MSET 4

/* Command flags. Please check the definition of struct serverCommand in this file
 * for more information about the meaning of every flag. */
#define CMD_WRITE (1ULL << 0)
#define CMD_READONLY (1ULL << 1)
#define CMD_DENYOOM (1ULL << 2)
#define CMD_MODULE (1ULL << 3) /* Command exported by module. */
#define CMD_ADMIN (1ULL << 4)
#define CMD_PUBSUB (1ULL << 5)
#define CMD_NOSCRIPT (1ULL << 6)
#define CMD_BLOCKING (1ULL << 8) /* Has potential to block. */
#define CMD_LOADING (1ULL << 9)
#define CMD_STALE (1ULL << 10)
#define CMD_SKIP_MONITOR (1ULL << 11)
#define CMD_SKIP_COMMANDLOG (1ULL << 12)
#define CMD_ASKING (1ULL << 13)
#define CMD_FAST (1ULL << 14)
#define CMD_NO_AUTH (1ULL << 15)
#define CMD_MAY_REPLICATE (1ULL << 16)
#define CMD_SENTINEL (1ULL << 17)
#define CMD_ONLY_SENTINEL (1ULL << 18)
#define CMD_NO_MANDATORY_KEYS (1ULL << 19)
#define CMD_PROTECTED (1ULL << 20)
#define CMD_MODULE_GETKEYS (1ULL << 21)    /* Use the modules getkeys interface. */
#define CMD_MODULE_NO_CLUSTER (1ULL << 22) /* Deny on Cluster. */
#define CMD_NO_ASYNC_LOADING (1ULL << 23)
#define CMD_NO_MULTI (1ULL << 24)
#define CMD_MOVABLE_KEYS (1ULL << 25) /* The legacy range spec doesn't cover all keys. \
                                       * Populated by populateCommandLegacyRangeSpec. */
#define CMD_ALLOW_BUSY ((1ULL << 26))
#define CMD_MODULE_GETCHANNELS (1ULL << 27) /* Use the modules getchannels interface. */
#define CMD_TOUCHES_ARBITRARY_KEYS (1ULL << 28)
#define CMD_ALL_DBS (1ULL << 29)
/* Command flags. Please don't forget to add command flag documentation in struct
 * serverCommand in server.h file. */

/* Must be compatible with RedisModuleCommandArg. See moduleCopyCommandArgs. */
typedef struct serverCommandArg {
    const char *name;
    serverCommandArgType type;
    int key_spec_index;
    const char *token;
    const char *summary;
    const char *since;
    int flags;
    const char *deprecated_since;
    int num_args;
    struct serverCommandArg *subargs;
    const char *display_text;
} serverCommandArg;

/* Returns the command group name by group number. */
const char *commandGroupStr(int index);

#endif
