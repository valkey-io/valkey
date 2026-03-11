/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2014, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2015, Matt Stancliff <matt at genges dot com>,
 *                     Jan-Erik Rediger <janerik at fnordig dot com>
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
#include "win32.h"

#include "valkey.h"

#include "net.h"
#include "valkey_private.h"

#include <sds.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static valkeyReply *createReplyObject(int type);
static void *createStringObject(const valkeyReadTask *task, char *str, size_t len);
static void *createArrayObject(const valkeyReadTask *task, size_t elements);
static void *createIntegerObject(const valkeyReadTask *task, long long value);
static void *createDoubleObject(const valkeyReadTask *task, double value, char *str, size_t len);
static void *createNilObject(const valkeyReadTask *task);
static void *createBoolObject(const valkeyReadTask *task, int bval);

/* Default set of functions to build the reply. Keep in mind that such a
 * function returning NULL is interpreted as OOM. */
static valkeyReplyObjectFunctions defaultFunctions = {
    createStringObject,
    createArrayObject,
    createIntegerObject,
    createDoubleObject,
    createNilObject,
    createBoolObject,
    freeReplyObject};

/* Create a reply object */
static valkeyReply *createReplyObject(int type) {
    valkeyReply *r = vk_calloc(1, sizeof(*r));

    if (r == NULL)
        return NULL;

    r->type = type;
    return r;
}

/* Free a reply object */
void freeReplyObject(void *reply) {
    valkeyReply *r = reply;
    size_t j;

    if (r == NULL)
        return;

    switch (r->type) {
    case VALKEY_REPLY_INTEGER:
    case VALKEY_REPLY_NIL:
    case VALKEY_REPLY_BOOL:
        break; /* Nothing to free */
    case VALKEY_REPLY_ARRAY:
    case VALKEY_REPLY_MAP:
    case VALKEY_REPLY_ATTR:
    case VALKEY_REPLY_SET:
    case VALKEY_REPLY_PUSH:
        if (r->element != NULL) {
            for (j = 0; j < r->elements; j++)
                freeReplyObject(r->element[j]);
            vk_free(r->element);
        }
        break;
    case VALKEY_REPLY_ERROR:
    case VALKEY_REPLY_STATUS:
    case VALKEY_REPLY_STRING:
    case VALKEY_REPLY_DOUBLE:
    case VALKEY_REPLY_VERB:
    case VALKEY_REPLY_BIGNUM:
        vk_free(r->str);
        break;
    }
    vk_free(r);
}

static void *createStringObject(const valkeyReadTask *task, char *str, size_t len) {
    valkeyReply *r, *parent;
    char *buf;

    r = createReplyObject(task->type);
    if (r == NULL)
        return NULL;

    assert(task->type == VALKEY_REPLY_ERROR ||
           task->type == VALKEY_REPLY_STATUS ||
           task->type == VALKEY_REPLY_STRING ||
           task->type == VALKEY_REPLY_VERB ||
           task->type == VALKEY_REPLY_BIGNUM);

    /* Copy string value */
    if (task->type == VALKEY_REPLY_VERB) {
        buf = vk_malloc(len - 4 + 1); /* Skip 4 bytes of verbatim type header. */
        if (buf == NULL)
            goto oom;

        memcpy(r->vtype, str, 3);
        r->vtype[3] = '\0';
        memcpy(buf, str + 4, len - 4);
        buf[len - 4] = '\0';
        r->len = len - 4;
    } else {
        buf = vk_malloc(len + 1);
        if (buf == NULL)
            goto oom;

        memcpy(buf, str, len);
        buf[len] = '\0';
        r->len = len;
    }
    r->str = buf;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == VALKEY_REPLY_ARRAY ||
               parent->type == VALKEY_REPLY_MAP ||
               parent->type == VALKEY_REPLY_ATTR ||
               parent->type == VALKEY_REPLY_SET ||
               parent->type == VALKEY_REPLY_PUSH);
        parent->element[task->idx] = r;
    }
    return r;

oom:
    freeReplyObject(r);
    return NULL;
}

