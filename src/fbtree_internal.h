/* Internal header for fbtree — struct layouts and constants.
 * Include this ONLY in fbtree.c and its unit tests.
 * Production code outside the fbtree module should use the opaque public
 * API in fbtree.h. */

#ifndef FBTREE_INTERNAL_H
#define FBTREE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "fbtree.h"
#include "sds.h"

/* Architecture-specific constants for node sizing.
 * 64-bit: optimized for 8-byte pointers, targets 1792-byte innerNode (jemalloc size class)
 * 32-bit: uses same logical fanout, smaller nodes due to 4-byte pointers */
#define NODE_SIZE 61

#if SIZE_MAX == UINT64_MAX   /* 64-bit */
#define EMBED_PREFIX_LEN 254 /* Tuned so innerNode fits in 2048-byte jemalloc size class */
#elif SIZE_MAX == UINT32_MAX /* 32-bit */
#define EMBED_PREFIX_LEN 30  /* Tuned to fit innerNode exactly in 1024-byte jemalloc size class */
#endif

#define FEATURE_SIZE 4
#define FEATURE_ROW_SIZE 64 /* size of cache line */
/* SIMD comparison instructions (e.g., _mm256_cmpgt_epi8) operate on signed
 * bytes, but our feature bytes are unsigned [0,255]. XOR with 0x80 maps
 * unsigned order to signed order: 0→-128, 127→-1, 128→0, 255→127.
 * Features are stored pre-biased; search targets are biased at lookup time. */
#define FEATURE_BIAS 0x80
#define MAX_TREE_DEPTH 16 /* NODE_SIZE=61, so depth 6 handles >61^6 = ~51 billion elements */

/* Common header for all node types */
typedef struct {
    bool is_leaf;
    uint8_t num_items;
} node;

typedef struct {
    node header;
    char embedded_prefix[EMBED_PREFIX_LEN]; /* Short prefix inline; long prefix stores char* at aligned offset */
    size_t prefix_len;                      /* Common prefix length of this node's anchor values (NOT all subtree keys).
                                             * Features are the 4 bytes at offset prefix_len in each anchor, so the
                                             * prefix skips identical leading bytes to keep features discriminating.
                                             * Note: this can exceed a child's prefix_len — see updateCommonPrefix. */
    char features[FEATURE_SIZE][FEATURE_ROW_SIZE];
    sds anchors[NODE_SIZE]; /* High keys of children (rightmost value in each child's subtree) */
    node *children[NODE_SIZE];
    size_t child_sizes[NODE_SIZE];      /* subtree element counts for rank queries */
    uint8_t child_num_items[NODE_SIZE]; /* direct item count of each child */
} innerNode;

typedef struct leafNode {
    node header;
    struct leafNode *prev;
    struct leafNode *next;
    sds values[NODE_SIZE];
} leafNode;

struct fbtreeIndex {
    node *root;
    leafNode *leftmost_leaf;  /* Cache for fast-path prepend */
    leafNode *rightmost_leaf; /* Cache for fast-path append */
};

/* Architecture-specific size assertions */
#if SIZE_MAX == UINT64_MAX /* 64-bit */
static_assert(sizeof(innerNode) == 2048, "64-bit innerNode should fit in 2048-byte jemalloc size class");
static_assert(sizeof(leafNode) == 512, "64-bit leafNode should fit perfectly in jemalloc size class");
#elif SIZE_MAX == UINT32_MAX /* 32-bit */
/* NODE_SIZE is tuned for 64-bit. On 32-bit, structs are smaller per-slot but
 * NODE_SIZE stays the same, so they don't fill their size class optimally.
 * A 32-bit-specific NODE_SIZE (~90-100) would be needed to minimize waste. */
static_assert(sizeof(innerNode) <= 2048, "32-bit innerNode must fit in 2048-byte jemalloc size class");
static_assert(sizeof(leafNode) <= 512, "32-bit leafNode must fit in 512-byte jemalloc size class");
#endif
static_assert(NODE_SIZE <= FEATURE_ROW_SIZE, "NODE_SIZE must fit in feature row");

/* Long prefix: when prefix_len > EMBED_PREFIX_LEN, pointer stored at aligned offset within embedded_prefix. */
#define LONG_PREFIX_PTR_OFFSET \
    ((sizeof(void *) - (offsetof(innerNode, embedded_prefix) % sizeof(void *))) % sizeof(void *))
static_assert(EMBED_PREFIX_LEN >= LONG_PREFIX_PTR_OFFSET + sizeof(char *), "embedded_prefix must fit aligned pointer");

/* Debug functions — test-only, not part of the public API. */
/* Validate tree invariants. Returns true when consistent. On failure, when
 * errmsg is non-NULL, a message describing the first failed check is
 * written into it (truncated to errmsg_len). */
bool fbtreeDebugValidate(fbtreeIndex *fbt, bool verbose, char *errmsg, size_t errmsg_len);

#endif /* FBTREE_INTERNAL_H */
