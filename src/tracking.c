/* tracking.c - Client side caching: keys tracking and invalidation
 *
 * Copyright (c) 2019, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"

/* The tracking table is constituted by a radix tree of keys, each pointing
 * to a radix tree of client IDs, used to track the clients that may have
 * certain keys in their local, client side, cache.
 *
 * When a client enables tracking with "CLIENT TRACKING on", each key served to
 * the client is remembered in the table mapping the keys to the client IDs.
 * Later, when a key is modified, all the clients that may have local copy
 * of such key will receive an invalidation message.
 *
 * Clients will normally take frequently requested objects in memory, removing
 * them when invalidation messages are received. */
rax *TrackingTable = NULL;
rax *PrefixTable = NULL;
uint64_t TrackingTableTotalItems = 0; /* Total number of IDs stored across
                                         the whole tracking table. This gives
                                         a hint about the total memory we
                                         are using server side for CSC. */
robj *TrackingChannelName;

/* This is the structure that we have as value of the PrefixTable, and
 * represents the list of keys modified, and the list of clients that need
 * to be notified, for a given prefix. */
typedef struct bcastState {
    rax *keys;    /* Keys modified in the current event loop cycle. */
    rax *clients; /* Clients subscribed to the notification events for this
                     prefix. */
} bcastState;

/* Remove the tracking state from the client 'c'. Note that there is not much
 * to do for us here, if not to decrement the counter of the clients in
 * tracking mode, because we just store the ID of the client in the tracking
 * table, so we'll remove the ID reference in a lazy way. Otherwise, when a
 * client with many entries in the table is removed, it would cost a lot of
 * time to do the cleanup. */
void disableTracking(client *c) {
    /* If this client is in broadcasting mode, we need to unsubscribe it
     * from all the prefixes it is registered to. */
    if (c->flag.tracking_bcast) {
        raxIterator ri;
        raxStart(&ri, c->pubsub_data->client_tracking_prefixes);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
            void *result;
            int found = raxFind(PrefixTable, ri.key, ri.key_len, &result);
            serverAssert(found);
            bcastState *bs = result;
            raxRemove(bs->clients, (unsigned char *)&c, sizeof(c), NULL);
            /* Was it the last client? Remove the prefix from the
             * table. */
            if (raxSize(bs->clients) == 0) {
                raxFree(bs->clients);
                raxFree(bs->keys);
                zfree(bs);
                raxRemove(PrefixTable, ri.key, ri.key_len, NULL);
            }
        }
        raxStop(&ri);
        raxFree(c->pubsub_data->client_tracking_prefixes);
        c->pubsub_data->client_tracking_prefixes = NULL;
    }

    /* Clear flags and adjust the count. */
    if (c->flag.tracking) {
        server.tracking_clients--;
        c->flag.tracking = 0;
        c->flag.tracking_broken_redir = 0;
        c->flag.tracking_bcast = 0;
        c->flag.tracking_optin = 0;
        c->flag.tracking_optout = 0;
        c->flag.tracking_caching = 0;
        c->flag.tracking_noloop = 0;
    }
}

static int stringCheckPrefix(unsigned char *s1, size_t s1_len, unsigned char *s2, size_t s2_len) {
    size_t min_length = s1_len < s2_len ? s1_len : s2_len;
    return memcmp(s1, s2, min_length) == 0;
}

/* Check if any of the provided prefixes collide with one another or
 * with an existing prefix for the client. A collision is defined as two
 * prefixes that will emit an invalidation for the same key. If no prefix
 * collision is found, 1 is return, otherwise 0 is returned and the client
 * has an error emitted describing the error. */