static void *createArrayObject(const valkeyReadTask *task, size_t elements) {
    valkeyReply *r, *parent;

    r = createReplyObject(task->type);
    if (r == NULL)
        return NULL;

    if (elements > 0) {
        r->element = vk_calloc(elements, sizeof(valkeyReply *));
        if (r->element == NULL) {
            freeReplyObject(r);
            return NULL;
        }
    }

    r->elements = elements;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == VALKEY_REPLY_ARRAY ||
               parent->type == VALKEY_REPLY_MAP ||
               parent->type == VALKEY_REPLY_ATTR ||
               parent->type == VALKEY_REPLY_SET ||
               parent->type == VALKEY_REPLY_PUSH);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createIntegerObject(const valkeyReadTask *task, long long value) {
    valkeyReply *r, *parent;

    r = createReplyObject(VALKEY_REPLY_INTEGER);
    if (r == NULL)
        return NULL;

    r->integer = value;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == VALKEY_REPLY_ARRAY ||
               parent->type == VALKEY_REPLY_MAP ||
               parent->type == VALKEY_REPLY_ATTR ||
               parent->type == VALKEY_REPLY_SET ||
               parent->type == VALKEY_REPLY_PUSH);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createDoubleObject(const valkeyReadTask *task, double value, char *str, size_t len) {
    valkeyReply *r, *parent;

    if (len == SIZE_MAX) // Prevents vk_malloc(0) if len equals SIZE_MAX
        return NULL;

    r = createReplyObject(VALKEY_REPLY_DOUBLE);
    if (r == NULL)
        return NULL;

    r->dval = value;
    r->str = vk_malloc(len + 1);
    if (r->str == NULL) {
        freeReplyObject(r);
        return NULL;
    }

    /* The double reply also has the original protocol string representing a
     * double as a null terminated string. This way the caller does not need
     * to format back for string conversion, especially since Valkey does efforts
     * to make the string more human readable avoiding the classical double
     * decimal string conversion artifacts. */
    memcpy(r->str, str, len);
    r->str[len] = '\0';
    r->len = len;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == VALKEY_REPLY_ARRAY ||
               parent->type == VALKEY_REPLY_MAP ||
               parent->type == VALKEY_REPLY_ATTR ||
               parent->type == VALKEY_REPLY_SET ||
               parent->type == VALKEY_REPLY_PUSH);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createNilObject(const valkeyReadTask *task) {
    valkeyReply *r, *parent;

    r = createReplyObject(VALKEY_REPLY_NIL);
    if (r == NULL)
        return NULL;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == VALKEY_REPLY_ARRAY ||
               parent->type == VALKEY_REPLY_MAP ||
               parent->type == VALKEY_REPLY_ATTR ||
               parent->type == VALKEY_REPLY_SET ||
               parent->type == VALKEY_REPLY_PUSH);
        parent->element[task->idx] = r;
    }
    return r;
}

static void *createBoolObject(const valkeyReadTask *task, int bval) {
    valkeyReply *r, *parent;

    r = createReplyObject(VALKEY_REPLY_BOOL);
    if (r == NULL)
        return NULL;

    r->integer = bval != 0;

    if (task->parent) {
        parent = task->parent->obj;
        assert(parent->type == VALKEY_REPLY_ARRAY ||
               parent->type == VALKEY_REPLY_MAP ||
               parent->type == VALKEY_REPLY_ATTR ||
               parent->type == VALKEY_REPLY_SET ||
               parent->type == VALKEY_REPLY_PUSH);
        parent->element[task->idx] = r;
    }
    return r;
}

/* Return the number of digits of 'v' when converted to string in radix 10.
 * Implementation borrowed from link in valkey/src/util.c:string2ll(). */
static uint32_t countDigits(uint64_t v) {
    uint32_t result = 1;
    for (;;) {
        if (v < 10)
            return result;
        if (v < 100)
            return result + 1;
        if (v < 1000)
            return result + 2;
        if (v < 10000)
            return result + 3;
        v /= 10000U;
        result += 4;
    }
}

/* Helper that calculates the bulk length given a certain string length. */
static size_t bulklen(size_t len) {
    return 1 + countDigits(len) + 2 + len + 2;
}

