#include <stdarg.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <math.h>

#include "include/util.h"
#include <jemalloc/jemalloc.h>

// Defined in flashcache.c
void flashcacheLogState(int level);

flashcache_logger logger = NULL;

static size_t current_memory_usage = 0;

size_t getCurrentMemoryUsage() {
    return current_memory_usage;
}

int isPowerOf2(size_t v) {
    return ((v & (v - 1)) == 0);
}

/**
 * Returns the number of bits required to hold the given number of elements
 */
int getNumBitsRequired(size_t num) {
    flashcacheAssert(num > 0);
    return ceil(log(num) / log(2));
}

/**
 * Computes Index Hash. Lets understand it with an example, for eg:
 *
 * hash = 1011010001101101 0001101101000110 1101000110110100 0110110100011010  (64 bits)
 * Collision hash is first batch of 16 bits i.e 1011010001101101
 * collision_bits_used = 4
 * base_size_bits = 10
 * Expected Index hash = 1101 0001101101 (least significant 4 bits from collision hash + 10 bits from
 * next batch of 16 bits)
 *
 * Approach for getting expected index hash,
 * 1. Right shift the bits which we dont need for index hash i.e
 *    (64 - FC_LOG_ENTRY_COLLISION_HASHBITS - base_size_bits) bits which will provide hash = 1011010001101101 0001101101
 * 2. Get least significant 14(base_size_bits + collision_bits_used) bits from current hash,
 *    it will provide hash 1101 0001101101
 */
size_t computeIndexHash(size_t hash, size_t collision_bits_used, size_t base_size_bits) {
    flashcacheAssert((sizeof(hash) * 8) >= (FC_LOG_ENTRY_COLLISION_HASHBITS + base_size_bits));
    hash = hash >> ((sizeof(hash) * 8) - FC_LOG_ENTRY_COLLISION_HASHBITS - base_size_bits);
    hash = hash & ((1LL << (collision_bits_used + base_size_bits)) - 1);
    return hash;
}

size_t computeCollisionHash(flashcache_hash_function hash_function,
        char const *key, size_t key_len) {
    size_t hash = hash_function(key, key_len);
    flashcacheAssert((sizeof(hash) * 8) >= FC_LOG_ENTRY_COLLISION_HASHBITS);
    hash >>= ((sizeof(hash) * 8) - FC_LOG_ENTRY_COLLISION_HASHBITS);
    return hash;
}

// Assert that is used in this library to enable more logging when the assert
// fails. This assert will be used in production system and prevents calling
// application from accidentally turning it off using NDEBUG.
void flashcacheAssert(int expression) {
    if (!expression) {
        flashcacheLogState(FC_LL_WARNING);
        // Cause a seg fault if the expression is 0
        *((char *)-1L) = 'x';
    }
}

// Similar to flashcacheAssert, except meant to
// indicate crashes that are expected, and handled by HM.
void flashcacheAssertHandledCrash(int expression) {
    if (!expression) {
        flashcacheLogState(FC_LL_WARNING);
        // Cause a seg fault if the expression is 0
        *((char *)-1L) = 'x';
    }
}

// Used the hex dumper from Redis
void flashcacheLogHexDump(int level, char const *descr, char *value, size_t len) {
    char buf[FC_MAX_HEX_DUMP_LEN + 1], *b;
    unsigned char *v = (unsigned char *) value;
    char charset[] = "0123456789abcdef";

    if (len > FC_MAX_HEX_DUMP_LEN) {
        len = FC_MAX_HEX_DUMP_LEN;
    }

    flashcacheLogger(level, "%s (hexdump of %zu bytes):", descr, len);
    b = buf;
    while (len) {
        b[0] = charset[(*v)>>4];
        b[1] = charset[(*v)&0xf];
        b[2] = '\0';
        b += 2;
        len--;
        v++;
    }
    flashcacheLogger(level, "%s", buf);
}

// Wrapper function for malloc.
void *fcMalloc(size_t size) {
    flashcacheAssert(size > 0);
    void *ptr = malloc(size);
    if (ptr) {
        current_memory_usage += malloc_usable_size(ptr);
    }
    return ptr;
}