int checkPrefixCollisionsOrReply(client *c, robj **prefixes, size_t numprefix) {
    for (size_t i = 0; i < numprefix; i++) {
        /* Check input list has no overlap with existing prefixes. */
        if (c->pubsub_data->client_tracking_prefixes) {
            raxIterator ri;
            raxStart(&ri, c->pubsub_data->client_tracking_prefixes);
            raxSeek(&ri, "^", NULL, 0);
            while (raxNext(&ri)) {
                if (stringCheckPrefix(ri.key, ri.key_len, objectGetVal(prefixes[i]), sdslen(objectGetVal(prefixes[i])))) {
                    sds collision = sdsnewlen(ri.key, ri.key_len);
                    addReplyErrorFormat(c,
                                        "Prefix '%s' overlaps with an existing prefix '%s'. "
                                        "Prefixes for a single client must not overlap.",
                                        (unsigned char *)objectGetVal(prefixes[i]), (unsigned char *)collision);
                    sdsfree(collision);
                    raxStop(&ri);
                    return 0;
                }
            }
            raxStop(&ri);
        }
        /* Check input has no overlap with itself. */
        for (size_t j = i + 1; j < numprefix; j++) {
            if (stringCheckPrefix(objectGetVal(prefixes[i]), sdslen(objectGetVal(prefixes[i])), objectGetVal(prefixes[j]),
                                  sdslen(objectGetVal(prefixes[j])))) {
                addReplyErrorFormat(c,
                                    "Prefix '%s' overlaps with another provided prefix '%s'. "
                                    "Prefixes for a single client must not overlap.",
                                    (unsigned char *)objectGetVal(prefixes[i]), (unsigned char *)objectGetVal(prefixes[j]));
                return 0;
            }
        }
    }
    return 1;
}

/* Set the client 'c' to track the prefix 'prefix'. If the client 'c' is
 * already registered for the specified prefix, no operation is performed. */
void enableBcastTrackingForPrefix(client *c, char *prefix, size_t plen) {
    void *result;
    bcastState *bs;
    /* If this is the first client subscribing to such prefix, create
     * the prefix in the table. */
    if (!raxFind(PrefixTable, (unsigned char *)prefix, plen, &result)) {
        bs = zmalloc(sizeof(*bs));
        bs->keys = raxNew();
        bs->clients = raxNew();
        raxInsert(PrefixTable, (unsigned char *)prefix, plen, bs, NULL);
    } else {
        bs = result;
    }
    if (raxTryInsert(bs->clients, (unsigned char *)&c, sizeof(c), NULL, NULL)) {
        if (c->pubsub_data->client_tracking_prefixes == NULL) c->pubsub_data->client_tracking_prefixes = raxNew();
        raxInsert(c->pubsub_data->client_tracking_prefixes, (unsigned char *)prefix, plen, NULL, NULL);
    }
}

/* Enable the tracking state for the client 'c', and as a side effect allocates
 * the tracking table if needed. If the 'redirect_to' argument is non zero, the
 * invalidation messages for this client will be sent to the client ID
 * specified by the 'redirect_to' argument. Note that if such client will
 * eventually get freed, we'll send a message to the original client to
 * inform it of the condition. Multiple clients can redirect the invalidation
 * messages to the same client ID. */
void enableTracking(client *c, uint64_t redirect_to, struct ClientFlags options, robj **prefix, size_t numprefix) {
    if (!c->flag.tracking) server.tracking_clients++;
    c->flag.tracking = 1;
    c->flag.tracking_broken_redir = 0;
    c->flag.tracking_bcast = 0;
    c->flag.tracking_optin = 0;
    c->flag.tracking_optout = 0;
    c->flag.tracking_noloop = 0;
    initClientPubSubData(c);
    c->pubsub_data->client_tracking_redirection = redirect_to;

    /* This may be the first client we ever enable. Create the tracking
     * table if it does not exist. */
    if (TrackingTable == NULL) {
        TrackingTable = raxNew();
        PrefixTable = raxNew();
        TrackingChannelName = createStringObject("__redis__:invalidate", 20);
    }

    /* For broadcasting, set the list of prefixes in the client. */
    if (options.tracking_bcast) {
        c->flag.tracking_bcast = 1;
        if (numprefix == 0) enableBcastTrackingForPrefix(c, "", 0);
        for (size_t j = 0; j < numprefix; j++) {
            sds sdsprefix = objectGetVal(prefix[j]);
            enableBcastTrackingForPrefix(c, sdsprefix, sdslen(sdsprefix));
        }
    }

    /* Set the remaining flags that don't need any special handling. */
    c->flag.tracking_optin = options.tracking_optin;
    c->flag.tracking_optout = options.tracking_optout;
    c->flag.tracking_noloop = options.tracking_noloop;
}

/* This function is called after the execution of a readonly command in the
 * case the client 'c' has keys tracking enabled and the tracking is not
 * in BCAST mode. It will populate the tracking invalidation table according
 * to the keys the user fetched, so that the server will know what are the clients
 * that should receive an invalidation message with certain groups of keys
 * are modified. */
