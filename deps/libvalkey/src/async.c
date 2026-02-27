/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2011, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
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

#include "fmacros.h"

#include "alloc.h"
#include "read.h"
#include "valkey.h"

#include <stdlib.h>
#include <string.h>
#ifndef _MSC_VER
#include <strings.h>
#endif
#include "win32.h"

#include "async.h"
#include "async_private.h"
#include "net.h"
#include "valkey_private.h"
#include "vkutil.h"

#include <dict.h>
#include <sds.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>

#ifdef NDEBUG
#undef assert
#define assert(e) (void)(e)
#endif

typedef struct {
    sds command;
    size_t len;
    valkeyCallbackFn *user_callback;
    void *user_priv_data;
} ssubscribeCallbackData;

/* Forward declarations of valkey.c functions */
int valkeyAppendCmdLen(valkeyContext *c, const char *cmd, size_t len);

/* Functions managing dictionary of callbacks for pub/sub. */
static uint64_t callbackHash(const void *key) {
    return dictGenHashFunction((const unsigned char *)key,
                               sdslen((const sds)key));
}

static int callbackKeyCompare(const void *key1, const void *key2) {
    int l1, l2;

    l1 = sdslen((const sds)key1);
    l2 = sdslen((const sds)key2);
    if (l1 != l2)
        return 0;
    return memcmp(key1, key2, l1) == 0;
}

static void callbackKeyDestructor(void *key) {
    sdsfree((sds)key);
}

static void callbackValDestructor(void *val) {
    vk_free(val);
}

static dictType callbackDict = {
    .hashFunction = callbackHash,
    .keyCompare = callbackKeyCompare,
    .keyDestructor = callbackKeyDestructor,
    .valDestructor = callbackValDestructor};

static valkeyAsyncContext *valkeyAsyncInitialize(valkeyContext *c) {
    valkeyAsyncContext *ac;
    dict *channels = NULL, *patterns = NULL, *schannels = NULL;

    channels = dictCreate(&callbackDict);
    if (channels == NULL)
        goto oom;

    patterns = dictCreate(&callbackDict);
    if (patterns == NULL)
        goto oom;

    schannels = dictCreate(&callbackDict);
    if (schannels == NULL)
        goto oom;

    ac = vk_realloc(c, sizeof(valkeyAsyncContext));
    if (ac == NULL)
        goto oom;

    c = &(ac->c);

    /* The regular connect functions will always set the flag VALKEY_CONNECTED.
     * For the async API, we want to wait until the first write event is
     * received up before setting this flag, so reset it here. */
    c->flags &= ~VALKEY_CONNECTED;

    ac->err = 0;
    ac->errstr = NULL;
    ac->data = NULL;
    ac->dataCleanup = NULL;

    ac->ev.data = NULL;
    ac->ev.addRead = NULL;
    ac->ev.delRead = NULL;
    ac->ev.addWrite = NULL;
    ac->ev.delWrite = NULL;
    ac->ev.cleanup = NULL;
    ac->ev.scheduleTimer = NULL;

    ac->onConnect = NULL;
    ac->onDisconnect = NULL;

    ac->replies.head = NULL;
    ac->replies.tail = NULL;
    ac->sub.replies.head = NULL;
    ac->sub.replies.tail = NULL;
    ac->sub.channels = channels;
    ac->sub.patterns = patterns;
    ac->sub.schannels = schannels;
    ac->sub.pending_unsubs = 0;

    return ac;
oom:
    dictRelease(channels);
    dictRelease(patterns);
    dictRelease(schannels);
    return NULL;
}

/* We want the error field to be accessible directly instead of requiring
 * an indirection to the valkeyContext struct. */
static void valkeyAsyncCopyError(valkeyAsyncContext *ac) {
    if (!ac)
        return;

    valkeyContext *c = &(ac->c);
    ac->err = c->err;
    ac->errstr = c->errstr;
}

valkeyAsyncContext *valkeyAsyncConnectWithOptions(const valkeyOptions *options) {
    valkeyOptions myOptions = *options;
    valkeyContext *c;
    valkeyAsyncContext *ac;

    /* Clear any erroneously set sync callback and flag that we don't want to
     * use freeReplyObject by default. */
    myOptions.push_cb = NULL;
    myOptions.options |= VALKEY_OPT_NO_PUSH_AUTOFREE;

    myOptions.options |= VALKEY_OPT_NONBLOCK;
    c = valkeyConnectWithOptions(&myOptions);
    if (c == NULL) {
        return NULL;
    }

    ac = valkeyAsyncInitialize(c);
    if (ac == NULL) {
        valkeyFree(c);
        return NULL;
    }

    /* Set any configured async push handler */
    valkeyAsyncSetPushCallback(ac, myOptions.async_push_cb);

    valkeyAsyncCopyError(ac);
    return ac;
}

valkeyAsyncContext *valkeyAsyncConnect(const char *ip, int port) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, ip, port);
    return valkeyAsyncConnectWithOptions(&options);
}

valkeyAsyncContext *valkeyAsyncConnectBind(const char *ip, int port,
                                           const char *source_addr) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, ip, port);
    options.endpoint.tcp.source_addr = source_addr;
    return valkeyAsyncConnectWithOptions(&options);
}

