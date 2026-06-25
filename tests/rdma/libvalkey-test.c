#ifdef __linux__ /* currently RDMA is only supported on Linux */

#define _GNU_SOURCE
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "test.h"
#include "valkey/rdma.h"
#include "valkey/valkey.h"

#define DEFAULT_THREADS 16
#define DEFAULT_CLIENTS 128
#define DEFAULT_PIPELINE 384
#define DEFAULT_REQUESTS 200000
#define DEFAULT_DATASIZE 256
#define MAX_KEY_LEN 128

/* Global CLI parameters shared read-only by all worker threads. */
typedef struct test_config {
  rdma_test_config rdma;
  int clients;
  int pipeline;
  long long requests;
  size_t datasize;
  char *value;
} test_config;

/* Per-pthread slice: owns clients [first_client, last_client). */
typedef struct worker_config {
  const test_config *cfg;
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
  valkeyContext *context;
} client_state;

static void usage(const char *proc) {
  printf("%s usage:\n", proc);
  rdmaTestPrintCommonUsage();
  printf("\t--clients/-c CLIENTS\n");
  printf("\t--pipeline/-P PIPELINE\n");
  printf("\t--requests/-n REQUESTS\n");
  printf("\t--datasize/-d DATASIZE\n");
}

static void make_key(char *buf, size_t len, int client_id, long long seq) {
  snprintf(buf, len, "rdma:%04d:%012lld", client_id, seq);
}

static long long requests_for_client(const test_config *cfg, int client_id) {
  long long base = cfg->requests / cfg->clients;
  long long extra = client_id < (cfg->requests % cfg->clients) ? 1 : 0;

  return base + extra;
}