int valkeyvFormatCommand(char **target, const char *format, va_list ap) {
    const char *c = format;
    char *cmd = NULL;   /* final command */
    int pos;            /* position in final command */
    sds curarg, newarg; /* current argument */
    int touched = 0;    /* was the current argument touched? */
    char **curargv = NULL, **newargv = NULL;
    int argc = 0;
    int totlen = 0;
    int error_type = 0; /* 0 = no error; -1 = memory error; -2 = format error */
    int j;

    /* Abort if there is not target to set */
    if (target == NULL)
        return -1;

    /* Build the command string accordingly to protocol */
    curarg = sdsempty();
    if (curarg == NULL)
        return -1;

    while (*c != '\0') {
        if (*c != '%' || c[1] == '\0') {
            if (*c == ' ') {
                if (touched) {
                    newargv = vk_realloc(curargv, sizeof(char *) * (argc + 1));
                    if (newargv == NULL)
                        goto memory_err;
                    curargv = newargv;
                    curargv[argc++] = curarg;
                    totlen += bulklen(sdslen(curarg));

                    /* curarg is put in argv so it can be overwritten. */
                    curarg = sdsempty();
                    if (curarg == NULL)
                        goto memory_err;
                    touched = 0;
                }
            } else {
                newarg = sdscatlen(curarg, c, 1);
                if (newarg == NULL)
                    goto memory_err;
                curarg = newarg;
                touched = 1;
            }
        } else {
            char *arg;
            size_t size;

            /* Set newarg so it can be checked even if it is not touched. */
            newarg = curarg;

            switch (c[1]) {
            case 's':
                arg = va_arg(ap, char *);
                if (arg == NULL)
                    goto format_err;
                size = strlen(arg);
                if (size > 0)
                    newarg = sdscatlen(curarg, arg, size);
                break;
            case 'b':
                arg = va_arg(ap, char *);
                if (arg == NULL)
                    goto format_err;
                size = va_arg(ap, size_t);
                if (size > 0)
                    newarg = sdscatlen(curarg, arg, size);
                break;
            case '%':
                newarg = sdscat(curarg, "%");
                break;
            default:
                /* Try to detect printf format */
                {
                    static const char intfmts[] = "diouxX";
                    static const char flags[] = "#0-+ ";
                    char _format[16];
                    const char *_p = c + 1;
                    size_t _l = 0;
                    va_list _cpy;

                    /* Flags */
                    while (*_p != '\0' && strchr(flags, *_p) != NULL)
                        _p++;

                    /* Field width */
                    while (*_p != '\0' && isdigit((int)*_p))
                        _p++;

                    /* Precision */
                    if (*_p == '.') {
                        _p++;
                        while (*_p != '\0' && isdigit((int)*_p))
                            _p++;
                    }

                    /* Copy va_list before consuming with va_arg */
                    va_copy(_cpy, ap);

                    /* Make sure we have more characters otherwise strchr() accepts
                     * '\0' as an integer specifier. This is checked after above
                     * va_copy() to avoid UB in fmt_invalid's call to va_end(). */
                    if (*_p == '\0')
                        goto fmt_invalid;

                    /* Integer conversion (without modifiers) */
                    if (strchr(intfmts, *_p) != NULL) {
                        va_arg(ap, int);
                        goto fmt_valid;
                    }

                    /* Double conversion (without modifiers) */
                    if (strchr("eEfFgGaA", *_p) != NULL) {
                        va_arg(ap, double);
                        goto fmt_valid;
                    }

                    /* Size: char */
                    if (_p[0] == 'h' && _p[1] == 'h') {
                        _p += 2;
                        if (*_p != '\0' && strchr(intfmts, *_p) != NULL) {
                            va_arg(ap, int); /* char gets promoted to int */
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                    /* Size: short */
                    if (_p[0] == 'h') {
                        _p += 1;
                        if (*_p != '\0' && strchr(intfmts, *_p) != NULL) {
                            va_arg(ap, int); /* short gets promoted to int */
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                    /* Size: long long */
                    if (_p[0] == 'l' && _p[1] == 'l') {
                        _p += 2;
                        if (*_p != '\0' && strchr(intfmts, *_p) != NULL) {
                            va_arg(ap, long long);
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                    /* Size: long */
                    if (_p[0] == 'l') {
                        _p += 1;
                        if (*_p != '\0' && strchr(intfmts, *_p) != NULL) {
                            va_arg(ap, long);
                            goto fmt_valid;
                        }
                        goto fmt_invalid;
                    }

                fmt_invalid:
                    va_end(_cpy);
                    goto format_err;

                fmt_valid:
                    _l = (_p + 1) - c;
                    if (_l < sizeof(_format) - 2) {
                        memcpy(_format, c, _l);
                        _format[_l] = '\0';
                        newarg = sdscatvprintf(curarg, _format, _cpy);

                        /* Update current position (note: outer blocks
                         * increment c twice so compensate here) */
                        c = _p - 1;
                    }

                    va_end(_cpy);
                    break;
                }
            }

            if (newarg == NULL)
                goto memory_err;
            curarg = newarg;

            touched = 1;
            c++;
            if (*c == '\0')
                break;
        }
        c++;
    }

    /* Add the last argument if needed */
    if (touched) {
        newargv = vk_realloc(curargv, sizeof(char *) * (argc + 1));
        if (newargv == NULL)
            goto memory_err;
        curargv = newargv;
        curargv[argc++] = curarg;
        totlen += bulklen(sdslen(curarg));
    } else {
        sdsfree(curarg);
    }

    /* Clear curarg because it was put in curargv or was free'd. */
    curarg = NULL;

    /* Add bytes needed to hold multi bulk count */
    totlen += 1 + countDigits(argc) + 2;

    /* Build the command at protocol level */
    cmd = vk_malloc(totlen + 1);
    if (cmd == NULL)
        goto memory_err;

    pos = sprintf(cmd, "*%d\r\n", argc);
    for (j = 0; j < argc; j++) {
        pos += sprintf(cmd + pos, "$%zu\r\n", sdslen(curargv[j]));
        memcpy(cmd + pos, curargv[j], sdslen(curargv[j]));
        pos += sdslen(curargv[j]);
        sdsfree(curargv[j]);
        cmd[pos++] = '\r';
        cmd[pos++] = '\n';
    }
    assert(pos == totlen);
    cmd[pos] = '\0';

    vk_free(curargv);
    *target = cmd;
    return totlen;

format_err:
    error_type = -2;
    goto cleanup;

memory_err:
    error_type = -1;
    goto cleanup;

cleanup:
    if (curargv) {
        while (argc--)
            sdsfree(curargv[argc]);
        vk_free(curargv);
    }

    sdsfree(curarg);
    vk_free(cmd);

    return error_type;
}

/* Format a command according to the RESP protocol. This function
 * takes a format similar to printf:
 *
 * %s represents a C null terminated string you want to interpolate
 * %b represents a binary safe string
 *
 * When using %b you need to provide both the pointer to the string
 * and the length in bytes as a size_t. Examples:
 *
 * len = valkeyFormatCommand(target, "GET %s", mykey);
 * len = valkeyFormatCommand(target, "SET %s %b", mykey, myval, myvallen);
 */
int valkeyFormatCommand(char **target, const char *format, ...) {
    va_list ap;
    int len;
    va_start(ap, format);
    len = valkeyvFormatCommand(target, format, ap);
    va_end(ap);

    /* The API says "-1" means bad result, but we now also return "-2" in some
     * cases.  Force the return value to always be -1. */
    if (len < 0)
        len = -1;

    return len;
}

/* Format a command according to the RESP protocol using an sds string and
 * sdscatfmt for the processing of arguments. This function takes the
 * number of arguments, an array with arguments and an array with their
 * lengths. If the latter is set to NULL, strlen will be used to compute the
 * argument lengths.
 */
long long valkeyFormatSdsCommandArgv(sds *target, int argc, const char **argv,
                                     const size_t *argvlen) {
    sds cmd, aux;
    unsigned long long totlen, len;
    int j;

    /* Abort on a NULL target */
    if (target == NULL)
        return -1;

    /* Calculate our total size */
    totlen = 1 + countDigits(argc) + 2;
    for (j = 0; j < argc; j++) {
        len = argvlen ? argvlen[j] : strlen(argv[j]);
        totlen += bulklen(len);
    }

    /* Use an SDS string for command construction */
    cmd = sdsempty();
    if (cmd == NULL)
        return -1;

    /* We already know how much storage we need */
    aux = sdsMakeRoomFor(cmd, totlen);
    if (aux == NULL) {
        sdsfree(cmd);
        return -1;
    }

    cmd = aux;

    /* Construct command */
    cmd = sdscatfmt(cmd, "*%i\r\n", argc);
    for (j = 0; j < argc; j++) {
        len = argvlen ? argvlen[j] : strlen(argv[j]);
        cmd = sdscatfmt(cmd, "$%U\r\n", len);
        cmd = sdscatlen(cmd, argv[j], len);
        cmd = sdscatlen(cmd, "\r\n", sizeof("\r\n") - 1);
    }

    assert(sdslen(cmd) == totlen);

    *target = cmd;
    return totlen;
}

/* Format a command according to the RESP protocol. This function takes the
 * number of arguments, an array with arguments and an array with their
 * lengths. If the latter is set to NULL, strlen will be used to compute the
 * argument lengths.
 */
long long valkeyFormatCommandArgv(char **target, int argc, const char **argv, const size_t *argvlen) {
    char *cmd = NULL; /* final command */
    size_t pos;       /* position in final command */
    size_t len, totlen;
    int j;

    /* Abort on a NULL target */
    if (target == NULL)
        return -1;

    /* Calculate number of bytes needed for the command */
    totlen = 1 + countDigits(argc) + 2;
    for (j = 0; j < argc; j++) {
        len = argvlen ? argvlen[j] : strlen(argv[j]);
        totlen += bulklen(len);
    }

    /* Build the command at protocol level */
    cmd = vk_malloc(totlen + 1);
    if (cmd == NULL)
        return -1;

    pos = sprintf(cmd, "*%d\r\n", argc);
    for (j = 0; j < argc; j++) {
        len = argvlen ? argvlen[j] : strlen(argv[j]);
        pos += sprintf(cmd + pos, "$%zu\r\n", len);
        memcpy(cmd + pos, argv[j], len);
        pos += len;
        cmd[pos++] = '\r';
        cmd[pos++] = '\n';
    }
    assert(pos == totlen);
    cmd[pos] = '\0';

    *target = cmd;
    return totlen;
}

void valkeyFreeCommand(char *cmd) {
    vk_free(cmd);
}

void valkeySetError(valkeyContext *c, int type, const char *str) {
    size_t len;

    c->err = type;
    if (str != NULL) {
        len = strlen(str);
        len = len < (sizeof(c->errstr) - 1) ? len : (sizeof(c->errstr) - 1);
        memcpy(c->errstr, str, len);
        c->errstr[len] = '\0';
    } else {
        /* Only VALKEY_ERR_IO may lack a description! */
        assert(type == VALKEY_ERR_IO);
        strerror_r(errno, c->errstr, sizeof(c->errstr));
    }
}

valkeyReader *valkeyReaderCreate(void) {
    return valkeyReaderCreateWithFunctions(&defaultFunctions);
}

static void valkeyPushAutoFree(void *privdata, void *reply) {
    (void)privdata;
    freeReplyObject(reply);
}

static valkeyContext *valkeyContextInit(void) {
    valkeyContext *c;

    c = vk_calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;

    c->obuf = sdsempty();
    c->reader = valkeyReaderCreate();
    c->fd = VALKEY_INVALID_FD;

    if (c->obuf == NULL || c->reader == NULL) {
        valkeyFree(c);
        return NULL;
    }

    return c;
}

void valkeyFree(valkeyContext *c) {
    if (c == NULL)
        return;

    if (c->funcs && c->funcs->close) {
        c->funcs->close(c);
    }

    sdsfree(c->obuf);
    valkeyReaderFree(c->reader);
    vk_free(c->tcp.host);
    vk_free(c->tcp.source_addr);
    vk_free(c->unix_sock.path);
    vk_free(c->connect_timeout);
    vk_free(c->command_timeout);
    vk_free(c->saddr);

    if (c->privdata && c->free_privdata)
        c->free_privdata(c->privdata);

    if (c->funcs && c->funcs->free_privctx)
        c->funcs->free_privctx(c->privctx);

    memset(c, 0xff, sizeof(*c));
    vk_free(c);
}

valkeyFD valkeyFreeKeepFd(valkeyContext *c) {
    valkeyFD fd = c->fd;
    c->fd = VALKEY_INVALID_FD;
    valkeyFree(c);
    return fd;
}

int valkeyReconnect(valkeyContext *c) {
    valkeyOptions options = {.connect_timeout = c->connect_timeout};

    c->err = 0;
    memset(c->errstr, '\0', strlen(c->errstr));

    assert(c->funcs);
    if (c->funcs && c->funcs->close)
        c->funcs->close(c);

    if (c->privctx && c->funcs && c->funcs->free_privctx) {
        c->funcs->free_privctx(c->privctx);
        c->privctx = NULL;
    }

    sdsfree(c->obuf);
    valkeyReaderFree(c->reader);

    c->obuf = sdsempty();
    c->reader = valkeyReaderCreate();

    if (c->obuf == NULL || c->reader == NULL) {
        valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    switch (c->connection_type) {
    case VALKEY_CONN_TCP:
        /* FALLTHRU */
    case VALKEY_CONN_RDMA:
        options.endpoint.tcp.source_addr = c->tcp.source_addr;
        options.endpoint.tcp.ip = c->tcp.host;
        options.endpoint.tcp.port = c->tcp.port;
        break;
    case VALKEY_CONN_UNIX:
        options.endpoint.unix_socket = c->unix_sock.path;
        break;
    default:
        /* Something bad happened here and shouldn't have. There isn't
           enough information in the context to reconnect. */
        valkeySetError(c, VALKEY_ERR_OTHER, "Not enough information to reconnect");
        return VALKEY_ERR;
    }

    if (c->funcs && c->funcs->connect &&
        c->funcs->connect(c, &options) != VALKEY_OK) {
        return VALKEY_ERR;
    }

    if (c->command_timeout != NULL && (c->flags & VALKEY_BLOCK) &&
        c->fd != VALKEY_INVALID_FD && c->funcs && c->funcs->set_timeout) {
        c->funcs->set_timeout(c, *c->command_timeout);
    }

    return VALKEY_OK;
}

valkeyContext *valkeyConnectWithOptions(const valkeyOptions *options) {
    valkeyContext *c;

    if (options->type >= VALKEY_CONN_MAX) {
        return NULL;
    }

    c = valkeyContextInit();
    if (c == NULL) {
        return NULL;
    }
    if (!(options->options & VALKEY_OPT_NONBLOCK)) {
        c->flags |= VALKEY_BLOCK;
    }
    if (options->options & VALKEY_OPT_REUSEADDR) {
        c->flags |= VALKEY_REUSEADDR;
    }
    if (options->options & VALKEY_OPT_NOAUTOFREE) {
        c->flags |= VALKEY_NO_AUTO_FREE;
    }
    if (options->options & VALKEY_OPT_NOAUTOFREEREPLIES) {
        c->flags |= VALKEY_NO_AUTO_FREE_REPLIES;
    }
    if (options->options & VALKEY_OPT_PREFER_IPV4) {
        c->flags |= VALKEY_PREFER_IPV4;
    }
    if (options->options & VALKEY_OPT_PREFER_IPV6) {
        c->flags |= VALKEY_PREFER_IPV6;
    }

    if (options->options & VALKEY_OPT_MPTCP) {
        if (!valkeyHasMptcp()) {
            valkeySetError(c, VALKEY_ERR_PROTOCOL, "MPTCP is not supported on this platform");
            return c;
        }
        c->flags |= VALKEY_MPTCP;
    }

    /* Set any user supplied RESP3 PUSH handler or use freeReplyObject
     * as a default unless specifically flagged that we don't want one. */
    if (options->push_cb != NULL)
        valkeySetPushCallback(c, options->push_cb);
    else if (!(options->options & VALKEY_OPT_NO_PUSH_AUTOFREE))
        valkeySetPushCallback(c, valkeyPushAutoFree);

    c->privdata = options->privdata;
    c->free_privdata = options->free_privdata;
    c->connection_type = options->type;
    /* Make sure we set a valkeyContextFuncs before returning any context. */
    valkeyContextSetFuncs(c);

    if (valkeyContextUpdateConnectTimeout(c, options->connect_timeout) != VALKEY_OK ||
        valkeyContextUpdateCommandTimeout(c, options->command_timeout) != VALKEY_OK) {
        valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
        return c;
    }

    c->funcs->connect(c, options);
    if (c->err == 0 && c->fd != VALKEY_INVALID_FD &&
        options->command_timeout != NULL && (c->flags & VALKEY_BLOCK)) {
        c->funcs->set_timeout(c, *options->command_timeout);
    }

    return c;
}

/* Connect to a server instance. On error the field error in the returned
 * context will be set to the return value of the error function.
 * When no set of reply functions is given, the default set will be used. */
valkeyContext *valkeyConnect(const char *ip, int port) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, ip, port);
    return valkeyConnectWithOptions(&options);
}

valkeyContext *valkeyConnectWithTimeout(const char *ip, int port, const struct timeval tv) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, ip, port);
    options.connect_timeout = &tv;
    return valkeyConnectWithOptions(&options);
}

valkeyContext *valkeyConnectNonBlock(const char *ip, int port) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, ip, port);
    options.options |= VALKEY_OPT_NONBLOCK;
    return valkeyConnectWithOptions(&options);
}

valkeyContext *valkeyConnectBindNonBlock(const char *ip, int port,
                                         const char *source_addr) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, ip, port);
    options.endpoint.tcp.source_addr = source_addr;
    options.options |= VALKEY_OPT_NONBLOCK;
    return valkeyConnectWithOptions(&options);
}

