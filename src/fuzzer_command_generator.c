/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "fmacros.h"
#include <valkey/valkey.h>
#include "commands.h"
#include "fuzzer_command_generator.h"
#include "sds.h"
#include "dict.h"
#include "server.h"

#include <assert.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>

#define MAX_DEFAULT_NUMKEYS 5
#define MAX_NUM_PER_LUA 5 /* Maximum number of commands per Lua script */
#define DEFAULT_INTEGER_MAX 50
#define DEFAULT_DOUBLE_MIN 1.0
#define DEFAULT_DOUBLE_MAX 50.0
#define RANDOM_TIME_VARIANCE 60
#define OPTIONAL_ARG_PROBABILITY 2       /* 1/2 probability */
#define LUA_SCRIPT_PROBABILITY 100       /* 1/100 (1%) probability of generating a Lua script */
#define MULTIEXEC_PROBABILITY 50         /* 1/50 (2%) probability of generating a MULTI/EXEC block */
#define MULTIEXEC_END_PROBABILITY 5      /* 1/5 (20%) probability of ending a MULTI/EXEC block */
#define MALFORMED_COMMAND_PROBABILITY 20 /* 1/20 (5%) probability of generating a malformed command */
#define EXEC_PROBABILITY 5               /* 1/5 (20%) probability of ending a MULTI/EXEC block */

typedef enum {
    CMD_GROUP_UNKNOWN = 0,
    CMD_GROUP_STRING = 1,
    CMD_GROUP_LIST = 2,
    CMD_GROUP_SET = 3,
    CMD_GROUP_HASH = 4,
    CMD_GROUP_SORTED_SET = 5,
    CMD_GROUP_STREAM = 6,
    CMD_GROUP_HYPERLOGLOG = 7,
    CMD_GROUP_GEO = 8,
    CMD_GROUP_BITMAP = 9,
    CMD_GROUP_PUBSUB = 10,
    CMD_GROUP_GENERIC = 11
} CommandGroupType;

typedef enum {
    CONFIG_TYPE_STRING,
    CONFIG_TYPE_INT,
    CONFIG_TYPE_BOOL,
    CONFIG_TYPE_ENUM,
    CONFIG_TYPE_SPECIAL
} ConfigValueType;

struct CommandInfo;

/* Specifications for a command argument. */
typedef struct CommandArgument {
    sds name;
    serverCommandArgType type;
    sds token;
    int flags;
    int subargCount;
    struct CommandArgument *subargs;
    struct CommandInfo *parent; /* Reference to parent command */
} CommandArgument;

struct CommandInfo {
    sds name;
    int argCount;
    CommandArgument *args; /* An array of the command arguments. */
    struct CommandInfo *subcommands;
    uint64_t flags;         /* Bitmask of command flags from server.h */
    CommandGroupType group; /* Command group/type (e.g., "list", "set", "string", etc.) */
} CommandInfo;

typedef struct {
    int argc;
    sds *argv;
    sds fullname;
    int arity;           /* Command arity: positive if command has fixed number of arguments, negative if variable */
    int has_subcommands; /* Flag indicating if the command has subcommands */
    struct CommandInfo info;
} CommandEntry;

typedef struct ConfigEntry {
    sds value;
    ConfigValueType type;
} ConfigEntry;

/* Global fuzzer context structure */
typedef struct {
    CommandEntry *commandRegistry;
    size_t commandRegistrySize;
    CommandEntry *subscribeCommandRegistry;
    size_t subscribeCommandRegistrySize;
    dict *configDict;
    sds *aclCategories;
    size_t aclCategoriesCount;
    int max_keys;
    int cluster_mode;
} FuzzerContext;

typedef struct FuzzerClientCtx {
    int in_multiexec;
    int in_subscribe_mode;
    int subscribe_type;   /* 0 = SUBSCRIBE, 1 = PSUBSCRIBE, 2 = SSUBSCRIBE */
    int in_lua_script;    /* Flag to indicate if we're generating commands for a Lua script */
    sds current_slot_tag; /* Current slot tag for cluster mode to ensure all keys map to same slot */
    int numkeys;          /* Number of keys for current command generation */
    int fuzz_flags;       /* Fuzzing mode bit flags */
} FuzzerClientCtx;

/* Global fuzzer context */
static FuzzerContext *fuzz_ctx = NULL;

/* Thread-local client context */
static __thread FuzzerClientCtx *client_ctx = NULL;

static int mapFlagType(const sds flagStr) {
    static const struct {
        int flag;
        const char *name;
    } flagNames[] = {
        {CMD_WRITE, "write"},
        {CMD_READONLY, "readonly"},
        {CMD_DENYOOM, "denyoom"},
        {CMD_MODULE, "module"},
        {CMD_ADMIN, "admin"},
        {CMD_PUBSUB, "pubsub"},
        {CMD_NOSCRIPT, "noscript"},
        {CMD_BLOCKING, "blocking"},
        {CMD_LOADING, "loading"},
        {CMD_STALE, "stale"},
        {CMD_SKIP_MONITOR, "skip_monitor"},
        {CMD_SKIP_COMMANDLOG, "skip_commandlog"},
        {CMD_ASKING, "asking"},
        {CMD_FAST, "fast"},
        {CMD_NO_AUTH, "no_auth"},
        {CMD_MAY_REPLICATE, "may_replicate"},
        {CMD_SENTINEL, "sentinel"},
        {CMD_ONLY_SENTINEL, "only_sentinel"},
        {CMD_NO_MANDATORY_KEYS, "no_mandatory_keys"},
        {CMD_PROTECTED, "protected"},
        {CMD_MODULE_GETKEYS, "module_getkeys"},
        {CMD_MODULE_NO_CLUSTER, "module_no_cluster"},
        {CMD_NO_ASYNC_LOADING, "no_async_loading"},
        {CMD_NO_MULTI, "no_multi"},
        {CMD_MOVABLE_KEYS, "movablekeys"},
        {CMD_ALLOW_BUSY, "allow_busy"},
        {CMD_MODULE_GETCHANNELS, "module_getchannels"},
        {CMD_TOUCHES_ARBITRARY_KEYS, "touches_arbitrary_keys"},
        {0, NULL}};

    /* Compile-time check: Ensure we have entries for all flags up to bit 28.
     * If this fails, new CMD_* flags were added to server.h - update flagNames array above. */
    _Static_assert(sizeof(flagNames) / sizeof(flagNames[0]) > 28,
                   "flagNames array missing entries - update mapFlagType when CMD_* flags are added to server.h");

    if (!flagStr) return 0;

    for (int i = 0; flagNames[i].name != NULL; i++) {
        if (strcasecmp(flagStr, flagNames[i].name) == 0) {
            return flagNames[i].flag;
        }
    }

    return 0;
}

/* Map a type string to the corresponding argument type enum */
static int mapArgumentType(const sds typeStr) {
    static const struct {
        const char *name;
        int type;
    } typeMap[] = {
        {"string", ARG_TYPE_STRING},
        {"integer", ARG_TYPE_INTEGER},
        {"double", ARG_TYPE_DOUBLE},
        {"key", ARG_TYPE_KEY},
        {"pattern", ARG_TYPE_PATTERN},
        {"unix-time", ARG_TYPE_UNIX_TIME},
        {"pure-token", ARG_TYPE_PURE_TOKEN},
        {"oneof", ARG_TYPE_ONEOF},
        {"block", ARG_TYPE_BLOCK},
        {NULL, 0}};

    for (int i = 0; typeMap[i].name != NULL; i++) {
        if (!strcmp(typeStr, typeMap[i].name)) {
            return typeMap[i].type;
        }
    }

    return 0; /* Unknown type */
}

/* Map a group string to the corresponding group type enum */
static CommandGroupType mapGroupType(const sds groupStr) {
    static const struct {
        const char *name;
        CommandGroupType group;
    } groupMap[] = {
        {"string", CMD_GROUP_STRING},
        {"list", CMD_GROUP_LIST},
        {"set", CMD_GROUP_SET},
        {"hash", CMD_GROUP_HASH},
        {"sorted-set", CMD_GROUP_SORTED_SET},
        {"sorted_set", CMD_GROUP_SORTED_SET},
        {"stream", CMD_GROUP_STREAM},
        {"hyperloglog", CMD_GROUP_HYPERLOGLOG},
        {"geo", CMD_GROUP_GEO},
        {"bitmap", CMD_GROUP_BITMAP},
        {"pubsub", CMD_GROUP_PUBSUB},
        {"generic", CMD_GROUP_GENERIC},
        {NULL, CMD_GROUP_UNKNOWN}};

    if (!groupStr) return CMD_GROUP_UNKNOWN;

    for (int i = 0; groupMap[i].name != NULL; i++) {
        if (strcasecmp(groupStr, groupMap[i].name) == 0) {
            return groupMap[i].group;
        }
    }

    return CMD_GROUP_UNKNOWN;
}

static FuzzerCommand *allocCommand(void) {
    FuzzerCommand *cmd = zmalloc(sizeof(FuzzerCommand));
    cmd->argc = 0;
    cmd->size = 16;
    cmd->argv = zmalloc(sizeof(sds) * cmd->size);
    return cmd;
}

static void appendArg(FuzzerCommand *cmd, sds arg) {
    if (cmd->argc >= cmd->size) {
        cmd->size *= 2;
        cmd->argv = zrealloc(cmd->argv, sizeof(sds) * cmd->size);
    }
    cmd->argv[cmd->argc++] = arg;
}

void freeCommand(FuzzerCommand *cmd) {
    for (int i = 0; i < cmd->argc; i++) {
        sdsfree(cmd->argv[i]);
    }
    zfree(cmd->argv);
    zfree(cmd);
}

char *printCommand(FuzzerCommand *cmd) {
    static __thread char buffer[1024];
    int offset = 0;

    buffer[0] = '\0';
    for (int i = 0; i < cmd->argc; i++) {
        int arg_len = sdslen(cmd->argv[i]);
        if (arg_len > 50) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%.50s... ", cmd->argv[i]);
        } else {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s ", cmd->argv[i]);
        }

        /* Prevent buffer overflow */
        if ((size_t)offset >= sizeof(buffer) - 1) {
            break;
        }
    }

    return buffer;
}