valkeyAsyncContext *valkeyAsyncConnectBindWithReuse(const char *ip, int port,
                                                    const char *source_addr) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, ip, port);
    options.options |= VALKEY_OPT_REUSEADDR;
    options.endpoint.tcp.source_addr = source_addr;
    return valkeyAsyncConnectWithOptions(&options);
}

valkeyAsyncContext *valkeyAsyncConnectUnix(const char *path) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_UNIX(&options, path);
    return valkeyAsyncConnectWithOptions(&options);
}

int valkeyAsyncSetConnectCallback(valkeyAsyncContext *ac, valkeyConnectCallback *fn) {
    /* If already set, this is an error */
    if (ac->onConnect)
        return VALKEY_ERR;

    ac->onConnect = fn;

    /* The common way to detect an established connection is to wait for
     * the first write event to be fired. This assumes the related event
     * library functions are already set. */
    _EL_ADD_WRITE(ac);

    return VALKEY_OK;
}

int valkeyAsyncSetDisconnectCallback(valkeyAsyncContext *ac, valkeyDisconnectCallback *fn) {
    if (ac->onDisconnect == NULL) {
        ac->onDisconnect = fn;
        return VALKEY_OK;
    }
    return VALKEY_ERR;
}

/* Helper functions to push/shift callbacks */
static int valkeyPushCallback(valkeyCallbackList *list, valkeyCallback *source) {
    valkeyCallback *cb;

    /* Copy callback from stack to heap */
    cb = vk_malloc(sizeof(*cb));
    if (cb == NULL)
        return VALKEY_ERR_OOM;

    /* Otherwise cb will remain uninitialized but will be saved in the list */
    assert(source != NULL);
    if (source != NULL) {
        memcpy(cb, source, sizeof(*cb));
        cb->next = NULL;
    }

    /* Store callback in list */
    if (list->head == NULL)
        list->head = cb;
    if (list->tail != NULL)
        list->tail->next = cb;
    list->tail = cb;
    return VALKEY_OK;
}

static int valkeyShiftCallback(valkeyCallbackList *list, valkeyCallback *target) {
    valkeyCallback *cb = list->head;
    if (cb != NULL) {
        list->head = cb->next;
        if (cb == list->tail)
            list->tail = NULL;

        /* Copy callback from heap to stack */
        if (target != NULL)
            memcpy(target, cb, sizeof(*cb));
        vk_free(cb);
        return VALKEY_OK;
    }
    return VALKEY_ERR;
}

static void valkeyRunCallback(valkeyAsyncContext *ac, valkeyCallback *cb, valkeyReply *reply) {
    valkeyContext *c = &(ac->c);
    if (cb->fn != NULL) {
        c->flags |= VALKEY_IN_CALLBACK;
        cb->fn(ac, reply, cb->privdata);
        c->flags &= ~VALKEY_IN_CALLBACK;
    }
}

static void valkeyRunPushCallback(valkeyAsyncContext *ac, valkeyReply *reply) {
    if (ac->push_cb != NULL) {
        ac->c.flags |= VALKEY_IN_CALLBACK;
        ac->push_cb(ac, reply);
        ac->c.flags &= ~VALKEY_IN_CALLBACK;
    }
}

static void valkeyRunConnectCallback(valkeyAsyncContext *ac, int status) {
    if (ac->onConnect == NULL)
        return;

    if (!(ac->c.flags & VALKEY_IN_CALLBACK)) {
        ac->c.flags |= VALKEY_IN_CALLBACK;
        ac->onConnect(ac, status);
        ac->c.flags &= ~VALKEY_IN_CALLBACK;
    } else {
        /* already in callback */
        ac->onConnect(ac, status);
    }
}

static void valkeyRunDisconnectCallback(valkeyAsyncContext *ac, int status) {
    if (ac->onDisconnect) {
        if (!(ac->c.flags & VALKEY_IN_CALLBACK)) {
            ac->c.flags |= VALKEY_IN_CALLBACK;
            ac->onDisconnect(ac, status);
            ac->c.flags &= ~VALKEY_IN_CALLBACK;
        } else {
            /* already in callback */
            ac->onDisconnect(ac, status);
        }
    }
}

/* Helper function to free the context. */
static void valkeyAsyncFreeInternal(valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);
    valkeyCallback cb;
    dictIterator it;
    dictEntry *de;

    /* Execute pending callbacks with NULL reply. */
    while (valkeyShiftCallback(&ac->replies, &cb) == VALKEY_OK)
        valkeyRunCallback(ac, &cb, NULL);
    while (valkeyShiftCallback(&ac->sub.replies, &cb) == VALKEY_OK)
        valkeyRunCallback(ac, &cb, NULL);

    /* Run subscription callbacks with NULL reply */
    if (ac->sub.channels) {
        dictInitIterator(&it, ac->sub.channels);
        while ((de = dictNext(&it)) != NULL)
            valkeyRunCallback(ac, dictGetVal(de), NULL);

        dictRelease(ac->sub.channels);
    }

    if (ac->sub.patterns) {
        dictInitIterator(&it, ac->sub.patterns);
        while ((de = dictNext(&it)) != NULL)
            valkeyRunCallback(ac, dictGetVal(de), NULL);

        dictRelease(ac->sub.patterns);
    }

    if (ac->sub.schannels) {
        dictInitIterator(&it, ac->sub.schannels);
        while ((de = dictNext(&it)) != NULL)
            valkeyRunCallback(ac, dictGetVal(de), NULL);

        dictRelease(ac->sub.schannels);
    }

    /* Signal event lib to clean up */
    _EL_CLEANUP(ac);

    /* Execute disconnect callback. When valkeyAsyncFree() initiated destroying
     * this context, the status will always be VALKEY_OK. */
    if (c->flags & VALKEY_CONNECTED) {
        int status = ac->err == 0 ? VALKEY_OK : VALKEY_ERR;
        if (c->flags & VALKEY_FREEING)
            status = VALKEY_OK;
        valkeyRunDisconnectCallback(ac, status);
    }

    if (ac->dataCleanup) {
        ac->dataCleanup(ac->data);
    }

    /* Cleanup self */
    valkeyFree(c);
}

