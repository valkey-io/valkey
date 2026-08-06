/* Feature B-Tree: a cache-optimized B+tree for sorted binary strings.
 * This is a general-purpose data structure - zset-specific logic is in zset_fbtree_adapter.c */

#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include "config.h"
#include "fbtree.h"
#include "fbtree_internal.h"
#include "serverassert.h"
#include "zmalloc.h"
#include "sds.h"

#if HAVE_X86_SIMD
#include <immintrin.h>
#elif HAVE_ARM_NEON
#include <arm_neon.h>
#endif

typedef enum {
    ITER_AT_POSITION,  /* Positioned at a valid element */
    ITER_BEFORE_START, /* Positioned before first element (fbtreePrev returns NULL, fbtreeNext works) */
    ITER_PAST_END,     /* Positioned past last element (fbtreeNext returns NULL, fbtreePrev works) */
} IterState;

typedef struct {
    fbtreeIndex *fbt;
    leafNode *current_leaf;
    uint8_t current_index;
    uint8_t leaf_count;
    IterState state;
} iter;

static_assert(sizeof(fbtreeIterator) >= sizeof(iter), "Opaque iterator size check");

typedef struct {
    sds updated_anchor;  /* Pointer to updated anchor string if it's changed */
    node *new_node;      /* Pointer to new child node to insert (node split happened) */
    sds new_node_anchor; /* Pointer to new node's anchor string (node split happened) */
    sds inserted_item;   /* Pointer to the newly inserted item in the leaf */
} insertResult;

/* Hint for optimized tree traversal - allows skipping inner node searches */
typedef enum {
    HINT_NONE,     /* No hint - use normal search */
    HINT_LEFTMOST, /* Traverse to leftmost child at each level */
    HINT_RIGHTMOST /* Traverse to rightmost child at each level */
} TraversalHint;

typedef struct {
    sds updated_anchor;   /* Pointer to updated anchor string if it's changed */
    bool delete_executed; /* True if key was found and deleted, False if not found no-op */
} deleteResult;

/* Conversion from user-facing opaque iterator type to internal struct */
static inline iter *iteratorFromOpaque(fbtreeIterator *iterator) {
    return (iter *)(void *)iterator;
}

/* Get low_key (minimum) from leaf node - leaves are always kept sorted */
static sds leafNodeLowKey(leafNode *leaf) {
    return (leaf->header.num_items == 0) ? NULL : leaf->values[0];
}

/* Get high_key pointer from leaf node */
static sds leafNodeHighKey(leafNode *leaf) {
    return (leaf->header.num_items == 0) ? NULL : leaf->values[leaf->header.num_items - 1];
}

/* Get high_key (maximum anchor) from any node type */
static sds nodeHighKey(node *n) {
    if (n->is_leaf) {
        return leafNodeHighKey((leafNode *)n);
    }
    innerNode *inner = (innerNode *)n;
    return (inner->header.num_items == 0) ? NULL : inner->anchors[inner->header.num_items - 1];
}

/* Get feature byte j from string s, biased for SIMD signed comparison.
 * Returns 0 ^ FEATURE_BIAS if the string is shorter than prefix_len + j. */
static char getFeatureByte(const_sds s, size_t prefix_len, int j) {
    size_t idx = prefix_len + j;
    unsigned char raw = (idx < sdslen(s)) ? (unsigned char)s[idx] : 0;
    return (char)(raw ^ FEATURE_BIAS);
}


static inline bool innerNodeHasLongPrefix(innerNode *inner) {
    return inner->prefix_len > EMBED_PREFIX_LEN;
}

static inline const char *innerNodeGetPrefix(innerNode *inner) {
    if (innerNodeHasLongPrefix(inner)) {
        char **ptr = (char **)&inner->embedded_prefix[LONG_PREFIX_PTR_OFFSET];
        return *ptr;
    }
    return inner->embedded_prefix;
}

static void innerNodeFreePrefix(innerNode *inner) {
    if (innerNodeHasLongPrefix(inner)) {
        char **ptr = (char **)&inner->embedded_prefix[LONG_PREFIX_PTR_OFFSET];
        zfree(*ptr);
    }
}

static void innerNodeSetPrefix(innerNode *inner, const char *data, size_t len) {
    innerNodeFreePrefix(inner);
    inner->prefix_len = len;
    if (len > EMBED_PREFIX_LEN) {
        char **ptr = (char **)&inner->embedded_prefix[LONG_PREFIX_PTR_OFFSET];
        *ptr = zmalloc(len);
        memcpy(*ptr, data, len);
    } else if (len > 0) {
        memcpy(inner->embedded_prefix, data, len);
    }
}

static innerNode *innerNodeCreate(void) {
    /* zcalloc zeroes all fields — is_leaf defaults to false (0). */
    return zcalloc(sizeof(innerNode));
}

static leafNode *leafNodeCreate(void) {
    leafNode *node = zcalloc(sizeof(*node));
    node->header.is_leaf = true;
    return node;
}

static leafNode *leafNodeCreateWithItem(sds item) {
    leafNode *leaf = leafNodeCreate();
    leaf->values[0] = item;
    leaf->header.num_items = 1;
    return leaf;
}

fbtreeIndex *fbtreeCreate(void) {
    fbtreeIndex *fbt = zmalloc(sizeof(*fbt));
    fbt->root = NULL;
    fbt->leftmost_leaf = NULL;
    fbt->rightmost_leaf = NULL;
    return fbt;
}

/* Callback invoked for each item being deleted, before sdsfree.
 * Allows callers to perform side effects (e.g., hashtable removal). */
typedef void (*fbtreeItemCallback)(sds item, void *ctx);

static void freeNodeRecursive(node *n, fbtreeItemCallback callback, void *ctx) {
    if (!n) return;

    if (n->is_leaf) {
        leafNode *leaf = (leafNode *)n;
        for (int i = 0; i < leaf->header.num_items; i++) {
            if (callback) callback(leaf->values[i], ctx);
            sdsfree(leaf->values[i]);
        }
        zfree(leaf);
    } else {
        innerNode *inner = (innerNode *)n;
        innerNodeFreePrefix(inner);
        for (int i = 0; i < inner->header.num_items; i++) {
            if (inner->children[i] != NULL) {
                freeNodeRecursive(inner->children[i], callback, ctx);
            }
        }
        zfree(inner);
    }
}

/* Free all nodes and reset to empty. If callback is non-NULL, it is invoked
 * for each item before freeing. */
static void fbtreeDeleteAll(fbtreeIndex *fbt, fbtreeItemCallback callback, void *callback_ctx) {
    freeNodeRecursive(fbt->root, callback, callback_ctx);
    fbt->root = NULL;
    fbt->leftmost_leaf = NULL;
    fbt->rightmost_leaf = NULL;
}

void fbtreeEmpty(fbtreeIndex *fbt) {
    fbtreeDeleteAll(fbt, NULL, NULL);
}

void fbtreeFree(fbtreeIndex *fbt) {
    fbtreeEmpty(fbt);
    zfree(fbt);
}

static void recomputeFeatures(innerNode *inner) {
    for (int i = 0; i < inner->header.num_items; i++) {
        assert(sdslen(inner->anchors[i]) >= inner->prefix_len);
        for (int j = 0; j < FEATURE_SIZE; j++)
            inner->features[j][i] = getFeatureByte(inner->anchors[i], inner->prefix_len, j);
    }
}

/* Copy count children (anchors, child pointers, child_sizes, child_num_items,
 * features) from src starting at src_idx to dst starting at dst_idx.
 * Regions must not overlap — use innerNodeMoveChildren for overlapping shifts. */
static inline void innerNodeCopyChildren(innerNode *dst, int dst_idx, innerNode *src, int src_idx, int count) {
    for (int j = 0; j < FEATURE_SIZE; j++)
        memcpy(&dst->features[j][dst_idx], &src->features[j][src_idx], count);
    memcpy(&dst->anchors[dst_idx], &src->anchors[src_idx], count * sizeof(dst->anchors[0]));
    memcpy(&dst->children[dst_idx], &src->children[src_idx], count * sizeof(dst->children[0]));
    memcpy(&dst->child_sizes[dst_idx], &src->child_sizes[src_idx], count * sizeof(dst->child_sizes[0]));
    memcpy(&dst->child_num_items[dst_idx], &src->child_num_items[src_idx], count * sizeof(dst->child_num_items[0]));
}

/* Move count children within the same node from src_idx to dst_idx.
 * Handles overlapping regions (for insert/remove shifts). */
static inline void innerNodeMoveChildren(innerNode *node, int dst_idx, int src_idx, int count) {
    for (int j = 0; j < FEATURE_SIZE; j++)
        memmove(&node->features[j][dst_idx], &node->features[j][src_idx], count);
    memmove(&node->anchors[dst_idx], &node->anchors[src_idx], count * sizeof(node->anchors[0]));
    memmove(&node->children[dst_idx], &node->children[src_idx], count * sizeof(node->children[0]));
    memmove(&node->child_sizes[dst_idx], &node->child_sizes[src_idx], count * sizeof(node->child_sizes[0]));
    memmove(&node->child_num_items[dst_idx], &node->child_num_items[src_idx], count * sizeof(node->child_num_items[0]));
}

static bool updateCommonPrefix(innerNode *inner) {
    if (inner->header.num_items < 2) return false;

    /* Compute the common prefix of this node's first and last anchors.
     * Anchors are high keys of children, so this is the common prefix among
     * anchor values — NOT the common prefix of all keys in the subtree.
     *
     * This means prefix_len can exceed a child's prefix_len. The leftmost
     * child's key range extends below its high key (anchor), so its own
     * anchors may diverge earlier than the parent's anchors do. Example:
     * parent anchors "60_100" and "60_200" share prefix "60_" (len=3), but
     * child[0] contains keys "59_999" through "60_100", so child[0]'s
     * anchors only share prefix "" (len=0).
     *
     * This is handled correctly by findChildIndex: when a lookup key is
     * less than the prefix, it returns child index 0 (leftmost).  */
    const_sds first_anchor = inner->anchors[0];
    const_sds last_anchor = inner->anchors[inner->header.num_items - 1];
    size_t first_len = sdslen(first_anchor);
    size_t last_len = sdslen(last_anchor);
    size_t max_len = first_len < last_len ? first_len : last_len;
    size_t len = 0;
    while (len < max_len && first_anchor[len] == last_anchor[len]) len++;

    bool prefix_changed = (len != inner->prefix_len) ||
                          (len > 0 && memcmp(innerNodeGetPrefix(inner), first_anchor, len) != 0);
    if (prefix_changed) {
        innerNodeSetPrefix(inner, first_anchor, len);
        recomputeFeatures(inner);
        return true;
    }
    return false;
}

static size_t getSubtreeSize(node *n) {
    if (n->is_leaf) {
        return n->num_items;
    } else {
        innerNode *inner = (innerNode *)n;
        size_t total = 0;
        for (int i = 0; i < inner->header.num_items; i++) {
            total += inner->child_sizes[i];
        }
        return total;
    }
}

/* Insert a child into an inner node in sorted order. Returns true if parent's anchor/feature needs to be updated */
static bool innerNodeInsert(innerNode *parent, const node *child, sds child_anchor, size_t insert_index) {
    assert(parent->header.num_items < NODE_SIZE);

    /* shift higher elements to make space */
    size_t num_to_move = parent->header.num_items - insert_index;
    if (num_to_move > 0) {
        innerNodeMoveChildren(parent, insert_index + 1, insert_index, num_to_move);
    }

    /* insert child - features stored pre-biased for SIMD comparison */
    for (int j = 0; j < FEATURE_SIZE; j++)
        parent->features[j][insert_index] = getFeatureByte(child_anchor, parent->prefix_len, j);
    parent->anchors[insert_index] = child_anchor;
    parent->children[insert_index] = (node *)child;
    parent->child_sizes[insert_index] = getSubtreeSize((node *)child);
    parent->child_num_items[insert_index] = ((node *)child)->num_items;
    parent->header.num_items++;

    /* update prefix - might need to initialize, or common length could become shorter */
    if (insert_index == 0 || insert_index + 1 == parent->header.num_items) {
        updateCommonPrefix(parent);
    }

    bool anchor_changed = (num_to_move == 0);
    return anchor_changed;
}

/* Remove child at given index from inner node. Caller must free the child. */
static void innerNodeRemoveChild(innerNode *parent, int index) {
    assert(index < parent->header.num_items);
    int num_to_move = parent->header.num_items - index - 1;
    if (num_to_move > 0) {
        innerNodeMoveChildren(parent, index, index + 1, num_to_move);
    }
    parent->header.num_items--;
    if (index == 0 || index == parent->header.num_items) {
        updateCommonPrefix(parent);
    }
}

/* Remove children at indices [start_idx, end_idx] inclusive from inner node.
 * Caller must free the removed children. Does NOT update prefix/features
 * since the caller may be doing further modifications. */
static void innerNodeRemoveChildrenRange(innerNode *parent, int start_idx, int end_idx) {
    assert(start_idx >= 0 && end_idx < parent->header.num_items && start_idx <= end_idx);
    int remove_count = end_idx - start_idx + 1;
    int num_to_move = parent->header.num_items - end_idx - 1;
    if (num_to_move > 0) {
        innerNodeMoveChildren(parent, start_idx, end_idx + 1, num_to_move);
    }
    parent->header.num_items -= remove_count;
}

/* Refresh a child's cached metadata in the parent: child_sizes, anchor,
 * features, child_num_items. Call after the child's contents changed (e.g.,
 * after truncation). Does NOT update common prefix. */