void trackingRememberKeys(client *tracking, client *executing) {
    /* Return if we are in optin/out mode and the right CACHING command
     * was/wasn't given in order to modify the default behavior. */
    uint64_t optin = tracking->flag.tracking_optin;
    uint64_t optout = tracking->flag.tracking_optout;
    uint64_t caching_given = tracking->flag.tracking_caching;
    if ((optin && !caching_given) || (optout && caching_given)) return;

    getKeysResult result;
    initGetKeysResult(&result);
    int numkeys = getKeysFromCommandWithSpecs(executing->cmd, executing->argv, executing->argc, GET_KEYSPEC_DEFAULT, &result);
    if (!numkeys) {
        getKeysFreeResult(&result);
        return;
    }
    /* Shard channels are treated as special keys for client
     * library to rely on `COMMAND` command to discover the node
     * to connect to. These channels doesn't need to be tracked. */
    if (executing->cmd->flags & CMD_PUBSUB) {
        return;
    }

    keyReference *keys = result.keys;

    for (int j = 0; j < numkeys; j++) {
        int idx = keys[j].pos;
        sds sdskey = objectGetVal(executing->argv[idx]);
        void *result;
        rax *ids;
        if (!raxFind(TrackingTable, (unsigned char *)sdskey, sdslen(sdskey), &result)) {
            ids = raxNew();
            int inserted = raxTryInsert(TrackingTable, (unsigned char *)sdskey, sdslen(sdskey), ids, NULL);
            serverAssert(inserted == 1);
        } else {
            ids = result;
        }
        if (raxTryInsert(ids, (unsigned char *)&tracking->id, sizeof(tracking->id), NULL, NULL))
            TrackingTableTotalItems++;
    }
    getKeysFreeResult(&result);
}

/* Given a key name, this function sends an invalidation message in the
 * proper channel (depending on RESP version: PubSub or Push message) and
 * to the proper client (in case of redirection), in the context of the
 * client 'c' with tracking enabled.
 *
 * In case the 'proto' argument is non zero, the function will assume that
 * 'keyname' points to a buffer of 'keylen' bytes already expressed in the
 * form of RESP protocol. This is used for:
 * - In BCAST mode, to send an array of invalidated keys to all
 *   applicable clients
 * - Following a flush command, to send a single RESP NULL to indicate
 *   that all keys are now invalid. */
void sendTrackingMessage(client *c, char *keyname, size_t keylen, int proto) {
    struct ClientFlags old_flags = c->flag;
    c->flag.pushing = 1;

    int using_redirection = 0;
    if (c->pubsub_data->client_tracking_redirection) {
        client *redir = lookupClientByID(c->pubsub_data->client_tracking_redirection);
        if (!redir || redir->flag.close_after_reply || redir->flag.close_asap) {
            c->flag.tracking_broken_redir = 1;
            /* We need to signal to the original connection that we
             * are unable to send invalidation messages to the redirected
             * connection, because the client no longer exist. */
            if (c->resp > 2) {
                addReplyPushLen(c, 2);
                addReplyBulkCBuffer(c, "tracking-redir-broken", 21);
                addReplyLongLong(c, c->pubsub_data->client_tracking_redirection);
            }
            if (!old_flags.pushing) c->flag.pushing = 0;
            return;
        }
        if (!old_flags.pushing) c->flag.pushing = 0;
        c = redir;
        using_redirection = 1;
        old_flags = c->flag;
        c->flag.pushing = 1;
    }

    /* Only send such info for clients in RESP version 3 or more. However
     * if redirection is active, and the connection we redirect to is
     * in Pub/Sub mode, we can support the feature with RESP 2 as well,
     * by sending Pub/Sub messages in the __redis__:invalidate channel. */
    if (c->resp > 2) {
        addReplyPushLen(c, 2);
        addReplyBulkCBuffer(c, "invalidate", 10);
    } else if (using_redirection && c->flag.pubsub) {
        /* We use a static object to speedup things, however we assume
         * that addReplyPubsubMessage() will not take a reference. */
        addReplyPubsubMessage(c, TrackingChannelName, NULL, shared.messagebulk);
    } else {
        /* If are here, the client is neither using RESP3, nor is
         * redirecting to another client. We can't send anything to
         * it since RESP2 does not support push messages in the same
         * connection. */
        if (!old_flags.pushing) c->flag.pushing = 0;
        return;
    }

    /* Send the "value" part, which is the array of keys. */
    if (proto) {
        addReplyProto(c, keyname, keylen);
    } else {
        addReplyArrayLen(c, 1);
        addReplyBulkCBuffer(c, keyname, keylen);
    }
    updateClientMemUsageAndBucket(c);
    if (!old_flags.pushing) c->flag.pushing = 0;
}

