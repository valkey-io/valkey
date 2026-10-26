/* Some unit tests that don't require Valkey to be running. */

#include "fmacros.h"
#include "win32.h"

#include "cluster.h"
#include "command.h"
#include "test_utils.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helper for the macro ASSERT_KEY below. */
void check_key(const char *key, struct cmd *command, const char *file, int line) {
    if (command->result != CMD_PARSE_OK) {
        fprintf(stderr, "%s:%d: Command parsing failed: %s\n", file, line,
                command->errstr);
        assert(0);
    }
    char *actual_key = command->key.start;
    int actual_keylen = command->key.len;
    if ((int)strlen(key) != actual_keylen ||
        strncmp(key, actual_key, actual_keylen)) {
        fprintf(stderr,
                "%s:%d: Expected key to be \"%s\" but got \"%.*s\"\n",
                file, line, key, actual_keylen, actual_key);
        assert(0);
    }
}

/* Checks that a command (struct cmd *) has the given key (string). */
#define ASSERT_KEY(command, key)                     \
    do {                                             \
        check_key(key, command, __FILE__, __LINE__); \
    } while (0)

void test_valkey_parse_error_nonresp(void) {
    struct cmd *c = command_get();
    c->cmd = strdup("+++Not RESP+++\r\n");
    c->clen = strlen(c->cmd);

    valkey_parse_cmd(c);
    ASSERT_MSG(c->result == CMD_PARSE_ERROR, "Unexpected parse success");
    ASSERT_MSG(!strcmp(c->errstr, "Command parse error"), c->errstr);
    command_destroy(c);
}

/* Create a 65 bytes command string while limit is 64 bytes */
void test_valkey_parse_too_long_cmd(void) {
    char str[66];
    memset(str, 'A', 65);
    str[65] = '\0';
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, str);
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_MSG(c->result == CMD_PARSE_ERROR, "Unexpected parse success");
    ASSERT_MSG(!strncmp(c->errstr, "Unknown command AAAA", 20), c->errstr);
    command_destroy(c);
}

/* Parse a subcommand longer than the 64 char limit */
void test_valkey_parse_too_long_subcommand(void) {
    char subcmd[66];
    memset(subcmd, 'A', 65);
    subcmd[65] = '\0';

    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "XGROUP %s", subcmd);
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_MSG(c->result == CMD_PARSE_ERROR, "Unexpected parse success");
    ASSERT_MSG(!strncmp(c->errstr, "Unknown command XGROUP AAAAAAA", 30), c->errstr);
    command_destroy(c);
}

void test_valkey_parse_unknown_cmd(void) {
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "OIOIOI");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_MSG(c->result == CMD_PARSE_ERROR, "Unexpected parse success");
    ASSERT_MSG(!strcmp(c->errstr, "Unknown command OIOIOI"), c->errstr);
    command_destroy(c);
}

void test_valkey_parse_cmd_get(void) {
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "GET foo");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_MSG(c->result == CMD_PARSE_OK, "Parse not OK");
    ASSERT_KEY(c, "foo");
    command_destroy(c);
}

void test_valkey_parse_cmd_mset(void) {
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "MSET foo val1 bar val2");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_MSG(c->result == CMD_PARSE_OK, "Parse not OK");
    ASSERT_KEY(c, "foo");
    command_destroy(c);
}

void test_valkey_parse_cmd_eval_1(void) {
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "EVAL dummyscript 1 foo");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_MSG(c->result == CMD_PARSE_OK, "Parse not OK");
    ASSERT_KEY(c, "foo");
    command_destroy(c);
}

void test_valkey_parse_cmd_eval_0(void) {
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "EVAL dummyscript 0 foo");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_MSG(c->result == CMD_PARSE_OK, "Parse not OK");
    ASSERT_MSG(c->key.len == 0, "Unexpected key found");
    command_destroy(c);
}

void test_valkey_parse_cmd_xgroup_no_subcommand(void) {
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "XGROUP");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_MSG(c->result == CMD_PARSE_ERROR, "Unexpected parse success");
    ASSERT_MSG(!strcmp(c->errstr, "Unknown command XGROUP"), c->errstr);
    command_destroy(c);
}

