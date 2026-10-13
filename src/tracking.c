/* tracking.c - Client side caching: keys tracking and invalidation
 *
 * Copyright (c) 2019, Salvatore Sanfilippo <antirez at gmail dot com>
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
#include <stdbool.h>

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
                                         an hint about the total memory we
                                         are using server side for CSC. */
robj *TrackingChannelName;

/* This is the structure that we have as value of the PrefixTable, and
 * represents the list of keys modified, and the list of clients that need
 * to be notified, for a given prefix. */
typedef struct bcastState {
    rax *keys;      /* Keys modified in the current event loop cycle. */
    rax *clients;   /* Clients subscribed to the notification events for this
                       prefix. */
} bcastState;

/* Remove the tracking state from the client 'c'. Note that there is not much
 * to do for us here, if not to decrement the counter of the clients in
 * tracking mode, because we just store the ID of the client in the tracking
 * table, so we'll remove the ID reference in a lazy way. Otherwise when a
 * client with many entries in the table is removed, it would cost a lot of
 * time to do the cleanup. */
void disableTracking(client *c) {
    /* If this client is in broadcasting mode, we need to unsubscribe it
     * from all the prefixes it is registered to. */
    if (c->flags & CLIENT_TRACKING_BCAST) {
        raxIterator ri;
        raxStart(&ri,c->client_tracking_prefixes);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            bcastState *bs = raxFind(PrefixTable,ri.key,ri.key_len);
            serverAssert(bs != raxNotFound);
            raxRemove(bs->clients,(unsigned char*)&c,sizeof(c),NULL);
            /* Was it the last client? Remove the prefix from the
             * table. */
            if (raxSize(bs->clients) == 0) {
                raxFree(bs->clients);
                raxFree(bs->keys);
                zfree(bs);
                raxRemove(PrefixTable,ri.key,ri.key_len,NULL);
            }
        }
        raxStop(&ri);
        raxFree(c->client_tracking_prefixes);
        c->client_tracking_prefixes = NULL;
    }

    /* Clear flags and adjust the count. */
    if (c->flags & CLIENT_TRACKING) {
        server.tracking_clients--;
        c->flags &= ~(CLIENT_TRACKING|CLIENT_TRACKING_BROKEN_REDIR|
                      CLIENT_TRACKING_BCAST|CLIENT_TRACKING_OPTIN|
                      CLIENT_TRACKING_OPTOUT|CLIENT_TRACKING_CACHING|
                      CLIENT_TRACKING_NOLOOP);
    }
}

static int stringCheckPrefix(unsigned char *s1, size_t s1_len, unsigned char *s2, size_t s2_len) {
    size_t min_length = s1_len < s2_len ? s1_len : s2_len;
    return memcmp(s1,s2,min_length) == 0;   
}

/* Check if any of the provided prefixes collide with one another or
 * with an existing prefix for the client. A collision is defined as two 
 * prefixes that will emit an invalidation for the same key. If no prefix 
 * collision is found, 1 is return, otherwise 0 is returned and the client 
 * has an error emitted describing the error. */
int checkPrefixCollisionsOrReply(client *c, robj **prefixes, size_t numprefix) {
    for (size_t i = 0; i < numprefix; i++) {
        /* Check input list has no overlap with existing prefixes. */
        if (c->client_tracking_prefixes) {
            raxIterator ri;
            raxStart(&ri,c->client_tracking_prefixes);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                if (stringCheckPrefix(ri.key,ri.key_len,
                    prefixes[i]->ptr,sdslen(prefixes[i]->ptr))) 
                {
                    sds collision = sdsnewlen(ri.key,ri.key_len);
                    addReplyErrorFormat(c,
                        "Prefix '%s' overlaps with an existing prefix '%s'. "
                        "Prefixes for a single client must not overlap.",
                        (unsigned char *)prefixes[i]->ptr,
                        (unsigned char *)collision);
                    sdsfree(collision);
                    raxStop(&ri);
                    return 0;
                }
            }
            raxStop(&ri);
        }
        /* Check input has no overlap with itself. */
        for (size_t j = i + 1; j < numprefix; j++) {
            if (stringCheckPrefix(prefixes[i]->ptr,sdslen(prefixes[i]->ptr),
                prefixes[j]->ptr,sdslen(prefixes[j]->ptr)))
            {
                addReplyErrorFormat(c,
                    "Prefix '%s' overlaps with another provided prefix '%s'. "
                    "Prefixes for a single client must not overlap.",
                    (unsigned char *)prefixes[i]->ptr,
                    (unsigned char *)prefixes[j]->ptr);
                return i;
            }
        }
    }
    return 1;
}

/* Set the client 'c' to track the prefix 'prefix'. If the client 'c' is
 * already registered for the specified prefix, no operation is performed. */
void enableBcastTrackingForPrefix(client *c, char *prefix, size_t plen) {
    bcastState *bs = raxFind(PrefixTable,(unsigned char*)prefix,plen);
    /* If this is the first client subscribing to such prefix, create
     * the prefix in the table. */
    if (bs == raxNotFound) {
        bs = zmalloc(sizeof(*bs));
        bs->keys = raxNew();
        bs->clients = raxNew();
        raxInsert(PrefixTable,(unsigned char*)prefix,plen,bs,NULL);
    }
    if (raxTryInsert(bs->clients,(unsigned char*)&c,sizeof(c),NULL,NULL)) {
        if (c->client_tracking_prefixes == NULL)
            c->client_tracking_prefixes = raxNew();
        raxInsert(c->client_tracking_prefixes,
                  (unsigned char*)prefix,plen,NULL,NULL);
    }
}

/* Enable the tracking state for the client 'c', and as a side effect allocates
 * the tracking table if needed. If the 'redirect_to' argument is non zero, the
 * invalidation messages for this client will be sent to the client ID
 * specified by the 'redirect_to' argument. Note that if such client will
 * eventually get freed, we'll send a message to the original client to
 * inform it of the condition. Multiple clients can redirect the invalidation
 * messages to the same client ID. */
void enableTracking(client *c, uint64_t redirect_to, uint64_t options, robj **prefix, size_t numprefix) {
    if (!(c->flags & CLIENT_TRACKING)) server.tracking_clients++;
    c->flags |= CLIENT_TRACKING;
    c->flags &= ~(CLIENT_TRACKING_BROKEN_REDIR|CLIENT_TRACKING_BCAST|
                  CLIENT_TRACKING_OPTIN|CLIENT_TRACKING_OPTOUT|
                  CLIENT_TRACKING_NOLOOP);
    c->client_tracking_redirection = redirect_to;

    /* This may be the first client we ever enable. Create the tracking
     * table if it does not exist. */
    if (TrackingTable == NULL) {
        TrackingTable = raxNew();
        PrefixTable = raxNew();
        TrackingChannelName = createStringObject("__redis__:invalidate",20);
    }

    /* For broadcasting, set the list of prefixes in the client. */
    if (options & CLIENT_TRACKING_BCAST) {
        c->flags |= CLIENT_TRACKING_BCAST;
        if (numprefix == 0) enableBcastTrackingForPrefix(c,"",0);
        for (size_t j = 0; j < numprefix; j++) {
            sds sdsprefix = prefix[j]->ptr;
            enableBcastTrackingForPrefix(c,sdsprefix,sdslen(sdsprefix));
        }
    }

    /* Set the remaining flags that don't need any special handling. */
    c->flags |= options & (CLIENT_TRACKING_OPTIN|CLIENT_TRACKING_OPTOUT|
                           CLIENT_TRACKING_NOLOOP);
}

/* This function is called after the execution of a readonly command in the
 * case the client 'c' has keys tracking enabled and the tracking is not
 * in BCAST mode. It will populate the tracking invalidation table according
 * to the keys the user fetched, so that Redis will know what are the clients
 * that should receive an invalidation message with certain groups of keys
 * are modified. */