/* This function is called when a key is modified in the server and in the case
 * we have at least one client with the BCAST mode enabled.
 * Its goal is to set the key in the right broadcast state if the key
 * matches one or more prefixes in the prefix table. Later when we
 * return to the event loop, we'll send invalidation messages to the
 * clients subscribed to each prefix. */
void trackingRememberKeyToBroadcast(client *c, char *keyname, size_t keylen) {
    raxIterator ri;
    raxStart(&ri, PrefixTable);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
        if (ri.key_len > keylen) continue;
        if (ri.key_len != 0 && memcmp(ri.key, keyname, ri.key_len) != 0) continue;
        bcastState *bs = ri.data;
        /* We insert the client pointer as associated value in the radix
         * tree. This way we know who was the client that did the last
         * change to the key, and can avoid sending the notification in the
         * case the client is in NOLOOP mode. */
        raxInsert(bs->keys, (unsigned char *)keyname, keylen, c, NULL);
    }
    raxStop(&ri);
}

/* This function is called from signalModifiedKey() or other places in the server
 * when a key changes value. In the context of keys tracking, our task here is
 * to send a notification to every client that may have keys about such caching
 * slot.
 *
 * Note that 'c' may be NULL in case the operation was performed outside the
 * context of a client modifying the database (for instance when we delete a
 * key because of expire).
 *
 * The last argument 'bcast' tells the function if it should also schedule
 * the key for broadcasting to clients in BCAST mode. This is the case when
 * the function is called from the server core once a key is modified, however
 * we also call the function in order to evict keys in the key table in case
 * of memory pressure: in that case the key didn't really change, so we want
 * just to notify the clients that are in the table for this key, that would
 * otherwise miss the fact we are no longer tracking the key for them. */
void trackingInvalidateKey(client *c, robj *keyobj, int bcast) {
    if (TrackingTable == NULL) return;

    unsigned char *key = (unsigned char *)objectGetVal(keyobj);
    size_t keylen = sdslen(objectGetVal(keyobj));

    if (bcast && raxSize(PrefixTable) > 0) trackingRememberKeyToBroadcast(c, (char *)key, keylen);

    void *result;
    if (!raxFind(TrackingTable, key, keylen, &result)) return;
    rax *ids = result;

    raxIterator ri;
    raxStart(&ri, ids);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
        uint64_t id;
        memcpy(&id, ri.key, sizeof(id));
        client *target = lookupClientByID(id);
        /* Note that if the client is in BCAST mode, we don't want to
         * send invalidation messages that were pending in the case
         * previously the client was not in BCAST mode. This can happen if
         * TRACKING is enabled normally, and then the client switches to
         * BCAST mode. */
        if (target == NULL || !(target->flag.tracking) || target->flag.tracking_bcast) {
            continue;
        }

        /* If the client enabled the NOLOOP mode, don't send notifications
         * about keys changed by the client itself. */
        if (target->flag.tracking_noloop && target == server.current_client) {
            continue;
        }

        /* If target is current client and it's executing a command, we need schedule key invalidation.
         * As the invalidation messages may be interleaved with command
         * response and should after command response. */
        if (target == server.current_client && (server.current_client->flag.executing_command)) {
            incrRefCount(keyobj);
            listAddNodeTail(server.tracking_pending_keys, keyobj);
        } else {
            sendTrackingMessage(target, (char *)objectGetVal(keyobj), sdslen(objectGetVal(keyobj)), 0);
        }
    }
    raxStop(&ri);

    /* Free the tracking table: we'll create the radix tree and populate it
     * again if more keys will be modified in this caching slot. */
    TrackingTableTotalItems -= raxSize(ids);
    raxFree(ids);
    raxRemove(TrackingTable, (unsigned char *)key, keylen, NULL);
}

