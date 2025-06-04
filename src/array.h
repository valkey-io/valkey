#ifndef __ARRAY_H_
#define __ARRAY_H_

#include "zmalloc.h"

/* array.h - A generic dynamic array implementation.
 * Similar to SDS, the array's metadata and data are stored contiguously.
 * So the actual array layout is this one:
 * +-------------+-------------+---------+-----+-----------------------\
 * | len | alloc | free method | element | ... | element | Free space  \
 * +-------------+-------------+---------+-----+-----------------------\
 *                             |
 *                             `-> Pointer returned to the user.
 */

/* Metadata struct of array */
typedef struct array {
    unsigned long len;       /* Current length of the array */
    unsigned long alloc;     /* Allocated size of the array */
    void (*free)(void *ptr); /* Free method for each element */
    char data[];             /* Pointer returned to the user */
} array;

/* Helper Macros and Internal Functions */
#define ARRAY_META_SIZE sizeof(array)

/* Convert the data pointer to the array metadata pointer */
#define ARRAY_HDR(v) (((array *)(v)) - 1)

/* It is an internal function that expands the array before pushing each element
 * and return the data pointer */
static void *arrayGrowIfNeed(array *a, unsigned long item_size) {
    if (a->len < a->alloc) {
        a->len += 1;
        return a->data;
    }
    a->alloc = a->alloc ? a->alloc * 2 : 8;
    a = zrealloc(a, a->alloc * item_size + ARRAY_META_SIZE);
    a->len += 1;
    return a->data;
}

/* Exported APIs
 * arrayNew, arrayPush, and other APIs are implemented via macros,
 * which makes the code harder to read, primarily for two reasons:
 *   1. To obtain the element size at compile time, avoiding the
 *      need to store element size in the array's metadata;
 *   2. When used, it behaves more like a static array. For example,
 *      we can access individual elements via dynamic_array[index].
 */
#define arrayNew(v, s, f)                                                          \
    do {                                                                           \
        array *__array_meta_ptr__ = zmalloc(ARRAY_META_SIZE + sizeof(*(v)) * (s)); \
        __array_meta_ptr__->len = 0;                                               \
        __array_meta_ptr__->alloc = (s);                                           \
        __array_meta_ptr__->free = (f);                                            \
        v = (void *)(__array_meta_ptr__ + 1);                                      \
    } while (0)

#define arrayLen(v) (ARRAY_HDR(v)->len)

#define arrayVail(v) (ARRAY_HDR(v)->alloc - ARRAY_HDR(v)->len)

#define arrayFreeMethod(v) (ARRAY_HDR(v)->free)

#define arrayPush(v, e)                                  \
    do {                                                 \
        v = arrayGrowIfNeed(ARRAY_HDR(v), sizeof(*(v))); \
        v[ARRAY_HDR(v)->len - 1] = e;                    \
    } while (0)

#define arrayForeach(v, f)                                            \
    do {                                                              \
        if (!f) break;                                                \
        for (unsigned long __x__ = 0; __x__ < arrayLen(v); __x__++) { \
            (f)(v[__x__]);                                            \
        }                                                             \
    } while (0)

#define arrayFree(v)                         \
    do {                                     \
        if (!v) break;                       \
        arrayForeach(v, arrayFreeMethod(v)); \
        zfree(ARRAY_HDR(v));                 \
    } while (0)
#endif