/* Dictionary type implementation for config entries */
uint64_t configDictHashFunction(const void *key) {
    return dictGenHashFunction(key, strlen(key));
}

void configDictValDestructor(void *val) {
    ConfigEntry *entry = (ConfigEntry *)val;
    sdsfree(entry->value);
    zfree(entry);
}

static int sdsKeyCompare(const void *key1, const void *key2) {
    int l1, l2;
    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

static uint64_t sdsHash(const void *key) {
    return dictGenHashFunction((unsigned char *)key, sdslen((char *)key));
}

static void sdsDestructor(void *val) {
    sdsfree(val);
}

/* Dictionary type for config entries */
static dictType configDictType = {
    sdsHash,                 /* hash function */
    NULL,                    /* key dup */
    sdsKeyCompare,           /* key compare */
    sdsDestructor,           /* key destructor */
    configDictValDestructor, /* val destructor */
    NULL                     /* allow to expand */
};

dict *initConfigDict(void) {
    return dictCreate(&configDictType);
}

static int isEnumConfig(const char *key) {
    static const char *enumConfigs[] = {
        "repl-diskless-load",
        "loglevel",
        "maxmemory-policy",
        "appendfsync",
        "oom-score-adj",
        "acl-pubsub-default",
        "sanitize-dump-payload",
        "cluster-preferred-endpoint-type",
        "propagation-error-behavior",
        "shutdown-on-sigint",
        "shutdown-on-sigterm",
        "log-format",
        "log-timestamp-format",
        "rdb-version-check",
        NULL};

    for (int i = 0; enumConfigs[i] != NULL; i++) {
        if (strcasecmp(key, enumConfigs[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int isSpecialConfig(const char *key) {
    static const char *specialConfigs[] = {
        "dir",
        "save",
        "client-output-buffer-limit",
        "oom-score-adj-values",
        "notify-keyspace-events",
        "bind",
        "rdma-bind",
        "latency-tracking-info-percentiles",
        NULL};

    for (int i = 0; specialConfigs[i] != NULL; i++) {
        if (strcasecmp(key, specialConfigs[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

ConfigValueType determineConfigValueType(const char *value) {
    /* Check if it's a boolean */
    if (strcasecmp(value, "yes") == 0 || strcasecmp(value, "no") == 0) {
        return CONFIG_TYPE_BOOL;
    }

    /* Check if it's an integer */
    char *endptr;
    strtol(value, &endptr, 10);
    if (*value != '\0' && *endptr == '\0') {
        return CONFIG_TYPE_INT;
    }

    /* Default to string */
    return CONFIG_TYPE_STRING;
}

/* Check if a config should be filtered out to keep the server testable */
static int shouldFilterConfig(const char *key) {
    static const char *filteredConfigs[] = {
        "port",
        "cluster-port",
        "requirepass",
        "bind",
        "rdma-bind",
        "min-replicas-to-write",
        "replicaof",
        "dir",
        "save",
        "shutdown-on-sigint",
        "shutdown-on-sigterm",
        NULL};

    if (!key) return 0;

    /* Filter out any tls-* configs */
    if (strncasecmp(key, "tls-", 4) == 0) {
        return 1;
    }

    for (int i = 0; filteredConfigs[i] != NULL; i++) {
        if (strcasecmp(key, filteredConfigs[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

void addConfigEntry(dict *configDict, const char *key, const char *value) {
    ConfigEntry *entry = zmalloc(sizeof(ConfigEntry));
    entry->value = sdsnew(value);

    /* Determine the config type */
    if (isEnumConfig(key)) {
        entry->type = CONFIG_TYPE_ENUM;
    } else if (isSpecialConfig(key)) {
        entry->type = CONFIG_TYPE_SPECIAL;
    } else {
        entry->type = determineConfigValueType(value);
    }

    sds dict_key = sdsnew(key);
    dictAdd(configDict, (void *)dict_key, entry);
}

void generateRandomEnumValue(FuzzerCommand *cmd, ConfigEntry *entry, const char *config_name) {
    if (strcasecmp(config_name, "maxmemory-policy") == 0) {
        static const char *policies[] = {"volatile-lru", "volatile-lfu", "volatile-random",
                                         "volatile-ttl", "allkeys-lru", "allkeys-lfu",
                                         "allkeys-random", "noeviction"};
        appendArg(cmd, sdsnew(policies[rand() % 8]));
    } else if (strcasecmp(config_name, "loglevel") == 0) {
        static const char *levels[] = {"debug", "verbose", "notice", "warning", "nothing"};
        appendArg(cmd, sdsnew(levels[rand() % 5]));
    } else if (strcasecmp(config_name, "appendfsync") == 0) {
        static const char *options[] = {"everysec", "always", "no"};
        appendArg(cmd, sdsnew(options[rand() % 3]));
    } else if (strcasecmp(config_name, "oom-score-adj") == 0) {
        static const char *options[] = {"no", "yes", "relative", "absolute"};
        appendArg(cmd, sdsnew(options[rand() % 4]));
    } else if (strcasecmp(config_name, "acl-pubsub-default") == 0) {
        static const char *options[] = {"allchannels", "resetchannels"};
        appendArg(cmd, sdsnew(options[rand() % 2]));
    } else if (strcasecmp(config_name, "sanitize-dump-payload") == 0) {
        static const char *options[] = {"no", "yes", "clients"};
        appendArg(cmd, sdsnew(options[rand() % 3]));
    } else if (strcasecmp(config_name, "propagation-error-behavior") == 0) {
        static const char *options[] = {"ignore", "panic", "panic-on-replicas"};
        appendArg(cmd, sdsnew(options[rand() % 3]));
    } else if (strcasecmp(config_name, "log-format") == 0) {
        static const char *options[] = {"legacy", "logfmt"};
        appendArg(cmd, sdsnew(options[rand() % 2]));
    } else if (strcasecmp(config_name, "log-timestamp-format") == 0) {
        static const char *options[] = {"legacy", "iso8601", "milliseconds"};
        appendArg(cmd, sdsnew(options[rand() % 3]));
    } else if (strcasecmp(config_name, "rdb-version-check") == 0) {
        static const char *options[] = {"strict", "relaxed"};
        appendArg(cmd, sdsnew(options[rand() % 2]));
    } else if (strcasecmp(config_name, "repl-diskless-load") == 0) {
        static const char *options[] = {"disabled", "on-empty-db", "swapdb", "flush-before-load"};
        appendArg(cmd, sdsnew(options[rand() % 4]));
    } else {
        /* Default case - use the current value */
        appendArg(cmd, sdsnew(entry->value));
    }
}

void generateRandomSpecialValue(FuzzerCommand *cmd, ConfigEntry *entry, const char *config_name) {
    if (strcasecmp(config_name, "save") == 0) {
        /* Generate a valid save configuration: <seconds> <changes> */
        int seconds = 60 * (1 + rand() % 60); /* 60 to 3600 seconds */
        int changes = 1 + rand() % 10000;     /* 1 to 10000 changes */
        appendArg(cmd, sdscatprintf(sdsempty(), "%d %d", seconds, changes));
    } else if (strcasecmp(config_name, "client-output-buffer-limit") == 0) {
        /* Format: <class> <hard limit> <soft limit> <soft seconds> */
        static const char *classes[] = {"normal", "replica", "pubsub"};
        const char *class = classes[rand() % 3];
        long hard_limit = (1 + rand() % 10) * 1024 * 1024; /* 1MB to 10MB */
        long soft_limit = hard_limit / 2;                  /* Half of hard limit */
        int soft_seconds = 10 + rand() % 50;               /* 10 to 60 seconds */
        appendArg(cmd, sdscatprintf(sdsempty(), "%s %ld %ld %d",
                                    class, hard_limit, soft_limit, soft_seconds));
    } else if (strcasecmp(config_name, "notify-keyspace-events") == 0) {
        /* Generate valid keyspace notification configuration */
        static const char *options[] = {
            "",   /* Empty string = notifications disabled */
            "AK", /* All keyspace events for keys */
            "AE", /* All keyspace events for events */
            "K",  /* Keyspace events */
            "E",  /* Key-event events */
            "g",  /* Generic commands */
            "l",  /* List commands */
            "s",  /* Set commands */
            "h",  /* Hash commands */
            "z",  /* Sorted set commands */
            "x",  /* Expired events */
            "e",  /* Evicted events */
            "KEA" /* Combination */
        };
        appendArg(cmd, sdsnew(options[rand() % 14]));
    } else if (strcasecmp(config_name, "oom-score-adj-values") == 0) {
        /* Format: <value> <value> <value> */
        int val1 = rand() % 1000;
        int val2 = rand() % 1000;
        int val3 = rand() % 1000;
        appendArg(cmd, sdscatprintf(sdsempty(), "%d %d %d", val1, val2, val3));
    } else if (strcasecmp(config_name, "latency-tracking-info-percentiles") == 0) {
        /* Generate 1-3 percentile values between 0.0 and 100.0 */
        int num_percentiles = 1 + rand() % 3;
        sds percentiles = sdsempty();

        for (int i = 0; i < num_percentiles; i++) {
            double p = ((double)rand() / RAND_MAX) * 100.0;
            percentiles = sdscatprintf(percentiles, "%.1f", p);
            if (i < num_percentiles - 1) {
                percentiles = sdscat(percentiles, " ");
            }
        }
        appendArg(cmd, percentiles);
    } else if (strcasecmp(config_name, "rdma-bind") == 0 ||
               strcasecmp(config_name, "bind") == 0 ||
               strcasecmp(config_name, "dir") == 0) {
        /* For these configs, use the current value as we don't want to change them */
        appendArg(cmd, sdsnew(entry->value));
    } else {
        /* For any other special configs, use the current value */
        appendArg(cmd, sdsnew(entry->value));
    }
}

/* Generate a random value for a config entry based on its type */
void generateRandomConfigValue(FuzzerCommand *cmd, ConfigEntry *entry) {
    const char *config_name = cmd->argv[2];

    switch (entry->type) {
    case CONFIG_TYPE_BOOL:
        /* For boolean, randomly choose "yes" or "no" */
        appendArg(cmd, sdsnew(rand() % 2 ? "yes" : "no"));
        return;
    case CONFIG_TYPE_INT: {
        /* For integers, generate a value within a reasonable range */
        long original = strtol(entry->value, NULL, 10);
        long min = original / 2;
        long max = original * 5;

        /* Special case for maxmemory - ensure it's at least 10MB so we will continue to be able to run commands against the server*/
        if (strcasecmp(config_name, "maxmemory") == 0) {
            const long limit = 1024 * 1024 * 10;
            if (min < limit) min = limit;
        }

        /* Special case for maxmemory-clients - ensure it's at least 2KB so we will continue to be able to run commands against the server*/
        if (strcasecmp(config_name, "maxmemory-clients") == 0) {
            const long limit = 1024 * 2;
            if (min < limit) min = limit;
        }

        if (max < min + 1) max = min + 1;
        appendArg(cmd, sdscatprintf(sdsempty(), "%ld", min + rand() % (max - min + 1)));
        return;
    }
    case CONFIG_TYPE_ENUM:
        generateRandomEnumValue(cmd, entry, config_name);
        return;
    case CONFIG_TYPE_SPECIAL:
        generateRandomSpecialValue(cmd, entry, config_name);
        return;
    case CONFIG_TYPE_STRING:
    default:
        /* For strings, generate a random string */
        appendArg(cmd, sdscatprintf(sdsempty(), "random-string-%d", rand() % 1000));
        return;
    }
}

void generateConfigSetCommand(FuzzerCommand *cmd) {
    dict *configDict = fuzz_ctx->configDict;

    /* Get a random key from the dictionary */
    dictEntry *randomEntry = dictGetRandomKey(configDict);
    const char *key = dictGetKey(randomEntry);
    ConfigEntry *entry = dictGetVal(randomEntry);

    /* Build the CONFIG SET command */
    appendArg(cmd, sdsnew("CONFIG"));
    appendArg(cmd, sdsnew("SET"));
    appendArg(cmd, sdsnew(key));

    /* Generate a random value for this config */
    generateRandomConfigValue(cmd, entry);
}

static void parseAclCategories(valkeyReply *reply) {
    fuzz_ctx->aclCategoriesCount = reply->elements;
    fuzz_ctx->aclCategories = zmalloc(sizeof(sds) * reply->elements);

    for (size_t i = 0; i < reply->elements; i++) {
        if (reply->element[i]->type == VALKEY_REPLY_STRING) {
            fuzz_ctx->aclCategories[i] = sdsnew(reply->element[i]->str);
        }
    }
}

static void freeAclCategories(void) {
    if (!fuzz_ctx->aclCategories) return;
    for (size_t i = 0; i < fuzz_ctx->aclCategoriesCount; i++) {
        sdsfree(fuzz_ctx->aclCategories[i]);
    }
    zfree(fuzz_ctx->aclCategories);
    fuzz_ctx->aclCategories = NULL;
    fuzz_ctx->aclCategoriesCount = 0;
}

dict *parseConfigOutput(valkeyReply *reply) {
    dict *configDict = initConfigDict();

    /* `CONFIG GET *` returns an array of key-value pairs */
    for (size_t i = 0; i < reply->elements; i += 2) {
        if (i + 1 >= reply->elements) break;

        if (reply->element[i]->type != VALKEY_REPLY_STRING ||
            reply->element[i + 1]->type != VALKEY_REPLY_STRING) continue;

        const char *key = reply->element[i]->str;
        const char *value = reply->element[i + 1]->str;

        /* Skip configs that could make the server untestable */
        if (shouldFilterConfig(key)) continue;

        addConfigEntry(configDict, key, value);
    }

    return configDict;
}

/* Process argument flags from Vallkey reply */
static void processArgumentFlags(CommandArgument *cmdArg, valkeyReply *flags) {
    static const struct {
        const char *name;
        int flag;
    } flagMap[] = {
        {"optional", CMD_ARG_OPTIONAL},
        {"multiple", CMD_ARG_MULTIPLE},
        {"multiple_token", CMD_ARG_MULTIPLE_TOKEN},
        {NULL, 0}};

    for (size_t j = 0; j < flags->elements; j++) {
        char *flagStr = flags->element[j]->str;

        for (int k = 0; flagMap[k].name != NULL; k++) {
            if (strcmp(flagStr, flagMap[k].name)) continue;

            cmdArg->flags |= flagMap[k].flag;
            break;
        }
    }
}

/* Forward declaration */
static void parseCommandArguments(valkeyReply *arguments, CommandArgument *result, struct CommandInfo *parent);

static void processSubarguments(CommandArgument *cmdArg, valkeyReply *arguments) {
    cmdArg->subargs = zcalloc(arguments->elements * sizeof(CommandArgument));
    cmdArg->subargCount = arguments->elements;
    parseCommandArguments(arguments, cmdArg->subargs, cmdArg->parent);
}

/* Parse a single command argument from argument map */
static void parseCommandArgument(CommandArgument *cmdArg, valkeyReply *argMap) {
    if (argMap->type != VALKEY_REPLY_MAP && argMap->type != VALKEY_REPLY_ARRAY) return;

    for (size_t i = 0; i < argMap->elements; i += 2) {
        assert(argMap->element[i]->type == VALKEY_REPLY_STRING);
        char *key = argMap->element[i]->str;
        valkeyReply *value = argMap->element[i + 1];

        if (!strcmp(key, "name")) {
            cmdArg->name = zstrdup(value->str);
        } else if (!strcmp(key, "token")) {
            cmdArg->token = zstrdup(value->str);
        } else if (!strcmp(key, "type")) {
            assert(value->type == VALKEY_REPLY_STRING);
            cmdArg->type = mapArgumentType(value->str);
        } else if (!strcmp(key, "arguments")) {
            processSubarguments(cmdArg, value);
        } else if (!strcmp(key, "flags")) {
            processArgumentFlags(cmdArg, value);
        }
    }
}

/* Parse command arguments from Vallkey reply */
static void parseCommandArguments(valkeyReply *arguments, CommandArgument *result, struct CommandInfo *parent) {
    for (size_t j = 0; j < arguments->elements; j++) {
        result[j].parent = parent;
        parseCommandArgument(&result[j], arguments->element[j]);
    }
}

/* Returns the total number of commands and subcommands in the command docs table. */
static size_t countTotalCommands(valkeyReply *commandTable) {
    size_t commandCount = commandTable->elements / 2;

    /* The command docs table maps command names to a map of their specs. */
    for (size_t i = 0; i < commandTable->elements; i += 2) {
        valkeyReply *map = commandTable->element[i + 1];

        for (size_t j = 0; j < map->elements; j += 2) {
            char *key = map->element[j]->str;

            if (!strcmp(key, "subcommands")) {
                valkeyReply *subcommands = map->element[j + 1];
                commandCount += subcommands->elements / 2;
            }
        }
    }
    return commandCount;
}

/*  Fill in the fields of a help entry for the command/subcommand name. */
static void populateCommandEntry(CommandEntry *command, sds cmdName, sds subcommandName) {
    command->argc = subcommandName ? 2 : 1;
    command->argv = zmalloc(sizeof(char *) * command->argc);
    command->argv[0] = zstrdup(cmdName);

    /* Convert to uppercase */
    for (sds p = command->argv[0]; *p; p++) {
        *p = toupper(*p);
    }

    if (subcommandName) {
        /* Subcommand name may be two words separated by a pipe character. */
        sds pipe = strchr(subcommandName, '|');
        if (pipe != NULL) {
            command->argv[1] = zstrdup(pipe + 1);
        } else {
            command->argv[1] = zstrdup(subcommandName);
        }

        /* Convert to uppercase */
        for (sds p = command->argv[1]; *p; p++) {
            *p = toupper(*p);
        }
    }

    /* Create fullname */
    int fullnameLength = strlen(command->argv[0]) + 1; /* +1 for space */
    if (subcommandName) {
        fullnameLength += strlen(command->argv[1]) + 1; /* +1 for space */
    }

    command->fullname = zmalloc(fullnameLength);
    valkey_strlcpy(command->fullname, command->argv[0], fullnameLength);

    if (subcommandName) {
        valkey_strlcat(command->fullname, " ", fullnameLength);
        valkey_strlcat(command->fullname, command->argv[1], fullnameLength);
    }

    /* Initialize new fields */
    command->arity = 0; /* Will be set later in extractCommandFlags */
    command->info.name = command->fullname;
    command->info.args = NULL;
    command->info.argCount = 0;
}

/* Find a command in the registry by name and update its arity and flags */
static void updateCommand(sds cmdName, int arity, valkeyReply *flagsArray) {
    for (size_t j = 0; j < fuzz_ctx->commandRegistrySize; j++) {
        CommandEntry *cmd = &fuzz_ctx->commandRegistry[j];

        /* Check if this is the command we're looking for */
        if (cmd->argc != 1 || strcasecmp(cmd->argv[0], cmdName) != 0) continue;

        /* Set the arity */
        cmd->arity = arity;

        /* Copy each flag and update the flags bitmask */
        cmd->info.flags = 0;
        for (size_t k = 0; k < flagsArray->elements; k++) {
            if (flagsArray->element[k]->type != VALKEY_REPLY_STATUS) continue;

            sds flagStr = flagsArray->element[k]->str;
            /* Map the string flag to its enum value and update the bitmask */
            uint64_t flagValue = mapFlagType(flagStr);
            cmd->info.flags |= flagValue;
        }

        return;
    }
}

/* Extract command flags from COMMAND output and update the command registry */
static void extractCommandFlags(valkeyReply *info) {
    if (!info || info->type != VALKEY_REPLY_ARRAY) {
        return;
    }

    /* Iterate through each command in the COMMAND output */
    for (size_t i = 0; i < info->elements; i++) {
        valkeyReply *cmdEntry = info->element[i];

        /* Each command entry should be an array */
        if (cmdEntry->type != VALKEY_REPLY_ARRAY || cmdEntry->elements < 6) continue;

        /* Get command name */
        if (cmdEntry->element[0]->type != VALKEY_REPLY_STRING) continue;

        sds cmdName = cmdEntry->element[0]->str;

        /* Get command arity (element 1 in the COMMAND output) */
        int arity = 0;
        if (cmdEntry->element[1]->type == VALKEY_REPLY_INTEGER) {
            arity = cmdEntry->element[1]->integer;
        }

        /* Get command flags (element 2 in the COMMAND output) */
        if (cmdEntry->element[2]->type != VALKEY_REPLY_ARRAY) continue;

        valkeyReply *flagsArray = cmdEntry->element[2];

        for (size_t j = 0; j < fuzz_ctx->commandRegistrySize; j++) {
            CommandEntry *cmd = &fuzz_ctx->commandRegistry[j];

            /* Check if this is the command we're looking for */
            if (cmd->argc == 1 && strcasecmp(cmd->argv[0], cmdName) != 0) {
                /* update the command in our registry */
                updateCommand(cmdName, arity, flagsArray);
            }

            break;
        }
    }
}

/* Initialize a command entry for the command/subcommand described in 'specs'.
 * 'next' points to the next help entry to be filled in.
 * Returns a pointer to the next available position in the help entries table.
 * If the command has subcommands, this is called recursively for the subcommands.*/
static CommandEntry *initializeCommandEntry(sds cmdName, sds subcommandName, CommandEntry *next, valkeyReply *specs) {
    CommandEntry *command = next++;
    populateCommandEntry(command, cmdName, subcommandName);

    assert(specs->type == VALKEY_REPLY_MAP || specs->type == VALKEY_REPLY_ARRAY);

    /* Initialize command flags and group */
    command->info.flags = 0;
    command->info.group = CMD_GROUP_UNKNOWN;

    for (size_t j = 0; j < specs->elements; j += 2) {
        assert(specs->element[j]->type == VALKEY_REPLY_STRING);
        sds key = specs->element[j]->str;

        if (!strcmp(key, "arguments")) {
            valkeyReply *arguments = specs->element[j + 1];
            assert(arguments->type == VALKEY_REPLY_ARRAY);
            command->info.args = zcalloc(arguments->elements * sizeof(CommandArgument));
            command->info.argCount = arguments->elements;
            parseCommandArguments(arguments, command->info.args, &command->info);
        } else if (!strcmp(key, "group")) {
            /* Extract the command group/type */
            if (specs->element[j + 1]->type == VALKEY_REPLY_STRING) {
                command->info.group = mapGroupType(specs->element[j + 1]->str);
            }
        } else if (!strcmp(key, "subcommands")) {
            valkeyReply *subcommands = specs->element[j + 1];
            assert(subcommands->type == VALKEY_REPLY_MAP || subcommands->type == VALKEY_REPLY_ARRAY);

            /* Set has_subcommands flag to true */
            command->has_subcommands = 1;

            for (size_t i = 0; i < subcommands->elements; i += 2) {
                assert(subcommands->element[i]->type == VALKEY_REPLY_STRING);
                sds subName = subcommands->element[i]->str;
                valkeyReply *subcommand = subcommands->element[i + 1];
                assert(subcommand->type == VALKEY_REPLY_MAP || subcommand->type == VALKEY_REPLY_ARRAY);
                next = initializeCommandEntry(cmdName, subName, next, subcommand);
            }
        }
    }
    return next;
}

/* Initializes entries for all commands in the COMMAND DOCS reply.*/
static void initializeCommandRegistry(valkeyReply *commandTable) {
    /* Initialize command registry */
    fuzz_ctx->commandRegistrySize = countTotalCommands(commandTable);
    fuzz_ctx->commandRegistry = zmalloc(sizeof(CommandEntry) * fuzz_ctx->commandRegistrySize);
    /* Commands allowed in subscribe mode */
    const char *allowedCommands[] = {
        "SUBSCRIBE", "PSUBSCRIBE", "SSUBSCRIBE", "UNSUBSCRIBE", "PUNSUBSCRIBE", "SUNSUBSCRIBE", "PING", "QUIT", "RESET"};
    int numAllowedCommands = sizeof(allowedCommands) / sizeof(allowedCommands[0]);
    /* Allocate memory for the subscribe command registry */
    fuzz_ctx->subscribeCommandRegistry = zmalloc(sizeof(CommandEntry) * numAllowedCommands);
    fuzz_ctx->subscribeCommandRegistrySize = numAllowedCommands;

    CommandEntry *next = fuzz_ctx->commandRegistry;

    for (size_t i = 0; i < commandTable->elements; i += 2) {
        assert(commandTable->element[i]->type == VALKEY_REPLY_STRING);
        sds cmdName = commandTable->element[i]->str;

        assert(commandTable->element[i + 1]->type == VALKEY_REPLY_MAP ||
               commandTable->element[i + 1]->type == VALKEY_REPLY_ARRAY);
        valkeyReply *cmdSpecs = commandTable->element[i + 1];
        next = initializeCommandEntry(cmdName, NULL, next, cmdSpecs);
    }

    /* Copy to subscribeCommandRegistry the relevant commands */
    for (int i = 0; i < numAllowedCommands; i++) {
        for (size_t j = 0; j < fuzz_ctx->commandRegistrySize; j++) {
            CommandEntry *cmd = &fuzz_ctx->commandRegistry[j];
            if (strcasecmp(cmd->fullname, allowedCommands[i]) == 0) {
                fuzz_ctx->subscribeCommandRegistry[i] = *cmd;
                break;
            }
        }
    }
}

static void freeCommandArgument(CommandArgument *arg) {
    if (arg->name) {
        zfree(arg->name);
        arg->name = NULL;
    }
    if (arg->token) {
        zfree(arg->token);
        arg->token = NULL;
    }
    /* Free subarguments recursively */
    if (arg->subargs) {
        for (int i = 0; i < arg->subargCount; i++) {
            freeCommandArgument(&arg->subargs[i]);
        }
        zfree(arg->subargs);
        arg->subargs = NULL;
    }
}

static void freeCommandArguments(CommandEntry *command) {
    if (command->info.args) {
        /* zfree each argument */
        for (int i = 0; i < command->info.argCount; i++) {
            freeCommandArgument(&command->info.args[i]);
        }
        zfree(command->info.args);
        command->info.args = NULL;
    }
}

static void freeCommandEntry(CommandEntry *command) {
    freeCommandArguments(command);
    zfree(command->fullname);
    command->fullname = NULL;
    for (int i = 0; i < command->argc; i++) {
        zfree(command->argv[i]);
    }
    zfree(command->argv);
    command->argv = NULL;
}


/* Check if a command should be filtered out from the command registry as they may make the server untestable */
static int shouldFilterCommand(const sds cmdName, int fuzz_flags) {
    static const char *filteredCommands[] = {
        "SHUTDOWN",
        "FLUSHDB",
        "FLUSHALL",
        "REPLICAOF",
        "SLAVEOF",
        "FAILOVER",
        "CLUSTER REPLICATE",
        "SCRIPT DEBUG",
        "CLIENT PAUSE",
        "SAVE",
        "BGSAVE",
        "BGREWRITEAOF",
        "SYNC",
        "PSYNC",
        "MULTI",
        "EXEC",
        "EVAL",
        "CLIENT REPLY",
        "MONITOR",
        "DEBUG",
        "PFSELFTEST",
        "CONFIG REWRITE",
        "ACL LOAD",
        NULL};

    if (!cmdName) return 0;

    /* Check standard filtered commands */
    for (int i = 0; filteredCommands[i] != NULL; i++) {
        if (strcasecmp(cmdName, filteredCommands[i]) == 0) return 1;
    }

    /* If not in cluster mode, filter out all CLUSTER commands */
    if (!fuzz_ctx->cluster_mode) {
        if (strncasecmp(cmdName, "CLUSTER", 7) == 0) return 1;
        if (strncasecmp(cmdName, "ASKING", 6) == 0) return 1;
    }

    /* Filter CONFIG SET command if config-commands flag not set */
    if (!(fuzz_flags & FUZZ_MODE_CONFIG_COMMANDS) && strcasecmp(cmdName, "CONFIG SET") == 0) {
        return 1;
    }

    return 0;
}

static void removeInvalidCommands(int fuzz_flags) {
    /* Iterate over the registry and remove if args require and have subcommands */
    for (size_t i = 0; i < fuzz_ctx->commandRegistrySize;) {
        CommandEntry *cmd = &fuzz_ctx->commandRegistry[i];

        if (shouldFilterCommand(cmd->fullname, fuzz_flags) || (cmd->has_subcommands && cmd->arity == -2)) {
            /* Delete entry */

            /* First delete from subscribeCommandRegistry */
            for (size_t j = 0; j < fuzz_ctx->subscribeCommandRegistrySize; j++) {
                if (strcasecmp(fuzz_ctx->subscribeCommandRegistry[j].fullname, cmd->fullname) == 0) {
                    fuzz_ctx->subscribeCommandRegistry[j] = fuzz_ctx->subscribeCommandRegistry[fuzz_ctx->subscribeCommandRegistrySize - 1];
                    fuzz_ctx->subscribeCommandRegistrySize--;
                    break;
                }
            }
            freeCommandEntry(cmd);

            /* Delete from commandRegistry */
            fuzz_ctx->commandRegistry[i] = fuzz_ctx->commandRegistry[fuzz_ctx->commandRegistrySize - 1];
            fuzz_ctx->commandRegistrySize--;
        } else {
            i++;
        }
    }
}

void initializeRandomSeed(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand(time(NULL) ^ (unsigned long)pthread_self() ^ tv.tv_usec);
}

/* Initialize the fuzzer with a connected Valkey context */
int initFuzzer(valkeyContext *ctx, int num_keys, int cluster_mode, int fuzz_flags) {
    int ret = -1;
    fuzz_ctx = zmalloc(sizeof(FuzzerContext));
    /* Set global configuration values */
    fuzz_ctx->max_keys = num_keys > 0 ? num_keys : MAX_DEFAULT_NUMKEYS;
    fuzz_ctx->cluster_mode = cluster_mode;
    fuzz_ctx->aclCategories = NULL;
    fuzz_ctx->aclCategoriesCount = 0;

    valkeyReply *commandDocs = NULL;
    valkeyReply *commandInfo = NULL;
    valkeyReply *configOutput = NULL;
    valkeyReply *aclCatOutput = NULL;

    /* Execute COMMAND DOCS to get command documentation */
    commandDocs = valkeyCommand(ctx, "COMMAND DOCS");
    if (!commandDocs || commandDocs->type == VALKEY_REPLY_ERROR) {
        printf("Error: Failed to execute COMMAND DOCS. %s\n",
               commandDocs ? commandDocs->str : "No reply received");
        goto cleanup;
    }

    /* Execute COMMAND to get command flags */
    commandInfo = valkeyCommand(ctx, "COMMAND");
    if (!commandInfo || commandInfo->type == VALKEY_REPLY_ERROR) {
        printf("Error: Failed to execute COMMAND. %s\n",
               commandInfo ? commandInfo->str : "No reply received");
        goto cleanup;
    }

    /* Execute CONFIG GET * to get configuration parameters */
    configOutput = valkeyCommand(ctx, "CONFIG GET *");
    if (!configOutput || configOutput->type == VALKEY_REPLY_ERROR) {
        printf("Error: Failed to execute CONFIG GET *. %s\n",
               configOutput ? configOutput->str : "No reply received");
        goto cleanup;
    }

    /* Execute ACL CAT to get ACL categories */
    aclCatOutput = valkeyCommand(ctx, "ACL CAT");
    if (!aclCatOutput || aclCatOutput->type == VALKEY_REPLY_ERROR) {
        /* ACL CAT might not be available in older versions, continue without it */
        printf("Warning: ACL CAT command failed, using fallback ACL categories\n");
    } else {
        parseAclCategories(aclCatOutput);
    }

    /* First initialize with COMMAND DOCS data */
    initializeCommandRegistry(commandDocs);

    /* Add the command flags info from commandInfo which is COMMAND output */
    extractCommandFlags(commandInfo);

    /* Remove invalid commands */
    removeInvalidCommands(fuzz_flags);

    /* Parse configuration output */
    fuzz_ctx->configDict = parseConfigOutput(configOutput);
    /* Initialize random seed */
    initializeRandomSeed();
    ret = 0; /* Success */

cleanup:
    if (commandDocs) freeReplyObject(commandDocs);
    if (commandInfo) freeReplyObject(commandInfo);
    if (configOutput) freeReplyObject(configOutput);
    if (aclCatOutput) freeReplyObject(aclCatOutput);

    return ret;
}

/* Check if an optional argument should be skipped (50% chance) */
static int shouldSkipOptionalArgument(CommandArgument *arg) {
    return (arg->flags & CMD_ARG_OPTIONAL) && (rand() % OPTIONAL_ARG_PROBABILITY == 0);
}

/* Generate a new random slot tag for cluster mode */
static sds generateSlotTag(void) {
    int slotNum = rand() % 21; /* Generate slot number from 0 to 20 */
    return sdscatprintf(sdsempty(), "{slot-%d}", slotNum);
}

/* Ensure client context has a current slot tag for cluster mode */
static void ensureSlotTag(void) {
    /* In cluster mode, ensure we have a slot tag */
    if (!client_ctx->current_slot_tag) {
        client_ctx->current_slot_tag = generateSlotTag();
    }
}

/* Add keys to the command based on command group/type */
static void addKeysToCommand(FuzzerCommand *cmd, int numkeys, CommandArgument *arg) {
    /* Default prefix if we can't determine the type */
    const char *keyPrefix = "key";

    /* Try to determine the key type from the command group */
    if (arg->parent && arg->parent->group) {
        CommandGroupType groupType = arg->parent->group;

        switch (groupType) {
        case CMD_GROUP_STRING:
            keyPrefix = "string";
            break;
        case CMD_GROUP_LIST:
            keyPrefix = "list";
            break;
        case CMD_GROUP_SET:
            keyPrefix = "set";
            break;
        case CMD_GROUP_HASH:
            keyPrefix = "hash";
            break;
        case CMD_GROUP_SORTED_SET:
            keyPrefix = "zset";
            break;
        case CMD_GROUP_STREAM:
            keyPrefix = "stream";
            break;
        case CMD_GROUP_HYPERLOGLOG:
            keyPrefix = "hll";
            break;
        case CMD_GROUP_GEO:
            keyPrefix = "geo";
            break;
        case CMD_GROUP_BITMAP:
            keyPrefix = "bitmap";
            break;
        case CMD_GROUP_PUBSUB:
            keyPrefix = "channel";
            break;
        case CMD_GROUP_GENERIC: {
            /* For generic commands, randomly select one of the key types */
            static const char *keyTypes[] = {
                "string", "list", "set", "hash", "zset", "stream", "hll", "geo", "bitmap", "key"};
            int randomTypeIndex = rand() % (sizeof(keyTypes) / sizeof(keyTypes[0]));
            keyPrefix = keyTypes[randomTypeIndex];
        } break;
        default:
            break;
        }
    }

    for (int i = 0; i < numkeys; i++) {
        int keyNumber = rand() % fuzz_ctx->max_keys;
        sds keyName;

        /* In cluster mode, ensure all keys use the same slot tag to map to the same slot */
        if (fuzz_ctx->cluster_mode && client_ctx && client_ctx->current_slot_tag) {
            keyName = sdscatprintf(sdsempty(), "%s%s:%d", client_ctx->current_slot_tag, keyPrefix, keyNumber);
        } else {
            keyName = sdscatprintf(sdsempty(), "%s:%d", keyPrefix, keyNumber);
        }

        appendArg(cmd, keyName);
    }
}

void generateSingleCmd(FuzzerCommand *cmd);
void generateCommandsWithLua(FuzzerCommand *cmd);

/* Check if the current command is a lexicographical range command */
static int isLexicographicalCommand(CommandArgument *arg) {
    if (!arg || !arg->parent || !arg->parent->name) {
        return 0;
    }

    const char *cmdName = arg->parent->name;

    /* Check for explicit lexicographical commands */
    if (strstr(cmdName, "LEX") != NULL) {
        return 1;
    }
    return 0;
}

/* Generate a lexicographical range value (for commands like ZLEXCOUNT, ZRANGEBYLEX, etc.) */
static void generateLexRangeValue(FuzzerCommand *cmd, const char *argName) {
    /* Lexicographical range values can be:
     * - [value] (inclusive)
     * - (value) (exclusive)
     * - - (negative infinity)
     * - + (positive infinity)  */

    int choice = rand() % 10;

    if (choice == 0) {
        /* Negative infinity */
        appendArg(cmd, sdsnew("-"));
    } else if (choice == 1) {
        /* Positive infinity */
        appendArg(cmd, sdsnew("+"));
    } else if (choice < 6) {
        /* Inclusive range with a string value */
        appendArg(cmd, sdscatprintf(sdsempty(), "[%s%d",
                                    strcmp(argName, "min") == 0 ? "a" : "z", rand() % 100));
    } else {
        /* Exclusive range with a string value */
        appendArg(cmd, sdscatprintf(sdsempty(), "(%s%d",
                                    strcmp(argName, "min") == 0 ? "a" : "z", rand() % 100));
    }
}

/* Generate a random ACL rule */
static void generateAclRule(FuzzerCommand *cmd) {
    /* Generate valid ACL rules dynamically using fetched categories */
    int rule_type = rand() % 10;

    switch (rule_type) {
    case 0: /* User state rules */
        appendArg(cmd, sdsnew(rand() % 2 ? "on" : "off"));
        break;
    case 1: /* Password rules */
        if (rand() % 3 == 0) {
            appendArg(cmd, sdsnew("nopass"));
        } else {
            appendArg(cmd, sdscatprintf(sdsempty(), ">pass%d", rand() % 100));
        }
        break;
    case 2: /* Key pattern rules */
    {
        int choice = rand() % 4;
        if (choice == 0) {
            appendArg(cmd, sdsnew("allkeys"));
        } else if (choice == 1) {
            appendArg(cmd, sdsnew("nokeys"));
        } else if (choice == 2) {
            appendArg(cmd, sdsnew("~*"));
        } else {
            appendArg(cmd, sdscatprintf(sdsempty(), "~key:%d:*", rand() % 10));
        }
    } break;
    case 3: /* Command category allow rules using dynamic categories */
        if (fuzz_ctx->aclCategories && fuzz_ctx->aclCategoriesCount > 0) {
            int cat_idx = rand() % fuzz_ctx->aclCategoriesCount;
            appendArg(cmd, sdscatprintf(sdsempty(), "+@%s", fuzz_ctx->aclCategories[cat_idx]));
        } else {
            /* Fallback to common categories if ACL CAT failed */
            static const char *fallback_cats[] = {"all", "read", "write", "admin", "dangerous"};
            int cat_idx = rand() % (sizeof(fallback_cats) / sizeof(fallback_cats[0]));
            appendArg(cmd, sdscatprintf(sdsempty(), "+@%s", fallback_cats[cat_idx]));
        }
        break;
    case 4: /* Command category deny rules using dynamic categories */
        if (fuzz_ctx->aclCategories && fuzz_ctx->aclCategoriesCount > 0) {
            int cat_idx = rand() % fuzz_ctx->aclCategoriesCount;
            appendArg(cmd, sdscatprintf(sdsempty(), "-@%s", fuzz_ctx->aclCategories[cat_idx]));
        } else {
            /* Fallback to common categories if ACL CAT failed */
            static const char *fallback_cats[] = {"dangerous", "admin", "write", "blocking"};
            int cat_idx = rand() % (sizeof(fallback_cats) / sizeof(fallback_cats[0]));
            appendArg(cmd, sdscatprintf(sdsempty(), "-@%s", fallback_cats[cat_idx]));
        }
        break;
    case 5: /* Specific command allow rules */
    {
        static const char *commands[] = {"get", "set", "del", "exists", "ping",
                                         "info", "keys", "scan", "type", "ttl"};
        int cmd_idx = rand() % (sizeof(commands) / sizeof(commands[0]));
        appendArg(cmd, sdscatprintf(sdsempty(), "+%s", commands[cmd_idx]));
    } break;
    case 6: /* Specific command deny rules */
    {
        static const char *commands[] = {"flushdb", "flushall", "shutdown", "debug",
                                         "config", "eval", "script", "client"};
        int cmd_idx = rand() % (sizeof(commands) / sizeof(commands[0]));
        appendArg(cmd, sdscatprintf(sdsempty(), "-%s", commands[cmd_idx]));
    } break;
    case 7: /* Channel pattern rules */
    {
        int choice = rand() % 3;
        if (choice == 0) {
            appendArg(cmd, sdsnew("allchannels"));
        } else if (choice == 1) {
            appendArg(cmd, sdsnew("&*"));
        } else {
            appendArg(cmd, sdscatprintf(sdsempty(), "&channel:%d:*", rand() % 5));
        }
    } break;
    case 8: /* Reset rules */
        appendArg(cmd, sdsnew("reset"));
        break;
    default: /* Simple combination rules */
        if (rand() % 2) {
            appendArg(cmd, sdsnew("+@read"));
        } else {
            appendArg(cmd, sdsnew("-@dangerous"));
        }
        break;
    }
}

/* Generate a random network address (IP or hostname) with port */
static sds generateRandomAddress(void) {
    /* Generate a valid IP:port format */
    int port = 10000 + (rand() % 55535); /* Port range 10000-65535 */

    /* 50% chance to generate IPv4, 50% chance to generate hostname */
    if (rand() % 2 == 0) {
        /* Generate IPv4 address */
        return sdscatprintf(sdsempty(), "192.168.%d.%d:%d",
                            rand() % 256, rand() % 256, port);
    } else {
        /* Generate hostname */
        return sdscatprintf(sdsempty(), "host-%d.example.com:%d",
                            rand() % 10, port);
    }
}

/* Generate plausible string values for Valkey command arguments */
static void generateStringArgValue(FuzzerCommand *cmd, const char *argName, CommandArgument *arg) {
    static const char *usernames[] = {"alice", "bob", "charlie", "dave", "eve"};
    static const char *commands[] = {"GET", "SET", "DEL", "HSET", "LPUSH", "ZADD", "PUBLISH"};
    static const char *types[] = {"string", "list", "set", "zset", "hash", "stream"};
    static const char *capabilities[] = {"read", "write", "admin", "pubsub", "blocking", "dangerous"};
    static const char *events[] = {"connected", "disconnected", "updated", "expired", "evicted", "failed"};
    static const char *sections[] = {
        "all",
        "server",
        "clients",
        "memory",
        "persistence",
        "stats",
        "replication",
        "cpu",
        "modules",
        "debug",
        "module_list",
        "errorstats",
        "cluster",
        "keyspace",
        "everything",
        "latencystats",
    };

    // Generate values based on argument name and append directly to cmd
    if (strcmp(argName, "command-name") == 0) {
        appendArg(cmd, sdsnew(commands[rand() % (sizeof(commands) / sizeof(commands[0]))]));
    } else if (strcmp(argName, "command") == 0) {
        // Select a random command from the available commands array
        appendArg(cmd, sdsnew(commands[rand() % (sizeof(commands) / sizeof(commands[0]))]));
    } else if (strcmp(argName, "username") == 0) {
        appendArg(cmd, sdsnew(usernames[rand() % (sizeof(usernames) / sizeof(usernames[0]))]));
    } else if (strcmp(argName, "password") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "pass%d", rand() % 1000));
    } else if (strcmp(argName, "channel") == 0 || strcmp(argName, "shardchannel") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "channel:%d", rand() % 2));
    } else if (strcmp(argName, "key") == 0) {
        sds keyName;
        /* In cluster mode, ensure all keys use the same slot tag to map to the same slot */
        if (fuzz_ctx->cluster_mode && client_ctx && client_ctx->current_slot_tag) {
            keyName = sdscatprintf(sdsempty(), "%skey:%d", client_ctx->current_slot_tag, rand() % 100);
        } else {
            keyName = sdscatprintf(sdsempty(), "key:%d", rand() % 100);
        }
        appendArg(cmd, keyName);
    } else if (strcmp(argName, "field") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "field:%d", rand() % 20));
    } else if (strcmp(argName, "value") == 0) {
        /* Generate random value with 95% chance between 1 byte and 1KB, 5% chance between 1KB and 10KB */
        int value_size;
        if ((rand() % 100) < 95) {
            /* 95% chance: between 1 byte and 1KB */
            value_size = 1 + (rand() % 1024);
        } else {
            /* 5% chance: between 1KB and 10KB */
            value_size = 1024 + (rand() % (10240 - 1024));
        }

        sds value = sdsnewlen(NULL, value_size);
        memset(value, 'x', value_size);
        appendArg(cmd, value);
    } else if (strcmp(argName, "serialized-value") == 0) {
        /* Generate a serialized value for restore command */
        static const char *serialized_values[] = {
            /* String value value-534 with proper RDB format */
            "\x00\x09value-534\x0b\x00\xc9\x88\x82M\xfb{\x0e1",
            /* Integer value 537 with proper RDB format */
            "\x00\xc1\x19\x02\x0b\x00\x03Uh3\xba\xdc\xde\xac"};
        appendArg(cmd, sdscatprintf(sdsempty(), "\"%s\"", serialized_values[rand() % 2]));
    } else if (strcmp(argName, "member") == 0 ||
               strcmp(argName, "member1") == 0 ||
               strcmp(argName, "member2") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "member:%d", rand() % 50));
    } else if (strcmp(argName, "host") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "host-%d.example.com", rand() % 5));
    } else if (strcmp(argName, "ip") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "192.168.%d.%d", rand() % 256, rand() % 256));
    } else if (strcmp(argName, "message") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "message-%d", rand() % 1000));
    } else if (strcmp(argName, "name") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "name-%d", rand() % 20));
    } else if (strcmp(argName, "group") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "group-%d", rand() % 10));
    } else if (strcmp(argName, "consumer") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "consumer-%d", rand() % 15));
    } else if (strcmp(argName, "id") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "%d-%d", rand() % 1000, rand() % 1000));
    } else if (strcmp(argName, "start") == 0 || strcmp(argName, "end") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "%d", rand() % 1000));
    } else if (strcmp(argName, "min") == 0) {
        if (isLexicographicalCommand(arg)) {
            generateLexRangeValue(cmd, argName);
        } else {
            appendArg(cmd, sdscatprintf(sdsempty(), "%d", rand() % 50));
        }
    } else if (strcmp(argName, "max") == 0) {
        if (isLexicographicalCommand(arg)) {
            generateLexRangeValue(cmd, argName);
        } else {
            appendArg(cmd, sdscatprintf(sdsempty(), "%d", rand() % 50 + 50));
        }
    } else if (strcmp(argName, "type") == 0) {
        appendArg(cmd, sdsnew(types[rand() % (sizeof(types) / sizeof(types[0]))]));
    } else if (strcmp(argName, "capability") == 0) {
        appendArg(cmd, sdsnew(capabilities[rand() % (sizeof(capabilities) / sizeof(capabilities[0]))]));
    } else if (strcmp(argName, "capa") == 0) {
        /* For client list CAPA filter, currently only 'r' is supported (CLIENT_CAPA_REDIRECT) */
        appendArg(cmd, sdsnew("r"));
    } else if (strcmp(argName, "section") == 0) {
        appendArg(cmd, sdsnew(sections[rand() % (sizeof(sections) / sizeof(sections[0]))]));
    } else if (strcmp(argName, "event") == 0) {
        appendArg(cmd, sdsnew(events[rand() % (sizeof(events) / sizeof(events[0]))]));
    } else if (strcmp(argName, "path") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "/path/to/file%d", rand() % 10));
    } else if (strcmp(argName, "prefix") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "prefix:%d:", rand() % 5));
    } else if (strcmp(argName, "script") == 0) {
        FuzzerCommand *luaCmd = allocCommand();
        generateCommandsWithLua(luaCmd);
        /* Extract just the script part (second argument of EVAL command) */
        appendArg(cmd, sdsdup(luaCmd->argv[1]));
        freeCommand(luaCmd);
    } else if (strcmp(argName, "function") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "myfunc%d", rand() % 5));
    } else if (strcmp(argName, "function-code") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "\"#!lua name=myfunc%d \nserver.register_function('test', function(keys, args) return args[1] end) \"", rand() % 5));
    } else if (strcmp(argName, "library-name") == 0 || strcmp(argName, "library-name-pattern") == 0 ||
               strcmp(argName, "lib-name") == 0 || strcmp(argName, "libname") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "lib%d", rand() % 5));
    } else if (strcmp(argName, "libver") == 0 || strcmp(argName, "lib-ver") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "%d.%d.%d", rand() % 10, rand() % 10, rand() % 10));
    } else if (strcmp(argName, "node-id") == 0 || strcmp(argName, "nodename") == 0 ||
               strcmp(argName, "importing") == 0 || strcmp(argName, "migrating") == 0 ||
               strcmp(argName, "node") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "%08x%08x%08x%08x%08x",
                                    rand(), rand(), rand(), rand(), rand()));
    } else if (strcmp(argName, "encoding") == 0) {
        /* For BITFIELD command, encoding should be i<bits> or u<bits> format */
        static const char *signs[] = {"i", "u"};
        static const int bits[] = {1, 2, 4, 8, 16, 32, 64};
        appendArg(cmd, sdscatprintf(sdsempty(), "%s%d", signs[rand() % 2], bits[rand() % (sizeof(bits) / sizeof(bits[0]))]));
    } else if (strcmp(argName, "old-format") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "192.168.%d.%d:%d", rand() % 256, rand() % 256, 6379 + (rand() % 1000)));
    } else if (strcmp(argName, "runid") == 0 || strcmp(argName, "replicationid") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "%08x%08x%08x%08x%08x",
                                    rand(), rand(), rand(), rand(), rand()));
    } else if (strcmp(argName, "sha1") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "%08x%08x%08x%08x%08x",
                                    rand(), rand(), rand(), rand(), rand()));
    } else if (strcmp(argName, "last-id") == 0 || strcmp(argName, "lastid") == 0 ||
               strcmp(argName, "max-deleted-id") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "%d-%d", rand() % 1000, rand() % 1000));
    } else if (strcmp(argName, "min-idle-time") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "%d", rand() % 10000));
    } else if (strcmp(argName, "connection-name") == 0 || strcmp(argName, "clientname") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "conn-%d", rand() % 20));
    } else if (strcmp(argName, "primary-name") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "primary-%d", rand() % 5));
    } else if (strcmp(argName, "category") == 0) {
        static const char *categories[] = {"connection", "generic", "string", "list", "set",
                                           "sorted_set", "hash", "pubsub", "transactions", "scripting"};
        appendArg(cmd, sdsnew(categories[rand() % (sizeof(categories) / sizeof(categories[0]))]));
    } else if (strcmp(argName, "flags") == 0) {
        static const char *client_flags[] = {"A", "b", "c", "d", "e", "i", "M", "N",
                                             "O", "P", "r", "S", "u", "U", "x", "t", "T", "R", "B", "I"};
        appendArg(cmd, sdsnew(client_flags[rand() % (sizeof(client_flags) / sizeof(client_flags[0]))]));
    } else if (strcmp(argName, "element") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "elem-%d", rand() % 30));
    } else if (strcmp(argName, "pivot") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "pivot-%d", rand() % 10));
    } else if (strcmp(argName, "parameter") == 0) {
        static const char *params[] = {"maxmemory", "timeout", "databases", "appendonly", "save"};
        appendArg(cmd, sdsnew(params[rand() % (sizeof(params) / sizeof(params[0]))]));
    } else if (strcmp(argName, "rule") == 0) {
        /* Call the dedicated ACL rule generation function */
        generateAclRule(cmd);
    } else if (strcmp(argName, "subcommand") == 0) {
        static const char *subcmds[] = {"get", "set", "reset", "help", "info", "list"};
        appendArg(cmd, sdsnew(subcmds[rand() % (sizeof(subcmds) / sizeof(subcmds[0]))]));
    } else if (strcmp(argName, "stop") == 0) {
        if (rand() % 2 == 0)
            appendArg(cmd, sdscatprintf(sdsempty(), "%d", rand() % 1000));
        else
            appendArg(cmd, sdsnew(rand() % 2 ? "-" : "+"));
    } else if (strcmp(argName, "module-name") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "module-%d", rand() % 100));
    } else if (strcmp(argName, "arg") == 0 || strcmp(argName, "args") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "arg%d", rand() % 10));
    } else if (strcmp(argName, "command") == 0) {
        appendArg(cmd, sdsnew(commands[rand() % (sizeof(commands) / sizeof(commands[0]))]));
    } else if (strcmp(argName, "threshold") == 0) {
        appendArg(cmd, sdscatprintf(sdsempty(), "%d", rand() % 30));
    } else if (strcmp(argName, "metric") == 0) {
        static const char *metrics[] = {"key-count", "cpu-usec", "network-bytes-in", "network-bytes-out"};
        appendArg(cmd, sdsnew(metrics[rand() % 4]));
    } else if (strcmp(argName, "addr") == 0 || strcmp(argName, "laddr") == 0) {
        appendArg(cmd, generateRandomAddress());
    } else {
        // Default case for any other string arguments
        appendArg(cmd, sdscatprintf(sdsempty(), "str-%s-%d", argName, rand() % 1000));
    }
}

