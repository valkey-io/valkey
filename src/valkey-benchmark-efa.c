/* EFA transfer dialect for valkey-large-object. See valkey-benchmark-efa.h.
 *
 * The libfabric bring-up mirrors tests/harness/fabric_target.rs in the module
 * repository, which is the sequence the module's own fabric services are known to
 * pair with. The benchmark never posts an operation: the server initiates every
 * RMA, so this side only has to stay enabled, hold the server's addresses, and
 * poll its completion queue so a manual-progress provider (tcp) services inbound
 * writes.
 *
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fmacros.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>

#include "valkey-benchmark-efa.h"
#include "zmalloc.h"

/* A u64 is at most 20 decimal digits, so every substituted value fits the token
 * exactly and no RESP length changes after the template is built. */
#define WIDE_TOKEN_LEN 20
/* sizeof(struct efa_ep_addr). fi_av_insert expects this length on efa. */
#define EFA_ADDRESS_LEN 32
static const char RKEY_TOKEN[] = "__efa_rkey__";
static const char ADDR_TOKEN[] = "__efa_addr__";
static const char LEN_TOKEN[] = "__efa_len__";
static const char RKEY_WIDE[] = "__efa_rkey__________";
static const char ADDR_WIDE[] = "__efa_addr__________";
static const char LEN_WIDE[] = "__efa_len___________";

struct efaRegion {
    void *buffer;
    struct fid_mr *memory_region;
    uint64_t rkey;
    uint64_t remote_address;
    size_t length;
};

static struct {
    struct fi_info *info;
    struct fid_fabric *fabric;
    struct fid_domain *domain;
    struct fid_av *address_vector;
    struct fid_cq *completion_queue;
    struct fid_ep *endpoint;
    size_t region_len;
    int virtual_addressing;
    unsigned char *local_address;
    size_t local_address_len;
    /* tcp hands requested_key back as the rkey, so each region needs its own. */
    uint64_t next_requested_key;
    pthread_t progress_thread;
    /* Server addresses already inserted; HELLO replies repeat them per connection. */
    pthread_mutex_t peers_mutex;
    unsigned char **peers;
    size_t *peer_lens;
    size_t peer_count;
} fabric;

static void fatal(const char *what, int code) {
    fprintf(stderr, "EFA: %s failed: %s (%d)\n", what, fi_strerror(-code), code);
    exit(1);
}

static void check(int code, const char *what) {
    if (code != 0) fatal(what, code);
}

static sds encodeHex(const unsigned char *bytes, size_t len) {
    static const char digits[] = "0123456789abcdef";
    sds out = sdsnewlen(NULL, len * 2);
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 0x0f];
    }
    return out;
}

static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Returns the decoded length, or 0 when `text` is not even-length hex. */
static size_t decodeHex(const char *text, size_t text_len, unsigned char *out) {
    if (text_len == 0 || text_len % 2 != 0) return 0;
    for (size_t i = 0; i < text_len; i += 2) {
        int high = hexDigit(text[i]), low = hexDigit(text[i + 1]);
        if (high < 0 || low < 0) return 0;
        out[i / 2] = (unsigned char)(high << 4 | low);
    }
    return text_len / 2;
}

/* Inbound RMA raises no completion on the target, so this loop only exists to
 * drive progress and surface asynchronous errors. */
static void *progressLoop(void *arg) {
    (void)arg;
    for (;;) {
        struct fi_cq_entry entry;
        ssize_t n = fi_cq_read(fabric.completion_queue, &entry, 1);
        if (n == -FI_EAVAIL) {
            struct fi_cq_err_entry error;
            memset(&error, 0, sizeof(error));
            if (fi_cq_readerr(fabric.completion_queue, &error, 0) > 0) {
                fprintf(stderr, "EFA: completion error: %s\n",
                        fi_cq_strerror(fabric.completion_queue, error.prov_errno, error.err_data, NULL, 0));
            }
        } else if (n == -FI_EAGAIN) {
            usleep(10);
        }
    }
    return NULL;
}

