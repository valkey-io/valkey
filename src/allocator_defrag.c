/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 * This file implements allocator-specific defragmentation logic used
 * within the Valkey engine. Below is the relationship between various
 * components involved in allocation and defragmentation:
 *
 *                  Application code
 *                     /       \
 *         allocation /         \ defrag
 *                   /           \
 *              zmalloc    allocator_defrag
 *               /  |   \       /     \
 *              /   |    \     /       \
 *             /    |     \   /         \
 *        libc  tcmalloc  jemalloc     other
 *
 * Explanation:
 * - **Application code**: High-level application logic that uses memory
 *   allocation and may trigger defragmentation.
 * - **zmalloc**: An abstraction layer over the memory allocator, providing
 *   a uniform allocation interface to the application code. It can delegate
 *   to various underlying allocators (e.g., libc, tcmalloc, jemalloc, or others).
 *   It is not dependant on defrag implementation logic and it's possible to use jemalloc
 *   version that does not support defrag.
 * - **allocator_defrag**: This file contains allocator-specific logic for
 *   defragmentation, invoked from `defrag.c` when memory defragmentation is needed.
 *   currently jemalloc is the only allocator with implemented defrag logic. It is possible that
 *   future implementation will include non-allocator defragmentation (think of data-structure
 *   compaction for example).
 * - **Underlying allocators**: These are the actual memory allocators, such as
 *   libc, tcmalloc, jemalloc, or other custom allocators. The defragmentation
 *   logic in `allocator_defrag` interacts with these allocators to reorganize
 *   memory and reduce fragmentation.
 *
 * The `defrag.c` file acts as the central entry point for defragmentation,
 * invoking allocator-specific implementations provided here in `allocator_defrag.c`.
 *
 * Note: Developers working on `zmalloc` or `allocator_defrag` should refer to
 * the other component to ensure both are using the same allocator configuration.
 */

#include "server.h"
#include "serverassert.h"
#include "allocator_defrag.h"

#if defined(HAVE_DEFRAG) && defined(USE_JEMALLOC)

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

#define BATCH_QUERY_ARGS_OUT 3
#define SLAB_NFREE(out, i) out[(i) * BATCH_QUERY_ARGS_OUT]
#define SLAB_LEN(out, i) out[(i) * BATCH_QUERY_ARGS_OUT + 2]
#define SLAB_NUM_REGS(out, i) out[(i) * BATCH_QUERY_ARGS_OUT + 1]

#define UTILIZATION_THRESHOLD_FACTOR_MILI (125) // 12.5% additional utilization

/*
 * Represents a precomputed key for querying jemalloc statistics.
 *
 * The `jeMallctlKey` structure stores a key corresponding to a specific jemalloc
 * statistics field name. This key is used with the `je_mallctlbymib` interface
 * to query statistics more efficiently, bypassing the need for runtime string
 * lookup and translation performed by `je_mallctl`.
 *
 * - `je_mallctlnametomib` is called once for each statistics field to precompute
 *   and store the key corresponding to the field name.
 * - Subsequent queries use `je_mallctlbymib` with the stored key, avoiding the
 *   overhead of repeated string-based lookups.
 *
 */
typedef struct jeMallctlKey {
    size_t key[6]; /* The precomputed key used to query jemalloc statistics. */
    size_t keylen; /* The length of the key array. */
} jeMallctlKey;

/* Stores MIB (Management Information Base) keys for jemalloc bin queries.
 *
 * This struct holds precomputed `jeMallctlKey` values for querying various
 * jemalloc bin-related statistics efficiently.
 */
typedef struct jeBinInfoKeys {
    jeMallctlKey curr_slabs;    /* Key to query the current number of slabs in the bin. */
    jeMallctlKey nonfull_slabs; /* Key to query the number of non-full slabs in the bin. */
    jeMallctlKey curr_regs;     /* Key to query the current number of regions in the bin. */
} jeBinInfoKeys;

/* Represents detailed information about a jemalloc bin.
 *
 * This struct provides metadata about a jemalloc bin, including the size of
 * its regions, total number of regions, and related MIB keys for efficient
 * queries.
 */
typedef struct jeBinInfo {
    size_t reg_size;         /* Size of each region in the bin. */
    uint32_t nregs;          /* Total number of regions in the bin. */
    jeBinInfoKeys info_keys; /* Precomputed MIB keys for querying bin statistics. */
} jeBinInfo;

