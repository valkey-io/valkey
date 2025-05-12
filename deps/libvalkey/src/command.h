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

#ifndef VALKEY_COMMAND_H
#define VALKEY_COMMAND_H

#include <stdint.h>

typedef enum cmd_parse_result {
    CMD_PARSE_OK,     /* parsing ok */
    CMD_PARSE_ENOMEM, /* out of memory */
    CMD_PARSE_ERROR,  /* parsing error */
    CMD_PARSE_REPAIR, /* more to parse -> repair parsed & unparsed data */
    CMD_PARSE_AGAIN,  /* incomplete -> parse again */
} cmd_parse_result_t;

typedef enum cmd_type {
    CMD_UNKNOWN,
/* Request commands */
#define COMMAND(_type, _name, _subname, _arity, _keymethod, _keypos) \
    CMD_REQ_VALKEY_##_type,
#include "cmddef.h"
#undef COMMAND
    /* Response types */
    CMD_RSP_VALKEY_STATUS, /* simple string */
    CMD_RSP_VALKEY_ERROR,
    CMD_RSP_VALKEY_INTEGER,
    CMD_RSP_VALKEY_BULK,
    CMD_RSP_VALKEY_MULTIBULK,
    CMD_SENTINEL
} cmd_type_t;

struct keypos {
    char *start;  /* key start pos */
    uint32_t len; /* Length of key */
};

struct cmd {
    cmd_parse_result_t result; /* command parsing result */
    char *errstr;              /* error info when the command parse failed */

    char *cmd;
    uint32_t clen; /* command length */

    struct keypos key; /* First found key in command. */

    /* Command destination */
    int slot_num;    /* Command should be sent to slot.
                      * Set to -1 if command is sent to a given node,
                      * or if a slot can not be found or calculated. */
    char *node_addr; /* Command sent to this node address */
};

void valkey_parse_cmd(struct cmd *r);

struct cmd *command_get(void);
void command_destroy(struct cmd *command);

#endif /* VALKEY_COMMAND_H */