// Wrapper function for calloc.
void *fcCalloc(size_t block_count, size_t size) {
    flashcacheAssert((block_count * size) > 0);
    void *ptr = calloc(block_count, size);
    if (ptr) {
        current_memory_usage += malloc_usable_size(ptr);
    }
    return ptr;
}

// Wrapper function for realloc.
void *fcRealloc(void *ptr, size_t size) {
    flashcacheAssert(size > 0);
    size_t oldsize = 0;
    if (ptr) {
        oldsize = malloc_usable_size(ptr);
    }
    void *newptr = realloc(ptr, size);
    if (newptr) {
        flashcacheAssert(current_memory_usage >= oldsize);
        current_memory_usage -= oldsize;
        current_memory_usage += malloc_usable_size(newptr);
    }
    return newptr;
}

// Wrapper function for free.
void fcFree(void *ptr) {
    flashcacheAssert(ptr != NULL);
    size_t size = malloc_usable_size(ptr);
    free(ptr);
    flashcacheAssert(current_memory_usage >= size);
    current_memory_usage -= size;
}

// Updates current_memory_usage without actually freeing the memory
void fcPseudoFree(void *ptr) {
    flashcacheAssert(ptr != NULL);
    size_t size = malloc_usable_size(ptr);
    flashcacheAssert(current_memory_usage >= size);
    current_memory_usage -= size;
}

// Wrapper function for posix_memalign.
void *fcPosixMemalign(size_t alignment, size_t size) {
    flashcacheAssert(size > 0);
    void *memptr = NULL;
    int ret = posix_memalign((void**) &memptr, alignment, size);
    (void)(ret);
    flashcacheAssert(memptr != NULL);
    current_memory_usage += malloc_usable_size(memptr);
    return memptr;
}

// Update the histogram interval with given value.
void updateHistogram(long long current_value, unsigned long long * histogram,
                     flashcacheHistogramInterval *intervals) {
    unsigned int index = 0;
    do {
        if (current_value <= intervals[index].interval_end) {
            histogram[index]++;
            return;
        }
    } while (intervals[index++].interval_end != LLONG_MAX);
    // Should never reach here, there must be a bucket for each value
    flashcacheAssertWithLogging(0, "Histogram value %lld does not have a bucket\n", current_value);
}

size_t getFileSize(char const *filename) {
    int fd, stat_val, ioctl_val;
    int syscall_retries = 10;
    struct stat stat_buf;
    unsigned long block_size = 0;

    stat_val = stat(filename, &stat_buf);
    flashcacheAssertWithLogging(stat_val != -1,
                                "getFileSize(): stat() failed, errno=%d, errmsg=%s\n",
                                errno, strerror(errno));

    // Check if path is a block, if so use ioctl() instead
    if ((stat_buf.st_mode & S_IFMT) == S_IFBLK) {
        while ((fd = open(filename, O_RDONLY)) == -1) {
            if (errno != EINTR || syscall_retries-- == 0) {
                flashcacheAssertWithLogging(0, "getFileSize(): open() failed, errno=%d, errmsg=%s\n",
                                            errno, strerror(errno));
            }
        }

        ioctl_val = ioctl(fd, BLKGETSIZE64, &block_size);
        flashcacheAssertWithLogging(ioctl_val != -1,
                                    "getFileSize(): ioctl() failed, errno=%d, errmsg=%s\n",
                                    errno, strerror(errno));

        close(fd);
        return block_size;
    }
    return stat_buf.st_size;
}

size_t getPartialItemReadSizeBytes(size_t item_len, size_t offset) {
    // Blocksize should accomodate the additional data to be read if the item is in the middle
    // of the page. If the offset is more than the page size remove extra bytes and add the item length
    // to generate the expected size of bytes to read.
    size_t blocksize = (offset - getFloorPageAlignedOffset(offset)) + item_len;
    return blocksize;
}

char *createPageAlignedBuffer(size_t buf_size) {
    return fcPosixMemalign(FC_PAGESIZE, buf_size);
}