void trackingHandlePendingKeyInvalidations(void) {
    if (!listLength(server.tracking_pending_keys)) return;

    /* Flush pending invalidation messages only when we are not in nested call.
     * So the messages are not interleaved with transaction response. */
    if (server.execution_nesting) return;

    listNode *ln;
    listIter li;

    listRewind(server.tracking_pending_keys, &li);
    while ((ln = listNext(&li)) != NULL) {
        robj *key = listNodeValue(ln);
        /* current_client maybe freed, so we need to send invalidation
         * message only when current_client is still alive */
        if (server.current_client != NULL) {
            if (key != NULL) {
                sendTrackingMessage(server.current_client, (char *)objectGetVal(key), sdslen(objectGetVal(key)), 0);
            } else {
                sendTrackingMessage(server.current_client, objectGetVal(shared.null[server.current_client->resp]),
                                    sdslen(objectGetVal(shared.null[server.current_client->resp])), 1);
            }
        }
        if (key != NULL) decrRefCount(key);
    }
    listEmpty(server.tracking_pending_keys);
}

/* This function is called when one or all of the databases are
 * flushed. Caching keys are not specific for each DB but are global:
 * currently what we do is send a special notification to clients with
 * tracking enabled, sending a RESP NULL, which means, "all the keys",
 * in order to avoid flooding clients with many invalidation messages
 * for all the keys they may hold.
 */
void freeTrackingRadixTreeCallback(void *rt) {
    raxFree(rt);
}

void freeTrackingRadixTree(rax *rt) {
    raxFreeWithCallback(rt, freeTrackingRadixTreeCallback);
}

/* A RESP NULL is sent to indicate that all keys are invalid */
void trackingInvalidateKeysOnFlush(int async) {
    if (server.tracking_clients) {
        listNode *ln;
        listIter li;
        listRewind(server.clients, &li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            if (c->flag.tracking) {
                if (c == server.current_client) {
                    /* We use a special NULL to indicate that we should send null */
                    listAddNodeTail(server.tracking_pending_keys, NULL);
                } else {
                    sendTrackingMessage(c, objectGetVal(shared.null[c->resp]), sdslen(objectGetVal(shared.null[c->resp])), 1);
                }
            }
        }
    }

    /* In case of FLUSHALL, reclaim all the memory used by tracking. */
    if (TrackingTable) {
        if (async) {
            freeTrackingRadixTreeAsync(TrackingTable);
        } else {
            freeTrackingRadixTree(TrackingTable);
        }
        TrackingTable = raxNew();
        TrackingTableTotalItems = 0;
    }
}

/* Tracking forces the server to remember information about which client may have
 * certain keys. In workloads where there are a lot of reads, but keys are
 * hardly modified, the amount of information we have to remember server side
 * could be a lot, with the number of keys being totally not bound.
 *
 * So the server allows the user to configure a maximum number of keys for the
 * invalidation table. This function makes sure that we don't go over the
 * specified fill rate: if we are over, we can just evict information about
 * a random key, and send invalidation messages to clients like if the key was
 * modified. */
void trackingLimitUsedSlots(void) {
    static unsigned int timeout_counter = 0;
    if (TrackingTable == NULL) return;
    if (server.tracking_table_max_keys == 0) return; /* No limits set. */
    size_t max_keys = server.tracking_table_max_keys;
    if (raxSize(TrackingTable) <= max_keys) {
        timeout_counter = 0;
        return; /* Limit not reached. */
    }

    /* We have to invalidate a few keys to reach the limit again. The effort
     * we do here is proportional to the number of times we entered this
     * function and found that we are still over the limit. */
    int effort = 100 * (timeout_counter + 1);

    /* We just remove one key after another by using a random walk. */
    raxIterator ri;
    raxStart(&ri, TrackingTable);
    while (effort > 0) {
        effort--;
        raxSeek(&ri, "^", NULL, 0);
        raxRandomWalk(&ri, 0);
        if (raxEOF(&ri)) break;
        robj *keyobj = createStringObject((char *)ri.key, ri.key_len);
        trackingInvalidateKey(NULL, keyobj, 0);
        decrRefCount(keyobj);
        if (raxSize(TrackingTable) <= max_keys) {
            timeout_counter = 0;
            raxStop(&ri);
            return; /* Return ASAP: we are again under the limit. */
        }
    }

    /* If we reach this point, we were not able to go under the configured
     * limit using the maximum effort we had for this run. */
    raxStop(&ri);
    timeout_counter++;
}

/* Hard cap on client-ID liveness checks one sweep step performs, bounding the
 * scratch space below. The effective per-step budget is the endtime deadline;
 * this cap is a memory bound, not a tuning knob. */
#define TRACKING_SWEEP_MAX_ITEMS_PER_STEP 1000