valkeyContext *valkeyConnectBindNonBlockWithReuse(const char *ip, int port,
                                                  const char *source_addr) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, ip, port);
    options.endpoint.tcp.source_addr = source_addr;
    options.options |= VALKEY_OPT_NONBLOCK | VALKEY_OPT_REUSEADDR;
    return valkeyConnectWithOptions(&options);
}

valkeyContext *valkeyConnectUnix(const char *path) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_UNIX(&options, path);
    return valkeyConnectWithOptions(&options);
}

valkeyContext *valkeyConnectUnixWithTimeout(const char *path, const struct timeval tv) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_UNIX(&options, path);
    options.connect_timeout = &tv;
    return valkeyConnectWithOptions(&options);
}

valkeyContext *valkeyConnectUnixNonBlock(const char *path) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_UNIX(&options, path);
    options.options |= VALKEY_OPT_NONBLOCK;
    return valkeyConnectWithOptions(&options);
}

valkeyContext *valkeyConnectFd(valkeyFD fd) {
    valkeyOptions options = {0};
    options.type = VALKEY_CONN_USERFD;
    options.endpoint.fd = fd;
    return valkeyConnectWithOptions(&options);
}

/* Set read/write timeout on a blocking socket. */
int valkeySetTimeout(valkeyContext *c, const struct timeval tv) {
    if (!(c->flags & VALKEY_BLOCK))
        return VALKEY_ERR;

    if (valkeyContextUpdateCommandTimeout(c, &tv) != VALKEY_OK) {
        valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    return c->funcs->set_timeout(c, tv);
}

int valkeyEnableKeepAliveWithInterval(valkeyContext *c, int interval) {
    return valkeyKeepAlive(c, interval);
}

/* Enable connection KeepAlive. */
int valkeyEnableKeepAlive(valkeyContext *c) {
    return valkeyKeepAlive(c, VALKEY_KEEPALIVE_INTERVAL);
}

/* Set the socket option TCP_USER_TIMEOUT. */
int valkeySetTcpUserTimeout(valkeyContext *c, unsigned int timeout) {
    return valkeyContextSetTcpUserTimeout(c, timeout);
}

/* Set a user provided RESP3 PUSH handler and return any old one set. */
valkeyPushFn *valkeySetPushCallback(valkeyContext *c, valkeyPushFn *fn) {
    valkeyPushFn *old = c->push_cb;
    c->push_cb = fn;
    return old;
}

/* Use this function to handle a read event on the descriptor. It will try
 * and read some bytes from the socket and feed them to the reply parser.
 *
 * After this function is called, you may use valkeyGetReplyFromReader to
 * see if there is a reply available. */
int valkeyBufferRead(valkeyContext *c) {
    char buf[1024 * 16];
    int nread;

    /* Return early when the context has seen an error. */
    if (c->err)
        return VALKEY_ERR;

    if (c->funcs->read_zc) {
        char *zc_buf;
        nread = c->funcs->read_zc(c, &zc_buf);
        if (nread < 0) {
            return VALKEY_ERR;
        }
        if (nread > 0 && valkeyReaderFeed(c->reader, zc_buf, nread) != VALKEY_OK) {
            valkeySetError(c, c->reader->err, c->reader->errstr);
            return VALKEY_ERR;
        }
        return c->funcs->read_zc_done(c);
    }
    nread = c->funcs->read(c, buf, sizeof(buf));
    if (nread < 0) {
        return VALKEY_ERR;
    }
    if (nread > 0 && valkeyReaderFeed(c->reader, buf, nread) != VALKEY_OK) {
        valkeySetError(c, c->reader->err, c->reader->errstr);
        return VALKEY_ERR;
    }
    return VALKEY_OK;
}

/* Write the output buffer to the socket.
 *
 * Returns VALKEY_OK when the buffer is empty, or (a part of) the buffer was
 * successfully written to the socket. When the buffer is empty after the
 * write operation, "done" is set to 1 (if given).
 *
 * Returns VALKEY_ERR if an unrecoverable error occurred in the underlying
 * c->funcs->write function.
 */
int valkeyBufferWrite(valkeyContext *c, int *done) {

    /* Return early when the context has seen an error. */
    if (c->err)
        return VALKEY_ERR;

    if (sdslen(c->obuf) > 0) {
        ssize_t nwritten = c->funcs->write(c);
        if (nwritten < 0) {
            return VALKEY_ERR;
        } else if (nwritten > 0) {
            if (nwritten == (ssize_t)sdslen(c->obuf)) {
                sdsfree(c->obuf);
                c->obuf = sdsempty();
                if (c->obuf == NULL)
                    goto oom;
            } else {
                /* No length check in Valkeys sdsrange() */
                if (sdslen(c->obuf) > SSIZE_MAX)
                    goto oom;
                sdsrange(c->obuf, nwritten, -1);
            }
        }
    }
    if (done != NULL)
        *done = (sdslen(c->obuf) == 0);
    return VALKEY_OK;

oom:
    valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
    return VALKEY_ERR;
}

/* Internal helper that returns 1 if the reply was a RESP3 PUSH
 * message and we handled it with a user-provided callback. */
static int valkeyHandledPushReply(valkeyContext *c, void *reply) {
    if (reply && c->push_cb && valkeyIsPushReply(reply)) {
        c->push_cb(c->privdata, reply);
        return 1;
    }

    return 0;
}

/* Get a reply from our reader or set an error in the context. */
int valkeyGetReplyFromReader(valkeyContext *c, void **reply) {
    if (valkeyReaderGetReply(c->reader, reply) == VALKEY_ERR) {
        valkeySetError(c, c->reader->err, c->reader->errstr);
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

/* Internal helper to get the next reply from our reader while handling
 * any PUSH messages we encounter along the way.  This is separate from
 * valkeyGetReplyFromReader so as to not change its behavior. */
static int valkeyNextInBandReplyFromReader(valkeyContext *c, void **reply) {
    do {
        if (valkeyGetReplyFromReader(c, reply) == VALKEY_ERR)
            return VALKEY_ERR;
    } while (valkeyHandledPushReply(c, *reply));

    return VALKEY_OK;
}

int valkeyGetReply(valkeyContext *c, void **reply) {
    int wdone = 0;
    void *aux = NULL;

    /* Try to read pending replies */
    if (valkeyNextInBandReplyFromReader(c, &aux) == VALKEY_ERR)
        return VALKEY_ERR;

    /* For the blocking context, flush output buffer and read reply */
    if (aux == NULL && c->flags & VALKEY_BLOCK) {
        /* Write until done */
        do {
            if (valkeyBufferWrite(c, &wdone) == VALKEY_ERR)
                return VALKEY_ERR;
        } while (!wdone);

        /* Read until there is a reply */
        do {
            if (valkeyBufferRead(c) == VALKEY_ERR)
                return VALKEY_ERR;

            if (valkeyNextInBandReplyFromReader(c, &aux) == VALKEY_ERR)
                return VALKEY_ERR;
        } while (aux == NULL);
    }

    /* Set reply or free it if we were passed NULL */
    if (reply != NULL) {
        *reply = aux;
    } else {
        freeReplyObject(aux);
    }

    return VALKEY_OK;
}

/* Helper function for the valkeyAppendCommand* family of functions.
 *
 * Write a formatted command to the output buffer. When this family
 * is used, you need to call valkeyGetReply yourself to retrieve
 * the reply (or replies in pub/sub).
 */
int valkeyAppendCmdLen(valkeyContext *c, const char *cmd, size_t len) {
    sds newbuf;

    newbuf = sdscatlen(c->obuf, cmd, len);
    if (newbuf == NULL) {
        valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    c->obuf = newbuf;
    return VALKEY_OK;
}

int valkeyAppendFormattedCommand(valkeyContext *c, const char *cmd, size_t len) {

    if (valkeyAppendCmdLen(c, cmd, len) != VALKEY_OK) {
        return VALKEY_ERR;
    }

    return VALKEY_OK;
}

int valkeyvAppendCommand(valkeyContext *c, const char *format, va_list ap) {
    char *cmd;
    int len;

    len = valkeyvFormatCommand(&cmd, format, ap);
    if (len == -1) {
        valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    } else if (len == -2) {
        valkeySetError(c, VALKEY_ERR_OTHER, "Invalid format string");
        return VALKEY_ERR;
    }

    if (valkeyAppendCmdLen(c, cmd, len) != VALKEY_OK) {
        vk_free(cmd);
        return VALKEY_ERR;
    }

    vk_free(cmd);
    return VALKEY_OK;
}

int valkeyAppendCommand(valkeyContext *c, const char *format, ...) {
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = valkeyvAppendCommand(c, format, ap);
    va_end(ap);
    return ret;
}

int valkeyAppendCommandArgv(valkeyContext *c, int argc, const char **argv, const size_t *argvlen) {
    sds cmd;
    long long len;

    len = valkeyFormatSdsCommandArgv(&cmd, argc, argv, argvlen);
    if (len == -1) {
        valkeySetError(c, VALKEY_ERR_OOM, "Out of memory");
        return VALKEY_ERR;
    }

    if (valkeyAppendCmdLen(c, cmd, len) != VALKEY_OK) {
        sdsfree(cmd);
        return VALKEY_ERR;
    }

    sdsfree(cmd);
    return VALKEY_OK;
}

/* Helper function for the valkeyCommand* family of functions.
 *
 * Write a formatted command to the output buffer. If the given context is
 * blocking, immediately read the reply into the "reply" pointer. When the
 * context is non-blocking, the "reply" pointer will not be used and the
 * command is simply appended to the write buffer.
 *
 * Returns the reply when a reply was successfully retrieved. Returns NULL
 * otherwise. When NULL is returned in a blocking context, the error field
 * in the context will be set.
 */
static void *valkeyBlockForReply(valkeyContext *c) {
    void *reply;

    if (c->flags & VALKEY_BLOCK) {
        if (valkeyGetReply(c, &reply) != VALKEY_OK)
            return NULL;
        return reply;
    }
    return NULL;
}

void *valkeyvCommand(valkeyContext *c, const char *format, va_list ap) {
    if (valkeyvAppendCommand(c, format, ap) != VALKEY_OK)
        return NULL;
    return valkeyBlockForReply(c);
}

void *valkeyCommand(valkeyContext *c, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    void *reply = valkeyvCommand(c, format, ap);
    va_end(ap);
    return reply;
}

void *valkeyCommandArgv(valkeyContext *c, int argc, const char **argv, const size_t *argvlen) {
    if (valkeyAppendCommandArgv(c, argc, argv, argvlen) != VALKEY_OK)
        return NULL;
    return valkeyBlockForReply(c);
}
