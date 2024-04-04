#ifndef SERVERMODULE_H
#define SERVERMODULE_H

#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>


typedef struct RedisModuleString RedisModuleString;
typedef struct RedisModuleKey RedisModuleKey;

/* -------------- Defines NOT common between core and modules ------------- */

#if defined SERVERMODULE_CORE
/* Things only defined for the modules core (server), not exported to modules
 * that include this file. */

#define RedisModuleString robj

#endif /* defined SERVERMODULE_CORE */

#if !defined SERVERMODULE_CORE && !defined SERVERMODULE_CORE_MODULE
/* Things defined for modules, but not for core-modules. */

typedef long long mstime_t;
typedef long long ustime_t;

#endif /* !defined SERVERMODULE_CORE && !defined SERVERMODULE_CORE_MODULE */

/* ---------------- Defines common between core and modules --------------- */

/* Error status return values. */
#define SERVERMODULE_OK 0
#define SERVERMODULE_ERR 1

/* Module Based Authentication status return values. */
#define SERVERMODULE_AUTH_HANDLED 0
#define SERVERMODULE_AUTH_NOT_HANDLED 1

/* API versions. */
#define SERVERMODULE_APIVER_1 1

/* Version of the RedisModuleTypeMethods structure. Once the RedisModuleTypeMethods 
 * structure is changed, this version number needs to be changed synchronistically. */
#define SERVERMODULE_TYPE_METHOD_VERSION 5

/* API flags and constants */
#define SERVERMODULE_READ (1<<0)
#define SERVERMODULE_WRITE (1<<1)

/* RedisModule_OpenKey extra flags for the 'mode' argument.
 * Avoid touching the LRU/LFU of the key when opened. */
#define SERVERMODULE_OPEN_KEY_NOTOUCH (1<<16)
/* Don't trigger keyspace event on key misses. */
#define SERVERMODULE_OPEN_KEY_NONOTIFY (1<<17)
/* Don't update keyspace hits/misses counters. */
#define SERVERMODULE_OPEN_KEY_NOSTATS (1<<18)
/* Avoid deleting lazy expired keys. */
#define SERVERMODULE_OPEN_KEY_NOEXPIRE (1<<19)
/* Avoid any effects from fetching the key */
#define SERVERMODULE_OPEN_KEY_NOEFFECTS (1<<20)
/* Mask of all SERVERMODULE_OPEN_KEY_* values. Any new mode should be added to this list.
 * Should not be used directly by the module, use RM_GetOpenKeyModesAll instead.
 * Located here so when we will add new modes we will not forget to update it. */
#define _SERVERMODULE_OPEN_KEY_ALL SERVERMODULE_READ | SERVERMODULE_WRITE | SERVERMODULE_OPEN_KEY_NOTOUCH | SERVERMODULE_OPEN_KEY_NONOTIFY | SERVERMODULE_OPEN_KEY_NOSTATS | SERVERMODULE_OPEN_KEY_NOEXPIRE | SERVERMODULE_OPEN_KEY_NOEFFECTS

/* List push and pop */
#define SERVERMODULE_LIST_HEAD 0
#define SERVERMODULE_LIST_TAIL 1

/* Key types. */
#define SERVERMODULE_KEYTYPE_EMPTY 0
#define SERVERMODULE_KEYTYPE_STRING 1
#define SERVERMODULE_KEYTYPE_LIST 2
#define SERVERMODULE_KEYTYPE_HASH 3
#define SERVERMODULE_KEYTYPE_SET 4
#define SERVERMODULE_KEYTYPE_ZSET 5
#define SERVERMODULE_KEYTYPE_MODULE 6
#define SERVERMODULE_KEYTYPE_STREAM 7

/* Reply types. */
#define SERVERMODULE_REPLY_UNKNOWN -1
#define SERVERMODULE_REPLY_STRING 0
#define SERVERMODULE_REPLY_ERROR 1
#define SERVERMODULE_REPLY_INTEGER 2
#define SERVERMODULE_REPLY_ARRAY 3
#define SERVERMODULE_REPLY_NULL 4
#define SERVERMODULE_REPLY_MAP 5
#define SERVERMODULE_REPLY_SET 6
#define SERVERMODULE_REPLY_BOOL 7
#define SERVERMODULE_REPLY_DOUBLE 8
#define SERVERMODULE_REPLY_BIG_NUMBER 9
#define SERVERMODULE_REPLY_VERBATIM_STRING 10
#define SERVERMODULE_REPLY_ATTRIBUTE 11
#define SERVERMODULE_REPLY_PROMISE 12

/* Postponed array length. */
#define SERVERMODULE_POSTPONED_ARRAY_LEN -1  /* Deprecated, please use SERVERMODULE_POSTPONED_LEN */
#define SERVERMODULE_POSTPONED_LEN -1

/* Expire */
#define SERVERMODULE_NO_EXPIRE -1

/* Sorted set API flags. */
#define SERVERMODULE_ZADD_XX      (1<<0)
#define SERVERMODULE_ZADD_NX      (1<<1)
#define SERVERMODULE_ZADD_ADDED   (1<<2)
#define SERVERMODULE_ZADD_UPDATED (1<<3)
#define SERVERMODULE_ZADD_NOP     (1<<4)
#define SERVERMODULE_ZADD_GT      (1<<5)
#define SERVERMODULE_ZADD_LT      (1<<6)

/* Hash API flags. */
#define SERVERMODULE_HASH_NONE       0
#define SERVERMODULE_HASH_NX         (1<<0)
#define SERVERMODULE_HASH_XX         (1<<1)
#define SERVERMODULE_HASH_CFIELDS    (1<<2)
#define SERVERMODULE_HASH_EXISTS     (1<<3)
#define SERVERMODULE_HASH_COUNT_ALL  (1<<4)

#define SERVERMODULE_CONFIG_DEFAULT 0 /* This is the default for a module config. */
#define SERVERMODULE_CONFIG_IMMUTABLE (1ULL<<0) /* Can this value only be set at startup? */
#define SERVERMODULE_CONFIG_SENSITIVE (1ULL<<1) /* Does this value contain sensitive information */
#define SERVERMODULE_CONFIG_HIDDEN (1ULL<<4) /* This config is hidden in `config get <pattern>` (used for tests/debugging) */
#define SERVERMODULE_CONFIG_PROTECTED (1ULL<<5) /* Becomes immutable if enable-protected-configs is enabled. */
#define SERVERMODULE_CONFIG_DENY_LOADING (1ULL<<6) /* This config is forbidden during loading. */

#define SERVERMODULE_CONFIG_MEMORY (1ULL<<7) /* Indicates if this value can be set as a memory value */
#define SERVERMODULE_CONFIG_BITFLAGS (1ULL<<8) /* Indicates if this value can be set as a multiple enum values */

/* StreamID type. */
typedef struct RedisModuleStreamID {
    uint64_t ms;
    uint64_t seq;
} RedisModuleStreamID;

/* StreamAdd() flags. */
#define SERVERMODULE_STREAM_ADD_AUTOID (1<<0)
/* StreamIteratorStart() flags. */
#define SERVERMODULE_STREAM_ITERATOR_EXCLUSIVE (1<<0)
#define SERVERMODULE_STREAM_ITERATOR_REVERSE (1<<1)
/* StreamIteratorTrim*() flags. */
#define SERVERMODULE_STREAM_TRIM_APPROX (1<<0)

/* Context Flags: Info about the current context returned by
 * RM_GetContextFlags(). */

/* The command is running in the context of a Lua script */
#define SERVERMODULE_CTX_FLAGS_LUA (1<<0)
/* The command is running inside a Redis transaction */
#define SERVERMODULE_CTX_FLAGS_MULTI (1<<1)
/* The instance is a master */
#define SERVERMODULE_CTX_FLAGS_MASTER (1<<2)
/* The instance is a slave */
#define SERVERMODULE_CTX_FLAGS_SLAVE (1<<3)
/* The instance is read-only (usually meaning it's a slave as well) */
#define SERVERMODULE_CTX_FLAGS_READONLY (1<<4)
/* The instance is running in cluster mode */
#define SERVERMODULE_CTX_FLAGS_CLUSTER (1<<5)
/* The instance has AOF enabled */
#define SERVERMODULE_CTX_FLAGS_AOF (1<<6)
/* The instance has RDB enabled */
#define SERVERMODULE_CTX_FLAGS_RDB (1<<7)
/* The instance has Maxmemory set */
#define SERVERMODULE_CTX_FLAGS_MAXMEMORY (1<<8)
/* Maxmemory is set and has an eviction policy that may delete keys */
#define SERVERMODULE_CTX_FLAGS_EVICT (1<<9)
/* Redis is out of memory according to the maxmemory flag. */
#define SERVERMODULE_CTX_FLAGS_OOM (1<<10)
/* Less than 25% of memory available according to maxmemory. */
#define SERVERMODULE_CTX_FLAGS_OOM_WARNING (1<<11)
/* The command was sent over the replication link. */
#define SERVERMODULE_CTX_FLAGS_REPLICATED (1<<12)
/* Redis is currently loading either from AOF or RDB. */
#define SERVERMODULE_CTX_FLAGS_LOADING (1<<13)
/* The replica has no link with its master, note that
 * there is the inverse flag as well:
 *
 *  SERVERMODULE_CTX_FLAGS_REPLICA_IS_ONLINE
 *
 * The two flags are exclusive, one or the other can be set. */
#define SERVERMODULE_CTX_FLAGS_REPLICA_IS_STALE (1<<14)
/* The replica is trying to connect with the master.
 * (REPL_STATE_CONNECT and REPL_STATE_CONNECTING states) */
#define SERVERMODULE_CTX_FLAGS_REPLICA_IS_CONNECTING (1<<15)
/* THe replica is receiving an RDB file from its master. */
#define SERVERMODULE_CTX_FLAGS_REPLICA_IS_TRANSFERRING (1<<16)
/* The replica is online, receiving updates from its master. */
#define SERVERMODULE_CTX_FLAGS_REPLICA_IS_ONLINE (1<<17)
/* There is currently some background process active. */
#define SERVERMODULE_CTX_FLAGS_ACTIVE_CHILD (1<<18)
/* The next EXEC will fail due to dirty CAS (touched keys). */
#define SERVERMODULE_CTX_FLAGS_MULTI_DIRTY (1<<19)
/* Redis is currently running inside background child process. */
#define SERVERMODULE_CTX_FLAGS_IS_CHILD (1<<20)
/* The current client does not allow blocking, either called from
 * within multi, lua, or from another module using RM_Call */
#define SERVERMODULE_CTX_FLAGS_DENY_BLOCKING (1<<21)
/* The current client uses RESP3 protocol */
#define SERVERMODULE_CTX_FLAGS_RESP3 (1<<22)
/* Redis is currently async loading database for diskless replication. */
#define SERVERMODULE_CTX_FLAGS_ASYNC_LOADING (1<<23)
/* Redis is starting. */
#define SERVERMODULE_CTX_FLAGS_SERVER_STARTUP (1<<24)

/* Next context flag, must be updated when adding new flags above!
This flag should not be used directly by the module.
 * Use RedisModule_GetContextFlagsAll instead. */
#define _SERVERMODULE_CTX_FLAGS_NEXT (1<<25)

/* Keyspace changes notification classes. Every class is associated with a
 * character for configuration purposes.
 * NOTE: These have to be in sync with NOTIFY_* in server.h */
#define SERVERMODULE_NOTIFY_KEYSPACE (1<<0)    /* K */
#define SERVERMODULE_NOTIFY_KEYEVENT (1<<1)    /* E */
#define SERVERMODULE_NOTIFY_GENERIC (1<<2)     /* g */
#define SERVERMODULE_NOTIFY_STRING (1<<3)      /* $ */
#define SERVERMODULE_NOTIFY_LIST (1<<4)        /* l */
#define SERVERMODULE_NOTIFY_SET (1<<5)         /* s */
#define SERVERMODULE_NOTIFY_HASH (1<<6)        /* h */
#define SERVERMODULE_NOTIFY_ZSET (1<<7)        /* z */
#define SERVERMODULE_NOTIFY_EXPIRED (1<<8)     /* x */
#define SERVERMODULE_NOTIFY_EVICTED (1<<9)     /* e */
#define SERVERMODULE_NOTIFY_STREAM (1<<10)     /* t */
#define SERVERMODULE_NOTIFY_KEY_MISS (1<<11)   /* m (Note: This one is excluded from SERVERMODULE_NOTIFY_ALL on purpose) */
#define SERVERMODULE_NOTIFY_LOADED (1<<12)     /* module only key space notification, indicate a key loaded from rdb */
#define SERVERMODULE_NOTIFY_MODULE (1<<13)     /* d, module key space notification */
#define SERVERMODULE_NOTIFY_NEW (1<<14)        /* n, new key notification */

/* Next notification flag, must be updated when adding new flags above!
This flag should not be used directly by the module.
 * Use RedisModule_GetKeyspaceNotificationFlagsAll instead. */
#define _SERVERMODULE_NOTIFY_NEXT (1<<15)

#define SERVERMODULE_NOTIFY_ALL (SERVERMODULE_NOTIFY_GENERIC | SERVERMODULE_NOTIFY_STRING | SERVERMODULE_NOTIFY_LIST | SERVERMODULE_NOTIFY_SET | SERVERMODULE_NOTIFY_HASH | SERVERMODULE_NOTIFY_ZSET | SERVERMODULE_NOTIFY_EXPIRED | SERVERMODULE_NOTIFY_EVICTED | SERVERMODULE_NOTIFY_STREAM | SERVERMODULE_NOTIFY_MODULE)      /* A */

