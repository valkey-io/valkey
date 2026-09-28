#include <stdlib.h>

#include "include/util.h"
#include "include/bitset.h"

#define BITS_PER_BYTE (8)

bitset *bitsetCreate(size_t size) {
    flashcacheAssert(size != 0);

    bitset *bset = (bitset *) fcMalloc(sizeof(bitset));
    flashcacheAssert(bset != NULL);

    size_t num_bytes = (size / BITS_PER_BYTE) + (size % BITS_PER_BYTE ? 1 : 0);
    bset->set = (uint8_t *) fcCalloc(sizeof(uint8_t), num_bytes);
    flashcacheAssert(bset->set != NULL);

    bset->size = size;
    return bset;
}

void bitsetSet(bitset *bset, size_t idx) {
    flashcacheAssert(idx < bset->size);

    int byte_position = idx / BITS_PER_BYTE;
    int bit_position = idx % BITS_PER_BYTE;

    bset->set[byte_position] |= (1 << bit_position);
}

uint8_t bitsetGet(bitset *bset, size_t idx) {
    flashcacheAssert(idx < bset->size);

    int byte_position = idx / BITS_PER_BYTE;
    int bit_position = idx % BITS_PER_BYTE;

    return ((bset->set[byte_position] & (1 << bit_position)) >> bit_position);
}

void bitsetRelease(bitset *bset) {
    if (bset == NULL) {
        return;
    }
    fcFree(bset->set);
    fcFree(bset);
}