void efaInit(const char *provider, const char *bind, size_t region_len) {
    int efa_direct = strcmp(provider, "efa-direct") == 0;
    if (!efa_direct && strcmp(provider, "tcp") != 0) {
        fprintf(stderr, "EFA: --efa-provider must be efa-direct or tcp, got '%s'\n", provider);
        exit(1);
    }
    if (bind && efa_direct) {
        fprintf(stderr, "EFA: --efa-bind applies to the tcp provider only\n");
        exit(1);
    }
    fabric.region_len = region_len;
    pthread_mutex_init(&fabric.peers_mutex, NULL);

    struct fi_info *hints = fi_allocinfo();
    if (!hints) fatal("fi_allocinfo", -FI_ENOMEM);
    hints->caps = FI_MSG | FI_RMA | FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE;
    hints->ep_attr->type = FI_EP_RDM;
    hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_VIRT_ADDR;
    /* fi_freeinfo frees these, so they must be heap strings. */
    hints->fabric_attr->prov_name = strdup(efa_direct ? "efa" : "tcp");
    if (efa_direct) {
        /* The efa provider exposes both the rxr `efa` fabric and device-RDMA
         * `efa-direct`; the module's services run on the latter, which needs FI_CONTEXT2. */
        hints->fabric_attr->name = strdup("efa-direct");
        hints->mode |= FI_CONTEXT2;
    }
    int code = fi_getinfo(fi_version(), bind, NULL, bind ? FI_SOURCE : 0, hints, &fabric.info);
    fi_freeinfo(hints);
    if (code == -FI_ENODATA) {
        fprintf(stderr, "EFA: no %s fabric on this host%s%s\n", provider, bind ? " bound to " : "", bind ? bind : "");
        exit(1);
    }
    check(code, "fi_getinfo");
    if (!fabric.info) {
        fprintf(stderr, "EFA: no %s provider matched\n", provider);
        exit(1);
    }

    check(fi_fabric(fabric.info->fabric_attr, &fabric.fabric, NULL), "fi_fabric");
    check(fi_domain(fabric.fabric, fabric.info, &fabric.domain, NULL), "fi_domain");
    struct fi_av_attr av_attr;
    memset(&av_attr, 0, sizeof(av_attr));
    av_attr.type = FI_AV_MAP;
    check(fi_av_open(fabric.domain, &av_attr, &fabric.address_vector, NULL), "fi_av_open");
    struct fi_cq_attr cq_attr;
    memset(&cq_attr, 0, sizeof(cq_attr));
    cq_attr.format = FI_CQ_FORMAT_CONTEXT;
    check(fi_cq_open(fabric.domain, &cq_attr, &fabric.completion_queue, NULL), "fi_cq_open");
    check(fi_endpoint(fabric.domain, fabric.info, &fabric.endpoint, NULL), "fi_endpoint");
    check(fi_ep_bind(fabric.endpoint, &fabric.address_vector->fid, 0), "fi_ep_bind(av)");
    check(fi_ep_bind(fabric.endpoint, &fabric.completion_queue->fid, FI_TRANSMIT | FI_RECV), "fi_ep_bind(cq)");
    check(fi_enable(fabric.endpoint), "fi_enable");
    fabric.virtual_addressing = (fabric.info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) != 0;

    /* Ask for the length first, then the address itself. */
    fabric.local_address_len = 0;
    fi_getname(&fabric.endpoint->fid, NULL, &fabric.local_address_len);
    fabric.local_address = zmalloc(fabric.local_address_len);
    check(fi_getname(&fabric.endpoint->fid, fabric.local_address, &fabric.local_address_len), "fi_getname");

    code = pthread_create(&fabric.progress_thread, NULL, progressLoop, NULL);
    if (code != 0) {
        fprintf(stderr, "EFA: pthread_create failed: %s\n", strerror(code));
        exit(1);
    }
}

