/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * This file utilizes prefetching keys and data for multiple commands in a batch,
 * to improve performance by amortizing memory access costs across multiple operations.
 */

#include "memory_prefetch.h"
#include "server.h"
#include "io_threads.h"

typedef enum {
    PREFETCH_ENTRY,        /* Initial state, prefetch entries associated with the given key's hash */
    PREFETCH_VALUE,        /* prefetch the value object of the entry found in the previous step */
    PREFETCH_VALUE_NESTED, /* nested prefetch of inner hashtable for hash/zset types */
    PREFETCH_DONE          /* Indicates that prefetching for this key is complete */
} PrefetchState;

typedef enum {
    NESTED_PREFETCH_HEADER, /* Prefetch val->ptr (data structure header) */
    NESTED_PREFETCH_INIT,   /* Init incremental find on inner hashtable */
    NESTED_PREFETCH_STEP,   /* Step through incremental find */
    NESTED_PREFETCH_VALUE,  /* Prefetch the found entry's value (non-embedded only) */
} NestedPrefetchPhase;

typedef struct KeyPrefetchInfo {
    PrefetchState state; /* Current state of the prefetch operation */
    hashtableIncrementalFindState hashtab_state;
    /* Fields for nested prefetching of inner hashtables (hash/zset) */
    robj *member;      /* field/member to look up in the inner table, NULL if none */
    int inner_is_zset; /* inner table is a zset index: lookup keys need marking */
    NestedPrefetchPhase nested_phase;
    hashtableIncrementalFindState inner_hashtab_state;
} KeyPrefetchInfo;

/* PrefetchCommandsBatch structure holds the state of the current batch of client commands being processed. */
typedef struct PrefetchCommandsBatch {
    size_t cur_idx;                 /* Index of the current key being processed */
    size_t keys_done;               /* Number of keys that have been prefetched */
    size_t key_count;               /* Number of keys in the current batch */
    size_t client_count;            /* Number of clients in the current batch */
    size_t max_prefetch_size;       /* Maximum number of keys to prefetch in a batch */
    size_t executed_commands;       /* Number of commands executed in the current batch */
    int *slots;                     /* Array of slots for each key */
    void **keys;                    /* Array of keys to prefetch in the current batch */
    client **clients;               /* Array of clients in the current batch */
    robj **key_members;             /* Member to prefetch for each key (NULL = no nested prefetch) */
    hashtable **keys_tables;        /* Main table for each key */
    KeyPrefetchInfo *prefetch_info; /* Prefetch info for each key */
} PrefetchCommandsBatch;

static PrefetchCommandsBatch *batch = NULL;

void freePrefetchCommandsBatch(void) {
    if (batch == NULL) {
        return;
    }

    zfree(batch->clients);
    zfree(batch->key_members);
    zfree(batch->keys);
    zfree(batch->keys_tables);
    zfree(batch->slots);
    zfree(batch->prefetch_info);
    zfree(batch);
    batch = NULL;
}

void prefetchCommandsBatchInit(void) {
    if (batch) return;
    size_t max_prefetch_size = server.prefetch_batch_max_size;

    if (max_prefetch_size == 0) {
        return;
    }

    batch = zcalloc(sizeof(PrefetchCommandsBatch));
    batch->max_prefetch_size = max_prefetch_size;
    batch->clients = zcalloc(max_prefetch_size * sizeof(client *));
    batch->key_members = zcalloc(max_prefetch_size * sizeof(robj *));
    batch->keys = zcalloc(max_prefetch_size * sizeof(void *));
    batch->keys_tables = zcalloc(max_prefetch_size * sizeof(hashtable *));
    batch->slots = zcalloc(max_prefetch_size * sizeof(int));
    batch->prefetch_info = zcalloc(max_prefetch_size * sizeof(KeyPrefetchInfo));
}

int onMaxBatchSizeChange(const char **err) {
    UNUSED(err);
    if (batch && batch->client_count > 0) {
        /* We need to process the current batch before updating the size */
        return 1;
    }

    freePrefetchCommandsBatch();
    prefetchCommandsBatchInit();
    return 1;
}

/* Move to the next key in the batch. */
static void moveToNextKey(void) {
    batch->cur_idx = (batch->cur_idx + 1) % batch->key_count;
}

static void markKeyAsdone(KeyPrefetchInfo *info) {
    info->state = PREFETCH_DONE;
    server.stat_total_prefetch_entries++;
    batch->keys_done++;
}

