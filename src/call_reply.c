/*
 * Copyright (c) 2009-2021, Redis Ltd.
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
#include "call_reply.h"

#define REPLY_FLAG_ROOT (1 << 0)
#define REPLY_FLAG_PARSED (1 << 1)
#define REPLY_FLAG_RESP3 (1 << 2)
#define REPLY_FLAG_EXACT_TYPE (1 << 3)

typedef ValkeyModuleReplyHandlers RespHandlers;

static void callRawReplyNull(void *ctx, const char *proto, size_t proto_len) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->null) return;
    h->null(h->context, proto, proto_len);
}

static void callRawReplyBulkString(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->bulkString) return;
    h->bulkString(h->context, str, len, proto, proto_len);
}

static void callRawReplyNullBulkString(void *ctx, const char *proto, size_t proto_len) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->nullBulkString) return;
    h->nullBulkString(h->context, proto, proto_len);
}

static void callRawReplyNullArray(void *ctx, const char *proto, size_t proto_len) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->nullArray) return;
    h->nullArray(h->context, proto, proto_len);
}

static void callRawReplyError(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->error) return;
    h->error(h->context, str, len, proto, proto_len);
}

static void callRawReplySimpleStr(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->simpleString) return;
    h->simpleString(h->context, str, len, proto, proto_len);
}

static void callRawReplyLong(void *ctx, long long val, const char *proto, size_t proto_len) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->integer) return;
    h->integer(h->context, val, proto, proto_len);
}

static void callRawReplyArray(ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->arrayStart) return;
    h->arrayStart(h->context, len);
    for (size_t i = 0; i < len; i++) {
        parseReply(parser, ctx);
    }
    if (!h->arrayEnd) return;
    h->arrayEnd(h->context, proto, parser->curr_location - proto);
}

static void callRawReplySet(ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->setStart) return;
    h->setStart(h->context, len);
    for (size_t i = 0; i < len; i++) {
        parseReply(parser, ctx);
    }
    if (!h->setEnd) return;
    h->setEnd(h->context, proto, parser->curr_location - proto);
}

static void callRawReplyMap(ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->mapStart) return;
    h->mapStart(h->context, len);
    for (size_t i = 0; i < len; i++) {
        parseReply(parser, ctx);
        parseReply(parser, ctx);
    }
    if (!h->mapEnd) return;
    h->mapEnd(h->context, proto, parser->curr_location - proto);
}

static void callRawReplyDouble(void *ctx, double val, const char *proto, size_t proto_len) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->doubleVal) return;
    h->doubleVal(h->context, val, proto, proto_len);
}

static void callRawReplyBool(void *ctx, int val, const char *proto, size_t proto_len) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->boolVal) return;
    h->boolVal(h->context, val, proto, proto_len);
}

static void callRawReplyBigNumber(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->bigNumber) return;
    h->bigNumber(h->context, str, len, proto, proto_len);
}

static void callRawReplyVerbatimString(void *ctx,
                                       const char *format,
                                       const char *str,
                                       size_t len,
                                       const char *proto,
                                       size_t proto_len) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->verbatimString) return;
    h->verbatimString(h->context, str, len, format, proto, proto_len);
}

static void callRawReplyAttribute(ReplyParser *parser, void *ctx, size_t len, const char *proto) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->attributeStart) return;
    h->attributeStart(h->context, len);
    for (size_t i = 0; i < len; i++) {
        parseReply(parser, ctx);
        parseReply(parser, ctx);
    }
    if (!h->attributeEnd) return;
    h->attributeEnd(h->context, proto, parser->curr_location - proto);

    parseReply(parser, ctx);
}

static void callRawReplyParseError(void *ctx) {
    RespHandlers *h = (RespHandlers *)ctx;
    if (!h->replyParsingError) return;
    h->replyParsingError(h->context);
}

static const ReplyParserCallbacks RawReplyParserCallbacks = {
    .null_callback = callRawReplyNull,
    .bulk_string_callback = callRawReplyBulkString,
    .null_bulk_string_callback = callRawReplyNullBulkString,
    .null_array_callback = callRawReplyNullArray,
    .error_callback = callRawReplyError,
    .simple_str_callback = callRawReplySimpleStr,
    .long_callback = callRawReplyLong,
    .array_callback = callRawReplyArray,
    .set_callback = callRawReplySet,
    .map_callback = callRawReplyMap,
    .double_callback = callRawReplyDouble,
    .bool_callback = callRawReplyBool,
    .big_number_callback = callRawReplyBigNumber,
    .verbatim_string_callback = callRawReplyVerbatimString,
    .attribute_callback = callRawReplyAttribute,
    .error = callRawReplyParseError,
};

/* Parse the RESP reply accumulated in client `c`'s output buffer and deliver
 * it to the handler callbacks in `handlers`.
 *
 * If `handlers->onAvailable` is set it is called first with the raw
 * RESP bytes.  When it returns 0, per-type callbacks are skipped; when it
 * returns 1 (or is NULL), the reply is walked recursively and each value is
 * dispatched to the matching typed callback.
 *
 * The client's output buffer is consumed by this call (bufpos reset, reply
 * list drained). */