void trackingRememberKeys(client *tracking, client *executing) {
    /* Return if we are in optin/out mode and the right CACHING command
     * was/wasn't given in order to modify the default behavior. */
    uint64_t optin = tracking->flags & CLIENT_TRACKING_OPTIN;
    uint64_t optout = tracking->flags & CLIENT_TRACKING_OPTOUT;
    uint64_t caching_given = tracking->flags & CLIENT_TRACKING_CACHING;
    if ((optin && !caching_given) || (optout && caching_given)) return;

    getKeysResult result = GETKEYS_RESULT_INIT;
    int numkeys = getKeysFromCommand(executing->cmd,executing->argv,executing->argc,&result);
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

    for(int j = 0; j < numkeys; j++) {
        int idx = keys[j].pos;
        sds sdskey = executing->argv[idx]->ptr;
        rax *ids = raxFind(TrackingTable,(unsigned char*)sdskey,sdslen(sdskey));
        if (ids == raxNotFound) {
            ids = raxNew();
            int inserted = raxTryInsert(TrackingTable,(unsigned char*)sdskey,
                                        sdslen(sdskey),ids, NULL);
            serverAssert(inserted == 1);
        }
        if (raxTryInsert(ids,(unsigned char*)&tracking->id,sizeof(tracking->id),NULL,NULL))
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
 * form of Redis RESP protocol. This is used for:
 * - In BCAST mode, to send an array of invalidated keys to all
 *   applicable clients
 * - Following a flush command, to send a single RESP NULL to indicate
 *   that all keys are now invalid. */
void sendTrackingMessage(client *c, char *keyname, size_t keylen, int proto) {
    uint64_t old_flags = c->flags;
    c->flags |= CLIENT_PUSHING;

    int using_redirection = 0;
    if (c->client_tracking_redirection) {
        client *redir = lookupClientByID(c->client_tracking_redirection);
        if (!redir) {
            c->flags |= CLIENT_TRACKING_BROKEN_REDIR;
            /* We need to signal to the original connection that we
             * are unable to send invalidation messages to the redirected
             * connection, because the client no longer exist. */
            if (c->resp > 2) {
                addReplyPushLen(c,2);
                addReplyBulkCBuffer(c,"tracking-redir-broken",21);
                addReplyLongLong(c,c->client_tracking_redirection);
            }
            if (!(old_flags & CLIENT_PUSHING)) c->flags &= ~CLIENT_PUSHING;
            return;
        }
        if (!(old_flags & CLIENT_PUSHING)) c->flags &= ~CLIENT_PUSHING;
        c = redir;
        using_redirection = 1;
        old_flags = c->flags;
        c->flags |= CLIENT_PUSHING;
    }

    /* Only send such info for clients in RESP version 3 or more. However
     * if redirection is active, and the connection we redirect to is
     * in Pub/Sub mode, we can support the feature with RESP 2 as well,
     * by sending Pub/Sub messages in the __redis__:invalidate channel. */
    if (c->resp > 2) {
        addReplyPushLen(c,2);
        addReplyBulkCBuffer(c,"invalidate",10);
    } else if (using_redirection && c->flags & CLIENT_PUBSUB) {
        /* We use a static object to speedup things, however we assume
         * that addReplyPubsubMessage() will not take a reference. */
        addReplyPubsubMessage(c,TrackingChannelName,NULL,shared.messagebulk);
    } else {
        /* If are here, the client is not using RESP3, nor is
         * redirecting to another client. We can't send anything to
         * it since RESP2 does not support push messages in the same
         * connection. */
        if (!(old_flags & CLIENT_PUSHING)) c->flags &= ~CLIENT_PUSHING;
        return;
    }

    /* Send the "value" part, which is the array of keys. */
    if (proto) {
        addReplyProto(c,keyname,keylen);
    } else {
        addReplyArrayLen(c,1);
        addReplyBulkCBuffer(c,keyname,keylen);
    }
    updateClientMemUsageAndBucket(c);
    if (!(old_flags & CLIENT_PUSHING)) c->flags &= ~CLIENT_PUSHING;
}

/* This function is called when a key is modified in Redis and in the case
 * we have at least one client with the BCAST mode enabled.
 * Its goal is to set the key in the right broadcast state if the key
 * matches one or more prefixes in the prefix table. Later when we
 * return to the event loop, we'll send invalidation messages to the
 * clients subscribed to each prefix. */
void trackingRememberKeyToBroadcast(client *c, char *keyname, size_t keylen) {
    raxIterator ri;
    raxStart(&ri,PrefixTable);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        if (ri.key_len > keylen) continue;
        if (ri.key_len != 0 && memcmp(ri.key,keyname,ri.key_len) != 0)
            continue;
        bcastState *bs = ri.data;
        /* We insert the client pointer as associated value in the radix
         * tree. This way we know who was the client that did the last
         * change to the key, and can avoid sending the notification in the
         * case the client is in NOLOOP mode. */
        raxInsert(bs->keys,(unsigned char*)keyname,keylen,c,NULL);
    }
    raxStop(&ri);
}

/* This function is called from signalModifiedKey() or other places in Redis
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
 * the function is called from the Redis core once a key is modified, however
 * we also call the function in order to evict keys in the key table in case
 * of memory pressure: in that case the key didn't really change, so we want
 * just to notify the clients that are in the table for this key, that would
 * otherwise miss the fact we are no longer tracking the key for them. */
void trackingInvalidateKey(client *c, robj *keyobj, int bcast) {
    if (TrackingTable == NULL) return;

    unsigned char *key = (unsigned char*)keyobj->ptr;
    size_t keylen = sdslen(keyobj->ptr);

    if (bcast && raxSize(PrefixTable) > 0)
        trackingRememberKeyToBroadcast(c,(char *)key,keylen);

    rax *ids = raxFind(TrackingTable,key,keylen);
    if (ids == raxNotFound) return;

    raxIterator ri;
    raxStart(&ri,ids);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        uint64_t id;
        memcpy(&id,ri.key,sizeof(id));
        client *target = lookupClientByID(id);
        /* Note that if the client is in BCAST mode, we don't want to
         * send invalidation messages that were pending in the case
         * previously the client was not in BCAST mode. This can happen if
         * TRACKING is enabled normally, and then the client switches to
         * BCAST mode. */
        if (target == NULL ||
            !(target->flags & CLIENT_TRACKING)||
            target->flags & CLIENT_TRACKING_BCAST)
        {
            continue;
        }

        /* If the client enabled the NOLOOP mode, don't send notifications
         * about keys changed by the client itself. */
        if (target->flags & CLIENT_TRACKING_NOLOOP &&
            target == server.current_client)
        {
            continue;
        }

        /* If target is current client and it's executing a command, we need schedule key invalidation.
         * As the invalidation messages may be interleaved with command
         * response and should after command response. */
        if (target == server.current_client && (server.current_client->flags & CLIENT_EXECUTING_COMMAND)) {
            incrRefCount(keyobj);
            listAddNodeTail(server.tracking_pending_keys, keyobj);
        } else {
            sendTrackingMessage(target,(char *)keyobj->ptr,sdslen(keyobj->ptr),0);
        }
    }
    raxStop(&ri);

    /* Free the tracking table: we'll create the radix tree and populate it
     * again if more keys will be modified in this caching slot. */
    TrackingTableTotalItems -= raxSize(ids);
    raxFree(ids);
    raxRemove(TrackingTable,(unsigned char*)key,keylen,NULL);
}

void trackingHandlePendingKeyInvalidations(void) {
    if (!listLength(server.tracking_pending_keys)) return;

    /* Flush pending invalidation messages only when we are not in nested call.
     * So the messages are not interleaved with transaction response. */
    if (server.execution_nesting) return;

    listNode *ln;
    listIter li;

    listRewind(server.tracking_pending_keys,&li);
    while ((ln = listNext(&li)) != NULL) {
        robj *key = listNodeValue(ln);
        /* current_client maybe freed, so we need to send invalidation
         * message only when current_client is still alive */
        if (server.current_client != NULL) {
            if (key != NULL) {
                sendTrackingMessage(server.current_client,(char *)key->ptr,sdslen(key->ptr),0);
            } else {
                sendTrackingMessage(server.current_client,shared.null[server.current_client->resp]->ptr,
                    sdslen(shared.null[server.current_client->resp]->ptr),1);
            }
        }
        if (key != NULL) decrRefCount(key);
    }
    listEmpty(server.tracking_pending_keys);
}

