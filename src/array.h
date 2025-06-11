#ifndef __ARRAY_H_
#define __ARRAY_H_

#include <stdlib.h>
#include <stdint.h>

/* a generic dynamic array implementation */
typedef struct array {
    void *data;       // Pointer to the actual array data
    uint32_t alloc;   // Number of allocated items
    uint32_t len;     // Current number of used items
    size_t item_size; // Size of each element in bytes
} array;

/* API */
void arrayInit(array *a, uint32_t alloc, size_t item_size);
void *arrayGet(array *a, uint32_t idx);
uint32_t arrayLen(array *a);
void *arrayPush(array *a);
void arrayCleanup(array *a);

/* Usage example:
 *   array arr;
 *   arrayInit(&arr, 10, sizeof(int));  // Initialize for 10 integers
 *
 *   int* new_int = arrayPush(&arr);   // Add new element
 *   *new_int = 42;                    // Initialize value
 *
 *   int* val = arrayGet(&arr, 0);     // Access element
 *   printf("%d\n", *val);             // Output: 42
 *
 *   arrayCleanup(&arr);               // Release memory
 */
#endif
