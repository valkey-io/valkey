/* ==========================================================================
 * libvalkey-test.c - libvalkey pipeline test client for Valkey Over RDMA
 *                      (Linux only)
 * --------------------------------------------------------------------------
 * Copyright (C) 2026  quanye yang <quanyeyang@proton.me>
 *
 * This work is licensed under BSD 3-Clause, License 1 of the COPYING file in
 * the top-level directory.
 * ==========================================================================
 */

#ifdef __linux__ /* currently RDMA is only supported on Linux */

#define _GNU_SOURCE
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "test.h"
#include "valkey/rdma.h"
#include "valkey/valkey.h"

#define DEFAULT_THREADS 16
#define DEFAULT_CLIENTS 128
#define DEFAULT_PIPELINE 384
#define DEFAULT_MAXKEYS 2048
#define DEFAULT_MINKEYS 1024
#define MAX_KEY_LEN 128

/* Per-pthread slice: owns clients [first_client, last_client). cfg is shared
 * read-only by all worker threads. */
typedef struct worker_config {
    const rdma_test_config *cfg;
    int thread_id;
    int first_client;
    int last_client;
} worker_config;

/* One RDMA connection; processed counts completed cmds, pending is current
 * batch. */
typedef struct client_state {
    int client_id;
    long long requests;  /* total SET or GET cmds for this connection */
    long long processed; /* completed so far in the current phase */
    int pending;         /* cmds appended but not yet drained this batch */
    char **values;       /* per-seq random values [0,requests) for SET/GET */
    valkeyContext *context;
} client_state;

static int check_status_reply(valkeyReply *reply, const char *expected, int client_id, long long seq) {
    if (!reply || reply->type != VALKEY_REPLY_STATUS ||
        strcmp(reply->str, expected) != 0) {
        fprintf(stderr, "client %d request %lld expected status %s\n", client_id,
                seq, expected);
        if (reply)
            fprintf(stderr, "reply type=%d len=%zu str=%s\n", reply->type, reply->len,
                    reply->str ? reply->str : "(null)");
        return -1;
    }

    return 0;
}

static int check_string_reply(valkeyReply *reply, const char *value, size_t datasize, int client_id, long long seq) {
    if (!reply || reply->type != VALKEY_REPLY_STRING ||
        !rdmaTestValueEquals(reply->str, reply->len, value, datasize)) {
        fprintf(stderr, "client %d request %lld expected %zu-byte string\n",
                client_id, seq, datasize);
        if (reply)
            fprintf(stderr, "reply type=%d len=%zu\n", reply->type, reply->len);
        return -1;
    }

    return 0;
}

static valkeyContext *connect_rdma(const rdma_test_config *cfg, int client_id) {
    valkeyOptions options = {0};
    valkeyContext *context;
    valkeyReply *reply;

    VALKEY_OPTIONS_SET_RDMA(&options, cfg->host, cfg->port);
    context = valkeyConnectWithOptions(&options);
    if (!context) {
        fprintf(stderr, "client %d failed to allocate valkey context\n", client_id);
        return NULL;
    }
    if (context->err) {
        fprintf(stderr, "client %d connection error: %s\n", client_id,
                context->errstr);
        valkeyFree(context);
        return NULL;
    }

    reply = valkeyCommand(context, "PING");
    if (check_status_reply(reply, "PONG", client_id, -1) != 0) {
        freeReplyObject(reply);
        valkeyFree(context);
        return NULL;
    }
    freeReplyObject(reply);

    return context;
}

static int flush_context(valkeyContext *context, int client_id) {
    int done = 0;

    while (!done) {
        if (valkeyBufferWrite(context, &done) != VALKEY_OK) {
            fprintf(stderr, "client %d write error: %s\n", client_id,
                    context->errstr);
            return -1;
        }
    }

    return 0;
}

