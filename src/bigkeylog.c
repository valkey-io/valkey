#include "bigkeylog.h"

void bigkeylogFreeEntry(bigkeylogEntry *entry) {
    decrRefCount(entry->key);
    entry->key = NULL;
}

/* Initialize the bigkey log. This function should be called a single time at server startup. */
void bigkeylogInit(void) {
    server.bigkeylog = zmalloc(sizeof(bigkeylogEntry) * server.bigkeylog_bucket_size);
}

void bigkeylogPushEntryIfNeeded(robj *keyobj, long long num_elements) {
    if (server.bigkeylog_num_elements_larger_than < 0 || server.bigkeylog_bucket_size == 0) return; /* Bigkeylog disabled */
    if (num_elements < server.bigkeylog_num_elements_larger_than) return;

    sds key = keyobj->ptr;
    unsigned int idx = crc16(key, sdslen(key)) % server.bigkeylog_bucket_size;
    bigkeylogEntry *entry = &server.bigkeylog[idx];

    /* If the entry is already set, free the entry */
    if (entry->key != NULL) {
        bigkeylogFreeEntry(entry);
    }

    incrRefCount(keyobj);
    entry->key = keyobj;
    entry->num_elements = num_elements;
    entry->time = time(NULL);
}

void bigkeylogReset(void) {
    for (unsigned long i = 0; i < server.bigkeylog_bucket_size; i++) {
        if (server.bigkeylog[i].key != NULL) {
            bigkeylogFreeEntry(&server.bigkeylog[i]);
        }
    }
}

unsigned long bigkeylogLength(void) {
    unsigned long len = 0;
    for (unsigned long i = 0; i < server.bigkeylog_bucket_size; i++) {
        if (server.bigkeylog[i].key != NULL) {
            len++;
        }
    }
    return len;
}

void bigkeylogCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "help")) {
        const char *help[] = {
            "GET",
            "    Return all entries from the bigkeylog.",
            "    Entries are made of: key, size, timestamp",
            "LEN",
            "    Return the length of the bigkeylog.",
            "RESET",
            "    Reset the bigkeylog.",
            NULL,
        };
        addReplyHelp(c, help);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "reset")) {
        bigkeylogReset();
        addReply(c, shared.ok);
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr, "len")) {
        addReplyLongLong(c, bigkeylogLength());
    } else if ((c->argc == 2) && !strcasecmp(c->argv[1]->ptr, "get")) {
        unsigned long len = bigkeylogLength();

        addReplyArrayLen(c, len);
        for (unsigned long i = 0; i < server.bigkeylog_bucket_size; i++) {
            if (server.bigkeylog[i].key != NULL) {
                addReplyArrayLen(c, 3);
                addReplyBulkCBuffer(c, server.bigkeylog[i].key->ptr, sdslen(server.bigkeylog[i].key->ptr));
                addReplyLongLong(c, server.bigkeylog[i].num_elements);
                addReplyLongLong(c, server.bigkeylog[i].time);
            }
        }
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
