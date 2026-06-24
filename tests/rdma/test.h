#ifndef VALKEY_RDMA_TEST_H
#define VALKEY_RDMA_TEST_H

#include <getopt.h>
#include <stdio.h>

#define RDMA_TEST_COMMON_OPTIONS                                              \
  {"help", no_argument, NULL, 'H'},                                           \
      {"host", required_argument, NULL, 'h'},                                 \
      {"port", required_argument, NULL, 'p'},                                 \
      {"thread", required_argument, NULL, 't'}

#define RDMA_TEST_COMMON_SHORT_OPTS "Hh:p:t:"

static inline void rdmaTestPrintCommonUsage(void) {
  printf("\t--help/-H\n");
  printf("\t--host/-h HOSTADDR\n");
  printf("\t--port/-p PORT\n");
  printf("\t--thread/-t THREADS\n");
}

#endif /* VALKEY_RDMA_TEST_H */
