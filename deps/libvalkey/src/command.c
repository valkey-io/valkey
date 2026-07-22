/*
 * Copyright (c) 2015-2017, Ieshen Zheng <ieshen.zheng at 163 dot com>
 * Copyright (c) 2020, Nick <heronr1 at gmail dot com>
 * Copyright (c) 2020-2021, Bjorn Svensson <bjorn.a.svensson at est dot tech>
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

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#ifndef _WIN32
#include <strings.h>
#else
#include <malloc.h>
#endif
#include "win32.h"

#include "alloc.h"
#include "command.h"
#include "vkutil.h"

#include <sds.h>

#include <stdio.h>
#include <string.h>

#define MAX_COMMAND_LEN 64
#define LF (uint8_t)10
#define CR (uint8_t)13

typedef enum {
    KEYPOS_NONE,
    KEYPOS_UNKNOWN,
    KEYPOS_INDEX,
    KEYPOS_KEYNUM
} cmd_keypos;

typedef struct {
    cmd_type_t type;           /* A constant identifying the command. */
    const char *name;          /* Command name */
    const char *subname;       /* Subcommand name or NULL */
    cmd_keypos firstkeymethod; /* First key none, unknown, pos or keynum */
    int8_t firstkeypos;        /* Position of first key or the  arg */
    int8_t arity;              /* Arity, neg number means min num args */
} cmddef;

/* Populate the table with code in cmddef.h generated from JSON files. */
static cmddef server_commands[] = {
#define COMMAND(_type, _name, _subname, _arity, _keymethod, _keypos) \
    {.type = CMD_REQ_VALKEY_##_type,                                 \
     .name = _name,                                                  \
     .subname = _subname,                                            \
     .firstkeymethod = KEYPOS_##_keymethod,                          \
     .firstkeypos = _keypos,                                         \
     .arity = _arity},
#include "cmddef.h"
#undef COMMAND
};

static inline void to_upper(char *dst, const char *src, uint32_t len) {
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (src[i] >= 'a' && src[i] <= 'z')
            dst[i] = src[i] - ('a' - 'A');
        else
            dst[i] = src[i];
    }
}

/* Looks up a command or subcommand in the command table. Arg0 and arg1 are used
 * to lookup the command. The function returns the cmddef for a found command,
 * or NULL on failure. */
cmddef *valkey_lookup_cmd(const char *arg0, uint32_t arg0_len, const char *arg1,
                          uint32_t arg1_len) {
    if (arg0_len > MAX_COMMAND_LEN)
        return NULL;
    char cmd[MAX_COMMAND_LEN];
    char subcmd[MAX_COMMAND_LEN] = "";
    int num_commands = sizeof(server_commands) / sizeof(cmddef);
    /* Compare command name in uppercase. */
    to_upper(cmd, arg0, arg0_len);
    /* Find the command using binary search. */
    int left = 0, right = num_commands - 1;
    while (left <= right) {
        int i = (left + right) / 2;
        cmddef *c = &server_commands[i];

        int cmp = strncmp(c->name, cmd, arg0_len);
        if (cmp == 0 && strlen(c->name) > arg0_len)
            cmp = 1; /* "HGETALL" vs "HGET" */

        /* If command name matches, compare subcommand if any */
        if (cmp == 0 && c->subname != NULL) {
            if (arg1 == NULL || arg1_len == 0 || arg1_len > MAX_COMMAND_LEN) {
                /* Command has subcommands, but none given or is too long. */
                return NULL;
            }
            if (subcmd[0] == '\0')
                to_upper(subcmd, arg1, arg1_len);
            cmp = strncmp(c->subname, subcmd, arg1_len);
            if (cmp == 0 && strlen(c->subname) > arg1_len)
                cmp = 1;
        }

        if (cmp < 0) {
            left = i + 1;
        } else if (cmp > 0) {
            right = i - 1;
        } else {
            /* Found it. */
            return c;
        }
    }
    return NULL;
}

/* Parses a bulk string starting at 'p' and ending somewhere before 'end'.
 * Returns the remaining of the input after consuming the bulk string. The
 * pointers *str and *len are pointed to the parsed string and its length. On
 * parse error, NULL is returned. */