/* Represents the configuration for jemalloc bins.
 *
 * This struct contains information about the number of bins and metadata for
 * each bin, as well as precomputed keys for batch utility queries and epoch updates.
 */
typedef struct jemallocCB {
    unsigned nbins;                /* Number of bins in the jemalloc configuration. */
    jeBinInfo *bin_info;           /* Array of `jeBinInfo` structs, one for each bin. */
    jeMallctlKey util_batch_query; /* Key to query batch utilization information. */
    jeMallctlKey epoch;            /* Key to trigger statistics sync between threads. */
} jemallocCB;

/* Represents the latest usage statistics for a jemalloc bin.
 *
 * This struct tracks the current usage of a bin, including the number of slabs
 * and regions, and calculates the number of full slabs from other fields.
 */
typedef struct jemallocBinUsageData {
    size_t curr_slabs;         /* Current number of slabs in the bin. */
    size_t curr_nonfull_slabs; /* Current number of non-full slabs in the bin. */
    size_t curr_regs;          /* Current number of regions in the bin. */
} jemallocBinUsageData;


static int defrag_supported = 0;
/* Control block holding information about bins and query helper -
 * this structure is initialized once when calling allocatorDefragInit. It does not change afterwards*/
static jemallocCB je_cb = {0, NULL, {{0}, 0}, {{0}, 0}};
/* Holds the latest usage statistics for each bin. This structure is updated when calling
 * allocatorDefragGetFragSmallbins and later is used to make a defrag decision for a memory pointer. */
static jemallocBinUsageData *je_usage_info = NULL;


/* -----------------------------------------------------------------------------
 * Alloc/Free API that are cooperative with defrag
 * -------------------------------------------------------------------------- */

/* Allocation and free functions that bypass the thread cache
 * and go straight to the allocator arena bins.
 * Currently implemented only for jemalloc. Used for online defragmentation.
 */
void *allocatorDefragAlloc(size_t size) {
    void *ptr = je_mallocx(size, MALLOCX_TCACHE_NONE);
    return ptr;
}
void allocatorDefragFree(void *ptr, size_t size) {
    if (ptr == NULL) return;
    je_sdallocx(ptr, size, MALLOCX_TCACHE_NONE);
}

/* -----------------------------------------------------------------------------
 * Helper functions for jemalloc translation between size and index
 * -------------------------------------------------------------------------- */

/* Get the bin index in bin array from the reg_size.
 *
 * these are reverse engineered mapping of reg_size -> binind. We need this information because the utilization query
 * returns the size of the buffer and not the bin index, and we need the bin index to access it's usage information
 *
 * Note: In case future PR will return the binind (that is better API anyway) we can get rid of
 * these conversion functions
 */
static inline unsigned jeSize2BinIndexLgQ3(size_t sz) {
    /* Smallest power-of-2 quantum for binning */
    const size_t size_class_group_size = 4;
    /* Number of bins in each power-of-2 size class group */
    const size_t lg_quantum_3_first_pow2 = 3;
    /* Offset for exponential bins */
    const size_t lg_quantum_3_offset = ((64 >> lg_quantum_3_first_pow2) - 1);
    /* Small sizes (8-64 bytes) use linear binning */
    if (sz <= 64) {           // 64 = 1 << (lg_quantum_3_first_pow2 + 3)
        return (sz >> 3) - 1; // Divide by 8 and subtract 1
    }

    /* For larger sizes, use exponential binning */

    /* Calculate leading zeros of (sz - 1) to properly handle power-of-2 sizes */
    unsigned leading_zeros = __builtin_clzll(sz - 1);
    unsigned exp = 64 - leading_zeros; // Effective log2(sz)

    /* Calculate the size's position within its group */
    unsigned within_group_offset = size_class_group_size -
                                   (((1ULL << exp) - sz) >> (exp - lg_quantum_3_first_pow2));

    /* Calculate the final bin index */
    return within_group_offset +
           ((exp - (lg_quantum_3_first_pow2 + 3)) - 1) * size_class_group_size +
           lg_quantum_3_offset;
}
/* -----------------------------------------------------------------------------
 * Interface functions to get fragmentation info from jemalloc
 * -------------------------------------------------------------------------- */
#define ARENA_TO_QUERY MALLCTL_ARENAS_ALL

static inline void jeRefreshStats(const jemallocCB *je_cb) {
    uint64_t epoch = 1; // Value doesn't matter
    size_t sz = sizeof(epoch);
    /* Refresh stats */
    je_mallctlbymib(je_cb->epoch.key, je_cb->epoch.keylen, &epoch, &sz, &epoch, sz);
}