/* A special pointer that we can use between the core and the module to signal
 * field deletion, and that is impossible to be a valid pointer. */
#define SERVERMODULE_HASH_DELETE ((RedisModuleString*)(long)1)

/* Error messages. */
#define SERVERMODULE_ERRORMSG_WRONGTYPE "WRONGTYPE Operation against a key holding the wrong kind of value"

#define SERVERMODULE_POSITIVE_INFINITE (1.0/0.0)
#define SERVERMODULE_NEGATIVE_INFINITE (-1.0/0.0)

/* Cluster API defines. */
#define SERVERMODULE_NODE_ID_LEN 40
#define SERVERMODULE_NODE_MYSELF     (1<<0)
#define SERVERMODULE_NODE_MASTER     (1<<1)
#define SERVERMODULE_NODE_SLAVE      (1<<2)
#define SERVERMODULE_NODE_PFAIL      (1<<3)
#define SERVERMODULE_NODE_FAIL       (1<<4)
#define SERVERMODULE_NODE_NOFAILOVER (1<<5)

#define SERVERMODULE_CLUSTER_FLAG_NONE 0
#define SERVERMODULE_CLUSTER_FLAG_NO_FAILOVER (1<<1)
#define SERVERMODULE_CLUSTER_FLAG_NO_REDIRECTION (1<<2)

#define SERVERMODULE_NOT_USED(V) ((void) V)

/* Logging level strings */
#define SERVERMODULE_LOGLEVEL_DEBUG "debug"
#define SERVERMODULE_LOGLEVEL_VERBOSE "verbose"
#define SERVERMODULE_LOGLEVEL_NOTICE "notice"
#define SERVERMODULE_LOGLEVEL_WARNING "warning"

/* Bit flags for aux_save_triggers and the aux_load and aux_save callbacks */
#define SERVERMODULE_AUX_BEFORE_RDB (1<<0)
#define SERVERMODULE_AUX_AFTER_RDB (1<<1)

/* RM_Yield flags */
#define SERVERMODULE_YIELD_FLAG_NONE (1<<0)
#define SERVERMODULE_YIELD_FLAG_CLIENTS (1<<1)

/* RM_BlockClientOnKeysWithFlags flags */
#define SERVERMODULE_BLOCK_UNBLOCK_DEFAULT (0)
#define SERVERMODULE_BLOCK_UNBLOCK_DELETED (1<<0)

/* This type represents a timer handle, and is returned when a timer is
 * registered and used in order to invalidate a timer. It's just a 64 bit
 * number, because this is how each timer is represented inside the radix tree
 * of timers that are going to expire, sorted by expire time. */
typedef uint64_t RedisModuleTimerID;

/* CommandFilter Flags */

/* Do filter RedisModule_Call() commands initiated by module itself. */
#define SERVERMODULE_CMDFILTER_NOSELF    (1<<0)

/* Declare that the module can handle errors with RedisModule_SetModuleOptions. */
#define SERVERMODULE_OPTIONS_HANDLE_IO_ERRORS    (1<<0)

/* When set, Redis will not call RedisModule_SignalModifiedKey(), implicitly in
 * RedisModule_CloseKey, and the module needs to do that when manually when keys
 * are modified from the user's perspective, to invalidate WATCH. */
#define SERVERMODULE_OPTION_NO_IMPLICIT_SIGNAL_MODIFIED (1<<1)

/* Declare that the module can handle diskless async replication with RedisModule_SetModuleOptions. */
#define SERVERMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD    (1<<2)

/* Declare that the module want to get nested key space notifications.
 * If enabled, the module is responsible to break endless loop. */
#define SERVERMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS    (1<<3)

/* Next option flag, must be updated when adding new module flags above!
 * This flag should not be used directly by the module.
 * Use RedisModule_GetModuleOptionsAll instead. */
#define _SERVERMODULE_OPTIONS_FLAGS_NEXT (1<<4)

/* Definitions for RedisModule_SetCommandInfo. */

typedef enum {
    SERVERMODULE_ARG_TYPE_STRING,
    SERVERMODULE_ARG_TYPE_INTEGER,
    SERVERMODULE_ARG_TYPE_DOUBLE,
    SERVERMODULE_ARG_TYPE_KEY, /* A string, but represents a keyname */
    SERVERMODULE_ARG_TYPE_PATTERN,
    SERVERMODULE_ARG_TYPE_UNIX_TIME,
    SERVERMODULE_ARG_TYPE_PURE_TOKEN,
    SERVERMODULE_ARG_TYPE_ONEOF, /* Must have sub-arguments */
    SERVERMODULE_ARG_TYPE_BLOCK /* Must have sub-arguments */
} RedisModuleCommandArgType;

#define SERVERMODULE_CMD_ARG_NONE            (0)
#define SERVERMODULE_CMD_ARG_OPTIONAL        (1<<0) /* The argument is optional (like GET in SET command) */
#define SERVERMODULE_CMD_ARG_MULTIPLE        (1<<1) /* The argument may repeat itself (like key in DEL) */
#define SERVERMODULE_CMD_ARG_MULTIPLE_TOKEN  (1<<2) /* The argument may repeat itself, and so does its token (like `GET pattern` in SORT) */
#define _SERVERMODULE_CMD_ARG_NEXT           (1<<3)

typedef enum {
    SERVERMODULE_KSPEC_BS_INVALID = 0, /* Must be zero. An implicitly value of
                                       * zero is provided when the field is
                                       * absent in a struct literal. */
    SERVERMODULE_KSPEC_BS_UNKNOWN,
    SERVERMODULE_KSPEC_BS_INDEX,
    SERVERMODULE_KSPEC_BS_KEYWORD
} RedisModuleKeySpecBeginSearchType;

typedef enum {
    SERVERMODULE_KSPEC_FK_OMITTED = 0, /* Used when the field is absent in a
                                       * struct literal. Don't use this value
                                       * explicitly. */
    SERVERMODULE_KSPEC_FK_UNKNOWN,
    SERVERMODULE_KSPEC_FK_RANGE,
    SERVERMODULE_KSPEC_FK_KEYNUM
} RedisModuleKeySpecFindKeysType;

/* Key-spec flags. For details, see the documentation of
 * RedisModule_SetCommandInfo and the key-spec flags in server.h. */
#define SERVERMODULE_CMD_KEY_RO (1ULL<<0)
#define SERVERMODULE_CMD_KEY_RW (1ULL<<1)
#define SERVERMODULE_CMD_KEY_OW (1ULL<<2)
#define SERVERMODULE_CMD_KEY_RM (1ULL<<3)
#define SERVERMODULE_CMD_KEY_ACCESS (1ULL<<4)
#define SERVERMODULE_CMD_KEY_UPDATE (1ULL<<5)
#define SERVERMODULE_CMD_KEY_INSERT (1ULL<<6)
#define SERVERMODULE_CMD_KEY_DELETE (1ULL<<7)
#define SERVERMODULE_CMD_KEY_NOT_KEY (1ULL<<8)
#define SERVERMODULE_CMD_KEY_INCOMPLETE (1ULL<<9)
#define SERVERMODULE_CMD_KEY_VARIABLE_FLAGS (1ULL<<10)

/* Channel flags, for details see the documentation of
 * RedisModule_ChannelAtPosWithFlags. */
#define SERVERMODULE_CMD_CHANNEL_PATTERN (1ULL<<0)
#define SERVERMODULE_CMD_CHANNEL_PUBLISH (1ULL<<1)
#define SERVERMODULE_CMD_CHANNEL_SUBSCRIBE (1ULL<<2)
#define SERVERMODULE_CMD_CHANNEL_UNSUBSCRIBE (1ULL<<3)

typedef struct RedisModuleCommandArg {
    const char *name;
    RedisModuleCommandArgType type;
    int key_spec_index;       /* If type is KEY, this is a zero-based index of
                               * the key_spec in the command. For other types,
                               * you may specify -1. */
    const char *token;        /* If type is PURE_TOKEN, this is the token. */
    const char *summary;
    const char *since;
    int flags;                /* The SERVERMODULE_CMD_ARG_* macros. */
    const char *deprecated_since;
    struct RedisModuleCommandArg *subargs;
    const char *display_text;
} RedisModuleCommandArg;

typedef struct {
    const char *since;
    const char *changes;
} RedisModuleCommandHistoryEntry;

typedef struct {
    const char *notes;
    uint64_t flags; /* SERVERMODULE_CMD_KEY_* macros. */
    RedisModuleKeySpecBeginSearchType begin_search_type;
    union {
        struct {
            /* The index from which we start the search for keys */
            int pos;
        } index;
        struct {
            /* The keyword that indicates the beginning of key args */
            const char *keyword;
            /* An index in argv from which to start searching.
             * Can be negative, which means start search from the end, in reverse
             * (Example: -2 means to start in reverse from the penultimate arg) */
            int startfrom;
        } keyword;
    } bs;
    RedisModuleKeySpecFindKeysType find_keys_type;
    union {
        struct {
            /* Index of the last key relative to the result of the begin search
             * step. Can be negative, in which case it's not relative. -1
             * indicating till the last argument, -2 one before the last and so
             * on. */
            int lastkey;
            /* How many args should we skip after finding a key, in order to
             * find the next one. */
            int keystep;
            /* If lastkey is -1, we use limit to stop the search by a factor. 0
             * and 1 mean no limit. 2 means 1/2 of the remaining args, 3 means
             * 1/3, and so on. */
            int limit;
        } range;
        struct {
            /* Index of the argument containing the number of keys to come
             * relative to the result of the begin search step */
            int keynumidx;
            /* Index of the fist key. (Usually it's just after keynumidx, in
             * which case it should be set to keynumidx + 1.) */
            int firstkey;
            /* How many args should we skip after finding a key, in order to
             * find the next one, relative to the result of the begin search
             * step. */
            int keystep;
        } keynum;
    } fk;
} RedisModuleCommandKeySpec;

typedef struct {
    int version;
    size_t sizeof_historyentry;
    size_t sizeof_keyspec;
    size_t sizeof_arg;
} RedisModuleCommandInfoVersion;

static const RedisModuleCommandInfoVersion RedisModule_CurrentCommandInfoVersion = {
    .version = 1,
    .sizeof_historyentry = sizeof(RedisModuleCommandHistoryEntry),
    .sizeof_keyspec = sizeof(RedisModuleCommandKeySpec),
    .sizeof_arg = sizeof(RedisModuleCommandArg)
};

#define SERVERMODULE_COMMAND_INFO_VERSION (&RedisModule_CurrentCommandInfoVersion)

typedef struct {
    /* Always set version to SERVERMODULE_COMMAND_INFO_VERSION */
    const RedisModuleCommandInfoVersion *version;
    /* Version 1 fields (added in Redis 7.0.0) */
    const char *summary;          /* Summary of the command */
    const char *complexity;       /* Complexity description */
    const char *since;            /* Debut module version of the command */
    RedisModuleCommandHistoryEntry *history; /* History */
    /* A string of space-separated tips meant for clients/proxies regarding this
     * command */
    const char *tips;
    /* Number of arguments, it is possible to use -N to say >= N */
    int arity;
    RedisModuleCommandKeySpec *key_specs;
    RedisModuleCommandArg *args;
} RedisModuleCommandInfo;

/* Eventloop definitions. */
#define SERVERMODULE_EVENTLOOP_READABLE 1
#define SERVERMODULE_EVENTLOOP_WRITABLE 2
typedef void (*RedisModuleEventLoopFunc)(int fd, void *user_data, int mask);
typedef void (*RedisModuleEventLoopOneShotFunc)(void *user_data);

/* Server events definitions.
 * Those flags should not be used directly by the module, instead
 * the module should use RedisModuleEvent_* variables.
 * Note: This must be synced with moduleEventVersions */
#define SERVERMODULE_EVENT_REPLICATION_ROLE_CHANGED 0
#define SERVERMODULE_EVENT_PERSISTENCE 1
#define SERVERMODULE_EVENT_FLUSHDB 2
#define SERVERMODULE_EVENT_LOADING 3
#define SERVERMODULE_EVENT_CLIENT_CHANGE 4
#define SERVERMODULE_EVENT_SHUTDOWN 5
#define SERVERMODULE_EVENT_REPLICA_CHANGE 6
#define SERVERMODULE_EVENT_MASTER_LINK_CHANGE 7
#define SERVERMODULE_EVENT_CRON_LOOP 8
#define SERVERMODULE_EVENT_MODULE_CHANGE 9
#define SERVERMODULE_EVENT_LOADING_PROGRESS 10
#define SERVERMODULE_EVENT_SWAPDB 11
#define SERVERMODULE_EVENT_REPL_BACKUP 12 /* Deprecated since Redis 7.0, not used anymore. */
#define SERVERMODULE_EVENT_FORK_CHILD 13
#define SERVERMODULE_EVENT_REPL_ASYNC_LOAD 14
#define SERVERMODULE_EVENT_EVENTLOOP 15
#define SERVERMODULE_EVENT_CONFIG 16
#define SERVERMODULE_EVENT_KEY 17
#define _SERVERMODULE_EVENT_NEXT 18 /* Next event flag, should be updated if a new event added. */

