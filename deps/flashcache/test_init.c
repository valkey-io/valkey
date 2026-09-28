#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "include/flashcache.h"
#include "include/flashcache_common.h"

void noop_logger(int level, const char *fmt, ...) {
    (void)level; (void)fmt;
}

uint64_t mono_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void noop_eviction(void *ctx, uint32_t dbid, char *key, size_t key_len) {
    (void)ctx; (void)dbid; (void)key; (void)key_len;
}

void noop_asio(void *ctx) { (void)ctx; }

int main() {
    printf("Creating test file...\n");
    system("fallocate -l 1G /tmp/fc_test.db");

    static char sentinel = 0;
    flashcacheEvictionDetails eviction = { &sentinel, noop_eviction };
    flashcacheAsioControlMsgCallbackDetails asio = { &sentinel, noop_asio };

    printf("Calling flashcacheInit...\n");
    flashcacheReturnCode rc = flashcacheInit(
        "/tmp/fc_test.db", 1073741824, 1024, 16, 90, 128,
        1048576, 5, 0, mono_clock, &eviction, noop_logger, &asio
    );
    printf("flashcacheInit returned: %d\n", rc);

    if (rc == FC_OK) {
        printf("FlashCache initialized successfully!\n");
        flashcacheTearDown();
    }

    system("rm -f /tmp/fc_test.db");
    return 0;
}