void invokeReplyHandlers(ValkeyModuleCtx *ctx, client *c, ValkeyModuleReplyHandlers *handlers) {
    char *buf = NULL;
    size_t buf_len = 0;
    int free_buffer = 0;

    serverAssert(!c->flag.blocked);

    if (listLength(c->reply) == 0 && (size_t)c->bufpos < c->buf_usable_size) {
        /* This is a fast path for the common case of a reply inside the
         * client static buffer. Don't create an SDS string but just use
         * the client buffer directly. */
        c->buf[c->bufpos] = '\0';
        buf = c->buf;
        buf_len = c->bufpos;
        c->bufpos = 0;
    } else {
        listIter iter;
        listRewind(c->reply, &iter);
        listNode *node;
        size_t lensum = c->bufpos;
        while ((node = listNext(&iter))) {
            clientReplyBlock *o = listNodeValue(node);
            lensum += o->used;
        }
        buf = zmalloc_usable(lensum + 1, NULL);
        char *ptr = buf;
        memcpy(ptr, c->buf, c->bufpos);
        ptr += c->bufpos;
        c->bufpos = 0;
        while (listLength(c->reply)) {
            clientReplyBlock *o = listNodeValue(listFirst(c->reply));
            memcpy(ptr, o->buf, o->used);
            ptr += o->used;
            listDelNode(c->reply, listFirst(c->reply));
        }
        ptr[0] = '\0';
        buf_len = lensum;
        free_buffer = 1;
    }

    int continue_parsing = 1;

    if (handlers->onAvailable) {
        continue_parsing = handlers->onAvailable(handlers->context, ctx, buf, buf_len);
    }

    if (continue_parsing) {
        ReplyParser parser = {.curr_location = buf, .callbacks = RawReplyParserCallbacks};
        parseReply(&parser, handlers);
    }

    if (free_buffer) {
        zfree(buf);
    }
}

/* --------------------------------------------------------
 * An opaque struct used to parse a RESP protocol reply and
 * represent it. Used when parsing replies such as in RM_Call
 * or Lua scripts.
 * -------------------------------------------------------- */
struct CallReply {
    void *private_data;
    sds original_proto; /* Available only for root reply. */
    const char *proto;
    size_t proto_len;
    int type;   /* REPLY_... */
    int flags;  /* REPLY_FLAG... */
    size_t len; /* Length of a string, or the number elements in an array. */
    union {
        const char *str; /* String pointer for string and error replies. This
                          * does not need to be freed, always points inside
                          * a reply->proto buffer of the reply object or, in
                          * case of array elements, of parent reply objects. */
        struct {
            const char *str;
            const char *format;
        } verbatim_str;          /* Reply value for verbatim string */
        long long ll;            /* Reply value for integer reply. */
        double d;                /* Reply value for double reply. */
        struct CallReply *array; /* Array of sub-reply elements. used for set, array, map, and attribute */
    } val;
    list *deferred_error_list;   /* list of errors in sds form or NULL */
    struct CallReply *attribute; /* attribute reply, NULL if not exists */
};

typedef struct CallReplyFrame {
    CallReply *rep;
    int idx;
    struct CallReplyFrame *prev;
} CallReplyFrame;