/* Time budget for one scheduled sweep step, and the guardrails for the
 * adaptive sweep period (see trackingSweepDeadClients below). */
#define TRACKING_SWEEP_TIME_BUDGET_US 100
#define TRACKING_SWEEP_MIN_PERIOD_MS 100
#define TRACKING_SWEEP_MAX_PERIOD_MS 60000

/* One bounded step of a table-wide sweep reclaiming disconnected clients'
 * IDs. IDs are normally removed lazily when their key is next modified
 * (trackingInvalidateKey); a client that disconnects while its keys are never
 * touched again would leak them forever, and cleaning up on disconnect would
 * break that path's O(1) guarantee. The resume cursor is a (key, id) pair so
 * a single heavily-tracked key cannot blow the per-step budget.
 *
 * The step stops at the 'endtime' monotonic deadline (0 = no time limit) or
 * after TRACKING_SWEEP_MAX_ITEMS_PER_STEP liveness checks, whichever comes
 * first. Adds the number of removed IDs to '*removed' (may be NULL); returns
 * 1 when the pass reached the end of the table, 0 when it stopped on budget.
 *
 * An ID is removed only if lookupClientByID(id) == NULL: IDs are never
 * reused, so a connected client's ID can never be removed. Keys whose inner
 * radix tree becomes empty are removed silently - nobody tracks them anymore,
 * so no invalidation is owed. */
int trackingSweepStep(monotime endtime, uint64_t *removed) {
    /* Resume cursor: sds copy of the last visited key (NULL = start of a new
     * pass) and, when the previous step stopped inside that key's inner radix
     * tree, the last checked ID within it. Copies rather than iterator
     * pointers: our removals may invalidate the iterator and the key memory
     * it points to. */
    static sds cursor_key = NULL;
    static uint64_t cursor_id = 0;
    static int cursor_in_key = 0;

    if (TrackingTable == NULL) {
        if (cursor_key != NULL) {
            sdsfree(cursor_key);
            cursor_key = NULL;
        }
        cursor_in_key = 0;
        return 1;
    }

    /* Keys emptied during this step, with their (empty but still valid)
     * inner rax. Freeing and removing them is deferred until the outer
     * iteration ends: removing mid-iteration would invalidate the iterator,
     * and deferring the raxFree keeps every outer entry valid for the walk.
     *
     * cap+1 entries suffice: every visited non-empty key consumes at least
     * one unit of the cap, and at most one visited key (the resume key) can
     * already be empty. The per-key dead batch is capped for the same
     * reason. */
    sds empty_keys[TRACKING_SWEEP_MAX_ITEMS_PER_STEP + 1];
    rax *empty_ids[TRACKING_SWEEP_MAX_ITEMS_PER_STEP + 1];
    uint64_t dead[TRACKING_SWEEP_MAX_ITEMS_PER_STEP];
    int num_empty = 0;
    int budget = TRACKING_SWEEP_MAX_ITEMS_PER_STEP;
    unsigned int checks_since_clock = 0;
    int out_of_time = 0;

    raxIterator ri;
    raxStart(&ri, TrackingTable);
    if (cursor_key == NULL) {
        raxSeek(&ri, "^", NULL, 0);
    } else if (cursor_in_key) {
        /* The previous call stopped inside cursor_key's inner radix tree:
         * revisit the same key to finish it. ">=" also handles the key having
         * been removed since (we then continue from the next key). */
        raxSeek(&ri, ">=", (unsigned char *)cursor_key, sdslen(cursor_key));
    } else {
        /* Resume strictly after the last fully-processed key. Using ">" is
         * safe even if that key was removed since the previous invocation. */
        raxSeek(&ri, ">", (unsigned char *)cursor_key, sdslen(cursor_key));
    }

    while (budget > 0 && !out_of_time && raxNext(&ri)) {
        rax *ids = ri.data;

        /* Resume mid-key only if this is the exact key the previous call
         * stopped in; it may have been removed (and the seek landed on its
         * successor), in which case we start from the first ID. */
        int resume_mid = cursor_in_key && cursor_key != NULL && ri.key_len == sdslen(cursor_key) &&
                         memcmp(ri.key, cursor_key, ri.key_len) == 0;
        cursor_in_key = 0;
        int stopped_mid = 0;

        /* Remember the key we are processing as the resume point. */
        sdsfree(cursor_key);
        cursor_key = sdsnewlen(ri.key, ri.key_len);

        /* Gather the dead IDs first, then remove them, so we never mutate the
         * inner radix tree while its iterator is live. */
        size_t num_dead = 0;
        raxIterator idi;
        raxStart(&idi, ids);
        if (resume_mid) {
            raxSeek(&idi, ">", (unsigned char *)&cursor_id, sizeof(cursor_id));
        } else {
            raxSeek(&idi, "^", NULL, 0);
        }
        while (raxNext(&idi)) {
            uint64_t id;
            memcpy(&id, idi.key, sizeof(id));
            /* Only a NULL lookup means the client is gone; a still-connected
             * client is always preserved. */
            if (lookupClientByID(id) == NULL) dead[num_dead++] = id;
            budget--;
            /* Check the deadline on a throttled cadence: reading the
             * monotonic clock for every ID would cost more than the liveness
             * check itself. */
            if (endtime != 0 && ++checks_since_clock >= 16) {
                checks_since_clock = 0;
                if (getMonotonicUs() >= endtime) out_of_time = 1;
            }
            if (budget == 0 || out_of_time) {
                /* Budget exhausted: resume within this key on the next step.
                 * ">" on the saved ID is safe even if we remove it below. */
                cursor_id = id;
                cursor_in_key = 1;
                stopped_mid = 1;
                break;
            }
        }
        raxStop(&idi);

        for (size_t j = 0; j < num_dead; j++) {
            if (raxRemove(ids, (unsigned char *)&dead[j], sizeof(dead[j]), NULL)) {
                TrackingTableTotalItems--;
                if (removed) (*removed)++;
            }
        }

        /* Fully swept and now empty: schedule the key for removal after
         * iteration (as trackingInvalidateKey does, minus the invalidation
         * send). A key left empty at a budget boundary is reclaimed when the
         * next step revisits it. */
        if (!stopped_mid && raxSize(ids) == 0) {
            empty_keys[num_empty] = sdsnewlen(ri.key, ri.key_len);
            empty_ids[num_empty] = ids;
            num_empty++;
        }
    }

    /* raxEOF is true only if the iterator was exhausted (we reached the end of
     * the table), as opposed to stopping because we ran out of budget. */
    int reached_end = raxEOF(&ri) != 0;
    raxStop(&ri);

    for (int j = 0; j < num_empty; j++) {
        raxFree(empty_ids[j]);
        raxRemove(TrackingTable, (unsigned char *)empty_keys[j], sdslen(empty_keys[j]), NULL);
        sdsfree(empty_keys[j]);
    }

    /* Full pass complete: restart from the beginning next time. */
    if (reached_end) {
        sdsfree(cursor_key);
        cursor_key = NULL;
        cursor_in_key = 0;
    }
    return reached_end;
}