/* This function is called when one or all the Redis databases are
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
    raxFreeWithCallback(rt,freeTrackingRadixTreeCallback);
}

/* A RESP NULL is sent to indicate that all keys are invalid */
void trackingInvalidateKeysOnFlush(int async) {
    if (server.tracking_clients) {
        listNode *ln;
        listIter li;
        listRewind(server.clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = listNodeValue(ln);
            if (c->flags & CLIENT_TRACKING) {
                if (c == server.current_client) {
                    /* We use a special NULL to indicate that we should send null */
                    listAddNodeTail(server.tracking_pending_keys,NULL);
                } else {
                    sendTrackingMessage(c,shared.null[c->resp]->ptr,sdslen(shared.null[c->resp]->ptr),1);
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

/* Maximum time in microseconds a single call to trackingLimitUsedSlots()
 * spends invalidating keys. */
#define TRACKING_EVICTION_TIME_LIMIT_US 500

/* Tracking forces Redis to remember information about which client may have
 * certain keys. In workloads where there are a lot of reads, but keys are
 * hardly modified, the amount of information we have to remember server side
 * could be a lot, with the number of keys being totally not bound.
 *
 * So Redis allows the user to configure a maximum number of keys for the
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
    int effort = 100 * (timeout_counter+1);
    monotime timer;
    elapsedStart(&timer);

    /* We just remove one key after another by using a random walk. */
    raxIterator ri;
    raxStart(&ri,TrackingTable);
    while(effort > 0) {
        effort--;
        raxSeek(&ri,"^",NULL,0);
        raxRandomWalk(&ri,0);
        if (raxEOF(&ri)) break;
        robj *keyobj = createStringObject((char*)ri.key,ri.key_len);
        trackingInvalidateKey(NULL,keyobj,0);
        decrRefCount(keyobj);
        if (raxSize(TrackingTable) <= max_keys) {
            timeout_counter = 0;
            raxStop(&ri);
            return; /* Return ASAP: we are again under the limit. */
        }
        /* A key can be tracked by any number of clients, so the number of
         * keys does not bound the work. Stop on time as well, the next calls
         * continue the eviction. */
        if (elapsedUs(timer) > TRACKING_EVICTION_TIME_LIMIT_US) break;
    }

    /* If we reach this point, we were not able to go under the configured
     * limit using the maximum effort we had for this run. */
    raxStop(&ri);
    timeout_counter++;
}

/* Time budget for one scheduled sweep step, the guardrails for the adaptive
 * sweep period (see trackingSweepDeadClients below), and how many liveness
 * checks a step performs between reads of the monotonic clock. */
#define TRACKING_SWEEP_TIME_BUDGET_US 100
#define TRACKING_SWEEP_MIN_PERIOD_MS 100
#define TRACKING_SWEEP_MAX_PERIOD_MS 60000
#define TRACKING_SWEEP_CHECKS_PER_CLOCK 16

/* Resume point of an incremental walk over the tracking table (used by both
 * the dead-ID sweep and the defrag stage): an sds copy of the last visited
 * outer key (NULL = start of a new pass) and, when the walk stopped inside
 * that key's inner radix tree, the last visited client ID within it. Copies
 * rather than iterator pointers: removals may invalidate the iterator and
 * the key memory it points to. */
typedef struct {
    sds key;
    uint64_t id;
    bool in_key;
} tracking_cursor;

static void resetCursor(tracking_cursor *cursor) {
    if (cursor->key != NULL) {
        sdsfree(cursor->key);
        cursor->key = NULL;
    }
    cursor->in_key = false;
}

/* One bounded step of a table-wide sweep reclaiming disconnected clients'
 * IDs. The lazy path (trackingInvalidateKey) only reclaims IDs when their
 * key is modified, so dead IDs on never-modified keys would leak forever;
 * cleaning up on disconnect instead would break that path's O(1) guarantee.
 * The resume cursor is a (key, id) pair so a single heavily-tracked key
 * cannot blow the per-step budget.
 *
 * Stops at the 'endtime' monotonic deadline. An 'endtime' of 0 means no
 * deadline: the step is a complete pass over the table from the beginning,
 * regardless of where a previous bounded step stopped. Sets '*removed' (may
 * be NULL) to this step's removal count on every return path, so callers
 * need not pre-initialize it. Returns 1 when the pass reached the end of the
 * table, 0 when it stopped on the deadline.
 *
 * An ID is removed only if lookupClientByID(id) == NULL; IDs are never
 * reused, so a connected client's ID never is. Keys whose inner radix tree
 * becomes empty are removed silently - nobody tracks them, so no
 * invalidation is owed. */
int trackingSweepStep(monotime endtime, uint64_t *removed) {
    static tracking_cursor sweep_cursor;

    if (removed)
        *removed = 0;

    if (TrackingTable == NULL) {
        resetCursor(&sweep_cursor);
        return 1;
    }
    if (endtime == 0)
        resetCursor(&sweep_cursor);

    unsigned int checks_since_clock = 0;
    bool out_of_time = false;

    raxIterator ri;
    raxStart(&ri, TrackingTable);
    if (sweep_cursor.key == NULL) {
        raxSeek(&ri, "^", NULL, 0);
    } else if (sweep_cursor.in_key) {
        /* The previous call stopped inside the cursor key's inner radix tree:
         * revisit the same key to finish it. ">=" also handles the key having
         * been removed since (we then continue from the next key). */
        raxSeek(&ri, ">=", (unsigned char *)sweep_cursor.key, sdslen(sweep_cursor.key));
    } else {
        /* Resume strictly after the last fully-processed key. Using ">" is
         * safe even if that key was removed since the previous invocation. */
        raxSeek(&ri, ">", (unsigned char *)sweep_cursor.key, sdslen(sweep_cursor.key));
    }

    while (!out_of_time && raxNext(&ri)) {
        rax *ids = ri.data;

        /* Resume mid-key only if this is the exact key the previous call
         * stopped in; it may have been removed (and the seek landed on its
         * successor), in which case we start from the first ID. */
        bool resume_mid = sweep_cursor.in_key && sweep_cursor.key != NULL && ri.key_len == sdslen(sweep_cursor.key) &&
                          memcmp(ri.key, sweep_cursor.key, ri.key_len) == 0;
        sweep_cursor.in_key = false;

        raxIterator idi;
        raxStart(&idi, ids);
        if (resume_mid) {
            raxSeek(&idi, ">", (unsigned char *)&sweep_cursor.id, sizeof(sweep_cursor.id));
        } else {
            raxSeek(&idi, "^", NULL, 0);
        }
        while (raxNext(&idi)) {
            uint64_t id;
            memcpy(&id, idi.key, sizeof(id));
            /* Only a NULL lookup means the client is gone; a still-connected
             * client is always preserved. */
            if (lookupClientByID(id) == NULL) {
                raxRemove(ids, (unsigned char *)&id, sizeof(id), NULL);
                TrackingTableTotalItems--;
                if (removed)
                    (*removed)++;
                /* The removal invalidated the iterator: re-seek past the
                 * removed ID, as the stream trimming code does. */
                raxSeek(&idi, ">=", (unsigned char *)&id, sizeof(id));
            }
            /* Check the deadline on a throttled cadence: reading the
             * monotonic clock for every ID would cost more than the liveness
             * check itself. */
            if (endtime != 0 && ++checks_since_clock >= TRACKING_SWEEP_CHECKS_PER_CLOCK) {
                checks_since_clock = 0;
                if (getMonotonicUs() >= endtime) {
                    /* Out of time: save the resume point (the only time the
                     * key is copied). ">" on the saved ID is correct whether
                     * or not we just removed it. */
                    out_of_time = true;
                    sdsfree(sweep_cursor.key);
                    sweep_cursor.key = sdsnewlen(ri.key, ri.key_len);
                    sweep_cursor.id = id;
                    sweep_cursor.in_key = true;
                    break;
                }
            }
        }
        raxStop(&idi);

        /* Inner tree drained: reclaim the key right away, then re-seek the
         * outer iterator past it. ri.key is the iterator's own copy of the
         * key, so it stays valid across the removal (as in streamTrim). If
         * we also just ran out of time inside this key, the saved cursor now
         * names a key that no longer exists: resume after it, not inside. */
        if (raxSize(ids) == 0) {
            raxFree(ids);
            raxRemove(TrackingTable, ri.key, ri.key_len, NULL);
            raxSeek(&ri, ">=", ri.key, ri.key_len);
            sweep_cursor.in_key = false;
        }
    }

    /* raxEOF is true only if the iterator was exhausted (we reached the end of
     * the table), as opposed to stopping on the deadline. */
    bool reached_end = raxEOF(&ri) != 0;
    raxStop(&ri);

    /* Full pass complete: restart from the beginning next time. */
    if (reached_end)
        resetCursor(&sweep_cursor);
    return reached_end;
}

/* Synchronously sweep the whole table. Driven by DEBUG SWEEP-TRACKING-TABLE;
 * unbounded by design, debug only. */
void trackingSweepFull(void) { trackingSweepStep(0, NULL); }

/* Scheduled entry point, called on every serverCron tick. Runs one
 * TRACKING_SWEEP_TIME_BUDGET_US step on an adaptive period: halved when a
 * step reclaims something, doubled when it finds nothing, clamped to
 * [TRACKING_SWEEP_MIN_PERIOD_MS, TRACKING_SWEEP_MAX_PERIOD_MS] and starting
 * at the slow end - a quiet server pays one 100us scan per minute, a burst
 * of disconnects quickly ramps reclamation up.
 *
 * Deliberately not gated on server.tracking_clients: the leak's typical
 * shape is a table full of dead IDs after every tracking client has
 * disconnected. It does bail out while the table itself doesn't exist
 * (tracking never enabled, or torn down), since there is nothing to sweep. */
void trackingSweepDeadClients(void) {
    /* When the next step is due, and the current adaptive period. */
    static monotime next_run = 0;
    static int period_ms = TRACKING_SWEEP_MAX_PERIOD_MS;

    if (TrackingTable == NULL) {
        /* Nothing to sweep; start from the slow end again if a table appears. */
        next_run = 0;
        period_ms = TRACKING_SWEEP_MAX_PERIOD_MS;
        return;
    }

    monotime now = getMonotonicUs();
    if (now < next_run)
        return;

    uint64_t removed;
    trackingSweepStep(now + TRACKING_SWEEP_TIME_BUDGET_US, &removed);

    if (removed > 0) {
        period_ms /= 2;
        if (period_ms < TRACKING_SWEEP_MIN_PERIOD_MS)
            period_ms = TRACKING_SWEEP_MIN_PERIOD_MS;
    } else {
        period_ms *= 2;
        if (period_ms > TRACKING_SWEEP_MAX_PERIOD_MS)
            period_ms = TRACKING_SWEEP_MAX_PERIOD_MS;
    }
    next_run = now + period_ms * 1000;
}

/* Generate Redis protocol for an array containing all the key names
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
        raxStart(&ri,keys);
        raxSeek(&ri,"^",NULL,0);
        while(raxNext(&ri)) {
            if (ri.data != c) count++;
        }
        raxStop(&ri);

        if (count == 0) return NULL;
    }

    /* Create the array reply with the list of keys once, then send
    * it to all the clients subscribed to this prefix. */
    char buf[32];
    size_t len = ll2string(buf,sizeof(buf),count);
    sds proto = sdsempty();
    proto = sdsMakeRoomFor(proto,count*15);
    proto = sdscatlen(proto,"*",1);
    proto = sdscatlen(proto,buf,len);
    proto = sdscatlen(proto,"\r\n",2);
    raxStart(&ri,keys);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        if (c && ri.data == c) continue;
        len = ll2string(buf,sizeof(buf),ri.key_len);
        proto = sdscatlen(proto,"$",1);
        proto = sdscatlen(proto,buf,len);
        proto = sdscatlen(proto,"\r\n",2);
        proto = sdscatlen(proto,ri.key,ri.key_len);
        proto = sdscatlen(proto,"\r\n",2);
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

    raxStart(&ri,PrefixTable);
    raxSeek(&ri,"^",NULL,0);

    /* For each prefix... */
    while(raxNext(&ri)) {
        bcastState *bs = ri.data;

        if (raxSize(bs->keys)) {
            /* Generate the common protocol for all the clients that are
             * not using the NOLOOP option. */
            sds proto = trackingBuildBroadcastReply(NULL,bs->keys);

            /* Send this array of keys to every client in the list. */
            raxStart(&ri2,bs->clients);
            raxSeek(&ri2,"^",NULL,0);
            while(raxNext(&ri2)) {
                client *c;
                memcpy(&c,ri2.key,sizeof(c));
                if (c->flags & CLIENT_TRACKING_NOLOOP) {
                    /* This client may have certain keys excluded. */
                    sds adhoc = trackingBuildBroadcastReply(c,bs->keys);
                    if (adhoc) {
                        sendTrackingMessage(c,adhoc,sdslen(adhoc),1);
                        sdsfree(adhoc);
                    }
                } else {
                    sendTrackingMessage(c,proto,sdslen(proto),1);
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

static int defragRaxNode(raxNode **noderef) {
    raxNode *newnode = activeDefragAlloc(*noderef);
    if (newnode) {
        *noderef = newnode;
        return 1;
    }
    return 0;
}

/* Incrementally defragment the client-side-caching tracking table; driven
 * by the tracking-table stage in defrag.c.
 *
 * The table can hold up to tracking-table-max-keys entries, each owning an
 * inner radix tree, and a single never-modified key can accumulate an
 * unbounded number of (dead) client IDs, so a synchronous pass over either
 * dimension could stall the event loop. Like the kvstore defrag stages, the
 * walk is cursor-based, but the cursor is a (key, client-ID) pair so it can
 * stop and resume inside one key's inner tree. Inner radix trees hold
 * client IDs as keys with no data values, so only their struct and nodes
 * need relocation.
 *
 * Returns 0 to be resumed from the exact ID it stopped at, 1 when the pass
 * over the table is complete (or there is nothing to defrag). Called with
 * endtime == 0 to reset the resume state at the start of a defrag cycle. */
int defragTrackingTable(monotime endtime) {
    static tracking_cursor defrag_cursor;
    /* Whether the outer rax struct and head node have been relocated in the
     * current pass. */
    static bool initialized = false;

    if (endtime == 0) {
        /* Required initialization at the start of each defrag cycle. */
        resetCursor(&defrag_cursor);
        initialized = false;
        return 0;
    }

    /* Tracking may never have been enabled, or may have been torn down (e.g.
     * by a flush) mid-cycle; either way there is nothing left to defrag. */
    if (TrackingTable == NULL) {
        resetCursor(&defrag_cursor);
        initialized = false;
        return 1;
    }

    /* Relocate the outer rax struct and its head node once per pass. If the
     * table is freed AND recreated between invocations (flush followed by new
     * tracking activity), 'initialized' stays set, so the new outer struct is
     * not relocated until the next pass; its keys are still walked and their
     * inner radix trees still defragged below. That one-pass gap is accepted
     * to keep the resume logic simple. */
    rax *rt = TrackingTable;
    if (!initialized) {
        rax *newrt = activeDefragAlloc(rt);
        if (newrt) {
            TrackingTable = newrt;
            rt = newrt;
        }
        defragRaxNode(&rt->head);
        initialized = true;
    }

    raxIterator ri;
    raxStart(&ri, rt);
    if (defrag_cursor.key == NULL) {
        /* Fresh pass: assign the node callback before the seek so the nodes
         * walked before the first key are covered too. */
        ri.node_cb = defragRaxNode;
        raxSeek(&ri, "^", NULL, 0);
    } else {
        /* Resume at the cursor key itself when the deadline hit inside its
         * inner radix tree, or strictly after it otherwise. Assign the node
         * callback after the seek so nodes on the path to the cursor key (already
         * defragged on the invocation that walked them) aren't re-walked.
         * Both seeks are safe even if the cursor key was removed since. */
        raxSeek(&ri, defrag_cursor.in_key ? ">=" : ">", (unsigned char *)defrag_cursor.key, sdslen(defrag_cursor.key));
        ri.node_cb = defragRaxNode;
    }

    unsigned int iterations = 0;
    long long prev_defragged = server.stat_active_defrag_hits;
    unsigned long long prev_scanned = server.stat_active_defrag_scanned;

    while (raxNext(&ri)) {
        rax *ids = ri.data;

        /* Resume mid-key only if this is the exact key the deadline hit in;
         * it may have been removed (and the seek landed on its successor), in
         * which case we treat the landed-on key as freshly started. */
        bool resume_mid = defrag_cursor.in_key && defrag_cursor.key != NULL &&
                          ri.key_len == sdslen(defrag_cursor.key) && memcmp(ri.key, defrag_cursor.key, ri.key_len) == 0;
        defrag_cursor.in_key = false;

        /* Starting this key (not resuming): relocate the inner rax struct and
         * its head node, writing the possibly-moved pointer back to the outer
         * entry. Node relocations during the walk below never move the rax
         * struct itself. */
        if (!resume_mid) {
            rax *newids = activeDefragAlloc(ids);
            if (newids)
                raxSetData(ri.node, ri.data = ids = newids);
            defragRaxNode(&ids->head);
        }

        raxIterator idi;
        raxStart(&idi, ids);
        if (resume_mid) {
            raxSeek(&idi, ">", (unsigned char *)&defrag_cursor.id, sizeof(defrag_cursor.id));
            idi.node_cb = defragRaxNode;
        } else {
            idi.node_cb = defragRaxNode;
            raxSeek(&idi, "^", NULL, 0);
        }
        while (raxNext(&idi)) {
            server.stat_active_defrag_scanned++;
            /* Deadline check inside the inner tree: one tracked key can hold
             * an unbounded number of (dead) client IDs, so within-key
             * progress must be interruptible too. */
            if (++iterations > 16 || server.stat_active_defrag_hits > prev_defragged ||
                server.stat_active_defrag_scanned - prev_scanned > 64) {
                if (getMonotonicUs() >= endtime) {
                    /* Save the resume point (the only time the key is copied). */
                    sdsfree(defrag_cursor.key);
                    defrag_cursor.key = sdsnewlen(ri.key, ri.key_len);
                    memcpy(&defrag_cursor.id, idi.key, sizeof(defrag_cursor.id));
                    defrag_cursor.in_key = true;
                    raxStop(&idi);
                    raxStop(&ri);
                    return 0;
                }
                iterations = 0;
                prev_defragged = server.stat_active_defrag_hits;
                prev_scanned = server.stat_active_defrag_scanned;
            }
        }
        raxStop(&idi);

        /* Between-keys deadline check, mirroring the other stages. */
        if (++iterations > 16 || server.stat_active_defrag_hits > prev_defragged ||
            server.stat_active_defrag_scanned - prev_scanned > 64) {
            if (getMonotonicUs() >= endtime) {
                /* Resume strictly after this fully-processed key. */
                sdsfree(defrag_cursor.key);
                defrag_cursor.key = sdsnewlen(ri.key, ri.key_len);
                defrag_cursor.in_key = false;
                raxStop(&ri);
                return 0;
            }
            iterations = 0;
            prev_defragged = server.stat_active_defrag_hits;
            prev_scanned = server.stat_active_defrag_scanned;
        }
    }
    raxStop(&ri);

    /* Completed a full pass over the table; reset for the next cycle. */
    resetCursor(&defrag_cursor);
    initialized = false;
    return 1;
}

/* Unit-test-only accessor (not declared in server.h; the test file forward
 * declares it): returns the address of the module-private tracking table so
 * tests can install and inspect a table without a full server. White-box by
 * nature - tests using it are coupled to tracking.c internals. */
rax **unitTestOnly_getTrackingTable(void) { return &TrackingTable; }

/* Unit-test-only: the number of liveness checks a sweep step performs between
 * clock reads, so tests can place IDs on a step boundary without duplicating
 * the constant. */
int unitTestOnly_trackingSweepChecksPerClock(void) { return TRACKING_SWEEP_CHECKS_PER_CLOCK; }

uint64_t trackingGetTotalPrefixes(void) {
    if (PrefixTable == NULL) return 0;
    return raxSize(PrefixTable);
}

#ifdef REDIS_TEST
#include "testhelp.h"
#define TRACKING_TEST_EXPECT(condition) test_cond("Tracking sweep: " #condition, condition)
#define TRACKING_TEST_ASSERT(condition)                                                                                \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            test_cond("Tracking sweep: " #condition, 0);                                                               \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static int trackingTestFind(rax *rt, unsigned char *key, size_t len, void **value) {
    *value = raxFind(rt, key, len);
    return *value != raxNotFound;
}

/* -----------------------------------------------------------------------------
 * Client-side caching: reclaiming dead client IDs from the tracking table.
 *
 * The sweep decides liveness with lookupClientByID(), i.e. server.clients_index,
 * so these tests install a fake client index and a hand-built tracking table.
 * They are white-box by design: budget boundaries, mid-key resumption and
 * cursor invalidation need precise table shapes that cannot be built or
 * observed through the public tracking API.
 * -------------------------------------------------------------------------- */

/* Internal to tracking.c and deliberately absent from server.h. */
extern uint64_t TrackingTableTotalItems;
int trackingSweepStep(monotime endtime, uint64_t *removed);
rax **unitTestOnly_getTrackingTable(void);
int unitTestOnly_trackingSweepChecksPerClock(void);

/* An endtime that is already in the past under the fake clock below: every
 * step stops at its first clock check. */
#define TRACKING_TEST_EXPIRED_DEADLINE ((monotime)1)

static monotime trackingTestFakeMonotimeUs;
static monotime trackingTestFakeGetMonotonicUs(void) { return trackingTestFakeMonotimeUs; }

/* Make lookupClientByID(id) return non-NULL. The key encoding matches
 * linkClient(): the id in network byte order. */
static void trackingTestAddLiveClient(uint64_t id) {
    client *c = zcalloc(sizeof(client));
    c->id = id;
    uint64_t be = htonu64(id);
    raxInsert(server.clients_index, (unsigned char *)&be, sizeof(be), c, NULL);
}

/* Register 'ids' as trackers of 'keyname' the way trackingRememberKeys does,
 * keeping TrackingTableTotalItems consistent with the table. */
static void trackingTestTrackKeyIds(rax *tt, const char *keyname, const uint64_t *ids, int n) {
    void *found;
    rax *inner;
    size_t klen = strlen(keyname);
    if (!trackingTestFind(tt, (unsigned char *)keyname, klen, &found)) {
        inner = raxNew();
        raxInsert(tt, (unsigned char *)keyname, klen, inner, NULL);
    } else {
        inner = found;
    }
    for (int i = 0; i < n; i++) {
        uint64_t id = ids[i];
        if (raxTryInsert(inner, (unsigned char *)&id, sizeof(id), NULL, NULL))
            TrackingTableTotalItems++;
    }
}

static int trackingTestKeyHasId(rax *inner, uint64_t id) {
    void *found;
    return trackingTestFind(inner, (unsigned char *)&id, sizeof(id), &found);
}

/* Count every ID in the table and report whether all of them reference a live client. */
static uint64_t trackingTestCountIdsAndCheckLive(rax *tt, int *all_live_out) {
    uint64_t total = 0;
    int all_live = 1;
    raxIterator ri;
    raxStart(&ri, tt);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri)) {
        rax *ids = ri.data;
        total += raxSize(ids);
        raxIterator idi;
        raxStart(&idi, ids);
        raxSeek(&idi, "^", NULL, 0);
        while (raxNext(&idi)) {
            uint64_t id;
            memcpy(&id, idi.key, sizeof(id));
            if (lookupClientByID(id) == NULL)
                all_live = 0;
        }
        raxStop(&idi);
    }
    raxStop(&ri);
    *all_live_out = all_live;
    return total;
}

/* Drop 'keyname' the way the tail of trackingInvalidateKey does (minus the
 * invalidation send), simulating a key invalidated between two sweep steps. */
static void trackingTestRemoveKey(rax *tt, const char *keyname) {
    void *found;
    size_t klen = strlen(keyname);
    if (!trackingTestFind(tt, (unsigned char *)keyname, klen, &found))
        return;
    rax *inner = found;
    TrackingTableTotalItems -= raxSize(inner);
    raxFree(inner);
    raxRemove(tt, (unsigned char *)keyname, klen, NULL);
}

/* Sweep with an expired deadline until the pass completes, checking on every
 * step that it made progress and kept the counter consistent with the table.
 * Returns the number of steps that stopped on the deadline. */
static int trackingTestSweepToEnd(rax *tt, uint64_t expected_live_total) {
    int steps = 0;
    uint64_t removed;
    while (!trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed)) {
        steps++;
        TRACKING_TEST_EXPECT(removed > 0);
        int all_live = 0;
        uint64_t actual = trackingTestCountIdsAndCheckLive(tt, &all_live);
        TRACKING_TEST_EXPECT(actual == TrackingTableTotalItems);
        TRACKING_TEST_EXPECT(actual >= expected_live_total);
        if (steps > 100000)
            break; /* never an open loop */
    }
    return steps;
}

/* Run 'body' against an empty tracking table, an empty fake client index and a
 * fake monotonic clock, then restore everything so nothing leaks between tests
 * (including when the body bails out on a failed assertion). */
static int runTrackingSweepTest(int (*body)(rax *tt)) {
    monotime (*orig_get_monotonic_us)(void) = getMonotonicUs;
    rax *saved_clients_index = server.clients_index;
    rax **tt = unitTestOnly_getTrackingTable();

    getMonotonicUs = trackingTestFakeGetMonotonicUs;
    trackingTestFakeMonotimeUs = 1000000;
    server.clients_index = raxNew();
    *tt = raxNew();
    TrackingTableTotalItems = 0;

    int result = body(*tt);

    raxIterator ri;
    raxStart(&ri, *tt);
    raxSeek(&ri, "^", NULL, 0);
    while (raxNext(&ri))
        raxFree(ri.data);
    raxStop(&ri);
    raxFree(*tt);
    *tt = NULL;
    TrackingTableTotalItems = 0;

    /* With no tracking table the sweep entry points forget their static state
     * (resume cursor, scheduler period). */
    trackingSweepStep(0, NULL);
    trackingSweepDeadClients();

    raxFreeWithCallback(server.clients_index, zfree);
    server.clients_index = saved_clients_index;
    getMonotonicUs = orig_get_monotonic_us;
    return result;
}

/* A mix of live and dead IDs is swept so that only live IDs remain, the counter
 * matches, and keys whose inner radix tree became empty are removed. The dead
 * ID in k1 sits between two live ones, so the post-removal re-seek must land on
 * the live successor and preserve it. */
static int trackingSweeperRemovesDeadIdsAndEmptyKeys(rax *tt) {
    uint64_t live1 = 1, dead2 = 2, live3 = 3;

    /* Clients 1 and 3 are connected; client 2 is gone (never registered). */
    trackingTestAddLiveClient(live1);
    trackingTestAddLiveClient(live3);

    uint64_t k1ids[3] = {live1, dead2, live3}; /* dead between two live */
    uint64_t k2ids[1] = {dead2};               /* only dead -> becomes empty */
    uint64_t k3ids[1] = {live3};               /* only live */
    trackingTestTrackKeyIds(tt, "k1", k1ids, 3);
    trackingTestTrackKeyIds(tt, "k2", k2ids, 1);
    trackingTestTrackKeyIds(tt, "k3", k3ids, 1);

    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 3);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 5);

    /* No deadline: a single step sweeps the whole table and reaches EOF. */
    TRACKING_TEST_EXPECT(trackingSweepStep(0, NULL) == 1);

    /* k2 became empty and was removed; k1 and k3 remain. */
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 2);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 3);

    void *found;
    TRACKING_TEST_EXPECT(!trackingTestFind(tt, (unsigned char *)"k2", 2, &found));

    /* k1 keeps both live IDs, including the one after the removed ID. */
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"k1", 2, &found));
    rax *k1inner = found;
    TRACKING_TEST_EXPECT(raxSize(k1inner) == 2);
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(k1inner, live1));
    TRACKING_TEST_EXPECT(!trackingTestKeyHasId(k1inner, dead2));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(k1inner, live3));

    /* k3's live id is preserved. */
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"k3", 2, &found));
    rax *k3inner = found;
    TRACKING_TEST_EXPECT(raxSize(k3inner) == 1);
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(k3inner, live3));

    /* The counter matches the surviving IDs, all of which are live. */
    int all_live = 0;
    uint64_t actual = trackingTestCountIdsAndCheckLive(tt, &all_live);
    TRACKING_TEST_EXPECT(actual == trackingGetTotalItems());
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperRemovesDeadIdsAndEmptyKeys(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperRemovesDeadIdsAndEmptyKeys);
}