static int check_status_reply(valkeyReply *reply, const char *expected,
                              int client_id, long long seq) {
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

static int check_string_reply(valkeyReply *reply, const test_config *cfg,
                              int client_id, long long seq) {
  if (!reply || reply->type != VALKEY_REPLY_STRING ||
      reply->len != cfg->datasize ||
      memcmp(reply->str, cfg->value, cfg->datasize) != 0) {
    fprintf(stderr, "client %d request %lld expected %zu-byte string\n",
            client_id, seq, cfg->datasize);
    if (reply)
      fprintf(stderr, "reply type=%d len=%zu\n", reply->type, reply->len);
    return -1;
  }

  return 0;
}

static valkeyContext *connect_rdma(const test_config *cfg, int client_id) {
  valkeyOptions options = {0};
  valkeyContext *context;
  valkeyReply *reply;

  VALKEY_OPTIONS_SET_RDMA(&options, cfg->rdma.host, cfg->rdma.port);
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

static int append_set(client_state *state, const test_config *cfg,
                      long long seq) {
  char key[MAX_KEY_LEN];

  make_key(key, sizeof(key), state->client_id, seq);
  if (valkeyAppendCommand(state->context, "SET %s %b", key, cfg->value,
                          cfg->datasize) != VALKEY_OK) {
    fprintf(stderr, "client %d append SET error: %s\n", state->client_id,
            state->context->errstr);
    return -1;
  }

  return 0;
}

static int append_get(client_state *state, long long seq) {
  char key[MAX_KEY_LEN];

  make_key(key, sizeof(key), state->client_id, seq);
  if (valkeyAppendCommand(state->context, "GET %s", key) != VALKEY_OK) {
    fprintf(stderr, "client %d append GET error: %s\n", state->client_id,
            state->context->errstr);
    return -1;
  }

  return 0;
}

static int drain_reply(client_state *state, const test_config *cfg, int is_get,
                       long long seq) {
  valkeyReply *reply = NULL;
  int ret;

  if (valkeyGetReply(state->context, (void **)&reply) != VALKEY_OK) {
    fprintf(stderr, "client %d read error: %s\n", state->client_id,
            state->context->errstr);
    return -1;
  }

  ret = is_get ? check_string_reply(reply, cfg, state->client_id, seq)
               : check_status_reply(reply, "OK", state->client_id, seq);
  freeReplyObject(reply);

  return ret;
}

/* Run SET (is_get=0) or GET (is_get=1) for every connection in this worker. */
static int run_phase(client_state *states, int state_count,
                     const test_config *cfg, int is_get) {
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
          if (append_set(state, cfg, seq) != 0)
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
        if (drain_reply(state, cfg, is_get, seq) != 0)
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
  const test_config *cfg = worker->cfg;
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
    states[i].requests = requests_for_client(cfg, client_id);
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

  printf("Valkey Over RDMA libvalkey thread[%d] clients %d-%d SET/GET %lld "
         "requests [OK]\n",
         worker->thread_id, worker->first_client, worker->last_client - 1,
         cfg->requests);
  ret = 0;

cleanup:
  for (int i = 0; i < state_count; i++) {
    if (states[i].context)
      valkeyFree(states[i].context);
  }
  free(states);

  return (void *)(long)ret;
}

static void parse_args(int argc, char **argv, test_config *cfg) {
  static struct option long_opts[] = {
      RDMA_TEST_COMMON_OPTIONS,
      {"clients", required_argument, NULL, 'c'},
      {"pipeline", required_argument, NULL, 'P'},
      {"requests", required_argument, NULL, 'n'},
      {"datasize", required_argument, NULL, 'd'},
      RDMA_TEST_OPTIONS_END,
  };
  int opt;

  while ((opt = getopt_long(argc, argv, RDMA_TEST_COMMON_SHORT_OPTS "c:P:n:d:",
                            long_opts, NULL)) != -1) {
    switch (opt) {
    case 'h':
      cfg->rdma.host = optarg;
      break;
    case 'p':
      cfg->rdma.port = rdmaTestParsePort(optarg);
      break;
    case 't':
      cfg->rdma.threads = rdmaTestParsePositiveInt("--thread", optarg);
      break;
    case 'c':
      cfg->clients = rdmaTestParsePositiveInt("--clients", optarg);
      break;
    case 'P':
      cfg->pipeline = rdmaTestParsePositiveInt("--pipeline", optarg);
      break;
    case 'n':
      cfg->requests = rdmaTestParsePositiveLongLong("--requests", optarg);
      break;
    case 'd':
      cfg->datasize = rdmaTestParsePositiveInt("--datasize", optarg);
      break;
    case 'H':
      usage(argv[0]);
      exit(0);
    default:
      usage(argv[0]);
      exit(1);
    }
  }

  if (!cfg->rdma.host) {
    fprintf(stderr, "missing --host/-h\n");
    usage(argv[0]);
    exit(1);
  }
  if (cfg->rdma.threads > cfg->clients)
    cfg->rdma.threads = cfg->clients;
}

static void init_value(test_config *cfg) {
  cfg->value = malloc(cfg->datasize);
  if (!cfg->value) {
    fprintf(stderr, "failed to allocate value buffer\n");
    exit(1);
  }

  for (size_t i = 0; i < cfg->datasize; i++)
    cfg->value[i] = 'A' + (i % 26);
}

int main(int argc, char **argv) {
  test_config cfg = {
      .rdma =
          {
              .port = RDMA_TEST_DEFAULT_PORT,
              .threads = DEFAULT_THREADS,
          },
      .clients = DEFAULT_CLIENTS,
      .pipeline = DEFAULT_PIPELINE,
      .requests = DEFAULT_REQUESTS,
      .datasize = DEFAULT_DATASIZE,
  };
  pthread_t *threads;
  worker_config *workers;
  int ret = 0;

  parse_args(argc, argv, &cfg);
  init_value(&cfg);

  if (valkeyInitiateRdma() != VALKEY_OK) {
    fprintf(stderr, "failed to initialize libvalkey RDMA support\n");
    free(cfg.value);
    return 1;
  }

  threads = calloc(cfg.rdma.threads, sizeof(*threads));
  workers = calloc(cfg.rdma.threads, sizeof(*workers));
  if (!threads || !workers) {
    fprintf(stderr, "failed to allocate worker metadata\n");
    free(threads);
    free(workers);
    free(cfg.value);
    return 1;
  }

  printf("Valkey Over RDMA libvalkey test host=%s port=%d threads=%d "
         "clients=%d pipeline=%d requests=%lld datasize=%zu\n",
         cfg.rdma.host, cfg.rdma.port, cfg.rdma.threads, cfg.clients,
         cfg.pipeline, cfg.requests, cfg.datasize);

  for (int i = 0; i < cfg.rdma.threads; i++) {
    workers[i].cfg = &cfg;
    workers[i].thread_id = i;
    workers[i].first_client = (cfg.clients * i) / cfg.rdma.threads;
    workers[i].last_client = (cfg.clients * (i + 1)) / cfg.rdma.threads;
    if (pthread_create(&threads[i], NULL, worker_main, &workers[i])) {
      fprintf(stderr, "failed to create thread %d\n", i);
      ret = 1;
      cfg.rdma.threads = i;
      break;
    }
  }

  for (int i = 0; i < cfg.rdma.threads; i++) {
    void *thread_ret = NULL;

    pthread_join(threads[i], &thread_ret);
    if ((long)thread_ret)
      ret = 1;
  }

  if (!ret)
    printf("Valkey Over RDMA libvalkey pipeline test [OK]\n");

  free(threads);
  free(workers);
  free(cfg.value);

  return ret;
}

#else /* __linux__ */

#error "BUILD ERROR: RDMA is only supported on Linux"

#endif /* __linux__ */