/* Synchronously sweep the whole table until a full pass completes without
 * removing anything, i.e. until every ID left references a live client.
 * Driven by DEBUG SWEEP-TRACKING-TABLE; unbounded by design, debug only. */
void trackingSweepFull(void) {
    uint64_t removed;
    do {
        removed = 0;
        while (!trackingSweepStep(0, &removed))
            ;
    } while (removed > 0);
}

/* Scheduled entry point, called on every serverCron tick. Runs one
 * TRACKING_SWEEP_TIME_BUDGET_US step on an adaptive period: halved when a
 * step reclaims something, doubled when it finds nothing, clamped to
 * [TRACKING_SWEEP_MIN_PERIOD_MS, TRACKING_SWEEP_MAX_PERIOD_MS] and starting
 * at the slow end - a quiet server pays one 100us scan per minute, a burst
 * of disconnects quickly ramps reclamation up.
 *
 * Deliberately not gated on server.tracking_clients: the leak's typical
 * shape is a table full of dead IDs after every tracking client has
 * disconnected. */
void trackingSweepDeadClients(void) {
    static monotime next_run = 0;
    static long long period_ms = TRACKING_SWEEP_MAX_PERIOD_MS;

    monotime now = getMonotonicUs();
    if (now < next_run) return;

    uint64_t removed = 0;
    trackingSweepStep(now + TRACKING_SWEEP_TIME_BUDGET_US, &removed);

    if (removed > 0) {
        period_ms /= 2;
        if (period_ms < TRACKING_SWEEP_MIN_PERIOD_MS) period_ms = TRACKING_SWEEP_MIN_PERIOD_MS;
    } else {
        period_ms *= 2;
        if (period_ms > TRACKING_SWEEP_MAX_PERIOD_MS) period_ms = TRACKING_SWEEP_MAX_PERIOD_MS;
    }
    next_run = now + (monotime)period_ms * 1000;
}

