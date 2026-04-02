/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BlockInuse - Client blocking mechanism for keys that are currently
 * being processed by a background thread.
 */

#include "server.h"
#include "blocked_inuse.h"

/* Internal blockInUse data structure */
static hashtable *client_to_keys; /* Maps client pointers to a list of keys the client is blocked on. */
static hashtable *key_to_clients; /* Maps keys to a list of clients blocked on them. */

static void markClientBlocked(client *c) {
    serverAssert(c->flag.blocked == 0 && c->flag.unblocked == 0);
    serverAssert(c->flag.pending_command == 1); /* Caller must have a pending command to be resumed on unblock */
    c->flag.blocked_in_use = 1;
}

/* ----------------------------- client_to_keys Hashtable util ------------------------- */
/* Entry for client_to_keys hashtable */
typedef struct {
    client *c;
    int num_keys;        /* Number of keys in the array */
    robj **keys;         /* Array of keys the client is blocked on */
    mstime_t blocked_at; /* Timestamp when client was blocked (from server.mstime) */
} clientDataEntry;

/* Hashtable callbacks */
static const void *clientDataEntryGetClient(const void *entry) {
    return ((clientDataEntry *)entry)->c;
}

static void clientDataEntryDestructor(void *entry) {
    clientDataEntry *e = entry;
    if (e->keys) {
        serverAssert(e->num_keys == 0); // All key refcounts must be decremented before destruction
        zfree(e->keys);
    }
    zfree(e);
}

static hashtableType clientDataHashtableType = {
    .entryGetKey = clientDataEntryGetClient,
    .hashFunction = hashtableClientHash,
    .keyCompare = hashtableClientKeyCompare,
    .entryDestructor = clientDataEntryDestructor,
};

/* Utility functions for client_to_keys hashtable */

// Create a new clientDataEntry for client and add it to client_to_keys.
static clientDataEntry *clientToKeys_addClientDataEntry(client *c, int num_keys, robj **keys) {
    serverAssert(!hashtableFind(client_to_keys, c, NULL)); // client must not already exist in the client_to_keys
    serverAssert(num_keys > 0);

    clientDataEntry *entry = zcalloc(sizeof(clientDataEntry));
    entry->c = c;
    entry->num_keys = num_keys;
    entry->blocked_at = server.mstime;
    entry->keys = keys;
    hashtableAdd(client_to_keys, entry);
    return entry;
}

// Remove the clientDataEntry for client from client_to_keys.
static void clientToKeys_removeClientDataEntry(client *c) {
    hashtableDelete(client_to_keys, c);
}

/*
 * Remove 'key' from the list of keys this client is blocked on.
 */
static clientDataEntry *clientToKeys_removeKey(client *c, robj *key) {
    clientDataEntry *entry;
    if (!hashtableFind(client_to_keys, c, (void **)&entry)) return NULL;

    sds key_sds = objectGetVal(key);
    for (int i = 0; i < entry->num_keys; ++i) {
        sds curr_key = objectGetVal(entry->keys[i]);
        if (sdscmp(curr_key, key_sds) == 0) {
            decrRefCount(entry->keys[i]);
            entry->keys[i] = entry->keys[entry->num_keys - 1];
            entry->keys[entry->num_keys - 1] = NULL;
            entry->num_keys--;
            return entry;
        }
    }
    serverAssert(false); // key must exist
}

/* ----------------------------- key_to_clients Hashtable Util ------------------------- */

/* Entry for key_to_clients hashtable, maps a key object to the list of clients blocked on it */
typedef struct {
    robj *key;     /* Key object */
    list *clients; /* List of clients blocked on this key */
} keyToClientsEntry;

/* Hashtable callbacks */

static const void *keyToClientsGetKey(const void *entry) {
    return ((keyToClientsEntry *)entry)->key;
}

static void keyToClientsDestructor(void *entry) {
    keyToClientsEntry *e = entry;
    decrRefCount(e->key);
    listRelease(e->clients);
    zfree(e);
}

static hashtableType keyToClientsHashtableType = {
    .entryGetKey = keyToClientsGetKey,
    .hashFunction = dictEncObjHash,
    .keyCompare = hashtableEncObjKeyCompare,
    .entryDestructor = keyToClientsDestructor,
};

/* Utility functions for key_to_clients hashtable */

// Return the list of clients blocked on key, or NULL if none exist.
static list *keyToClients_getBlockedClientsList(robj *key) {
    keyToClientsEntry *entry;
    if (hashtableFind(key_to_clients, key, (void **)&entry)) {
        return entry->clients;
    }
    return NULL;
}

// Create a new keyToClientsEntry for key, add it to key_to_clients,
// and return its clients list. Precondition: the key must not already exist.
static list *keyToClients_addEntry(robj *key) {
    serverAssert(!keyToClients_getBlockedClientsList(key));

    keyToClientsEntry *entry = zcalloc(sizeof(keyToClientsEntry));
    entry->key = key;
    incrRefCount(key);
    entry->clients = listCreate();
    hashtableAdd(key_to_clients, entry);
    return entry->clients;
}

// Remove the entry for key from key_to_clients.
static void keyToClients_deleteKey(robj *key) {
    hashtableDelete(key_to_clients, key);
}

/*
 * Unlink a client from key_to_clients.
 */