/* Free the async context. When this function is called from a callback,
 * control needs to be returned to valkeyProcessCallbacks() before actual
 * free'ing. To do so, a flag is set on the context which is picked up by
 * valkeyProcessCallbacks(). Otherwise, the context is immediately free'd. */
void valkeyAsyncFree(valkeyAsyncContext *ac) {
    if (ac == NULL)
        return;

    valkeyContext *c = &(ac->c);

    c->flags |= VALKEY_FREEING;
    if (!(c->flags & VALKEY_IN_CALLBACK))
        valkeyAsyncFreeInternal(ac);
}

/* Helper function to make the disconnect happen and clean up. */
void valkeyAsyncDisconnectInternal(valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);

    /* Make sure error is accessible if there is any */
    valkeyAsyncCopyError(ac);

    if (ac->err == 0) {
        /* For clean disconnects, there should be no pending callbacks. */
        int ret = valkeyShiftCallback(&ac->replies, NULL);
        assert(ret == VALKEY_ERR);
    } else {
        /* Disconnection is caused by an error, make sure that pending
         * callbacks cannot call new commands. */
        c->flags |= VALKEY_DISCONNECTING;
    }

    /* cleanup event library on disconnect.
     * this is safe to call multiple times */
    _EL_CLEANUP(ac);

    /* For non-clean disconnects, valkeyAsyncFreeInternal() will execute pending
     * callbacks with a NULL-reply. */
    if (!(c->flags & VALKEY_NO_AUTO_FREE)) {
        valkeyAsyncFreeInternal(ac);
    }
}

/* Tries to do a clean disconnect from the server, meaning it stops new commands
 * from being issued, but tries to flush the output buffer and execute
 * callbacks for all remaining replies. When this function is called from a
 * callback, there might be more replies and we can safely defer disconnecting
 * to valkeyProcessCallbacks(). Otherwise, we can only disconnect immediately
 * when there are no pending callbacks. */
void valkeyAsyncDisconnect(valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);
    c->flags |= VALKEY_DISCONNECTING;

    /** unset the auto-free flag here, because disconnect undoes this */
    c->flags &= ~VALKEY_NO_AUTO_FREE;
    if (!(c->flags & VALKEY_IN_CALLBACK) && ac->replies.head == NULL)
        valkeyAsyncDisconnectInternal(ac);
}

static int valkeyIsShardedVariant(const char *cstr) {
    return !strncasecmp("sm", cstr, 2) || /* smessage */
           !strncasecmp("ss", cstr, 2) || /* ssubscribe */
           !strncasecmp("sun", cstr, 3);  /* sunsubscribe */
}

static int valkeyGetSubscribeCallback(valkeyAsyncContext *ac, valkeyReply *reply, valkeyCallback *dstcb) {
    valkeyContext *c = &(ac->c);
    dict *callbacks;
    valkeyCallback *cb = NULL;
    dictEntry *de;
    int pvariant, svariant;
    char *stype;
    sds sname = NULL;

    /* Match reply with the expected format of a pushed message.
     * The type and number of elements (3 to 4) are specified at:
     * https://valkey.io/docs/topics/pubsub/#format-of-pushed-messages */
    if ((reply->type == VALKEY_REPLY_ARRAY && !(c->flags & VALKEY_SUPPORTS_PUSH) && reply->elements >= 3) ||
        reply->type == VALKEY_REPLY_PUSH) {
        assert(reply->element[0]->type == VALKEY_REPLY_STRING);
        stype = reply->element[0]->str;
        pvariant = (tolower(stype[0]) == 'p') ? 1 : 0;
        svariant = valkeyIsShardedVariant(stype);

        callbacks = pvariant ? ac->sub.patterns :
                    svariant ? ac->sub.schannels :
                               ac->sub.channels;

        /* Locate the right callback */
        if (reply->element[1]->type == VALKEY_REPLY_STRING) {
            sname = sdsnewlen(reply->element[1]->str, reply->element[1]->len);
            if (sname == NULL)
                goto oom;

            if ((de = dictFind(callbacks, sname)) != NULL) {
                cb = dictGetVal(de);
                memcpy(dstcb, cb, sizeof(*dstcb));
            }
        }

        /* If this is a subscribe reply decrease pending counter. */
        if (strcasecmp(stype + pvariant + svariant, "subscribe") == 0) {
            assert(cb != NULL);
            cb->pending_subs -= 1;
            cb->subscribed = 1;
        } else if (strcasecmp(stype + pvariant + svariant, "unsubscribe") == 0) {
            if (cb == NULL)
                ac->sub.pending_unsubs -= 1;
            else if (cb->pending_subs == 0)
                dictDelete(callbacks, sname);

            /* If this was the last unsubscribe message, revert to
             * non-subscribe mode. */
            assert(reply->element[2]->type == VALKEY_REPLY_INTEGER);

            /* Unset subscribed flag only when no pipelined pending subscribe
             * or pending unsubscribe replies. */
            if (reply->element[2]->integer == 0 &&
                dictSize(ac->sub.channels) == 0 &&
                dictSize(ac->sub.patterns) == 0 &&
                dictSize(ac->sub.schannels) == 0 &&
                ac->sub.pending_unsubs == 0) {
                c->flags &= ~VALKEY_SUBSCRIBED;

                /* Move ongoing regular command callbacks. */
                valkeyCallback cb;
                while (valkeyShiftCallback(&ac->sub.replies, &cb) == VALKEY_OK) {
                    valkeyPushCallback(&ac->replies, &cb);
                }
            }
        }
        sdsfree(sname);
    } else {
        /* Shift callback for pending command in subscribed context. */
        valkeyShiftCallback(&ac->sub.replies, dstcb);
    }
    return VALKEY_OK;
oom:
    valkeySetError(&(ac->c), VALKEY_ERR_OOM, "Out of memory");
    valkeyAsyncCopyError(ac);
    return VALKEY_ERR;
}