/* The step reports the number of removed IDs through its out parameter (set,
 * not accumulated), and trackingSweepFull (DEBUG SWEEP-TRACKING-TABLE) is a
 * no-op on a clean table. */
static int trackingSweepStepReportsRemovedAndFullSweepDrains(rax *tt) {
    uint64_t kids[2] = {1, 2}; /* no clients registered: both dead */
    trackingTestTrackKeyIds(tt, "k1", kids, 2);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 2);

    /* One unbounded step drains the table and accounts for every reclaimed ID. */
    uint64_t removed;
    TRACKING_TEST_EXPECT(trackingSweepStep(0, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 2);
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 0);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);

    /* A step over a clean table reports zero. */
    TRACKING_TEST_EXPECT(trackingSweepStep(0, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 0);

    trackingSweepFull();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);
    return 0;
}

int test_trackingSweepStepReportsRemovedAndFullSweepDrains(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweepStepReportsRemovedAndFullSweepDrains);
}

/* The scheduled entry point (serverCron path) self-gates on an adaptive period:
 * a call that reclaims something halves it, a call that finds nothing doubles
 * it, clamped to [100ms, 60s]. Driven under the fake clock. */
static int trackingSweepSchedulerAdaptsPeriodUnderFakeClock(rax *tt) {
    uint64_t dead1 = 1, dead2 = 2, dead3 = 3;
    trackingTestTrackKeyIds(tt, "k1", &dead1, 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Freshly reset: the first call is due and the period starts at its 60s
     * maximum. Reclaiming halves it to 30s. */
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);

    /* Immediately after: inside the period, a new dead ID survives. */
    trackingTestTrackKeyIds(tt, "k2", &dead2, 1);
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Advance just under 30s: still gated. */
    trackingTestFakeMonotimeUs += 29 * 1000 * 1000;
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Cross 30s: due, reclaims, period halves again to 15s. */
    trackingTestFakeMonotimeUs += 2 * 1000 * 1000;
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);

    /* 15s later with nothing to reclaim: runs, finds nothing, period doubles to 30s. */
    trackingTestFakeMonotimeUs += 16 * 1000 * 1000;
    trackingSweepDeadClients();
    trackingTestTrackKeyIds(tt, "k3", &dead3, 1);
    /* 16s later: inside the doubled 30s period, so still gated. */
    trackingTestFakeMonotimeUs += 16 * 1000 * 1000;
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);
    /* Another 15s: past 30s, due. */
    trackingTestFakeMonotimeUs += 15 * 1000 * 1000;
    trackingSweepDeadClients();
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);
    return 0;
}