/* Extract key that corresponds to the given name for fast query. This should be called once for each key_name */
static inline int jeQueryKeyInit(const char *key_name, jeMallctlKey *key_info) {
    key_info->keylen = sizeof(key_info->key) / sizeof(key_info->key[0]);
    int res = je_mallctlnametomib(key_name, key_info->key, &key_info->keylen);
    /* sanity check that returned value is not larger than provided */
    assert(key_info->keylen <= sizeof(key_info->key) / sizeof(key_info->key[0]));
    return res;
}

/* Query jemalloc control interface using previously extracted key (with jeQueryKeyInit) instead of name string.
 * This interface (named MIB in jemalloc) is faster as it avoids string dict lookup at run-time. */
static inline int jeQueryCtlInterface(const jeMallctlKey *key_info, void *value) {
    size_t sz = sizeof(size_t);
    return je_mallctlbymib(key_info->key, key_info->keylen, value, &sz, NULL, 0);
}

static inline int binQueryHelperInitialization(jeBinInfoKeys *helper, unsigned bin_index) {
    char mallctl_name[128];

    /* Mib of fetch number of used regions in the bin */
    snprintf(mallctl_name, sizeof(mallctl_name), "stats.arenas." STRINGIFY(ARENA_TO_QUERY) ".bins.%d.curregs", bin_index);
    if (jeQueryKeyInit(mallctl_name, &helper->curr_regs) != 0) return -1;
    /* Mib of fetch number of current slabs in the bin */
    snprintf(mallctl_name, sizeof(mallctl_name), "stats.arenas." STRINGIFY(ARENA_TO_QUERY) ".bins.%d.curslabs", bin_index);
    if (jeQueryKeyInit(mallctl_name, &helper->curr_slabs) != 0) return -1;
    /* Mib of fetch nonfull slabs */
    snprintf(mallctl_name, sizeof(mallctl_name), "stats.arenas." STRINGIFY(ARENA_TO_QUERY) ".bins.%d.nonfull_slabs", bin_index);
    if (jeQueryKeyInit(mallctl_name, &helper->nonfull_slabs) != 0) return -1;

    return 0;
}

/* Initializes the defragmentation system for the jemalloc memory allocator.
 *
 * This function performs the necessary setup and initialization steps for the defragmentation system.
 * It retrieves the configuration information for the jemalloc arenas and bins, and initializes the usage
 * statistics data structure.
 *
 * return 0 on success, or a non-zero error code on failure.
 *
 * The initialization process involves the following steps:
 * 1. Check if defragmentation is supported by the current jemalloc version.
 * 2. Retrieve the arena bin configuration information using the `je_mallctlbymib` function.
 * 3. Initialize the `usage_latest` structure with the bin usage statistics and configuration data.
 * 4. Set the `defrag_supported` flag to indicate that defragmentation is enabled.
 *
 * Note: This function must be called before using any other defragmentation-related functionality.
 * It should be called during the initialization phase of the code that uses the
 * defragmentation feature.
 */
int allocatorDefragInit(void) {
    char mallctl_name[100];
    jeBinInfo *bin_info;
    size_t sz;
    int je_res;

    /* the init should be called only once, fail if unexpected call */
    assert(!defrag_supported);

    /* Get the mib of the per memory pointers query command that is used during defrag scan over memory */
    if (jeQueryKeyInit("experimental.utilization.batch_query", &je_cb.util_batch_query) != 0) return -1;

    je_res = jeQueryKeyInit("epoch", &je_cb.epoch);
    assert(je_res == 0);
    jeRefreshStats(&je_cb);

    /* get quantum for verification only, current code assumes lg-quantum should be 3 */
    size_t jemalloc_quantum;
    sz = sizeof(jemalloc_quantum);
    je_mallctl("arenas.quantum", &jemalloc_quantum, &sz, NULL, 0);
    /* lg-quantum should be 3 so jemalloc_quantum should be 1<<3 */
    assert(jemalloc_quantum == 8);

    sz = sizeof(je_cb.nbins);
    je_res = je_mallctl("arenas.nbins", &je_cb.nbins, &sz, NULL, 0);
    assert(je_res == 0 && je_cb.nbins != 0);

    je_cb.bin_info = je_calloc(je_cb.nbins, sizeof(jeBinInfo));
    assert(je_cb.bin_info != NULL);
    je_usage_info = je_calloc(je_cb.nbins, sizeof(jemallocBinUsageData));
    assert(je_usage_info != NULL);

    for (unsigned j = 0; j < je_cb.nbins; j++) {
        bin_info = &je_cb.bin_info[j];
        /* The size of the current bin */
        snprintf(mallctl_name, sizeof(mallctl_name), "arenas.bin.%d.size", j);
        sz = sizeof(bin_info->reg_size);
        je_res = je_mallctl(mallctl_name, &bin_info->reg_size, &sz, NULL, 0);
        assert(je_res == 0);
        /* Number of regions per slab */
        snprintf(mallctl_name, sizeof(mallctl_name), "arenas.bin.%d.nregs", j);
        sz = sizeof(bin_info->nregs);
        je_res = je_mallctl(mallctl_name, &bin_info->nregs, &sz, NULL, 0);
        assert(je_res == 0);

        /* init bin specific fast query keys */
        je_res = binQueryHelperInitialization(&bin_info->info_keys, j);
        assert(je_res == 0);

        /* verify the reverse map of reg_size to bin index */
        assert(jeSize2BinIndexLgQ3(bin_info->reg_size) == j);
    }

    /* defrag is supported mark it to enable defrag queries */
    defrag_supported = 1;
    return 0;
}

