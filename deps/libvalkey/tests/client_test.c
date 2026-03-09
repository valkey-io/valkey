#include "fmacros.h"

#include "sockcompat.h"
#include "vkutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#else
#define strcasecmp _stricmp
#endif
#include "adapters/poll.h"
#include "async.h"
#include "valkey.h"
#include "valkey_private.h"

#include <sds.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#ifdef VALKEY_TEST_TLS
#include "tls.h"
#endif
#ifdef VALKEY_TEST_RDMA
#include "rdma.h"
#endif
#ifdef VALKEY_TEST_ASYNC
#include "adapters/libevent.h"

#include <event2/event.h>
#endif

enum connection_type {
    CONN_TCP,
    CONN_TCP_CLUSTER,
    CONN_MPTCP,
    CONN_UNIX,
    CONN_FD,
    CONN_TLS,
    CONN_RDMA
};

struct config {
    enum connection_type type;
    struct timeval connect_timeout;

    struct {
        const char *host;
        int port;
    } tcp;

    struct {
        const char *host;
        int port;
    } tcp_cluster;

    struct {
        const char *path;
    } unix_sock;

    struct {
        const char *host;
        int port;
        const char *ca_cert;
        const char *cert;
        const char *key;
    } tls;

    struct {
        const char *host;
        const char *source_addr;
        /* int port; use the same port as TCP */
    } rdma;
};

struct privdata {
    int dtor_counter;
};

struct pushCounters {
    int nil;
    int str;
};

static int insecure_calloc_calls;

#ifdef VALKEY_TEST_TLS
valkeyTLSContext *_tls_ctx = NULL;
#endif

/* The following lines make up our testing "framework" :) */
static int tests = 0, fails = 0, skips = 0;
#define test(_s)                   \
    {                              \
        printf("#%02d ", ++tests); \
        printf(_s);                \
    }
#define test_cond(_c)                          \
    if (_c)                                    \
        printf("\033[0;32mPASSED\033[0;0m\n"); \
    else {                                     \
        printf("\033[0;31mFAILED\033[0;0m\n"); \
        fails++;                               \
    }
#define test_skipped()                           \
    {                                            \
        printf("\033[01;33mSKIPPED\033[0;0m\n"); \
        skips++;                                 \
    }

static void millisleep(int ms) {
#ifdef _MSC_VER
    Sleep(ms);
#else
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};

    nanosleep(&ts, NULL);
#endif
}

static long long usec(void) {
#ifndef _MSC_VER
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec) * 1000000) + tv.tv_usec;
#else
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10;
#endif
}

/* The assert() calls below have side effects, so we need assert()
 * even if we are compiling without asserts (-DNDEBUG). */
#ifdef NDEBUG
#undef assert
#define assert(e) (void)(e)
#endif

#define valkeyTestPanic(msg)                                                      \
    do {                                                                          \
        fprintf(stderr, "PANIC: %s (In function \"%s\", file \"%s\", line %d)\n", \
                msg, __func__, __FILE__, __LINE__);                               \
        exit(1);                                                                  \
    } while (1)

/* Helper to extract server version information.  Aborts on any failure. */
#define VALKEY_VERSION_FIELD "valkey_version:"
#define REDIS_VERSION_FIELD "redis_version:"
void get_server_version(valkeyContext *c, int *majorptr, int *minorptr) {
    valkeyReply *reply;
    char *eptr, *s, *e;
    int major, minor;

    reply = valkeyCommand(c, "INFO");
    if (reply == NULL || c->err || reply->type != VALKEY_REPLY_STRING)
        goto abort;
    if ((s = strstr(reply->str, VALKEY_VERSION_FIELD)) != NULL)
        s += strlen(VALKEY_VERSION_FIELD);
    else if ((s = strstr(reply->str, REDIS_VERSION_FIELD)) != NULL)
        s += strlen(REDIS_VERSION_FIELD);
    else
        goto abort;

    /* We need a field terminator and at least 'x.y.z' (5) bytes of data */
    if ((e = strstr(s, "\r\n")) == NULL || (e - s) < 5)
        goto abort;

    /* Extract version info */
    major = strtol(s, &eptr, 10);
    if (*eptr != '.')
        goto abort;
    minor = strtol(eptr + 1, NULL, 10);

    /* Push info the caller wants */
    if (majorptr)
        *majorptr = major;
    if (minorptr)
        *minorptr = minor;

    freeReplyObject(reply);
    return;

abort:
    freeReplyObject(reply);
    fprintf(stderr, "Error:  Cannot determine server version, aborting\n");
    exit(1);
}

static valkeyContext *select_database(valkeyContext *c) {
    valkeyReply *reply;

    /* Switch to DB 9 for testing, now that we know we can chat. */
    reply = valkeyCommand(c, "SELECT 9");
    assert(reply != NULL);
    freeReplyObject(reply);

    /* Make sure the DB is empty */
    reply = valkeyCommand(c, "DBSIZE");
    assert(reply != NULL);
    if (reply->type == VALKEY_REPLY_INTEGER && reply->integer == 0) {
        /* Awesome, DB 9 is empty and we can continue. */
        freeReplyObject(reply);
    } else {
        printf("Database #9 is not empty, test cannot continue\n");
        exit(1);
    }

    return c;
}

/* Switch protocol */
static void send_hello(valkeyContext *c, int version) {
    valkeyReply *reply;
    int expected;

    reply = valkeyCommand(c, "HELLO %d", version);
    expected = version == 3 ? VALKEY_REPLY_MAP : VALKEY_REPLY_ARRAY;
    assert(reply != NULL && reply->type == expected);
    freeReplyObject(reply);
}

/* Toggle client tracking */
static void send_client_tracking(valkeyContext *c, const char *str) {
    valkeyReply *reply;

    reply = valkeyCommand(c, "CLIENT TRACKING %s", str);
    assert(reply != NULL && reply->type == VALKEY_REPLY_STATUS);
    freeReplyObject(reply);
}

static int disconnect(valkeyContext *c, int keep_fd) {
    valkeyReply *reply;

    /* Make sure we're on DB 9. */
    reply = valkeyCommand(c, "SELECT 9");
    assert(reply != NULL);
    freeReplyObject(reply);
    reply = valkeyCommand(c, "FLUSHDB");
    assert(reply != NULL);
    freeReplyObject(reply);

    /* Free the context as well, but keep the fd if requested. */
    if (keep_fd)
        return valkeyFreeKeepFd(c);
    valkeyFree(c);
    return -1;
}

static void do_tls_handshake(valkeyContext *c) {
#ifdef VALKEY_TEST_TLS
    valkeyInitiateTLSWithContext(c, _tls_ctx);
    if (c->err) {
        printf("TLS error: %s\n", c->errstr);
        valkeyFree(c);
        exit(1);
    }
#else
    (void)c;
#endif
}

static valkeyContext *do_connect(struct config config) {
    valkeyContext *c = NULL;

    if (config.type == CONN_TCP) {
        c = valkeyConnect(config.tcp.host, config.tcp.port);
    } else if (config.type == CONN_TCP_CLUSTER) {
        c = valkeyConnect(config.tcp_cluster.host, config.tcp_cluster.port);
    } else if (config.type == CONN_MPTCP) {
        valkeyOptions options = {0};
        VALKEY_OPTIONS_SET_MPTCP(&options, config.tcp.host, config.tcp.port);
        c = valkeyConnectWithOptions(&options);
    } else if (config.type == CONN_TLS) {
        c = valkeyConnect(config.tls.host, config.tls.port);
    } else if (config.type == CONN_UNIX) {
        c = valkeyConnectUnix(config.unix_sock.path);
#ifdef VALKEY_TEST_RDMA
    } else if (config.type == CONN_RDMA) {
        valkeyOptions options = {0};
        if (config.rdma.source_addr) {
            VALKEY_OPTIONS_SET_RDMA_WITH_SOURCE_ADDR(&options, config.rdma.host,
                                                     config.tcp.port, config.rdma.source_addr);
        } else {
            VALKEY_OPTIONS_SET_RDMA(&options, config.rdma.host, config.tcp.port);
        }
        c = valkeyConnectWithOptions(&options);
#endif
    } else if (config.type == CONN_FD) {
        /* Create a dummy connection just to get an fd to inherit */
        valkeyContext *dummy_ctx = valkeyConnectUnix(config.unix_sock.path);
        if (dummy_ctx) {
            int fd = disconnect(dummy_ctx, 1);
            printf("Connecting to inherited fd %d\n", fd);
            c = valkeyConnectFd(fd);
        }
    } else {
        valkeyTestPanic("Unknown connection type!");
    }

    if (c == NULL) {
        printf("Connection error: can't allocate valkey context\n");
        exit(1);
    } else if (c->err) {
        printf("Connection error: %s\n", c->errstr);
        valkeyFree(c);
        exit(1);
    }

    if (config.type == CONN_TLS) {
        do_tls_handshake(c);
    }

    if (config.type != CONN_TCP_CLUSTER) {
        select_database(c);
    }
    return c;
}

static void do_reconnect(valkeyContext *c, struct config config) {
    valkeyReconnect(c);

    if (config.type == CONN_TLS) {
        do_tls_handshake(c);
    }
}