int test_trackingSweepSchedulerAdaptsPeriodUnderFakeClock(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweepSchedulerAdaptsPeriodUnderFakeClock);
}

/* Incrementality across keys: with more IDs than one expired-deadline step
 * covers, the sweep takes several steps, each making progress and never
 * touching a live ID, and completes the pass. */
static int trackingSweeperIsIncrementalAndResumesAcrossCalls(rax *tt) {
    const int total_keys = 300;
    const int ids_per_key = 4;
    uint64_t canary = 1000000;
    trackingTestAddLiveClient(canary);

    for (int i = 0; i < total_keys; i++) {
        char kn[16];
        /* Zero-padded so lexicographic order equals numeric order. */
        snprintf(kn, sizeof(kn), "key%03d", i);
        uint64_t dead_ids[4];
        for (int j = 0; j < ids_per_key; j++)
            dead_ids[j] = (uint64_t)(i * ids_per_key + j + 1); /* no client -> dead */
        trackingTestTrackKeyIds(tt, kn, dead_ids, ids_per_key);
    }
    trackingTestTrackKeyIds(tt, "key150", &canary, 1);

    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == (uint64_t)total_keys);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == (uint64_t)(total_keys * ids_per_key + 1));

    int steps = trackingTestSweepToEnd(tt, 1);
    TRACKING_TEST_EXPECT(steps > 1);

    /* Every dead ID and emptied key is gone; the canary's key survives with only the canary. */
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperIsIncrementalAndResumesAcrossCalls(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperIsIncrementalAndResumesAcrossCalls);
}