/* Generate RESP for an array containing all the key names
 * in the 'keys' radix tree. If the client is not NULL, the list will not
 * include keys that were modified the last time by this client, in order
 * to implement the NOLOOP option.
 *
 * If the resulting array would be empty, NULL is returned instead. */
sds trackingBuildBroadcastReply(client *c, rax *keys) {
    raxIterator ri;
    uint64_t count;

    if (c == NULL) {
        count = raxSize(keys);
    } else {
        count = 0;
        raxStart(&ri, keys);
        raxSeek(&ri, "^", NULL, 0);
        while (raxNext(&ri)) {
            if (ri.data != c) count++;
        }
        raxStop(&ri);

        if (count == 0) return NULL;
    }

    /* Create the array reply with the list of keys once, then send
     * it to all the clients subscribed to this prefix. */
    char buf[32];
    size_t len = ll2string(buf, sizeof(buf), count);
    sds proto = sdsempty();
    proto = sdsMakeRoomFor(proto, count * 15);
    proto = sdscatlen(proto, "*", 1);
    proto = sdscatlen(proto, buf, len);
    proto = sdscatlen(proto, "\r\n", 2);
    raxStart(&ri, keys);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
        if (c && ri.data == c) continue;
        len = ll2string(buf, sizeof(buf), ri.key_len);
        proto = sdscatlen(proto, "$", 1);
        proto = sdscatlen(proto, buf, len);
        proto = sdscatlen(proto, "\r\n", 2);
        proto = sdscatlen(proto, ri.key, ri.key_len);
        proto = sdscatlen(proto, "\r\n", 2);
    }
    raxStop(&ri);
    return proto;
}

/* This function will run the prefixes of clients in BCAST mode and
 * keys that were modified about each prefix, and will send the
 * notifications to each client in each prefix. */
void trackingBroadcastInvalidationMessages(void) {
    raxIterator ri, ri2;

    /* Return ASAP if there is nothing to do here. */
    if (TrackingTable == NULL || !server.tracking_clients) return;

    raxStart(&ri, PrefixTable);
    raxSeek(&ri, "^", NULL, 0);

    /* For each prefix... */
    while (raxNext(&ri)) {
        bcastState *bs = ri.data;

        if (raxSize(bs->keys)) {
            /* Generate the common protocol for all the clients that are
             * not using the NOLOOP option. */
            sds proto = trackingBuildBroadcastReply(NULL, bs->keys);

            /* Send this array of keys to every client in the list. */
            raxStart(&ri2, bs->clients);
            raxSeek(&ri2, "^", NULL, 0);
            while (raxNext(&ri2)) {
                client *c;
                memcpy(&c, ri2.key, sizeof(c));
                if (c->flag.tracking_noloop) {
                    /* This client may have certain keys excluded. */
                    sds adhoc = trackingBuildBroadcastReply(c, bs->keys);
                    if (adhoc) {
                        sendTrackingMessage(c, adhoc, sdslen(adhoc), 1);
                        sdsfree(adhoc);
                    }
                } else {
                    sendTrackingMessage(c, proto, sdslen(proto), 1);
                }
            }
            raxStop(&ri2);

            /* Clean up: we can remove everything from this state, because we
             * want to only track the new keys that will be accumulated starting
             * from now. */
            sdsfree(proto);
        }
        raxFree(bs->keys);
        bs->keys = raxNew();
    }
    raxStop(&ri);
}

/* This is just used in order to access the amount of used slots in the
 * tracking table. */
uint64_t trackingGetTotalItems(void) {
    return TrackingTableTotalItems;
}

uint64_t trackingGetTotalKeys(void) {
    if (TrackingTable == NULL) return 0;
    return raxSize(TrackingTable);
}

/* Return the address of the global tracking table so that the active-defrag
 * stage can relocate the outer radix tree (and its inner radix trees) and write
 * back the possibly-relocated pointer. Returns a pointer to a NULL rax when
 * tracking has never been enabled. */
rax **getTrackingTable(void) {
    return &TrackingTable;
}

uint64_t trackingGetTotalPrefixes(void) {
    if (PrefixTable == NULL) return 0;
    return raxSize(PrefixTable);
}