static void innerNodeRefreshChildMeta(innerNode *parent, int index) {
    parent->child_sizes[index] = getSubtreeSize(parent->children[index]);
    parent->child_num_items[index] = parent->children[index]->num_items;
    sds anchor = nodeHighKey(parent->children[index]);
    parent->anchors[index] = anchor;
    for (int j = 0; j < FEATURE_SIZE; j++)
        parent->features[j][index] = getFeatureByte(anchor, parent->prefix_len, j);
}

/* TODO: Support asymmetric inner node splits for append/prepend (hint-gated),
 * matching what leafNodeSplit does. Sequential loads would achieve ~100% inner
 * node load factor instead of ~50%. Requires passing TraversalHint through. */
static innerNode *innerNodeSplit(innerNode *left_node) {
    assert(left_node->header.num_items == NODE_SIZE);
    innerNode *right_node = innerNodeCreate();

    /* move higher half of elements to right node */
    const size_t num_left_keys = NODE_SIZE / 2;
    const size_t num_right_keys = NODE_SIZE - num_left_keys;

    innerNodeCopyChildren(right_node, 0, left_node, num_left_keys, num_right_keys);

    right_node->header.num_items = num_right_keys;
    left_node->header.num_items = num_left_keys;

    /* each covers a smaller range of the dataset, so prefix could be longer now */
    /* Copy prefix from left to right before updateCommonPrefix potentially changes it */
    innerNodeSetPrefix(right_node, innerNodeGetPrefix(left_node), left_node->prefix_len); // TODO: only copy size of prefix? Optimize to avoid copy maybe?
    updateCommonPrefix(right_node);
    updateCommonPrefix(left_node);
    return right_node;
}