/* One key holding many more dead IDs than one step covers: the mid-key cursor
 * resumes inside the key across steps, the key survives while IDs remain, and
 * is reclaimed once drained. */
static int trackingSweeperBoundsWorkWithinOneHugeKey(rax *tt) {
    const uint64_t total_ids = 2500;

    for (uint64_t id = 1; id <= total_ids; id++)
        trackingTestTrackKeyIds(tt, "hotkey", &id, 1); /* all dead */
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids);

    /* The first step stops inside the key: the key is still there, with fewer IDs. */
    uint64_t removed;
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(removed > 0);
    TRACKING_TEST_EXPECT(removed < total_ids);
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids - removed);

    int steps = trackingTestSweepToEnd(tt, 0);
    TRACKING_TEST_EXPECT(steps > 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 0);
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 0);
    return 0;
}

int test_trackingSweeperBoundsWorkWithinOneHugeKey(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperBoundsWorkWithinOneHugeKey);
}

/* Live IDs scattered through a huge key are preserved across mid-key
 * resumptions, wherever the step boundaries fall. */
static int trackingSweeperPreservesLiveIdsInHugeKey(rax *tt) {
    const uint64_t total_ids = 1200;
    const uint64_t live_ids[3] = {5, 500, 1200};

    for (uint64_t id = 1; id <= total_ids; id++)
        trackingTestTrackKeyIds(tt, "hotkey", &id, 1);
    for (int i = 0; i < 3; i++)
        trackingTestAddLiveClient(live_ids[i]);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids);

    int steps = trackingTestSweepToEnd(tt, 3);
    TRACKING_TEST_EXPECT(steps > 1);

    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 3);
    void *found;
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"hotkey", 6, &found));
    rax *inner = found;
    for (int i = 0; i < 3; i++)
        TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, live_ids[i]));
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 3);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperPreservesLiveIdsInHugeKey(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperPreservesLiveIdsInHugeKey);
}

