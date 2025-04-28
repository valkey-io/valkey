#include "keyinfo.h"

void keyinfoFreeEntry(keyinfoEntry *entry) {
    decrRefCount(entry->key);
    entry->key = NULL;
}

/* Initialize the bigkey log. This function should be called a single time at server startup. */
void keyinfoInit(void) {
    for (int i = 0; i < KEYINFO_TYPE_NUM; i++) {
        server.keyinfo[i].entries_max_len = server.keyinfo[i].max_len;
        server.keyinfo[i].entries = zcalloc(sizeof(keyinfoEntry) * server.keyinfo[i].entries_max_len);
    }
}

static unsigned int getIndex(robj *keyobj, long long max_len) {
    sds key = keyobj->ptr;
    return crc16(key, sdslen(key)) % max_len;
}

void keyinfoResize(int type) {
    long long new_len = server.keyinfo[type].max_len;
    keyinfoEntry *entriesResized = zcalloc(sizeof(keyinfoEntry) * new_len);

    for (long i = 0; i < server.keyinfo[type].entries_max_len; i++) {
        keyinfoEntry *entry = &server.keyinfo[type].entries[i];
        if (entry->key != NULL) {
            unsigned int idx = getIndex(entry->key, new_len);
            entriesResized[idx].key = entry->key;
            entriesResized[idx].value = entry->value;
            entriesResized[idx].time = entry->time;
        }
    }

    zfree(server.keyinfo[type].entries);
    server.keyinfo[type].entries = entriesResized;
    server.keyinfo[type].entries_max_len = new_len;
}

void keyinfoUpdateEntryIfNeeded(robj *keyobj, long long value, int type) {
    if (server.keyinfo[type].threshold < 0 || server.keyinfo[type].max_len == 0) return; /* keyinfo disabled */

    unsigned int idx = getIndex(keyobj, server.keyinfo[type].max_len);
    keyinfoEntry *entry = &server.keyinfo[type].entries[idx];

    if (value <= server.keyinfo[type].threshold) {
        if (entry->key != NULL) {
            keyinfoFreeEntry(entry);
        }
        return;
    }

    incrRefCount(keyobj);
    /* If the entry is already set, free the entry */
    if (entry->key != NULL) {
        keyinfoFreeEntry(entry);
    }

    entry->key = keyobj;
    entry->value = value;
    entry->time = time(NULL);
}

void keyinfoReset(int type) {
    for (long i = 0; i < server.keyinfo[type].max_len; i++) {
        keyinfoEntry *entry = &server.keyinfo[type].entries[i];
        if (entry->key != NULL) {
            keyinfoFreeEntry(entry);
        }
    }
}

/* Add size to the keyinfo structure and update it when the number of entries in keyinfo
 * increases or decreases, making it's time complexity O(1). */
long keyinfoLength(int type) {
    long len = 0;
    for (long i = 0; i < server.keyinfo[type].max_len; i++) {
        if (server.keyinfo[type].entries[i].key != NULL) {
            len++;
        }
    }
    return len;
}

static int keyinfoGetTypeOrReply(client *c, robj *o) {
    if (!strcasecmp(o->ptr, "many-elements")) return KEYINFO_TYPE_MANY_ELEMENTS;
    addReplyError(c, "type should be one of the following: many-elements");
    return -1;
}

/* The KEYINFO command. Implements all the subcommands needed to handle the keyinfo. */
void keyinfoCommand(client *c) {
    int type;
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "help")) {
        const char *help[] = {
            "GET <count> <type>",
            "    Return top <count> entries of the specified <type> from the keyinfo (-1 mean all).",
            "    Entries are made of:",
            "    id, key,",
            "        the number of elements for type of many-elements,",
            "    timestamp",
            "LEN <type>",
            "    Return the length of the specified type of keyinfo.",
            "RESET <type>",
            "    Reset the specified type of keyinfo.",
            NULL,
        };
        addReplyHelp(c, help);
    } else if (c->argc == 3 && !strcasecmp(c->argv[1]->ptr, "reset")) {
        if ((type = keyinfoGetTypeOrReply(c, c->argv[2])) == -1) return;
        keyinfoReset(type);
        addReply(c, shared.ok);
    } else if (c->argc == 3 && !strcasecmp(c->argv[1]->ptr, "len")) {
        if ((type = keyinfoGetTypeOrReply(c, c->argv[2])) == -1) return;
        addReplyLongLong(c, keyinfoLength(type));
    } else if (c->argc == 4 && !strcasecmp(c->argv[1]->ptr, "get")) {
        long count;

        /* Consume count arg. */
        if (getRangeLongFromObjectOrReply(c, c->argv[2], -1, LONG_MAX, &count,
                                          "count should be greater than or equal to -1") != C_OK)
            return;

        if ((type = keyinfoGetTypeOrReply(c, c->argv[3])) == -1) return;

        if (count == -1) {
            /* We treat -1 as a special value, which means to get all keyinfo.
             * Simply set count to the length of server.keyinfo. */
            count = keyinfoLength(type);
        } else {
            /* TODO : long/unsigned long */
            count = min(count, keyinfoLength(type));
        }

        addReplyArrayLen(c, count);
        long replied = 0;
        for (long i = 0; i < server.keyinfo[type].max_len && replied < count; i++) {
            keyinfoEntry *entry = &server.keyinfo[type].entries[i];
            if (entry->key != NULL) {
                addReplyArrayLen(c, 4);
                addReplyLongLong(c, i);
                addReplyBulkCBuffer(c, entry->key->ptr, sdslen(entry->key->ptr));
                addReplyLongLong(c, entry->value);
                addReplyLongLong(c, entry->time);
                replied++;
            }
        }
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