/* Check if a command is a blocking command by examining its flags*/
static int isBlockingCommand(struct CommandInfo *cmd) {
    return (cmd->flags & CMD_BLOCKING);
}

/* Check if an argument is a timeout parameter for a blocking command */
static int isBlockingTimeout(CommandArgument *arg) {
    if (!arg->name || !arg->parent || !arg->parent->name) {
        return 0;
    }

    /* Check if this is a timeout parameter */
    if (strcasecmp(arg->name, "timeout") != 0) {
        return 0;
    }

    /* Check if the parent command is a blocking command */
    return isBlockingCommand(arg->parent);
}

static void addArgumentToCommand(FuzzerCommand *cmd, CommandArgument *arg) {
    /* Skip optional arguments randomly */
    if (shouldSkipOptionalArgument(arg)) return;

    /* Check if multiple flag is set - generate 1-3 values randomly */
    int repeat_count = 1;
    if (arg->flags & CMD_ARG_MULTIPLE) {
        /* Special case: for WEIGHTS in set operations (ZUNION, ZINTER, etc.),
         * the number of weights must match the number of keys */
        if (arg->token && strcasecmp(arg->token, "WEIGHTS") == 0) {
            repeat_count = client_ctx->numkeys;
        } else if (rand() % 30 == 0) {
            repeat_count = 1 + (rand() % 3); /* Random value between 1 and 3 */
        }
    }

    for (int i = 0; i < repeat_count; i++) {
        /* Add token if present */
        if (arg->token != NULL) {
            appendArg(cmd, sdsnew(arg->token));
            if (arg->type == ARG_TYPE_PURE_TOKEN) continue;
        }

        /* Handle numkeys parameter */
        if (arg->name && strcmp(arg->name, "numkeys") == 0) {
            client_ctx->numkeys = 1 + (rand() % (MAX_DEFAULT_NUMKEYS));
            appendArg(cmd, sdscatprintf(sdsempty(), "%d", client_ctx->numkeys));
            repeat_count = 1;
        } else if (arg->type == ARG_TYPE_ONEOF) {
            int index = rand() % arg->subargCount; /* Choose randomly one of the args */
            CommandArgument *selected_arg = &arg->subargs[index];

            /* For oneof arguments, temporarily clear the optional flag to ensure
             * the selected argument is not skipped, since we need at least one option */
            int original_flags = selected_arg->flags;
            selected_arg->flags &= ~CMD_ARG_OPTIONAL;
            addArgumentToCommand(cmd, selected_arg);
            selected_arg->flags = original_flags; /* Restore original flags */
        } else if (arg->type == ARG_TYPE_STRING) {
            if (arg->name) {
                /* Check if this is a timeout parameter for a blocking command */
                if (isBlockingTimeout(arg)) {
                    /* For blocking commands, always use 1 second timeout */
                    appendArg(cmd, sdsnew("1"));
                } else {
                    generateStringArgValue(cmd, arg->name, arg);
                }
            } else {
                appendArg(cmd, sdsnew("string-value"));
            }
        } else if (arg->type == ARG_TYPE_INTEGER) {
            /* Check if this is a timeout parameter for a blocking command */
            if (isBlockingTimeout(arg)) {
                /* For blocking commands, always use 1 second timeout */
                appendArg(cmd, sdsnew("1"));
            } else if (arg->name && strcmp(arg->name, "bit") == 0) {
                /* For bit arguments, generate only 0 or 1 */
                appendArg(cmd, sdscatprintf(sdsempty(), "%d", rand() % 2));
            } else {
                appendArg(cmd, sdscatprintf(sdsempty(), "%d", 1 + (rand() % DEFAULT_INTEGER_MAX)));
            }
        } else if (arg->type == ARG_TYPE_DOUBLE) {
            /* Check if this is a timeout parameter for a blocking command */
            if (isBlockingTimeout(arg)) {
                /* For blocking commands, always use 1 second timeout */
                double timeout = (double)rand() / RAND_MAX;
                appendArg(cmd, sdscatprintf(sdsempty(), "%f", timeout));
            } else {
                double val = ((double)rand() / RAND_MAX) * (DEFAULT_DOUBLE_MAX - DEFAULT_DOUBLE_MIN);
                appendArg(cmd, sdscatprintf(sdsempty(), "%f", val));
            }
        } else if (arg->type == ARG_TYPE_UNIX_TIME) {
            time_t currentTime = time(NULL);
            /* add a random number of seconds to the current time */
            currentTime += rand() % RANDOM_TIME_VARIANCE;
            appendArg(cmd, sdscatprintf(sdsempty(), "%ld", currentTime));
        } else if (arg->type == ARG_TYPE_PATTERN) {
            appendArg(cmd, sdsnew("*"));
        } else if (arg->type == ARG_TYPE_KEY) {
            /* For key arguments, don't use repeat_count - generate exactly numkeys keys */
            addKeysToCommand(cmd, client_ctx->numkeys, arg);
            break; /* Exit the repeat loop for key arguments */
        } else if (arg->type == ARG_TYPE_BLOCK) {
            for (int j = 0; j < arg->subargCount; j++) {
                addArgumentToCommand(cmd, &arg->subargs[j]);
            }
        }
    }
}

