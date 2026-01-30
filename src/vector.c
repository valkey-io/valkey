#include "vector.h"

#include "serverassert.h"
#include "zmalloc.h"

/* Usage example:
 *   vector arr;
 *   vectorInit(&arr, 10, sizeof(int));  // Initialize for 10 integers
 *
 *   int* new_int = vectorPush(&arr);    // Add new element
 *   *new_int = 42;                      // Initialize value
 *
 *   int* val = vectorGet(&arr, 0);      // Access element
 *   printf("%d\n", *val);               // Output: 42
 *
 *   vectorCleanup(&arr);                // Release memory
 */

/* Initialize a dynamic array (vector). */
void vectorInit(vector *a, uint32_t alloc, size_t item_size) {
    assert(item_size);
    if (alloc) {
        a->data = zmalloc(alloc * item_size);
    } else {
        a->data = NULL;
    }
    a->alloc = alloc;
    a->len = 0;
    a->item_size = item_size;
}

/* Get current vector length. */
uint32_t vectorLen(vector *a) {
    return a->len;
}

/* Get pointer to element at specified index. */
void *vectorGet(vector *a, uint32_t idx) {
    assert(idx < a->len);
    return (uint8_t *)a->data + idx * a->item_size;
}

/* Adds a new uninitialized element to the array and returns a pointer to it. */
void *vectorPush(vector *a) {
    if (a->len == a->alloc) {
        uint32_t alloc = a->alloc ? 2 * a->alloc : 8;
        a->data = zrealloc(a->data, alloc * a->item_size);
        a->alloc = alloc;
    }

    void *item = (uint8_t *)a->data + a->len * a->item_size;
    a->len++;
    return item;
}

/* Clean up vector resources:
 * - Frees data memory if allocated (non-NULL).
 * - The vector structure (vector *a) itself is NOT freed here.
 */
void vectorCleanup(vector *a) {
    if (a->data) {
        zfree(a->data);
    }
}