#define valkeyIsSpontaneousPushReply(r) \
    (valkeyIsPushReply(r) && !valkeyIsSubscribeReply(r))

static int valkeyIsSubscribeReply(valkeyReply *reply) {
    char *str;
    size_t len, off;

    /* We will always have at least one string with the subscribe/message type */
    if (reply->elements < 1 || reply->element[0]->type != VALKEY_REPLY_STRING ||
        reply->element[0]->len < sizeof("message") - 1) {
        return 0;
    }

    /* Get the string/len moving past 'p' if needed */
    off = tolower(reply->element[0]->str[0]) == 'p' || valkeyIsShardedVariant(reply->element[0]->str);
    str = reply->element[0]->str + off;
    len = reply->element[0]->len - off;

    return !strncasecmp(str, "subscribe", len) ||
           !strncasecmp(str, "message", len) ||
           !strncasecmp(str, "unsubscribe", len);
}

void valkeyProcessCallbacks(valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);
    void *reply = NULL;
    int status;

    while ((status = valkeyGetReply(c, &reply)) == VALKEY_OK) {
        if (reply == NULL) {
            /* When the connection is being disconnected and there are
             * no more replies, this is the cue to really disconnect. */
            if (c->flags & VALKEY_DISCONNECTING && sdslen(c->obuf) == 0 && ac->replies.head == NULL) {
                valkeyAsyncDisconnectInternal(ac);
                return;
            }
            /* When the connection is not being disconnected, simply stop
             * trying to get replies and wait for the next loop tick. */
            break;
        }

        /* Keep track of push message support for subscribe handling */
        if (valkeyIsPushReply(reply))
            c->flags |= VALKEY_SUPPORTS_PUSH;

        /* Send any non-subscribe related PUSH messages to our PUSH handler
         * while allowing subscribe related PUSH messages to pass through.
         * This allows existing code to be backward compatible and work in
         * either RESP2 or RESP3 mode. */
        if (valkeyIsSpontaneousPushReply(reply)) {
            valkeyRunPushCallback(ac, reply);
            c->reader->fn->freeObject(reply);
            continue;
        }

        /* Even if the context is subscribed, pending regular
         * callbacks will get a reply before pub/sub messages arrive. */
        valkeyCallback cb = {NULL, NULL, 0, 0, NULL};
        if (valkeyShiftCallback(&ac->replies, &cb) != VALKEY_OK) {
            /*
             * A spontaneous reply in a not-subscribed context can be the error
             * reply that is sent when a new connection exceeds the maximum
             * number of allowed connections on the server side.
             *
             * This is seen as an error instead of a regular reply because the
             * server closes the connection after sending it.
             *
             * To prevent the error from being overwritten by an EOF error the
             * connection is closed here. See issue #43.
             *
             * Another possibility is that the server is loading its dataset.
             * In this case we also want to close the connection, and have the
             * user wait until the server is ready to take our request.
             */
            if (((valkeyReply *)reply)->type != VALKEY_REPLY_ERROR) {
                /* No more regular callbacks and no errors, the context *must* be subscribed. */
                assert(c->flags & VALKEY_SUBSCRIBED);
                if (c->flags & VALKEY_SUBSCRIBED)
                    valkeyGetSubscribeCallback(ac, reply, &cb);
            } else if (
                (c->flags & VALKEY_SUBSCRIBED) && (((valkeyReply *)reply)->type == VALKEY_REPLY_ERROR) && (strncmp(((valkeyReply *)reply)->str, "MOVED", 5) == 0 || strncmp(((valkeyReply *)reply)->str, "CROSSSLOT", 9) == 0) && valkeyShiftCallback(&ac->sub.replies, &cb) == VALKEY_OK) {
                /* Ssubscribe error */
            } else {
                c->err = VALKEY_ERR_OTHER;
                snprintf(c->errstr, sizeof(c->errstr), "%s", ((valkeyReply *)reply)->str);
                c->reader->fn->freeObject(reply);
                valkeyAsyncDisconnectInternal(ac);
                return;
            }
        }

        if (cb.fn != NULL) {
            valkeyRunCallback(ac, &cb, reply);
            if (!(c->flags & VALKEY_NO_AUTO_FREE_REPLIES)) {
                c->reader->fn->freeObject(reply);
            }

            /* Proceed with free'ing when valkeyAsyncFree() was called. */
            if (c->flags & VALKEY_FREEING) {
                valkeyAsyncFreeInternal(ac);
                return;
            }
        } else {
            /* No callback for this reply. This can either be a NULL callback,
             * or there were no callbacks to begin with. Either way, don't
             * abort with an error, but simply ignore it because the client
             * doesn't know what the server will spit out over the wire. */
            c->reader->fn->freeObject(reply);
        }

        /* If in monitor mode, repush the callback */
        if (c->flags & VALKEY_MONITORING) {
            valkeyPushCallback(&ac->replies, &cb);
        }
    }

    /* Disconnect when there was an error reading the reply */
    if (status != VALKEY_OK)
        valkeyAsyncDisconnectInternal(ac);
}