static void generateCommandArguments(FuzzerCommand *cmd, CommandEntry *selectedCommand) {
    /* Reset numkeys for each new command */
    client_ctx->numkeys = 1;

    for (int i = 0; i < selectedCommand->info.argCount; i++) {
        addArgumentToCommand(cmd, &selectedCommand->info.args[i]);
    }
}

/* Handle pubsub command selection and state management */
static CommandEntry *handlePubSubCommandSelection(void) {
    CommandEntry *selectedCommand;
    int randomIndex = rand() % fuzz_ctx->subscribeCommandRegistrySize;
    selectedCommand = &fuzz_ctx->subscribeCommandRegistry[randomIndex];

    /* Check if this command puts the client in unsubscribe mode */
    if ((strcasecmp(selectedCommand->fullname, "UNSUBSCRIBE") == 0 && client_ctx->subscribe_type == 0) ||
        (strcasecmp(selectedCommand->fullname, "PUNSUBSCRIBE") == 0 && client_ctx->subscribe_type == 1) ||
        (strcasecmp(selectedCommand->fullname, "SUNSUBSCRIBE") == 0 && client_ctx->subscribe_type == 2) ||
        (strcasecmp(selectedCommand->fullname, "RESET") == 0)) {
        client_ctx->in_subscribe_mode = 0;
        client_ctx->subscribe_type = 0;
    }

    return selectedCommand;
}

