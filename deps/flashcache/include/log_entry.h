#ifndef __FLASHCACHE_LOG_ENTRY_H
#define __FLASHCACHE_LOG_ENTRY_H

#include <stdint.h>

#define FC_LOG_ENTRY_TOTAL_BITS            (64)
#define FC_LOG_ENTRY_ON_FLASH_BITS         (1)
#define FC_LOG_ENTRY_ADDITIONAL_PAGES_BITS (8)
#define FC_LOG_ENTRY_COLLISION_HASHBITS    (16)

#define FC_MAX_ADDITIONAL_PAGES ((1 << FC_LOG_ENTRY_ADDITIONAL_PAGES_BITS) - 1)
#define FC_ITEM_ALIGNMENT_BYTES (8)

// We keep 39 bits for storing the trimmed log offset. 3 bits are trimmed from the offset as its 8 byte aligned.
// This helps us support a maximum log size of 4 TiB (2^42).
#define FC_LOG_ENTRY_LOG_OFFSET_BITS       (FC_LOG_ENTRY_TOTAL_BITS - \
        FC_LOG_ENTRY_ON_FLASH_BITS - FC_LOG_ENTRY_ADDITIONAL_PAGES_BITS -\
        FC_LOG_ENTRY_COLLISION_HASHBITS)

typedef struct logEntry {
    /**
     * The on flash flag is used to determine if the item is on flash or in
     * memory. We use a union of staging buffer ptr and a log entry to store the
     * reference of the item in index entry. The most significant bit is used as
     * a tag to determine whether the item is a log entry or a staging buffer ptr.
     * The most significant bit is expected to be 0 in a memory pointer.
     * This is the reason we have the on_flash flag as the first bit in this
     * entry.
     */
    uint8_t on_flash               : FC_LOG_ENTRY_ON_FLASH_BITS;

    /**
     * Hash used to avoid collision when multiple entries are mapped to the same
     * hash bucket. This hash is also used to grow the hash table without the need
     * of fetch the key from flash and rehashing the key. The maximum size of the
     * hash table can be equal to the (initial size of hash table *
     * 2 ^ FC_LOG_ENTRY_COLLISION_HASHBITS).
     */
    size_t hash                    : FC_LOG_ENTRY_COLLISION_HASHBITS;

    // The number of additional pages that needs to be read inorder to read the item.
    size_t additional_pages        : FC_LOG_ENTRY_ADDITIONAL_PAGES_BITS;

    // As all the offsets are supposed to be a multiple of alignment bytes, we can avoid storing
    // the last few bits in the offset. trimmed log offset is the byte offset in the log where the
    // item is written divided by the alignment bytes.
    uint64_t trimmed_log_offset    : FC_LOG_ENTRY_LOG_OFFSET_BITS;
} logEntry;

#ifndef __cplusplus
// This causes compilation error when included in C++ code. Therefore we don't include
// this check when this header is included from C++ code.
_Static_assert(sizeof(logEntry) * 8 == FC_LOG_ENTRY_TOTAL_BITS,
        "Incorrect log entry size");
#endif
#endif  // __FLASHCACHE_LOG_ENTRY_H