/* Returns the next KeyPrefetchInfo structure that needs to be processed. */
static KeyPrefetchInfo *getNextPrefetchInfo(void) {
    size_t start_idx = batch->cur_idx;
    do {
        KeyPrefetchInfo *info = &batch->prefetch_info[batch->cur_idx];
        if (info->state != PREFETCH_DONE) return info;
        batch->cur_idx = (batch->cur_idx + 1) % batch->key_count;
    } while (batch->cur_idx != start_idx);
    return NULL;
}

/* Initialize per-key state and start the main-hashtable find for each key. */
static void initBatchInfo(hashtable **tables) {
    /* Initialize the prefetch info */
    for (size_t i = 0; i < batch->key_count; i++) {
        KeyPrefetchInfo *info = &batch->prefetch_info[i];
        if (!tables[i] || hashtableSize(tables[i]) == 0) {
            info->state = PREFETCH_DONE;
            batch->keys_done++;
            continue;
        }
        info->state = PREFETCH_ENTRY;
        info->member = batch->key_members[i];
        info->inner_is_zset = 0;
        info->nested_phase = NESTED_PREFETCH_HEADER;
        hashtableIncrementalFindInit(&info->hashtab_state, tables[i], batch->keys[i]);
    }
}

/* A key is eligible for nested prefetch when its command supplied a member and the
 * value is backed by a hashtable we can look the member up in. */
static inline int canNestedPrefetch(KeyPrefetchInfo *info, robj *val) {
    return info->member != NULL && (val->encoding == OBJ_ENCODING_HASHTABLE ||
                                    (val->type == OBJ_ZSET && val->encoding == OBJ_ENCODING_BTREE));
}

/* Advance the main-hashtable find and pick the next state once the entry is found. */
static void prefetchEntry(KeyPrefetchInfo *info) {
    if (hashtableIncrementalFindStep(&info->hashtab_state)) {
        /* Not done yet */
        moveToNextKey();
    } else if (server.io_threads_num >= server.min_io_threads_copy_avoid) {
        /* Copy avoidance should be more efficient without value prefetch
         * starting certain number of I/O threads, but hash and zset keys still
         * need their inner hashtable prefetched. */
        void *entry;
        if (hashtableIncrementalFindGetResult(&info->hashtab_state, &entry) && canNestedPrefetch(info, entry)) {
            info->state = PREFETCH_VALUE_NESTED;
        } else {
            markKeyAsdone(info);
        }
    } else {
        info->state = PREFETCH_VALUE;
    }
}

/* Prefetch the entry's value object, then hand hash and zset keys to the nested path. */
static void prefetchValue(KeyPrefetchInfo *info) {
    void *entry;
    if (hashtableIncrementalFindGetResult(&info->hashtab_state, &entry)) {
        robj *val = entry;
        if (val->encoding == OBJ_ENCODING_RAW && val->type == OBJ_STRING) {
            valkey_prefetch(objectGetVal(val));
        }
        if (canNestedPrefetch(info, val)) {
            info->state = PREFETCH_VALUE_NESTED;
            return;
        }
    }

    markKeyAsdone(info);
}

/* Nested prefetch: walk the inner hashtable for hash/zset types using a phased
 * approach (HEADER -> INIT -> STEP [-> VALUE]) to amortize cache misses across
 * commands in the batch. Prefetches the single member supplied by the command.
 * The VALUE phase runs only for non-embedded hash values. */