static void test_format_commands(void) {
    char *cmd;
    int len;

    test("Format command without interpolation: ");
    len = valkeyFormatCommand(&cmd, "SET foo bar");
    test_cond(strncmp(cmd, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", len) == 0 &&
              len == 4 + 4 + (3 + 2) + 4 + (3 + 2) + 4 + (3 + 2));
    vk_free(cmd);

    test("Format command with %%s string interpolation: ");
    len = valkeyFormatCommand(&cmd, "SET %s %s", "foo", "bar");
    test_cond(strncmp(cmd, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", len) == 0 &&
              len == 4 + 4 + (3 + 2) + 4 + (3 + 2) + 4 + (3 + 2));
    vk_free(cmd);

    test("Format command with %%s and an empty string: ");
    len = valkeyFormatCommand(&cmd, "SET %s %s", "foo", "");
    test_cond(strncmp(cmd, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$0\r\n\r\n", len) == 0 &&
              len == 4 + 4 + (3 + 2) + 4 + (3 + 2) + 4 + (0 + 2));
    vk_free(cmd);

    test("Format command with an empty string in between proper interpolations: ");
    len = valkeyFormatCommand(&cmd, "SET %s %s", "", "foo");
    test_cond(strncmp(cmd, "*3\r\n$3\r\nSET\r\n$0\r\n\r\n$3\r\nfoo\r\n", len) == 0 &&
              len == 4 + 4 + (3 + 2) + 4 + (0 + 2) + 4 + (3 + 2));
    vk_free(cmd);

    test("Format command with %%b string interpolation: ");
    len = valkeyFormatCommand(&cmd, "SET %b %b", "foo", (size_t)3, "b\0r", (size_t)3);
    test_cond(strncmp(cmd, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nb\0r\r\n", len) == 0 &&
              len == 4 + 4 + (3 + 2) + 4 + (3 + 2) + 4 + (3 + 2));
    vk_free(cmd);

    test("Format command with %%b and an empty string: ");
    len = valkeyFormatCommand(&cmd, "SET %b %b", "foo", (size_t)3, "", (size_t)0);
    test_cond(strncmp(cmd, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$0\r\n\r\n", len) == 0 &&
              len == 4 + 4 + (3 + 2) + 4 + (3 + 2) + 4 + (0 + 2));
    vk_free(cmd);

    test("Format command with literal %%: ");
    len = valkeyFormatCommand(&cmd, "SET %% %%");
    test_cond(strncmp(cmd, "*3\r\n$3\r\nSET\r\n$1\r\n%\r\n$1\r\n%\r\n", len) == 0 &&
              len == 4 + 4 + (3 + 2) + 4 + (1 + 2) + 4 + (1 + 2));
    vk_free(cmd);

    /* Vararg width depends on the type. These tests make sure that the
     * width is correctly determined using the format and subsequent varargs
     * can correctly be interpolated. */
#define INTEGER_WIDTH_TEST(fmt, type)                                                           \
    do {                                                                                        \
        type value = 123;                                                                       \
        test("Format command with printf-delegation (" #type "): ");                            \
        len = valkeyFormatCommand(&cmd, "key:%08" fmt " str:%s", value, "hello");               \
        test_cond(strncmp(cmd, "*2\r\n$12\r\nkey:00000123\r\n$9\r\nstr:hello\r\n", len) == 0 && \
                  len == 4 + 5 + (12 + 2) + 4 + (9 + 2));                                       \
        vk_free(cmd);                                                                           \
    } while (0)

#define FLOAT_WIDTH_TEST(type)                                                                  \
    do {                                                                                        \
        type value = 123.0;                                                                     \
        test("Format command with printf-delegation (" #type "): ");                            \
        len = valkeyFormatCommand(&cmd, "key:%08.3f str:%s", value, "hello");                   \
        test_cond(strncmp(cmd, "*2\r\n$12\r\nkey:0123.000\r\n$9\r\nstr:hello\r\n", len) == 0 && \
                  len == 4 + 5 + (12 + 2) + 4 + (9 + 2));                                       \
        vk_free(cmd);                                                                           \
    } while (0)

    INTEGER_WIDTH_TEST("d", int);
    INTEGER_WIDTH_TEST("hhd", char);
    INTEGER_WIDTH_TEST("hd", short);
    INTEGER_WIDTH_TEST("ld", long);
    INTEGER_WIDTH_TEST("lld", long long);
    INTEGER_WIDTH_TEST("u", unsigned int);
    INTEGER_WIDTH_TEST("hhu", unsigned char);
    INTEGER_WIDTH_TEST("hu", unsigned short);
    INTEGER_WIDTH_TEST("lu", unsigned long);
    INTEGER_WIDTH_TEST("llu", unsigned long long);
    FLOAT_WIDTH_TEST(float);
    FLOAT_WIDTH_TEST(double);

    test("Format command with unhandled printf format (specifier 'p' not supported): ");
    len = valkeyFormatCommand(&cmd, "key:%08p %b", (void *)1234, "foo", (size_t)3);
    test_cond(len == -1);

    test("Format command with invalid printf format (specifier missing): ");
    len = valkeyFormatCommand(&cmd, "%-");
    test_cond(len == -1);

    test("Format command with %%b and an invalid string (NULL string): ");
    len = valkeyFormatCommand(&cmd, "%b", NULL, 45);
    test_cond(len == -1);

    test("Format command with %%s and an invalid string (NULL string): ");
    len = valkeyFormatCommand(&cmd, "%s", NULL);
    test_cond(len == -1);

    const char *argv[3];
    argv[0] = "SET";
    argv[1] = "foo\0xxx";
    argv[2] = "bar";
    size_t lens[3] = {3, 7, 3};
    int argc = 3;

    test("Format command by passing argc/argv without lengths: ");
    len = valkeyFormatCommandArgv(&cmd, argc, argv, NULL);
    test_cond(strncmp(cmd, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", len) == 0 &&
              len == 4 + 4 + (3 + 2) + 4 + (3 + 2) + 4 + (3 + 2));
    vk_free(cmd);

    test("Format command by passing argc/argv with lengths: ");
    len = valkeyFormatCommandArgv(&cmd, argc, argv, lens);
    test_cond(strncmp(cmd, "*3\r\n$3\r\nSET\r\n$7\r\nfoo\0xxx\r\n$3\r\nbar\r\n", len) == 0 &&
              len == 4 + 4 + (3 + 2) + 4 + (7 + 2) + 4 + (3 + 2));
    vk_free(cmd);

    sds sds_cmd;

    sds_cmd = NULL;
    test("Format command into sds by passing argc/argv without lengths: ");
    len = valkeyFormatSdsCommandArgv(&sds_cmd, argc, argv, NULL);
    test_cond(strncmp(sds_cmd, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", len) == 0 &&
              len == 4 + 4 + (3 + 2) + 4 + (3 + 2) + 4 + (3 + 2));
    sdsfree(sds_cmd);

    sds_cmd = NULL;
    test("Format command into sds by passing argc/argv with lengths: ");
    len = valkeyFormatSdsCommandArgv(&sds_cmd, argc, argv, lens);
    test_cond(strncmp(sds_cmd, "*3\r\n$3\r\nSET\r\n$7\r\nfoo\0xxx\r\n$3\r\nbar\r\n", len) == 0 &&
              len == 4 + 4 + (3 + 2) + 4 + (7 + 2) + 4 + (3 + 2));
    sdsfree(sds_cmd);
}

static void test_append_formatted_commands(struct config config) {
    valkeyContext *c;
    valkeyReply *reply;
    char *cmd;
    int len;

    c = do_connect(config);

    test("Append format command: ");

    len = valkeyFormatCommand(&cmd, "SET foo bar");

    test_cond(valkeyAppendFormattedCommand(c, cmd, len) == VALKEY_OK);

    assert(valkeyGetReply(c, (void *)&reply) == VALKEY_OK);

    vk_free(cmd);
    freeReplyObject(reply);

    disconnect(c, 0);
}

static void test_tcp_options(struct config cfg) {
    valkeyContext *c;

    c = do_connect(cfg);

    test("We can enable TCP_KEEPALIVE: ");
    test_cond(valkeyEnableKeepAlive(c) == VALKEY_OK);

#ifdef TCP_USER_TIMEOUT
    test("We can set TCP_USER_TIMEOUT: ");
    test_cond(valkeySetTcpUserTimeout(c, 100) == VALKEY_OK);
#else
    test("Setting TCP_USER_TIMEOUT errors when unsupported: ");
    test_cond(valkeySetTcpUserTimeout(c, 100) == VALKEY_ERR && c->err == VALKEY_ERR_IO);
#endif

    valkeyFree(c);
}

static void test_unix_keepalive(struct config cfg) {
    valkeyContext *c;
    valkeyReply *r;

    c = do_connect(cfg);

    test("Setting TCP_KEEPALIVE on a unix socket returns an error: ");
    test_cond(valkeyEnableKeepAlive(c) == VALKEY_ERR && c->err == 0);

    test("Setting TCP_KEEPALIVE on a unix socket doesn't break the connection: ");
    r = valkeyCommand(c, "PING");
    test_cond(r != NULL && r->type == VALKEY_REPLY_STATUS && r->len == 4 &&
              !memcmp(r->str, "PONG", 4));
    freeReplyObject(r);

    valkeyFree(c);
}

static void test_reply_reader(void) {
    valkeyReader *reader;
    void *reply, *root;
    int ret;
    int i;

    test("Error handling in reply parser: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, (char *)"@foo\r\n", 6);
    ret = valkeyReaderGetReply(reader, NULL);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Protocol error, got \"@\" as reply type byte") == 0);
    valkeyReaderFree(reader);

    /* when the reply already contains multiple items, they must be free'd
     * on an error. valgrind will bark when this doesn't happen. */
    test("Memory cleanup in reply parser: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, (char *)"*2\r\n", 4);
    valkeyReaderFeed(reader, (char *)"$5\r\nhello\r\n", 11);
    valkeyReaderFeed(reader, (char *)"@foo\r\n", 6);
    ret = valkeyReaderGetReply(reader, NULL);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Protocol error, got \"@\" as reply type byte") == 0);
    valkeyReaderFree(reader);

    reader = valkeyReaderCreate();
    test("Can handle arbitrarily nested multi-bulks: ");
    for (i = 0; i < 128; i++) {
        valkeyReaderFeed(reader, (char *)"*1\r\n", 4);
    }
    valkeyReaderFeed(reader, (char *)"$6\r\nLOLWUT\r\n", 12);
    ret = valkeyReaderGetReply(reader, &reply);
    root = reply; /* Keep track of the root reply */
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_ARRAY &&
              ((valkeyReply *)reply)->elements == 1);

    test("Can parse arbitrarily nested multi-bulks correctly: ");
    while (i--) {
        assert(reply != NULL && ((valkeyReply *)reply)->type == VALKEY_REPLY_ARRAY);
        reply = ((valkeyReply *)reply)->element[0];
    }
    test_cond(((valkeyReply *)reply)->type == VALKEY_REPLY_STRING &&
              !memcmp(((valkeyReply *)reply)->str, "LOLWUT", 6));
    freeReplyObject(root);
    valkeyReaderFree(reader);

    test("Correctly parses LLONG_MAX: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, ":9223372036854775807\r\n", 22);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_INTEGER &&
              ((valkeyReply *)reply)->integer == LLONG_MAX);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Set error when > LLONG_MAX: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, ":9223372036854775808\r\n", 22);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Bad integer value") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Correctly parses LLONG_MIN: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, ":-9223372036854775808\r\n", 23);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_INTEGER &&
              ((valkeyReply *)reply)->integer == LLONG_MIN);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Set error when < LLONG_MIN: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, ":-9223372036854775809\r\n", 23);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Bad integer value") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Set error when array < -1: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "*-2\r\n+asdf\r\n", 12);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Multi-bulk length out of range") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Set error when bulk < -1: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "$-2\r\nasdf\r\n", 11);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Bulk string length out of range") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Can configure maximum multi-bulk elements: ");
    reader = valkeyReaderCreate();
    reader->maxelements = 1024;
    valkeyReaderFeed(reader, "*1025\r\n", 7);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Multi-bulk length out of range") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Multi-bulk never overflows regardless of maxelements: ");
    size_t bad_mbulk_len = (SIZE_MAX / sizeof(void *)) + 3;
    char bad_mbulk_reply[100];
    snprintf(bad_mbulk_reply, sizeof(bad_mbulk_reply), "*%llu\r\n+asdf\r\n",
             (unsigned long long)bad_mbulk_len);

    reader = valkeyReaderCreate();
    reader->maxelements = 0; /* Don't rely on default limit */
    valkeyReaderFeed(reader, bad_mbulk_reply, strlen(bad_mbulk_reply));
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR && strcasecmp(reader->errstr, "Out of memory") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

#if LLONG_MAX > SIZE_MAX
    test("Set error when array > SIZE_MAX: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "*9223372036854775807\r\n+asdf\r\n", 29);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Multi-bulk length out of range") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Set error when bulk > SIZE_MAX: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "$9223372036854775807\r\nasdf\r\n", 28);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Bulk string length out of range") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);
#endif

    test("Works with NULL functions for reply: ");
    reader = valkeyReaderCreate();
    reader->fn = NULL;
    valkeyReaderFeed(reader, (char *)"+OK\r\n", 5);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK && reply == (void *)VALKEY_REPLY_STATUS);
    valkeyReaderFree(reader);

    test("Works when a single newline (\\r\\n) covers two calls to feed: ");
    reader = valkeyReaderCreate();
    reader->fn = NULL;
    valkeyReaderFeed(reader, (char *)"+OK\r", 4);
    ret = valkeyReaderGetReply(reader, &reply);
    assert(ret == VALKEY_OK && reply == NULL);
    valkeyReaderFeed(reader, (char *)"\n", 1);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK && reply == (void *)VALKEY_REPLY_STATUS);
    valkeyReaderFree(reader);

    test("Don't reset state after protocol error: ");
    reader = valkeyReaderCreate();
    reader->fn = NULL;
    valkeyReaderFeed(reader, (char *)"x", 1);
    ret = valkeyReaderGetReply(reader, &reply);
    assert(ret == VALKEY_ERR);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR && reply == NULL);
    valkeyReaderFree(reader);

    test("Don't reset state after protocol error(not segfault): ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, (char *)"*3\r\n$3\r\nSET\r\n$5\r\nhello\r\n$", 25);
    ret = valkeyReaderGetReply(reader, &reply);
    assert(ret == VALKEY_OK);
    valkeyReaderFeed(reader, (char *)"3\r\nval\r\n", 8);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_ARRAY &&
              ((valkeyReply *)reply)->elements == 3);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    /* Regression test for issue #45 on GitHub. */
    test("Don't do empty allocation for empty multi bulk: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, (char *)"*0\r\n", 4);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_ARRAY &&
              ((valkeyReply *)reply)->elements == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    /* RESP3 verbatim strings (GitHub issue #802) */
    test("Can parse RESP3 verbatim strings: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, (char *)"=10\r\ntxt:LOLWUT\r\n", 17);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_VERB &&
              !memcmp(((valkeyReply *)reply)->str, "LOLWUT", 6));
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    /* RESP3 push messages (GitHub issue #815) */
    test("Can parse RESP3 push messages: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, (char *)">2\r\n$6\r\nLOLWUT\r\n:42\r\n", 21);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_PUSH &&
              ((valkeyReply *)reply)->elements == 2 &&
              ((valkeyReply *)reply)->element[0]->type == VALKEY_REPLY_STRING &&
              !memcmp(((valkeyReply *)reply)->element[0]->str, "LOLWUT", 6) &&
              ((valkeyReply *)reply)->element[1]->type == VALKEY_REPLY_INTEGER &&
              ((valkeyReply *)reply)->element[1]->integer == 42);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Can parse RESP3 doubles: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, ",3.14159265358979323846\r\n", 25);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_DOUBLE &&
              fabs(((valkeyReply *)reply)->dval - 3.14159265358979323846) < 0.00000001 &&
              ((valkeyReply *)reply)->len == 22 &&
              strcmp(((valkeyReply *)reply)->str, "3.14159265358979323846") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Set error on invalid RESP3 double: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, ",3.14159\000265358979323846\r\n", 26);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Bad double value") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Correctly parses RESP3 double INFINITY: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, ",inf\r\n", 6);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_DOUBLE &&
              isinf(((valkeyReply *)reply)->dval) &&
              ((valkeyReply *)reply)->dval > 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Correctly parses RESP3 double NaN: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, ",nan\r\n", 6);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_DOUBLE &&
              isnan(((valkeyReply *)reply)->dval));
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Correctly parses RESP3 double -Nan: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, ",-nan\r\n", 7);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_DOUBLE &&
              isnan(((valkeyReply *)reply)->dval));
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Can parse RESP3 nil: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "_\r\n", 3);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_NIL);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Set error on invalid RESP3 nil: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "_nil\r\n", 6);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Bad nil value") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Can parse RESP3 bool (true): ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "#t\r\n", 4);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_BOOL &&
              ((valkeyReply *)reply)->integer);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Can parse RESP3 bool (false): ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "#f\r\n", 4);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_BOOL &&
              !((valkeyReply *)reply)->integer);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Set error on invalid RESP3 bool: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "#foobar\r\n", 9);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_ERR &&
              strcasecmp(reader->errstr, "Bad bool value") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Can parse RESP3 map: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "%2\r\n+first\r\n:123\r\n$6\r\nsecond\r\n#t\r\n", 34);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_MAP &&
              ((valkeyReply *)reply)->elements == 4 &&
              ((valkeyReply *)reply)->element[0]->type == VALKEY_REPLY_STATUS &&
              ((valkeyReply *)reply)->element[0]->len == 5 &&
              !strcmp(((valkeyReply *)reply)->element[0]->str, "first") &&
              ((valkeyReply *)reply)->element[1]->type == VALKEY_REPLY_INTEGER &&
              ((valkeyReply *)reply)->element[1]->integer == 123 &&
              ((valkeyReply *)reply)->element[2]->type == VALKEY_REPLY_STRING &&
              ((valkeyReply *)reply)->element[2]->len == 6 &&
              !strcmp(((valkeyReply *)reply)->element[2]->str, "second") &&
              ((valkeyReply *)reply)->element[3]->type == VALKEY_REPLY_BOOL &&
              ((valkeyReply *)reply)->element[3]->integer);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Can parse RESP3 attribute: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "|2\r\n+foo\r\n:123\r\n+bar\r\n#t\r\n", 26);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_ATTR &&
              ((valkeyReply *)reply)->elements == 4 &&
              ((valkeyReply *)reply)->element[0]->type == VALKEY_REPLY_STATUS &&
              ((valkeyReply *)reply)->element[0]->len == 3 &&
              !strcmp(((valkeyReply *)reply)->element[0]->str, "foo") &&
              ((valkeyReply *)reply)->element[1]->type == VALKEY_REPLY_INTEGER &&
              ((valkeyReply *)reply)->element[1]->integer == 123 &&
              ((valkeyReply *)reply)->element[2]->type == VALKEY_REPLY_STATUS &&
              ((valkeyReply *)reply)->element[2]->len == 3 &&
              !strcmp(((valkeyReply *)reply)->element[2]->str, "bar") &&
              ((valkeyReply *)reply)->element[3]->type == VALKEY_REPLY_BOOL &&
              ((valkeyReply *)reply)->element[3]->integer);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Can parse RESP3 set: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "~5\r\n+orange\r\n$5\r\napple\r\n#f\r\n:100\r\n:999\r\n", 40);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_SET &&
              ((valkeyReply *)reply)->elements == 5 &&
              ((valkeyReply *)reply)->element[0]->type == VALKEY_REPLY_STATUS &&
              ((valkeyReply *)reply)->element[0]->len == 6 &&
              !strcmp(((valkeyReply *)reply)->element[0]->str, "orange") &&
              ((valkeyReply *)reply)->element[1]->type == VALKEY_REPLY_STRING &&
              ((valkeyReply *)reply)->element[1]->len == 5 &&
              !strcmp(((valkeyReply *)reply)->element[1]->str, "apple") &&
              ((valkeyReply *)reply)->element[2]->type == VALKEY_REPLY_BOOL &&
              !((valkeyReply *)reply)->element[2]->integer &&
              ((valkeyReply *)reply)->element[3]->type == VALKEY_REPLY_INTEGER &&
              ((valkeyReply *)reply)->element[3]->integer == 100 &&
              ((valkeyReply *)reply)->element[4]->type == VALKEY_REPLY_INTEGER &&
              ((valkeyReply *)reply)->element[4]->integer == 999);
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Can parse RESP3 bignum: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "(3492890328409238509324850943850943825024385\r\n", 46);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_BIGNUM &&
              ((valkeyReply *)reply)->len == 43 &&
              !strcmp(((valkeyReply *)reply)->str, "3492890328409238509324850943850943825024385"));
    freeReplyObject(reply);
    valkeyReaderFree(reader);

    test("Can parse RESP3 doubles in an array: ");
    reader = valkeyReaderCreate();
    valkeyReaderFeed(reader, "*1\r\n,3.14159265358979323846\r\n", 29);
    ret = valkeyReaderGetReply(reader, &reply);
    test_cond(ret == VALKEY_OK &&
              ((valkeyReply *)reply)->type == VALKEY_REPLY_ARRAY &&
              ((valkeyReply *)reply)->elements == 1 &&
              ((valkeyReply *)reply)->element[0]->type == VALKEY_REPLY_DOUBLE &&
              fabs(((valkeyReply *)reply)->element[0]->dval - 3.14159265358979323846) < 0.00000001 &&
              ((valkeyReply *)reply)->element[0]->len == 22 &&
              strcmp(((valkeyReply *)reply)->element[0]->str, "3.14159265358979323846") == 0);
    freeReplyObject(reply);
    valkeyReaderFree(reader);
}

