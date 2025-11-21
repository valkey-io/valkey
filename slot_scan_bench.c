// slot_scan_bench.c
// Compile: clang -O3 -Wall -Wextra -o slot_scan_bench slot_scan_bench.c

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLUSTER_SLOTS       16384
#define SLOT_BYTES          (CLUSTER_SLOTS / 8)
#define CLUSTER_SLOT_WORDS  (CLUSTER_SLOTS / 64)
#define SLOT_WORD_OFFSET(w) ((w) << 3)   /* 8 bytes per 64-bit word */

/* --- Helpers mirroring Valkey style --- */

static inline void bitmapSetBit(unsigned char *bitmap, int slot) {
    int byte = slot >> 3;
    int bit = slot & 7;
    bitmap[byte] |= (unsigned char) (1u << bit);
}

static inline int bitmapTestBit(const unsigned char *bitmap, int slot) {
    int byte = slot >> 3;
    int bit = slot & 7;
    return (bitmap[byte] >> bit) & 1;
}

/* Our "slots" and "owner_not_claiming_slot" simulation */
static int slots[CLUSTER_SLOTS];
static unsigned char owner_not_claiming[SLOT_BYTES];

/* Simulated isSlotUnclaimed, matching:
 *  (slots[slot] == NULL || bitmapTestBit(owner_not_claiming_slot, slot))
 *
 * Here we treat owner == 0 as "no owner / NULL".
 */
static inline int isSlotUnclaimedBench(int slot) {
    return slots[slot] == 0 || bitmapTestBit(owner_not_claiming, slot);
}

/* When iterating through the slot bitmap, group every 64 bits as
 * a word to speed up. */
static inline int clusterExtractSlotFromWord(uint64_t *slot_word, size_t slot_word_index) {
    /* Get the index of the least-significant set bit, in this 64-bit word */
    const unsigned bit = (unsigned) __builtin_ctzll(*slot_word);
    const int slot = (int) ((slot_word_index << 6) | bit); // word_index * 64 + bit
    *slot_word &= *slot_word - 1; /* clear that bit */
    return slot;
}

/* --- Two scanning strategies --- */

/* Old approach: iterate all 16384 slots and test each bit. */
static uint64_t scan_slow(const unsigned char *claimed_slots, int sender) {
    uint64_t acc = 0;
    for (int slot = 0; slot < CLUSTER_SLOTS; ++slot) {
        if (!bitmapTestBit(claimed_slots, slot)) continue;

        int owner = slots[slot];
        if (owner == sender || isSlotUnclaimedBench(slot)) continue;
        acc += (uint64_t)owner;
    }
    return acc;
}

/* New approach: word-based scan using CLUSTER_SLOT_WORDS + SLOT_WORD_OFFSET
 * and clusterExtractSlotFromWord.
 */
static uint64_t scan_fast(const unsigned char *claimed_slots, int sender) {
    uint64_t acc = 0;
    for (size_t w = 0; w < CLUSTER_SLOT_WORDS; ++w) {
        uint64_t word;
        memcpy(&word, claimed_slots + SLOT_WORD_OFFSET(w), sizeof(word));
        while (word) {
            int slot = clusterExtractSlotFromWord(&word, w);
            int owner = slots[slot];
            if (owner == sender || isSlotUnclaimedBench(slot)) continue;
            acc += (uint64_t)owner;
        }
    }
    return acc;
}

/* --- Simple timing helpers --- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + ts.tv_nsec;
}


/* Fill claimed_slots, slots[], and owner_not_claiming[] with some
 * semi-realistic distribution:
 *
 * - shard_size = number of claimed slots for this node
 * - some slots have owner 0 (unclaimed)
 * - some slots are marked in owner_not_claiming
 */
static void prepare_state(unsigned char *claimed_slots, int shard_size) {
    memset(claimed_slots, 0, SLOT_BYTES);
    memset(owner_not_claiming, 0, SLOT_BYTES);

    /* For simplicity: mark "shard_size" claimed slots uniformly. */
    for (int i = 0; i < shard_size; ++i) {
        int slot = rand() % CLUSTER_SLOTS;
        bitmapSetBit(claimed_slots, slot);
    }

    /* Initialize slots[]:  some 0 (unowned), some 1..4 as owners. */
    for (int slot = 0; slot < CLUSTER_SLOTS; ++slot) {
        int r = rand() % 10;
        if (r == 0) {
            slots[slot] = 0;       // treat as "NULL"
        } else {
            slots[slot] = (rand() % 4) + 1; // owner IDs 1..4
        }
    }

    /* For a small percentage of slots, mark owner_not_claiming */
    for (int i = 0; i < CLUSTER_SLOTS / 20; ++i) { // ~5% of slots
        int slot = rand() % CLUSTER_SLOTS;
        bitmapSetBit(owner_not_claiming, slot);
    }
}

static void run_case(const char *label, const int shard_size, const int iterations) {
    unsigned char claimed_slots[SLOT_BYTES];

    /* Simulate "sender" as owner id 1 */
    int sender = 1;

    prepare_state(claimed_slots, shard_size);

    /* Warm up CPU instruction cache*/
    for (int i = 0; i < 1000; ++i) {
        (void)scan_slow(claimed_slots, sender);
        (void)scan_fast(claimed_slots, sender);
    }

    uint64_t t0, t1;
    double slow_time, fast_time;
    uint64_t acc_slow = 0, acc_fast = 0;

    t0 = now_ns();
    for (int i = 0; i < iterations; ++i) {
        acc_slow += scan_slow(claimed_slots, sender);
    }
    t1 = now_ns();
    slow_time = t1 - t0;

    t0 = now_ns();
    for (int i = 0; i < iterations; ++i) {
        acc_fast += scan_fast(claimed_slots, sender);
    }
    t1 = now_ns();
    fast_time = t1 - t0;

    /* This line is critical to avoid compiler's over-optimization that would
     * skip the entire experiment program.
     */
    printf("acc_slow=%llu, acc_fast=%llu \n", acc_slow, acc_fast);

    printf("[%s] set_bits=%d iters=%d  slow=%.fns  fast=%.fns  speedup=%.2fx\n",
           label, shard_size, iterations, slow_time, fast_time,
           slow_time / fast_time);
}

int main(void) {
    srand(42);

    /* Tune this parameter as you'd like */
    const int iters = 1000;
    run_case("VERY_SPARSE", 16, iters);
    run_case("SPARSE", 256, iters);
    run_case("MEDIUM", 1024, iters);
    run_case("DENSE", 4096, iters);

    return 0;
}