typedef struct CallReplyBuilderCtx {
    CallReplyFrame *current;
} CallReplyBuilderCtx;

#define SCOPE_OBJ_CACHE_SIZE 8

CallReplyFrame scope_obj_cache[SCOPE_OBJ_CACHE_SIZE] = {0};
size_t scope_obj_cache_idx = 0;
size_t scope_dynamic_alloc_count = 0;

static inline CallReply *getCallReply(void *ctx) {
    return ((CallReplyBuilderCtx *)ctx)->current->rep + ((CallReplyBuilderCtx *)ctx)->current->idx;
}

static inline CallReply *nextCallReply(void *ctx) {
    return ((CallReplyBuilderCtx *)ctx)->current->rep + ++((CallReplyBuilderCtx *)ctx)->current->idx;
}

static inline void pushCallReplyFrame(CallReplyBuilderCtx *ctx, CallReply *array_rep) {
    CallReplyFrame *new_scope = NULL;
    if (scope_obj_cache_idx < SCOPE_OBJ_CACHE_SIZE) {
        new_scope = &scope_obj_cache[scope_obj_cache_idx++];
    } else {
        scope_dynamic_alloc_count++;
        new_scope = zmalloc(sizeof(CallReplyFrame));
    }
    new_scope->rep = array_rep;
    new_scope->idx = -1;
    new_scope->prev = ctx->current;
    ctx->current = new_scope;
}

static inline void popCallReplyFrame(CallReplyBuilderCtx *ctx) {
    CallReplyFrame *prev_scope = ctx->current->prev;
    if (scope_dynamic_alloc_count > 0) {
        zfree(ctx->current);
        scope_dynamic_alloc_count--;
    } else {
        /* Just reset the current scope object and return it to the cache. */
        ctx->current->rep = NULL;
        ctx->current->idx = -1;
        ctx->current->prev = NULL;
        scope_obj_cache_idx--;
    }
    ctx->current = prev_scope;
}

static void callReplySetSharedData(CallReply *rep, int type, const char *proto, size_t proto_len, int extra_flags) {
    rep->type = type;
    rep->proto = proto;
    rep->proto_len = proto_len;
    rep->flags |= extra_flags;
}

void callReplyNull(void *ctx, const char *proto, size_t proto_len) {
    CallReply *rep = nextCallReply(ctx);
    callReplySetSharedData(rep, VALKEYMODULE_REPLY_NULL, proto, proto_len, REPLY_FLAG_RESP3);
}

void callReplyNullBulkString(void *ctx, const char *proto, size_t proto_len) {
    CallReply *rep = nextCallReply(ctx);
    callReplySetSharedData(rep, VALKEYMODULE_REPLY_NULL, proto, proto_len, 0);
}

void callReplyNullArray(void *ctx, const char *proto, size_t proto_len) {
    CallReply *rep = nextCallReply(ctx);
    int type = rep->flags & REPLY_FLAG_EXACT_TYPE ? VALKEYMODULE_REPLY_ARRAY_NULL
                                                  : VALKEYMODULE_REPLY_NULL;
    callReplySetSharedData(rep, type, proto, proto_len, 0);
}

void callReplyBulkString(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    CallReply *rep = nextCallReply(ctx);
    callReplySetSharedData(rep, VALKEYMODULE_REPLY_STRING, proto, proto_len, 0);
    rep->len = len;
    rep->val.str = str;
}

void callReplySimpleString(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    CallReply *rep = nextCallReply(ctx);
    int type = rep->flags & REPLY_FLAG_EXACT_TYPE ? VALKEYMODULE_REPLY_SIMPLE_STRING
                                                  : VALKEYMODULE_REPLY_STRING;
    callReplySetSharedData(rep, type, proto, proto_len, 0);
    rep->len = len;
    rep->val.str = str;
}

void callReplyVerbatimString(void *ctx, const char *str, size_t len, const char *fmt, const char *proto, size_t proto_len) {
    CallReply *rep = nextCallReply(ctx);
    callReplySetSharedData(rep, VALKEYMODULE_REPLY_VERBATIM_STRING, proto, proto_len, REPLY_FLAG_RESP3);
    rep->len = len;
    rep->val.verbatim_str.str = str;
    rep->val.verbatim_str.format = fmt;
}