/* Check if command puts client in subscribe mode and update state accordingly */
static void checkAndUpdateSubscribeMode(const char *commandName) {
    if (strcasecmp(commandName, "SUBSCRIBE") == 0) {
        client_ctx->subscribe_type = 0; /* SUBSCRIBE type */
    } else if (strcasecmp(commandName, "SSUBSCRIBE") == 0) {
        client_ctx->subscribe_type = 2; /* SSUBSCRIBE type */
    } else if (strcasecmp(commandName, "PSUBSCRIBE") == 0) {
        client_ctx->subscribe_type = 1; /* PSUBSCRIBE type */
    } else {
        return;
    }
    client_ctx->in_subscribe_mode = 1;
}

void generateSingleCmd(FuzzerCommand *cmd) {
    /* Ensure we have a slot tag for cluster mode to keep all keys in the same slot */
    ensureSlotTag();

    CommandEntry *selectedCommand;

    do {
        int randomIndex = rand() % fuzz_ctx->commandRegistrySize;
        selectedCommand = &fuzz_ctx->commandRegistry[randomIndex];
    } while (client_ctx->in_lua_script && (selectedCommand->info.flags & CMD_NOSCRIPT));

    if (client_ctx->in_subscribe_mode) {
        selectedCommand = handlePubSubCommandSelection();
    }

    /* Special case CONFIG SET command */
    if (strcasecmp(selectedCommand->fullname, "CONFIG SET") == 0) {
        generateConfigSetCommand(cmd);
        return;
    }

    /* Initialize result with command name */
    /* If the command has sub commands we need to add both the command and the subcommand */
    if (strstr(selectedCommand->fullname, " ")) {
        /* Add the command name */
        sds commandName = sdsnewlen(selectedCommand->fullname, strcspn(selectedCommand->fullname, " "));
        sds subCommand = sdsnewlen(strchr(selectedCommand->fullname, ' ') + 1, strlen(selectedCommand->fullname) - strcspn(selectedCommand->fullname, " "));
        appendArg(cmd, commandName);
        appendArg(cmd, subCommand);
    } else {
        appendArg(cmd, sdsnew(selectedCommand->fullname));
    }

    /* Add arguments based on the command's argument specification */
    if (selectedCommand->info.args != NULL) {
        generateCommandArguments(cmd, selectedCommand);
    }

    /* Check if this command puts the client in subscribe mode */
    if (client_ctx->in_subscribe_mode) return;

    /* Check and update subscribe mode based on command name */
    checkAndUpdateSubscribeMode(selectedCommand->fullname);
}

