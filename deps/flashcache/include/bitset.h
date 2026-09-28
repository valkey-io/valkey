#ifndef __FLASHCACHE_BITSET_H
#define __FLASHCACHE_BITSET_H

#include <stddef.h>
#include <stdint.h>

typedef struct bitset {
    // byte array to store the value corresponding to each index
    uint8_t *set;

    // size of the bitset
    size_t size;
} bitset;

// Creates a new bitset
bitset *bitsetCreate(size_t size);

// Set the value at provided index to 1
void bitsetSet(bitset *bset, size_t idx);

// Get the value at the provided index, Return 1 if the value at index is set else returns 0
uint8_t bitsetGet(bitset *bset, size_t idx);

// Deletes all the resources used by bitset
void bitsetRelease(bitset *bset);

#endif  // __FLASHCACHE_BITSET_H