typedef struct RedisModuleEvent {
    uint64_t id;        /* SERVERMODULE_EVENT_... defines. */
    uint64_t dataver;   /* Version of the structure we pass as 'data'. */
} RedisModuleEvent;

struct RedisModuleCtx;
struct RedisModuleDefragCtx;
typedef void (*RedisModuleEventCallback)(struct RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data);

/* IMPORTANT: When adding a new version of one of below structures that contain
 * event data (RedisModuleFlushInfoV1 for example) we have to avoid renaming the
 * old RedisModuleEvent structure.
 * For example, if we want to add RedisModuleFlushInfoV2, the RedisModuleEvent
 * structures should be:
 *      RedisModuleEvent_FlushDB = {
 *          SERVERMODULE_EVENT_FLUSHDB,
 *          1
 *      },
 *      RedisModuleEvent_FlushDBV2 = {
 *          SERVERMODULE_EVENT_FLUSHDB,
 *          2
 *      }
 * and NOT:
 *      RedisModuleEvent_FlushDBV1 = {
 *          SERVERMODULE_EVENT_FLUSHDB,
 *          1
 *      },
 *      RedisModuleEvent_FlushDB = {
 *          SERVERMODULE_EVENT_FLUSHDB,
 *          2
 *      }
 * The reason for that is forward-compatibility: We want that module that
 * compiled with a new redismodule.h to be able to work with a old server,
 * unless the author explicitly decided to use the newer event type.
 */
static const RedisModuleEvent
    RedisModuleEvent_ReplicationRoleChanged = {
        SERVERMODULE_EVENT_REPLICATION_ROLE_CHANGED,
        1
    },
    RedisModuleEvent_Persistence = {
        SERVERMODULE_EVENT_PERSISTENCE,
        1
    },
    RedisModuleEvent_FlushDB = {
        SERVERMODULE_EVENT_FLUSHDB,
        1
    },
    RedisModuleEvent_Loading = {
        SERVERMODULE_EVENT_LOADING,
        1
    },
    RedisModuleEvent_ClientChange = {
        SERVERMODULE_EVENT_CLIENT_CHANGE,
        1
    },
    RedisModuleEvent_Shutdown = {
        SERVERMODULE_EVENT_SHUTDOWN,
        1
    },
    RedisModuleEvent_ReplicaChange = {
        SERVERMODULE_EVENT_REPLICA_CHANGE,
        1
    },
    RedisModuleEvent_CronLoop = {
        SERVERMODULE_EVENT_CRON_LOOP,
        1
    },
    RedisModuleEvent_MasterLinkChange = {
        SERVERMODULE_EVENT_MASTER_LINK_CHANGE,
        1
    },
    RedisModuleEvent_ModuleChange = {
        SERVERMODULE_EVENT_MODULE_CHANGE,
        1
    },
    RedisModuleEvent_LoadingProgress = {
        SERVERMODULE_EVENT_LOADING_PROGRESS,
        1
    },
    RedisModuleEvent_SwapDB = {
        SERVERMODULE_EVENT_SWAPDB,
        1
    },
    /* Deprecated since Redis 7.0, not used anymore. */
    __attribute__ ((deprecated))
    RedisModuleEvent_ReplBackup = {
        SERVERMODULE_EVENT_REPL_BACKUP, 
        1
    },
    RedisModuleEvent_ReplAsyncLoad = {
        SERVERMODULE_EVENT_REPL_ASYNC_LOAD,
        1
    },
    RedisModuleEvent_ForkChild = {
        SERVERMODULE_EVENT_FORK_CHILD,
        1
    },
    RedisModuleEvent_EventLoop = {
        SERVERMODULE_EVENT_EVENTLOOP,
        1
    },
    RedisModuleEvent_Config = {
        SERVERMODULE_EVENT_CONFIG,
        1
    },
    RedisModuleEvent_Key = {
        SERVERMODULE_EVENT_KEY,
        1
    };

/* Those are values that are used for the 'subevent' callback argument. */
#define SERVERMODULE_SUBEVENT_PERSISTENCE_RDB_START 0
#define SERVERMODULE_SUBEVENT_PERSISTENCE_AOF_START 1
#define SERVERMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START 2
#define SERVERMODULE_SUBEVENT_PERSISTENCE_ENDED 3
#define SERVERMODULE_SUBEVENT_PERSISTENCE_FAILED 4
#define SERVERMODULE_SUBEVENT_PERSISTENCE_SYNC_AOF_START 5
#define _SERVERMODULE_SUBEVENT_PERSISTENCE_NEXT 6

#define SERVERMODULE_SUBEVENT_LOADING_RDB_START 0
#define SERVERMODULE_SUBEVENT_LOADING_AOF_START 1
#define SERVERMODULE_SUBEVENT_LOADING_REPL_START 2
#define SERVERMODULE_SUBEVENT_LOADING_ENDED 3
#define SERVERMODULE_SUBEVENT_LOADING_FAILED 4
#define _SERVERMODULE_SUBEVENT_LOADING_NEXT 5

#define SERVERMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED 0
#define SERVERMODULE_SUBEVENT_CLIENT_CHANGE_DISCONNECTED 1
#define _SERVERMODULE_SUBEVENT_CLIENT_CHANGE_NEXT 2

#define SERVERMODULE_SUBEVENT_MASTER_LINK_UP 0
#define SERVERMODULE_SUBEVENT_MASTER_LINK_DOWN 1
#define _SERVERMODULE_SUBEVENT_MASTER_NEXT 2

#define SERVERMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE 0
#define SERVERMODULE_SUBEVENT_REPLICA_CHANGE_OFFLINE 1
#define _SERVERMODULE_SUBEVENT_REPLICA_CHANGE_NEXT 2

#define SERVERMODULE_EVENT_REPLROLECHANGED_NOW_MASTER 0
#define SERVERMODULE_EVENT_REPLROLECHANGED_NOW_REPLICA 1
#define _SERVERMODULE_EVENT_REPLROLECHANGED_NEXT 2

#define SERVERMODULE_SUBEVENT_FLUSHDB_START 0
#define SERVERMODULE_SUBEVENT_FLUSHDB_END 1
#define _SERVERMODULE_SUBEVENT_FLUSHDB_NEXT 2

#define SERVERMODULE_SUBEVENT_MODULE_LOADED 0
#define SERVERMODULE_SUBEVENT_MODULE_UNLOADED 1
#define _SERVERMODULE_SUBEVENT_MODULE_NEXT 2

#define SERVERMODULE_SUBEVENT_CONFIG_CHANGE 0
#define _SERVERMODULE_SUBEVENT_CONFIG_NEXT 1

#define SERVERMODULE_SUBEVENT_LOADING_PROGRESS_RDB 0
#define SERVERMODULE_SUBEVENT_LOADING_PROGRESS_AOF 1
#define _SERVERMODULE_SUBEVENT_LOADING_PROGRESS_NEXT 2

/* Replication Backup events are deprecated since Redis 7.0 and are never fired. */
#define SERVERMODULE_SUBEVENT_REPL_BACKUP_CREATE 0
#define SERVERMODULE_SUBEVENT_REPL_BACKUP_RESTORE 1
#define SERVERMODULE_SUBEVENT_REPL_BACKUP_DISCARD 2
#define _SERVERMODULE_SUBEVENT_REPL_BACKUP_NEXT 3

#define SERVERMODULE_SUBEVENT_REPL_ASYNC_LOAD_STARTED 0
#define SERVERMODULE_SUBEVENT_REPL_ASYNC_LOAD_ABORTED 1
#define SERVERMODULE_SUBEVENT_REPL_ASYNC_LOAD_COMPLETED 2
#define _SERVERMODULE_SUBEVENT_REPL_ASYNC_LOAD_NEXT 3

#define SERVERMODULE_SUBEVENT_FORK_CHILD_BORN 0
#define SERVERMODULE_SUBEVENT_FORK_CHILD_DIED 1
#define _SERVERMODULE_SUBEVENT_FORK_CHILD_NEXT 2

#define SERVERMODULE_SUBEVENT_EVENTLOOP_BEFORE_SLEEP 0
#define SERVERMODULE_SUBEVENT_EVENTLOOP_AFTER_SLEEP 1
#define _SERVERMODULE_SUBEVENT_EVENTLOOP_NEXT 2

#define SERVERMODULE_SUBEVENT_KEY_DELETED 0
#define SERVERMODULE_SUBEVENT_KEY_EXPIRED 1
#define SERVERMODULE_SUBEVENT_KEY_EVICTED 2
#define SERVERMODULE_SUBEVENT_KEY_OVERWRITTEN 3
#define _SERVERMODULE_SUBEVENT_KEY_NEXT 4

#define _SERVERMODULE_SUBEVENT_SHUTDOWN_NEXT 0
#define _SERVERMODULE_SUBEVENT_CRON_LOOP_NEXT 0
#define _SERVERMODULE_SUBEVENT_SWAPDB_NEXT 0

/* RedisModuleClientInfo flags. */
#define SERVERMODULE_CLIENTINFO_FLAG_SSL (1<<0)
#define SERVERMODULE_CLIENTINFO_FLAG_PUBSUB (1<<1)
#define SERVERMODULE_CLIENTINFO_FLAG_BLOCKED (1<<2)
#define SERVERMODULE_CLIENTINFO_FLAG_TRACKING (1<<3)
#define SERVERMODULE_CLIENTINFO_FLAG_UNIXSOCKET (1<<4)
#define SERVERMODULE_CLIENTINFO_FLAG_MULTI (1<<5)

/* Here we take all the structures that the module pass to the core
 * and the other way around. Notably the list here contains the structures
 * used by the hooks API RedisModule_RegisterToServerEvent().
 *
 * The structures always start with a 'version' field. This is useful
 * when we want to pass a reference to the structure to the core APIs,
 * for the APIs to fill the structure. In that case, the structure 'version'
 * field is initialized before passing it to the core, so that the core is
 * able to cast the pointer to the appropriate structure version. In this
 * way we obtain ABI compatibility.
 *
 * Here we'll list all the structure versions in case they evolve over time,
 * however using a define, we'll make sure to use the last version as the
 * public name for the module to use. */

#define SERVERMODULE_CLIENTINFO_VERSION 1
typedef struct RedisModuleClientInfo {
    uint64_t version;       /* Version of this structure for ABI compat. */
    uint64_t flags;         /* SERVERMODULE_CLIENTINFO_FLAG_* */
    uint64_t id;            /* Client ID. */
    char addr[46];          /* IPv4 or IPv6 address. */
    uint16_t port;          /* TCP port. */
    uint16_t db;            /* Selected DB. */
} RedisModuleClientInfoV1;

#define RedisModuleClientInfo RedisModuleClientInfoV1

#define SERVERMODULE_CLIENTINFO_INITIALIZER_V1 { .version = 1 }

#define SERVERMODULE_REPLICATIONINFO_VERSION 1
typedef struct RedisModuleReplicationInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    int master;             /* true if master, false if replica */
    char *masterhost;       /* master instance hostname for NOW_REPLICA */
    int masterport;         /* master instance port for NOW_REPLICA */
    char *replid1;          /* Main replication ID */
    char *replid2;          /* Secondary replication ID */
    uint64_t repl1_offset;  /* Main replication offset */
    uint64_t repl2_offset;  /* Offset of replid2 validity */
} RedisModuleReplicationInfoV1;

#define RedisModuleReplicationInfo RedisModuleReplicationInfoV1

#define SERVERMODULE_FLUSHINFO_VERSION 1
typedef struct RedisModuleFlushInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    int32_t sync;           /* Synchronous or threaded flush?. */
    int32_t dbnum;          /* Flushed database number, -1 for ALL. */
} RedisModuleFlushInfoV1;

#define RedisModuleFlushInfo RedisModuleFlushInfoV1

#define SERVERMODULE_MODULE_CHANGE_VERSION 1
typedef struct RedisModuleModuleChange {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    const char* module_name;/* Name of module loaded or unloaded. */
    int32_t module_version; /* Module version. */
} RedisModuleModuleChangeV1;

#define RedisModuleModuleChange RedisModuleModuleChangeV1

#define SERVERMODULE_CONFIGCHANGE_VERSION 1
typedef struct RedisModuleConfigChange {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    uint32_t num_changes;   /* how many redis config options were changed */
    const char **config_names; /* the config names that were changed */
} RedisModuleConfigChangeV1;

#define RedisModuleConfigChange RedisModuleConfigChangeV1

#define SERVERMODULE_CRON_LOOP_VERSION 1
typedef struct RedisModuleCronLoopInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    int32_t hz;             /* Approximate number of events per second. */
} RedisModuleCronLoopV1;

#define RedisModuleCronLoop RedisModuleCronLoopV1

#define SERVERMODULE_LOADING_PROGRESS_VERSION 1
typedef struct RedisModuleLoadingProgressInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    int32_t hz;             /* Approximate number of events per second. */
    int32_t progress;       /* Approximate progress between 0 and 1024, or -1
                             * if unknown. */
} RedisModuleLoadingProgressV1;

#define RedisModuleLoadingProgress RedisModuleLoadingProgressV1

#define SERVERMODULE_SWAPDBINFO_VERSION 1
typedef struct RedisModuleSwapDbInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    int32_t dbnum_first;    /* Swap Db first dbnum */
    int32_t dbnum_second;   /* Swap Db second dbnum */
} RedisModuleSwapDbInfoV1;

#define RedisModuleSwapDbInfo RedisModuleSwapDbInfoV1