void callReplyError(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    CallReply *rep = nextCallReply(ctx);
    callReplySetSharedData(rep, VALKEYMODULE_REPLY_ERROR, proto, proto_len, 0);
    rep->len = len;
    rep->val.str = str;
}

void callReplyLong(void *ctx, long long val, const char *proto, size_t proto_len) {
    CallReply *rep = nextCallReply(ctx);
    callReplySetSharedData(rep, VALKEYMODULE_REPLY_INTEGER, proto, proto_len, 0);
    rep->val.ll = val;
}

void callReplyDouble(void *ctx, double val, const char *proto, size_t proto_len) {
    CallReply *rep = nextCallReply(ctx);
    callReplySetSharedData(rep, VALKEYMODULE_REPLY_DOUBLE, proto, proto_len, REPLY_FLAG_RESP3);
    rep->val.d = val;
}

void callReplyBigNumber(void *ctx, const char *str, size_t len, const char *proto, size_t proto_len) {
    CallReply *rep = nextCallReply(ctx);
    callReplySetSharedData(rep, VALKEYMODULE_REPLY_BIG_NUMBER, proto, proto_len, REPLY_FLAG_RESP3);
    rep->len = len;
    rep->val.str = str;
}

void callReplyBool(void *ctx, int val, const char *proto, size_t proto_len) {
    CallReply *rep = nextCallReply(ctx);
    callReplySetSharedData(rep, VALKEYMODULE_REPLY_BOOL, proto, proto_len, REPLY_FLAG_RESP3);
    rep->val.ll = val;
}

void callReplyParseCollectionStart(void *ctx, size_t len, int type) {
    CallReply *rep = nextCallReply(ctx);

    if (type == VALKEYMODULE_REPLY_ATTRIBUTE) {
        rep->attribute = zcalloc(sizeof(CallReply));
        rep = rep->attribute;
    }

    int is_map = type == VALKEYMODULE_REPLY_MAP || type == VALKEYMODULE_REPLY_ATTRIBUTE;

    rep->type = type;
    rep->len = len;
    size_t num_elements = is_map ? len * 2 : len;
    rep->val.array = zcalloc(sizeof(CallReply) * num_elements);

    for (size_t i = 0; i < num_elements; i++) {
        rep->val.array[i].private_data = rep->private_data;
    }

    if (type != VALKEYMODULE_REPLY_ARRAY) {
        rep->flags |= REPLY_FLAG_RESP3;
    }

    pushCallReplyFrame(ctx, rep->val.array);
}

void callReplyParseCollectionEnd(void *ctx, const char *proto, size_t proto_len, int type) {
    int is_map = type == VALKEYMODULE_REPLY_MAP || type == VALKEYMODULE_REPLY_ATTRIBUTE;
    CallReplyBuilderCtx *p_ctx = ctx;
    /* idx is the index of the last added element, so we need to add 1 to get the number of added elements. */
    size_t num_added_elements = p_ctx->current->idx + 1;

    popCallReplyFrame(ctx);
    CallReply *rep = getCallReply(ctx);

    if (type == VALKEYMODULE_REPLY_ATTRIBUTE) {
        rep = rep->attribute;
    }

    serverAssert(rep->type == type);
    serverAssert(rep->len == (is_map ? num_added_elements / 2 : num_added_elements));
    rep->flags |= REPLY_FLAG_PARSED;
    rep->proto = proto;
    rep->proto_len = proto_len;

    size_t num_elements = is_map ? rep->len * 2 : rep->len;
    for (size_t i = 0; i < num_elements; i++) {
        rep->val.array[i].flags |= REPLY_FLAG_PARSED;
        if (rep->val.array[i].flags & REPLY_FLAG_RESP3) {
            rep->flags |= REPLY_FLAG_RESP3;
        }
    }
}

void callReplyArrayStart(void *ctx, size_t len) {
    callReplyParseCollectionStart(ctx, len, VALKEYMODULE_REPLY_ARRAY);
}

void callReplyArrayEnd(void *ctx, const char *proto, size_t proto_len) {
    callReplyParseCollectionEnd(ctx, proto, proto_len, VALKEYMODULE_REPLY_ARRAY);
}