static void prefetchValueNested(KeyPrefetchInfo *info) {
    void *entry;
    if (!hashtableIncrementalFindGetResult(&info->hashtab_state, &entry)) {
        markKeyAsdone(info);
        return;
    }
    robj *val = entry;

    switch (info->nested_phase) {
    case NESTED_PREFETCH_HEADER:
        /* Prefetch the data structure header (val->ptr). */
        valkey_prefetch(objectGetVal(val));
        info->nested_phase = NESTED_PREFETCH_INIT;
        moveToNextKey();
        return;

    case NESTED_PREFETCH_INIT: {
        /* The header is warm now, so the inner hashtable pointer can be read. */
        hashtable *inner_ht = NULL;
        if (val->encoding == OBJ_ENCODING_HASHTABLE) {
            inner_ht = objectGetVal(val);
        } else if (val->type == OBJ_ZSET && val->encoding == OBJ_ENCODING_BTREE) {
            zset *zs = objectGetVal(val);
            inner_ht = zs->ht;
            info->inner_is_zset = 1;
        }
        if (!inner_ht || hashtableSize(inner_ht) == 0) {
            markKeyAsdone(info);
            return;
        }
        /* The zset hashtable stores packed [score][element] items, so a plain sds
         * lookup key must be marked for the callbacks to read it as an element. */
        sds member = objectGetVal(info->member);
        if (info->inner_is_zset) zsetMarkLookupKey(member);
        hashtableIncrementalFindInit(&info->inner_hashtab_state, inner_ht, member);
        if (info->inner_is_zset) zsetUnmarkLookupKey(member);
        info->nested_phase = NESTED_PREFETCH_STEP;
        moveToNextKey();
        return;
    }

    case NESTED_PREFETCH_STEP: {
        /* A step may invoke the compare callback, so mark the lookup key here too. */
        sds step_member = objectGetVal(info->member);
        if (info->inner_is_zset) zsetMarkLookupKey(step_member);
        int more = hashtableIncrementalFindStep(&info->inner_hashtab_state);
        if (info->inner_is_zset) zsetUnmarkLookupKey(step_member);
        if (more) {
            moveToNextKey();
            return;
        }
        /* Only non-embedded hash values have a separate value pointer worth
         * prefetching; embedded values and zset/set skip the VALUE phase. */
        if (val->type == OBJ_HASH) {
            void *inner_entry;
            if (hashtableIncrementalFindGetResult(&info->inner_hashtab_state, &inner_entry) && inner_entry &&
                !entryHasEmbeddedValue(inner_entry)) {
                info->nested_phase = NESTED_PREFETCH_VALUE;
                moveToNextKey();
                return;
            }
        }
        markKeyAsdone(info);
        return;
    }

    case NESTED_PREFETCH_VALUE: {
        void *inner_entry;
        if (hashtableIncrementalFindGetResult(&info->inner_hashtab_state, &inner_entry) && inner_entry) {
            char *value = entryGetValue(inner_entry, NULL);
            if (value) valkey_prefetch(value);
        }
        markKeyAsdone(info);
        return;
    }
    default: serverPanic("Unknown nested prefetch phase %d", info->nested_phase);
    }
}

/* Prefetch hashtable data for an array of keys.
 *
 * This function takes an array of tables and keys, attempting to bring
 * data closer to the L1 cache that might be needed for hashtable operations
 * on those keys.
 *
 * tables - An array of hashtables to prefetch data from.
 * prefetch_value - If true, we prefetch the value data for each key.
 * to bring the key's value data closer to the L1 cache as well.
 */
static void hashtablePrefetch(hashtable **tables) {
    initBatchInfo(tables);
    KeyPrefetchInfo *info;
    while ((info = getNextPrefetchInfo())) {
        switch (info->state) {
        case PREFETCH_ENTRY: prefetchEntry(info); break;
        case PREFETCH_VALUE: prefetchValue(info); break;
        case PREFETCH_VALUE_NESTED: prefetchValueNested(info); break;
        default: serverPanic("Unknown prefetch state %d", info->state);
        }
    }
}

static void resetCommandsBatch(void) {
    batch->cur_idx = 0;
    batch->keys_done = 0;
    batch->key_count = 0;
    batch->client_count = 0;
    batch->executed_commands = 0;
}

/* Prefetch command-related data:
 * 1. Prefetch the command arguments allocated by the I/O thread to bring them closer to the L1 cache.
 * 2. Prefetch the keys and values for all commands in the current batch from the main hashtable. */
static void prefetchCommands(void) {
    /* Prefetch argv's for all clients */
    for (size_t i = 0; i < batch->client_count; i++) {
        client *c = batch->clients[i];
        if (!c || c->argc <= 1) continue;
        /* Skip prefetching first argv (cmd name) it was already looked up by the I/O thread. */
        for (int j = 1; j < c->argc; j++) {
            valkey_prefetch(c->argv[j]);
        }
    }

    /* Prefetch the argv->ptr if required */
    for (size_t i = 0; i < batch->client_count; i++) {
        client *c = batch->clients[i];
        if (!c || c->argc <= 1) continue;
        for (int j = 1; j < c->argc; j++) {
            if (c->argv[j]->encoding == OBJ_ENCODING_RAW) {
                valkey_prefetch(objectGetVal(c->argv[j]));
            }
        }
    }

    /* Get the keys ptrs - we do it here after the key obj was prefetched. */
    for (size_t i = 0; i < batch->key_count; i++) {
        batch->keys[i] = objectGetVal((robj *)batch->keys[i]);
    }

    /* Prefetch hashtable keys for all commands. Prefetching is beneficial only if there are more than one key. */
    if (batch->key_count > 1) {
        server.stat_total_prefetch_batches++;
        /* Prefetch keys from the main hashtable */
        hashtablePrefetch(batch->keys_tables);
    }
}