/* Total size of consumed meomry in unused regs in small bins (AKA external fragmentation).
 * The function will refresh the epoch.
 *
 * return total fragmentation bytes
 */
unsigned long allocatorDefragGetFragSmallbins(void) {
    assert(defrag_supported);
    unsigned long frag = 0;
    jeRefreshStats(&je_cb);
    for (unsigned j = 0; j < je_cb.nbins; j++) {
        jeBinInfo *bin_info = &je_cb.bin_info[j];
        jemallocBinUsageData *bin_usage = &je_usage_info[j];

        /* Number of current slabs in the bin */
        jeQueryCtlInterface(&bin_info->info_keys.curr_regs, &bin_usage->curr_regs);
        /* Number of current slabs in the bin */
        jeQueryCtlInterface(&bin_info->info_keys.curr_slabs, &bin_usage->curr_slabs);
        /* Number of non full slabs in the bin */
        jeQueryCtlInterface(&bin_info->info_keys.nonfull_slabs, &bin_usage->curr_nonfull_slabs);

        /* Calculate the fragmentation bytes for the current bin and add it to the total. */
        frag += ((bin_info->nregs * bin_usage->curr_slabs) - bin_usage->curr_regs) * bin_info->reg_size;
    }
    return frag;
}

/* Determines whether defragmentation should be performed on a pointer based on jemalloc information.
 *
 * bin_info Pointer to the bin information structure.
 * bin_usage Pointer to the bin usage structure.
 * nalloced Number of allocated regions in the bin.
 *
 * return 1 if defragmentation should be performed, 0 otherwise.
 *
 * This function checks the following conditions to determine if defragmentation should be performed:
 * 1. If the number of allocated regions (nalloced) is equal to the total number of regions (bin_info->nregs),
 *    defragmentation is not necessary as moving regions is guaranteed not to change the fragmentation ratio.
 * 2. If the number of non-full slabs (bin_usage->curr_nonfull_slabs) is less than 2, defragmentation is not performed
 *    because there is no other slab to move regions to.
 * 3. If slab utilization < 'avg utilization'*1.125 [code 1.125 == (1000+UTILIZATION_THRESHOLD_FACTOR_MILI)/1000]
 *    than we should defrag. This is aligned with previous je_defrag_hint implementation.
 */
static inline int makeDefragDecision(jeBinInfo *bin_info, jemallocBinUsageData *bin_usage, unsigned long nalloced) {
    unsigned long curr_full_slabs = bin_usage->curr_slabs - bin_usage->curr_nonfull_slabs;
    size_t allocated_nonfull = bin_usage->curr_regs - curr_full_slabs * bin_info->nregs;
    if (bin_info->nregs == nalloced || bin_usage->curr_nonfull_slabs < 2 ||
        1000 * nalloced * bin_usage->curr_nonfull_slabs > (1000 + UTILIZATION_THRESHOLD_FACTOR_MILI) * allocated_nonfull) {
        return 0;
    }
    return 1;
}

/*
 * Performs defragmentation analysis for a given ptr.
 *
 * ptr - ptr to memory region to be analyzed.
 *
 * return - the function returns 1 if defrag should be performed, 0 otherwise.
 */