/* Generates multiple commands and wraps them in a Lua script.*/
void generateCommandsWithLua(FuzzerCommand *cmd) {
    /* Determine how many commands to include (between 1 and MAX_NUM_PER_LUA) */
    int numCommands = 1 + (rand() % MAX_NUM_PER_LUA);

    /* Start building the Lua script */
    appendArg(cmd, sdsnew("EVAL"));
    sds script = sdsnew("local result = {} ");

    /* Set flag to indicate we're generating commands for a Lua script */
    client_ctx->in_lua_script = 1;

    /* Generate and add commands to the Lua script */
    for (int i = 0; i < numCommands; i++) {
        FuzzerCommand *subCommand = allocCommand();
        generateSingleCmd(subCommand);
        script = sdscatprintf(script, "result[%d] = redis.call(", i + 1);
        /* concatenate the command to the buffer */
        for (int j = 0; j < subCommand->argc; j++) {
            script = sdscatprintf(script, "\"%s\",", subCommand->argv[j]);
        }
        /* Override the last comma */
        script[sdslen(script) - 1] = ')';
        script = sdscat(script, " ");
        /* Free the subcommand arguments */
        freeCommand(subCommand);
    }

    /* Reset the Lua script flag */
    client_ctx->in_lua_script = 0;

    /* Complete the Lua script (for simplicity we don't supply the keys and args) */
    script = sdscat(script, "return result");
    /* Add the script to the command arguments */
    appendArg(cmd, script);
    /* Add the number of keys and arguments to the command */
    appendArg(cmd, sdsnew("0"));
}

