#ifndef __INTRINSICS_H
#define __INTRINSICS_H

#include <stdint.h>

/* Count the number of trailing zero bits in a 64-bit integer. */
static inline int32_t builtin_ctzll(uint64_t value) {
    if (value == 0) return 64;
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_ctzll(value);
#else
    int bitpos = 0;
    while (value & 1 == 0) {
        value >>= 1;
        ++bitpos;
    }
    return bitpos;
#endif
}

#endif /* __INTRINSICS_H */
