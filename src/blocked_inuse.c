/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BlockInuse - Client blocking mechanism for keys that are currently
 * in use by other operations.
 */

#include "server.h"
#include "blocked_inuse.h"

/* External hashtable functions from server.c */
extern uint64_t dictEncObjHash(const void *key);
extern int hashtableEncObjKeyCompare(const void *key1, const void *key2);
extern uint64_t hashtableClientHash(const void *key);
extern int hashtableClientKeyCompare(const void *key1, const void *key2);

// Internal blockInuse data structure
static hashtable *client_to_keys; /* Maps client pointers to a list of keys the client is blocked on. */
static hashtable *key_to_clients; /* Maps keys to a list of clients blocked on them. */

static void markClientBlocked(client *c) {
    serverAssert(c->flag.blocked == 0 && c->flag.unblocked == 0);
    c->flag.blockInuse_blocked = 1;
    c->flag.pending_command = 1;
}

/* ----------------------------- client_to_keys Hashtable util ------------------------- */
/* Entry for client_to_keys hashtable */
typedef struct {
    client *c;
    robj **keys;         /* Array of keys the client is blocked on */
    int n_keys;          /* Number of keys in the array */
    mstime_t blocked_at; /* Timestamp when client was blocked (from server.mstime) */
} clientDataEntry;

/* Hashtable callbacks */
static const void *clientDataEntryGetClient(const void *entry) {
    return ((clientDataEntry *)entry)->c;
}