/* Shuffle arguments (excluding command name) */
static void shuffleArguments(FuzzerCommand *cmd) {
    if (cmd->argc <= 2) return;

    for (int j = 0; j < 2; j++) {
        int idx1 = (rand() % (cmd->argc - 1)) + 1;
        int idx2 = (rand() % (cmd->argc - 1)) + 1;
        if (idx1 != idx2) {
            sds temp = cmd->argv[idx1];
            cmd->argv[idx1] = cmd->argv[idx2];
            cmd->argv[idx2] = temp;
        }
    }
}

/* Create a corrupted argument value */
static sds createCorruptedArg(void) {
    switch (rand() % 5) {
    case 0: /* Random binary data */
    {
        int len = rand() % 16 + 10;
        sds arg = sdsnewlen(NULL, len);
        for (int k = 0; k < len - 1; k++)
            ((char *)arg)[k] = rand() % 256;
        return arg;
    }
    case 1: /* Random Long string */
    {
        int len = rand() % 128 + 1024;
        sds arg = sdsnewlen(NULL, len);
        memset(arg, 'A' + (rand() % 26), len - 1);
        return arg;
    }
    case 2: /* Empty string */
        return sdsnew("");
    case 3: /* Special characters */
        return sdsnew("\n\r\t\"\\'$%^&*(){}[]<>");
    case 4: /* Invalid number */
    default: {
        const char *invalid_nums[] = {"123abc", "-+123", "12.34.56", "NaN", "Infinity"};
        return sdsnew(invalid_nums[rand() % 5]);
    }
    }
}

