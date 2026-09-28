#include "include/crc.h"

#if defined(__x86_64__)
#include <smmintrin.h>

static inline uint32_t fc_crc32c_8(uint32_t x, uint8_t y) {
    return _mm_crc32_u8(x, y);
}

static inline uint32_t fc_crc32c_16(uint32_t x, uint16_t y) {
    return _mm_crc32_u16(x, y);
}

static inline uint32_t fc_crc32c_32(uint32_t x, uint32_t y) {
    return _mm_crc32_u32(x, y);
}

static inline uint32_t fc_crc32c_64(uint32_t x, uint64_t y) {
    return _mm_crc32_u64(x, y);
}

#elif defined(__aarch64__)
#include <arm_acle.h>
#define __ARM_FEATURE_CRC32 1

static inline uint32_t fc_crc32c_8(uint32_t x, uint8_t y) {
    return __crc32cb(x, y);
}

static inline uint32_t fc_crc32c_16(uint32_t x, uint16_t y) {
    return __crc32ch(x, y);
}

static inline uint32_t fc_crc32c_32(uint32_t x, uint32_t y) {
    return __crc32cw(x, y);
}

static inline uint32_t fc_crc32c_64(uint32_t x, uint64_t y) {
    return __crc32cd(x, y);
}

#endif

uint32_t flashcacheCrc32c(uint32_t crc, char const *buf, size_t len) {
    crc = crc ^ 0xffffffff;
    if (len & 1) {
        crc = fc_crc32c_8(crc, *buf);
        buf++;
        len--;
    }

    if (len & 2) {
        crc = fc_crc32c_16(crc, *((uint16_t const *) buf));
        buf += 2;
        len -= 2;
    }

    if (len & 4) {
        crc = fc_crc32c_32(crc, *((uint32_t const *) buf));
        buf += 4;
        len -= 4;
    }

    while (len) {
        crc = fc_crc32c_64(crc, *((uint64_t const *) buf));
        buf += 8;
        len -= 8;
    }
    return crc ^ 0xffffffff;
}