void callReplyMapStart(void *ctx, size_t len) {
    callReplyParseCollectionStart(ctx, len, VALKEYMODULE_REPLY_MAP);
}

void callReplyMapEnd(void *ctx, const char *proto, size_t proto_len) {
    callReplyParseCollectionEnd(ctx, proto, proto_len, VALKEYMODULE_REPLY_MAP);
}

void callReplySetStart(void *ctx, size_t len) {
    callReplyParseCollectionStart(ctx, len, VALKEYMODULE_REPLY_SET);
}

void callReplySetEnd(void *ctx, const char *proto, size_t proto_len) {
    callReplyParseCollectionEnd(ctx, proto, proto_len, VALKEYMODULE_REPLY_SET);
}

void callReplyAttributeStart(void *ctx, size_t len) {
    callReplyParseCollectionStart(ctx, len, VALKEYMODULE_REPLY_ATTRIBUTE);
}

void callReplyAttributeEnd(void *ctx, const char *proto, size_t proto_len) {
    callReplyParseCollectionEnd(ctx, proto, proto_len, VALKEYMODULE_REPLY_ATTRIBUTE);
    CallReplyBuilderCtx *p_ctx = ctx;
    /* there should be at least one element in the attribute collection */
    serverAssert(p_ctx->current->idx > -1);

    /* attribute is not part of the root collection, so we need to decrease the idx
     * to not count it as an element in the root collection */
    p_ctx->current->idx--;
}

void callReplyParseError(void *ctx) {
    CallReply *rep = nextCallReply(ctx);
    rep->type = VALKEYMODULE_REPLY_UNKNOWN;
}

RespHandlers callReplyParsingCtx = {
    .null = callReplyNull,
    .nullBulkString = callReplyNullBulkString,
    .nullArray = callReplyNullArray,
    .bulkString = callReplyBulkString,
    .simpleString = callReplySimpleString,
    .verbatimString = callReplyVerbatimString,
    .error = callReplyError,
    .integer = callReplyLong,
    .doubleVal = callReplyDouble,
    .bigNumber = callReplyBigNumber,
    .boolVal = callReplyBool,
    .attributeStart = callReplyAttributeStart,
    .attributeEnd = callReplyAttributeEnd,
    .arrayStart = callReplyArrayStart,
    .arrayEnd = callReplyArrayEnd,
    .mapStart = callReplyMapStart,
    .mapEnd = callReplyMapEnd,
    .setStart = callReplySetStart,
    .setEnd = callReplySetEnd,
    .replyParsingError = callReplyParseError,
    .context = NULL,
};

/* Recursively free the current call reply and its sub-replies. */
static void freeCallReplyInternal(CallReply *rep) {
    if (rep->type == VALKEYMODULE_REPLY_ARRAY || rep->type == VALKEYMODULE_REPLY_SET) {
        for (size_t i = 0; i < rep->len; ++i) {
            freeCallReplyInternal(rep->val.array + i);
        }
        zfree(rep->val.array);
    }

    if (rep->type == VALKEYMODULE_REPLY_MAP || rep->type == VALKEYMODULE_REPLY_ATTRIBUTE) {
        for (size_t i = 0; i < rep->len; ++i) {
            freeCallReplyInternal(rep->val.array + i * 2);
            freeCallReplyInternal(rep->val.array + i * 2 + 1);
        }
        zfree(rep->val.array);
    }

    if (rep->attribute) {
        freeCallReplyInternal(rep->attribute);
        zfree(rep->attribute);
    }
}

/* Free the given call reply and its children (in case of nested reply) recursively.
 * If private data was set when the CallReply was created it will not be freed, as it's
 * the caller's responsibility to free it before calling freeCallReply(). */
void freeCallReply(CallReply *rep) {
    if (!(rep->flags & REPLY_FLAG_ROOT)) {
        return;
    }
    if (rep->flags & REPLY_FLAG_PARSED) {
        if (rep->type == VALKEYMODULE_REPLY_PROMISE) {
            zfree(rep);
            return;
        }
        freeCallReplyInternal(rep);
    }
    sdsfree(rep->original_proto);
    if (rep->deferred_error_list) listRelease(rep->deferred_error_list);
    zfree(rep);
}