#define SERVERMODULE_KEYINFO_VERSION 1
typedef struct RedisModuleKeyInfo {
    uint64_t version;       /* Not used since this structure is never passed
                               from the module to the core right now. Here
                               for future compatibility. */
    RedisModuleKey *key;    /* Opened key. */
} RedisModuleKeyInfoV1;

#define RedisModuleKeyInfo RedisModuleKeyInfoV1

typedef enum {
    SERVERMODULE_ACL_LOG_AUTH = 0, /* Authentication failure */
    SERVERMODULE_ACL_LOG_CMD, /* Command authorization failure */
    SERVERMODULE_ACL_LOG_KEY, /* Key authorization failure */
    SERVERMODULE_ACL_LOG_CHANNEL /* Channel authorization failure */
} RedisModuleACLLogEntryReason;

/* Incomplete structures needed by both the core and modules. */
typedef struct RedisModuleIO RedisModuleIO;
typedef struct RedisModuleDigest RedisModuleDigest;
typedef struct RedisModuleInfoCtx RedisModuleInfoCtx;
typedef struct RedisModuleDefragCtx RedisModuleDefragCtx;

/* Function pointers needed by both the core and modules, these needs to be
 * exposed since you can't cast a function pointer to (void *). */
typedef void (*RedisModuleInfoFunc)(RedisModuleInfoCtx *ctx, int for_crash_report);
typedef void (*RedisModuleDefragFunc)(RedisModuleDefragCtx *ctx);
typedef void (*RedisModuleUserChangedFunc) (uint64_t client_id, void *privdata);

/* ------------------------- End of common defines ------------------------ */

/* ----------- The rest of the defines are only for modules ----------------- */
#if !defined SERVERMODULE_CORE || defined SERVERMODULE_CORE_MODULE
/* Things defined for modules and core-modules. */

/* Macro definitions specific to individual compilers */
#ifndef SERVERMODULE_ATTR_UNUSED
#    ifdef __GNUC__
#        define SERVERMODULE_ATTR_UNUSED __attribute__((unused))
#    else
#        define SERVERMODULE_ATTR_UNUSED
#    endif
#endif

#ifndef SERVERMODULE_ATTR_PRINTF
#    ifdef __GNUC__
#        define SERVERMODULE_ATTR_PRINTF(idx,cnt) __attribute__((format(printf,idx,cnt)))
#    else
#        define SERVERMODULE_ATTR_PRINTF(idx,cnt)
#    endif
#endif

#ifndef SERVERMODULE_ATTR_COMMON
#    if defined(__GNUC__) && !(defined(__clang__) && defined(__cplusplus))
#        define SERVERMODULE_ATTR_COMMON __attribute__((__common__))
#    else
#        define SERVERMODULE_ATTR_COMMON
#    endif
#endif

/* Incomplete structures for compiler checks but opaque access. */
typedef struct RedisModuleCtx RedisModuleCtx;
typedef struct RedisModuleCommand RedisModuleCommand;
typedef struct RedisModuleCallReply RedisModuleCallReply;
typedef struct RedisModuleType RedisModuleType;
typedef struct RedisModuleBlockedClient RedisModuleBlockedClient;
typedef struct RedisModuleClusterInfo RedisModuleClusterInfo;
typedef struct RedisModuleDict RedisModuleDict;
typedef struct RedisModuleDictIter RedisModuleDictIter;
typedef struct RedisModuleCommandFilterCtx RedisModuleCommandFilterCtx;
typedef struct RedisModuleCommandFilter RedisModuleCommandFilter;
typedef struct RedisModuleServerInfoData RedisModuleServerInfoData;
typedef struct RedisModuleScanCursor RedisModuleScanCursor;
typedef struct RedisModuleUser RedisModuleUser;
typedef struct RedisModuleKeyOptCtx RedisModuleKeyOptCtx;
typedef struct RedisModuleRdbStream RedisModuleRdbStream;