static void test_free_null(void) {
    void *valkeyCtx = NULL;
    void *reply = NULL;

    test("Don't fail when valkeyFree is passed a NULL value: ");
    valkeyFree(valkeyCtx);
    test_cond(valkeyCtx == NULL);

    test("Don't fail when freeReplyObject is passed a NULL value: ");
    freeReplyObject(reply);
    test_cond(reply == NULL);
}

/* Wrap malloc to abort on failure so OOM checks don't make the test logic
 * harder to follow. */
static void *vk_malloc_safe(size_t size) {
    void *ptr = vk_malloc(size);
    if (ptr == NULL) {
        fprintf(stderr, "Error:  Out of memory\n");
        exit(-1);
    }

    return ptr;
}

static void *vk_malloc_fail(size_t size) {
    (void)size;
    return NULL;
}

static void *vk_calloc_fail(size_t nmemb, size_t size) {
    (void)nmemb;
    (void)size;
    return NULL;
}

static void *vk_calloc_insecure(size_t nmemb, size_t size) {
    (void)nmemb;
    (void)size;
    insecure_calloc_calls++;
    return (void *)0xdeadc0de;
}

static void *vk_realloc_fail(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
    return NULL;
}

static char *vk_test_strdup(const char *s) {
    size_t len;
    char *dup;

    len = strlen(s);
    dup = vk_malloc_safe(len + 1);

    memcpy(dup, s, len + 1);

    return dup;
}

static void test_allocator_injection(void) {
    void *ptr;

    valkeyAllocFuncs ha = {
        .mallocFn = vk_malloc_fail,
        .callocFn = vk_calloc_fail,
        .reallocFn = vk_realloc_fail,
        .strdupFn = vk_test_strdup,
        .freeFn = free,
    };

    // Override valkey allocators
    valkeySetAllocators(&ha);

    test("valkeyContext uses injected allocators: ");
    valkeyContext *c = valkeyConnect("localhost", 6379);
    test_cond(c == NULL);

    test("valkeyReader uses injected allocators: ");
    valkeyReader *reader = valkeyReaderCreate();
    test_cond(reader == NULL);

    /* Make sure libvalkey itself protects against a non-overflow checking calloc */
    test("libvalkey calloc wrapper protects against overflow: ");
    ha.callocFn = vk_calloc_insecure;
    valkeySetAllocators(&ha);
    ptr = vk_calloc((SIZE_MAX / sizeof(void *)) + 3, sizeof(void *));
    test_cond(ptr == NULL && insecure_calloc_calls == 0);

    // Return allocators to default
    valkeyResetAllocators();
}

#define VALKEY_BAD_DOMAIN "nonexistent.example.com"
static void test_blocking_connection_errors(void) {
    struct addrinfo hints = {.ai_family = AF_INET};
    struct addrinfo *ai_tmp = NULL;
    valkeyContext *c;

    int rv = getaddrinfo(VALKEY_BAD_DOMAIN, "6379", &hints, &ai_tmp);
    if (rv != 0) {
        // Address does *not* exist
        test("Returns error when host cannot be resolved: ");
        // First see if this domain name *actually* resolves to NXDOMAIN
        c = valkeyConnect(VALKEY_BAD_DOMAIN, 6379);
        test_cond(
            c->err == VALKEY_ERR_OTHER &&
            (strcmp(c->errstr, "Name or service not known") == 0 ||
             strcmp(c->errstr, "Can't resolve: " VALKEY_BAD_DOMAIN) == 0 ||
             strcmp(c->errstr, "Name does not resolve") == 0 ||
             strcmp(c->errstr, "nodename nor servname provided, or not known") == 0 ||
             strcmp(c->errstr, "node name or service name not known") == 0 ||
             strcmp(c->errstr, "No address associated with hostname") == 0 ||
             strcmp(c->errstr, "Temporary failure in name resolution") == 0 ||
             strcmp(c->errstr, "hostname nor servname provided, or not known") == 0 ||
             strcmp(c->errstr, "no address associated with name") == 0 ||
             strcmp(c->errstr, "No such host is known. ") == 0));
        valkeyFree(c);
    } else {
        printf("Skipping NXDOMAIN test. Found evil ISP!\n");
        freeaddrinfo(ai_tmp);
    }

#ifndef _WIN32
    valkeyOptions opt = {0};
    struct timeval tv;

    test("Returns error when the port is not open: ");
    c = valkeyConnect((char *)"localhost", 1);
    test_cond(c->err == VALKEY_ERR_IO &&
              strcmp(c->errstr, "Connection refused") == 0);
    valkeyFree(c);

    /* Verify we don't regress from the fix in PR #1180 */
    test("We don't clobber connection exception with setsockopt error: ");
    tv = (struct timeval){.tv_sec = 0, .tv_usec = 500000};
    opt.command_timeout = opt.connect_timeout = &tv;
    VALKEY_OPTIONS_SET_TCP(&opt, "localhost", 10337);
    c = valkeyConnectWithOptions(&opt);
#ifdef __CYGWIN__
    /* Cygwin's socket layer will poll until timeout. */
    test_cond(c->err == VALKEY_ERR_IO &&
              strcmp(c->errstr, "Connection timed out") == 0);
#else
    test_cond(c->err == VALKEY_ERR_IO &&
              strcmp(c->errstr, "Connection refused") == 0);
#endif
    valkeyFree(c);

    test("Returns error when the unix_sock socket path doesn't accept connections: ");
    c = valkeyConnectUnix((char *)"/tmp/nonexistent.sock");
    test_cond(c->err == VALKEY_ERR_IO); /* Don't care about the message... */
    valkeyFree(c);
#endif
}

/* Test push handler */
void push_handler(void *privdata, void *r) {
    struct pushCounters *pcounts = privdata;
    valkeyReply *reply = r, *payload;

    assert(reply && reply->type == VALKEY_REPLY_PUSH && reply->elements == 2);

    payload = reply->element[1];
    if (payload->type == VALKEY_REPLY_ARRAY) {
        payload = payload->element[0];
    }

    if (payload->type == VALKEY_REPLY_STRING) {
        pcounts->str++;
    } else if (payload->type == VALKEY_REPLY_NIL) {
        pcounts->nil++;
    }

    freeReplyObject(reply);
}

/* Dummy function just to test setting a callback with valkeyOptions */
void push_handler_async(valkeyAsyncContext *ac, void *reply) {
    (void)ac;
    (void)reply;
}

static void test_resp3_push_handler(valkeyContext *c) {
    struct pushCounters pc = {0};
    valkeyPushFn *old = NULL;
    valkeyReply *reply;
    void *privdata;

    /* Switch to RESP3 and turn on client tracking */
    send_hello(c, 3);
    send_client_tracking(c, "ON");
    privdata = c->privdata;
    c->privdata = &pc;

    reply = valkeyCommand(c, "GET key:0");
    assert(reply != NULL);
    freeReplyObject(reply);

    test("RESP3 PUSH messages are handled out of band by default: ");
    reply = valkeyCommand(c, "SET key:0 val:0");
    test_cond(reply != NULL && reply->type == VALKEY_REPLY_STATUS);
    freeReplyObject(reply);

    assert((reply = valkeyCommand(c, "GET key:0")) != NULL);
    freeReplyObject(reply);

    old = valkeySetPushCallback(c, push_handler);
    test("We can set a custom RESP3 PUSH handler: ");
    reply = valkeyCommand(c, "SET key:0 val:0");
    /* We need another command because depending on the server version, the
     * notification may be delivered after the command's reply. */
    assert(reply != NULL);
    freeReplyObject(reply);
    reply = valkeyCommand(c, "PING");
    test_cond(reply != NULL && reply->type == VALKEY_REPLY_STATUS && pc.str == 1);
    freeReplyObject(reply);

    test("We properly handle a NIL invalidation payload: ");
    reply = valkeyCommand(c, "FLUSHDB");
    assert(reply != NULL);
    freeReplyObject(reply);
    reply = valkeyCommand(c, "PING");
    test_cond(reply != NULL && reply->type == VALKEY_REPLY_STATUS && pc.nil == 1);
    freeReplyObject(reply);

    /* Unset the push callback and generate an invalidate message making
     * sure it is not handled out of band. */
    test("With no handler, PUSH replies come in-band: ");
    valkeySetPushCallback(c, NULL);
    assert((reply = valkeyCommand(c, "GET key:0")) != NULL);
    freeReplyObject(reply);
    assert((reply = valkeyCommand(c, "SET key:0 invalid")) != NULL);
    /* Depending on server version, we may receive either push notification or
     * status reply. Both cases are valid. */
    if (reply->type == VALKEY_REPLY_STATUS) {
        freeReplyObject(reply);
        reply = valkeyCommand(c, "PING");
    }
    test_cond(reply->type == VALKEY_REPLY_PUSH);
    freeReplyObject(reply);

    test("With no PUSH handler, no replies are lost: ");
    assert(valkeyGetReply(c, (void **)&reply) == VALKEY_OK);
    test_cond(reply != NULL && reply->type == VALKEY_REPLY_STATUS);
    freeReplyObject(reply);

    /* Return to the originally set PUSH handler */
    assert(old != NULL);
    valkeySetPushCallback(c, old);

    /* Switch back to RESP2 and disable tracking */
    c->privdata = privdata;
    send_client_tracking(c, "OFF");
    send_hello(c, 2);
}

valkeyOptions get_server_tcp_options(struct config config) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, config.tcp.host, config.tcp.port);
    return options;
}

valkeyOptions get_server_tcp_cluster_options(struct config config) {
    valkeyOptions options = {0};
    VALKEY_OPTIONS_SET_TCP(&options, config.tcp_cluster.host, config.tcp_cluster.port);
    return options;
}