/* One forward pass: the wide form begins with the short token, so rescanning
 * from the start would match the replacement itself. */
static sds replaceAll(sds arg, const char *token, const char *wide) {
    size_t token_len = strlen(token);
    sds out = sdsempty();
    const char *cursor = arg;
    const char *at;
    while ((at = strstr(cursor, token)) != NULL) {
        out = sdscatlen(out, cursor, at - cursor);
        out = sdscat(out, wide);
        cursor = at + token_len;
    }
    out = sdscat(out, cursor);
    sdsfree(arg);
    return out;
}

sds efaExpandPlaceholders(sds arg) {
    arg = replaceAll(arg, RKEY_TOKEN, RKEY_WIDE);
    arg = replaceAll(arg, ADDR_TOKEN, ADDR_WIDE);
    return replaceAll(arg, LEN_TOKEN, LEN_WIDE);
}

efaRegion *efaRegisterRegion(void) {
    efaRegion *region = zmalloc(sizeof(*region));
    region->length = fabric.region_len;
    if (posix_memalign(&region->buffer, 4096, region->length) != 0) {
        fprintf(stderr, "EFA: cannot allocate a %zu byte region\n", region->length);
        exit(1);
    }
    /* Something non-constant for BLOB.SET to carry; the content is never checked. */
    for (size_t i = 0; i < region->length; i++) ((unsigned char *)region->buffer)[i] = (unsigned char)(i * 2654435761u >> 24);

    check(fi_mr_reg(fabric.domain, region->buffer, region->length,
                    FI_REMOTE_READ | FI_REMOTE_WRITE | FI_READ | FI_WRITE, 0,
                    fabric.next_requested_key++, 0, &region->memory_region, NULL),
          "fi_mr_reg");
    region->rkey = fi_mr_key(region->memory_region);
    /* Virtual-address providers (efa) take the buffer's address; offset providers
     * (tcp) address from the start of the registration. */
    region->remote_address = fabric.virtual_addressing ? (uint64_t)(uintptr_t)region->buffer : 0;
    return region;
}

void efaReleaseRegion(efaRegion *region) {
    if (!region) return;
    fi_close(&region->memory_region->fid);
    free(region->buffer);
    zfree(region);
}

enum tokenField {
    TOKEN_RKEY,
    TOKEN_ADDR,
    TOKEN_LEN,
};

typedef struct {
    size_t offset;
    enum tokenField field;
} tokenSlot;

static struct {
    size_t cmd_len;
    size_t count;
    tokenSlot *slots;
} tokens;

static void findTokens(const char *cmd, size_t cmd_len, const char *wide, enum tokenField field) {
    const char *end = cmd + cmd_len;
    for (const char *at = cmd; at + WIDE_TOKEN_LEN <= end; at++) {
        /* memchr jumps to the next candidate; the tokens never contain '\0'. */
        at = memchr(at, wide[0], end - at);
        if (!at || at + WIDE_TOKEN_LEN > end) return;
        if (memcmp(at, wide, WIDE_TOKEN_LEN) == 0) {
            tokens.slots = zrealloc(tokens.slots, (tokens.count + 1) * sizeof(*tokens.slots));
            tokens.slots[tokens.count].offset = at - cmd;
            tokens.slots[tokens.count].field = field;
            tokens.count++;
            at += WIDE_TOKEN_LEN - 1;
        }
    }
}

void efaInitPlaceholders(const char *cmd, size_t cmd_len) {
    zfree(tokens.slots);
    memset(&tokens, 0, sizeof(tokens));
    tokens.cmd_len = cmd_len;
    findTokens(cmd, cmd_len, RKEY_WIDE, TOKEN_RKEY);
    findTokens(cmd, cmd_len, ADDR_WIDE, TOKEN_ADDR);
    findTokens(cmd, cmd_len, LEN_WIDE, TOKEN_LEN);
}