CallReply *callReplyCreatePromise(void *private_data) {
    CallReply *res = zmalloc(sizeof(*res));
    res->type = VALKEYMODULE_REPLY_PROMISE;
    /* Mark the reply as parsed so there will be not attempt to parse
     * it when calling reply API such as freeCallReply.
     * Also mark the reply as root so freeCallReply will not ignore it. */
    res->flags |= REPLY_FLAG_PARSED | REPLY_FLAG_ROOT;
    res->private_data = private_data;
    return res;
}

/* Parse the buffer located in rep->original_proto and update the CallReply
 * structure to represent its contents. */
static void callReplyParse(CallReply *rep) {
    if (rep->flags & REPLY_FLAG_PARSED) {
        return;
    }

    CallReplyBuilderCtx builder_ctx = {.current = NULL};
    pushCallReplyFrame(&builder_ctx, rep);

    callReplyParsingCtx.context = &builder_ctx;

    ReplyParser parser = {.curr_location = rep->proto, .callbacks = RawReplyParserCallbacks};
    parseReply(&parser, &callReplyParsingCtx);
    rep->flags |= REPLY_FLAG_PARSED;

    popCallReplyFrame(&builder_ctx);
}

/* Return the call reply type (VALKEYMODULE_REPLY_...). */
int callReplyType(CallReply *rep) {
    if (!rep) return VALKEYMODULE_REPLY_UNKNOWN;
    callReplyParse(rep);
    return rep->type;
}

/* Return reply string as buffer and len. Applicable to:
 * - VALKEYMODULE_REPLY_STRING
 * - VALKEYMODULE_REPLY_SIMPLE_STRING
 * - VALKEYMODULE_REPLY_ERROR
 *
 * The return value is borrowed from CallReply, so it must not be freed
 * explicitly or used after CallReply itself is freed.
 *
 * The returned value is not NULL terminated and its length is returned by
 * reference through len, which must not be NULL.
 */
const char *callReplyGetString(CallReply *rep, size_t *len) {
    callReplyParse(rep);
    if (rep->type != VALKEYMODULE_REPLY_STRING &&
        rep->type != VALKEYMODULE_REPLY_SIMPLE_STRING &&
        rep->type != VALKEYMODULE_REPLY_ERROR) return NULL;
    if (len) *len = rep->len;
    return rep->val.str;
}

/* Return a long long reply value. Applicable to:
 * - VALKEYMODULE_REPLY_INTEGER
 */
long long callReplyGetLongLong(CallReply *rep) {
    callReplyParse(rep);
    if (rep->type != VALKEYMODULE_REPLY_INTEGER) return LLONG_MIN;
    return rep->val.ll;
}

/* Return a double reply value. Applicable to:
 * - VALKEYMODULE_REPLY_DOUBLE
 */
double callReplyGetDouble(CallReply *rep) {
    callReplyParse(rep);
    if (rep->type != VALKEYMODULE_REPLY_DOUBLE) return LLONG_MIN;
    return rep->val.d;
}

/* Return a reply Boolean value. Applicable to:
 * - VALKEYMODULE_REPLY_BOOL
 */
int callReplyGetBool(CallReply *rep) {
    callReplyParse(rep);
    if (rep->type != VALKEYMODULE_REPLY_BOOL) return INT_MIN;
    return rep->val.ll;
}

/* Return reply length. Applicable to:
 * - VALKEYMODULE_REPLY_STRING
 * - VALKEYMODULE_REPLY_ERROR
 * - VALKEYMODULE_REPLY_ARRAY
 * - VALKEYMODULE_REPLY_SET
 * - VALKEYMODULE_REPLY_MAP
 * - VALKEYMODULE_REPLY_ATTRIBUTE
 */
size_t callReplyGetLen(CallReply *rep) {
    callReplyParse(rep);
    switch (rep->type) {
    case VALKEYMODULE_REPLY_STRING:
    case VALKEYMODULE_REPLY_ERROR:
    case VALKEYMODULE_REPLY_ARRAY:
    case VALKEYMODULE_REPLY_SET:
    case VALKEYMODULE_REPLY_MAP:
    case VALKEYMODULE_REPLY_ATTRIBUTE: return rep->len;
    default: return 0;
    }
}