static void test_resp3_push_options(struct config config) {
    valkeyAsyncContext *ac;
    valkeyContext *c;
    valkeyOptions options;

    test("We set a default RESP3 handler for valkeyContext: ");
    options = get_server_tcp_options(config);
    assert((c = valkeyConnectWithOptions(&options)) != NULL);
    test_cond(c->push_cb != NULL);
    valkeyFree(c);

    test("We don't set a default RESP3 push handler for valkeyAsyncContext: ");
    options = get_server_tcp_options(config);
    assert((ac = valkeyAsyncConnectWithOptions(&options)) != NULL);
    test_cond(ac->c.push_cb == NULL);
    valkeyAsyncFree(ac);

    test("Our VALKEY_OPT_NO_PUSH_AUTOFREE flag works: ");
    options = get_server_tcp_options(config);
    options.options |= VALKEY_OPT_NO_PUSH_AUTOFREE;
    assert((c = valkeyConnectWithOptions(&options)) != NULL);
    test_cond(c->push_cb == NULL);
    valkeyFree(c);

    test("We can use valkeyOptions to set a custom PUSH handler for valkeyContext: ");
    options = get_server_tcp_options(config);
    options.push_cb = push_handler;
    assert((c = valkeyConnectWithOptions(&options)) != NULL);
    test_cond(c->push_cb == push_handler);
    valkeyFree(c);

    test("We can use valkeyOptions to set a custom PUSH handler for valkeyAsyncContext: ");
    options = get_server_tcp_options(config);
    options.async_push_cb = push_handler_async;
    assert((ac = valkeyAsyncConnectWithOptions(&options)) != NULL);
    test_cond(ac->push_cb == push_handler_async);
    valkeyAsyncFree(ac);
}

void free_privdata(void *privdata) {
    struct privdata *data = privdata;
    data->dtor_counter++;
}

static void test_privdata_hooks(struct config config) {
    struct privdata data = {0};
    valkeyOptions options;
    valkeyContext *c;

    test("We can use valkeyOptions to set privdata: ");
    options = get_server_tcp_options(config);
    VALKEY_OPTIONS_SET_PRIVDATA(&options, &data, free_privdata);
    assert((c = valkeyConnectWithOptions(&options)) != NULL);
    test_cond(c->privdata == &data);

    test("Our privdata destructor fires when we free the context: ");
    valkeyFree(c);
    test_cond(data.dtor_counter == 1);
}

static void test_blocking_connection(struct config config) {
    valkeyContext *c;
    valkeyReply *reply;
    int major;

    c = do_connect(config);

    test("Is able to deliver commands: ");
    reply = valkeyCommand(c, "PING");
    test_cond(reply->type == VALKEY_REPLY_STATUS &&
              strcasecmp(reply->str, "pong") == 0);
    freeReplyObject(reply);

    test("Is a able to send commands verbatim: ");
    reply = valkeyCommand(c, "SET foo bar");
    test_cond(reply->type == VALKEY_REPLY_STATUS &&
              strcasecmp(reply->str, "ok") == 0);
    freeReplyObject(reply);

    test("%%s String interpolation works: ");
    reply = valkeyCommand(c, "SET %s %s", "foo", "hello world");
    freeReplyObject(reply);
    reply = valkeyCommand(c, "GET foo");
    test_cond(reply->type == VALKEY_REPLY_STRING &&
              strcmp(reply->str, "hello world") == 0);
    freeReplyObject(reply);

    test("%%b String interpolation works: ");
    reply = valkeyCommand(c, "SET %b %b", "foo", (size_t)3, "hello\x00world", (size_t)11);
    freeReplyObject(reply);
    reply = valkeyCommand(c, "GET foo");
    test_cond(reply->type == VALKEY_REPLY_STRING &&
              memcmp(reply->str, "hello\x00world", 11) == 0);

    test("Binary reply length is correct: ");
    test_cond(reply->len == 11)
        freeReplyObject(reply);

    test("Can parse nil replies: ");
    reply = valkeyCommand(c, "GET nokey");
    test_cond(reply->type == VALKEY_REPLY_NIL)
        freeReplyObject(reply);

    /* test 7 */
    test("Can parse integer replies: ");
    reply = valkeyCommand(c, "INCR mycounter");
    test_cond(reply->type == VALKEY_REPLY_INTEGER && reply->integer == 1);
    freeReplyObject(reply);

    test("Can parse multi bulk replies: ");
    freeReplyObject(valkeyCommand(c, "LPUSH mylist foo"));
    freeReplyObject(valkeyCommand(c, "LPUSH mylist bar"));
    reply = valkeyCommand(c, "LRANGE mylist 0 -1");
    test_cond(reply->type == VALKEY_REPLY_ARRAY &&
              reply->elements == 2 &&
              !memcmp(reply->element[0]->str, "bar", 3) &&
              !memcmp(reply->element[1]->str, "foo", 3));
    freeReplyObject(reply);

    /* m/e with multi bulk reply *before* other reply.
     * specifically test ordering of reply items to parse. */
    test("Can handle nested multi bulk replies: ");
    freeReplyObject(valkeyCommand(c, "MULTI"));
    freeReplyObject(valkeyCommand(c, "LRANGE mylist 0 -1"));
    freeReplyObject(valkeyCommand(c, "PING"));
    reply = (valkeyCommand(c, "EXEC"));
    test_cond(reply->type == VALKEY_REPLY_ARRAY &&
              reply->elements == 2 &&
              reply->element[0]->type == VALKEY_REPLY_ARRAY &&
              reply->element[0]->elements == 2 &&
              !memcmp(reply->element[0]->element[0]->str, "bar", 3) &&
              !memcmp(reply->element[0]->element[1]->str, "foo", 3) &&
              reply->element[1]->type == VALKEY_REPLY_STATUS &&
              strcasecmp(reply->element[1]->str, "pong") == 0);
    freeReplyObject(reply);

    test("Send command by passing argc/argv: ");
    const char *argv[3] = {"SET", "foo", "bar"};
    size_t argvlen[3] = {3, 3, 3};
    reply = valkeyCommandArgv(c, 3, argv, argvlen);
    test_cond(reply->type == VALKEY_REPLY_STATUS);
    freeReplyObject(reply);

    /* Make sure passing NULL to valkeyGetReply is safe */
    test("Can pass NULL to valkeyGetReply: ");
    assert(valkeyAppendCommand(c, "PING") == VALKEY_OK);
    test_cond(valkeyGetReply(c, NULL) == VALKEY_OK);

    get_server_version(c, &major, NULL);
    if (major >= 6)
        test_resp3_push_handler(c);
    test_resp3_push_options(config);

    test_privdata_hooks(config);

    disconnect(c, 0);
}

/* Send DEBUG SLEEP 0 to detect if we have this command */
static int detect_debug_sleep(valkeyContext *c) {
    int detected;
    valkeyReply *reply = valkeyCommand(c, "DEBUG SLEEP 0\r\n");

    if (reply == NULL || c->err) {
        const char *cause = c->err ? c->errstr : "(none)";
        fprintf(stderr, "Error testing for DEBUG SLEEP (server error: %s), exiting\n", cause);
        exit(-1);
    }

    detected = reply->type == VALKEY_REPLY_STATUS;
    freeReplyObject(reply);

    return detected;
}

static void test_blocking_connection_timeouts(struct config config) {
    valkeyContext *c;
    valkeyReply *reply;
    ssize_t s;
    const char *sleep_cmd = "DEBUG SLEEP 1\r\n";
    struct timeval tv = {.tv_sec = 0, .tv_usec = 10000};

    c = do_connect(config);
    test("Successfully completes a command when the timeout is not exceeded: ");
    reply = valkeyCommand(c, "SET foo fast");
    freeReplyObject(reply);
    valkeySetTimeout(c, tv);
    reply = valkeyCommand(c, "GET foo");
    test_cond(reply != NULL && reply->type == VALKEY_REPLY_STRING && memcmp(reply->str, "fast", 4) == 0);
    freeReplyObject(reply);
    disconnect(c, 0);

    c = do_connect(config);
    test("Does not return a reply when the command times out: ");
    if (detect_debug_sleep(c)) {
        valkeyAppendFormattedCommand(c, sleep_cmd, strlen(sleep_cmd));

        // flush connection buffer without waiting for the reply
        s = c->funcs->write(c);
        assert(s == (ssize_t)sdslen(c->obuf));
        sdsfree(c->obuf);
        c->obuf = sdsempty();

        valkeySetTimeout(c, tv);
        reply = valkeyCommand(c, "GET foo");
#ifndef _WIN32
        test_cond(s > 0 && reply == NULL && c->err == VALKEY_ERR_IO &&
                  strcmp(c->errstr, "Resource temporarily unavailable") == 0);
#else
        test_cond(s > 0 && reply == NULL && c->err == VALKEY_ERR_TIMEOUT &&
                  strcmp(c->errstr, "recv timeout") == 0);
#endif
        freeReplyObject(reply);

        // wait for the DEBUG SLEEP to complete so that the server is unblocked for the following tests
        millisleep(1500);
    } else {
        test_skipped();
    }

    test("Reconnect properly reconnects after a timeout: ");
    do_reconnect(c, config);
    reply = valkeyCommand(c, "PING");
    test_cond(reply != NULL && reply->type == VALKEY_REPLY_STATUS && strcmp(reply->str, "PONG") == 0);
    freeReplyObject(reply);

    test("Reconnect properly uses owned parameters: ");
    config.tcp.host = "foo";
    config.unix_sock.path = "foo";
    do_reconnect(c, config);
    reply = valkeyCommand(c, "PING");
    test_cond(reply != NULL && reply->type == VALKEY_REPLY_STATUS && strcmp(reply->str, "PONG") == 0);
    freeReplyObject(reply);

    disconnect(c, 0);
}

static void test_blocking_io_errors(struct config config) {
    valkeyContext *c;
    valkeyReply *reply;
    void *_reply;
    int major, minor;

    /* Connect to target given by config. */
    c = do_connect(config);
    get_server_version(c, &major, &minor);

    test("Returns I/O error when the connection is lost: ");
    reply = valkeyCommand(c, "QUIT");
    if (major > 2 || (major == 2 && minor > 0)) {
        /* > 2.0 returns OK on QUIT and read() should be issued once more
         * to know the descriptor is at EOF. */
        test_cond(strcasecmp(reply->str, "OK") == 0 &&
                  valkeyGetReply(c, &_reply) == VALKEY_ERR);
        freeReplyObject(reply);
    } else {
        test_cond(reply == NULL);
    }

#ifndef _WIN32
    /* On 2.0, QUIT will cause the connection to be closed immediately and
     * the read(2) for the reply on QUIT will set the error to EOF.
     * On >2.0, QUIT will return with OK and another read(2) needed to be
     * issued to find out the socket was closed by the server. In both
     * conditions, the error will be set to EOF. */
    assert(c->err == VALKEY_ERR_EOF &&
           strcmp(c->errstr, "Server closed the connection") == 0);
#endif
    valkeyFree(c);

    c = do_connect(config);
    test("Returns I/O error on socket timeout: ");
    struct timeval tv = {0, 1000};
    assert(valkeySetTimeout(c, tv) == VALKEY_OK);
    int respcode = valkeyGetReply(c, &_reply);
#ifndef _WIN32
    test_cond(respcode == VALKEY_ERR && c->err == VALKEY_ERR_IO && errno == EAGAIN);
#else
    test_cond(respcode == VALKEY_ERR && c->err == VALKEY_ERR_TIMEOUT);
#endif
    valkeyFree(c);
}

static void test_invalid_timeout_errors(struct config config) {
    valkeyContext *c = NULL;

    test("Set error when an invalid timeout usec value is used during connect: ");

    config.connect_timeout.tv_sec = 0;
    config.connect_timeout.tv_usec = 10000001;

    if (config.type == CONN_TCP || config.type == CONN_TLS) {
        c = valkeyConnectWithTimeout(config.tcp.host, config.tcp.port, config.connect_timeout);
    } else if (config.type == CONN_MPTCP) {
        valkeyOptions options = {0};
        VALKEY_OPTIONS_SET_MPTCP(&options, config.tcp.host, config.tcp.port);
        options.connect_timeout = &config.connect_timeout;
        c = valkeyConnectWithOptions(&options);
    } else if (config.type == CONN_UNIX) {
        c = valkeyConnectUnixWithTimeout(config.unix_sock.path, config.connect_timeout);
#ifdef VALKEY_TEST_RDMA
    } else if (config.type == CONN_RDMA) {
        valkeyOptions options = {0};
        VALKEY_OPTIONS_SET_RDMA(&options, config.tcp.host, config.tcp.port);
        options.connect_timeout = &config.connect_timeout;
        c = valkeyConnectWithOptions(&options);
#endif
    } else {
        valkeyTestPanic("Unknown connection type!");
    }

    test_cond(c != NULL && c->err == VALKEY_ERR_IO && strcmp(c->errstr, "Invalid timeout specified") == 0);
    valkeyFree(c);

    test("Set error when an invalid timeout sec value is used during connect: ");

    config.connect_timeout.tv_sec = (((LONG_MAX)-999) / 1000) + 1;
    config.connect_timeout.tv_usec = 0;

    if (config.type == CONN_TCP || config.type == CONN_TLS) {
        c = valkeyConnectWithTimeout(config.tcp.host, config.tcp.port, config.connect_timeout);
    } else if (config.type == CONN_MPTCP) {
        valkeyOptions options = {0};
        VALKEY_OPTIONS_SET_MPTCP(&options, config.tcp.host, config.tcp.port);
        options.connect_timeout = &config.connect_timeout;
        c = valkeyConnectWithOptions(&options);
    } else if (config.type == CONN_UNIX) {
        c = valkeyConnectUnixWithTimeout(config.unix_sock.path, config.connect_timeout);
#ifdef VALKEY_TEST_RDMA
    } else if (config.type == CONN_RDMA) {
        valkeyOptions options = {0};
        VALKEY_OPTIONS_SET_RDMA(&options, config.tcp.host, config.tcp.port);
        options.connect_timeout = &config.connect_timeout;
        c = valkeyConnectWithOptions(&options);
#endif
    } else {
        valkeyTestPanic("Unknown connection type!");
    }

    test_cond(c != NULL && c->err == VALKEY_ERR_IO && strcmp(c->errstr, "Invalid timeout specified") == 0);
    valkeyFree(c);
}

