#ifndef __FLASHCACHE_HASH_H
#define __FLASHCACHE_HASH_H

#include <stddef.h>
#include <stdint.h>
#include "include/util.h"

#define FLASHCACHE_HASHER_SEED_SIZE (16)
#define FLASHCACHE_SIPHASH_HASHER (0)

// A hasher struct used for generating hash value for a specified key
typedef struct {
    // Initializes the hash function. If a seed is not provide, a random seed is
    // used for initialization.
    void (*init)(const uint8_t *seed);

    // Retrieves the current hash seed
    void (*get_seed)(uint8_t *seed);

    // Function that generates a hash value for the specified key
    flashcache_hash_function hash_function;

    // Retrieve the type of the hash
    uint32_t (*get_type)();
} flashcacheHasher;

// Retrieve a hasher by type
flashcacheHasher *flashcacheHasherGetByType(uint32_t type);

// Validates the provided hasher
void flashcacheHasherValidate(flashcacheHasher *hasher);

#endif  // __FLASHCACHE_HASH_H