static int append_set(client_state *state, const char *value, size_t datasize, long long seq) {
    char key[MAX_KEY_LEN];

    rdmaTestFormatClientSeqKey(key, sizeof(key), state->client_id, seq);
    if (valkeyAppendCommand(state->context, "SET %s %b", key, value, datasize) !=
        VALKEY_OK) {
        fprintf(stderr, "client %d append SET error: %s\n", state->client_id,
                state->context->errstr);
        return -1;
    }

    return 0;
}

static int append_get(client_state *state, long long seq) {
    char key[MAX_KEY_LEN];

    rdmaTestFormatClientSeqKey(key, sizeof(key), state->client_id, seq);
    if (valkeyAppendCommand(state->context, "GET %s", key) != VALKEY_OK) {
        fprintf(stderr, "client %d append GET error: %s\n", state->client_id,
                state->context->errstr);
        return -1;
    }

    return 0;
}

static int drain_reply(client_state *state, const char *value, size_t datasize, int is_get, long long seq) {
    valkeyReply *reply = NULL;
    int ret;

    if (valkeyGetReply(state->context, (void **)&reply) != VALKEY_OK) {
        fprintf(stderr, "client %d read error: %s\n", state->client_id,
                state->context->errstr);
        return -1;
    }

    ret = is_get
              ? check_string_reply(reply, value, datasize, state->client_id, seq)
              : check_status_reply(reply, "OK", state->client_id, seq);
    freeReplyObject(reply);

    return ret;
}

/* Run SET (is_get=0) or GET (is_get=1) for every connection in this worker. */
static int run_phase(client_state *states, int state_count, const rdma_test_config *cfg, int is_get) {
    int remaining_clients = state_count;

    while (remaining_clients) {
        remaining_clients = 0;

        /* Step 1: append up to pipeline commands into each context's obuf. */
        for (int i = 0; i < state_count; i++) {
            client_state *state = &states[i];
            int batch;

            state->pending = 0;
            if (state->processed >= state->requests)
                continue;

            remaining_clients++;
            batch = cfg->pipeline;
            if (state->requests - state->processed < batch)
                batch = state->requests - state->processed;

            for (int j = 0; j < batch; j++) {
                long long seq = state->processed + j;
                if (is_get) {
                    if (append_get(state, seq) != 0)
                        return -1;
                } else {
                    if (append_set(state, state->values[seq], cfg->datasize, seq) != 0)
                        return -1;
                }
            }
            state->pending = batch;
        }

        /* Step 2: flush obuf over RDMA (may require multiple valkeyBufferWrite). */
        for (int i = 0; i < state_count; i++) {
            if (states[i].pending &&
                flush_context(states[i].context, states[i].client_id) != 0)
                return -1;
        }

        /* Step 3: read replies in append order and validate OK or value bytes. */
        for (int i = 0; i < state_count; i++) {
            client_state *state = &states[i];

            for (int j = 0; j < state->pending; j++) {
                long long seq = state->processed + j;
                if (drain_reply(state, state->values[seq], cfg->datasize, is_get,
                                seq) != 0)
                    return -1;
            }
            state->processed += state->pending;
        }
    }

    return 0;
}

/* Each worker thread: own a client slice, one RDMA conn per client, SET then
 * GET. */