static void test_throughput(struct config config) {
    valkeyContext *c = do_connect(config);
    valkeyReply **replies;
    int i, num;
    long long t1, t2;

    test("Throughput:\n");
    for (i = 0; i < 500; i++)
        freeReplyObject(valkeyCommand(c, "LPUSH mylist foo"));

    num = 1000;
    replies = vk_malloc_safe(sizeof(valkeyReply *) * num);
    t1 = usec();
    for (i = 0; i < num; i++) {
        replies[i] = valkeyCommand(c, "PING");
        assert(replies[i] != NULL && replies[i]->type == VALKEY_REPLY_STATUS);
    }
    t2 = usec();
    for (i = 0; i < num; i++)
        freeReplyObject(replies[i]);
    vk_free(replies);
    printf("\t(%dx PING: %.3fs)\n", num, (t2 - t1) / 1000000.0);

    replies = vk_malloc_safe(sizeof(valkeyReply *) * num);
    t1 = usec();
    for (i = 0; i < num; i++) {
        replies[i] = valkeyCommand(c, "LRANGE mylist 0 499");
        assert(replies[i] != NULL && replies[i]->type == VALKEY_REPLY_ARRAY);
        assert(replies[i] != NULL && replies[i]->elements == 500);
    }
    t2 = usec();
    for (i = 0; i < num; i++)
        freeReplyObject(replies[i]);
    vk_free(replies);
    printf("\t(%dx LRANGE with 500 elements: %.3fs)\n", num, (t2 - t1) / 1000000.0);

    replies = vk_malloc_safe(sizeof(valkeyReply *) * num);
    t1 = usec();
    for (i = 0; i < num; i++) {
        replies[i] = valkeyCommand(c, "INCRBY incrkey %d", 1000000);
        assert(replies[i] != NULL && replies[i]->type == VALKEY_REPLY_INTEGER);
    }
    t2 = usec();
    for (i = 0; i < num; i++)
        freeReplyObject(replies[i]);
    vk_free(replies);
    printf("\t(%dx INCRBY: %.3fs)\n", num, (t2 - t1) / 1000000.0);

    num = 10000;
    replies = vk_malloc_safe(sizeof(valkeyReply *) * num);
    for (i = 0; i < num; i++)
        valkeyAppendCommand(c, "PING");
    t1 = usec();
    for (i = 0; i < num; i++) {
        assert(valkeyGetReply(c, (void *)&replies[i]) == VALKEY_OK);
        assert(replies[i] != NULL && replies[i]->type == VALKEY_REPLY_STATUS);
    }
    t2 = usec();
    for (i = 0; i < num; i++)
        freeReplyObject(replies[i]);
    vk_free(replies);
    printf("\t(%dx PING (pipelined): %.3fs)\n", num, (t2 - t1) / 1000000.0);

    replies = vk_malloc_safe(sizeof(valkeyReply *) * num);
    for (i = 0; i < num; i++)
        valkeyAppendCommand(c, "LRANGE mylist 0 499");
    t1 = usec();
    for (i = 0; i < num; i++) {
        assert(valkeyGetReply(c, (void *)&replies[i]) == VALKEY_OK);
        assert(replies[i] != NULL && replies[i]->type == VALKEY_REPLY_ARRAY);
        assert(replies[i] != NULL && replies[i]->elements == 500);
    }
    t2 = usec();
    for (i = 0; i < num; i++)
        freeReplyObject(replies[i]);
    vk_free(replies);
    printf("\t(%dx LRANGE with 500 elements (pipelined): %.3fs)\n", num, (t2 - t1) / 1000000.0);

    replies = vk_malloc_safe(sizeof(valkeyReply *) * num);
    for (i = 0; i < num; i++)
        valkeyAppendCommand(c, "INCRBY incrkey %d", 1000000);
    t1 = usec();
    for (i = 0; i < num; i++) {
        assert(valkeyGetReply(c, (void *)&replies[i]) == VALKEY_OK);
        assert(replies[i] != NULL && replies[i]->type == VALKEY_REPLY_INTEGER);
    }
    t2 = usec();
    for (i = 0; i < num; i++)
        freeReplyObject(replies[i]);
    vk_free(replies);
    printf("\t(%dx INCRBY (pipelined): %.3fs)\n", num, (t2 - t1) / 1000000.0);

    disconnect(c, 0);
}

#ifdef VALKEY_TEST_ASYNC

#pragma GCC diagnostic ignored "-Woverlength-strings" /* required on gcc 4.8.x due to assert statements */

struct event_base *base;

typedef struct TestState {
    valkeyOptions *options;
    int checkpoint;
    int resp3;
    int disconnect;
} TestState;

/* Helper to disconnect and stop event loop */
void async_disconnect(valkeyAsyncContext *ac) {
    valkeyAsyncDisconnect(ac);
    event_base_loopbreak(base);
}

/* Testcase timeout, will trigger a failure */
void timeout_cb(evutil_socket_t fd, short event, void *arg) {
    (void)fd;
    (void)event;
    (void)arg;
    printf("Timeout in async testing!\n");
    exit(1);
}

/* Unexpected call, will trigger a failure */
void unexpected_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    (void)ac;
    (void)r;
    printf("Unexpected call: %s\n", (char *)privdata);
    exit(1);
}

/* Helper function to publish a message via own client. */
void publish_msg(valkeyOptions *options, const char *channel, const char *msg) {
    valkeyContext *c = valkeyConnectWithOptions(options);
    assert(c != NULL);
    valkeyReply *reply = valkeyCommand(c, "PUBLISH %s %s", channel, msg);
    assert(reply->type == VALKEY_REPLY_INTEGER && reply->integer == 1);
    freeReplyObject(reply);
    disconnect(c, 0);
}

/* Helper function to publish a message via own client. */
void spublish_msg(valkeyOptions *options, const char *channel, const char *msg) {
    valkeyContext *c = valkeyConnectWithOptions(options);
    assert(c != NULL);
    valkeyReply *reply = valkeyCommand(c, "SPUBLISH %s %s", channel, msg);
    assert(reply->type == VALKEY_REPLY_INTEGER && reply->integer == 1);
    freeReplyObject(reply);
    disconnect(c, 0);
}

/* Expect a reply of type INTEGER */
void integer_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    TestState *state = privdata;
    assert(reply != NULL && reply->type == VALKEY_REPLY_INTEGER);
    state->checkpoint++;
    if (state->disconnect)
        async_disconnect(ac);
}

/* Subscribe callback for test_pubsub_handling and test_pubsub_handling_resp3:
 * - a published message triggers an unsubscribe
 * - a command is sent before the unsubscribe response is received. */
void subscribe_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    TestState *state = privdata;

    assert(reply != NULL &&
           reply->type == (state->resp3 ? VALKEY_REPLY_PUSH : VALKEY_REPLY_ARRAY) &&
           reply->elements == 3);

    if (strcmp(reply->element[0]->str, "subscribe") == 0) {
        assert(strcmp(reply->element[1]->str, "mychannel") == 0 &&
               reply->element[2]->str == NULL);
        publish_msg(state->options, "mychannel", "Hello!");
    } else if (strcmp(reply->element[0]->str, "message") == 0) {
        assert(strcmp(reply->element[1]->str, "mychannel") == 0 &&
               strcmp(reply->element[2]->str, "Hello!") == 0);
        state->checkpoint++;

        /* Unsubscribe after receiving the published message. Send unsubscribe
         * which should call the callback registered during subscribe */
        valkeyAsyncCommand(ac, unexpected_cb,
                           (void *)"unsubscribe should call subscribe_cb()",
                           "unsubscribe");
        /* Send a regular command after unsubscribing, then disconnect */
        state->disconnect = 1;
        valkeyAsyncCommand(ac, integer_cb, state, "LPUSH mylist foo");

    } else if (strcmp(reply->element[0]->str, "unsubscribe") == 0) {
        assert(strcmp(reply->element[1]->str, "mychannel") == 0 &&
               reply->element[2]->str == NULL);
    } else {
        printf("Unexpected pubsub command: %s\n", reply->element[0]->str);
        exit(1);
    }
}

/* Expect a reply of type STATUS */
void status_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    TestState *state = privdata;
    assert(reply != NULL && reply->type == VALKEY_REPLY_STATUS);
    state->checkpoint++;
    if (state->disconnect)
        async_disconnect(ac);
}

void ssubscribe_moved_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    (void)ac;
    TestState *state = privdata;
    assert(reply != NULL);
    assert(reply->type == VALKEY_REPLY_ERROR);
    assert(strncmp(reply->str, "MOVED", 5) == 0);

    state->checkpoint++;
    state->disconnect = 1;
}

void ssubscribe_crossslot_error_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    (void)ac;
    TestState *state = privdata;

    assert(reply != NULL);
    assert(reply->type == VALKEY_REPLY_ERROR);
    assert(strncmp(reply->str, "CROSSSLOT", 9) == 0);
    state->checkpoint++;
    spublish_msg(state->options, "aaaa", "Hello!");
}

/* Subscribe callback for test_sharded_pubsub_crossslot_handling:
 * - a published message triggers another ssubscribe to first channel and other slot channel
 * - after receiving CROSSLOT error send smessage to first channel
 * - a command is sent before the unsubscribe response is received. */
void ssubscribe_crossslot_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    TestState *state = privdata;

    assert(reply != NULL &&
           reply->type == (state->resp3 ? VALKEY_REPLY_PUSH : VALKEY_REPLY_ARRAY) &&
           reply->elements == 3);

    if (strcmp(reply->element[0]->str, "ssubscribe") == 0) {
        assert(strcmp(reply->element[1]->str, "aaaa") == 0 &&
               reply->element[2]->str == NULL &&
               reply->element[2]->type == VALKEY_REPLY_INTEGER &&
               reply->element[2]->integer == 1);
        valkeyAsyncCommand(ac, ssubscribe_crossslot_error_cb, state, "ssubscribe aaa aaaa");
    } else if (strcmp(reply->element[0]->str, "smessage") == 0) {
        assert(strcmp(reply->element[1]->str, "aaaa") == 0 &&
               strcmp(reply->element[2]->str, "Hello!") == 0);
        state->checkpoint++;

        /* Unsubscribe after receiving the published message. Send unsubscribe
         * which should call the callback registered during subscribe */
        valkeyAsyncCommand(ac, unexpected_cb,
                           (void *)"unsubscribe should call ssubscribe_cb()",
                           "sunsubscribe");
        /* Send a regular command after unsubscribing, then disconnect */
        state->disconnect = 1;
        valkeyAsyncCommand(ac, integer_cb, state, "LPUSH mylist foo");
    } else if (strcmp(reply->element[0]->str, "sunsubscribe") == 0) {
        assert(strcmp(reply->element[1]->str, "aaaa") == 0 &&
               reply->element[2]->str == NULL);
    } else {
        printf("Unexpected pubsub command: %s\n", reply->element[0]->str);
        exit(1);
    }
}

/* Subscribe callback for test_sharded_pubsub_handling:
 * - a published message triggers an unsubscribe
 * - a command is sent before the unsubscribe response is received. */
void ssubscribe_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    TestState *state = privdata;

    assert(reply != NULL &&
           reply->type == (state->resp3 ? VALKEY_REPLY_PUSH : VALKEY_REPLY_ARRAY) &&
           reply->elements == 3);

    if (strcmp(reply->element[0]->str, "ssubscribe") == 0) {
        assert(strcmp(reply->element[1]->str, "aaaa") == 0 &&
               reply->element[2]->str == NULL &&
               reply->element[2]->type == VALKEY_REPLY_INTEGER &&
               reply->element[2]->integer == 1);
        spublish_msg(state->options, "aaaa", "Hello!");
    } else if (strcmp(reply->element[0]->str, "smessage") == 0) {
        assert(strcmp(reply->element[1]->str, "aaaa") == 0 &&
               strcmp(reply->element[2]->str, "Hello!") == 0);
        state->checkpoint++;

        /* Unsubscribe after receiving the published message. Send unsubscribe
         * which should call the callback registered during subscribe */
        valkeyAsyncCommand(ac, unexpected_cb,
                           (void *)"unsubscribe should call ssubscribe_cb()",
                           "sunsubscribe");
        /* Send a regular command after unsubscribing, then disconnect */
        state->disconnect = 1;
        valkeyAsyncCommand(ac, integer_cb, state, "LPUSH mylist{aaaa} foo");

    } else if (strcmp(reply->element[0]->str, "sunsubscribe") == 0) {
        assert(strcmp(reply->element[1]->str, "aaaa") == 0 &&
               reply->element[2]->str == NULL);
    } else {
        printf("Unexpected pubsub command: %s\n", reply->element[0]->str);
        exit(1);
    }
}

/* Expect a reply of type ARRAY */
void array_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    TestState *state = privdata;
    assert(reply != NULL && reply->type == VALKEY_REPLY_ARRAY);
    state->checkpoint++;
    if (state->disconnect)
        async_disconnect(ac);
}