char *valkey_parse_bulk(char *p, char *end, char **str, uint32_t *len) {
    uint32_t length = 0;
    if (p >= end || *p++ != '$')
        return NULL;
    while (p < end && *p >= '0' && *p <= '9') {
        length = length * 10 + (uint32_t)(*p++ - '0');
    }
    if (p >= end || *p++ != CR)
        return NULL;
    if (p >= end || *p++ != LF)
        return NULL;
    if (str)
        *str = p;
    if (len)
        *len = length;
    p += length;
    if (p >= end || *p++ != CR)
        return NULL;
    if (p >= end || *p++ != LF)
        return NULL;
    return p;
}

/*
 * Reference: https://valkey.io/docs/topics/protocol/
 *
 * Libvalkey uses the unified protocol to send requests to the Valkey
 * server. In the unified protocol all the arguments sent to the server
 * are binary safe and every request has the following general form:
 *
 *   *<number of arguments> CR LF
 *   $<number of bytes of argument 1> CR LF
 *   <argument data> CR LF
 *   ...
 *   $<number of bytes of argument N> CR LF
 *   <argument data> CR LF
 *
 */
void valkey_parse_cmd(struct cmd *r) {
    assert(r->cmd != NULL && r->clen > 0);
    char *p = r->cmd;
    char *end = r->cmd + r->clen;
    uint32_t rnarg = 0;                  /* Number of args including cmd name */
    int argidx = -1;                     /* Index of last parsed arg */
    char *arg;                           /* Last parsed arg */
    uint32_t arglen;                     /* Length of arg */
    char *arg0 = NULL, *arg1 = NULL;     /* The first two args */
    uint32_t arg0_len = 0, arg1_len = 0; /* Lengths of arg0 and arg1 */
    cmddef *info = NULL;                 /* Command info, when found */

    /* Check that the command line is multi-bulk. */
    if (*p++ != '*')
        goto error;

    /* Parse multi-bulk size (rnarg). */
    while (p < end && *p >= '0' && *p <= '9') {
        rnarg = rnarg * 10 + (uint32_t)(*p++ - '0');
    }
    if (p == end || *p++ != CR)
        goto error;
    if (p == end || *p++ != LF)
        goto error;
    if (rnarg == 0)
        goto error;

    /* Parse the first two args. */
    if ((p = valkey_parse_bulk(p, end, &arg0, &arg0_len)) == NULL)
        goto error;
    argidx++;
    if (rnarg > 1) {
        if ((p = valkey_parse_bulk(p, end, &arg1, &arg1_len)) == NULL)
            goto error;
        argidx++;
    }

    /* Lookup command. */
    if ((info = valkey_lookup_cmd(arg0, arg0_len, arg1, arg1_len)) == NULL)
        goto error; /* Command not found. */

    /* Arity check (negative arity means minimum num args) */
    if ((info->arity >= 0 && (int)rnarg != info->arity) ||
        (info->arity < 0 && (int)rnarg < -info->arity)) {
        goto error;
    }
    if (info->firstkeymethod == KEYPOS_NONE)
        goto done; /* Command takes no keys. */
    if (arg1 == NULL)
        goto error; /* Command takes keys, but no args given. Quick abort. */

    /* Below we assume arg1 != NULL, */

    /* Handle commands where firstkey depends on special logic. */
    if (info->firstkeymethod == KEYPOS_UNKNOWN) {
        /* Keyword-based first key position */
        const char *keyword;
        int startfrom;
        if (info->type == CMD_REQ_VALKEY_XREAD) {
            keyword = "STREAMS";
            startfrom = 1;
        } else if (info->type == CMD_REQ_VALKEY_XREADGROUP) {
            keyword = "STREAMS";
            startfrom = 4;
        } else {
            /* Not reached, but can be reached if Valkey adds more commands. */
            goto error;
        }

        /* Skip forward to the 'startfrom' arg index, then search for the keyword. */
        arg = arg1;
        arglen = arg1_len;
        while (argidx < (int)rnarg - 1) {
            if ((p = valkey_parse_bulk(p, end, &arg, &arglen)) == NULL)
                goto error; /* Keyword not provided, thus no keys. */
            if (argidx++ < startfrom)
                continue; /* Keyword can't appear in a position before 'startfrom' */
            if (!strncasecmp(keyword, arg, arglen)) {
                /* Keyword found. Now the first key is the next arg. */
                if ((p = valkey_parse_bulk(p, end, &arg, &arglen)) == NULL)
                    goto error;
                /* Keep found key. */
                r->key.start = arg;
                r->key.len = arglen;
                goto done;
            }
        }

        /* Keyword not provided. */
        goto error;
    }

    /* Find first key arg. */
    arg = arg1;
    arglen = arg1_len;
    for (; argidx < info->firstkeypos; argidx++) {
        if ((p = valkey_parse_bulk(p, end, &arg, &arglen)) == NULL)
            goto error;
    }

    if (info->firstkeymethod == KEYPOS_KEYNUM) {
        /* The arg specifies the number of keys and the first key is the next
         * arg. Example:
         *
         * EVAL script numkeys [key [key ...]] [arg [arg ...]] */
        if (!strncmp("0", arg, arglen))
            goto done; /* No args. */
        /* One or more args. The first key is the arg after the 'numkeys' arg. */
        if ((p = valkey_parse_bulk(p, end, &arg, &arglen)) == NULL)
            goto error;
        argidx++;
    }

    /* Now arg is the first key and arglen is its length. */

    if (info->type == CMD_REQ_VALKEY_MIGRATE && arglen == 0 &&
        info->firstkeymethod == KEYPOS_INDEX && info->firstkeypos == 3) {
        /* MIGRATE host port <key | ""> destination-db timeout [COPY] [REPLACE]
         * [[AUTH password] | [AUTH2 username password]] [KEYS key [key ...]]
         *
         * The key spec points out arg3 as the first key, but if it's an empty
         * string, we would need to search for the KEYS keyword arg backwards
         * from the end of the command line. This is not implemented. */
        goto error;
    }
    /* Keep found key. */
    r->key.start = arg;
    r->key.len = arglen;

done:
    r->result = CMD_PARSE_OK;
    return;

error:
    r->result = CMD_PARSE_ERROR;
    errno = EINVAL;
    size_t errmaxlen = 100; /* Enough for the error messages below. */
    if (r->errstr == NULL) {
        r->errstr = vk_malloc(errmaxlen);
        if (r->errstr == NULL) {
            r->result = CMD_PARSE_ENOMEM;
            return;
        }
    }

    if (info != NULL && info->subname != NULL)
        snprintf(r->errstr, errmaxlen, "Failed to find keys of command %s %s",
                 info->name, info->subname);
    else if (info != NULL)
        snprintf(r->errstr, errmaxlen, "Failed to find keys of command %s",
                 info->name);
    else if (info == NULL && arg0 != NULL && arg1 != NULL)
        snprintf(r->errstr, errmaxlen, "Unknown command %.*s %.*s", arg0_len,
                 arg0, arg1_len, arg1);
    else if (info == NULL && arg0 != NULL)
        snprintf(r->errstr, errmaxlen, "Unknown command %.*s", arg0_len, arg0);
    else
        snprintf(r->errstr, errmaxlen, "Command parse error");
    return;
}

struct cmd *command_get(void) {
    struct cmd *command;
    command = vk_malloc(sizeof(struct cmd));
    if (command == NULL) {
        return NULL;
    }

    command->result = CMD_PARSE_OK;
    command->errstr = NULL;
    command->cmd = NULL;
    command->clen = 0;
    command->key.start = NULL;
    command->key.len = 0;
    command->slot_num = -1;
    command->node_addr = NULL;

    return command;
}

void command_destroy(struct cmd *command) {
    if (command == NULL) {
        return;
    }

    if (command->cmd != NULL) {
        vk_free(command->cmd);
        command->cmd = NULL;
    }

    if (command->errstr != NULL) {
        vk_free(command->errstr);
        command->errstr = NULL;
    }

    if (command->node_addr != NULL) {
        sdsfree(command->node_addr);
        command->node_addr = NULL;
    }

    vk_free(command);
}