typedef int (*RedisModuleCmdFunc)(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
typedef void (*RedisModuleDisconnectFunc)(RedisModuleCtx *ctx, RedisModuleBlockedClient *bc);
typedef int (*RedisModuleNotificationFunc)(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key);
typedef void (*RedisModulePostNotificationJobFunc) (RedisModuleCtx *ctx, void *pd);
typedef void *(*RedisModuleTypeLoadFunc)(RedisModuleIO *rdb, int encver);
typedef void (*RedisModuleTypeSaveFunc)(RedisModuleIO *rdb, void *value);
typedef int (*RedisModuleTypeAuxLoadFunc)(RedisModuleIO *rdb, int encver, int when);
typedef void (*RedisModuleTypeAuxSaveFunc)(RedisModuleIO *rdb, int when);
typedef void (*RedisModuleTypeRewriteFunc)(RedisModuleIO *aof, RedisModuleString *key, void *value);
typedef size_t (*RedisModuleTypeMemUsageFunc)(const void *value);
typedef size_t (*RedisModuleTypeMemUsageFunc2)(RedisModuleKeyOptCtx *ctx, const void *value, size_t sample_size);
typedef void (*RedisModuleTypeDigestFunc)(RedisModuleDigest *digest, void *value);
typedef void (*RedisModuleTypeFreeFunc)(void *value);
typedef size_t (*RedisModuleTypeFreeEffortFunc)(RedisModuleString *key, const void *value);
typedef size_t (*RedisModuleTypeFreeEffortFunc2)(RedisModuleKeyOptCtx *ctx, const void *value);
typedef void (*RedisModuleTypeUnlinkFunc)(RedisModuleString *key, const void *value);
typedef void (*RedisModuleTypeUnlinkFunc2)(RedisModuleKeyOptCtx *ctx, const void *value);
typedef void *(*RedisModuleTypeCopyFunc)(RedisModuleString *fromkey, RedisModuleString *tokey, const void *value);
typedef void *(*RedisModuleTypeCopyFunc2)(RedisModuleKeyOptCtx *ctx, const void *value);
typedef int (*RedisModuleTypeDefragFunc)(RedisModuleDefragCtx *ctx, RedisModuleString *key, void **value);
typedef void (*RedisModuleClusterMessageReceiver)(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len);
typedef void (*RedisModuleTimerProc)(RedisModuleCtx *ctx, void *data);
typedef void (*RedisModuleCommandFilterFunc) (RedisModuleCommandFilterCtx *filter);
typedef void (*RedisModuleForkDoneHandler) (int exitcode, int bysignal, void *user_data);
typedef void (*RedisModuleScanCB)(RedisModuleCtx *ctx, RedisModuleString *keyname, RedisModuleKey *key, void *privdata);
typedef void (*RedisModuleScanKeyCB)(RedisModuleKey *key, RedisModuleString *field, RedisModuleString *value, void *privdata);
typedef RedisModuleString * (*RedisModuleConfigGetStringFunc)(const char *name, void *privdata);
typedef long long (*RedisModuleConfigGetNumericFunc)(const char *name, void *privdata);
typedef int (*RedisModuleConfigGetBoolFunc)(const char *name, void *privdata);
typedef int (*RedisModuleConfigGetEnumFunc)(const char *name, void *privdata);
typedef int (*RedisModuleConfigSetStringFunc)(const char *name, RedisModuleString *val, void *privdata, RedisModuleString **err);
typedef int (*RedisModuleConfigSetNumericFunc)(const char *name, long long val, void *privdata, RedisModuleString **err);
typedef int (*RedisModuleConfigSetBoolFunc)(const char *name, int val, void *privdata, RedisModuleString **err);
typedef int (*RedisModuleConfigSetEnumFunc)(const char *name, int val, void *privdata, RedisModuleString **err);
typedef int (*RedisModuleConfigApplyFunc)(RedisModuleCtx *ctx, void *privdata, RedisModuleString **err);
typedef void (*RedisModuleOnUnblocked)(RedisModuleCtx *ctx, RedisModuleCallReply *reply, void *private_data);
typedef int (*RedisModuleAuthCallback)(RedisModuleCtx *ctx, RedisModuleString *username, RedisModuleString *password, RedisModuleString **err);

typedef struct RedisModuleTypeMethods {
    uint64_t version;
    RedisModuleTypeLoadFunc rdb_load;
    RedisModuleTypeSaveFunc rdb_save;
    RedisModuleTypeRewriteFunc aof_rewrite;
    RedisModuleTypeMemUsageFunc mem_usage;
    RedisModuleTypeDigestFunc digest;
    RedisModuleTypeFreeFunc free;
    RedisModuleTypeAuxLoadFunc aux_load;
    RedisModuleTypeAuxSaveFunc aux_save;
    int aux_save_triggers;
    RedisModuleTypeFreeEffortFunc free_effort;
    RedisModuleTypeUnlinkFunc unlink;
    RedisModuleTypeCopyFunc copy;
    RedisModuleTypeDefragFunc defrag;
    RedisModuleTypeMemUsageFunc2 mem_usage2;
    RedisModuleTypeFreeEffortFunc2 free_effort2;
    RedisModuleTypeUnlinkFunc2 unlink2;
    RedisModuleTypeCopyFunc2 copy2;
    RedisModuleTypeAuxSaveFunc aux_save2;
} RedisModuleTypeMethods;

#define SERVERMODULE_GET_API(name) \
    RedisModule_GetApi("RedisModule_" #name, ((void **)&RedisModule_ ## name))

/* Default API declaration prefix (not 'extern' for backwards compatibility) */
#ifndef SERVERMODULE_API
#define SERVERMODULE_API
#endif

/* Default API declaration suffix (compiler attributes) */
#ifndef SERVERMODULE_ATTR
#define SERVERMODULE_ATTR SERVERMODULE_ATTR_COMMON
#endif

SERVERMODULE_API void * (*RedisModule_Alloc)(size_t bytes) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_TryAlloc)(size_t bytes) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_Realloc)(void *ptr, size_t bytes) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_TryRealloc)(void *ptr, size_t bytes) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_Free)(void *ptr) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_Calloc)(size_t nmemb, size_t size) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_TryCalloc)(size_t nmemb, size_t size) SERVERMODULE_ATTR;
SERVERMODULE_API char * (*RedisModule_Strdup)(const char *str) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetApi)(const char *, void *) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_CreateCommand)(RedisModuleCtx *ctx, const char *name, RedisModuleCmdFunc cmdfunc, const char *strflags, int firstkey, int lastkey, int keystep) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleCommand *(*RedisModule_GetCommand)(RedisModuleCtx *ctx, const char *name) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_CreateSubcommand)(RedisModuleCommand *parent, const char *name, RedisModuleCmdFunc cmdfunc, const char *strflags, int firstkey, int lastkey, int keystep) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SetCommandInfo)(RedisModuleCommand *command, const RedisModuleCommandInfo *info) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SetCommandACLCategories)(RedisModuleCommand *command, const char *ctgrsflags) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_AddACLCategory)(RedisModuleCtx *ctx, const char *name) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SetModuleAttribs)(RedisModuleCtx *ctx, const char *name, int ver, int apiver) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_IsModuleNameBusy)(const char *name) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_WrongArity)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithLongLong)(RedisModuleCtx *ctx, long long ll) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetSelectedDb)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SelectDb)(RedisModuleCtx *ctx, int newid) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_KeyExists)(RedisModuleCtx *ctx, RedisModuleString *keyname) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleKey * (*RedisModule_OpenKey)(RedisModuleCtx *ctx, RedisModuleString *keyname, int mode) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetOpenKeyModesAll)(void) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_CloseKey)(RedisModuleKey *kp) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_KeyType)(RedisModuleKey *kp) SERVERMODULE_ATTR;
SERVERMODULE_API size_t (*RedisModule_ValueLength)(RedisModuleKey *kp) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ListPush)(RedisModuleKey *kp, int where, RedisModuleString *ele) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_ListPop)(RedisModuleKey *key, int where) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_ListGet)(RedisModuleKey *key, long index) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ListSet)(RedisModuleKey *key, long index, RedisModuleString *value) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ListInsert)(RedisModuleKey *key, long index, RedisModuleString *value) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ListDelete)(RedisModuleKey *key, long index) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleCallReply * (*RedisModule_Call)(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...) SERVERMODULE_ATTR;
SERVERMODULE_API const char * (*RedisModule_CallReplyProto)(RedisModuleCallReply *reply, size_t *len) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_FreeCallReply)(RedisModuleCallReply *reply) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_CallReplyType)(RedisModuleCallReply *reply) SERVERMODULE_ATTR;
SERVERMODULE_API long long (*RedisModule_CallReplyInteger)(RedisModuleCallReply *reply) SERVERMODULE_ATTR;
SERVERMODULE_API double (*RedisModule_CallReplyDouble)(RedisModuleCallReply *reply) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_CallReplyBool)(RedisModuleCallReply *reply) SERVERMODULE_ATTR;
SERVERMODULE_API const char* (*RedisModule_CallReplyBigNumber)(RedisModuleCallReply *reply, size_t *len) SERVERMODULE_ATTR;
SERVERMODULE_API const char* (*RedisModule_CallReplyVerbatim)(RedisModuleCallReply *reply, size_t *len, const char **format) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleCallReply * (*RedisModule_CallReplySetElement)(RedisModuleCallReply *reply, size_t idx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_CallReplyMapElement)(RedisModuleCallReply *reply, size_t idx, RedisModuleCallReply **key, RedisModuleCallReply **val) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_CallReplyAttributeElement)(RedisModuleCallReply *reply, size_t idx, RedisModuleCallReply **key, RedisModuleCallReply **val) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_CallReplyPromiseSetUnblockHandler)(RedisModuleCallReply *reply, RedisModuleOnUnblocked on_unblock, void *private_data) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_CallReplyPromiseAbort)(RedisModuleCallReply *reply, void **private_data) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleCallReply * (*RedisModule_CallReplyAttribute)(RedisModuleCallReply *reply) SERVERMODULE_ATTR;
SERVERMODULE_API size_t (*RedisModule_CallReplyLength)(RedisModuleCallReply *reply) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleCallReply * (*RedisModule_CallReplyArrayElement)(RedisModuleCallReply *reply, size_t idx) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_CreateString)(RedisModuleCtx *ctx, const char *ptr, size_t len) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_CreateStringFromLongLong)(RedisModuleCtx *ctx, long long ll) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_CreateStringFromULongLong)(RedisModuleCtx *ctx, unsigned long long ull) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_CreateStringFromDouble)(RedisModuleCtx *ctx, double d) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_CreateStringFromLongDouble)(RedisModuleCtx *ctx, long double ld, int humanfriendly) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_CreateStringFromString)(RedisModuleCtx *ctx, const RedisModuleString *str) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_CreateStringFromStreamID)(RedisModuleCtx *ctx, const RedisModuleStreamID *id) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_CreateStringPrintf)(RedisModuleCtx *ctx, const char *fmt, ...) SERVERMODULE_ATTR_PRINTF(2,3) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_FreeString)(RedisModuleCtx *ctx, RedisModuleString *str) SERVERMODULE_ATTR;
SERVERMODULE_API const char * (*RedisModule_StringPtrLen)(const RedisModuleString *str, size_t *len) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithError)(RedisModuleCtx *ctx, const char *err) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithErrorFormat)(RedisModuleCtx *ctx, const char *fmt, ...) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithSimpleString)(RedisModuleCtx *ctx, const char *msg) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithArray)(RedisModuleCtx *ctx, long len) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithMap)(RedisModuleCtx *ctx, long len) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithSet)(RedisModuleCtx *ctx, long len) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithAttribute)(RedisModuleCtx *ctx, long len) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithNullArray)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithEmptyArray)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ReplySetArrayLength)(RedisModuleCtx *ctx, long len) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ReplySetMapLength)(RedisModuleCtx *ctx, long len) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ReplySetSetLength)(RedisModuleCtx *ctx, long len) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ReplySetAttributeLength)(RedisModuleCtx *ctx, long len) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ReplySetPushLength)(RedisModuleCtx *ctx, long len) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithStringBuffer)(RedisModuleCtx *ctx, const char *buf, size_t len) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithCString)(RedisModuleCtx *ctx, const char *buf) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithString)(RedisModuleCtx *ctx, RedisModuleString *str) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithEmptyString)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithVerbatimString)(RedisModuleCtx *ctx, const char *buf, size_t len) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithVerbatimStringType)(RedisModuleCtx *ctx, const char *buf, size_t len, const char *ext) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithNull)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithBool)(RedisModuleCtx *ctx, int b) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithLongDouble)(RedisModuleCtx *ctx, long double d) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithDouble)(RedisModuleCtx *ctx, double d) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithBigNumber)(RedisModuleCtx *ctx, const char *bignum, size_t len) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplyWithCallReply)(RedisModuleCtx *ctx, RedisModuleCallReply *reply) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StringToLongLong)(const RedisModuleString *str, long long *ll) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StringToULongLong)(const RedisModuleString *str, unsigned long long *ull) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StringToDouble)(const RedisModuleString *str, double *d) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StringToLongDouble)(const RedisModuleString *str, long double *d) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StringToStreamID)(const RedisModuleString *str, RedisModuleStreamID *id) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_AutoMemory)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_Replicate)(RedisModuleCtx *ctx, const char *cmdname, const char *fmt, ...) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ReplicateVerbatim)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API const char * (*RedisModule_CallReplyStringPtr)(RedisModuleCallReply *reply, size_t *len) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_CreateStringFromCallReply)(RedisModuleCallReply *reply) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DeleteKey)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_UnlinkKey)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StringSet)(RedisModuleKey *key, RedisModuleString *str) SERVERMODULE_ATTR;
SERVERMODULE_API char * (*RedisModule_StringDMA)(RedisModuleKey *key, size_t *len, int mode) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StringTruncate)(RedisModuleKey *key, size_t newlen) SERVERMODULE_ATTR;
SERVERMODULE_API mstime_t (*RedisModule_GetExpire)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SetExpire)(RedisModuleKey *key, mstime_t expire) SERVERMODULE_ATTR;
SERVERMODULE_API mstime_t (*RedisModule_GetAbsExpire)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SetAbsExpire)(RedisModuleKey *key, mstime_t expire) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ResetDataset)(int restart_aof, int async) SERVERMODULE_ATTR;
SERVERMODULE_API unsigned long long (*RedisModule_DbSize)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_RandomKey)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ZsetAdd)(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ZsetIncrby)(RedisModuleKey *key, double score, RedisModuleString *ele, int *flagsptr, double *newscore) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ZsetScore)(RedisModuleKey *key, RedisModuleString *ele, double *score) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ZsetRem)(RedisModuleKey *key, RedisModuleString *ele, int *deleted) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ZsetRangeStop)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ZsetFirstInScoreRange)(RedisModuleKey *key, double min, double max, int minex, int maxex) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ZsetLastInScoreRange)(RedisModuleKey *key, double min, double max, int minex, int maxex) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ZsetFirstInLexRange)(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ZsetLastInLexRange)(RedisModuleKey *key, RedisModuleString *min, RedisModuleString *max) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_ZsetRangeCurrentElement)(RedisModuleKey *key, double *score) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ZsetRangeNext)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ZsetRangePrev)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ZsetRangeEndReached)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_HashSet)(RedisModuleKey *key, int flags, ...) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_HashGet)(RedisModuleKey *key, int flags, ...) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StreamAdd)(RedisModuleKey *key, int flags, RedisModuleStreamID *id, RedisModuleString **argv, int64_t numfields) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StreamDelete)(RedisModuleKey *key, RedisModuleStreamID *id) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StreamIteratorStart)(RedisModuleKey *key, int flags, RedisModuleStreamID *startid, RedisModuleStreamID *endid) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StreamIteratorStop)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StreamIteratorNextID)(RedisModuleKey *key, RedisModuleStreamID *id, long *numfields) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StreamIteratorNextField)(RedisModuleKey *key, RedisModuleString **field_ptr, RedisModuleString **value_ptr) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StreamIteratorDelete)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API long long (*RedisModule_StreamTrimByLength)(RedisModuleKey *key, int flags, long long length) SERVERMODULE_ATTR;
SERVERMODULE_API long long (*RedisModule_StreamTrimByID)(RedisModuleKey *key, int flags, RedisModuleStreamID *id) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_IsKeysPositionRequest)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_KeyAtPos)(RedisModuleCtx *ctx, int pos) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_KeyAtPosWithFlags)(RedisModuleCtx *ctx, int pos, int flags) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_IsChannelsPositionRequest)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ChannelAtPosWithFlags)(RedisModuleCtx *ctx, int pos, int flags) SERVERMODULE_ATTR;
SERVERMODULE_API unsigned long long (*RedisModule_GetClientId)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_GetClientUserNameById)(RedisModuleCtx *ctx, uint64_t id) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetClientInfoById)(void *ci, uint64_t id) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_GetClientNameById)(RedisModuleCtx *ctx, uint64_t id) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SetClientNameById)(uint64_t id, RedisModuleString *name) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_PublishMessage)(RedisModuleCtx *ctx, RedisModuleString *channel, RedisModuleString *message) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_PublishMessageShard)(RedisModuleCtx *ctx, RedisModuleString *channel, RedisModuleString *message) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetContextFlags)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_AvoidReplicaTraffic)(void) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_PoolAlloc)(RedisModuleCtx *ctx, size_t bytes) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleType * (*RedisModule_CreateDataType)(RedisModuleCtx *ctx, const char *name, int encver, RedisModuleTypeMethods *typemethods) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ModuleTypeSetValue)(RedisModuleKey *key, RedisModuleType *mt, void *value) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ModuleTypeReplaceValue)(RedisModuleKey *key, RedisModuleType *mt, void *new_value, void **old_value) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleType * (*RedisModule_ModuleTypeGetType)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_ModuleTypeGetValue)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_IsIOError)(RedisModuleIO *io) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SetModuleOptions)(RedisModuleCtx *ctx, int options) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SignalModifiedKey)(RedisModuleCtx *ctx, RedisModuleString *keyname) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SaveUnsigned)(RedisModuleIO *io, uint64_t value) SERVERMODULE_ATTR;
SERVERMODULE_API uint64_t (*RedisModule_LoadUnsigned)(RedisModuleIO *io) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SaveSigned)(RedisModuleIO *io, int64_t value) SERVERMODULE_ATTR;
SERVERMODULE_API int64_t (*RedisModule_LoadSigned)(RedisModuleIO *io) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_EmitAOF)(RedisModuleIO *io, const char *cmdname, const char *fmt, ...) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SaveString)(RedisModuleIO *io, RedisModuleString *s) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SaveStringBuffer)(RedisModuleIO *io, const char *str, size_t len) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_LoadString)(RedisModuleIO *io) SERVERMODULE_ATTR;
SERVERMODULE_API char * (*RedisModule_LoadStringBuffer)(RedisModuleIO *io, size_t *lenptr) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SaveDouble)(RedisModuleIO *io, double value) SERVERMODULE_ATTR;
SERVERMODULE_API double (*RedisModule_LoadDouble)(RedisModuleIO *io) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SaveFloat)(RedisModuleIO *io, float value) SERVERMODULE_ATTR;
SERVERMODULE_API float (*RedisModule_LoadFloat)(RedisModuleIO *io) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SaveLongDouble)(RedisModuleIO *io, long double value) SERVERMODULE_ATTR;
SERVERMODULE_API long double (*RedisModule_LoadLongDouble)(RedisModuleIO *io) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_LoadDataTypeFromString)(const RedisModuleString *str, const RedisModuleType *mt) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_LoadDataTypeFromStringEncver)(const RedisModuleString *str, const RedisModuleType *mt, int encver) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_SaveDataTypeToString)(RedisModuleCtx *ctx, void *data, const RedisModuleType *mt) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_Log)(RedisModuleCtx *ctx, const char *level, const char *fmt, ...) SERVERMODULE_ATTR SERVERMODULE_ATTR_PRINTF(3,4);
SERVERMODULE_API void (*RedisModule_LogIOError)(RedisModuleIO *io, const char *levelstr, const char *fmt, ...) SERVERMODULE_ATTR SERVERMODULE_ATTR_PRINTF(3,4);
SERVERMODULE_API void (*RedisModule__Assert)(const char *estr, const char *file, int line) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_LatencyAddSample)(const char *event, mstime_t latency) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StringAppendBuffer)(RedisModuleCtx *ctx, RedisModuleString *str, const char *buf, size_t len) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_TrimStringAllocation)(RedisModuleString *str) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_RetainString)(RedisModuleCtx *ctx, RedisModuleString *str) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_HoldString)(RedisModuleCtx *ctx, RedisModuleString *str) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StringCompare)(const RedisModuleString *a, const RedisModuleString *b) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleCtx * (*RedisModule_GetContextFromIO)(RedisModuleIO *io) SERVERMODULE_ATTR;
SERVERMODULE_API const RedisModuleString * (*RedisModule_GetKeyNameFromIO)(RedisModuleIO *io) SERVERMODULE_ATTR;
SERVERMODULE_API const RedisModuleString * (*RedisModule_GetKeyNameFromModuleKey)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetDbIdFromModuleKey)(RedisModuleKey *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetDbIdFromIO)(RedisModuleIO *io) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetDbIdFromOptCtx)(RedisModuleKeyOptCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetToDbIdFromOptCtx)(RedisModuleKeyOptCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API const RedisModuleString * (*RedisModule_GetKeyNameFromOptCtx)(RedisModuleKeyOptCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API const RedisModuleString * (*RedisModule_GetToKeyNameFromOptCtx)(RedisModuleKeyOptCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API mstime_t (*RedisModule_Milliseconds)(void) SERVERMODULE_ATTR;
SERVERMODULE_API uint64_t (*RedisModule_MonotonicMicroseconds)(void) SERVERMODULE_ATTR;
SERVERMODULE_API ustime_t (*RedisModule_Microseconds)(void) SERVERMODULE_ATTR;
SERVERMODULE_API ustime_t (*RedisModule_CachedMicroseconds)(void) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_DigestAddStringBuffer)(RedisModuleDigest *md, const char *ele, size_t len) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_DigestAddLongLong)(RedisModuleDigest *md, long long ele) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_DigestEndSequence)(RedisModuleDigest *md) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetDbIdFromDigest)(RedisModuleDigest *dig) SERVERMODULE_ATTR;
SERVERMODULE_API const RedisModuleString * (*RedisModule_GetKeyNameFromDigest)(RedisModuleDigest *dig) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleDict * (*RedisModule_CreateDict)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_FreeDict)(RedisModuleCtx *ctx, RedisModuleDict *d) SERVERMODULE_ATTR;
SERVERMODULE_API uint64_t (*RedisModule_DictSize)(RedisModuleDict *d) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DictSetC)(RedisModuleDict *d, void *key, size_t keylen, void *ptr) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DictReplaceC)(RedisModuleDict *d, void *key, size_t keylen, void *ptr) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DictSet)(RedisModuleDict *d, RedisModuleString *key, void *ptr) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DictReplace)(RedisModuleDict *d, RedisModuleString *key, void *ptr) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_DictGetC)(RedisModuleDict *d, void *key, size_t keylen, int *nokey) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_DictGet)(RedisModuleDict *d, RedisModuleString *key, int *nokey) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DictDelC)(RedisModuleDict *d, void *key, size_t keylen, void *oldval) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DictDel)(RedisModuleDict *d, RedisModuleString *key, void *oldval) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleDictIter * (*RedisModule_DictIteratorStartC)(RedisModuleDict *d, const char *op, void *key, size_t keylen) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleDictIter * (*RedisModule_DictIteratorStart)(RedisModuleDict *d, const char *op, RedisModuleString *key) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_DictIteratorStop)(RedisModuleDictIter *di) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DictIteratorReseekC)(RedisModuleDictIter *di, const char *op, void *key, size_t keylen) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DictIteratorReseek)(RedisModuleDictIter *di, const char *op, RedisModuleString *key) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_DictNextC)(RedisModuleDictIter *di, size_t *keylen, void **dataptr) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_DictPrevC)(RedisModuleDictIter *di, size_t *keylen, void **dataptr) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_DictNext)(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_DictPrev)(RedisModuleCtx *ctx, RedisModuleDictIter *di, void **dataptr) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DictCompareC)(RedisModuleDictIter *di, const char *op, void *key, size_t keylen) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DictCompare)(RedisModuleDictIter *di, const char *op, RedisModuleString *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_RegisterInfoFunc)(RedisModuleCtx *ctx, RedisModuleInfoFunc cb) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_RegisterAuthCallback)(RedisModuleCtx *ctx, RedisModuleAuthCallback cb) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_InfoAddSection)(RedisModuleInfoCtx *ctx, const char *name) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_InfoBeginDictField)(RedisModuleInfoCtx *ctx, const char *name) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_InfoEndDictField)(RedisModuleInfoCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_InfoAddFieldString)(RedisModuleInfoCtx *ctx, const char *field, RedisModuleString *value) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_InfoAddFieldCString)(RedisModuleInfoCtx *ctx, const char *field,const  char *value) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_InfoAddFieldDouble)(RedisModuleInfoCtx *ctx, const char *field, double value) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_InfoAddFieldLongLong)(RedisModuleInfoCtx *ctx, const char *field, long long value) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_InfoAddFieldULongLong)(RedisModuleInfoCtx *ctx, const char *field, unsigned long long value) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleServerInfoData * (*RedisModule_GetServerInfo)(RedisModuleCtx *ctx, const char *section) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_FreeServerInfo)(RedisModuleCtx *ctx, RedisModuleServerInfoData *data) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_ServerInfoGetField)(RedisModuleCtx *ctx, RedisModuleServerInfoData *data, const char* field) SERVERMODULE_ATTR;
SERVERMODULE_API const char * (*RedisModule_ServerInfoGetFieldC)(RedisModuleServerInfoData *data, const char* field) SERVERMODULE_ATTR;
SERVERMODULE_API long long (*RedisModule_ServerInfoGetFieldSigned)(RedisModuleServerInfoData *data, const char* field, int *out_err) SERVERMODULE_ATTR;
SERVERMODULE_API unsigned long long (*RedisModule_ServerInfoGetFieldUnsigned)(RedisModuleServerInfoData *data, const char* field, int *out_err) SERVERMODULE_ATTR;
SERVERMODULE_API double (*RedisModule_ServerInfoGetFieldDouble)(RedisModuleServerInfoData *data, const char* field, int *out_err) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SubscribeToServerEvent)(RedisModuleCtx *ctx, RedisModuleEvent event, RedisModuleEventCallback callback) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SetLRU)(RedisModuleKey *key, mstime_t lru_idle) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetLRU)(RedisModuleKey *key, mstime_t *lru_idle) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SetLFU)(RedisModuleKey *key, long long lfu_freq) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetLFU)(RedisModuleKey *key, long long *lfu_freq) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleBlockedClient * (*RedisModule_BlockClientOnKeys)(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms, RedisModuleString **keys, int numkeys, void *privdata) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleBlockedClient * (*RedisModule_BlockClientOnKeysWithFlags)(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms, RedisModuleString **keys, int numkeys, void *privdata, int flags) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SignalKeyAsReady)(RedisModuleCtx *ctx, RedisModuleString *key) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_GetBlockedClientReadyKey)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleScanCursor * (*RedisModule_ScanCursorCreate)(void) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ScanCursorRestart)(RedisModuleScanCursor *cursor) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ScanCursorDestroy)(RedisModuleScanCursor *cursor) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_Scan)(RedisModuleCtx *ctx, RedisModuleScanCursor *cursor, RedisModuleScanCB fn, void *privdata) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ScanKey)(RedisModuleKey *key, RedisModuleScanCursor *cursor, RedisModuleScanKeyCB fn, void *privdata) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetContextFlagsAll)(void) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetModuleOptionsAll)(void) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetKeyspaceNotificationFlagsAll)(void) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_IsSubEventSupported)(RedisModuleEvent event, uint64_t subevent) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetServerVersion)(void) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetTypeMethodVersion)(void) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_Yield)(RedisModuleCtx *ctx, int flags, const char *busy_reply) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleBlockedClient * (*RedisModule_BlockClient)(RedisModuleCtx *ctx, RedisModuleCmdFunc reply_callback, RedisModuleCmdFunc timeout_callback, void (*free_privdata)(RedisModuleCtx*,void*), long long timeout_ms) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_BlockClientGetPrivateData)(RedisModuleBlockedClient *blocked_client) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_BlockClientSetPrivateData)(RedisModuleBlockedClient *blocked_client, void *private_data) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleBlockedClient * (*RedisModule_BlockClientOnAuth)(RedisModuleCtx *ctx, RedisModuleAuthCallback reply_callback, void (*free_privdata)(RedisModuleCtx*,void*)) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_UnblockClient)(RedisModuleBlockedClient *bc, void *privdata) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_IsBlockedReplyRequest)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_IsBlockedTimeoutRequest)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_GetBlockedClientPrivateData)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleBlockedClient * (*RedisModule_GetBlockedClientHandle)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_AbortBlock)(RedisModuleBlockedClient *bc) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_BlockedClientMeasureTimeStart)(RedisModuleBlockedClient *bc) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_BlockedClientMeasureTimeEnd)(RedisModuleBlockedClient *bc) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleCtx * (*RedisModule_GetThreadSafeContext)(RedisModuleBlockedClient *bc) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleCtx * (*RedisModule_GetDetachedThreadSafeContext)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_FreeThreadSafeContext)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ThreadSafeContextLock)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ThreadSafeContextTryLock)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ThreadSafeContextUnlock)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SubscribeToKeyspaceEvents)(RedisModuleCtx *ctx, int types, RedisModuleNotificationFunc cb) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_AddPostNotificationJob)(RedisModuleCtx *ctx, RedisModulePostNotificationJobFunc callback, void *pd, void (*free_pd)(void*)) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_NotifyKeyspaceEvent)(RedisModuleCtx *ctx, int type, const char *event, RedisModuleString *key) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetNotifyKeyspaceEvents)(void) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_BlockedClientDisconnected)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_RegisterClusterMessageReceiver)(RedisModuleCtx *ctx, uint8_t type, RedisModuleClusterMessageReceiver callback) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SendClusterMessage)(RedisModuleCtx *ctx, const char *target_id, uint8_t type, const char *msg, uint32_t len) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetClusterNodeInfo)(RedisModuleCtx *ctx, const char *id, char *ip, char *master_id, int *port, int *flags) SERVERMODULE_ATTR;
SERVERMODULE_API char ** (*RedisModule_GetClusterNodesList)(RedisModuleCtx *ctx, size_t *numnodes) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_FreeClusterNodesList)(char **ids) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleTimerID (*RedisModule_CreateTimer)(RedisModuleCtx *ctx, mstime_t period, RedisModuleTimerProc callback, void *data) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_StopTimer)(RedisModuleCtx *ctx, RedisModuleTimerID id, void **data) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetTimerInfo)(RedisModuleCtx *ctx, RedisModuleTimerID id, uint64_t *remaining, void **data) SERVERMODULE_ATTR;
SERVERMODULE_API const char * (*RedisModule_GetMyClusterID)(void) SERVERMODULE_ATTR;
SERVERMODULE_API size_t (*RedisModule_GetClusterSize)(void) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_GetRandomBytes)(unsigned char *dst, size_t len) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_GetRandomHexChars)(char *dst, size_t len) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SetDisconnectCallback)(RedisModuleBlockedClient *bc, RedisModuleDisconnectFunc callback) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SetClusterFlags)(RedisModuleCtx *ctx, uint64_t flags) SERVERMODULE_ATTR;
SERVERMODULE_API unsigned int (*RedisModule_ClusterKeySlot)(RedisModuleString *key) SERVERMODULE_ATTR;
SERVERMODULE_API const char *(*RedisModule_ClusterCanonicalKeyNameInSlot)(unsigned int slot) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ExportSharedAPI)(RedisModuleCtx *ctx, const char *apiname, void *func) SERVERMODULE_ATTR;
SERVERMODULE_API void * (*RedisModule_GetSharedAPI)(RedisModuleCtx *ctx, const char *apiname) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleCommandFilter * (*RedisModule_RegisterCommandFilter)(RedisModuleCtx *ctx, RedisModuleCommandFilterFunc cb, int flags) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_UnregisterCommandFilter)(RedisModuleCtx *ctx, RedisModuleCommandFilter *filter) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_CommandFilterArgsCount)(RedisModuleCommandFilterCtx *fctx) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_CommandFilterArgGet)(RedisModuleCommandFilterCtx *fctx, int pos) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_CommandFilterArgInsert)(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_CommandFilterArgReplace)(RedisModuleCommandFilterCtx *fctx, int pos, RedisModuleString *arg) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_CommandFilterArgDelete)(RedisModuleCommandFilterCtx *fctx, int pos) SERVERMODULE_ATTR;
SERVERMODULE_API unsigned long long (*RedisModule_CommandFilterGetClientId)(RedisModuleCommandFilterCtx *fctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_Fork)(RedisModuleForkDoneHandler cb, void *user_data) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SendChildHeartbeat)(double progress) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ExitFromChild)(int retcode) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_KillForkChild)(int child_pid) SERVERMODULE_ATTR;
SERVERMODULE_API float (*RedisModule_GetUsedMemoryRatio)(void) SERVERMODULE_ATTR;
SERVERMODULE_API size_t (*RedisModule_MallocSize)(void* ptr) SERVERMODULE_ATTR;
SERVERMODULE_API size_t (*RedisModule_MallocUsableSize)(void *ptr) SERVERMODULE_ATTR;
SERVERMODULE_API size_t (*RedisModule_MallocSizeString)(RedisModuleString* str) SERVERMODULE_ATTR;
SERVERMODULE_API size_t (*RedisModule_MallocSizeDict)(RedisModuleDict* dict) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleUser * (*RedisModule_CreateModuleUser)(const char *name) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_FreeModuleUser)(RedisModuleUser *user) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_SetContextUser)(RedisModuleCtx *ctx, const RedisModuleUser *user) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SetModuleUserACL)(RedisModuleUser *user, const char* acl) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_SetModuleUserACLString)(RedisModuleCtx * ctx, RedisModuleUser *user, const char* acl, RedisModuleString **error) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_GetModuleUserACLString)(RedisModuleUser *user) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_GetCurrentUserName)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleUser * (*RedisModule_GetModuleUserFromUserName)(RedisModuleString *name) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ACLCheckCommandPermissions)(RedisModuleUser *user, RedisModuleString **argv, int argc) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ACLCheckKeyPermissions)(RedisModuleUser *user, RedisModuleString *key, int flags) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_ACLCheckChannelPermissions)(RedisModuleUser *user, RedisModuleString *ch, int literal) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ACLAddLogEntry)(RedisModuleCtx *ctx, RedisModuleUser *user, RedisModuleString *object, RedisModuleACLLogEntryReason reason) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_ACLAddLogEntryByUserName)(RedisModuleCtx *ctx, RedisModuleString *user, RedisModuleString *object, RedisModuleACLLogEntryReason reason) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_AuthenticateClientWithACLUser)(RedisModuleCtx *ctx, const char *name, size_t len, RedisModuleUserChangedFunc callback, void *privdata, uint64_t *client_id) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_AuthenticateClientWithUser)(RedisModuleCtx *ctx, RedisModuleUser *user, RedisModuleUserChangedFunc callback, void *privdata, uint64_t *client_id) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DeauthenticateAndCloseClient)(RedisModuleCtx *ctx, uint64_t client_id) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_RedactClientCommandArgument)(RedisModuleCtx *ctx, int pos) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString * (*RedisModule_GetClientCertificate)(RedisModuleCtx *ctx, uint64_t id) SERVERMODULE_ATTR;
SERVERMODULE_API int *(*RedisModule_GetCommandKeys)(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int *num_keys) SERVERMODULE_ATTR;
SERVERMODULE_API int *(*RedisModule_GetCommandKeysWithFlags)(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int *num_keys, int **out_flags) SERVERMODULE_ATTR;
SERVERMODULE_API const char *(*RedisModule_GetCurrentCommandName)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_RegisterDefragFunc)(RedisModuleCtx *ctx, RedisModuleDefragFunc func) SERVERMODULE_ATTR;
SERVERMODULE_API void *(*RedisModule_DefragAlloc)(RedisModuleDefragCtx *ctx, void *ptr) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleString *(*RedisModule_DefragRedisModuleString)(RedisModuleDefragCtx *ctx, RedisModuleString *str) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DefragShouldStop)(RedisModuleDefragCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DefragCursorSet)(RedisModuleDefragCtx *ctx, unsigned long cursor) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_DefragCursorGet)(RedisModuleDefragCtx *ctx, unsigned long *cursor) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_GetDbIdFromDefragCtx)(RedisModuleDefragCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API const RedisModuleString * (*RedisModule_GetKeyNameFromDefragCtx)(RedisModuleDefragCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_EventLoopAdd)(int fd, int mask, RedisModuleEventLoopFunc func, void *user_data) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_EventLoopDel)(int fd, int mask) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_EventLoopAddOneShot)(RedisModuleEventLoopOneShotFunc func, void *user_data) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_RegisterBoolConfig)(RedisModuleCtx *ctx, const char *name, int default_val, unsigned int flags, RedisModuleConfigGetBoolFunc getfn, RedisModuleConfigSetBoolFunc setfn, RedisModuleConfigApplyFunc applyfn, void *privdata) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_RegisterNumericConfig)(RedisModuleCtx *ctx, const char *name, long long default_val, unsigned int flags, long long min, long long max, RedisModuleConfigGetNumericFunc getfn, RedisModuleConfigSetNumericFunc setfn, RedisModuleConfigApplyFunc applyfn, void *privdata) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_RegisterStringConfig)(RedisModuleCtx *ctx, const char *name, const char *default_val, unsigned int flags, RedisModuleConfigGetStringFunc getfn, RedisModuleConfigSetStringFunc setfn, RedisModuleConfigApplyFunc applyfn, void *privdata) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_RegisterEnumConfig)(RedisModuleCtx *ctx, const char *name, int default_val, unsigned int flags, const char **enum_values, const int *int_values, int num_enum_vals, RedisModuleConfigGetEnumFunc getfn, RedisModuleConfigSetEnumFunc setfn, RedisModuleConfigApplyFunc applyfn, void *privdata) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_LoadConfigs)(RedisModuleCtx *ctx) SERVERMODULE_ATTR;
SERVERMODULE_API RedisModuleRdbStream *(*RedisModule_RdbStreamCreateFromFile)(const char *filename) SERVERMODULE_ATTR;
SERVERMODULE_API void (*RedisModule_RdbStreamFree)(RedisModuleRdbStream *stream) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_RdbLoad)(RedisModuleCtx *ctx, RedisModuleRdbStream *stream, int flags) SERVERMODULE_ATTR;
SERVERMODULE_API int (*RedisModule_RdbSave)(RedisModuleCtx *ctx, RedisModuleRdbStream *stream, int flags) SERVERMODULE_ATTR;