/* Expect a NULL reply */
void null_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    (void)ac;
    assert(r == NULL);
    TestState *state = privdata;
    state->checkpoint++;
}

/* Test the command parsing, required for pub/sub in the async API. */
void test_async_command_parsing(struct config config) {
    test("Async command parsing: ");
    valkeyOptions options = get_server_tcp_options(config);
    valkeyAsyncContext *ac = valkeyAsyncConnectWithOptions(&options);
    assert(ac);

    /* Null ptr. */
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, NULL, 45));
    /* Empty command. */
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "", 0));
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, " $", 2));
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*0\r\n", 4));
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*1\r\n$-1\r\n", 9));
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*1\r\n$-1\r\nUNSUBSCRIBE\r\n", 22));
    /* Protocol error: erroneous bulkstring length and data. */
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*1\r\n$100000", 11));
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*1\r\n$100000\r", 12));
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*1\r\n$100000\r\n", 13));
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*1\r\n$10HELP\r\n\r\n", 15));
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*1\r\n$100000\r\nTO-SHORT\r\n", 23));
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*1\r\n$1\r\nTO-LONG\r\n", 17));
    assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*1\r\n$123456789\r\n", 11));

    /* Faulty length given to function. */
    for (int i = 0; i < 19; i++) {
        assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*2\r\n$9\r\nSUBSCRIBE\r\n$7\r\nCHANNEL\r\n", i));
    }
    for (int i = 0; i < 21; i++) {
        assert(VALKEY_ERR == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*1\r\n$11\r\nUNSUBSCRIBE\r\n", i));
    }

    /* Complete command. */
    assert(VALKEY_OK == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*2\r\n$9\r\nSUBSCRIBE\r\n$7\r\nCHANNEL\r\n", 32));
    assert(VALKEY_OK == valkeyAsyncFormattedCommand(ac, NULL, NULL, "*1\r\n$11\r\nUNSUBSCRIBE\r\n", 22));

    // Heap allocate command without NULL terminator.
    const char ping[] = "*1\r\n$4\r\nPING\r\n";
    size_t len = sizeof(ping) - 1;
    char *buf = vk_malloc_safe(len);
    memcpy(buf, ping, len);
    assert(VALKEY_OK == valkeyAsyncFormattedCommand(ac, NULL, NULL, buf, len));
    free(buf);

    valkeyAsyncFree(ac);
}

static void test_pubsub_handling(struct config config) {
    test("Subscribe, handle published message and unsubscribe: ");
    /* Setup event dispatcher with a testcase timeout */
    base = event_base_new();
    struct event *timeout = evtimer_new(base, timeout_cb, NULL);
    assert(timeout != NULL);

    evtimer_assign(timeout, base, timeout_cb, NULL);
    struct timeval timeout_tv = {.tv_sec = 10};
    evtimer_add(timeout, &timeout_tv);

    /* Connect */
    valkeyOptions options = get_server_tcp_options(config);
    valkeyAsyncContext *ac = valkeyAsyncConnectWithOptions(&options);
    assert(ac != NULL && ac->err == 0);
    valkeyLibeventAttach(ac, base);

    /* Start subscribe */
    TestState state = {.options = &options};
    valkeyAsyncCommand(ac, subscribe_cb, &state, "subscribe mychannel");

    /* Make sure non-subscribe commands are handled */
    valkeyAsyncCommand(ac, array_cb, &state, "PING");

    /* Start event dispatching loop */
    test_cond(event_base_dispatch(base) == 0);
    event_free(timeout);
    event_base_free(base);

    /* Verify test checkpoints */
    assert(state.checkpoint == 3);
}

static void test_sharded_pubsub_handling(struct config config) {
    test("Subscribe, handle published sharded message and unsubscribe: ");
    /* Setup event dispatcher with a testcase timeout */
    base = event_base_new();
    struct event *timeout = evtimer_new(base, timeout_cb, NULL);
    assert(timeout != NULL);

    evtimer_assign(timeout, base, timeout_cb, NULL);
    struct timeval timeout_tv = {.tv_sec = 10};
    evtimer_add(timeout, &timeout_tv);

    /* Connect */
    valkeyOptions options = get_server_tcp_cluster_options(config);
    valkeyAsyncContext *ac = valkeyAsyncConnectWithOptions(&options);
    assert(ac != NULL && ac->err == 0);
    valkeyLibeventAttach(ac, base);

    /* Start subscribe */
    TestState state = {.options = &options};
    valkeyAsyncCommand(ac, ssubscribe_cb, &state, "ssubscribe aaaa");

    /* Make sure non-subscribe commands are handled */
    valkeyAsyncCommand(ac, array_cb, &state, "PING");

    /* Start event dispatching loop */
    test_cond(event_base_dispatch(base) == 0);
    event_free(timeout);
    event_base_free(base);

    /* Verify test checkpoints */
    assert(state.checkpoint == 3);
}

static void test_sharded_pubsub_error_handling(struct config config) {
    test("Subscribe, handle MOVED error: ");
    /* Setup event dispatcher with a testcase timeout */
    base = event_base_new();
    struct event *timeout = evtimer_new(base, timeout_cb, NULL);
    assert(timeout != NULL);

    evtimer_assign(timeout, base, timeout_cb, NULL);
    struct timeval timeout_tv = {.tv_sec = 10};
    evtimer_add(timeout, &timeout_tv);

    /* Connect */
    valkeyOptions options = get_server_tcp_cluster_options(config);
    valkeyAsyncContext *ac = valkeyAsyncConnectWithOptions(&options);
    assert(ac != NULL && ac->err == 0);
    valkeyLibeventAttach(ac, base);

    /* Start subscribe */
    TestState state = {.options = &options};
    valkeyAsyncCommand(ac, ssubscribe_moved_cb, &state, "ssubscribe aaa");
    valkeyAsyncCommand(ac, status_cb, &state, "PING");

    /* Start event dispatching loop */
    test_cond(event_base_dispatch(base) == 0);
    event_free(timeout);
    event_base_free(base);

    /* Verify test checkpoints */
    assert(state.checkpoint == 2);
}

static void test_sharded_pubsub_crossslot_handling(struct config config) {
    test("Subscribe, handle CROSSSLOT error: ");
    /*
     *  Here we are subscribing to first channel (ssubscribe aaaa). Then we are trying to subscribe to two channel: aaaa and aaa. It should trigger a CROSSSLOT error.
     *  This Error should be processed in user provided callback. Subscription to initial channel should be preserved.
     *  We check this by sending message to "aaaa" channel.
     */
    /* Setup event dispatcher with a testcase timeout */
    base = event_base_new();
    struct event *timeout = evtimer_new(base, timeout_cb, NULL);
    assert(timeout != NULL);

    evtimer_assign(timeout, base, timeout_cb, NULL);
    struct timeval timeout_tv = {.tv_sec = 10};
    evtimer_add(timeout, &timeout_tv);

    /* Connect */
    valkeyOptions options = get_server_tcp_cluster_options(config);
    valkeyAsyncContext *ac = valkeyAsyncConnectWithOptions(&options);
    assert(ac != NULL && ac->err == 0);
    valkeyLibeventAttach(ac, base);

    /* Start subscribe */
    TestState state = {.options = &options};
    valkeyAsyncCommand(ac, ssubscribe_crossslot_cb, &state, "ssubscribe aaaa");
    valkeyAsyncCommand(ac, array_cb, &state, "PING");

    /* Start event dispatching loop */
    test_cond(event_base_dispatch(base) == 0);
    event_free(timeout);
    event_base_free(base);

    /* Verify test checkpoints */
    assert(state.checkpoint == 4);
}

/* Unexpected push message, will trigger a failure */
void unexpected_push_cb(valkeyAsyncContext *ac, void *r) {
    (void)ac;
    (void)r;
    printf("Unexpected call to the PUSH callback!\n");
    exit(1);
}

static void test_pubsub_handling_resp3(struct config config) {
    test("Subscribe, handle published message and unsubscribe using RESP3: ");
    /* Setup event dispatcher with a testcase timeout */
    base = event_base_new();
    struct event *timeout = evtimer_new(base, timeout_cb, NULL);
    assert(timeout != NULL);

    evtimer_assign(timeout, base, timeout_cb, NULL);
    struct timeval timeout_tv = {.tv_sec = 10};
    evtimer_add(timeout, &timeout_tv);

    /* Connect */
    valkeyOptions options = get_server_tcp_options(config);
    valkeyAsyncContext *ac = valkeyAsyncConnectWithOptions(&options);
    assert(ac != NULL && ac->err == 0);
    valkeyLibeventAttach(ac, base);

    /* Not expecting any push messages in this test */
    valkeyAsyncSetPushCallback(ac, unexpected_push_cb);

    /* Switch protocol */
    valkeyAsyncCommand(ac, NULL, NULL, "HELLO 3");

    /* Start subscribe */
    TestState state = {.options = &options, .resp3 = 1};
    valkeyAsyncCommand(ac, subscribe_cb, &state, "subscribe mychannel");

    /* Make sure non-subscribe commands are handled in RESP3 */
    valkeyAsyncCommand(ac, integer_cb, &state, "LPUSH mylist foo");
    valkeyAsyncCommand(ac, integer_cb, &state, "LPUSH mylist foo");
    valkeyAsyncCommand(ac, integer_cb, &state, "LPUSH mylist foo");
    /* Handle an array with 3 elements as a non-subscribe command */
    valkeyAsyncCommand(ac, array_cb, &state, "LRANGE mylist 0 2");

    /* Start event dispatching loop */
    test_cond(event_base_dispatch(base) == 0);
    event_free(timeout);
    event_base_free(base);

    /* Verify test checkpoints */
    assert(state.checkpoint == 6);
}

static void test_sharded_pubsub_handling_resp3(struct config config) {
    test("Sharded subscribe, handle published message and unsubscribe using RESP3: ");
    /* Setup event dispatcher with a testcase timeout */
    base = event_base_new();
    struct event *timeout = evtimer_new(base, timeout_cb, NULL);
    assert(timeout != NULL);

    evtimer_assign(timeout, base, timeout_cb, NULL);
    struct timeval timeout_tv = {.tv_sec = 10};
    evtimer_add(timeout, &timeout_tv);

    /* Connect */
    valkeyOptions options = get_server_tcp_cluster_options(config);
    valkeyAsyncContext *ac = valkeyAsyncConnectWithOptions(&options);
    assert(ac != NULL && ac->err == 0);
    valkeyLibeventAttach(ac, base);

    /* Not expecting any push messages in this test */
    valkeyAsyncSetPushCallback(ac, unexpected_push_cb);

    /* Switch protocol */
    valkeyAsyncCommand(ac, NULL, NULL, "HELLO 3");

    /* Start subscribe */
    TestState state = {.options = &options, .resp3 = 1};
    valkeyAsyncCommand(ac, ssubscribe_cb, &state, "ssubscribe aaaa");

    /* Make sure non-subscribe commands are handled in RESP3 */
    valkeyAsyncCommand(ac, integer_cb, &state, "LPUSH mylist{aaaa} foo");
    valkeyAsyncCommand(ac, integer_cb, &state, "LPUSH mylist{aaaa} foo");
    valkeyAsyncCommand(ac, integer_cb, &state, "LPUSH mylist{aaaa} foo");
    /* Handle an array with 3 elements as a non-subscribe command */
    valkeyAsyncCommand(ac, array_cb, &state, "LRANGE mylist{aaaa} 0 2");

    /* Start event dispatching loop */
    test_cond(event_base_dispatch(base) == 0);
    event_free(timeout);
    event_base_free(base);

    /* Verify test checkpoints */
    assert(state.checkpoint == 6);
}

/* Subscribe callback for test_command_timeout_during_pubsub:
 * - a subscribe response triggers a published message
 * - the published message triggers a command that times out
 * - the command timeout triggers a disconnect */
void subscribe_with_timeout_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    TestState *state = privdata;

    /* The non-clean disconnect should trigger the
     * subscription callback with a NULL reply. */
    if (reply == NULL) {
        state->checkpoint++;
        event_base_loopbreak(base);
        return;
    }

    assert(reply->type == (state->resp3 ? VALKEY_REPLY_PUSH : VALKEY_REPLY_ARRAY) &&
           reply->elements == 3);

    if (strcmp(reply->element[0]->str, "subscribe") == 0) {
        assert(strcmp(reply->element[1]->str, "mychannel") == 0 &&
               reply->element[2]->str == NULL);
        publish_msg(state->options, "mychannel", "Hello!");
        state->checkpoint++;
    } else if (strcmp(reply->element[0]->str, "message") == 0) {
        assert(strcmp(reply->element[1]->str, "mychannel") == 0 &&
               strcmp(reply->element[2]->str, "Hello!") == 0);
        state->checkpoint++;

        /* Send a command that will trigger a timeout */
        valkeyAsyncCommand(ac, null_cb, state, "DEBUG SLEEP 3");
        valkeyAsyncCommand(ac, null_cb, state, "LPUSH mylist foo");
    } else {
        printf("Unexpected pubsub command: %s\n", reply->element[0]->str);
        exit(1);
    }
}