/* Boundary: a step that stops exactly on a live ID must neither remove it nor
 * skip past the IDs after it. Live IDs are placed precisely on the first two
 * stop points. */
static int trackingSweeperStopsOnLiveIdAndResumesPastIt(rax *tt) {
    const uint64_t s = (uint64_t)unitTestOnly_trackingSweepChecksPerClock();
    const uint64_t total_ids = 2 * s + 8;
    void *found;

    for (uint64_t id = 1; id <= total_ids; id++)
        trackingTestTrackKeyIds(tt, "k", &id, 1);
    trackingTestAddLiveClient(s);
    trackingTestAddLiveClient(2 * s);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids);

    uint64_t removed;
    /* Step 1: checks 1..s; removes 1..s-1; stops on the live ID s. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(removed == s - 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids - (s - 1));
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"k", 1, &found));
    rax *inner = found;
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, s));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, s + 1)); /* not yet visited */

    /* Step 2: resumes after s; removes s+1..2s-1; stops on the live ID 2s. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(removed == s - 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == total_ids - 2 * (s - 1));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, s));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, 2 * s));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(inner, 2 * s + 1));

    /* Step 3: the remaining 8 dead IDs (fewer than a clock cadence) are
     * removed, the key stays with its two live IDs, the pass completes. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 8);
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 2);
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 2);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperStopsOnLiveIdAndResumesPastIt(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperStopsOnLiveIdAndResumesPastIt);
}

/* Boundary: the deadline hits exactly as a key's last ID is removed. The
 * emptied key must be reclaimed right away, and the next step must start at
 * the successor key's first ID rather than resuming inside a key that no
 * longer exists. */
static int trackingSweeperReclaimsKeyEmptiedExactlyAtDeadline(rax *tt) {
    const uint64_t s = (uint64_t)unitTestOnly_trackingSweepChecksPerClock();
    uint64_t live = 100;
    void *found;

    /* "kA" < "kB" < "kC". kA holds exactly one step's worth of dead IDs. */
    for (uint64_t id = 1; id <= s; id++)
        trackingTestTrackKeyIds(tt, "kA", &id, 1);
    uint64_t kb_ids[3] = {live, 101, 102};
    trackingTestTrackKeyIds(tt, "kB", kb_ids, 3);
    uint64_t kc_id = 200;
    trackingTestTrackKeyIds(tt, "kC", &kc_id, 1);
    trackingTestAddLiveClient(live);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == s + 4);

    uint64_t removed;
    /* Step 1: drains kA on the s-th check, which is also the deadline check.
     * kA is reclaimed in place; kB and kC are untouched. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(removed == s);
    TRACKING_TEST_EXPECT(!trackingTestFind(tt, (unsigned char *)"kA", 2, &found));
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"kB", 2, &found));
    TRACKING_TEST_EXPECT(raxSize(found) == 3);
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"kC", 2, &found));
    TRACKING_TEST_EXPECT(raxSize(found) == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 4);

    /* Step 2: starts at kB's first ID (live, kept), removes 101 and 102,
     * drains and reclaims kC, and reaches EOF. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 3);
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"kB", 2, &found));
    rax *kb = found;
    TRACKING_TEST_EXPECT(raxSize(kb) == 1);
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(kb, live));
    TRACKING_TEST_EXPECT(!trackingTestFind(tt, (unsigned char *)"kC", 2, &found));
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);

    /* Step 3: a fresh pass finds nothing to do. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 0);
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperReclaimsKeyEmptiedExactlyAtDeadline(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperReclaimsKeyEmptiedExactlyAtDeadline);
}

/* The key the cursor stopped inside may be invalidated before the next call:
 * the ">=" seek must then land on the successor key and finish the pass. If
 * the same key name is re-created holding an ID that sorts before the saved
 * in-key cursor, that ID is missed by the resumed pass and reclaimed by the
 * next one: the guarantee is eventual reclamation, not single-pass completeness. */