static void clientDataEntryDestructor(void *entry) {
    clientDataEntry *e = entry;
    if (e->keys) {
        // Refcounts for all keys should be decreased before calling this function
        // Ensure that n_keys is 0 before freeing keys
        serverAssert(e->n_keys == 0);
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

// Return the clientDataEntry for a client, or NULL if not found.
static clientDataEntry *clientToKeys_getClientDataEntry(client *c) {
    clientDataEntry *entry;
    if (hashtableFind(client_to_keys, c, (void **)&entry)) {
        return entry;
    }
    return NULL;
}

// Create a new clientDataEntry for client and add it to client_to_keys.
static clientDataEntry *clientToKeys_addClientDataEntry(client *c, int nKeys) {
    serverAssert(!clientToKeys_getClientDataEntry(c)); // client must not already exist in the client_to_keys
    serverAssert(nKeys > 0);

    clientDataEntry *entry;
    entry = zcalloc(sizeof(clientDataEntry));
    entry->c = c;
    entry->n_keys = 0;
    entry->blocked_at = server.mstime;
    entry->keys = zmalloc(sizeof(robj *) * nKeys);
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
    clientDataEntry *entry = clientToKeys_getClientDataEntry(c);
    if (entry == NULL) return NULL;

    sds key_sds = objectGetKey(key);
    for (int i = 0; i < entry->n_keys; ++i) {
        sds curr_key = objectGetKey(entry->keys[i]);
        if (sdscmp(curr_key, key_sds) == 0) {
            decrRefCount(entry->keys[i]);
            entry->keys[i] = entry->keys[entry->n_keys - 1];
            entry->keys[entry->n_keys - 1] = NULL;
            entry->n_keys--;
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

    keyToClientsEntry *entry;
    entry = zcalloc(sizeof(keyToClientsEntry));
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
    clientDataEntry *entry = clientToKeys_getClientDataEntry(c);
    if (!entry) return;

    for (int i = 0; i < entry->n_keys; ++i) {
        robj *key = entry->keys[i];
        list *clientList = keyToClients_getBlockedClientsList(key);
        serverAssert(clientList != NULL);
        listDelNode(clientList, listSearchKey(clientList, c));
        if (listLength(clientList) == 0) keyToClients_deleteKey(key);
        decrRefCount(key);
        entry->keys[i] = NULL;
    }
    entry->n_keys = 0;
}


/* ----------------------------- API implementation ------------------------- */

/* Check if client is blocked by blockInuse */
int blockInuse_clientBlocked(client *c) {
    return c->flag.blockInuse_blocked;
}

/*
 * Initialize blockInuse data structures.
 */
void blockInuse_init(void) {
    serverAssert(!client_to_keys);
    serverAssert(!key_to_clients);
    client_to_keys = hashtableCreate(&clientDataHashtableType);
    key_to_clients = hashtableCreate(&keyToClientsHashtableType);
}

/*
 * Release blockInuse data structures.
 * Only allowed if no clients are currently blocked.
 */
void blockInuse_release(void) {
    serverAssert(blockInuse_getNumberOfBlockedClients() == 0);
    serverAssert(blockInuse_getNumberOfBlockedKeys() == 0);
    if (client_to_keys) {
        hashtableRelease(client_to_keys);
        client_to_keys = NULL;
    }
    if (key_to_clients) {
        hashtableRelease(key_to_clients);
        key_to_clients = NULL;
    }
}

/* Get the current number of clients blocked by blockInuse. */
int blockInuse_getNumberOfBlockedClients(void) {
    return hashtableSize(client_to_keys);
}

/* Get the current number of blocked keys by blockInuse. */
int blockInuse_getNumberOfBlockedKeys(void) {
    return hashtableSize(key_to_clients);
}

/* Block a client on a set of keys. */
void blockInuse_blockClientOnKeys(client *c, int nKeys, robj *keys[]) {
    serverAssert(!(blockInuse_clientBlocked(c) || (c)->flag.unblocked || (c)->flag.blocked));
    serverAssert(nKeys > 0);
    serverAssert(!c->flag.replica);
    for (int i = 0; i < nKeys; ++i) {
        serverAssert(keys[i]->type == OBJ_STRING);
        // Verify key exists in at least one database across the server
        int found = 0;
        for (int j = 0; j < server.dbnum; ++j) {
            if (lookupKeyRead(server.db[j], keys[i]) != NULL) {
                found = 1;
                break;
            }
        }
        serverAssert(found);
    }

    // Initialize clientDataEntry and insert into client_to_keys
    clientDataEntry *entry = clientToKeys_addClientDataEntry(c, nKeys);
    markClientBlocked(c);

    for (int i = 0; i < nKeys; ++i) {
        robj *key = keys[i];

        // Find or initialize keyToClientsEntry in key_to_clients
        list *blockedClientsList = keyToClients_getBlockedClientsList(key);
        if (!blockedClientsList) blockedClientsList = keyToClients_addEntry(key);

        // Deduplicate: add client only if it’s not already the last in the list
        listNode *last_client = listLast(blockedClientsList);
        if (last_client == NULL || last_client->value != c) {
            // Add client to the key’s blocked clients list
            listAddNodeTail(blockedClientsList, c);

            // Add key to the clientDataEntry and increment reference count
            incrRefCount(key);
            entry->keys[entry->n_keys] = key;
            entry->n_keys++;
        }
    }

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
void blockInuse_unblockClientsOnKey(robj *key) {
    list *blockedClientsList = keyToClients_getBlockedClientsList(key);
    if (blockedClientsList == NULL) return;

    serverAssert(listLength(blockedClientsList) > 0);

    while (listLength(blockedClientsList) > 0) {
        listNode *ln = listFirst(blockedClientsList);
        client *c = listNodeValue(ln);

        // Remove client from this key's blocked list
        listDelNode(blockedClientsList, ln);

        // Remove this key from the client's blocked key list
        clientDataEntry *entry = clientToKeys_removeKey(c, key);

        if (entry->n_keys == 0) {
            // Client has no more blocked keys → mark unblocked
            serverAssert(blockInuse_clientBlocked(c) && c->flag.unblocked == 0);
            c->flag.unblocked = 1;
            c->flag.blockInuse_blocked = 0;
            listAddNodeTail(server.unblocked_clients, c);

            // Remove clientDataEntry from client_to_keys
            clientToKeys_removeClientDataEntry(c);
        }
    }

    // Remove the key entry from key_to_clients
    keyToClients_deleteKey(key);
}

/*
 * Unblock all clients on all keys.
 */
void blockInuse_unblockClientsOnAllKeys(void) {
    hashtableIterator iter;
    hashtableInitIterator(&iter, key_to_clients, HASHTABLE_ITER_SAFE);
    void *entry;
    while (hashtableNext(&iter, &entry)) {
        keyToClientsEntry *e = entry;
        robj *key = e->key;
        incrRefCount(key);
        blockInuse_unblockClientsOnKey(key);
        decrRefCount(key);
    }
    hashtableCleanupIterator(&iter);
}

/*
 * Unlink a blocked client from the blockInuse mapping, the client must be blocked by blockInuse.
 */
void blockInuse_unlinkClient(client *c) {
    clientDataEntry *entry = clientToKeys_getClientDataEntry(c);
    if (entry == NULL) return; // Not found in client_to_keys

    serverAssert(blockInuse_clientBlocked(c) && c->flag.unblocked == 0 && c->flag.blocked == 0);

    // Remove client from all key-to-client lists
    keyToClients_unlinkClient(c);

    // Clear the blocked flag and remove clientDataEntry
    c->flag.blockInuse_blocked = 0;
    clientToKeys_removeClientDataEntry(c);
}
