/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 *
 * Client blocking mechanism for keys currently being processed by a
 * background thread.
 *
 * This module provides a specialized blocking system that prevents concurrent
 * access to keys that are actively being modified or processed by a background
 * thread. Unlike the generic blocking operations in blocked.c, this mechanism
 * blocks clients when they attempt to access keys that are marked as "in use"
 * by background operations such as bgIteration.
 *
 * Key features:
 *   - Blocks clients on multiple keys simultaneously
 *   - Automatically unblocks clients when all their requested keys become
 *     available
 *   - Maintains bidirectional mappings: client->keys and key->clients
 *   - Integrates with the server's event loop via processUnblockedClients()
 *
 * Workflow:
 *   1. blockInUse_blockClientOnKeys() -
 *      The client is marked as blocked_in_use.
 *      Two mappings are updated:
 *        - client -> keys: record all keys this client is waiting on.
 *        - key -> clients: for each key, add this client to the key’s
 *          blocking client list.
 *
 *   2. Keys are unblocked individually via blockInUse_unblockClientsOnKey().
 *      For each key:
 *        - all clients are removed from that key’s blocking list
 *          (key -> clients),
 *        - and the corresponding key entry is removed from each client’s
 *          client -> keys mapping.
 *
 *   3. After a key is processed, if a client is no longer blocked on
 *      any remaining keys (i.e., its client -> keys set becomes empty),
 *      the client is:
 *        - fully removed from the blocking mappings,
 *        - the blocked_in_use flag is cleared,
 *        - added to the server unblocked list,
 *        - and the unblocked flag is set.
 *
 *   4. processUnblockedClients() (called from beforeSleep()) -
 *      The client is removed from the unblocked list, the unblocked flag
 *      is cleared, and its read handler is restored.
 *
 *   5. If a parsed (but unexecuted) command exists, it is executed.
 *      The server then continues parsing and executing commands from the
 *      remaining input buffer until the buffer is empty or the client
 *      becomes blocked again.
 *
 *
 * This is used to ensure data consistency during operations that require
 * exclusive access to keys, preventing race conditions and maintaining
 * transactional integrity.
 */

#ifndef BLOCKED_INUSE_H__
#define BLOCKED_INUSE_H__

struct client; // defined in server.h

/* Check if client is blocked by blockInUse */
bool blockInUse_isClientBlocked(client *c);

/* Initialize blockInUse structures, must be called once during server startup. */
void blockInUse_init(void);

/* Free blockInUse data structures, unblocks all clients before cleanup. */
void blockInUse_release(void);

/* Return the number of clients currently blocked by blockInUse. */
int blockInUse_getNumberOfBlockedClients(void);

/* Return the number of keys currently blocked by blockInUse. */
int blockInUse_getNumberOfBlockedKeys(void);

/*
 * Block a client on a list of keys. Duplicate keys are deduplicated: a key is
 * skipped if the client is already the last entry in that key's blocked-clients
 * list.
 *
 * The caller MUST set c->flag.pending_command = 1 before calling this function.
 * This ensures the pending command is executed when the client is later
 * unblocked via processPendingCommandAndInputBuffer().
 * The caller should then return without executing the command.
 */
void blockInUse_blockClientOnKeys(client *c, int num_keys, robj *keys[]);

/*
 * Unblock clients blocked on the given key.
 *
 * A client is unblocked only when it has no remaining dependencies on any
 * blocked keys. Such clients are added to the server.unblocked_clients list and
 * resumed later during processUnblockedClients() in blocked.c.
 */
void blockInUse_unblockClientsOnKey(robj *key);

/*
 * Unblock all clients that are currently blocked by blockInUse, across all
 * keys they are waiting on.
 *
 * Iterates the key_to_clients hashtable (which only contains keys with at
 * least one blocked client) and unblocks every client on each such key.
 * Unblocked clients are queued for reprocessing via queueClientForReprocessing()
 * and will be resumed during processUnblockedClients().
 *
 * After this call, no clients remain blocked by blockInUse.
 */
void blockInUse_unblockClientsOnAllKeys(void);

/*
 * Unlink a client currently blocked by blockInUse. Typically used when
 * a client is being freed while still blocked (e.g., client-initiated disconnect).
 *
 * This function removes the client from all blockInUse data structures.
 */
void blockInUse_unlinkClient(client *c);

#endif