/* Processes all the prefetched commands in the current batch. */
void processClientsCommandsBatch(void) {
    if (!batch || batch->client_count == 0) return;

    /* If executed_commands is not 0,
     * it means that we are in the middle of processing a batch and this is a recursive call */
    if (batch->executed_commands == 0) {
        prefetchCommands();
    }

    /* Process the commands */
    for (size_t i = 0; i < batch->client_count; i++) {
        client *c = batch->clients[i];
        if (c == NULL) continue;

        /* Set the client to null immediately to avoid accessing it again recursively when ProcessingEventsWhileBlocked */
        batch->clients[i] = NULL;
        batch->executed_commands++;
        if (processPendingCommandAndInputBuffer(c) != C_ERR) beforeNextClient(c);
    }

    resetCommandsBatch();

    /* Handle the case where the max prefetch size has been changed. */
    if (batch->max_prefetch_size != (size_t)server.prefetch_batch_max_size) {
        onMaxBatchSizeChange(NULL);
    }
}

/* Get a command's keys and add them to the current prefetching batch. */
static void addCommandToBatch(struct serverCommand *cmd, robj **argv, int argc, serverDb *db, int slot) {
    getKeysResult result;
    initGetKeysResult(&result);
    int num_keys = getKeysFromCommand(cmd, argv, argc, &result);
    int member_idx = cmd->member_arg_index;
    robj *member = (member_idx > 0 && member_idx < argc) ? argv[member_idx] : NULL;
    for (int i = 0; i < num_keys && batch->key_count < batch->max_prefetch_size; i++) {
        batch->keys[batch->key_count] = argv[result.keys[i].pos];
        batch->slots[batch->key_count] = slot >= 0 ? slot : 0;
        batch->keys_tables[batch->key_count] = kvstoreGetHashtable(db->keys, batch->slots[batch->key_count]);
        batch->key_members[batch->key_count] =
            (result.keys[i].flags & CMD_KEY_OW) && !(result.keys[i].flags & CMD_KEY_ACCESS) ? NULL : member;
        batch->key_count++;
    }
    getKeysFreeResult(&result);
}

/* Adds the client's command to the current batch and processes the batch
 * if it becomes full.
 *
 * Returns C_OK if the command was added successfully, C_ERR otherwise. */
int addCommandToBatchAndProcessIfFull(client *c) {
    if (!batch) return C_ERR;

    batch->clients[batch->client_count++] = c;

    /* Client's next command */
    if (c->parsed_cmd && !(c->read_flags & READ_FLAGS_BAD_ARITY)) {
        c->read_flags |= READ_FLAGS_PREFETCHED;
        addCommandToBatch(c->parsed_cmd, c->argv, c->argc, c->db, c->slot);
    }

    /* Commands in the queue. */
    for (int j = c->cmd_queue.off; j < c->cmd_queue.len && batch->key_count < batch->max_prefetch_size; j++) {
        parsedCommand *p = &c->cmd_queue.cmds[j];
        /* Error, incomplete command, or a command whose argc violates its arity. The latter must be
         * skipped because getKeysFromCommand() assumes the arity check has already passed. */
        if (!p->cmd || p->read_flags & READ_FLAGS_BAD_ARITY) continue;
        p->read_flags |= READ_FLAGS_PREFETCHED;
        addCommandToBatch(p->cmd, p->argv, p->argc, c->db, p->slot);
    }

    /* If the batch is full, process it.
     * We also check the client count to handle cases where
     * no keys exist for the clients' commands. */
    if (batch->client_count == batch->max_prefetch_size || batch->key_count == batch->max_prefetch_size) {
        processClientsCommandsBatch();
    }

    return C_OK;
}

/* Removes the given client from the pending prefetch batch, if present. */
void removeClientFromPendingCommandsBatch(client *c) {
    if (!batch) return;

    for (size_t i = 0; i < batch->client_count; i++) {
        if (batch->clients[i] == c) {
            batch->clients[i] = NULL;
            return;
        }
    }
}
