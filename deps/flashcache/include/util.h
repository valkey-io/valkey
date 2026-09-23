#ifndef __FLASHCACHE_UTIL_H
#define __FLASHCACHE_UTIL_H

#include "include/flashcache_common.h"
#include "include/log_entry.h"

#define UNUSED(x) (void)(x)
#define FC_PAGESIZE (4096)
#define FC_SECOND_TO_MICROSECOND (1000000)
#define FC_MAX_HEX_DUMP_LEN (60 * 1024)  // Maximum size of hex dump
#define FC_MIN(x, y) ((x < y) ? x : y)
#define FC_MAX(x, y) ((x > y) ? x : y)

extern flashcacheAsioControlMsgCallbackDetails asio_control_msg_callback;
extern flashcache_logger logger;

#define flashcacheLogger(level, fmt, ...) do {\
    if (logger == NULL) break;\
    logger(level, "[FLASHCACHE] "fmt"", ##__VA_ARGS__);\
  } while (0)

#define flashcacheAssertWithLogging(expression, fmt, ...) do {\
    int e = (expression);\
    if (e) break;\
    flashcacheLogger(FC_LL_WARNING, fmt, ##__VA_ARGS__);\
    flashcacheAssert(e);\
  } while (0)

#define flashcacheAssertHandledCrashWithLogging(expression, fmt, ...) do {\
    int e = (expression);\
    if (e) break;\
    flashcacheLogger(FC_LL_WARNING, fmt, ##__VA_ARGS__);\
    flashcacheAssertHandledCrash(e);\
  } while (0)

void flashcacheAssert(int expression);

void flashcacheAssertHandledCrash(int expression);

// Hash function used for storing the key in a hash table inside flashcache.
typedef uint64_t (*flashcache_hash_function)(char const *key, size_t key_len);

// CRC function used for computing the CRC of the provided character buffer
typedef uint32_t (*flashcache_crc_function)(uint32_t start_crc, char const *buf, size_t buf_len);

static inline size_t getFloorPageAlignedOffset(size_t offset) {
    return (offset / FC_PAGESIZE * FC_PAGESIZE);
}

static inline size_t getCeilPageAlignedOffset(size_t offset) {
    // If offset is not evently divisible by the FC_PAGESIZE
    if (offset % FC_PAGESIZE) {
        return (((offset / FC_PAGESIZE) + 1) * FC_PAGESIZE);
    }
    return offset;
}

// Function that trims the provided offset to remove the least significant bits that are known to
// be 0 as the offset is aligned.
static inline size_t trimLogOffset(size_t offset) {
    flashcacheAssert(offset % FC_ITEM_ALIGNMENT_BYTES == 0);
    return offset / FC_ITEM_ALIGNMENT_BYTES;
}

// Function that returns the actual offset of the item in log by adding back the trimmed bits.
static inline size_t expandTrimmedLogOffset(size_t trimmed_offset) {
    return trimmed_offset * FC_ITEM_ALIGNMENT_BYTES;
}

// Return 1 if the provided value is a power of 2 else returns 0
int isPowerOf2(size_t v);

// Returns the number of bits required to hold the given number of elements
int getNumBitsRequired(size_t num);

size_t computeIndexHash(size_t hash, size_t collision_bits_used, size_t base_size_bits);

size_t computeCollisionHash(flashcache_hash_function hash_function,
        char const *key, size_t key_len);

void flashcacheLogHexDump(int level, char const *descr, char *value, size_t len);

// Returns active memory usage.
size_t getCurrentMemoryUsage();

// Get the actual block or file size in bytes
size_t getFileSize(char const *filename);

/**
 * Wrapper function for malloc.
 * @param size : Size of memory to allocate
 * @return Pointer to newly allocated memory.
 */
void *fcMalloc(size_t size);

/**
 * Wrapper function for calloc.
 * @param block_count : No. of Contiguous block to allocated.
 * @param size : Size of memory to allocate.
 * @return Pointer to newly allocated memory.
 */
void *fcCalloc(size_t block_count, size_t size);

/**
 * Wrapper function for realloc.
 * @param ptr : pointer to the allocated memory which needs to be reallocated.
 * @param size : Size of memory to allocate.
 * @return Pointer to newly allocated memory.
 */
void *fcRealloc(void *ptr, size_t size);

/**
 * Wrapper function for free.
 * @param ptr : pointer to the allocated memory which needs to be deleted.
 */
void fcFree(void *ptr);

/**
 * Updates current_memory_usage without actually freeing the memory
 * @param ptr : pointer to the allocated memory.
 */
void fcPseudoFree(void *ptr);

/**
 * Wrapper function for posix_memalign.
 * Returns a Pointer to newly allocated memory.
 */
void *fcPosixMemalign(size_t alignment, size_t size);

/**
 * Update the histogram interval with given value.
 * @param current_value : Value to be updated in histogram
 * @param histogram : Array which stores the latency frequency for all interval.
 * @param intervals : Defined interval for given histogram
 */
void updateHistogram(long long current_value, unsigned long long *histogram,
                     flashcacheHistogramInterval *intervals);

/***
 * Align the offset of the item to read with the page size
 * @param item_len: the length of the item to read.
 * @param offset: the offset to read from.
 * @return the amount of bytes to read.
 */
size_t getPartialItemReadSizeBytes(size_t item_len, size_t offset);

/**
 * Create a buffer after aligning the requested size to page sizes.
 * @param buf_size the size to align to a page.
 * @return The buffer created.
 */
char *createPageAlignedBuffer(size_t buf_size);

#endif  // __FLASHCACHE_UTIL_H