static void valkeyAsyncHandleConnectFailure(valkeyAsyncContext *ac) {
    valkeyRunConnectCallback(ac, VALKEY_ERR);
    valkeyAsyncDisconnectInternal(ac);
}

/* Internal helper function to detect socket status the first time a read or
 * write event fires. When connecting was not successful, the connect callback
 * is called with a VALKEY_ERR status and the context is free'd. */
static int valkeyAsyncHandleConnect(valkeyAsyncContext *ac) {
    int completed = 0;
    valkeyContext *c = &(ac->c);

    if (valkeyCheckConnectDone(c, &completed) == VALKEY_ERR) {
        /* Error! */
        if (valkeyCheckSocketError(c) == VALKEY_ERR)
            valkeyAsyncCopyError(ac);
        valkeyAsyncHandleConnectFailure(ac);
        return VALKEY_ERR;
    } else if (completed == 1) {
        /* connected! */
        if (c->connection_type == VALKEY_CONN_TCP &&
            valkeySetTcpNoDelay(c) == VALKEY_ERR) {
            valkeyAsyncHandleConnectFailure(ac);
            return VALKEY_ERR;
        }

        /* flag us as fully connect, but allow the callback
         * to disconnect.  For that reason, permit the function
         * to delete the context here after callback return.
         */
        c->flags |= VALKEY_CONNECTED;
        valkeyRunConnectCallback(ac, VALKEY_OK);
        if ((ac->c.flags & VALKEY_DISCONNECTING)) {
            valkeyAsyncDisconnect(ac);
            return VALKEY_ERR;
        } else if ((ac->c.flags & VALKEY_FREEING)) {
            valkeyAsyncFree(ac);
            return VALKEY_ERR;
        }
        return VALKEY_OK;
    } else {
        return VALKEY_OK;
    }
}

void valkeyAsyncRead(valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);

    if (valkeyBufferRead(c) == VALKEY_ERR) {
        valkeyAsyncDisconnectInternal(ac);
    } else {
        /* Always re-schedule reads */
        _EL_ADD_READ(ac);
        valkeyProcessCallbacks(ac);
    }
}

/* This function should be called when the socket is readable.
 * It processes all replies that can be read and executes their callbacks.
 */
void valkeyAsyncHandleRead(valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);
    /* must not be called from a callback */
    assert(!(c->flags & VALKEY_IN_CALLBACK));

    if (!(c->flags & VALKEY_CONNECTED)) {
        /* Abort connect was not successful. */
        if (valkeyAsyncHandleConnect(ac) != VALKEY_OK)
            return;
        /* Try again later when the context is still not connected. */
        if (!(c->flags & VALKEY_CONNECTED))
            return;
    }

    c->funcs->async_read(ac);
}

void valkeyAsyncWrite(valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);
    int done = 0;

    if (valkeyBufferWrite(c, &done) == VALKEY_ERR) {
        valkeyAsyncDisconnectInternal(ac);
    } else {
        /* Continue writing when not done, stop writing otherwise */
        if (!done)
            _EL_ADD_WRITE(ac);
        else
            _EL_DEL_WRITE(ac);

        /* Always schedule reads after writes */
        _EL_ADD_READ(ac);
    }
}

void valkeyAsyncHandleWrite(valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);
    /* must not be called from a callback */
    assert(!(c->flags & VALKEY_IN_CALLBACK));

    if (!(c->flags & VALKEY_CONNECTED)) {
        /* Abort connect was not successful. */
        if (valkeyAsyncHandleConnect(ac) != VALKEY_OK)
            return;
        /* Try again later when the context is still not connected. */
        if (!(c->flags & VALKEY_CONNECTED))
            return;
    }

    c->funcs->async_write(ac);
}