static CallReply *callReplyGetCollectionElement(CallReply *rep, size_t idx, int elements_per_entry) {
    if (idx >= rep->len * elements_per_entry) return NULL; // real len is rep->len * elements_per_entry
    return rep->val.array + idx;
}

/* Return a reply array element at a given index. Applicable to:
 * - VALKEYMODULE_REPLY_ARRAY
 *
 * The return value is borrowed from CallReply, so it must not be freed
 * explicitly or used after CallReply itself is freed.
 */
CallReply *callReplyGetArrayElement(CallReply *rep, size_t idx) {
    callReplyParse(rep);
    if (rep->type != VALKEYMODULE_REPLY_ARRAY) return NULL;
    return callReplyGetCollectionElement(rep, idx, 1);
}

/* Return a reply set element at a given index. Applicable to:
 * - VALKEYMODULE_REPLY_SET
 *
 * The return value is borrowed from CallReply, so it must not be freed
 * explicitly or used after CallReply itself is freed.
 */
CallReply *callReplyGetSetElement(CallReply *rep, size_t idx) {
    callReplyParse(rep);
    if (rep->type != VALKEYMODULE_REPLY_SET) return NULL;
    return callReplyGetCollectionElement(rep, idx, 1);
}

static int callReplyGetMapElementInternal(CallReply *rep, size_t idx, CallReply **key, CallReply **val, int type) {
    callReplyParse(rep);
    if (rep->type != type) return C_ERR;
    if (idx >= rep->len) return C_ERR;
    if (key) *key = callReplyGetCollectionElement(rep, idx * 2, 2);
    if (val) *val = callReplyGetCollectionElement(rep, idx * 2 + 1, 2);
    return C_OK;
}

/* Retrieve a map reply key and value at a given index. Applicable to:
 * - VALKEYMODULE_REPLY_MAP
 *
 * The key and value are returned by reference through key and val,
 * which may also be NULL if not needed.
 *
 * Returns C_OK on success or C_ERR if reply type mismatches, or if idx is out
 * of range.
 *
 * The returned values are borrowed from CallReply, so they must not be freed
 * explicitly or used after CallReply itself is freed.
 */
int callReplyGetMapElement(CallReply *rep, size_t idx, CallReply **key, CallReply **val) {
    return callReplyGetMapElementInternal(rep, idx, key, val, VALKEYMODULE_REPLY_MAP);
}

/* Return reply attribute, or NULL if it does not exist. Applicable to all replies.
 *
 * The returned values are borrowed from CallReply, so they must not be freed
 * explicitly or used after CallReply itself is freed.
 */
CallReply *callReplyGetAttribute(CallReply *rep) {
    return rep->attribute;
}

/* Retrieve attribute reply key and value at a given index. Applicable to:
 * - VALKEYMODULE_REPLY_ATTRIBUTE
 *
 * The key and value are returned by reference through key and val,
 * which may also be NULL if not needed.
 *
 * Returns C_OK on success or C_ERR if reply type mismatches, or if idx is out
 * of range.
 *
 * The returned values are borrowed from CallReply, so they must not be freed
 * explicitly or used after CallReply itself is freed.
 */
int callReplyGetAttributeElement(CallReply *rep, size_t idx, CallReply **key, CallReply **val) {
    return callReplyGetMapElementInternal(rep, idx, key, val, VALKEYMODULE_REPLY_MAP);
}

/* Return a big number reply value. Applicable to:
 * - VALKEYMODULE_REPLY_BIG_NUMBER
 *
 * The returned values are borrowed from CallReply, so they must not be freed
 * explicitly or used after CallReply itself is freed.
 *
 * The return value is guaranteed to be a big number, as described in the RESP3
 * protocol specifications.
 *
 * The returned value is not NULL terminated and its length is returned by
 * reference through len, which must not be NULL.
 */
const char *callReplyGetBigNumber(CallReply *rep, size_t *len) {
    callReplyParse(rep);
    if (rep->type != VALKEYMODULE_REPLY_BIG_NUMBER) return NULL;
    *len = rep->len;
    return rep->val.str;
}

