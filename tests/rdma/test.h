#ifndef VALKEY_RDMA_TEST_H
#define VALKEY_RDMA_TEST_H

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define RDMA_TEST_DEFAULT_PORT 6379

#define RDMA_TEST_COMMON_OPTIONS                                              \
  {"help", no_argument, NULL, 'H'},                                           \
      {"host", required_argument, NULL, 'h'},                                 \
      {"port", required_argument, NULL, 'p'},                                 \
      {"thread", required_argument, NULL, 't'}

#define RDMA_TEST_OPTIONS_END {NULL, 0, NULL, 0}
#define RDMA_TEST_COMMON_SHORT_OPTS "Hh:p:t:"

typedef struct rdma_test_config {
  const char *host;
  int port;
  int threads;
} rdma_test_config;

static inline void rdmaTestPrintCommonUsage(void) {
  printf("\t--help/-H\n");
  printf("\t--host/-h HOSTADDR\n");
  printf("\t--port/-p PORT\n");
  printf("\t--thread/-t THREADS\n");
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

#endif /* VALKEY_RDMA_TEST_H */
