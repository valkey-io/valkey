#ifndef __VECTOR_H_
#define __VECTOR_H_

#include <stdlib.h>
#include <stdint.h>

/* a generic dynamic array implementation */
typedef struct vector {
    void *data;       // Pointer to the actual array data
    uint32_t alloc;   // Number of allocated items
    uint32_t len;     // Current number of used items
    size_t item_size; // Size of each element in bytes
} vector;

/* API */
void vectorInit(vector *a, uint32_t alloc, size_t item_size);
void *vectorGet(vector *a, uint32_t idx);
uint32_t vectorLen(vector *a);
void *vectorPush(vector *a);
void vectorCleanup(vector *a);
#endif
