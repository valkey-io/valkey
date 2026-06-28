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

#define RDMA_TEST_COMMON_OPTIONS                                               \
  {"help", no_argument, NULL, 'H'}, {"host", required_argument, NULL, 'h'},    \
      {"port", required_argument, NULL, 'p'},                                  \
      {"thread", required_argument, NULL, 't'},                                \
      {"datasize", required_argument, NULL, 'd'}

#define RDMA_TEST_OPTIONS_END {NULL, 0, NULL, 0}
#define RDMA_TEST_COMMON_SHORT_OPTS "Hh:p:t:"

typedef struct rdma_test_config {
  const char *host;
  int port;
  int threads;
  size_t datasize;
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