void valkeyAsyncHandleTimeout(valkeyAsyncContext *ac) {
    valkeyContext *c = &(ac->c);
    valkeyCallback cb;
    /* must not be called from a callback */
    assert(!(c->flags & VALKEY_IN_CALLBACK));

    if ((c->flags & VALKEY_CONNECTED)) {
        if (ac->replies.head == NULL && ac->sub.replies.head == NULL) {
            /* Nothing to do - just an idle timeout */
            return;
        }

        if (!ac->c.command_timeout ||
            (!ac->c.command_timeout->tv_sec && !ac->c.command_timeout->tv_usec)) {
            /* A belated connect timeout arriving, ignore */
            return;
        }
    }

    if (!c->err) {
        valkeySetError(c, VALKEY_ERR_TIMEOUT, "Timeout");
        valkeyAsyncCopyError(ac);
    }

    if (!(c->flags & VALKEY_CONNECTED)) {
        valkeyRunConnectCallback(ac, VALKEY_ERR);
    }

    while (valkeyShiftCallback(&ac->replies, &cb) == VALKEY_OK) {
        valkeyRunCallback(ac, &cb, NULL);
    }

    /**
     * TODO: Don't automatically sever the connection,
     * rather, allow to ignore <x> responses before the queue is clear
     */
    valkeyAsyncDisconnectInternal(ac);
}

static inline int vk_isdigit_ascii(char c) {
    return (unsigned)(c - '0') < 10;
}

#define MAX_BULK_LEN (512ULL * 1024ULL * 1024ULL)
vk_static_assert(MAX_BULK_LEN < (UINT64_MAX - 9U) / 10);
static const char *parseBulkLen(const char *p, const char *end, uint64_t *len) {
    uint64_t acc = 0;

    assert(p != NULL && end != NULL && end - p >= 0 && len != NULL);

    if (end == p || !vk_isdigit_ascii(*p))
        return NULL;

    while (p < end && vk_isdigit_ascii(*p)) {
        unsigned d = *p - '0';

        acc = acc * 10 + d;
        if (acc > (uint64_t)MAX_BULK_LEN)
            return NULL;

        p++;
    }

    *len = acc;
    return p;
}

/* Find the next argument in a command buffer, i.e. find the next bulkstring
 * in an array of bulkstrings.
 * Returns a pointer to the end of a found argument, which can be used when
 * finding following arguments, or NULL when an argument is not found.
 * The found string is returned by pointer via `str` and length in `strlen`. */
static const char *nextArgument(const char *buf, size_t buflen, const char **str, size_t *strlen) {
    if (buf == NULL || buflen == 0)
        goto error;

    const char *p = buf;

    /* Find a bulkstring identifier. */
    if (p[0] != '$') {
        if ((p = memchr(p, '$', buflen)) == NULL)
            goto error;
    }
    p++; /* Skip found '$' */

    uint64_t len;

    p = parseBulkLen(p, buf + buflen, &len);
    if (p == NULL)
        goto error;

    /* Calculate end pointer for \r\n<payload>\r\n */
    const char *end = p + 2 + len + 2;

    /* Validate the parsed length and field separators. */
    if ((size_t)(end - buf) > buflen || p[0] != '\r' || p[len + 2] != '\r')
        goto error;

    /* Return pointer to the string, length, and pointer to next element. */
    *str = p + 2;
    *strlen = len;

    if ((size_t)(end - buf) == buflen) /* No more data in buffer? */
        return NULL;

    return end;

error:
    *str = NULL;
    *strlen = 0;
    return NULL;
}

void valkeySsubscribeCallback(struct valkeyAsyncContext *ac, void *reply, void *privdata) {
    /*
      This callback called on the first reply from ssubscribe:
      - on successful subscription:
          iterate over all channels specified in original ssubscribe command, assign them user provided callback and mark as subscribed, then call original user callback.
      - on failed ssubscribe:
          iterate over all channels specified in original ssubscribe command, reduce pending_subs and remove all not subscribed callbacks
    */
    valkeyReply *r = reply;
    ssubscribeCallbackData *data = privdata;
    size_t clen, alen;
    const char *p, *cstr, *astr;
    sds sname;
    valkeyCallback *cb = NULL;
    dictEntry *de;

    assert(data != NULL);
    assert(data->command != NULL);
    assert(r != NULL);
    if (r->type == VALKEY_REPLY_ERROR) {
        /*/ On CROSSSLOT, MOVED and other errors */
        p = nextArgument(data->command, data->len, &cstr, &clen);
        while ((p = nextArgument(p, data->len - (p - data->command), &astr, &alen)) != NULL || astr != NULL) {
            sname = sdsnewlen(astr, alen);
            if (sname == NULL)
                goto oom;

            if ((de = dictFind(ac->sub.schannels, sname)) != NULL) {
                cb = dictGetVal(de);
                if (cb != NULL) {
                    cb->pending_subs -= 1;
                    if (cb->pending_subs == 0 && !cb->subscribed) {
                        dictDelete(ac->sub.schannels, sname);
                    }
                }
            }
            sdsfree(sname);
        }
    } else {
        if ((r->type == VALKEY_REPLY_ARRAY || r->type == VALKEY_REPLY_PUSH) && strncasecmp(r->element[0]->str, "ssubscribe", 10) == 0) {
            p = nextArgument(data->command, data->len, &cstr, &clen);
            while ((p = nextArgument(p, data->len - (p - data->command), &astr, &alen)) != NULL || astr != NULL) {
                sname = sdsnewlen(astr, alen);
                if (sname == NULL)
                    goto oom;

                if ((de = dictFind(ac->sub.schannels, sname)) != NULL) {
                    cb = dictGetVal(de);
                    if (cb != NULL) {
                        cb->subscribed = 1;
                        cb->fn = data->user_callback;
                    }
                }
                sdsfree(sname);
            }
        }

        valkeyCallback cb = {0};
        valkeyGetSubscribeCallback(ac, reply, &cb);
        valkeyRunCallback(ac, &cb, reply);
        vk_free(data->command);
        vk_free(privdata);
        return;
    }

    data->user_callback(ac, reply, data->user_priv_data);
    vk_free(data->command);
    vk_free(privdata);
    return;
oom:
    sdsfree(sname);
    vk_free(data->command);
    vk_free(privdata);
}