static int trackingSweeperToleratesCursorKeyRemovalBetweenCalls(rax *tt) {
    const int mid_ids = 4 * unitTestOnly_trackingSweepChecksPerClock(); /* several steps' worth */
    uint64_t canary = 42;
    void *found;
    uint64_t removed;

    trackingTestAddLiveClient(canary);

    /* "kaaa" < "kmid" < "kzzz": the first step drains "kaaa" and stops inside "kmid". */
    for (int i = 0; i < 3; i++) {
        uint64_t dead_id = (uint64_t)(100 + i); /* no client -> dead */
        trackingTestTrackKeyIds(tt, "kaaa", &dead_id, 1);
    }
    for (int i = 0; i < mid_ids; i++) {
        /* Every ID carries 0xFF in its lowest byte so that, in either byte
         * order, the ID 1 re-inserted below sorts before any of them - and
         * thus before the saved in-key cursor. */
        uint64_t dead_id = ((uint64_t)(i + 1) << 8) | 0xFF;
        trackingTestTrackKeyIds(tt, "kmid", &dead_id, 1);
    }
    for (int i = 0; i < 5; i++) {
        uint64_t dead_id = (uint64_t)(9000 + i);
        trackingTestTrackKeyIds(tt, "kzzz", &dead_id, 1);
    }
    trackingTestTrackKeyIds(tt, "kzzz", &canary, 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == (uint64_t)(3 + mid_ids + 6));

    /* Call 1: stops inside "kmid". */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(!trackingTestFind(tt, (unsigned char *)"kaaa", 4, &found));
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"kmid", 4, &found));
    TRACKING_TEST_EXPECT(raxSize(found) > 0);

    /* Remove the cursor key between calls, as trackingInvalidateKey would. */
    trackingTestRemoveKey(tt, "kmid");
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 6);

    /* The ">=" seek lands on the successor "kzzz"; the pass finishes, its dead
     * IDs are reclaimed and the live canary survives. */
    trackingTestSweepToEnd(tt, 1);
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TRACKING_TEST_EXPECT(all_live == 1);

    /* Re-creation variant: park the cursor inside "kmid" again... */
    for (int i = 0; i < 3; i++) {
        uint64_t dead_id = (uint64_t)(200 + i);
        trackingTestTrackKeyIds(tt, "kaaa", &dead_id, 1);
    }
    for (int i = 0; i < mid_ids; i++) {
        uint64_t dead_id = ((uint64_t)(i + 1) << 8) | 0xFF;
        trackingTestTrackKeyIds(tt, "kmid", &dead_id, 1);
    }
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"kmid", 4, &found));

    /* ...remove it, then re-create the same key name with a single dead ID
     * that sorts before the saved cursor. */
    trackingTestRemoveKey(tt, "kmid");
    uint64_t small_dead = 1;
    trackingTestTrackKeyIds(tt, "kmid", &small_dead, 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 2);

    /* The resumed call matches the re-created key by name and seeks strictly
     * past the saved cursor, so ID 1 is missed by this pass while the rest of
     * the table is still processed to EOF. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 0);
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"kmid", 4, &found));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(found, small_dead));
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 2);

    /* The next pass restarts from the beginning and reclaims the missed ID. */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 1);
    TRACKING_TEST_EXPECT(!trackingTestFind(tt, (unsigned char *)"kmid", 4, &found));
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);
    all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperToleratesCursorKeyRemovalBetweenCalls(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperToleratesCursorKeyRemovalBetweenCalls);
}

/* A key added lexicographically before the in-flight cursor is not seen by the
 * current pass and is reclaimed on the next pass once the cursor wraps at EOF. */
static int trackingSweeperReclaimsKeyInsertedBehindCursorOnNextPass(rax *tt) {
    uint64_t canary = 42;
    void *found;
    uint64_t removed;

    trackingTestAddLiveClient(canary);

    /* "kc" holds more IDs than one step covers, so the first step stops
     * inside it, leaving "kd" untouched. */
    const char *names[4] = {"ka", "kc", "kd", "kf"};
    const int counts[4] = {2, 4 * unitTestOnly_trackingSweepChecksPerClock(), 5, 5};
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < counts[k]; i++) {
            uint64_t dead_id = (uint64_t)((k + 1) * 10000 + i); /* no client -> dead */
            trackingTestTrackKeyIds(tt, names[k], &dead_id, 1);
        }
    }
    trackingTestTrackKeyIds(tt, "kf", &canary, 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == (uint64_t)(counts[0] + counts[1] + counts[2] + counts[3] + 1));

    /* Call 1: "ka" is drained and removed; the cursor sits inside "kc". */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 0);
    TRACKING_TEST_EXPECT(!trackingTestFind(tt, (unsigned char *)"ka", 2, &found));
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"kc", 2, &found));
    TRACKING_TEST_EXPECT(raxSize(found) > 0);
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"kd", 2, &found));
    TRACKING_TEST_EXPECT(raxSize(found) == (uint64_t)counts[2]);

    /* Insert a new key behind the cursor: "ka" < "kb" < "kc". */
    uint64_t behind_dead = 7777;
    trackingTestTrackKeyIds(tt, "kb", &behind_dead, 1);

    /* Finish the pass: "kc" is drained and reclaimed, "kd" and "kf" are swept
     * and EOF resets the cursor. "kb" sits behind the cursor for the whole
     * pass and must be untouched. */
    trackingTestSweepToEnd(tt, 2);
    TRACKING_TEST_ASSERT(trackingTestFind(tt, (unsigned char *)"kb", 2, &found));
    TRACKING_TEST_EXPECT(trackingTestKeyHasId(found, behind_dead));
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 2); /* "kb" + "kf" (canary) */
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 2);

    /* The next pass starts from the beginning and reclaims "kb". */
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 1);
    TRACKING_TEST_EXPECT(!trackingTestFind(tt, (unsigned char *)"kb", 2, &found));
    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 1);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 1);
    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 1);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperReclaimsKeyInsertedBehindCursorOnNextPass(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperReclaimsKeyInsertedBehindCursorOnNextPass);
}

/* When every ID references a still-connected client, the sweeper removes
 * nothing and the counter is unchanged. */
static int trackingSweeperPreservesLiveIds(rax *tt) {
    uint64_t ka_ids[3] = {1, 2, 3};
    uint64_t kb_ids[2] = {1, 2};

    trackingTestAddLiveClient(1);
    trackingTestAddLiveClient(2);
    trackingTestAddLiveClient(3);

    trackingTestTrackKeyIds(tt, "ka", ka_ids, 3);
    trackingTestTrackKeyIds(tt, "kb", kb_ids, 2);

    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 2);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 5);

    /* A full pass (5 checks, under one clock cadence) removes nothing. */
    uint64_t removed;
    TRACKING_TEST_EXPECT(trackingSweepStep(TRACKING_TEST_EXPIRED_DEADLINE, &removed) == 1);
    TRACKING_TEST_EXPECT(removed == 0);

    TRACKING_TEST_EXPECT(trackingGetTotalKeys() == 2);
    TRACKING_TEST_EXPECT(trackingGetTotalItems() == 5);

    int all_live = 0;
    TRACKING_TEST_EXPECT(trackingTestCountIdsAndCheckLive(tt, &all_live) == 5);
    TRACKING_TEST_EXPECT(all_live == 1);
    return 0;
}

int test_trackingSweeperPreservesLiveIds(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    return runTrackingSweepTest(trackingSweeperPreservesLiveIds);
}

int trackingTest(int argc, char **argv, int flags) {
    int failed_before = __failed_tests;
    test_cond("test_trackingSweeperRemovesDeadIdsAndEmptyKeys",
              test_trackingSweeperRemovesDeadIdsAndEmptyKeys(argc, argv, flags) == 0);
    test_cond("test_trackingSweepStepReportsRemovedAndFullSweepDrains",
              test_trackingSweepStepReportsRemovedAndFullSweepDrains(argc, argv, flags) == 0);
    test_cond("test_trackingSweepSchedulerAdaptsPeriodUnderFakeClock",
              test_trackingSweepSchedulerAdaptsPeriodUnderFakeClock(argc, argv, flags) == 0);
    test_cond("test_trackingSweeperIsIncrementalAndResumesAcrossCalls",
              test_trackingSweeperIsIncrementalAndResumesAcrossCalls(argc, argv, flags) == 0);
    test_cond("test_trackingSweeperBoundsWorkWithinOneHugeKey",
              test_trackingSweeperBoundsWorkWithinOneHugeKey(argc, argv, flags) == 0);
    test_cond("test_trackingSweeperPreservesLiveIdsInHugeKey",
              test_trackingSweeperPreservesLiveIdsInHugeKey(argc, argv, flags) == 0);
    test_cond("test_trackingSweeperStopsOnLiveIdAndResumesPastIt",
              test_trackingSweeperStopsOnLiveIdAndResumesPastIt(argc, argv, flags) == 0);
    test_cond("test_trackingSweeperReclaimsKeyEmptiedExactlyAtDeadline",
              test_trackingSweeperReclaimsKeyEmptiedExactlyAtDeadline(argc, argv, flags) == 0);
    test_cond("test_trackingSweeperToleratesCursorKeyRemovalBetweenCalls",
              test_trackingSweeperToleratesCursorKeyRemovalBetweenCalls(argc, argv, flags) == 0);
    test_cond("test_trackingSweeperReclaimsKeyInsertedBehindCursorOnNextPass",
              test_trackingSweeperReclaimsKeyInsertedBehindCursorOnNextPass(argc, argv, flags) == 0);
    test_cond("test_trackingSweeperPreservesLiveIds", test_trackingSweeperPreservesLiveIds(argc, argv, flags) == 0);
    return __failed_tests != failed_before;
}
#endif