/* Return a verbatim string reply value. Applicable to:
 * - VALKEYMODULE_REPLY_VERBATIM_STRING
 *
 * If format is non-NULL, the verbatim reply format is also returned by value.
 *
 * The optional output argument can be given to get a verbatim reply
 * format, or can be set NULL if not needed.
 *
 * The return value is borrowed from CallReply, so it must not be freed
 * explicitly or used after CallReply itself is freed.
 *
 * The returned value is not NULL terminated and its length is returned by
 * reference through len, which must not be NULL.
 */
const char *callReplyGetVerbatim(CallReply *rep, size_t *len, const char **format) {
    callReplyParse(rep);
    if (rep->type != VALKEYMODULE_REPLY_VERBATIM_STRING) return NULL;
    *len = rep->len;
    if (format) *format = rep->val.verbatim_str.format;
    return rep->val.verbatim_str.str;
}

/* Return the current reply blob.
 *
 * The return value is borrowed from CallReply, so it must not be freed
 * explicitly or used after CallReply itself is freed.
 */
const char *callReplyGetProto(CallReply *rep, size_t *proto_len) {
    *proto_len = rep->proto_len;
    return rep->proto;
}

/* Return CallReply private data, as set by the caller on callReplyCreate().
 */
void *callReplyGetPrivateData(CallReply *rep) {
    return rep->private_data;
}

/* Return true if the reply or one of it sub-replies is RESP3 formatted. */
int callReplyIsResp3(CallReply *rep) {
    return rep->flags & REPLY_FLAG_RESP3;
}

/* Returns a list of errors in sds form, or NULL. */
list *callReplyDeferredErrorList(CallReply *rep) {
    return rep->deferred_error_list;
}

/* Create a new CallReply struct from the reply blob.
 *
 * The function will own the reply blob, so it must not be used or freed by
 * the caller after passing it to this function.
 *
 * The reply blob will be freed when the returned CallReply struct is later
 * freed using freeCallReply().
 *
 * The deferred_error_list is an optional list of errors that are present
 * in the reply blob, if given, this function will take ownership on it.
 *
 * The private_data is optional and can later be accessed using
 * callReplyGetPrivateData().
 *
 * NOTE: The parser used for parsing the reply and producing CallReply is
 * designed to handle valid replies created by the server itself. IT IS NOT
 * DESIGNED TO HANDLE USER INPUT and using it to parse invalid replies is
 * unsafe.
 */
CallReply *callReplyCreate(sds reply, list *deferred_error_list, void *private_data) {
    CallReply *res = zmalloc(sizeof(*res));
    res->flags = REPLY_FLAG_ROOT;
    res->original_proto = reply;
    res->proto = reply;
    res->proto_len = sdslen(reply);
    res->private_data = private_data;
    res->attribute = NULL;
    res->deferred_error_list = deferred_error_list;
    return res;
}

/* Create a new CallReply struct from the reply blob representing an error message.
 * Automatically creating deferred_error_list and set a copy of the reply in it.
 * Refer to callReplyCreate for detailed explanation.
 * Reply string can come in one of two forms:
 * 1. A protocol reply starting with "-CODE" and ending with "\r\n"
 * 2. A plain string, in which case this function adds the protocol header and footer. */
CallReply *callReplyCreateError(sds reply, void *private_data) {
    sds err_buff = reply;
    if (err_buff[0] != '-') {
        err_buff = sdscatfmt(sdsempty(), "-ERR %S\r\n", reply);
        sdsfree(reply);
    }
    list *deferred_error_list = listCreate();
    listSetFreeMethod(deferred_error_list, sdsfreeVoid);
    listAddNodeTail(deferred_error_list, sdsnew(err_buff));
    return callReplyCreate(err_buff, deferred_error_list, private_data);
}

/* Enable exact reply type parsing to preserve type distinctions.
 *
 * This flag maintains the distinction between simple strings and bulk strings,
 * as well as preserving RESP2's distinction between null bulk strings and null arrays
 * when parsing the CallReply.
 */
void enableParseExactReplyTypeFlag(CallReply *rep) {
    serverAssert(!(rep->flags & REPLY_FLAG_PARSED));
    rep->flags |= REPLY_FLAG_EXACT_TYPE;
}