/* Helper function for the valkeyAsyncCommand* family of functions. Writes a
 * formatted command to the output buffer and registers the provided callback
 * function with the context. */
static int valkeyAsyncAppendCmdLen(valkeyAsyncContext *ac, valkeyCallbackFn *fn, void *privdata, const char *cmd, size_t len) {
    valkeyContext *c = &(ac->c);
    valkeyCallback cb;
    struct dict *cbdict;
    dictIterator it;
    dictEntry *de;
    valkeyCallback *existcb;
    int pvariant, hasnext, hasprefix, svariant;
    const char *cstr, *astr;
    size_t clen, alen;
    const char *p;
    sds sname = NULL;
    ssubscribeCallbackData *ssubscribe_data = NULL;

    /* Don't accept new commands when the connection is about to be closed. */
    if (c->flags & (VALKEY_DISCONNECTING | VALKEY_FREEING))
        return VALKEY_ERR;

    /* Get the first string in the command, and don't accept empty commands. */
    p = nextArgument(cmd, len, &cstr, &clen);
    if (cstr == NULL)
        return VALKEY_ERR;

    hasnext = (p && (p[0] == '$'));
    pvariant = (tolower(cstr[0]) == 'p') ? 1 : 0;
    svariant = valkeyIsShardedVariant(cstr);
    hasprefix = svariant || pvariant;
    cstr += hasprefix;
    clen -= hasprefix;

    /* Setup callback */
    cb.fn = fn;
    cb.privdata = privdata;
    cb.pending_subs = 1;
    cb.unsubscribe_sent = 0;
    cb.subscribed = 0;

    if (hasnext && strncasecmp(cstr, "subscribe\r\n", 11) == 0) {
        int was_subscribed = c->flags & VALKEY_SUBSCRIBED;
        c->flags |= VALKEY_SUBSCRIBED;

        /* Add every channel/pattern to the list of subscription callbacks. */
        while ((p = nextArgument(p, len - (p - cmd), &astr, &alen)) != NULL || astr != NULL) {
            sname = sdsnewlen(astr, alen);
            if (sname == NULL)
                goto oom;

            cbdict = pvariant ? ac->sub.patterns :
                     svariant ? ac->sub.schannels :
                                ac->sub.channels;
            if (svariant) {
                cb.fn = NULL;
            }

            if ((de = dictFind(cbdict, sname)) != NULL) {
                existcb = dictGetVal(de);
                cb.pending_subs = existcb->pending_subs + 1;
                cb.subscribed = existcb->subscribed;
                cb.fn = existcb->fn;
            }

            /* Create a duplicate to be stored in dict. */
            valkeyCallback *dup = vk_malloc(sizeof(*dup));
            if (dup == NULL)
                goto oom;
            memcpy(dup, &cb, sizeof(*dup));

            if (dictReplace(cbdict, sname, dup) == 0)
                sdsfree(sname);
        }

        if (svariant) {
            ssubscribe_data = vk_malloc(sizeof(*ssubscribe_data));
            if (ssubscribe_data == NULL)
                goto oom;

            /* Copy command to iterate over all channels. */
            ssubscribe_data->command = vk_malloc(len);
            if (ssubscribe_data->command == NULL)
                goto oom;
            memcpy(ssubscribe_data->command, cmd, len);
            ssubscribe_data->len = len;
            ssubscribe_data->user_callback = fn;
            ssubscribe_data->user_priv_data = privdata;

            cb.fn = &valkeySsubscribeCallback;
            cb.privdata = ssubscribe_data;
            cb.pending_subs = 1;
            cb.unsubscribe_sent = 0;
            cb.subscribed = 1;
            if (was_subscribed) {
                if (valkeyPushCallback(&ac->sub.replies, &cb) != VALKEY_OK)
                    goto oom;
            } else {
                if (valkeyPushCallback(&ac->replies, &cb) != VALKEY_OK)
                    goto oom;
            }
        }
    } else if (strncasecmp(cstr, "unsubscribe\r\n", 13) == 0) {
        /* It is only useful to call (P)UNSUBSCRIBE when the context is
         * subscribed to one or more channels or patterns. */
        if (!(c->flags & VALKEY_SUBSCRIBED))
            return VALKEY_ERR;

        cbdict = pvariant ? ac->sub.patterns :
                 svariant ? ac->sub.schannels :
                            ac->sub.channels;

        if (hasnext) {
            /* Send an unsubscribe with specific channels/patterns.
             * Bookkeeping the number of expected replies */
            while ((p = nextArgument(p, len - (p - cmd), &astr, &alen)) != NULL || astr != NULL) {
                sname = sdsnewlen(astr, alen);
                if (sname == NULL)
                    goto oom;

                de = dictFind(cbdict, sname);
                if (de != NULL) {
                    existcb = dictGetVal(de);
                    if (existcb->unsubscribe_sent == 0)
                        existcb->unsubscribe_sent = 1;
                    else
                        /* Already sent, reply to be ignored */
                        ac->sub.pending_unsubs += 1;
                } else {
                    /* Not subscribed to, reply to be ignored */
                    ac->sub.pending_unsubs += 1;
                }
                sdsfree(sname);
            }
        } else {
            /* Send an unsubscribe without specific channels/patterns.
             * Bookkeeping the number of expected replies */
            int no_subs = 1;
            dictInitIterator(&it, cbdict);
            while ((de = dictNext(&it)) != NULL) {
                existcb = dictGetVal(de);
                if (existcb->unsubscribe_sent == 0) {
                    existcb->unsubscribe_sent = 1;
                    no_subs = 0;
                }
            }
            /* Unsubscribing to all channels/patterns, where none is
             * subscribed to, results in a single reply to be ignored. */
            if (no_subs == 1)
                ac->sub.pending_unsubs += 1;
        }

        /* (P)UNSUBSCRIBE does not have its own response: every channel or
         * pattern that is unsubscribed will receive a message. This means we
         * should not append a callback function for this command. */
    } else if (strncasecmp(cstr, "monitor\r\n", 9) == 0) {
        /* Set monitor flag and push callback */
        c->flags |= VALKEY_MONITORING;
        if (valkeyPushCallback(&ac->replies, &cb) != VALKEY_OK)
            goto oom;
    } else {
        if (c->flags & VALKEY_SUBSCRIBED) {
            if (valkeyPushCallback(&ac->sub.replies, &cb) != VALKEY_OK)
                goto oom;
        } else {
            if (valkeyPushCallback(&ac->replies, &cb) != VALKEY_OK)
                goto oom;
        }
    }

    valkeyAppendCmdLen(c, cmd, len);

    /* Always schedule a write when the write buffer is non-empty */
    _EL_ADD_WRITE(ac);

    return VALKEY_OK;
oom:
    valkeySetError(&(ac->c), VALKEY_ERR_OOM, "Out of memory");
    valkeyAsyncCopyError(ac);
    if (ssubscribe_data) {
        vk_free(ssubscribe_data->command);
        vk_free(ssubscribe_data);
    }
    return VALKEY_ERR;
}

