#ifndef __FLASHCACHE_CRC_H
#define __FLASHCACHE_CRC_H

#include <stddef.h>
#include <stdint.h>

// Function to compute the CRC32c of the specified buffer
uint32_t flashcacheCrc32c(uint32_t crc, char const *buf, size_t len);

#endif  // __FLASHCACHE_CRC_H