static void keyToClients_unlinkClient(client *c) {
    clientDataEntry *entry;
    if (!hashtableFind(client_to_keys, c, (void **)&entry)) return;

    for (int i = 0; i < entry->num_keys; ++i) {
        robj *key = entry->keys[i];
        list *clientList = keyToClients_getBlockedClientsList(key);
        serverAssert(clientList != NULL);
        listDelNode(clientList, listSearchKey(clientList, c));
        if (listLength(clientList) == 0) keyToClients_deleteKey(key);
        decrRefCount(key);
        entry->keys[i] = NULL;
    }
    entry->num_keys = 0;
}


/* ----------------------------- API implementation ------------------------- */

/* Check if client is blocked by blockInUse */
bool blockInUse_isClientBlocked(client *c) {
    return c->flag.blocked_in_use;
}

/*
 * Initialize blockInUse data structures.
 */
void blockInUse_init(void) {
    serverAssert(!client_to_keys);
    serverAssert(!key_to_clients);
    client_to_keys = hashtableCreate(&clientDataHashtableType);
    key_to_clients = hashtableCreate(&keyToClientsHashtableType);
}

/*
 * Release blockInUse data structures.
 * Unblocks all clients before cleanup.
 */
void blockInUse_release(void) {
    blockInUse_unblockClientsOnAllKeys();
    if (client_to_keys) {
        hashtableRelease(client_to_keys);
        client_to_keys = NULL;
    }
    if (key_to_clients) {
        hashtableRelease(key_to_clients);
        key_to_clients = NULL;
    }
}

/* Get the current number of clients blocked by blockInUse. */
int blockInUse_getNumberOfBlockedClients(void) {
    return hashtableSize(client_to_keys);
}

/* Get the current number of blocked keys by blockInUse. */
int blockInUse_getNumberOfBlockedKeys(void) {
    return hashtableSize(key_to_clients);
}

/* Block a client on a set of keys. */
void blockInUse_blockClientOnKeys(client *c, int num_keys, robj *keys[]) {
    /* Client must not be in any blocked state. A blocked client has active
     * bstate and is owned by the unblock machinery in blocked.c; setting
     * blocked_in_use on top of that would cause conflicting unblock paths.
     * Similarly, an unblocked client is already queued for resumption. */
    serverAssert(!blockInUse_isClientBlocked(c) && !c->flag.unblocked && !c->flag.blocked);
    serverAssert(num_keys > 0);
    serverAssert(!c->flag.replica);
    robj **entry_keys = zcalloc(sizeof(robj *) * num_keys);
    int num_entry_keys = 0;

    for (int i = 0; i < num_keys; ++i) {
        robj *key = keys[i];
        serverAssert(key->type == OBJ_STRING);

        list *blockedClientsList = keyToClients_getBlockedClientsList(key);
        if (!blockedClientsList) blockedClientsList = keyToClients_addEntry(key);

        // Deduplicate: add client only if it’s not already the last in the list
        listNode *last_client = listLast(blockedClientsList);
        if (last_client == NULL || last_client->value != c) {
            listAddNodeTail(blockedClientsList, c);
            incrRefCount(key);
            entry_keys[num_entry_keys++] = key;
        }
    }

    clientToKeys_addClientDataEntry(c, num_entry_keys, entry_keys);
    markClientBlocked(c);

    // Disable client’s Read Handler to prevent reading commands while blocked
    if (c->conn) {
        connSetReadHandler(c->conn, NULL);
    }
}

/*
 * Unblock all clients blocked on the given key.
 *
 * - Each client is unblocked only when it has no remaining dependencies on other keys.
 * - Clients that become fully unblocked are added to server.unblocked_clients
 *   and will be resumed later in processUnblockedClients().
 */
void blockInUse_unblockClientsOnKey(robj *key) {
    list *blockedClientsList = keyToClients_getBlockedClientsList(key);
    if (blockedClientsList == NULL) return;

    serverAssert(listLength(blockedClientsList) > 0);

    while (listLength(blockedClientsList) > 0) {
        listNode *ln = listFirst(blockedClientsList);
        client *c = listNodeValue(ln);

        listDelNode(blockedClientsList, ln);
        clientDataEntry *entry = clientToKeys_removeKey(c, key);

        if (entry->num_keys == 0) {
            serverAssert(blockInUse_isClientBlocked(c) && c->flag.unblocked == 0);
            c->flag.blocked_in_use = 0;
            queueClientForReprocessing(c);

            clientToKeys_removeClientDataEntry(c);
        }
    }

    keyToClients_deleteKey(key);
}

/*
 * Unblock all clients on all keys.
 */
void blockInUse_unblockClientsOnAllKeys(void) {
    hashtableIterator iter;
    hashtableInitIterator(&iter, key_to_clients, HASHTABLE_ITER_SAFE);
    void *entry;
    while (hashtableNext(&iter, &entry)) {
        keyToClientsEntry *e = entry;
        robj *key = e->key;
        blockInUse_unblockClientsOnKey(key);
    }
    hashtableCleanupIterator(&iter);
    serverAssert(blockInUse_getNumberOfBlockedClients() == 0);
    serverAssert(blockInUse_getNumberOfBlockedKeys() == 0);
}

/*
 * Unlink a blocked client from the blockInUse mapping, the client must be blocked by blockInUse.
 */
void blockInUse_unlinkClient(client *c) {
    clientDataEntry *entry;
    if (!hashtableFind(client_to_keys, c, (void **)&entry)) return;

    serverAssert(blockInUse_isClientBlocked(c) && c->flag.unblocked == 0 && c->flag.blocked == 0);

    keyToClients_unlinkClient(c);
    c->flag.blocked_in_use = 0;
    clientToKeys_removeClientDataEntry(c);
}