static void *worker_main(void *arg) {
    worker_config *worker = arg;
    const rdma_test_config *cfg = worker->cfg;
    int state_count = worker->last_client - worker->first_client;
    client_state *states;
    int ret = 1;

    states = calloc(state_count, sizeof(*states));
    if (!states) {
        fprintf(stderr, "thread %d failed to allocate client states\n",
                worker->thread_id);
        return (void *)(long)1;
    }

    /* One valkeyContext (RDMA QP) per client_id in [first_client, last_client).
     */
    for (int i = 0; i < state_count; i++) {
        int client_id = worker->first_client + i;

        states[i].client_id = client_id;
        states[i].requests = rdmaTestRandCount(cfg->minkeys, cfg->maxkeys);
        states[i].values = calloc((size_t)states[i].requests, sizeof(char *));
        if (!states[i].values)
            goto cleanup;
        for (long long s = 0; s < states[i].requests; s++) {
            states[i].values[s] = rdmaTestNewValue(cfg->datasize);
            if (!states[i].values[s])
                goto cleanup;
        }
        states[i].context = connect_rdma(cfg, client_id);
        if (!states[i].context)
            goto cleanup;
    }

    /* Phase 1: write all keys; phase 2: read them back on the same connections.
     */
    if (run_phase(states, state_count, cfg, 0) != 0)
        goto cleanup;
    for (int i = 0; i < state_count; i++)
        states[i].processed = 0;
    if (run_phase(states, state_count, cfg, 1) != 0)
        goto cleanup;

    long long total = 0;
    for (int i = 0; i < state_count; i++)
        total += states[i].requests;
    printf("Valkey Over RDMA libvalkey thread[%d] clients %d-%d SET/GET %lld "
           "requests [OK]\n",
           worker->thread_id, worker->first_client, worker->last_client - 1,
           total);
    ret = 0;

cleanup:
    for (int i = 0; i < state_count; i++) {
        if (states[i].values) {
            for (long long s = 0; s < states[i].requests; s++)
                free(states[i].values[s]);
            free(states[i].values);
        }
        if (states[i].context)
            valkeyFree(states[i].context);
    }
    free(states);

    return (void *)(long)ret;
}

int main(int argc, char **argv) {
    rdma_test_config cfg = {
        .port = RDMA_TEST_DEFAULT_PORT,
        .threads = DEFAULT_THREADS,
        .clients = DEFAULT_CLIENTS,
        .pipeline = DEFAULT_PIPELINE,
        .minkeys = DEFAULT_MINKEYS,
        .maxkeys = DEFAULT_MAXKEYS,
        .datasize = RDMA_TEST_DEFAULT_DATASIZE_LIBVALKEY_TEST,
    };
    pthread_t *threads;
    worker_config *workers;
    int ret = 0;

    rdmaTestParseArgs(argc, argv, &cfg);

    if (cfg.threads > cfg.clients)
        cfg.threads = cfg.clients;
    if (cfg.threads < 1) {
        fprintf(stderr, "--thread/-t must be >= 1 for libvalkey-test\n");
        return 1;
    }

    /* Seed the per-connection command-count RNG (see rdmaTestRandCount). */
    srandom(time(NULL) ^ getpid());

    if (valkeyInitiateRdma() != VALKEY_OK) {
        fprintf(stderr, "failed to initialize libvalkey RDMA support\n");
        return 1;
    }

    threads = calloc(cfg.threads, sizeof(*threads));
    workers = calloc(cfg.threads, sizeof(*workers));
    if (!threads || !workers) {
        fprintf(stderr, "failed to allocate worker metadata\n");
        free(threads);
        free(workers);
        return 1;
    }

    printf("Valkey Over RDMA libvalkey test host=%s port=%d threads=%d "
           "clients=%d pipeline=%d minkeys=%d maxkeys=%d datasize=%zu\n",
           cfg.host, cfg.port, cfg.threads, cfg.clients, cfg.pipeline,
           cfg.minkeys, cfg.maxkeys, cfg.datasize);

    for (int i = 0; i < cfg.threads; i++) {
        workers[i].cfg = &cfg;
        workers[i].thread_id = i;
        workers[i].first_client = (cfg.clients * i) / cfg.threads;
        workers[i].last_client = (cfg.clients * (i + 1)) / cfg.threads;
        if (pthread_create(&threads[i], NULL, worker_main, &workers[i])) {
            fprintf(stderr, "failed to create thread %d\n", i);
            ret = 1;
            cfg.threads = i;
            break;
        }
    }

    for (int i = 0; i < cfg.threads; i++) {
        void *thread_ret = NULL;

        pthread_join(threads[i], &thread_ret);
        if ((long)thread_ret)
            ret = 1;
    }

    if (!ret)
        printf("Valkey Over RDMA libvalkey pipeline test [OK]\n");

    free(threads);
    free(workers);

    return ret;
}

#else /* __linux__ */

#error "BUILD ERROR: RDMA is only supported on Linux"

#endif /* __linux__ */