static void test_command_timeout_during_pubsub(struct config config) {
    test("Command timeout during Pub/Sub: ");
    /* Setup event dispatcher with a testcase timeout */
    base = event_base_new();
    struct event *timeout = evtimer_new(base, timeout_cb, NULL);
    assert(timeout != NULL);

    evtimer_assign(timeout, base, timeout_cb, NULL);
    struct timeval timeout_tv = {.tv_sec = 10};
    evtimer_add(timeout, &timeout_tv);

    /* Connect */
    valkeyOptions options = get_server_tcp_options(config);
    valkeyAsyncContext *ac = valkeyAsyncConnectWithOptions(&options);
    assert(ac != NULL && ac->err == 0);
    valkeyLibeventAttach(ac, base);

    /* Configure a command timeout */
    struct timeval command_timeout = {.tv_sec = 2};
    valkeyAsyncSetTimeout(ac, command_timeout);

    /* Not expecting any push messages in this test */
    valkeyAsyncSetPushCallback(ac, unexpected_push_cb);

    /* Switch protocol */
    valkeyAsyncCommand(ac, NULL, NULL, "HELLO 3");

    /* Start subscribe */
    TestState state = {.options = &options, .resp3 = 1};
    valkeyAsyncCommand(ac, subscribe_with_timeout_cb, &state, "subscribe mychannel");

    /* Start event dispatching loop */
    assert(event_base_dispatch(base) == 0);
    event_free(timeout);
    event_base_free(base);

    /* Verify test checkpoints */
    test_cond(state.checkpoint == 5);
}

/* Subscribe callback for test_pubsub_multiple_channels */
void subscribe_channel_a_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    TestState *state = privdata;

    assert(reply != NULL && reply->type == VALKEY_REPLY_ARRAY &&
           reply->elements == 3);

    if (strcmp(reply->element[0]->str, "subscribe") == 0) {
        assert(strcmp(reply->element[1]->str, "A") == 0);
        publish_msg(state->options, "A", "Hello!");
        state->checkpoint++;
    } else if (strcmp(reply->element[0]->str, "message") == 0) {
        assert(strcmp(reply->element[1]->str, "A") == 0 &&
               strcmp(reply->element[2]->str, "Hello!") == 0);
        state->checkpoint++;

        /* Unsubscribe to patterns, none which we subscribe to */
        valkeyAsyncCommand(ac, unexpected_cb,
                           (void *)"punsubscribe should not call unexpected_cb()",
                           "punsubscribe");
        /* Unsubscribe to channels, including channel X & Z which we don't subscribe to */
        valkeyAsyncCommand(ac, unexpected_cb,
                           (void *)"unsubscribe should not call unexpected_cb()",
                           "unsubscribe B X A A Z");
        /* Send a regular command after unsubscribing, then disconnect */
        state->disconnect = 1;
        valkeyAsyncCommand(ac, integer_cb, state, "LPUSH mylist foo");
    } else if (strcmp(reply->element[0]->str, "unsubscribe") == 0) {
        assert(strcmp(reply->element[1]->str, "A") == 0);
        state->checkpoint++;
    } else {
        printf("Unexpected pubsub command: %s\n", reply->element[0]->str);
        exit(1);
    }
}

/* Subscribe callback for test_pubsub_multiple_channels */
void subscribe_channel_b_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    TestState *state = privdata;
    (void)ac;

    assert(reply != NULL && reply->type == VALKEY_REPLY_ARRAY &&
           reply->elements == 3);

    if (strcmp(reply->element[0]->str, "subscribe") == 0) {
        assert(strcmp(reply->element[1]->str, "B") == 0);
        state->checkpoint++;
    } else if (strcmp(reply->element[0]->str, "unsubscribe") == 0) {
        assert(strcmp(reply->element[1]->str, "B") == 0);
        state->checkpoint++;
    } else {
        printf("Unexpected pubsub command: %s\n", reply->element[0]->str);
        exit(1);
    }
}

/* Test handling of multiple channels
 * - subscribe to channel A and B
 * - a published message on A triggers an unsubscribe of channel B, X, A and Z
 *   where channel X and Z are not subscribed to.
 * - the published message also triggers an unsubscribe to patterns. Since no
 *   pattern is subscribed to the responded pattern element type is NIL.
 * - a command sent after unsubscribe triggers a disconnect */
static void test_pubsub_multiple_channels(struct config config) {
    test("Subscribe to multiple channels: ");
    /* Setup event dispatcher with a testcase timeout */
    base = event_base_new();
    struct event *timeout = evtimer_new(base, timeout_cb, NULL);
    assert(timeout != NULL);

    evtimer_assign(timeout, base, timeout_cb, NULL);
    struct timeval timeout_tv = {.tv_sec = 10};
    evtimer_add(timeout, &timeout_tv);

    /* Connect */
    valkeyOptions options = get_server_tcp_options(config);
    valkeyAsyncContext *ac = valkeyAsyncConnectWithOptions(&options);
    assert(ac != NULL && ac->err == 0);
    valkeyLibeventAttach(ac, base);

    /* Not expecting any push messages in this test */
    valkeyAsyncSetPushCallback(ac, unexpected_push_cb);

    /* Start subscribing to two channels */
    TestState state = {.options = &options};
    valkeyAsyncCommand(ac, subscribe_channel_a_cb, &state, "subscribe A");
    valkeyAsyncCommand(ac, subscribe_channel_b_cb, &state, "subscribe B");

    /* Start event dispatching loop */
    assert(event_base_dispatch(base) == 0);
    event_free(timeout);
    event_base_free(base);

    /* Verify test checkpoints */
    test_cond(state.checkpoint == 6);
}

/* Command callback for test_monitor() */
void monitor_cb(valkeyAsyncContext *ac, void *r, void *privdata) {
    valkeyReply *reply = r;
    TestState *state = privdata;

    /* NULL reply is received when BYE triggers a disconnect. */
    if (reply == NULL) {
        event_base_loopbreak(base);
        return;
    }

    assert(reply != NULL && reply->type == VALKEY_REPLY_STATUS);
    state->checkpoint++;

    if (state->checkpoint == 1) {
        /* Response from MONITOR */
        valkeyContext *c = valkeyConnectWithOptions(state->options);
        assert(c != NULL);
        valkeyReply *reply = valkeyCommand(c, "SET first 1");
        assert(reply->type == VALKEY_REPLY_STATUS);
        freeReplyObject(reply);
        valkeyFree(c);
    } else if (state->checkpoint == 2) {
        /* Response for monitored command 'SET first 1' */
        assert(strstr(reply->str, "first") != NULL);
        valkeyContext *c = valkeyConnectWithOptions(state->options);
        assert(c != NULL);
        valkeyReply *reply = valkeyCommand(c, "SET second 2");
        assert(reply->type == VALKEY_REPLY_STATUS);
        freeReplyObject(reply);
        valkeyFree(c);
    } else if (state->checkpoint == 3) {
        /* Response for monitored command 'SET second 2' */
        assert(strstr(reply->str, "second") != NULL);
        /* Send QUIT to disconnect */
        valkeyAsyncCommand(ac, NULL, NULL, "QUIT");
    }
}

/* Test handling of the monitor command
 * - sends MONITOR to enable monitoring.
 * - sends SET commands via separate clients to be monitored.
 * - sends QUIT to stop monitoring and disconnect. */
static void test_monitor(struct config config) {
    test("Enable monitoring: ");
    /* Setup event dispatcher with a testcase timeout */
    base = event_base_new();
    struct event *timeout = evtimer_new(base, timeout_cb, NULL);
    assert(timeout != NULL);

    evtimer_assign(timeout, base, timeout_cb, NULL);
    struct timeval timeout_tv = {.tv_sec = 10};
    evtimer_add(timeout, &timeout_tv);

    /* Connect */
    valkeyOptions options = get_server_tcp_options(config);
    valkeyAsyncContext *ac = valkeyAsyncConnectWithOptions(&options);
    assert(ac != NULL && ac->err == 0);
    valkeyLibeventAttach(ac, base);

    /* Not expecting any push messages in this test */
    valkeyAsyncSetPushCallback(ac, unexpected_push_cb);

    /* Start monitor */
    TestState state = {.options = &options};
    valkeyAsyncCommand(ac, monitor_cb, &state, "monitor");

    /* Start event dispatching loop */
    test_cond(event_base_dispatch(base) == 0);
    event_free(timeout);
    event_base_free(base);

    /* Verify test checkpoints */
    assert(state.checkpoint == 3);
}
#endif /* VALKEY_TEST_ASYNC */

/* tests for async api using polling adapter, requires no extra libraries*/

/* enum for the test cases, the callbacks have different logic based on them */
typedef enum astest_no {
    ASTEST_CONNECT = 0,
    ASTEST_CONN_TIMEOUT,
    ASTEST_PINGPONG,
    ASTEST_PINGPONG_TIMEOUT,
    ASTEST_ISSUE_931,
    ASTEST_ISSUE_931_PING
} astest_no;

/* a static context for the async tests */
struct _astest {
    valkeyAsyncContext *ac;
    astest_no testno;
    int counter;
    int connects;
    int connect_status;
    int disconnects;
    int pongs;
    int disconnect_status;
    int connected;
    int err;
    char errstr[128];
};
static struct _astest astest;
vk_static_assert(sizeof(astest.errstr) == sizeof(((valkeyContext){0}).errstr));

/* async callbacks */
static void asCleanup(void *data) {
    struct _astest *t = (struct _astest *)data;
    t->ac = NULL;
}

static void commandCallback(struct valkeyAsyncContext *ac, void *_reply, void *_privdata);

static void connectCallback(valkeyAsyncContext *c, int status) {
    struct _astest *t = (struct _astest *)c->data;
    assert(t == &astest);
    assert(t->connects == 0);
    t->err = c->err;
    memcpy(t->errstr, c->errstr, sizeof(t->errstr));
    t->connects++;
    t->connect_status = status;
    t->connected = status == VALKEY_OK ? 1 : -1;

    if (t->testno == ASTEST_ISSUE_931) {
        /* disconnect again */
        valkeyAsyncDisconnect(c);
    } else if (t->testno == ASTEST_ISSUE_931_PING) {
        valkeyAsyncCommand(c, commandCallback, NULL, "PING");
    }
}
static void disconnectCallback(const valkeyAsyncContext *c, int status) {
    assert(c->data == (void *)&astest);
    assert(astest.disconnects == 0);
    astest.err = c->err;
    memcpy(astest.errstr, c->errstr, sizeof(astest.errstr));
    astest.disconnects++;
    astest.disconnect_status = status;
    astest.connected = 0;
}

static void commandCallback(struct valkeyAsyncContext *ac, void *_reply, void *_privdata) {
    valkeyReply *reply = (valkeyReply *)_reply;
    struct _astest *t = (struct _astest *)ac->data;
    assert(t == &astest);
    (void)_privdata;
    t->err = ac->err;
    memcpy(t->errstr, ac->errstr, sizeof(t->errstr));
    t->counter++;
    if (t->testno == ASTEST_PINGPONG || t->testno == ASTEST_ISSUE_931_PING) {
        assert(reply != NULL && reply->type == VALKEY_REPLY_STATUS && strcmp(reply->str, "PONG") == 0);
        t->pongs++;
        valkeyAsyncFree(ac);
    }
    if (t->testno == ASTEST_PINGPONG_TIMEOUT) {
        /* two ping pongs */
        assert(reply != NULL && reply->type == VALKEY_REPLY_STATUS && strcmp(reply->str, "PONG") == 0);
        t->pongs++;
        if (t->counter == 1) {
            int status = valkeyAsyncCommand(ac, commandCallback, NULL, "PING");
            assert(status == VALKEY_OK);
        } else {
            valkeyAsyncFree(ac);
        }
    }
}

static valkeyAsyncContext *do_aconnect(struct config config, astest_no testno) {
    valkeyOptions options = {0};
    memset(&astest, 0, sizeof(astest));

    astest.testno = testno;
    astest.connect_status = astest.disconnect_status = -2;

    if (config.type == CONN_TCP) {
        options.type = VALKEY_CONN_TCP;
        options.connect_timeout = &config.connect_timeout;
        VALKEY_OPTIONS_SET_TCP(&options, config.tcp.host, config.tcp.port);
    } else if (config.type == CONN_MPTCP) {
        options.type = VALKEY_CONN_TCP;
        options.connect_timeout = &config.connect_timeout;
        VALKEY_OPTIONS_SET_MPTCP(&options, config.tcp.host, config.tcp.port);
    } else if (config.type == CONN_TLS) {
        options.type = VALKEY_CONN_TCP;
        options.connect_timeout = &config.connect_timeout;
        VALKEY_OPTIONS_SET_TCP(&options, config.tls.host, config.tls.port);
    } else if (config.type == CONN_UNIX) {
        options.type = VALKEY_CONN_UNIX;
        options.endpoint.unix_socket = config.unix_sock.path;
    } else if (config.type == CONN_FD) {
        options.type = VALKEY_CONN_USERFD;
        /* Create a dummy connection just to get an fd to inherit */
        valkeyContext *dummy_ctx = valkeyConnectUnix(config.unix_sock.path);
        if (dummy_ctx) {
            valkeyFD fd = disconnect(dummy_ctx, 1);
            printf("Connecting to inherited fd %d\n", (int)fd);
            options.endpoint.fd = fd;
        }
    }
    valkeyAsyncContext *c = valkeyAsyncConnectWithOptions(&options);
    assert(c);
    astest.ac = c;
    c->data = &astest;
    c->dataCleanup = asCleanup;
    valkeyPollAttach(c);
    valkeyAsyncSetConnectCallback(c, connectCallback);
    valkeyAsyncSetDisconnectCallback(c, disconnectCallback);
    return c;
}

static void as_printerr(void) {
    printf("Async err %d : %s\n", astest.err, astest.errstr);
}

#define ASASSERT(e)        \
    do {                   \
        if (!(e))          \
            as_printerr(); \
        assert(e);         \
    } while (0);