void efaSubstituteRegion(const efaRegion *region, char *cmd_data, int cmd_count) {
    char text[3][WIDE_TOKEN_LEN + 1];
    snprintf(text[TOKEN_RKEY], sizeof(text[TOKEN_RKEY]), "%020" PRIu64, region->rkey);
    snprintf(text[TOKEN_ADDR], sizeof(text[TOKEN_ADDR]), "%020" PRIu64, region->remote_address);
    snprintf(text[TOKEN_LEN], sizeof(text[TOKEN_LEN]), "%020" PRIu64, (uint64_t)region->length);
    for (int cmd_index = 0; cmd_index < cmd_count; cmd_index++) {
        char *cmd = cmd_data + cmd_index * tokens.cmd_len;
        for (size_t i = 0; i < tokens.count; i++) {
            memcpy(cmd + tokens.slots[i].offset, text[tokens.slots[i].field], WIDE_TOKEN_LEN);
        }
    }
}

sds efaLocalAddressHex(void) {
    return encodeHex(fabric.local_address, fabric.local_address_len);
}

int efaFormatHello(char **buf) {
    sds hex = efaLocalAddressHex();
    int len = valkeyFormatCommand(buf, "BLOB.HELLO %s", hex);
    sdsfree(hex);
    return len;
}

static int peerKnown(const unsigned char *address, size_t len) {
    for (size_t i = 0; i < fabric.peer_count; i++) {
        if (fabric.peer_lens[i] == len && memcmp(fabric.peers[i], address, len) == 0) return 1;
    }
    return 0;
}

void efaHandleHelloReply(const valkeyReply *reply) {
    if (reply->type != VALKEY_REPLY_ARRAY || reply->elements == 0) {
        fprintf(stderr, "EFA: BLOB.HELLO reply is not an array of server addresses (type %d)\n", reply->type);
        exit(1);
    }
    pthread_mutex_lock(&fabric.peers_mutex);
    for (size_t i = 0; i < reply->elements; i++) {
        const valkeyReply *element = reply->element[i];
        if (element->type != VALKEY_REPLY_STRING) {
            fprintf(stderr, "EFA: BLOB.HELLO reply element %zu is not a string\n", i);
            exit(1);
        }
        unsigned char *address = zmalloc(element->len / 2 + 1);
        size_t len = decodeHex(element->str, element->len, address);
        if (len == 0) {
            fprintf(stderr, "EFA: BLOB.HELLO reply element %zu is not hex: %s\n", i, element->str);
            exit(1);
        }
        /* tcp addresses are sockaddrs of varying length. Efa has a fixed size. */
        if (fabric.info->addr_format == FI_ADDR_EFA && len != EFA_ADDRESS_LEN) {
            fprintf(stderr, "EFA: BLOB.HELLO reply element %zu is %zu bytes, not %d: %s\n", i, len,
                    EFA_ADDRESS_LEN, element->str);
            exit(1);
        }
        if (peerKnown(address, len)) {
            zfree(address);
            continue;
        }
        fi_addr_t peer;
        int inserted = fi_av_insert(fabric.address_vector, address, 1, &peer, 0, NULL);
        if (inserted != 1) {
            fprintf(stderr, "EFA: fi_av_insert of server address %s inserted %d of 1\n", element->str, inserted);
            exit(1);
        }
        fabric.peers = zrealloc(fabric.peers, (fabric.peer_count + 1) * sizeof(*fabric.peers));
        fabric.peer_lens = zrealloc(fabric.peer_lens, (fabric.peer_count + 1) * sizeof(*fabric.peer_lens));
        fabric.peers[fabric.peer_count] = address;
        fabric.peer_lens[fabric.peer_count] = len;
        fabric.peer_count++;
    }
    pthread_mutex_unlock(&fabric.peers_mutex);
}