#define RedisModule_IsAOFClient(id) ((id) == UINT64_MAX)

/* This is included inline inside each Redis module. */
static int RedisModule_Init(RedisModuleCtx *ctx, const char *name, int ver, int apiver) SERVERMODULE_ATTR_UNUSED;
static int RedisModule_Init(RedisModuleCtx *ctx, const char *name, int ver, int apiver) {
    void *getapifuncptr = ((void**)ctx)[0];
    RedisModule_GetApi = (int (*)(const char *, void *)) (unsigned long)getapifuncptr;
    SERVERMODULE_GET_API(Alloc);
    SERVERMODULE_GET_API(TryAlloc);
    SERVERMODULE_GET_API(Calloc);
    SERVERMODULE_GET_API(TryCalloc);
    SERVERMODULE_GET_API(Free);
    SERVERMODULE_GET_API(Realloc);
    SERVERMODULE_GET_API(TryRealloc);
    SERVERMODULE_GET_API(Strdup);
    SERVERMODULE_GET_API(CreateCommand);
    SERVERMODULE_GET_API(GetCommand);
    SERVERMODULE_GET_API(CreateSubcommand);
    SERVERMODULE_GET_API(SetCommandInfo);
    SERVERMODULE_GET_API(SetCommandACLCategories);
    SERVERMODULE_GET_API(AddACLCategory);
    SERVERMODULE_GET_API(SetModuleAttribs);
    SERVERMODULE_GET_API(IsModuleNameBusy);
    SERVERMODULE_GET_API(WrongArity);
    SERVERMODULE_GET_API(ReplyWithLongLong);
    SERVERMODULE_GET_API(ReplyWithError);
    SERVERMODULE_GET_API(ReplyWithErrorFormat);
    SERVERMODULE_GET_API(ReplyWithSimpleString);
    SERVERMODULE_GET_API(ReplyWithArray);
    SERVERMODULE_GET_API(ReplyWithMap);
    SERVERMODULE_GET_API(ReplyWithSet);
    SERVERMODULE_GET_API(ReplyWithAttribute);
    SERVERMODULE_GET_API(ReplyWithNullArray);
    SERVERMODULE_GET_API(ReplyWithEmptyArray);
    SERVERMODULE_GET_API(ReplySetArrayLength);
    SERVERMODULE_GET_API(ReplySetMapLength);
    SERVERMODULE_GET_API(ReplySetSetLength);
    SERVERMODULE_GET_API(ReplySetAttributeLength);
    SERVERMODULE_GET_API(ReplySetPushLength);
    SERVERMODULE_GET_API(ReplyWithStringBuffer);
    SERVERMODULE_GET_API(ReplyWithCString);
    SERVERMODULE_GET_API(ReplyWithString);
    SERVERMODULE_GET_API(ReplyWithEmptyString);
    SERVERMODULE_GET_API(ReplyWithVerbatimString);
    SERVERMODULE_GET_API(ReplyWithVerbatimStringType);
    SERVERMODULE_GET_API(ReplyWithNull);
    SERVERMODULE_GET_API(ReplyWithBool);
    SERVERMODULE_GET_API(ReplyWithCallReply);
    SERVERMODULE_GET_API(ReplyWithDouble);
    SERVERMODULE_GET_API(ReplyWithBigNumber);
    SERVERMODULE_GET_API(ReplyWithLongDouble);
    SERVERMODULE_GET_API(GetSelectedDb);
    SERVERMODULE_GET_API(SelectDb);
    SERVERMODULE_GET_API(KeyExists);
    SERVERMODULE_GET_API(OpenKey);
    SERVERMODULE_GET_API(GetOpenKeyModesAll);
    SERVERMODULE_GET_API(CloseKey);
    SERVERMODULE_GET_API(KeyType);
    SERVERMODULE_GET_API(ValueLength);
    SERVERMODULE_GET_API(ListPush);
    SERVERMODULE_GET_API(ListPop);
    SERVERMODULE_GET_API(ListGet);
    SERVERMODULE_GET_API(ListSet);
    SERVERMODULE_GET_API(ListInsert);
    SERVERMODULE_GET_API(ListDelete);
    SERVERMODULE_GET_API(StringToLongLong);
    SERVERMODULE_GET_API(StringToULongLong);
    SERVERMODULE_GET_API(StringToDouble);
    SERVERMODULE_GET_API(StringToLongDouble);
    SERVERMODULE_GET_API(StringToStreamID);
    SERVERMODULE_GET_API(Call);
    SERVERMODULE_GET_API(CallReplyProto);
    SERVERMODULE_GET_API(FreeCallReply);
    SERVERMODULE_GET_API(CallReplyInteger);
    SERVERMODULE_GET_API(CallReplyDouble);
    SERVERMODULE_GET_API(CallReplyBool);
    SERVERMODULE_GET_API(CallReplyBigNumber);
    SERVERMODULE_GET_API(CallReplyVerbatim);
    SERVERMODULE_GET_API(CallReplySetElement);
    SERVERMODULE_GET_API(CallReplyMapElement);
    SERVERMODULE_GET_API(CallReplyAttributeElement);
    SERVERMODULE_GET_API(CallReplyPromiseSetUnblockHandler);
    SERVERMODULE_GET_API(CallReplyPromiseAbort);
    SERVERMODULE_GET_API(CallReplyAttribute);
    SERVERMODULE_GET_API(CallReplyType);
    SERVERMODULE_GET_API(CallReplyLength);
    SERVERMODULE_GET_API(CallReplyArrayElement);
    SERVERMODULE_GET_API(CallReplyStringPtr);
    SERVERMODULE_GET_API(CreateStringFromCallReply);
    SERVERMODULE_GET_API(CreateString);
    SERVERMODULE_GET_API(CreateStringFromLongLong);
    SERVERMODULE_GET_API(CreateStringFromULongLong);
    SERVERMODULE_GET_API(CreateStringFromDouble);
    SERVERMODULE_GET_API(CreateStringFromLongDouble);
    SERVERMODULE_GET_API(CreateStringFromString);
    SERVERMODULE_GET_API(CreateStringFromStreamID);
    SERVERMODULE_GET_API(CreateStringPrintf);
    SERVERMODULE_GET_API(FreeString);
    SERVERMODULE_GET_API(StringPtrLen);
    SERVERMODULE_GET_API(AutoMemory);
    SERVERMODULE_GET_API(Replicate);
    SERVERMODULE_GET_API(ReplicateVerbatim);
    SERVERMODULE_GET_API(DeleteKey);
    SERVERMODULE_GET_API(UnlinkKey);
    SERVERMODULE_GET_API(StringSet);
    SERVERMODULE_GET_API(StringDMA);
    SERVERMODULE_GET_API(StringTruncate);
    SERVERMODULE_GET_API(GetExpire);
    SERVERMODULE_GET_API(SetExpire);
    SERVERMODULE_GET_API(GetAbsExpire);
    SERVERMODULE_GET_API(SetAbsExpire);
    SERVERMODULE_GET_API(ResetDataset);
    SERVERMODULE_GET_API(DbSize);
    SERVERMODULE_GET_API(RandomKey);
    SERVERMODULE_GET_API(ZsetAdd);
    SERVERMODULE_GET_API(ZsetIncrby);
    SERVERMODULE_GET_API(ZsetScore);
    SERVERMODULE_GET_API(ZsetRem);
    SERVERMODULE_GET_API(ZsetRangeStop);
    SERVERMODULE_GET_API(ZsetFirstInScoreRange);
    SERVERMODULE_GET_API(ZsetLastInScoreRange);
    SERVERMODULE_GET_API(ZsetFirstInLexRange);
    SERVERMODULE_GET_API(ZsetLastInLexRange);
    SERVERMODULE_GET_API(ZsetRangeCurrentElement);
    SERVERMODULE_GET_API(ZsetRangeNext);
    SERVERMODULE_GET_API(ZsetRangePrev);
    SERVERMODULE_GET_API(ZsetRangeEndReached);
    SERVERMODULE_GET_API(HashSet);
    SERVERMODULE_GET_API(HashGet);
    SERVERMODULE_GET_API(StreamAdd);
    SERVERMODULE_GET_API(StreamDelete);
    SERVERMODULE_GET_API(StreamIteratorStart);
    SERVERMODULE_GET_API(StreamIteratorStop);
    SERVERMODULE_GET_API(StreamIteratorNextID);
    SERVERMODULE_GET_API(StreamIteratorNextField);
    SERVERMODULE_GET_API(StreamIteratorDelete);
    SERVERMODULE_GET_API(StreamTrimByLength);
    SERVERMODULE_GET_API(StreamTrimByID);
    SERVERMODULE_GET_API(IsKeysPositionRequest);
    SERVERMODULE_GET_API(KeyAtPos);
    SERVERMODULE_GET_API(KeyAtPosWithFlags);
    SERVERMODULE_GET_API(IsChannelsPositionRequest);
    SERVERMODULE_GET_API(ChannelAtPosWithFlags);
    SERVERMODULE_GET_API(GetClientId);
    SERVERMODULE_GET_API(GetClientUserNameById);
    SERVERMODULE_GET_API(GetContextFlags);
    SERVERMODULE_GET_API(AvoidReplicaTraffic);
    SERVERMODULE_GET_API(PoolAlloc);
    SERVERMODULE_GET_API(CreateDataType);
    SERVERMODULE_GET_API(ModuleTypeSetValue);
    SERVERMODULE_GET_API(ModuleTypeReplaceValue);
    SERVERMODULE_GET_API(ModuleTypeGetType);
    SERVERMODULE_GET_API(ModuleTypeGetValue);
    SERVERMODULE_GET_API(IsIOError);
    SERVERMODULE_GET_API(SetModuleOptions);
    SERVERMODULE_GET_API(SignalModifiedKey);
    SERVERMODULE_GET_API(SaveUnsigned);
    SERVERMODULE_GET_API(LoadUnsigned);
    SERVERMODULE_GET_API(SaveSigned);
    SERVERMODULE_GET_API(LoadSigned);
    SERVERMODULE_GET_API(SaveString);
    SERVERMODULE_GET_API(SaveStringBuffer);
    SERVERMODULE_GET_API(LoadString);
    SERVERMODULE_GET_API(LoadStringBuffer);
    SERVERMODULE_GET_API(SaveDouble);
    SERVERMODULE_GET_API(LoadDouble);
    SERVERMODULE_GET_API(SaveFloat);
    SERVERMODULE_GET_API(LoadFloat);
    SERVERMODULE_GET_API(SaveLongDouble);
    SERVERMODULE_GET_API(LoadLongDouble);
    SERVERMODULE_GET_API(SaveDataTypeToString);
    SERVERMODULE_GET_API(LoadDataTypeFromString);
    SERVERMODULE_GET_API(LoadDataTypeFromStringEncver);
    SERVERMODULE_GET_API(EmitAOF);
    SERVERMODULE_GET_API(Log);
    SERVERMODULE_GET_API(LogIOError);
    SERVERMODULE_GET_API(_Assert);
    SERVERMODULE_GET_API(LatencyAddSample);
    SERVERMODULE_GET_API(StringAppendBuffer);
    SERVERMODULE_GET_API(TrimStringAllocation);
    SERVERMODULE_GET_API(RetainString);
    SERVERMODULE_GET_API(HoldString);
    SERVERMODULE_GET_API(StringCompare);
    SERVERMODULE_GET_API(GetContextFromIO);
    SERVERMODULE_GET_API(GetKeyNameFromIO);
    SERVERMODULE_GET_API(GetKeyNameFromModuleKey);
    SERVERMODULE_GET_API(GetDbIdFromModuleKey);
    SERVERMODULE_GET_API(GetDbIdFromIO);
    SERVERMODULE_GET_API(GetKeyNameFromOptCtx);
    SERVERMODULE_GET_API(GetToKeyNameFromOptCtx);
    SERVERMODULE_GET_API(GetDbIdFromOptCtx);
    SERVERMODULE_GET_API(GetToDbIdFromOptCtx);
    SERVERMODULE_GET_API(Milliseconds);
    SERVERMODULE_GET_API(MonotonicMicroseconds);
    SERVERMODULE_GET_API(Microseconds);
    SERVERMODULE_GET_API(CachedMicroseconds);
    SERVERMODULE_GET_API(DigestAddStringBuffer);
    SERVERMODULE_GET_API(DigestAddLongLong);
    SERVERMODULE_GET_API(DigestEndSequence);
    SERVERMODULE_GET_API(GetKeyNameFromDigest);
    SERVERMODULE_GET_API(GetDbIdFromDigest);
    SERVERMODULE_GET_API(CreateDict);
    SERVERMODULE_GET_API(FreeDict);
    SERVERMODULE_GET_API(DictSize);
    SERVERMODULE_GET_API(DictSetC);
    SERVERMODULE_GET_API(DictReplaceC);
    SERVERMODULE_GET_API(DictSet);
    SERVERMODULE_GET_API(DictReplace);
    SERVERMODULE_GET_API(DictGetC);
    SERVERMODULE_GET_API(DictGet);
    SERVERMODULE_GET_API(DictDelC);
    SERVERMODULE_GET_API(DictDel);
    SERVERMODULE_GET_API(DictIteratorStartC);
    SERVERMODULE_GET_API(DictIteratorStart);
    SERVERMODULE_GET_API(DictIteratorStop);
    SERVERMODULE_GET_API(DictIteratorReseekC);
    SERVERMODULE_GET_API(DictIteratorReseek);
    SERVERMODULE_GET_API(DictNextC);
    SERVERMODULE_GET_API(DictPrevC);
    SERVERMODULE_GET_API(DictNext);
    SERVERMODULE_GET_API(DictPrev);
    SERVERMODULE_GET_API(DictCompare);
    SERVERMODULE_GET_API(DictCompareC);
    SERVERMODULE_GET_API(RegisterInfoFunc);
    SERVERMODULE_GET_API(RegisterAuthCallback);
    SERVERMODULE_GET_API(InfoAddSection);
    SERVERMODULE_GET_API(InfoBeginDictField);
    SERVERMODULE_GET_API(InfoEndDictField);
    SERVERMODULE_GET_API(InfoAddFieldString);
    SERVERMODULE_GET_API(InfoAddFieldCString);
    SERVERMODULE_GET_API(InfoAddFieldDouble);
    SERVERMODULE_GET_API(InfoAddFieldLongLong);
    SERVERMODULE_GET_API(InfoAddFieldULongLong);
    SERVERMODULE_GET_API(GetServerInfo);
    SERVERMODULE_GET_API(FreeServerInfo);
    SERVERMODULE_GET_API(ServerInfoGetField);
    SERVERMODULE_GET_API(ServerInfoGetFieldC);
    SERVERMODULE_GET_API(ServerInfoGetFieldSigned);
    SERVERMODULE_GET_API(ServerInfoGetFieldUnsigned);
    SERVERMODULE_GET_API(ServerInfoGetFieldDouble);
    SERVERMODULE_GET_API(GetClientInfoById);
    SERVERMODULE_GET_API(GetClientNameById);
    SERVERMODULE_GET_API(SetClientNameById);
    SERVERMODULE_GET_API(PublishMessage);
    SERVERMODULE_GET_API(PublishMessageShard);
    SERVERMODULE_GET_API(SubscribeToServerEvent);
    SERVERMODULE_GET_API(SetLRU);
    SERVERMODULE_GET_API(GetLRU);
    SERVERMODULE_GET_API(SetLFU);
    SERVERMODULE_GET_API(GetLFU);
    SERVERMODULE_GET_API(BlockClientOnKeys);
    SERVERMODULE_GET_API(BlockClientOnKeysWithFlags);
    SERVERMODULE_GET_API(SignalKeyAsReady);
    SERVERMODULE_GET_API(GetBlockedClientReadyKey);
    SERVERMODULE_GET_API(ScanCursorCreate);
    SERVERMODULE_GET_API(ScanCursorRestart);
    SERVERMODULE_GET_API(ScanCursorDestroy);
    SERVERMODULE_GET_API(Scan);
    SERVERMODULE_GET_API(ScanKey);
    SERVERMODULE_GET_API(GetContextFlagsAll);
    SERVERMODULE_GET_API(GetModuleOptionsAll);
    SERVERMODULE_GET_API(GetKeyspaceNotificationFlagsAll);
    SERVERMODULE_GET_API(IsSubEventSupported);
    SERVERMODULE_GET_API(GetServerVersion);
    SERVERMODULE_GET_API(GetTypeMethodVersion);
    SERVERMODULE_GET_API(Yield);
    SERVERMODULE_GET_API(GetThreadSafeContext);
    SERVERMODULE_GET_API(GetDetachedThreadSafeContext);
    SERVERMODULE_GET_API(FreeThreadSafeContext);
    SERVERMODULE_GET_API(ThreadSafeContextLock);
    SERVERMODULE_GET_API(ThreadSafeContextTryLock);
    SERVERMODULE_GET_API(ThreadSafeContextUnlock);
    SERVERMODULE_GET_API(BlockClient);
    SERVERMODULE_GET_API(BlockClientGetPrivateData);
    SERVERMODULE_GET_API(BlockClientSetPrivateData);
    SERVERMODULE_GET_API(BlockClientOnAuth);
    SERVERMODULE_GET_API(UnblockClient);
    SERVERMODULE_GET_API(IsBlockedReplyRequest);
    SERVERMODULE_GET_API(IsBlockedTimeoutRequest);
    SERVERMODULE_GET_API(GetBlockedClientPrivateData);
    SERVERMODULE_GET_API(GetBlockedClientHandle);
    SERVERMODULE_GET_API(AbortBlock);
    SERVERMODULE_GET_API(BlockedClientMeasureTimeStart);
    SERVERMODULE_GET_API(BlockedClientMeasureTimeEnd);
    SERVERMODULE_GET_API(SetDisconnectCallback);
    SERVERMODULE_GET_API(SubscribeToKeyspaceEvents);
    SERVERMODULE_GET_API(AddPostNotificationJob);
    SERVERMODULE_GET_API(NotifyKeyspaceEvent);
    SERVERMODULE_GET_API(GetNotifyKeyspaceEvents);
    SERVERMODULE_GET_API(BlockedClientDisconnected);
    SERVERMODULE_GET_API(RegisterClusterMessageReceiver);
    SERVERMODULE_GET_API(SendClusterMessage);
    SERVERMODULE_GET_API(GetClusterNodeInfo);
    SERVERMODULE_GET_API(GetClusterNodesList);
    SERVERMODULE_GET_API(FreeClusterNodesList);
    SERVERMODULE_GET_API(CreateTimer);
    SERVERMODULE_GET_API(StopTimer);
    SERVERMODULE_GET_API(GetTimerInfo);
    SERVERMODULE_GET_API(GetMyClusterID);
    SERVERMODULE_GET_API(GetClusterSize);
    SERVERMODULE_GET_API(GetRandomBytes);
    SERVERMODULE_GET_API(GetRandomHexChars);
    SERVERMODULE_GET_API(SetClusterFlags);
    SERVERMODULE_GET_API(ClusterKeySlot);
    SERVERMODULE_GET_API(ClusterCanonicalKeyNameInSlot);
    SERVERMODULE_GET_API(ExportSharedAPI);
    SERVERMODULE_GET_API(GetSharedAPI);
    SERVERMODULE_GET_API(RegisterCommandFilter);
    SERVERMODULE_GET_API(UnregisterCommandFilter);
    SERVERMODULE_GET_API(CommandFilterArgsCount);
    SERVERMODULE_GET_API(CommandFilterArgGet);
    SERVERMODULE_GET_API(CommandFilterArgInsert);
    SERVERMODULE_GET_API(CommandFilterArgReplace);
    SERVERMODULE_GET_API(CommandFilterArgDelete);
    SERVERMODULE_GET_API(CommandFilterGetClientId);
    SERVERMODULE_GET_API(Fork);
    SERVERMODULE_GET_API(SendChildHeartbeat);
    SERVERMODULE_GET_API(ExitFromChild);
    SERVERMODULE_GET_API(KillForkChild);
    SERVERMODULE_GET_API(GetUsedMemoryRatio);
    SERVERMODULE_GET_API(MallocSize);
    SERVERMODULE_GET_API(MallocUsableSize);
    SERVERMODULE_GET_API(MallocSizeString);
    SERVERMODULE_GET_API(MallocSizeDict);
    SERVERMODULE_GET_API(CreateModuleUser);
    SERVERMODULE_GET_API(FreeModuleUser);
    SERVERMODULE_GET_API(SetContextUser);
    SERVERMODULE_GET_API(SetModuleUserACL);
    SERVERMODULE_GET_API(SetModuleUserACLString);
    SERVERMODULE_GET_API(GetModuleUserACLString);
    SERVERMODULE_GET_API(GetCurrentUserName);
    SERVERMODULE_GET_API(GetModuleUserFromUserName);
    SERVERMODULE_GET_API(ACLCheckCommandPermissions);
    SERVERMODULE_GET_API(ACLCheckKeyPermissions);
    SERVERMODULE_GET_API(ACLCheckChannelPermissions);
    SERVERMODULE_GET_API(ACLAddLogEntry);
    SERVERMODULE_GET_API(ACLAddLogEntryByUserName);
    SERVERMODULE_GET_API(DeauthenticateAndCloseClient);
    SERVERMODULE_GET_API(AuthenticateClientWithACLUser);
    SERVERMODULE_GET_API(AuthenticateClientWithUser);
    SERVERMODULE_GET_API(RedactClientCommandArgument);
    SERVERMODULE_GET_API(GetClientCertificate);
    SERVERMODULE_GET_API(GetCommandKeys);
    SERVERMODULE_GET_API(GetCommandKeysWithFlags);
    SERVERMODULE_GET_API(GetCurrentCommandName);
    SERVERMODULE_GET_API(RegisterDefragFunc);
    SERVERMODULE_GET_API(DefragAlloc);
    SERVERMODULE_GET_API(DefragRedisModuleString);
    SERVERMODULE_GET_API(DefragShouldStop);
    SERVERMODULE_GET_API(DefragCursorSet);
    SERVERMODULE_GET_API(DefragCursorGet);
    SERVERMODULE_GET_API(GetKeyNameFromDefragCtx);
    SERVERMODULE_GET_API(GetDbIdFromDefragCtx);
    SERVERMODULE_GET_API(EventLoopAdd);
    SERVERMODULE_GET_API(EventLoopDel);
    SERVERMODULE_GET_API(EventLoopAddOneShot);
    SERVERMODULE_GET_API(RegisterBoolConfig);
    SERVERMODULE_GET_API(RegisterNumericConfig);
    SERVERMODULE_GET_API(RegisterStringConfig);
    SERVERMODULE_GET_API(RegisterEnumConfig);
    SERVERMODULE_GET_API(LoadConfigs);
    SERVERMODULE_GET_API(RdbStreamCreateFromFile);
    SERVERMODULE_GET_API(RdbStreamFree);
    SERVERMODULE_GET_API(RdbLoad);
    SERVERMODULE_GET_API(RdbSave);

    if (RedisModule_IsModuleNameBusy && RedisModule_IsModuleNameBusy(name)) return SERVERMODULE_ERR;
    RedisModule_SetModuleAttribs(ctx,name,ver,apiver);
    return SERVERMODULE_OK;
}

#define RedisModule_Assert(_e) ((_e)?(void)0 : (RedisModule__Assert(#_e,__FILE__,__LINE__),exit(1)))

#define RMAPI_FUNC_SUPPORTED(func) (func != NULL)

#endif /* SERVERMODULE_CORE */
#endif /* SERVERMODULE_H */