/* Corrupt an argument's content */
static void corruptArgument(FuzzerCommand *cmd) {
    int idx = (rand() % cmd->argc);
    /* 70% chance to preserve command name */
    if (idx == 0 && (rand() % 10 < 7)) return;

    sdsfree(cmd->argv[idx]);
    cmd->argv[idx] = createCorruptedArg();
}

/* Remove a random argument */
static void removeArgument(FuzzerCommand *cmd) {
    if (cmd->argc <= 1) return;

    int idx = (rand() % (cmd->argc - 1)) + 1; /* Don't remove command name */
    sdsfree(cmd->argv[idx]);

    /* Shift remaining arguments */
    for (int k = idx; k < cmd->argc - 1; k++)
        cmd->argv[k] = cmd->argv[k + 1];

    cmd->argc--;
}

static void addRandomArgument(FuzzerCommand *cmd) {
    if (cmd->argc >= cmd->size) return;

    sds new_arg = NULL;
    switch (rand() % 3) {
    case 0: /* Random string */
        new_arg = sdsnew("random_value");
        break;
    case 1: /* Random number */
        new_arg = sdsfromlonglong(rand() % 1000);
        break;
    case 2:
        new_arg = sdsnew("\n\r\t\"\\'$%^&*(){}[]<>");
        break;
    }

    /* Insert at random position */
    int pos = rand() % (cmd->argc + 1);
    for (int k = cmd->argc; k > pos; k--)
        cmd->argv[k] = cmd->argv[k - 1];

    cmd->argv[pos] = new_arg;
    cmd->argc++;
}

/* Generate a malformed command by corrupting a legitimate command */
void generateMalformedCommand(FuzzerCommand *cmd) {
    /* First generate a legitimate command */
    generateSingleCmd(cmd);
    if (!cmd || cmd->argc <= 1) return;

    /* Apply 1-3 corruption types */
    int corruption_count = (rand() % 3) + 1;

    for (int i = 0; i < corruption_count; i++) {
        switch (rand() % 4) {
        case 0: shuffleArguments(cmd); break;
        case 1: corruptArgument(cmd); break;
        case 2: removeArgument(cmd); break;
        case 3: addRandomArgument(cmd); break;
        }
    }
}

/* Cleanup and zfree all resources allocated by the fuzzer */
void cleanupFuzzer(void) {
    if (!fuzz_ctx) return;
    if (fuzz_ctx->commandRegistry) {
        for (size_t i = 0; i < fuzz_ctx->commandRegistrySize; i++) {
            freeCommandEntry(&fuzz_ctx->commandRegistry[i]);
        }
        zfree(fuzz_ctx->commandRegistry);
    }

    if (fuzz_ctx->subscribeCommandRegistry) {
        zfree(fuzz_ctx->subscribeCommandRegistry);
    }

    if (fuzz_ctx->configDict) {
        dictRelease(fuzz_ctx->configDict);
    }

    freeAclCategories();

    zfree(fuzz_ctx);
    fuzz_ctx = NULL;
}

/* Initialize thread-local client context - called at thread start */
void initThreadClientCtx(int fuzz_flags) {
    client_ctx = zmalloc(sizeof(FuzzerClientCtx));
    client_ctx->in_multiexec = 0;
    client_ctx->in_subscribe_mode = 0;
    client_ctx->subscribe_type = 0;
    client_ctx->in_lua_script = 0;
    client_ctx->current_slot_tag = NULL;
    client_ctx->numkeys = 1;
    client_ctx->fuzz_flags = fuzz_flags;

    initializeRandomSeed();
}

void resetClientFuzzCtx(void) {
    if (client_ctx == NULL) return;
    client_ctx->in_multiexec = 0;
    client_ctx->in_subscribe_mode = 0;
    client_ctx->subscribe_type = 0;
    client_ctx->in_lua_script = 0;
    if (client_ctx->current_slot_tag) {
        sdsfree(client_ctx->current_slot_tag);
        client_ctx->current_slot_tag = NULL;
    }
}

/* Free thread-local client context */
void freeClientCtx(void) {
    if (client_ctx == NULL) return;
    if (client_ctx->current_slot_tag) {
        sdsfree(client_ctx->current_slot_tag);
    }
    zfree(client_ctx);
    client_ctx = NULL;
}

/* Generates a random command or a Lua script with commands */
FuzzerCommand *generateCmd(void) {
    FuzzerCommand *cmd = allocCommand();

    /* In malformed-commands mode, generate malformed commands 5% of the time */
    if ((client_ctx->fuzz_flags & FUZZ_MODE_MALFORMED_COMMANDS) && (rand() % MALFORMED_COMMAND_PROBABILITY == 0)) {
        generateMalformedCommand(cmd);
        return cmd;
    }

    if (client_ctx->in_multiexec) {
        if (rand() % MULTIEXEC_END_PROBABILITY == 0) {
            client_ctx->in_multiexec = 0;
            appendArg(cmd, sdsnew("EXEC"));
            return cmd;
        }
    }

    if (rand() % LUA_SCRIPT_PROBABILITY == 0 && !client_ctx->in_subscribe_mode) {
        generateCommandsWithLua(cmd);
    } else if (rand() % MULTIEXEC_PROBABILITY == 0 && !client_ctx->in_subscribe_mode) {
        client_ctx->in_multiexec = 1;
        appendArg(cmd, sdsnew("MULTI"));
    } else {
        generateSingleCmd(cmd);
    }

    sdsfree(client_ctx->current_slot_tag);
    client_ctx->current_slot_tag = NULL;

    return cmd;
}
