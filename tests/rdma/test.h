#ifndef VALKEY_RDMA_TEST_H
#define VALKEY_RDMA_TEST_H

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define RDMA_TEST_DEFAULT_PORT 6379

#define RDMA_TEST_DEFAULT_DATASIZE_RDMA_TEST 1024
#define RDMA_TEST_DEFAULT_DATASIZE_LIBVALKEY_TEST 256

#define RDMA_TEST_MAX_THREADS 32

typedef struct {
  const char *host;
  int port;
  int threads;
  size_t datasize;
  int minkeys;
  int maxkeys;
  int clients;
  int pipeline;
  long long requests;
} rdma_test_config;

static inline void usage(const char *proc) {
  printf("%s usage:\n", proc);
  printf("\t--help/-H\n");
  printf("\t--host/-h HOSTADDR\n");
  printf("\t--port/-p PORT\n");
  printf("\t--thread/-t THREADS\n");
  printf("\t--datasize/-d DATASIZE\n");
  printf("\t--maxkeys/-M MAXKEYS\n");
  printf("\t--minkeys/-m MINKEYS\n");
  printf("\t--clients/-c CLIENTS\n");
  printf("\t--pipeline/-P PIPELINE\n");
  printf("\t--requests/-n REQUESTS\n");
}

static inline int rdmaTestParseIntRange(const char *name, const char *value,
                                        long min, long max) {
  char *end = NULL;
  long val;

  errno = 0;
  val = strtol(value, &end, 10);
  if (errno || !end || *end || val < min || val > max) {
    fprintf(stderr, "%s must be an integer in [%ld, %ld]\n", name, min, max);
    exit(1);
  }

  return (int)val;
}

static inline int rdmaTestParsePositiveInt(const char *name,
                                           const char *value) {
  return rdmaTestParseIntRange(name, value, 1, INT_MAX);
}

static inline long long rdmaTestParsePositiveLongLong(const char *name,
                                                      const char *value) {
  char *end = NULL;
  long long val;

  errno = 0;
  val = strtoll(value, &end, 10);
  if (errno || !end || *end || val <= 0) {
    fprintf(stderr, "%s must be a positive integer\n", name);
    exit(1);
  }

  return val;
}

static inline int rdmaTestParsePort(const char *value) {
  return rdmaTestParseIntRange("--port", value, 1, 65535);
}

static inline size_t rdmaTestParseDatasize(const char *value) {
  return (size_t)rdmaTestParsePositiveInt("--datasize", value);
}

/* Parse every RDMA test CLI option into cfg. The caller seeds cfg with its own
 * defaults first; this only overrides fields supplied on the command line, so
 * options a test does not care about stay at their (zero) default. Exits on
 * --help, an unknown option, or a missing --host. */
static inline void rdmaTestParseArgs(int argc, char **argv,
                                     rdma_test_config *cfg) {
  static struct option long_opts[] = {
      {"help", no_argument, NULL, 'H'},
      {"host", required_argument, NULL, 'h'},
      {"port", required_argument, NULL, 'p'},
      {"thread", required_argument, NULL, 't'},
      {"datasize", required_argument, NULL, 'd'},
      {"minkeys", required_argument, NULL, 'm'},
      {"maxkeys", required_argument, NULL, 'M'},
      {"clients", required_argument, NULL, 'c'},
      {"pipeline", required_argument, NULL, 'P'},
      {"requests", required_argument, NULL, 'n'},
      {NULL, 0, NULL, 0},
  };
  static const char short_opts[] = "Hh:p:t:d:m:M:c:P:n:";
  int opt;

  while ((opt = getopt_long(argc, argv, short_opts, long_opts, NULL)) != -1) {
    switch (opt) {
    case 'h':
      cfg->host = optarg;
      break;
    case 'p':
      cfg->port = rdmaTestParsePort(optarg);
      break;
    case 't':
      cfg->threads =
          rdmaTestParseIntRange("--thread", optarg, 0, RDMA_TEST_MAX_THREADS);
      break;
    case 'd':
      cfg->datasize = rdmaTestParseDatasize(optarg);
      break;
    case 'm':
      cfg->minkeys = rdmaTestParsePositiveInt("--minkeys", optarg);
      break;
    case 'M':
      cfg->maxkeys = rdmaTestParsePositiveInt("--maxkeys", optarg);
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
    case 'H':
      usage(argv[0]);
      exit(0);
    default:
      usage(argv[0]);
      exit(1);
    }
  }

  /* host is required by every RDMA test; enforce it once here. */
  if (!cfg->host) {
    fprintf(stderr, "missing --host/-h\n");
    usage(argv[0]);
    exit(1);
  }
}

static inline size_t rdmaTestValueBufSize(size_t datasize) {
  return datasize + 1; /* NUL for C-string based clients */
}

/* Key formatting & value-fill helpers shared by the RDMA test clients.
 * Only the text generation is shared; each test keeps its own KV lifecycle
 * (struct layout, RESP encoding, reply validation). */
static inline void rdmaTestFormatThreadKey(char *buf, size_t len, int tid,
                                           int index) {
  snprintf(buf, len, "THREAD%02d-%06d", tid, index);
}

static inline void rdmaTestFormatClientSeqKey(char *buf, size_t len, int client,
                                              long long seq) {
  snprintf(buf, len, "rdma:%04d:%012lld", client, seq);
}

/* Deterministic A-Z pattern, for values that must be reproducible on read. */
static inline void rdmaTestFillPattern(char *buf, size_t len) {
  for (size_t i = 0; i < len; i++)
    buf[i] = 'A' + (i % 26);
}

/* Random uppercase A-Z, for values that only need to be non-trivial. */
static inline void rdmaTestFillRandomUppercase(char *buf, size_t len) {
  for (size_t i = 0; i < len; i++)
    buf[i] = 'A' + random() % 26;
}

#endif /* VALKEY_RDMA_TEST_H */