int valkeyvAsyncCommand(valkeyAsyncContext *ac, valkeyCallbackFn *fn, void *privdata, const char *format, va_list ap) {
    char *cmd;
    int len;
    int status;
    len = valkeyvFormatCommand(&cmd, format, ap);

    /* We don't want to pass -1 or -2 to future functions as a length. */
    if (len < 0)
        return VALKEY_ERR;

    status = valkeyAsyncAppendCmdLen(ac, fn, privdata, cmd, len);
    vk_free(cmd);
    return status;
}

int valkeyAsyncCommand(valkeyAsyncContext *ac, valkeyCallbackFn *fn, void *privdata, const char *format, ...) {
    va_list ap;
    int status;
    va_start(ap, format);
    status = valkeyvAsyncCommand(ac, fn, privdata, format, ap);
    va_end(ap);
    return status;
}

int valkeyAsyncCommandArgv(valkeyAsyncContext *ac, valkeyCallbackFn *fn, void *privdata, int argc, const char **argv, const size_t *argvlen) {
    sds cmd;
    long long len;
    int status;
    len = valkeyFormatSdsCommandArgv(&cmd, argc, argv, argvlen);
    if (len < 0)
        return VALKEY_ERR;
    status = valkeyAsyncAppendCmdLen(ac, fn, privdata, cmd, len);
    sdsfree(cmd);
    return status;
}

int valkeyAsyncFormattedCommand(valkeyAsyncContext *ac, valkeyCallbackFn *fn, void *privdata, const char *cmd, size_t len) {
    int status = valkeyAsyncAppendCmdLen(ac, fn, privdata, cmd, len);
    return status;
}

valkeyAsyncPushFn *valkeyAsyncSetPushCallback(valkeyAsyncContext *ac, valkeyAsyncPushFn *fn) {
    valkeyAsyncPushFn *old = ac->push_cb;
    ac->push_cb = fn;
    return old;
}

int valkeyAsyncSetTimeout(valkeyAsyncContext *ac, struct timeval tv) {
    if (!ac->c.command_timeout) {
        ac->c.command_timeout = vk_calloc(1, sizeof(tv));
        if (ac->c.command_timeout == NULL) {
            valkeySetError(&ac->c, VALKEY_ERR_OOM, "Out of memory");
            valkeyAsyncCopyError(ac);
            return VALKEY_ERR;
        }
    }

    if (tv.tv_sec != ac->c.command_timeout->tv_sec ||
        tv.tv_usec != ac->c.command_timeout->tv_usec) {
        *ac->c.command_timeout = tv;
    }

    return VALKEY_OK;
}