int allocatorShouldDefrag(void *ptr) {
    assert(defrag_supported);
    size_t out[BATCH_QUERY_ARGS_OUT];
    size_t out_sz = sizeof(out);
    size_t in_sz = sizeof(ptr);
    for (unsigned j = 0; j < BATCH_QUERY_ARGS_OUT; j++) {
        out[j] = -1;
    }
    je_mallctlbymib(je_cb.util_batch_query.key,
                    je_cb.util_batch_query.keylen,
                    out, &out_sz,
                    &ptr, in_sz);
    /* handle results with appropriate quantum value */
    assert(SLAB_NUM_REGS(out, 0) > 0);
    assert(SLAB_LEN(out, 0) > 0);
    assert(SLAB_NFREE(out, 0) != (size_t)-1);
    unsigned region_size = SLAB_LEN(out, 0) / SLAB_NUM_REGS(out, 0);
    /* check that the allocation size is in range of small bins */
    if (region_size > je_cb.bin_info[je_cb.nbins - 1].reg_size) {
        return 0;
    }
    /* get the index based on quantum used */
    unsigned binind = jeSize2BinIndexLgQ3(region_size);
    /* make sure binind is in range and reverse map is correct */
    assert(binind < je_cb.nbins && region_size == je_cb.bin_info[binind].reg_size);

    return makeDefragDecision(&je_cb.bin_info[binind],
                              &je_usage_info[binind],
                              je_cb.bin_info[binind].nregs - SLAB_NFREE(out, 0));
}

/* Utility function to get the fragmentation ratio from jemalloc.
 * It is critical to do that by comparing only heap maps that belong to
 * jemalloc, and skip ones the jemalloc keeps as spare. Since we use this
 * fragmentation ratio in order to decide if a defrag action should be taken
 * or not, a false detection can cause the defragmenter to waste a lot of CPU
 * without the possibility of getting any results. */
float getAllocatorFragmentation(size_t *out_frag_bytes) {
    size_t resident, active, allocated, frag_smallbins_bytes;
    zmalloc_get_allocator_info(&allocated, &active, &resident, NULL, NULL);
    frag_smallbins_bytes = allocatorDefragGetFragSmallbins();
    /* Calculate the fragmentation ratio as the proportion of wasted memory in small
     * bins (which are defraggable) relative to the total allocated memory (including large bins).
     * This is because otherwise, if most of the memory usage is large bins, we may show high percentage,
     * despite the fact it's not a lot of memory for the user. */
    float frag_pct = (float)frag_smallbins_bytes / allocated * 100;
    float rss_pct = ((float)resident / allocated) * 100 - 100;
    size_t rss_bytes = resident - allocated;
    if (out_frag_bytes) *out_frag_bytes = frag_smallbins_bytes;
    serverLog(LL_DEBUG, "allocated=%zu, active=%zu, resident=%zu, frag=%.2f%% (%.2f%% rss), frag_bytes=%zu (%zu rss)",
              allocated, active, resident, frag_pct, rss_pct, frag_smallbins_bytes, rss_bytes);
    return frag_pct;
}

#elif defined(DEBUG_FORCE_DEFRAG)
int allocatorDefragInit(void) {
    return 0;
}
void allocatorDefragFree(void *ptr, size_t size) {
    UNUSED(size);
    zfree(ptr);
}
__attribute__((malloc)) void *allocatorDefragAlloc(size_t size) {
    return zmalloc(size);
    return NULL;
}
unsigned long allocatorDefragGetFragSmallbins(void) {
    return 0;
}

int allocatorShouldDefrag(void *ptr) {
    UNUSED(ptr);
    return 1;
}

float getAllocatorFragmentation(size_t *out_frag_bytes) {
    *out_frag_bytes = server.active_defrag_ignore_bytes + 1;
    return server.active_defrag_threshold_upper;
}

#else
int allocatorDefragInit(void) {
    return -1;
}
void allocatorDefragFree(void *ptr, size_t size) {
    UNUSED(ptr);
    UNUSED(size);
}
__attribute__((malloc)) void *allocatorDefragAlloc(size_t size) {
    UNUSED(size);
    return NULL;
}
unsigned long allocatorDefragGetFragSmallbins(void) {
    return 0;
}

int allocatorShouldDefrag(void *ptr) {
    UNUSED(ptr);
    return 0;
}

float getAllocatorFragmentation(size_t *out_frag_bytes) {
    UNUSED(out_frag_bytes);
    return 0;
}
#endif