static int leafNodeBinarySearch(leafNode *leaf, const_sds string) {
    int left = 0, right = leaf->header.num_items;
    while (left < right) {
        int mid = (left + right) / 2;
        if (sdscmp(leaf->values[mid], string) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

static insertResult leafNodeInsert(leafNode *leaf, sds string) {
    assert(leaf->header.num_items < NODE_SIZE);

    int insert_index = leafNodeBinarySearch(leaf, string);
    int count = leaf->header.num_items;

    insertResult result = {
        .inserted_item = string,
        .updated_anchor = (insert_index == count) ? string : NULL};

    /* Shift elements right to make space */
    memmove(&leaf->values[insert_index + 1], &leaf->values[insert_index],
            (count - insert_index) * sizeof(sds));

    /* Insert at position - take ownership */
    leaf->values[insert_index] = string;
    leaf->header.num_items++;

    return result;
}

/* Helper to link a new right leaf after an existing left leaf */
static void linkLeafRight(leafNode *left, leafNode *right) {
    right->prev = left;
    right->next = left->next;
    left->next = right;
    if (right->next) right->next->prev = right;
}

/* Unlink a leaf from the doubly-linked list */
static void unlinkLeaf(leafNode *leaf) {
    if (leaf->prev) leaf->prev->next = leaf->next;
    if (leaf->next) leaf->next->prev = leaf->prev;
}

/* Free a node that has been emptied by deletion: unlink leaves from the leaf
 * chain, release any spilled prefix buffer on inner nodes, then free the
 * node itself. */
static void freeEmptyNode(node *n) {
    if (n->is_leaf)
        unlinkLeaf((leafNode *)n);
    else
        innerNodeFreePrefix((innerNode *)n);
    zfree(n);
}

/* Variant for emptied nodes whose leaves have already been removed from the
 * leaf chain (range deletion splices the chain before trimming boundary
 * subtrees): only the spilled prefix release applies. */
static void freeEmptyNodeAlreadyUnlinked(node *n) {
    if (!n->is_leaf) innerNodeFreePrefix((innerNode *)n);
    zfree(n);
}

static insertResult leafNodeSplit(leafNode *left_leaf, sds string, TraversalHint hint) {
    assert(left_leaf->header.num_items == NODE_SIZE);

    /* Binary search to find insertion point - reuse for pattern detection */
    int insert_index = leafNodeBinarySearch(left_leaf, string);

    if (insert_index == NODE_SIZE && hint == HINT_RIGHTMOST) {
        /* Append at tree edge: create new node with just the new item, left unchanged */
        leafNode *right_leaf = leafNodeCreateWithItem(string);
        linkLeafRight(left_leaf, right_leaf);
        return (insertResult){
            .new_node = (node *)right_leaf,
            .new_node_anchor = string,
            .inserted_item = right_leaf->values[0]};
    }

    if (insert_index == 0 && hint == HINT_LEFTMOST) {
        /* Prepend at tree edge: move all items to new right node, left gets just new item */
        leafNode *right_leaf = leafNodeCreate();
        memcpy(right_leaf->values, left_leaf->values, NODE_SIZE * sizeof(sds));
        right_leaf->header.num_items = NODE_SIZE;
        left_leaf->values[0] = string;
        left_leaf->header.num_items = 1;
        linkLeafRight(left_leaf, right_leaf);
        return (insertResult){
            .updated_anchor = string,
            .new_node = (node *)right_leaf,
            .new_node_anchor = leafNodeHighKey(right_leaf),
            .inserted_item = left_leaf->values[0]};
    }

    /* Middle insert: standard 50/50 split */
    leafNode *right_leaf = leafNodeCreate();
    size_t num_left = NODE_SIZE / 2;
    size_t num_right = NODE_SIZE - num_left;

    memcpy(right_leaf->values, &left_leaf->values[num_left], num_right * sizeof(sds));
    left_leaf->header.num_items = num_left;
    right_leaf->header.num_items = num_right;
    linkLeafRight(left_leaf, right_leaf);

    /* Insert into appropriate leaf */
    insertResult leaf_result = (insert_index <= (int)num_left)
                                   ? leafNodeInsert(left_leaf, string)
                                   : leafNodeInsert(right_leaf, string);

    return (insertResult){
        .updated_anchor = leafNodeHighKey(left_leaf),
        .new_node = (node *)right_leaf,
        .new_node_anchor = leafNodeHighKey(right_leaf),
        .inserted_item = leaf_result.inserted_item};
}

/* Scalar implementation - always available for testing */
static void featureSearchSIMD_scalar(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                     int num_keys,
                                     const unsigned char target[FEATURE_SIZE],
                                     int *out_left,
                                     int *out_right) {
    int left = 0, right = num_keys;

    for (int i = 0; i < num_keys; i++) {
        int cmp = 0;
        for (int row = 0; row < FEATURE_SIZE && cmp == 0; row++) {
            signed char target_biased = (signed char)(target[row] ^ FEATURE_BIAS);
            cmp = target_biased - (signed char)features[row][i];
        }
        if (cmp <= 0) {
            left = i;
            break;
        }
        left = i + 1;
    }

    right = left;
    for (int i = left; i < num_keys; i++) {
        int cmp = 0;
        for (int row = 0; row < FEATURE_SIZE && cmp == 0; row++) {
            signed char target_biased = (signed char)(target[row] ^ FEATURE_BIAS);
            cmp = target_biased - (signed char)features[row][i];
        }
        if (cmp < 0) break;
        right = i + 1;
    }

    *out_left = left;
    *out_right = right;
}

/* SIMD feature search: finds range [out_left, out_right) of keys matching target.
 * Features are stored pre-biased (XOR'd with 0x80), so we only bias the target.
 * Bitmasks track candidates (ge_mask: target >= key, le_mask: target <= key). */
#if HAVE_X86_SIMD

ATTRIBUTE_TARGET_AVX2
static void featureSearchSIMD_avx2(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                   int num_keys,
                                   const unsigned char target[FEATURE_SIZE],
                                   int *out_left,
                                   int *out_right) {
    uint64_t valid_mask = (num_keys >= 64) ? ~0ULL : (1ULL << num_keys) - 1;
    uint64_t ge_mask = valid_mask;
    uint64_t le_mask = valid_mask;

    for (int row = 0; row < FEATURE_SIZE && (ge_mask || le_mask); row++) {
        /* Bias target to match pre-biased features */
        __m256i target_biased = _mm256_set1_epi8((char)(target[row] ^ FEATURE_BIAS));
        uint64_t gt_this = 0, lt_this = 0;

        /* Process 64 feature bytes in 2x32-byte chunks */
        for (int chunk = 0; chunk < 2; chunk++) {
            __m256i feat = _mm256_loadu_si256((const __m256i *)&features[row][chunk * 32]);
            __m256i gt = _mm256_cmpgt_epi8(target_biased, feat);
            __m256i lt = _mm256_cmpgt_epi8(feat, target_biased);
            gt_this |= (uint64_t)(uint32_t)_mm256_movemask_epi8(gt) << (chunk * 32);
            lt_this |= (uint64_t)(uint32_t)_mm256_movemask_epi8(lt) << (chunk * 32);
        }

        /* Narrow candidate set: eliminate keys where comparison is decided */
        uint64_t undecided = ge_mask & le_mask;
        ge_mask &= ~(lt_this & undecided);
        le_mask &= ~(gt_this & undecided);
    }

    *out_left = le_mask ? __builtin_ctzll(le_mask) : num_keys;
    *out_right = (le_mask & ~ge_mask) ? __builtin_ctzll(le_mask & ~ge_mask) : num_keys;
}

ATTRIBUTE_TARGET_SSE2
static void featureSearchSIMD_sse2(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                   int num_keys,
                                   const unsigned char target[FEATURE_SIZE],
                                   int *out_left,
                                   int *out_right) {
    uint64_t valid_mask = (num_keys >= 64) ? ~0ULL : (1ULL << num_keys) - 1;
    uint64_t ge_mask = valid_mask;
    uint64_t le_mask = valid_mask;

    for (int row = 0; row < FEATURE_SIZE && (ge_mask || le_mask); row++) {
        /* Bias target to match pre-biased features */
        __m128i target_biased = _mm_set1_epi8((char)(target[row] ^ FEATURE_BIAS));
        uint64_t gt_this = 0, lt_this = 0;

        /* Process 64 feature bytes in 4x16-byte chunks */
        for (int chunk = 0; chunk < 4; chunk++) {
            __m128i feat = _mm_loadu_si128((const __m128i *)&features[row][chunk * 16]);
            __m128i gt = _mm_cmpgt_epi8(target_biased, feat);
            __m128i lt = _mm_cmplt_epi8(target_biased, feat);
            gt_this |= (uint64_t)(uint16_t)_mm_movemask_epi8(gt) << (chunk * 16);
            lt_this |= (uint64_t)(uint16_t)_mm_movemask_epi8(lt) << (chunk * 16);
        }

        uint64_t undecided = ge_mask & le_mask;
        ge_mask &= ~(lt_this & undecided);
        le_mask &= ~(gt_this & undecided);
    }

    *out_left = le_mask ? __builtin_ctzll(le_mask) : num_keys;
    *out_right = (le_mask & ~ge_mask) ? __builtin_ctzll(le_mask & ~ge_mask) : num_keys;
}

static void featureSearchSIMD(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                              int num_keys,
                              const unsigned char target[FEATURE_SIZE],
                              int *out_left,
                              int *out_right) {
    if (__builtin_cpu_supports("avx2")) {
        featureSearchSIMD_avx2(features, num_keys, target, out_left, out_right);
    } else {
        featureSearchSIMD_sse2(features, num_keys, target, out_left, out_right);
    }
}

#elif HAVE_ARM_NEON

/* Convert 16-byte NEON comparison result to 16-bit mask (one bit per byte). */
static uint16_t neon_movemask_16(uint8x16_t v) {
    /* Position bits: byte i contributes to bit i of result */
    static const uint8x16_t shift_amt = {0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7};
    uint8x16_t masked = vshrq_n_u8(v, 7); /* isolate high bit: 0x01 or 0x00 */
    uint8x16_t shifted = vshlq_u8(masked, vreinterpretq_s8_u8(shift_amt));
    /* Sum each half to get one byte with 8 bits packed */
    uint8_t lo = vaddv_u8(vget_low_u8(shifted));
    uint8_t hi = vaddv_u8(vget_high_u8(shifted));
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

/* Convert 4x16-byte NEON vectors to 64-bit mask (one bit per byte). */
static uint64_t neon_movemask_64(uint8x16_t v0, uint8x16_t v1, uint8x16_t v2, uint8x16_t v3) {
    return (uint64_t)neon_movemask_16(v0) |
           ((uint64_t)neon_movemask_16(v1) << 16) |
           ((uint64_t)neon_movemask_16(v2) << 32) |
           ((uint64_t)neon_movemask_16(v3) << 48);
}

static void featureSearchSIMD(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                              int num_keys,
                              const unsigned char target[FEATURE_SIZE],
                              int *out_left,
                              int *out_right) {
    /* Index vector for validity mask generation */
    static const uint8x16_t neon_indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    /* ge[chunk] tracks positions where target >= feature (not yet proven less)
     * le[chunk] tracks positions where target <= feature (not yet proven greater)
     * Initialize both with validity mask (0xFF = valid, 0x00 = invalid) */
    uint8x16_t ge[4], le[4];
    int full_chunks = num_keys / 16;
    for (int chunk = 0; chunk < full_chunks; chunk++)
        ge[chunk] = le[chunk] = vdupq_n_u8(0xFF);
    for (int chunk = full_chunks; chunk < 4; chunk++) {
        int remaining = num_keys - chunk * 16;
        if (remaining <= 0)
            ge[chunk] = le[chunk] = vdupq_n_u8(0x00);
        else
            ge[chunk] = le[chunk] = vcltq_u8(neon_indices, vdupq_n_u8((uint8_t)remaining));
    }

    for (int row = 0; row < FEATURE_SIZE; row++) {
        int8x16_t target_biased = vdupq_n_s8((int8_t)(target[row] ^ FEATURE_BIAS));

        for (int chunk = 0; chunk < 4; chunk++) {
            int8x16_t feat = vld1q_s8((const int8_t *)&features[row][chunk * 16]);
            uint8x16_t gt = vcgtq_s8(target_biased, feat); /* target > feature */
            uint8x16_t lt = vcltq_s8(target_biased, feat); /* target < feature */

            /* undecided = positions where both ge and le are still set */
            uint8x16_t undecided = vandq_u8(ge[chunk], le[chunk]);

            /* ge &= ~(lt & undecided): if target < feature and was undecided, target is not >= */
            ge[chunk] = vbicq_u8(ge[chunk], vandq_u8(lt, undecided));

            /* le &= ~(gt & undecided): if target > feature and was undecided, target is not <= */
            le[chunk] = vbicq_u8(le[chunk], vandq_u8(gt, undecided));
        }
    }

    /* Extract final bitmasks */
    uint64_t le_mask = neon_movemask_64(le[0], le[1], le[2], le[3]);
    uint64_t ge_mask = neon_movemask_64(ge[0], ge[1], ge[2], ge[3]);

    *out_left = le_mask ? __builtin_ctzll(le_mask) : num_keys;
    *out_right = (le_mask & ~ge_mask) ? __builtin_ctzll(le_mask & ~ge_mask) : num_keys;
}

#else

static void featureSearchSIMD(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                              int num_keys,
                              const unsigned char target[FEATURE_SIZE],
                              int *out_left,
                              int *out_right) {
    featureSearchSIMD_scalar(features, num_keys, target, out_left, out_right);
}
#endif /* HAVE_X86_SIMD */

/* Test wrappers to expose static functions for unit testing */
void featureSearchSIMD_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                    int num_keys,
                                    const unsigned char target[FEATURE_SIZE],
                                    int *out_left,
                                    int *out_right) {
    featureSearchSIMD(features, num_keys, target, out_left, out_right);
}

#if HAVE_X86_SIMD
void featureSearchSIMD_avx2_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                         int num_keys,
                                         const unsigned char target[FEATURE_SIZE],
                                         int *out_left,
                                         int *out_right) {
    featureSearchSIMD_avx2(features, num_keys, target, out_left, out_right);
}

void featureSearchSIMD_sse2_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                         int num_keys,
                                         const unsigned char target[FEATURE_SIZE],
                                         int *out_left,
                                         int *out_right) {
    featureSearchSIMD_sse2(features, num_keys, target, out_left, out_right);
}
#endif

void featureSearchSIMD_scalar_test_wrapper(char features[FEATURE_SIZE][FEATURE_ROW_SIZE],
                                           int num_keys,
                                           const unsigned char target[FEATURE_SIZE],
                                           int *out_left,
                                           int *out_right) {
    featureSearchSIMD_scalar(features, num_keys, target, out_left, out_right);
}

/* Find child index for insertion using feature vectors and anchors.
 * Two-pass approach:
 * 1. SIMD pass: narrow candidates using feature comparison
 * 2. Anchor pass: binary search on anchors within narrowed range (only if needed)
 *
 * Compares the full prefix at each node. If the key falls outside the prefix
 * range (less than all anchors or greater than all anchors), short-circuits
 * to child 0 or num_items without proceeding to features/anchors. */
static int findChildIndex(innerNode *inner, const_sds string) {
    if (inner->prefix_len > 0) {
        const char *prefix = innerNodeGetPrefix(inner);
        size_t slen = sdslen(string);
        size_t cmp_len = inner->prefix_len < slen ? inner->prefix_len : slen;
        int cmp = memcmp(string, prefix, cmp_len);
        if (cmp == 0 && slen < inner->prefix_len) cmp = -1; /* string shorter than prefix */
        if (cmp < 0) return 0;
        if (cmp > 0) return inner->header.num_items;
    }

    /* Extract target feature bytes */
    unsigned char target[FEATURE_SIZE];
    size_t slen = sdslen(string);
    for (int j = 0; j < FEATURE_SIZE; j++) {
        size_t idx = inner->prefix_len + j;
        target[j] = (idx < slen) ? (unsigned char)string[idx] : 0;
    }

    /* Pass 1: SIMD feature search to narrow range */
    int left, right;
    featureSearchSIMD(inner->features, inner->header.num_items, target, &left, &right);

    /* Pass 2: Binary search on anchors within narrowed range (handles collisions) */
    while (left < right) {
        int mid = (left + right) / 2;
        int cmp = sdscmp(string, inner->anchors[mid]);
        if (cmp <= 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    return left;
}

static insertResult innerNodeHandleChildSplit(innerNode *parent, node *new_child, sds new_child_anchor, size_t new_child_idx) {
    if (parent->header.num_items == NODE_SIZE) {
        /* We're full - need to split */
        innerNode *new_right_parent = innerNodeSplit(parent);

        innerNode *insert_node = parent;
        bool insert_in_right_parent = new_child_idx > parent->header.num_items;
        if (insert_in_right_parent) {
            insert_node = new_right_parent;
            new_child_idx -= parent->header.num_items;
        }

        innerNodeInsert(insert_node, new_child, new_child_anchor, new_child_idx);
        insertResult result = {
            .updated_anchor = parent->anchors[parent->header.num_items - 1],
            .new_node = (node *)new_right_parent,
            .new_node_anchor = new_right_parent->anchors[new_right_parent->header.num_items - 1]};
        return result;
    } else {
        /* We're not full - just insert new child */
        bool anchor_changed = innerNodeInsert(parent, new_child, new_child_anchor, new_child_idx);
        insertResult result = {
            .updated_anchor = anchor_changed ? new_child_anchor : NULL,
        };
        return result;
    }
}

static insertResult subtreeInsert(node *n, sds string, TraversalHint hint) {
    assert(n);
    if (n->is_leaf) {
        leafNode *leaf = (leafNode *)n;
        if (leaf->header.num_items == NODE_SIZE) {
            return leafNodeSplit(leaf, string, hint);
        } else {
            return leafNodeInsert(leaf, string);
        }
    } else {
        /* inner node - find correct child for insert */
        innerNode *parent = (innerNode *)n;
        assert(parent->header.num_items > 0);

        /* Use hint to skip search when possible */
        int child_idx;
        if (hint == HINT_RIGHTMOST) {
            child_idx = parent->header.num_items - 1;
        } else if (hint == HINT_LEFTMOST) {
            child_idx = 0;
        } else {
            child_idx = findChildIndex(parent, string);
            if (child_idx == parent->header.num_items) child_idx--;
        }

        insertResult child_insert_result = subtreeInsert(parent->children[child_idx], string, hint);

        if (child_insert_result.updated_anchor) {
            parent->anchors[child_idx] = child_insert_result.updated_anchor;
            /* Anchor changed - prefix may need to shrink if this is first or last child */
            if (child_idx == 0 || child_idx == parent->header.num_items - 1) {
                updateCommonPrefix(parent);
            }
            for (int j = 0; j < FEATURE_SIZE; j++)
                parent->features[j][child_idx] = getFeatureByte(child_insert_result.updated_anchor, parent->prefix_len, j);
        }

        if (child_insert_result.new_node) {
            /* Our child split - recalculate original child's size since it lost elements */
            parent->child_sizes[child_idx] = getSubtreeSize(parent->children[child_idx]);
            parent->child_num_items[child_idx] = parent->children[child_idx]->num_items;
            /* Our child split, and we need to insert the new child just after the existing one */
            insertResult result = innerNodeHandleChildSplit(parent, child_insert_result.new_node, child_insert_result.new_node_anchor, child_idx + 1);
            result.inserted_item = child_insert_result.inserted_item;
            return result;
        } else {
            /* No split - just increment size for the inserted element */
            parent->child_sizes[child_idx]++;
            parent->child_num_items[child_idx] = parent->children[child_idx]->num_items;
            /* subtree root did not split, so no new child node to deal with */
            bool parent_anchor_changed = (child_idx == parent->header.num_items - 1);
            insertResult result = {
                .updated_anchor = parent_anchor_changed ? parent->anchors[child_idx] : NULL,
                .inserted_item = child_insert_result.inserted_item,
            };
            return result;
        }
    }
}

sds fbtreeInsert(fbtreeIndex *fbt, sds string) {
    if (fbt->root == NULL) {
        leafNode *leaf = leafNodeCreateWithItem(string);
        fbt->root = (node *)leaf;
        fbt->leftmost_leaf = leaf;
        fbt->rightmost_leaf = leaf;
        return leaf->values[0];
    }

    /* Detect append/prepend patterns for optimized insert path */
    TraversalHint hint = HINT_NONE;
    leafNode *rightmost = fbt->rightmost_leaf;
    leafNode *leftmost = fbt->leftmost_leaf;

    if (rightmost && sdscmp(string, leafNodeHighKey(rightmost)) > 0) {
        hint = HINT_RIGHTMOST;
    } else if (leftmost && sdscmp(string, leafNodeLowKey(leftmost)) < 0) {
        hint = HINT_LEFTMOST;
    }

    /* Insert with hint - skips inner node searches for append/prepend */
    insertResult result = subtreeInsert(fbt->root, string, hint);

    if (result.new_node) {
        innerNode *new_root = innerNodeCreate();
        sds left_anchor = result.updated_anchor;
        if (!left_anchor) {
            left_anchor = nodeHighKey(fbt->root);
        }
        innerNodeInsert(new_root, fbt->root, left_anchor, 0);
        innerNodeInsert(new_root, result.new_node, result.new_node_anchor, 1);
        fbt->root = (node *)new_root;
    }

    /* Update leaf caches using linked list - O(1) */
    if (leftmost && leftmost->prev) {
        fbt->leftmost_leaf = leftmost->prev;
    }
    if (rightmost && rightmost->next) {
        fbt->rightmost_leaf = rightmost->next;
    }
    return result.inserted_item;
}

/* Remove item from leaf without freeing it. Returns the removed item. */
static sds leafNodeRemoveAt(leafNode *leaf, int delete_index) {
    assert(delete_index >= 0 && delete_index < leaf->header.num_items);
    sds item = leaf->values[delete_index];
    leaf->header.num_items--;
    memmove(&leaf->values[delete_index], &leaf->values[delete_index + 1],
            (leaf->header.num_items - delete_index) * sizeof(sds));
    return item;
}

/* Remove and free values at indices [start_idx, end_idx] inclusive from leaf.
 * Returns the number of items removed. */
static int leafNodeRemoveRange(leafNode *leaf, int start_idx, int end_idx) {
    assert(start_idx >= 0 && end_idx < leaf->header.num_items && start_idx <= end_idx);
    int remove_count = end_idx - start_idx + 1;

    for (int i = start_idx; i <= end_idx; i++) {
        sdsfree(leaf->values[i]);
    }

    int num_to_move = leaf->header.num_items - end_idx - 1;
    if (num_to_move > 0) {
        memmove(&leaf->values[start_idx], &leaf->values[end_idx + 1], num_to_move * sizeof(sds));
    }
    leaf->header.num_items -= remove_count;
    return remove_count;
}


static deleteResult leafNodeDelete(fbtreeIndex *fbt, leafNode *leaf, const_sds item) {
    assert(leaf->header.num_items > 0);

    /* Find item by pointer comparison */
    int delete_index = -1;
    for (int i = 0; i < leaf->header.num_items; i++) {
        if (leaf->values[i] == item) {
            delete_index = i;
            break;
        }
    }
    if (delete_index < 0) return (deleteResult){0};

    /* Update leaf caches before delete if this leaf will become empty.
     * Done here while leaf is hot in cache to avoid extra fetches. */
    if (leaf->header.num_items == 1) {
        if (leaf == fbt->leftmost_leaf) fbt->leftmost_leaf = leaf->next;
        if (leaf == fbt->rightmost_leaf) fbt->rightmost_leaf = leaf->prev;
    }

    sdsfree(leafNodeRemoveAt(leaf, delete_index));

    return (deleteResult){
        .delete_executed = true,
        .updated_anchor = (delete_index == leaf->header.num_items) ? leafNodeHighKey(leaf) : NULL};
}

static deleteResult subtreeDeleteItem(fbtreeIndex *fbt, node *n, const_sds item) {
    if (n->is_leaf)
        return leafNodeDelete(fbt, (leafNode *)n, item);

    innerNode *inner = (innerNode *)n;
    int index = findChildIndex(inner, item);
    if (index == inner->header.num_items) return (deleteResult){0};

    deleteResult child_result = subtreeDeleteItem(fbt, inner->children[index], item);
    /* Items are matched by pointer identity inside the leaf, but duplicate
     * values may span sibling children after splits. While this child's high
     * key still equals the item value, the pointed-to instance may live in a
     * later sibling: keep descending rightward. */
    while (!child_result.delete_executed) {
        if (sdscmp(inner->anchors[index], (sds)item) != 0) return child_result;
        index++;
        if (index == inner->header.num_items) return (deleteResult){0};
        child_result = subtreeDeleteItem(fbt, inner->children[index], item);
    }

    /* Update child size after delete */
    inner->child_sizes[index]--;

    if (child_result.updated_anchor) {
        inner->anchors[index] = child_result.updated_anchor;
        /* If the first or last anchor changed, the common prefix may need shortening */
        if (index == 0 || index == inner->header.num_items - 1) {
            updateCommonPrefix(inner);
        }
        for (int j = 0; j < FEATURE_SIZE; j++)
            inner->features[j][index] = getFeatureByte(child_result.updated_anchor, inner->prefix_len, j);
    }

    /* Remove empty child */
    if (inner->child_sizes[index] == 0) {
        freeEmptyNode(inner->children[index]);
        innerNodeRemoveChild(inner, index);
        /* Anchor update: if we removed last child, new last child's anchor bubbles up */
        sds new_anchor = (inner->header.num_items > 0 && index == inner->header.num_items)
                             ? inner->anchors[inner->header.num_items - 1]
                             : NULL;
        return (deleteResult){
            .updated_anchor = new_anchor,
            .delete_executed = true};
    }

    inner->child_num_items[index] = inner->children[index]->num_items;

    sds updated_anchor = NULL;
    if (child_result.updated_anchor || index >= inner->header.num_items) {
        updated_anchor = inner->anchors[inner->header.num_items - 1];
    }

    return (deleteResult){
        .updated_anchor = updated_anchor,
        .delete_executed = true};
}

/* Helper to handle root cleanup after delete */
static void fbtreePostDeleteCleanup(fbtreeIndex *fbt) {
    if (getSubtreeSize(fbt->root) == 0) {
        zfree(fbt->root);
        fbt->root = NULL;
        fbt->leftmost_leaf = NULL;
        fbt->rightmost_leaf = NULL;
    } else if (!fbt->root->is_leaf && fbt->root->num_items == 1) {
        innerNode *old_root = (innerNode *)fbt->root;
        fbt->root = old_root->children[0];
        innerNodeFreePrefix(old_root);
        zfree(old_root);
    }
}

bool fbtreeDelete(fbtreeIndex *fbt, const_sds item) {
    if (fbt->root == NULL) return false;

    deleteResult result = subtreeDeleteItem(fbt, fbt->root, item);
    if (!result.delete_executed) return false;

    fbtreePostDeleteCleanup(fbt);
    return true;
}

static deleteResult subtreePop(fbtreeIndex *fbt, node *n, TraversalHint hint, bool free_item) {
    assert(hint != HINT_NONE);
    if (n->is_leaf) {
        leafNode *leaf = (leafNode *)n;
        int idx = (hint == HINT_LEFTMOST) ? 0 : leaf->header.num_items - 1;
        sds item = leafNodeRemoveAt(leaf, idx);
        if (free_item) sdsfree(item);
        return (deleteResult){
            .delete_executed = true,
            .updated_anchor = (hint == HINT_RIGHTMOST) ? leafNodeHighKey(leaf) : NULL};
    }

    innerNode *inner = (innerNode *)n;
    int index = (hint == HINT_LEFTMOST) ? 0 : inner->header.num_items - 1;

    deleteResult child_result = subtreePop(fbt, inner->children[index], hint, free_item);
    if (!child_result.delete_executed) return child_result;

    inner->child_sizes[index]--;

    if (child_result.updated_anchor) {
        inner->anchors[index] = child_result.updated_anchor;
        /* If the first or last anchor changed, the common prefix may need shortening */
        if (index == 0 || index == inner->header.num_items - 1) {
            updateCommonPrefix(inner);
        }
        for (int j = 0; j < FEATURE_SIZE; j++)
            inner->features[j][index] = getFeatureByte(child_result.updated_anchor, inner->prefix_len, j);
    }

    /* Remove empty child */
    if (inner->child_sizes[index] == 0) {
        freeEmptyNode(inner->children[index]);
        innerNodeRemoveChild(inner, index);
        sds new_anchor = (inner->header.num_items > 0 && index == inner->header.num_items)
                             ? inner->anchors[inner->header.num_items - 1]
                             : NULL;
        return (deleteResult){
            .updated_anchor = new_anchor,
            .delete_executed = true};
    }

    inner->child_num_items[index] = inner->children[index]->num_items;

    sds updated_anchor = NULL;
    if (child_result.updated_anchor || index >= inner->header.num_items) {
        updated_anchor = inner->anchors[inner->header.num_items - 1];
    }

    return (deleteResult){
        .updated_anchor = updated_anchor,
        .delete_executed = true};
}

/* Pop and return the minimum element. Returns NULL if tree is empty.
 * Caller is responsible for freeing the returned sds. */
sds fbtreePopMin(fbtreeIndex *fbt) {
    if (!fbt->root || !fbt->leftmost_leaf) return NULL;

    leafNode *leaf = fbt->leftmost_leaf;
    sds item = leaf->values[0];

    /* Update cache before delete */
    if (leaf->header.num_items == 1)
        fbt->leftmost_leaf = leaf->next;

    subtreePop(fbt, fbt->root, HINT_LEFTMOST, false);
    fbtreePostDeleteCleanup(fbt);
    return item;
}

/* Pop and return the maximum element. Returns NULL if tree is empty.
 * Caller is responsible for freeing the returned sds. */
sds fbtreePopMax(fbtreeIndex *fbt) {
    if (!fbt->root || !fbt->rightmost_leaf) return NULL;

    leafNode *leaf = fbt->rightmost_leaf;
    sds item = leaf->values[leaf->header.num_items - 1];

    /* Update cache before delete */
    if (leaf->header.num_items == 1)
        fbt->rightmost_leaf = leaf->prev;

    subtreePop(fbt, fbt->root, HINT_RIGHTMOST, false);
    fbtreePostDeleteCleanup(fbt);
    return item;
}

const_sds fbtreePeekMin(fbtreeIndex *fbt) {
    if (!fbt->leftmost_leaf || fbt->leftmost_leaf->header.num_items == 0) return NULL;
    return fbt->leftmost_leaf->values[0];
}

const_sds fbtreePeekMax(fbtreeIndex *fbt) {
    if (!fbt->rightmost_leaf || fbt->rightmost_leaf->header.num_items == 0) return NULL;
    return fbt->rightmost_leaf->values[fbt->rightmost_leaf->header.num_items - 1];
}

/* Get element at given rank (0-indexed). Returns NULL if rank >= length */
const_sds fbtreeGetAtRank(fbtreeIndex *fbt, unsigned long rank) {
    if (!fbt->root) return NULL;

    node *current = fbt->root;
    unsigned long remaining = rank;

    while (!current->is_leaf) {
        innerNode *inner = (innerNode *)current;
        int i = 0;
        while (i < inner->header.num_items && remaining >= inner->child_sizes[i]) {
            remaining -= inner->child_sizes[i];
            i++;
        }
        if (i >= inner->header.num_items) return NULL;
        current = inner->children[i];
    }

    leafNode *leaf = (leafNode *)current;
    if (remaining >= leaf->header.num_items) return NULL;
    return leaf->values[remaining];
}

/* Rank of the pointer-identical item within subtree n, or -1 if absent. */
static long subtreeGetIndexOfItem(node *n, const_sds item) {
    if (n->is_leaf) {
        leafNode *leaf = (leafNode *)n;
        for (int i = 0; i < leaf->header.num_items; i++) {
            if (leaf->values[i] == item) return i;
        }
        return -1;
    }

    innerNode *inner = (innerNode *)n;
    int index = findChildIndex(inner, item);
    if (index == inner->header.num_items) return -1;

    long rank_base = 0;
    for (int i = 0; i < index; i++) rank_base += inner->child_sizes[i];

    /* Items are matched by pointer identity inside the leaf, but duplicate
     * values may span sibling children after splits: while this child's high
     * key still equals the item value, keep searching rightward. */
    while (index < inner->header.num_items) {
        long sub = subtreeGetIndexOfItem(inner->children[index], item);
        if (sub >= 0) return rank_base + sub;
        if (sdscmp(inner->anchors[index], (sds)item) != 0) return -1;
        rank_base += inner->child_sizes[index];
        index++;
    }
    return -1;
}

/* Get rank of an item given a direct pointer to it (from hashtable lookup).
 * The item pointer must be a valid pointer into a leaf node's values array. */
long fbtreeGetIndexOfItem(fbtreeIndex *fbt, const_sds item) {
    if (!fbt->root || !item) return -1;
    return subtreeGetIndexOfItem(fbt->root, item);
}

unsigned long fbtreeLength(fbtreeIndex *fbt) {
    return fbt->root ? getSubtreeSize(fbt->root) : 0;
}

/* Return the height of the tree: the number of levels from the root down to a
 * leaf. An empty tree (no root) has height 0, a single leaf root has height 1,
 * and each additional inner level adds 1. All root-to-leaf paths in a B+tree
 * are the same length, so this descends the leftmost spine in O(height). */
unsigned long fbtreeHeight(const fbtreeIndex *fbt) {
    if (!fbt || !fbt->root) return 0;
    unsigned long height = 1;
    const node *n = fbt->root;
    while (!n->is_leaf) {
        n = ((const innerNode *)n)->children[0];
        height++;
    }
    return height;
}

void fbtreeResetIterator(fbtreeIterator *iterator) {
    iter *it = iteratorFromOpaque(iterator);
    it->fbt = NULL;
    it->current_leaf = NULL;
    it->current_index = 0;
    it->leaf_count = 0;
    it->state = ITER_AT_POSITION;
}

void fbtreeInitIterator(fbtreeIterator *iterator, fbtreeIndex *fbt) {
    iter *it = iteratorFromOpaque(iterator);
    it->fbt = fbt->root ? fbt : NULL;
    it->current_leaf = NULL;
    it->current_index = 0;
    it->leaf_count = 0;
    it->state = ITER_AT_POSITION;
}

fbtreeIndex *fbtreeIteratorGetIndex(fbtreeIterator *iterator) {
    return iteratorFromOpaque(iterator)->fbt;
}

const_sds fbtreeNext(fbtreeIterator *iterator) {
    iter *it = iteratorFromOpaque(iterator);
    if (!it->fbt) return NULL;
    if (it->state == ITER_PAST_END) return NULL;

    if (!it->current_leaf) {
        /* First call - use cached leftmost leaf for O(1) start */
        it->current_leaf = it->fbt->leftmost_leaf;
        it->current_index = 0;
        it->leaf_count = it->current_leaf ? it->current_leaf->header.num_items : 0;
        it->state = ITER_AT_POSITION;
    }

    while (it->current_leaf) {
        if (it->current_index < it->leaf_count) {
            /* Returning a valid element: clear any BEFORE_START state left by a
             * seek that landed at rank 0, so a subsequent fbtreePrev() yields
             * this element instead of early-returning NULL. */
            it->state = ITER_AT_POSITION;
            return it->current_leaf->values[it->current_index++];
        }
        it->current_leaf = it->current_leaf->next;
        it->current_index = 0;
        it->leaf_count = it->current_leaf ? it->current_leaf->header.num_items : 0;
    }
    it->state = ITER_PAST_END;
    return NULL;
}

const_sds fbtreePrev(fbtreeIterator *iterator) {
    iter *it = iteratorFromOpaque(iterator);
    if (!it->fbt) return NULL;
    if (it->state == ITER_BEFORE_START) return NULL;

    if (!it->current_leaf) {
        /* First call - use cached rightmost leaf for O(1) start */
        it->current_leaf = it->fbt->rightmost_leaf;
        it->leaf_count = it->current_leaf ? it->current_leaf->header.num_items : 0;
        it->current_index = it->leaf_count;
        it->state = ITER_AT_POSITION;
    }

    while (it->current_leaf) {
        if (it->current_index > 0) {
            /* Returning a valid element: clear any PAST_END state left by a seek
             * that landed past the last element, so a subsequent fbtreeNext()
             * yields this element instead of early-returning NULL. */
            it->state = ITER_AT_POSITION;
            return it->current_leaf->values[--it->current_index];
        }
        it->current_leaf = it->current_leaf->prev;
        it->leaf_count = it->current_leaf ? it->current_leaf->header.num_items : 0;
        it->current_index = it->leaf_count;
    }
    it->state = ITER_BEFORE_START;
    return NULL;
}

void fbtreeSeekToRank(fbtreeIterator *iterator, unsigned long rank) {
    iter *it = iteratorFromOpaque(iterator);
    if (!it->fbt || !it->fbt->root) {
        it->current_leaf = NULL;
        return;
    }

    node *current = it->fbt->root;
    unsigned long remaining = rank;

    while (!current->is_leaf) {
        innerNode *inner = (innerNode *)current;
        int i = 0;
        while (i < inner->header.num_items && remaining >= inner->child_sizes[i]) {
            remaining -= inner->child_sizes[i];
            i++;
        }
        if (i >= inner->header.num_items) {
            /* Rank beyond tree — position past end */
            it->current_leaf = NULL;
            it->state = ITER_PAST_END;
            return;
        }
        current = inner->children[i];
    }

    leafNode *leaf = (leafNode *)current;
    if (remaining >= leaf->header.num_items) {
        /* Rank beyond tree — position past end */
        it->current_leaf = NULL;
        it->state = ITER_PAST_END;
        return;
    }
    it->current_leaf = leaf;
    it->leaf_count = leaf->header.num_items;
    it->current_index = (uint8_t)remaining;
    it->state = (rank == 0) ? ITER_BEFORE_START : ITER_AT_POSITION;
}

#define SCORE_SIZE sizeof(double) /* Normalized score prefix size */

/* Find child index for score lookup (8-byte prefix).
 * Compares the full prefix at each node (up to SCORE_SIZE bytes). */
static int findChildIndexByScore(innerNode *inner, const char *score) {
    /* Compare prefix bytes (up to 8) */
    if (inner->prefix_len > 0) {
        size_t cmp_len = inner->prefix_len < SCORE_SIZE ? inner->prefix_len : SCORE_SIZE;
        const char *prefix = innerNodeGetPrefix(inner);
        int cmp = memcmp(score, prefix, cmp_len);
        if (cmp < 0) return 0;
        if (cmp > 0) return inner->header.num_items;
    }

    /* If node prefix covers entire score, first child has first match */
    if (inner->prefix_len >= SCORE_SIZE) return 0;

    /* Extract feature bytes. The key is exactly SCORE_SIZE bytes; positions
     * past it read as zero, the minimal continuation, matching how
     * findChildIndex pads keys shorter than the feature window. */
    unsigned char target[FEATURE_SIZE];
    for (int j = 0; j < FEATURE_SIZE; j++) {
        size_t idx = inner->prefix_len + j;
        target[j] = idx < SCORE_SIZE ? (unsigned char)score[idx] : 0;
    }

    int left, right;
    featureSearchSIMD(inner->features, inner->header.num_items, target, &left, &right);

    /* Binary search with fixed 8-byte comparison */
    while (left < right) {
        int mid = (left + right) / 2;
        if (memcmp(inner->anchors[mid], score, SCORE_SIZE) < 0)
            left = mid + 1;
        else
            right = mid;
    }
    return left;
}

/* Binary search in leaf for first element with score >= given score */
static int leafNodeBinarySearchByScore(leafNode *leaf, const char *score) {
    int left = 0, right = leaf->header.num_items;
    while (left < right) {
        int mid = (left + right) / 2;
        if (memcmp(leaf->values[mid], score, SCORE_SIZE) < 0)
            left = mid + 1;
        else
            right = mid;
    }
    return left;
}

/* Seek to first element with score >= given score. Always positions iterator.
 * If score > all elements, positions past end (fbtreeNext returns NULL, fbtreePrev works).
 * If score < all elements, positions at start (fbtreePrev returns NULL, fbtreeNext works).
 * Returns the rank (0-indexed) of the position. If positioned past end, returns
 * the tree length (one past the last valid rank). Returns 0 for an empty tree. */
long fbtreeSeekToScore(const char *score, fbtreeIterator *iterator) {
    iter *it = iteratorFromOpaque(iterator);
    fbtreeIndex *fbt = it->fbt;
    it->current_leaf = NULL;
    it->current_index = 0;
    it->leaf_count = 0;
    it->state = ITER_AT_POSITION;

    if (!fbt || !fbt->root) {
        it->fbt = NULL;
        return 0;
    }

    node *current = fbt->root;
    long rank = 0;

    while (!current->is_leaf) {
        innerNode *inner = (innerNode *)current;
        int child_idx = findChildIndexByScore(inner, score);
        if (child_idx >= inner->header.num_items)
            child_idx = inner->header.num_items - 1;
        for (int i = 0; i < child_idx; i++)
            rank += inner->child_sizes[i];
        current = inner->children[child_idx];
    }

    leafNode *leaf = (leafNode *)current;
    int pos = leafNodeBinarySearchByScore(leaf, score);
    rank += pos;

    it->fbt = fbt;
    it->current_leaf = leaf;
    it->leaf_count = leaf->header.num_items;
    it->current_index = pos;

    if (pos >= leaf->header.num_items) {
        /* Score beyond tree max */
        it->current_leaf = NULL;
        it->leaf_count = 0;
        it->current_index = 0;
        it->state = ITER_PAST_END;
    } else if (pos == 0 && leaf == fbt->leftmost_leaf) {
        /* At first element of tree - nothing before this */
        it->state = ITER_BEFORE_START;
    }
    return rank;
}

/* Seek to first element with value >= given value using full sds comparison.
 * If value > all elements, positions past end (fbtreeNext returns NULL, fbtreePrev works).
 * If value < all elements, positions at start (fbtreePrev returns NULL, fbtreeNext works). */
long fbtreeSeekToValue(const_sds value, fbtreeIterator *iterator) {
    iter *it = iteratorFromOpaque(iterator);
    fbtreeIndex *fbt = it->fbt;
    it->current_leaf = NULL;
    it->current_index = 0;
    it->leaf_count = 0;
    it->state = ITER_AT_POSITION;

    if (!fbt || !fbt->root) {
        it->fbt = NULL;
        return 0;
    }

    node *current = fbt->root;
    long rank = 0;

    while (!current->is_leaf) {
        innerNode *inner = (innerNode *)current;
        int child_idx = findChildIndex(inner, value);
        if (child_idx >= inner->header.num_items) child_idx = inner->header.num_items - 1;
        for (int i = 0; i < child_idx; i++)
            rank += inner->child_sizes[i];
        current = inner->children[child_idx];
    }

    leafNode *leaf = (leafNode *)current;
    int pos = leafNodeBinarySearch(leaf, value);
    rank += pos;

    it->fbt = fbt;
    it->current_leaf = leaf;
    it->leaf_count = leaf->header.num_items;
    it->current_index = pos;

    if (pos >= leaf->header.num_items) {
        /* Value beyond tree max */
        it->current_leaf = NULL;
        it->leaf_count = 0;
        it->current_index = 0;
        it->state = ITER_PAST_END;
    } else if (pos == 0 && leaf == fbt->leftmost_leaf) {
        /* At first element of tree - nothing before this */
        it->state = ITER_BEFORE_START;
    }

    /* Rank of the first item whose packed value is >= 'value'
     * (i.e. the count of items strictly less than it). */
    return rank;
}

/* ========== Range Deletion ========== */

/* Boundary paths captured by path builders for use by deleteRangeCore and
 * deleteRangeSameLeaf. Stack-allocated; all arrays bounded by MAX_TREE_DEPTH. */
typedef struct {
    /* Shared path from root to split point */
    node *shared_path[MAX_TREE_DEPTH];
    int shared_left_idx[MAX_TREE_DEPTH];  /* left boundary child index at each level */
    int shared_right_idx[MAX_TREE_DEPTH]; /* right boundary child index at each level */
    int shared_depth;                     /* number of levels in shared path */

    /* Left sub-path from split point's left child down to start leaf */
    node *left_sub_path[MAX_TREE_DEPTH];
    int left_sub_idx[MAX_TREE_DEPTH]; /* child index at each level */
    int left_sub_depth;

    /* Right sub-path from split point's right child down to end leaf */
    node *right_sub_path[MAX_TREE_DEPTH];
    int right_sub_idx[MAX_TREE_DEPTH]; /* child index at each level */
    int right_sub_depth;

    /* Leaf-level boundary indices */
    leafNode *start_leaf;
    int start_idx; /* first index to delete in start_leaf */
    leafNode *end_leaf;
    int end_idx; /* last index to delete in end_leaf */

} BoundaryPaths;

/* Descend from an inner node to a leaf, recording the path. At each inner
 * node level, findChild is called to determine which child to descend into.
 * Returns the leaf node reached.
 *
 * When called with a compile-time-constant function pointer (e.g.,
 * findChildByScoreWrapper), the compiler can inline both this helper and the
 * callback at -O2, producing specialized code with no indirect calls. */
typedef int (*findChildFn)(innerNode *inner, const void *key);

static leafNode *descendSubPath(node *start,
                                findChildFn findChild,
                                const void *key,
                                node *path[],
                                int path_idx[],
                                int *path_depth) {
    node *current = start;
    while (!current->is_leaf) {
        innerNode *inner = (innerNode *)current;
        path[*path_depth] = current;
        int ci = findChild(inner, key);
        if (ci >= inner->header.num_items) ci = inner->header.num_items - 1;
        path_idx[*path_depth] = ci;
        (*path_depth)++;
        current = inner->children[ci];
    }
    return (leafNode *)current;
}

/* Wrappers to match the findChildFn signature */
static int findChildByScoreWrapper(innerNode *inner, const void *key) {
    return findChildIndexByScore(inner, (const char *)key);
}

static int findChildByValueWrapper(innerNode *inner, const void *key) {
    return findChildIndex(inner, (const_sds)key);
}

/* Comparison function for leaf-level index resolution.
 * Returns <0, 0, or >0 like memcmp/sdscmp. */
typedef int (*leafCmpFn)(const_sds element, const void *key);

static int leafCmpByScore(const_sds element, const void *key) {
    return memcmp(element, key, SCORE_SIZE);
}

static int leafCmpByValue(const_sds element, const void *key) {
    return sdscmp(element, (const_sds)key);
}

/* Resolve the start index (first element to delete) in a leaf.
 * Binary search for first element >= key, then advance past equals
 * if the bound is exclusive. */
static int resolveStartIdx(const leafNode *leaf, const void *key, int exclusive, leafCmpFn cmp) {
    int lo = 0, hi = leaf->header.num_items;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cmp(leaf->values[mid], key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (exclusive) {
        while (lo < leaf->header.num_items && cmp(leaf->values[lo], key) == 0) lo++;
    }
    return lo;
}

/* Resolve the end index (last element to delete) in a leaf.
 * Binary search for first element >= key, then:
 * - inclusive: advance past equals, subtract 1 (last element <= key)
 * - exclusive: subtract 1 (last element < key) */
static int resolveEndIdx(const leafNode *leaf, const void *key, int exclusive, leafCmpFn cmp) {
    int lo = 0, hi = leaf->header.num_items;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cmp(leaf->values[mid], key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (exclusive) {
        return lo - 1;
    }
    while (lo < leaf->header.num_items && cmp(leaf->values[lo], key) == 0) lo++;
    return lo - 1;
}

/* Resolve the start index in start_leaf and the end index in end_leaf when the
 * two boundaries fall in DIFFERENT leaves, software-pipelining the two binary
 * searches so both leaves' cache misses are outstanding at once (memory-level
 * parallelism). Each `values[mid]` is a pointer to a separately-allocated sds,
 * so every probe is a cache miss; interleaving the two independent searches and
 * prefetching both probe payloads per step lets the out-of-order core overlap
 * them instead of serializing ~2x log2(leaf) misses. Semantics match
 * resolveStartIdx (start) and resolveEndIdx (end). Same-leaf callers should use
 * those helpers directly -- there is only one leaf to search, so there is no
 * independent second stream to overlap. */
static void resolveBothIdxPrefetch(const leafNode *start_leaf, const void *min_key, int min_ex, const leafNode *end_leaf, const void *max_key, int max_ex, leafCmpFn cmp, int *out_start_idx, int *out_end_idx) {
    /* Two independent lower-bound searches (first element >= key) stepped in
     * lockstep. Prefetch both probe payloads before either compare so the misses
     * overlap in the load/fill buffers. */
    int slo = 0, shi = start_leaf->header.num_items;
    int elo = 0, ehi = end_leaf->header.num_items;
    while (slo < shi || elo < ehi) {
        int smid = (slo + shi) / 2;
        int emid = (elo + ehi) / 2;
        if (slo < shi) __builtin_prefetch(start_leaf->values[smid]);
        if (elo < ehi) __builtin_prefetch(end_leaf->values[emid]);
        if (slo < shi) {
            if (cmp(start_leaf->values[smid], min_key) < 0)
                slo = smid + 1;
            else
                shi = smid;
        }
        if (elo < ehi) {
            if (cmp(end_leaf->values[emid], max_key) < 0)
                elo = emid + 1;
            else
                ehi = emid;
        }
    }

    /* start_idx: first element >= min; skip equals when the bound is exclusive. */
    int start_idx = slo;
    if (min_ex) {
        while (start_idx < start_leaf->header.num_items && cmp(start_leaf->values[start_idx], min_key) == 0) start_idx++;
    }

    /* end_idx: last element <= max (inclusive) or < max (exclusive). */
    int end_idx = elo;
    if (!max_ex) {
        while (end_idx < end_leaf->header.num_items && cmp(end_leaf->values[end_idx], max_key) == 0) end_idx++;
    }
    end_idx -= 1;

    *out_start_idx = start_idx;
    *out_end_idx = end_idx;
}


/* Descend the tree once for a [min_key, max_key] range, recording the shared
 * root->split path and the two sub-paths down to the start/end leaves in `bp`.
 * `findChild` selects score-prefix vs full-value comparison, so this is shared
 * by range-delete and range-count. This only LOCATES the boundary leaves and
 * records the paths; the caller resolves the leaf-local start_idx/end_idx (which
 * lets range-count fuse the two leaf searches). Returns true if both boundaries
 * land in the same leaf (no split), false if the paths diverge at a split node.
 * Callers must first handle the empty-tree, whole-tree, and empty-range
 * short-circuits. */
static bool buildBoundaryPaths(fbtreeIndex *fbt,
                               BoundaryPaths *bp,
                               const void *min_key,
                               const void *max_key,
                               findChildFn findChild) {
    memset(bp, 0, sizeof(*bp));

    node *current = fbt->root;
    int depth = 0;

    while (!current->is_leaf) {
        innerNode *inner = (innerNode *)current;
        bp->shared_path[depth] = current;

        int li = findChild(inner, min_key);
        if (li >= inner->header.num_items) li = inner->header.num_items - 1;
        int ri = findChild(inner, max_key);
        if (ri >= inner->header.num_items) ri = inner->header.num_items - 1;

        bp->shared_left_idx[depth] = li;
        bp->shared_right_idx[depth] = ri;
        depth++;

        if (li == ri) {
            current = inner->children[li];
        } else {
            bp->shared_depth = depth;

            /* Descend left sub-path */
            bp->start_leaf = descendSubPath(
                inner->children[li], findChild, min_key,
                bp->left_sub_path, bp->left_sub_idx, &bp->left_sub_depth);

            /* Descend right sub-path */
            bp->end_leaf = descendSubPath(
                inner->children[ri], findChild, max_key,
                bp->right_sub_path, bp->right_sub_idx, &bp->right_sub_depth);

            return false; /* split */
        }
    }

    /* Both boundaries in the same leaf */
    bp->shared_depth = depth;
    bp->start_leaf = (leafNode *)current;
    bp->end_leaf = (leafNode *)current;
    return true; /* same leaf */
}

/* Compute the global 0-indexed rank of a boundary position recorded in `bp`.
 * `use_right` selects the right (max) boundary indices and sub-path; otherwise
 * the left (min) boundary. `leaf_idx` is the leaf-local offset to add (the
 * boundary's index within its leaf). Rank is the count of elements strictly
 * before the position: sum of child_sizes left of the chosen child index at
 * every level of the shared path and the sub-path, plus leaf_idx. */
static unsigned long rankFromBoundaryPath(const BoundaryPaths *bp, bool use_right, int leaf_idx) {
    unsigned long rank = 0;

    for (int d = 0; d < bp->shared_depth; d++) {
        const innerNode *inner = (const innerNode *)bp->shared_path[d];
        int ci = use_right ? bp->shared_right_idx[d] : bp->shared_left_idx[d];
        for (int i = 0; i < ci; i++) rank += inner->child_sizes[i];
    }

    int sub_depth = use_right ? bp->right_sub_depth : bp->left_sub_depth;
    for (int d = 0; d < sub_depth; d++) {
        const innerNode *inner = use_right ? (const innerNode *)bp->right_sub_path[d]
                                           : (const innerNode *)bp->left_sub_path[d];
        int ci = use_right ? bp->right_sub_idx[d] : bp->left_sub_idx[d];
        for (int i = 0; i < ci; i++) rank += inner->child_sizes[i];
    }

    return rank + (unsigned long)leaf_idx;
}

/* Delete a range within a single leaf. The shared_path records the path from
 * root to the leaf's parent for inner node fixup after removal.
 * Returns the number of elements deleted. */
static unsigned long deleteRangeSameLeaf(fbtreeIndex *fbt,
                                         BoundaryPaths *bp,
                                         fbtreeItemCallback callback,
                                         void *callback_ctx) {
    /* Empty range check for score/value paths where resolved indices
     * may indicate no matching elements. Rank paths never hit this. */
    if (bp->start_idx > bp->end_idx || bp->start_idx >= bp->start_leaf->header.num_items || bp->end_idx < 0) return 0;

    leafNode *leaf = bp->start_leaf;
    int start_idx = bp->start_idx;
    int end_idx = bp->end_idx;
    unsigned long deleted = (unsigned long)(end_idx - start_idx + 1);

    /* Update leftmost/rightmost caches if this leaf will become empty */
    if (start_idx == 0 && end_idx == leaf->header.num_items - 1) {
        if (leaf == fbt->leftmost_leaf) fbt->leftmost_leaf = leaf->next;
        if (leaf == fbt->rightmost_leaf) fbt->rightmost_leaf = leaf->prev;
    }

    /* Invoke callback for items being removed */
    if (callback) {
        for (int i = start_idx; i <= end_idx; i++) {
            callback(leaf->values[i], callback_ctx);
        }
    }

    leafNodeRemoveRange(leaf, start_idx, end_idx);

    /* Walk back up shared path: fix sizes and anchors */
    for (int d = bp->shared_depth - 1; d >= 0; d--) {
        innerNode *inner = (innerNode *)bp->shared_path[d];
        int ci = bp->shared_left_idx[d];
        inner->child_sizes[ci] -= deleted;

        if (inner->child_sizes[ci] == 0) {
            freeEmptyNode(inner->children[ci]);
            innerNodeRemoveChild(inner, ci);
        } else {
            innerNodeRefreshChildMeta(inner, ci);
            if (ci == 0 || ci == inner->header.num_items - 1) updateCommonPrefix(inner);
        }
    }


    fbtreePostDeleteCleanup(fbt);
    return deleted;
}

/* Core range deletion engine for the diverged-path case (boundaries in
 * different leaves). Splices the leaf chain, trims boundary leaves, fixes
 * up inner nodes bottom-to-top, and removes empty nodes.
 * If callback is non-NULL, it is invoked for each deleted item before sdsfree.
 * Returns the number of elements deleted. */
static unsigned long deleteRangeCore(fbtreeIndex *fbt, BoundaryPaths *bp, fbtreeItemCallback callback, void *callback_ctx) {
    /* Empty range check: score/value path builders can produce boundary
     * indices that fall outside the leaf (no matching elements in that leaf).
     * Rank paths never hit this since ranks are pre-validated. */
    if (bp->start_idx >= bp->start_leaf->header.num_items && bp->end_idx < 0) return 0;
    if (bp->end_idx < 0 && bp->start_leaf == bp->end_leaf) return 0;

    /* --- Phase 1: Boundaries diverged. Process the split. --- */
    int split_depth = bp->shared_depth - 1;
    innerNode *split_node = (innerNode *)bp->shared_path[split_depth];
    int li = bp->shared_left_idx[split_depth];
    int ri = bp->shared_right_idx[split_depth];

    leafNode *start_leaf = bp->start_leaf;
    int start_idx = bp->start_idx;
    leafNode *end_leaf = bp->end_leaf;
    int end_idx = bp->end_idx;

    /* --- Phase 2: Splice leaf chain and trim boundary leaves --- */

    /* For score/value-based paths, the boundary indices might be out of range:
     * - start_idx >= start_leaf->num_items: no matching elements in start leaf
     * - end_idx < 0: no matching elements in end leaf
     * In these cases, the boundary leaf is not trimmed at all. */
    bool start_leaf_untouched = (start_idx >= start_leaf->header.num_items);
    bool end_leaf_untouched = (end_idx < 0);

    /* Determine the surviving boundary leaves after trimming */
    bool start_leaf_dies = !start_leaf_untouched && (start_idx == 0);
    bool end_leaf_dies = !end_leaf_untouched && (end_idx == end_leaf->header.num_items - 1);

    /* The last surviving leaf on the left side */
    leafNode *left_survivor = start_leaf_dies ? start_leaf->prev : start_leaf;
    /* The first surviving leaf on the right side */
    leafNode *right_survivor = end_leaf_dies ? end_leaf->next : end_leaf;

    /* Update leftmost/rightmost caches */
    if (fbt->leftmost_leaf == start_leaf && start_leaf_dies) {
        fbt->leftmost_leaf = right_survivor;
    }
    if (fbt->rightmost_leaf == end_leaf && end_leaf_dies) {
        fbt->rightmost_leaf = left_survivor;
    }

    /* Splice the linked list: connect left_survivor <-> right_survivor.
     * All leaves between them become unreachable from the list. */
    if (left_survivor)
        left_survivor->next = right_survivor;
    if (right_survivor)
        right_survivor->prev = left_survivor;

    /* Count deleted elements during splice */
    unsigned long deleted = 0;

    /* Trim boundary leaves (free deleted sds values but keep the leaf node alive).
     * Even for leaves that will be fully freed via freeNodeRecursive, we must
     * zero their num_items so getSubtreeSize returns correct counts during fixup.
     * Skip trimming entirely for untouched boundary leaves (score/value edge case). */
    if (start_leaf_untouched) {
        /* No elements to delete in start leaf — leave it completely untouched */
    } else if (!start_leaf_dies) {
        for (int i = start_idx; i < start_leaf->header.num_items; i++) {
            if (callback) callback(start_leaf->values[i], callback_ctx);
            sdsfree(start_leaf->values[i]);
            deleted++;
        }
        start_leaf->header.num_items = start_idx;
    } else {
        for (int i = 0; i < start_leaf->header.num_items; i++) {
            if (callback) callback(start_leaf->values[i], callback_ctx);
            sdsfree(start_leaf->values[i]);
            deleted++;
        }
        start_leaf->header.num_items = 0;
    }
    if (end_leaf_untouched) {
        /* No elements to delete in end leaf — leave it completely untouched */
    } else if (!end_leaf_dies) {
        for (int i = 0; i <= end_idx; i++) {
            if (callback) callback(end_leaf->values[i], callback_ctx);
            sdsfree(end_leaf->values[i]);
            deleted++;
        }
        int remaining = end_leaf->header.num_items - end_idx - 1;
        memmove(&end_leaf->values[0], &end_leaf->values[end_idx + 1], remaining * sizeof(sds));
        end_leaf->header.num_items = remaining;
    } else {
        for (int i = 0; i < end_leaf->header.num_items; i++) {
            if (callback) callback(end_leaf->values[i], callback_ctx);
            sdsfree(end_leaf->values[i]);
            deleted++;
        }
        end_leaf->header.num_items = 0;
    }

    /* --- Phase 3: Fix up inner nodes bottom-to-top --- */

    /* Fix left subtree: remove children to the right of the left boundary child.
     * These subtrees are fully within the deleted range. Their leaves are already
     * disconnected from the linked list, so freeNodeRecursive is safe. */
    for (int d = bp->left_sub_depth - 1; d >= 0; d--) {
        innerNode *inner = (innerNode *)bp->left_sub_path[d];
        int ci = bp->left_sub_idx[d];

        for (int i = ci + 1; i < inner->header.num_items; i++) {
            deleted += getSubtreeSize(inner->children[i]);
            freeNodeRecursive(inner->children[i], callback, callback_ctx);
        }
        inner->header.num_items = ci + 1;

        /* Update or remove the boundary child */
        if (getSubtreeSize(inner->children[ci]) == 0) {
            freeEmptyNodeAlreadyUnlinked(inner->children[ci]);
            inner->header.num_items = ci;
        } else {
            innerNodeRefreshChildMeta(inner, ci);
        }
        updateCommonPrefix(inner);
    }

    /* Fix right subtree: remove children to the left of the right boundary child */
    for (int d = bp->right_sub_depth - 1; d >= 0; d--) {
        innerNode *inner = (innerNode *)bp->right_sub_path[d];
        int ci = bp->right_sub_idx[d];

        for (int i = 0; i < ci; i++) {
            deleted += getSubtreeSize(inner->children[i]);
            freeNodeRecursive(inner->children[i], callback, callback_ctx);
        }
        if (ci > 0) {
            innerNodeRemoveChildrenRange(inner, 0, ci - 1);
        }

        /* Boundary child is now at index 0 (if we removed left children).
         * Update the recorded index after removal. */
        bp->right_sub_idx[d] = 0;
        int new_ci = 0;
        if (getSubtreeSize(inner->children[new_ci]) == 0) {
            freeEmptyNodeAlreadyUnlinked(inner->children[new_ci]);
            innerNodeRemoveChildrenRange(inner, 0, 0);
        } else {
            innerNodeRefreshChildMeta(inner, new_ci);
        }
        updateCommonPrefix(inner);
    }

    /* Fix the split node: remove fully-deleted middle children, update boundary children */
    {
        /* Free middle children subtrees (fully inside the range) */
        for (int i = li + 1; i < ri; i++) {
            deleted += getSubtreeSize(split_node->children[i]);
            freeNodeRecursive(split_node->children[i], callback, callback_ctx);
        }

        /* Check if boundary subtrees are now empty */
        size_t left_size = getSubtreeSize(split_node->children[li]);
        size_t right_size = getSubtreeSize(split_node->children[ri]);

        int remove_start = li + 1;
        int remove_end = ri - 1;

        if (left_size == 0) {
            freeNodeRecursive(split_node->children[li], callback, callback_ctx);
            remove_start = li;
        }
        if (right_size == 0) {
            freeNodeRecursive(split_node->children[ri], callback, callback_ctx);
            remove_end = ri;
        }

        if (remove_start <= remove_end) {
            innerNodeRemoveChildrenRange(split_node, remove_start, remove_end);
        }

        /* Refresh metadata for surviving boundary children. After removal,
         * they occupy consecutive indices starting at li. */
        ri = li + (left_size > 0) + (right_size > 0) - 1;
        for (int i = li; i <= ri; i++) {
            innerNodeRefreshChildMeta(split_node, i);
        }
        updateCommonPrefix(split_node);
    }

    /* Fix ancestors above the split node */
    for (int d = split_depth - 1; d >= 0; d--) {
        innerNode *inner = (innerNode *)bp->shared_path[d];
        int ci = bp->shared_left_idx[d];

        if (getSubtreeSize(inner->children[ci]) == 0) {
            freeEmptyNodeAlreadyUnlinked(inner->children[ci]);
            innerNodeRemoveChild(inner, ci);
        } else {
            innerNodeRefreshChildMeta(inner, ci);
            if (ci == 0 || ci == inner->header.num_items - 1) updateCommonPrefix(inner);
        }
    }


    fbtreePostDeleteCleanup(fbt);

    return deleted;
}

/* Delete elements at ranks [start_rank, end_rank] (0-indexed, inclusive).
 * If callback is non-NULL, it is invoked for each deleted item before sdsfree.
 * Returns the number of elements deleted. */
unsigned long fbtreeDeleteRangeByRank(fbtreeIndex *fbt,
                                      unsigned long start_rank,
                                      unsigned long end_rank,
                                      void (*callback)(sds item, void *ctx),
                                      void *callback_ctx) {
    if (!fbt->root) return 0;

    unsigned long length = fbtreeLength(fbt);
    if (start_rank >= length) return 0;
    if (end_rank >= length) end_rank = length - 1;
    if (start_rank > end_rank) return 0;
    if (start_rank == 0 && end_rank == length - 1) {
        fbtreeDeleteAll(fbt, callback, callback_ctx);
        return length;
    }

    /* Build boundary paths by descending the tree using child_sizes accumulation */
    BoundaryPaths bp;
    memset(&bp, 0, sizeof(bp));

    node *current = fbt->root;
    unsigned long left_remaining = start_rank;
    unsigned long right_remaining = end_rank;
    int depth = 0;

    while (!current->is_leaf) {
        innerNode *inner = (innerNode *)current;
        bp.shared_path[depth] = current;

        int li = 0;
        unsigned long left_acc = 0;
        while (li < inner->header.num_items && left_acc + inner->child_sizes[li] <= left_remaining) {
            left_acc += inner->child_sizes[li];
            li++;
        }
        bp.shared_left_idx[depth] = li;
        left_remaining -= left_acc;

        int ri = 0;
        unsigned long right_acc = 0;
        while (ri < inner->header.num_items && right_acc + inner->child_sizes[ri] <= right_remaining) {
            right_acc += inner->child_sizes[ri];
            ri++;
        }
        bp.shared_right_idx[depth] = ri;
        right_remaining -= right_acc;

        depth++;

        if (li == ri) {
            current = inner->children[li];
        } else {
            bp.shared_depth = depth;

            /* Descend left sub-path */
            node *left_current = inner->children[li];
            while (!left_current->is_leaf) {
                innerNode *left_inner = (innerNode *)left_current;
                bp.left_sub_path[bp.left_sub_depth] = left_current;
                int ci = 0;
                unsigned long acc = 0;
                while (ci < left_inner->header.num_items && acc + left_inner->child_sizes[ci] <= left_remaining) {
                    acc += left_inner->child_sizes[ci];
                    ci++;
                }
                bp.left_sub_idx[bp.left_sub_depth] = ci;
                left_remaining -= acc;
                bp.left_sub_depth++;
                left_current = left_inner->children[ci];
            }
            bp.start_leaf = (leafNode *)left_current;
            bp.start_idx = (int)left_remaining;

            /* Descend right sub-path */
            node *right_current = inner->children[ri];
            while (!right_current->is_leaf) {
                innerNode *right_inner = (innerNode *)right_current;
                bp.right_sub_path[bp.right_sub_depth] = right_current;
                int ci = 0;
                unsigned long acc = 0;
                while (ci < right_inner->header.num_items && acc + right_inner->child_sizes[ci] <= right_remaining) {
                    acc += right_inner->child_sizes[ci];
                    ci++;
                }
                bp.right_sub_idx[bp.right_sub_depth] = ci;
                right_remaining -= acc;
                bp.right_sub_depth++;
                right_current = right_inner->children[ci];
            }
            bp.end_leaf = (leafNode *)right_current;
            bp.end_idx = (int)right_remaining;

            return deleteRangeCore(fbt, &bp, callback, callback_ctx);
        }
    }

    /* Both boundaries in the same leaf */
    bp.shared_depth = depth;
    bp.start_leaf = (leafNode *)current;
    bp.end_leaf = (leafNode *)current;
    bp.start_idx = (int)left_remaining;
    bp.end_idx = (int)right_remaining;
    return deleteRangeSameLeaf(fbt, &bp, callback, callback_ctx);
}

/* Delete elements with score prefix in [min_score, max_score].
 * min_ex/max_ex: if true, the corresponding bound is exclusive.
 * Score is an 8-byte big-endian normalized prefix (as stored in the tree).
 * If callback is non-NULL, it is invoked for each deleted item before sdsfree.
 * Returns the number of elements deleted. */
unsigned long fbtreeDeleteRangeByScore(fbtreeIndex *fbt,
                                       const char *min_score,
                                       const char *max_score,
                                       int min_ex,
                                       int max_ex,
                                       void (*callback)(sds item, void *ctx),
                                       void *callback_ctx) {
    if (!fbt->root) return 0;

    /* Delete-all short-circuit */
    sds first = leafNodeLowKey(fbt->leftmost_leaf);
    sds last = leafNodeHighKey(fbt->rightmost_leaf);
    int min_covers = min_ex ? memcmp(min_score, first, SCORE_SIZE) < 0 : memcmp(min_score, first, SCORE_SIZE) <= 0;
    int max_covers = max_ex ? memcmp(max_score, last, SCORE_SIZE) > 0 : memcmp(max_score, last, SCORE_SIZE) >= 0;
    if (min_covers && max_covers) {
        unsigned long count = fbtreeLength(fbt);
        fbtreeDeleteAll(fbt, callback, callback_ctx);
        return count;
    }

    /* Quick check: empty range */
    int range_cmp = memcmp(min_score, max_score, SCORE_SIZE);
    if (range_cmp > 0 || (range_cmp == 0 && (min_ex || max_ex))) return 0;

    /* Descend once to build the boundary paths, then delete. */
    BoundaryPaths bp;
    bool same_leaf = buildBoundaryPaths(fbt, &bp, min_score, max_score, findChildByScoreWrapper);
    bp.start_idx = resolveStartIdx(bp.start_leaf, min_score, min_ex, leafCmpByScore);
    bp.end_idx = resolveEndIdx(bp.end_leaf, max_score, max_ex, leafCmpByScore);
    return same_leaf ? deleteRangeSameLeaf(fbt, &bp, callback, callback_ctx)
                     : deleteRangeCore(fbt, &bp, callback, callback_ctx);
}

/* Delete elements with value in [min_val, max_val] using full sds comparison.
 * min_ex/max_ex: if true, the corresponding bound is exclusive.
 * If callback is non-NULL, it is invoked for each deleted item before sdsfree.
 * Returns the number of elements deleted. */
unsigned long fbtreeDeleteRangeByValue(fbtreeIndex *fbt,
                                       const_sds min_val,
                                       const_sds max_val,
                                       int min_ex,
                                       int max_ex,
                                       void (*callback)(sds item, void *ctx),
                                       void *callback_ctx) {
    if (!fbt->root) return 0;

    /* Delete-all short-circuit */
    sds first = leafNodeLowKey(fbt->leftmost_leaf);
    sds last = leafNodeHighKey(fbt->rightmost_leaf);
    int min_covers = min_ex ? sdscmp(min_val, first) < 0 : sdscmp(min_val, first) <= 0;
    int max_covers = max_ex ? sdscmp(max_val, last) > 0 : sdscmp(max_val, last) >= 0;
    if (min_covers && max_covers) {
        unsigned long count = fbtreeLength(fbt);
        fbtreeDeleteAll(fbt, callback, callback_ctx);
        return count;
    }

    /* Quick check: empty range */
    int range_cmp = sdscmp(min_val, max_val);
    if (range_cmp > 0 || (range_cmp == 0 && (min_ex || max_ex))) return 0;

    /* Descend once to build the boundary paths, then delete. */
    BoundaryPaths bp;
    bool same_leaf = buildBoundaryPaths(fbt, &bp, min_val, max_val, findChildByValueWrapper);
    bp.start_idx = resolveStartIdx(bp.start_leaf, min_val, min_ex, leafCmpByValue);
    bp.end_idx = resolveEndIdx(bp.end_leaf, max_val, max_ex, leafCmpByValue);
    return same_leaf ? deleteRangeSameLeaf(fbt, &bp, callback, callback_ctx)
                     : deleteRangeCore(fbt, &bp, callback, callback_ctx);
}

/* Count elements with score prefix in [min_score, max_score] via a single
 * shared descent (see buildBoundaryPaths), then rank arithmetic:
 * count = end_rank - start_rank. min_ex/max_ex mark the corresponding bound
 * exclusive. Score is an 8-byte big-endian normalized prefix (as stored). */
unsigned long fbtreeCountRangeByScore(fbtreeIndex *fbt,
                                      const char *min_score,
                                      const char *max_score,
                                      int min_ex,
                                      int max_ex) {
    if (!fbt->root) return 0;

    /* Whole-tree short-circuit */
    sds first = leafNodeLowKey(fbt->leftmost_leaf);
    sds last = leafNodeHighKey(fbt->rightmost_leaf);
    int min_covers = min_ex ? memcmp(min_score, first, SCORE_SIZE) < 0 : memcmp(min_score, first, SCORE_SIZE) <= 0;
    int max_covers = max_ex ? memcmp(max_score, last, SCORE_SIZE) > 0 : memcmp(max_score, last, SCORE_SIZE) >= 0;
    if (min_covers && max_covers) return fbtreeLength(fbt);

    /* Empty range */
    int range_cmp = memcmp(min_score, max_score, SCORE_SIZE);
    if (range_cmp > 0 || (range_cmp == 0 && (min_ex || max_ex))) return 0;

    BoundaryPaths bp;
    bool same_leaf = buildBoundaryPaths(fbt, &bp, min_score, max_score, findChildByScoreWrapper);
    if (same_leaf) {
        /* One leaf: nothing to overlap, resolve directly. */
        bp.start_idx = resolveStartIdx(bp.start_leaf, min_score, min_ex, leafCmpByScore);
        bp.end_idx = resolveEndIdx(bp.end_leaf, max_score, max_ex, leafCmpByScore);
    } else {
        /* Two leaves: software-pipeline the searches so both misses overlap. */
        resolveBothIdxPrefetch(bp.start_leaf, min_score, min_ex, bp.end_leaf, max_score, max_ex,
                               leafCmpByScore, &bp.start_idx, &bp.end_idx);
    }

    /* start_idx is the first in-range element; end_idx is the last in-range
     * element (inclusive), so its one-past position is end_idx + 1. */
    unsigned long start_rank = rankFromBoundaryPath(&bp, false, bp.start_idx);
    unsigned long end_rank = rankFromBoundaryPath(&bp, true, bp.end_idx + 1);
    return end_rank > start_rank ? end_rank - start_rank : 0;
}

/* Count elements with packed value in [min_val, max_val] via a single shared
 * descent (see buildBoundaryPaths), then rank arithmetic. min_ex/max_ex mark the
 * corresponding bound exclusive. Values are full packed [score][element] sds. */
unsigned long fbtreeCountRangeByValue(fbtreeIndex *fbt,
                                      const_sds min_val,
                                      const_sds max_val,
                                      int min_ex,
                                      int max_ex) {
    if (!fbt->root) return 0;

    /* Whole-tree short-circuit */
    sds first = leafNodeLowKey(fbt->leftmost_leaf);
    sds last = leafNodeHighKey(fbt->rightmost_leaf);
    int min_covers = min_ex ? sdscmp(min_val, first) < 0 : sdscmp(min_val, first) <= 0;
    int max_covers = max_ex ? sdscmp(max_val, last) > 0 : sdscmp(max_val, last) >= 0;
    if (min_covers && max_covers) return fbtreeLength(fbt);

    /* Empty range */
    int range_cmp = sdscmp(min_val, max_val);
    if (range_cmp > 0 || (range_cmp == 0 && (min_ex || max_ex))) return 0;

    BoundaryPaths bp;
    bool same_leaf = buildBoundaryPaths(fbt, &bp, min_val, max_val, findChildByValueWrapper);
    if (same_leaf) {
        /* One leaf: nothing to overlap, resolve directly. */
        bp.start_idx = resolveStartIdx(bp.start_leaf, min_val, min_ex, leafCmpByValue);
        bp.end_idx = resolveEndIdx(bp.end_leaf, max_val, max_ex, leafCmpByValue);
    } else {
        /* Two leaves: software-pipeline the searches so both misses overlap. */
        resolveBothIdxPrefetch(bp.start_leaf, min_val, min_ex, bp.end_leaf, max_val, max_ex,
                               leafCmpByValue, &bp.start_idx, &bp.end_idx);
    }

    unsigned long start_rank = rankFromBoundaryPath(&bp, false, bp.start_idx);
    unsigned long end_rank = rankFromBoundaryPath(&bp, true, bp.end_idx + 1);
    return end_rank > start_rank ? end_rank - start_rank : 0;
}

/* ========== Debug Functions ========== */

typedef struct {
    bool valid;
    size_t size;
    leafNode *leftmost_leaf;
    leafNode *rightmost_leaf;
} validateResult;

static void printBinaryString(const_sds s) {
    size_t len = sdslen(s);
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 32 && c < 127) {
            printf("%c", c);
        } else {
            printf("\033[2m%02x\033[0m", (unsigned char)c);
        }
    }
}

static void printIndent(int depth) {
    for (int j = 0; j < depth; j++) printf("│  ");
}

/* First-failure error reporting for validation: validateFail() writes a
 * detailed message into ctx (if provided) for the first failed check only,
 * preserving the deepest/earliest failure as later checks fail in cascade. */
typedef struct {
    char *errmsg;
    size_t errmsg_len;
} validateErrCtx;

static void validateFail(validateErrCtx *err, const char *fmt, ...) {
    if (!err || !err->errmsg || err->errmsg[0] != '\0') return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->errmsg, err->errmsg_len, fmt, ap);
    va_end(ap);
}

static validateResult validateNode(node *n, int depth, bool verbose, validateErrCtx *err);

static validateResult validateLeaf(leafNode *leaf, int depth, bool verbose, validateErrCtx *err) {
    size_t count = leaf->header.num_items;
    bool valid = (count <= NODE_SIZE);
    if (!valid) validateFail(err, "leaf at depth %d: num_items %zu exceeds NODE_SIZE %d", depth, count, NODE_SIZE);

    if (verbose) {
        printf(" Leaf (%zu items)\n", count);
        if (!valid) printf(" \033[31m[num_items %zu exceeds NODE_SIZE %d]\033[0m", count, NODE_SIZE);
        printf("\n");

        for (uint32_t i = 0; i < count; i++) {
            if (i % 8 == 0) {
                printIndent(depth);
                printf("├─");
            }
            printBinaryString(leaf->values[i]);
            printf(" ");
            if (i % 8 == 7) printf("\n");
        }
        if (count > 0 && count % 8 != 0) printf("\n");
    }
    return (validateResult){.valid = valid, .size = count, .leftmost_leaf = leaf, .rightmost_leaf = leaf};
}

static validateResult validateInner(innerNode *inner, int depth, bool verbose, validateErrCtx *err) {
    bool valid = true;
    size_t total_size = 0;
    leafNode *leftmost = NULL;
    leafNode *rightmost = NULL;

    if (verbose) {
        printf(" Inner (prefix=%zu, keys=%d)\n", inner->prefix_len, inner->header.num_items);
    }

    for (int i = 0; i < inner->header.num_items; i++) {
        const_sds anchor = inner->anchors[i];
        node *child = inner->children[i];

        /* Validate anchor starts with prefix */
        const char *prefix = innerNodeGetPrefix(inner);
        bool prefix_ok = sdslen(anchor) >= inner->prefix_len &&
                         memcmp(anchor, prefix, inner->prefix_len) == 0;

        /* Validate anchor matches child's high key */
        const_sds expected = nodeHighKey(child);
        bool anchor_ok = (expected == anchor);

        /* Validate feature matches anchor */
        bool feature_ok = true;
        for (int j = 0; j < FEATURE_SIZE && feature_ok; j++)
            feature_ok = (inner->features[j][i] == getFeatureByte(anchor, inner->prefix_len, j));

        /* Recursively validate child and get its size */
        if (verbose) {
            printIndent(depth);
            printf("\u251c\u2500[%02d] size=%zu anchor=", i, inner->child_sizes[i]);
            printBinaryString(anchor);
        }

        validateResult child_result = validateNode(child, depth + 1, verbose, err);

        /* Track leftmost/rightmost leaves */
        if (i == 0) leftmost = child_result.leftmost_leaf;
        rightmost = child_result.rightmost_leaf;

        /* Validate stored size matches actual size */
        bool size_ok = (inner->child_sizes[i] == child_result.size);

        /* Validate child_num_items matches child's actual num_items */
        bool num_items_ok = (inner->child_num_items[i] == inner->children[i]->num_items);

        if (!prefix_ok) validateFail(err, "inner at depth %d child %d: anchor missing node prefix (prefix_len %zu)", depth, i, inner->prefix_len);
        if (!anchor_ok) validateFail(err, "inner at depth %d child %d: anchor does not match child high key", depth, i);
        if (!feature_ok) validateFail(err, "inner at depth %d child %d: feature bytes do not match anchor", depth, i);
        if (!size_ok) validateFail(err, "inner at depth %d child %d: stored subtree size %zu != actual %zu", depth, i, inner->child_sizes[i], child_result.size);
        if (!num_items_ok) validateFail(err, "inner at depth %d child %d: stored child num_items %u != actual %u", depth, i, (unsigned)inner->child_num_items[i], (unsigned)inner->children[i]->num_items);

        valid = valid && prefix_ok && anchor_ok && feature_ok && size_ok && num_items_ok && child_result.valid;
        total_size += child_result.size;

        if (verbose && (!prefix_ok || !anchor_ok || !feature_ok || !size_ok || !num_items_ok)) {
            printIndent(depth);
            printf("   \033[31m");
            if (!prefix_ok) printf("prefix ");
            if (!anchor_ok) printf("anchor ");
            if (!feature_ok) printf("feature ");
            if (!size_ok) printf("size(%zu!=%zu) ", inner->child_sizes[i], child_result.size);
            if (!num_items_ok) printf("num_items(%u!=%u) ", inner->child_num_items[i], inner->children[i]->num_items);
            printf("FAIL\033[0m\n");
        }
    }
    return (validateResult){.valid = valid, .size = total_size, .leftmost_leaf = leftmost, .rightmost_leaf = rightmost};
}

static validateResult validateNode(node *n, int depth, bool verbose, validateErrCtx *err) {
    if (!n) return (validateResult){.valid = true, .size = 0};

    if (n->is_leaf) {
        return validateLeaf((leafNode *)n, depth, verbose, err);
    } else {
        return validateInner((innerNode *)n, depth, verbose, err);
    }
}

bool fbtreeDebugValidate(fbtreeIndex *fbt, bool verbose, char *errmsg, size_t errmsg_len) {
    if (errmsg && errmsg_len > 0) errmsg[0] = '\0';
    validateErrCtx err_ctx = {errmsg, errmsg_len};
    validateErrCtx *err = (errmsg && errmsg_len > 0) ? &err_ctx : NULL;

    unsigned long length = fbt->root ? getSubtreeSize(fbt->root) : 0;
    if (verbose) printf("FBTree (length=%lu)\n", length);
    if (!fbt->root) {
        /* Empty tree: caches must be NULL */
        if (fbt->leftmost_leaf || fbt->rightmost_leaf) {
            if (verbose) printf("\033[31mERROR: empty tree has non-NULL leaf cache\033[0m\n");
            validateFail(err, "empty tree has non-NULL leaf cache");
            return false;
        }
        return true;
    }

    validateResult result = validateNode(fbt->root, 0, verbose, err);

    /* Also verify total size matches computed length */
    bool length_ok = (result.size == length);
    if (!length_ok) {
        if (verbose) printf("\033[31mERROR: tree size %zu != computed length %lu\033[0m\n", result.size, length);
        validateFail(err, "recounted tree size %zu != root subtree size %lu", result.size, length);
    }

    /* Verify leaf caches point to actual leftmost/rightmost leaves */
    leafNode *actual_leftmost = result.leftmost_leaf;
    leafNode *actual_rightmost = result.rightmost_leaf;
    bool caches_ok = (fbt->leftmost_leaf == actual_leftmost && fbt->rightmost_leaf == actual_rightmost);
    if (!caches_ok) {
        if (verbose) printf("\033[31mERROR: leaf cache mismatch (leftmost: %p vs %p, rightmost: %p vs %p)\033[0m\n",
                            (void *)fbt->leftmost_leaf, (void *)actual_leftmost,
                            (void *)fbt->rightmost_leaf, (void *)actual_rightmost);
        validateFail(err, "leaf cache mismatch (leftmost %p vs actual %p, rightmost %p vs actual %p)",
                     (void *)fbt->leftmost_leaf, (void *)actual_leftmost,
                     (void *)fbt->rightmost_leaf, (void *)actual_rightmost);
    }

    return result.valid && length_ok && caches_ok;
}

/* ========== Defrag / Dismiss ========== */

unsigned long fbtreeDefragScan(fbtreeIndex *fbt, unsigned long cursor, void (*item_callback)(sds old_item, sds new_item, void *ctx), void *ctx, void *(*defragfn)(void *)) {
    if (!fbt || !fbt->root || fbtreeLength(fbt) == 0) return 0;
    if (cursor >= fbtreeLength(fbt)) return 0;

    /* Descend to the leaf holding rank 'cursor', recording the path so a
     * relocated leaf struct can be repointed from its parent and a relocated
     * high-key item re-published to the ancestor anchors that alias it.
     * Processing one leaf per descent keeps each step's work bounded to a
     * single contiguous node (prefetcher-friendly) and gives a fresh path per
     * call; the caller resumes via the returned cursor. O(log N) descent. */
    node *current = fbt->root;
    unsigned long remaining = cursor;
    innerNode *path[MAX_TREE_DEPTH];
    int path_idx[MAX_TREE_DEPTH];
    int depth = 0;

    while (!current->is_leaf) {
        innerNode *inner = (innerNode *)current;
        int i = 0;
        while (i < inner->header.num_items && remaining >= inner->child_sizes[i]) {
            remaining -= inner->child_sizes[i];
            i++;
        }
        if (i >= inner->header.num_items) return 0;
        path[depth] = inner;
        path_idx[depth] = i;
        depth++;
        current = inner->children[i];
    }

    leafNode *leaf = (leafNode *)current;

    /* Relocate the inner nodes whose leftmost descendant leaf is this one:
     * those where the child index taken at that node and at every level
     * beneath it is zero. Each inner node has exactly one leftmost leaf, so a
     * full sweep attempts every inner node exactly once, and each call
     * relocates at most tree-depth nodes, keeping per-call latency bounded.
     * Bottom-up, so a relocated node's verbatim children array already
     * carries any updated pointer to the node relocated just before it. */
    for (int d = depth - 1; d >= 0 && path_idx[d] == 0; d--) {
        innerNode *inner = path[d];
        /* A spilled long prefix lives in its own heap block; relocate it
         * before the node so the node's verbatim copy carries the updated
         * pointer. */
        if (innerNodeHasLongPrefix(inner)) {
            char **pptr = (char **)&inner->embedded_prefix[LONG_PREFIX_PTR_OFFSET];
            void *newprefix = defragfn(*pptr);
            if (newprefix) *pptr = (char *)newprefix;
        }
        void *newnode = defragfn(inner);
        if (!newnode) continue;
        path[d] = (innerNode *)newnode;
        if (d > 0)
            path[d - 1]->children[path_idx[d - 1]] = (node *)newnode;
        else
            fbt->root = (node *)newnode;
    }

    /* Relocate the leaf struct itself before touching its items, patching every
     * external reference: the parent's child pointer (or the tree root when the
     * leaf is the whole tree), the leaf-chain neighbors, and the
     * leftmost/rightmost caches. A leaf is a plain allocation, so its base is
     * the struct pointer and its contents (chain links, values) copy verbatim. */
    void *old_leaf = leaf;
    void *new_leaf = defragfn(old_leaf);
    if (new_leaf) {
        leaf = (leafNode *)new_leaf;
        if (leaf->prev) leaf->prev->next = leaf;
        if (leaf->next) leaf->next->prev = leaf;
        if (depth > 0)
            path[depth - 1]->children[path_idx[depth - 1]] = (node *)leaf;
        else
            fbt->root = (node *)leaf;
        if (fbt->leftmost_leaf == (leafNode *)old_leaf) fbt->leftmost_leaf = leaf;
        if (fbt->rightmost_leaf == (leafNode *)old_leaf) fbt->rightmost_leaf = leaf;
    }

    int start = (int)remaining;
    int last = leaf->header.num_items - 1;
    unsigned long processed = 0;

    for (int i = start; i < leaf->header.num_items; i++) {
        sds old_item = leaf->values[i];
        void *ptr = sdsAllocPtr(old_item);
        void *newptr = defragfn(ptr);
        if (newptr) {
            sds new_item = (char *)newptr + (old_item - (char *)ptr);
            leaf->values[i] = new_item;
            /* Each inner node stores the high key of every child as that
             * child's anchor (by pointer), so the leaf's last item is the
             * anchor for this leaf in its direct parent. Re-publish the moved
             * pointer to that parent, then climb while this leaf is also the
             * rightmost leaf of the enclosing subtree (the child taken at the
             * level below was its parent's last), since then it is that
             * ancestor's high key too. */
            if (i == last && depth > 0) {
                path[depth - 1]->anchors[path_idx[depth - 1]] = new_item;
                for (int d = depth - 2; d >= 0; d--) {
                    if (path_idx[d + 1] != path[d + 1]->header.num_items - 1) break;
                    path[d]->anchors[path_idx[d]] = new_item;
                }
            }
            item_callback(old_item, new_item, ctx);
        }
        processed++;
    }

    unsigned long next_rank = cursor + processed;
    return (next_rank >= fbtreeLength(fbt)) ? 0 : next_rank;
}

/* Walk the inner nodes above the leaf level, dismissing each node struct and
 * any spilled long-prefix block. The spilled prefix is the inner-node
 * allocation that can span whole pages; the node struct itself is dismissed
 * the same way leaf structs are. Children are dismissed before their parent
 * so the walk never reads a node after hinting it away. */
static void dismissInnerNodes(node *n) {
    if (n->is_leaf) return;
    innerNode *inner = (innerNode *)n;
    if (innerNodeHasLongPrefix(inner)) {
        char **pptr = (char **)&inner->embedded_prefix[LONG_PREFIX_PTR_OFFSET];
        zmadvise_dontneed(*pptr, inner->prefix_len);
    }
    for (int i = 0; i < inner->header.num_items; i++) {
        dismissInnerNodes(inner->children[i]);
    }
    zmadvise_dontneed(inner, 0);
}

void fbtreeDismissMemory(fbtreeIndex *fbt) {
    if (!fbt) return;
    if (fbt->root) dismissInnerNodes(fbt->root);
    leafNode *leaf = fbt->leftmost_leaf;
    while (leaf) {
        leafNode *next = leaf->next;
        /* The packed items are allocated separately from the leaf node.
         * With large members they are the allocations that span whole
         * pages, so each item is dismissed along with the leaf. */
        for (int i = 0; i < leaf->header.num_items; i++) {
            sds v = leaf->values[i];
            zmadvise_dontneed(sdsAllocPtr(v), sdsAllocSize(v));
        }
        zmadvise_dontneed(leaf, 0);
        leaf = next;
    }
}

size_t fbtreeEstimateStructureMemory(fbtreeIndex *fbt) {
    unsigned long len = fbtreeLength(fbt);
    size_t size = sizeof(fbtreeIndex);
    if (len == 0) return size;

    /* Leaf count is exact for small chains; for larger trees it is
     * extrapolated from the fill of the first leaves, which reflects how
     * sparse deletes have left the tree. */
    enum { FILL_SAMPLE_LEAVES = 16 };
    size_t leaves_seen = 0, items_seen = 0;
    leafNode *leaf = fbt->leftmost_leaf;
    while (leaf && leaves_seen < FILL_SAMPLE_LEAVES) {
        items_seen += leaf->header.num_items;
        leaves_seen++;
        leaf = leaf->next;
    }
    size_t est_leaves = (leaf == NULL) ? leaves_seen : (size_t)(len / ((double)items_seen / leaves_seen)) + 1;
    size += est_leaves * sizeof(leafNode);
    /* Inner levels shrink geometrically by the fanout. */
    size += (est_leaves / (NODE_SIZE - 1) + 1) * sizeof(innerNode);
    return size;
}