static void test_async_polling(struct config config) {
    int status;
    valkeyAsyncContext *c;
    struct config defaultconfig = config;

    test("Async connect: ");
    c = do_aconnect(config, ASTEST_CONNECT);
    assert(c);
    while (astest.connected == 0)
        valkeyPollTick(c, 0.1);
    assert(astest.connects == 1);
    ASASSERT(astest.connect_status == VALKEY_OK);
    assert(astest.disconnects == 0);
    test_cond(astest.connected == 1);

    test("Async free after connect: ");
    assert(astest.ac != NULL);
    valkeyAsyncFree(c);
    assert(astest.disconnects == 1);
    assert(astest.ac == NULL);
    test_cond(astest.disconnect_status == VALKEY_OK);

    if (config.type == CONN_TCP || config.type == CONN_TLS) {
        /* timeout can only be simulated with network */
        test("Async connect timeout: ");
        config.tcp.host = "192.168.254.254"; /* blackhole ip */
        config.connect_timeout.tv_usec = 100000;
        c = do_aconnect(config, ASTEST_CONN_TIMEOUT);
        assert(c);
        assert(c->err == 0);
        while (astest.connected == 0)
            valkeyPollTick(c, 0.1);
        assert(astest.connected == -1);
        /*
         * freeing should not be done, clearing should have happened.
         *valkeyAsyncFree(c);
         */
        assert(astest.ac == NULL);
        test_cond(astest.connect_status == VALKEY_ERR);
        config = defaultconfig;
    }

    /* Test a ping/pong after connection */
    test("Async PING/PONG: ");
    c = do_aconnect(config, ASTEST_PINGPONG);
    while (astest.connected == 0)
        valkeyPollTick(c, 0.1);
    status = valkeyAsyncCommand(c, commandCallback, NULL, "PING");
    assert(status == VALKEY_OK);
    while (astest.ac)
        valkeyPollTick(c, 0.1);
    test_cond(astest.pongs == 1);

    /* Test a ping/pong after connection that didn't time out. */
    if (config.type == CONN_TCP || config.type == CONN_TLS) {
        test("Async PING/PONG after connect timeout: ");
        config.connect_timeout.tv_usec = 10000; /* 10ms  */
        c = do_aconnect(config, ASTEST_PINGPONG_TIMEOUT);
        while (astest.connected == 0)
            valkeyPollTick(c, 0.1);
        /* sleep 0.1 s, allowing old timeout to arrive */
        millisleep(10);
        status = valkeyAsyncCommand(c, commandCallback, NULL, "PING");
        assert(status == VALKEY_OK);
        while (astest.ac)
            valkeyPollTick(c, 0.1);
        test_cond(astest.pongs == 2);
        config = defaultconfig;
    }

    /* Test disconnect from an on_connect callback */
    test("Disconnect from onConnected callback (Issue #931): ");
    c = do_aconnect(config, ASTEST_ISSUE_931);
    while (astest.disconnects == 0)
        valkeyPollTick(c, 0.1);
    assert(astest.connected == 0);
    assert(astest.connects == 1);
    test_cond(astest.disconnects == 1);

    /* Test ping/pong from an on_connect callback */
    test("Ping/Pong from onConnected callback (Issue #931): ");
    c = do_aconnect(config, ASTEST_ISSUE_931_PING);
    /* connect callback issues ping, response callback destroys context */
    while (astest.ac)
        valkeyPollTick(c, 0.1);
    assert(astest.connected == 0);
    assert(astest.connects == 1);
    assert(astest.disconnects == 1);
    test_cond(astest.pongs == 1);
}
/* End of Async polling_adapter driven tests */

#ifdef VALKEY_TEST_ASYNC
static void sharded_pubsub_test(struct config cfg) {
    int major;

    cfg.type = CONN_TCP_CLUSTER;
    valkeyContext *c = do_connect(cfg);
    get_server_version(c, &major, NULL);
    disconnect(c, 0);

    if (major >= 7) {
        test_sharded_pubsub_handling(cfg);
        test_sharded_pubsub_error_handling(cfg);
        test_sharded_pubsub_crossslot_handling(cfg);
        test_sharded_pubsub_handling_resp3(cfg);
    }
}
#endif /* VALKEY_TEST_ASYNC */

int main(int argc, char **argv) {
    struct config cfg = {
        .tcp = {
            .host = "127.0.0.1",
            .port = 6379},
        .tcp_cluster = {.host = "127.0.0.1", .port = 7000},
        .unix_sock = {
            .path = "/tmp/valkey.sock",
        },
    };
    int throughput = 1;
    int test_inherit_fd = 1;
    int skips_as_fails = 0;
    int test_unix_socket;
#ifdef VALKEY_TEST_ASYNC
    int enable_cluster_tests = 0;
#endif

    /* Parse command line options. */
    argv++;
    argc--;
    while (argc) {
        if (argc >= 2 && !strcmp(argv[0], "-h")) {
            argv++;
            argc--;
            cfg.tcp.host = argv[0];
        } else if (argc >= 2 && !strcmp(argv[0], "-p")) {
            argv++;
            argc--;
            cfg.tcp.port = atoi(argv[0]);
        } else if (argc >= 2 && !strcmp(argv[0], "--cluster-host")) {
            argv++;
            argc--;
            cfg.tcp_cluster.host = argv[0];
        } else if (argc >= 2 && !strcmp(argv[0], "--cluster-port")) {
            argv++;
            argc--;
            cfg.tcp_cluster.port = atoi(argv[0]);
        } else if (argc >= 2 && !strcmp(argv[0], "-s")) {
            argv++;
            argc--;
            cfg.unix_sock.path = argv[0];
        } else if (argc >= 1 && !strcmp(argv[0], "--skip-throughput")) {
            throughput = 0;
        } else if (argc >= 1 && !strcmp(argv[0], "--skip-inherit-fd")) {
            test_inherit_fd = 0;
#ifdef VALKEY_TEST_ASYNC
        } else if (argc >= 1 && !strcmp(argv[0], "--enable-cluster-tests")) {
            enable_cluster_tests = 1;
#endif
        } else if (argc >= 1 && !strcmp(argv[0], "--skips-as-fails")) {
            skips_as_fails = 1;
#ifdef VALKEY_TEST_TLS
        } else if (argc >= 2 && !strcmp(argv[0], "--tls-port")) {
            argv++;
            argc--;
            cfg.tls.port = atoi(argv[0]);
        } else if (argc >= 2 && !strcmp(argv[0], "--tls-host")) {
            argv++;
            argc--;
            cfg.tls.host = argv[0];
        } else if (argc >= 2 && !strcmp(argv[0], "--tls-ca-cert")) {
            argv++;
            argc--;
            cfg.tls.ca_cert = argv[0];
        } else if (argc >= 2 && !strcmp(argv[0], "--tls-cert")) {
            argv++;
            argc--;
            cfg.tls.cert = argv[0];
        } else if (argc >= 2 && !strcmp(argv[0], "--tls-key")) {
            argv++;
            argc--;
            cfg.tls.key = argv[0];
#endif
#ifdef VALKEY_TEST_RDMA
        } else if (argc >= 1 && !strcmp(argv[0], "--rdma-addr")) {
            argv++;
            argc--;
            cfg.rdma.host = argv[0];
        } else if (argc >= 1 && !strcmp(argv[0], "--rdma-source-addr")) {
            argv++;
            argc--;
            cfg.rdma.source_addr = argv[0];
#endif
        } else {
            fprintf(stderr, "Invalid argument: %s\n", argv[0]);
            exit(1);
        }
        argv++;
        argc--;
    }

#ifndef _WIN32
    /* Ignore broken pipe signal (for I/O error tests). */
    signal(SIGPIPE, SIG_IGN);

    test_unix_socket = access(cfg.unix_sock.path, F_OK) == 0;

#else
    /* Unix sockets don't exist in Windows */
    test_unix_socket = 0;
#endif

    test_allocator_injection();

    test_format_commands();
    test_reply_reader();
    test_blocking_connection_errors();
    test_free_null();

    printf("\nTesting against TCP connection (%s:%d):\n", cfg.tcp.host, cfg.tcp.port);
    cfg.type = CONN_TCP;
    test_blocking_connection(cfg);
    test_blocking_connection_timeouts(cfg);
    test_blocking_io_errors(cfg);
    test_invalid_timeout_errors(cfg);
    test_append_formatted_commands(cfg);
    test_tcp_options(cfg);
    if (throughput)
        test_throughput(cfg);

#ifdef IPPROTO_MPTCP
    printf("\nTesting against MPTCP connection (%s:%d):\n", cfg.tcp.host, cfg.tcp.port);
    cfg.type = CONN_MPTCP;
    test_blocking_connection(cfg);
    test_blocking_connection_timeouts(cfg);
    test_blocking_io_errors(cfg);
    test_invalid_timeout_errors(cfg);
    test_append_formatted_commands(cfg);
    test_tcp_options(cfg);
    if (throughput)
        test_throughput(cfg);
#endif

    printf("\nTesting against Unix socket connection (%s): ", cfg.unix_sock.path);
    if (test_unix_socket) {
        printf("\n");
        cfg.type = CONN_UNIX;
        test_blocking_connection(cfg);
        test_blocking_connection_timeouts(cfg);
        test_blocking_io_errors(cfg);
        test_invalid_timeout_errors(cfg);
        test_unix_keepalive(cfg);
        if (throughput)
            test_throughput(cfg);
    } else {
        test_skipped();
    }

#ifdef VALKEY_TEST_TLS
    if (cfg.tls.port && cfg.tls.host) {

        valkeyInitOpenSSL();
        _tls_ctx = valkeyCreateTLSContext(cfg.tls.ca_cert, NULL, cfg.tls.cert, cfg.tls.key, NULL, NULL);
        assert(_tls_ctx != NULL);

        printf("\nTesting against TLS connection (%s:%d):\n", cfg.tls.host, cfg.tls.port);
        cfg.type = CONN_TLS;

        test_blocking_connection(cfg);
        test_blocking_connection_timeouts(cfg);
        test_blocking_io_errors(cfg);
        test_invalid_timeout_errors(cfg);
        test_append_formatted_commands(cfg);
        if (throughput)
            test_throughput(cfg);

        valkeyFreeTLSContext(_tls_ctx);
        _tls_ctx = NULL;
    }
#endif

#ifdef VALKEY_TEST_RDMA
    if (cfg.rdma.host) {

        valkeyInitiateRdma();
        printf("\nTesting against RDMA connection (%s:%d):\n", cfg.rdma.host, cfg.tcp.port);
        cfg.type = CONN_RDMA;

        test_blocking_connection(cfg);
        test_blocking_connection_timeouts(cfg);
        test_blocking_io_errors(cfg);
        test_invalid_timeout_errors(cfg);
        test_append_formatted_commands(cfg);
        if (throughput)
            test_throughput(cfg);
    }
#endif

#ifdef VALKEY_TEST_ASYNC
    printf("\nTesting asynchronous API against TCP connection (%s:%d):\n", cfg.tcp.host, cfg.tcp.port);
    cfg.type = CONN_TCP;

    int major;
    valkeyContext *c = do_connect(cfg);
    get_server_version(c, &major, NULL);
    disconnect(c, 0);

    test_async_command_parsing(cfg);
    test_pubsub_handling(cfg);
    test_pubsub_multiple_channels(cfg);
    test_monitor(cfg);
    if (major >= 6) {
        test_pubsub_handling_resp3(cfg);
        test_command_timeout_during_pubsub(cfg);
    }

    if (enable_cluster_tests) {
        sharded_pubsub_test(cfg);
    }

#ifdef IPPROTO_MPTCP
    printf("\nTesting asynchronous API against MPTCP connection (%s:%d):\n", cfg.tcp.host, cfg.tcp.port);
    cfg.type = CONN_MPTCP;

    c = do_connect(cfg);
    get_server_version(c, &major, NULL);
    disconnect(c, 0);

    test_pubsub_handling(cfg);
    test_pubsub_multiple_channels(cfg);
    test_monitor(cfg);
    if (major >= 6) {
        test_pubsub_handling_resp3(cfg);
        test_command_timeout_during_pubsub(cfg);
    }
#endif /* IPPROTO_MPTCP */

#endif /* VALKEY_TEST_ASYNC */

    cfg.type = CONN_TCP;
    printf("\nTesting asynchronous API using polling_adapter TCP (%s:%d):\n", cfg.tcp.host, cfg.tcp.port);
    test_async_polling(cfg);
#ifdef IPPROTO_MPTCP
    cfg.type = CONN_MPTCP;
    printf("\nTesting asynchronous API using polling_adapter MPTCP (%s:%d):\n", cfg.tcp.host, cfg.tcp.port);
    test_async_polling(cfg);
#endif /* IPPROTO_MPTCP */
    if (test_unix_socket) {
        cfg.type = CONN_UNIX;
        printf("\nTesting asynchronous API using polling_adapter UNIX (%s):\n", cfg.unix_sock.path);
        test_async_polling(cfg);
    }

    if (test_inherit_fd) {
        printf("\nTesting against inherited fd (%s): ", cfg.unix_sock.path);
        if (test_unix_socket) {
            printf("\n");
            cfg.type = CONN_FD;
            test_blocking_connection(cfg);
        } else {
            test_skipped();
        }
    }

    if (fails || (skips_as_fails && skips)) {
        printf("*** %d TESTS FAILED ***\n", fails);
        if (skips) {
            printf("*** %d TESTS SKIPPED ***\n", skips);
        }
        return 1;
    }

    printf("ALL TESTS PASSED (%d skipped)\n", skips);
    return 0;
}