void test_valkey_parse_cmd_xgroup_unknown_subcommand(void) {
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "XGROUP OIOIOI");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_MSG(c->result == CMD_PARSE_ERROR, "Unexpected parse success");
    ASSERT_MSG(!strcmp(c->errstr, "Unknown command XGROUP OIOIOI"), c->errstr);
    command_destroy(c);
}

void test_valkey_parse_cmd_xgroup_destroy_no_key(void) {
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "xgroup destroy");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_MSG(c->result == CMD_PARSE_ERROR, "Parse not OK");
    const char *expected_error =
        "Failed to find keys of command XGROUP DESTROY";
    ASSERT_MSG(!strncmp(c->errstr, expected_error, strlen(expected_error)),
               c->errstr);
    command_destroy(c);
}

void test_valkey_parse_cmd_xgroup_destroy_ok(void) {
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "xgroup destroy mystream mygroup");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_KEY(c, "mystream");
    command_destroy(c);
}

void test_valkey_parse_cmd_xreadgroup_ok(void) {
    struct cmd *c = command_get();
    /* Use group name and consumer name "streams" just to try to confuse the
     * parser. The parser shouldn't mistake those for the STREAMS keyword. */
    int len = valkeyFormatCommand(
        &c->cmd, "XREADGROUP GROUP streams streams COUNT 1 streams mystream >");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_KEY(c, "mystream");
    command_destroy(c);
}

void test_valkey_parse_cmd_xread_ok(void) {
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(
        &c->cmd, "XREAD BLOCK 42 STREAMS mystream another $ $");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_KEY(c, "mystream");
    command_destroy(c);
}

void test_valkey_parse_cmd_restore_ok(void) {
    /* The ordering of RESTORE and RESTORE-ASKING in the lookup-table was wrong
     * in a previous version, leading to the command not being found. */
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "restore k 0 xxx");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_KEY(c, "k");
    command_destroy(c);
}

void test_valkey_parse_cmd_restore_asking_ok(void) {
    /* The ordering of RESTORE and RESTORE-ASKING in the lookup-table was wrong
     * in a previous version, leading to the command not being found. */
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "restore-asking k 0 xxx");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_KEY(c, "k");
    command_destroy(c);
}

void test_valkey_parse_cmd_georadius_ro_ok(void) {
    /* The position of GEORADIUS_RO was wrong in a previous version of the
     * lookup-table, leading to the command not being found. */
    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "georadius_ro k 0 0 0 km");
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_KEY(c, "k");
    command_destroy(c);
}

void test_valkey_parse_cmd_sadd_ok(void) {
    char key[201];
    memset(key, 'A', 200);
    key[200] = '\0';

    struct cmd *c = command_get();
    int len = valkeyFormatCommand(&c->cmd, "SADD %s value", key);
    ASSERT_MSG(len >= 0, "Format command error");
    c->clen = len;
    valkey_parse_cmd(c);
    ASSERT_KEY(c, key);
    command_destroy(c);
}

int main(void) {
    test_valkey_parse_error_nonresp();
    test_valkey_parse_too_long_cmd();
    test_valkey_parse_too_long_subcommand();
    test_valkey_parse_unknown_cmd();
    test_valkey_parse_cmd_get();
    test_valkey_parse_cmd_mset();
    test_valkey_parse_cmd_eval_1();
    test_valkey_parse_cmd_eval_0();
    test_valkey_parse_cmd_xgroup_no_subcommand();
    test_valkey_parse_cmd_xgroup_unknown_subcommand();
    test_valkey_parse_cmd_xgroup_destroy_no_key();
    test_valkey_parse_cmd_xgroup_destroy_ok();
    test_valkey_parse_cmd_xreadgroup_ok();
    test_valkey_parse_cmd_xread_ok();
    test_valkey_parse_cmd_restore_ok();
    test_valkey_parse_cmd_restore_asking_ok();
    test_valkey_parse_cmd_georadius_ro_ok();
    test_valkey_parse_cmd_sadd_ok();
    return 0;
}
