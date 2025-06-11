#include "array.h"

#include "serverassert.h"
#include "zmalloc.h"

void arrayInit(array *a, uint32_t alloc, size_t item_size) {
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

uint32_t arrayLen(array *a) {
    return a->len;
}

void *arrayGet(array *a, uint32_t idx) {
    assert(idx < a->len);
    return (uint8_t *)a->data + idx * a->item_size;
}

void *arrayPush(array *a) {
    if (a->len == a->alloc) {
        size_t alloc = a->alloc ? 2 * a->alloc : 8;
        a->data = zrealloc(a->data, alloc * a->item_size);
        a->alloc = alloc;
    }

    void *item = (uint8_t *)a->data + a->len * a->item_size;
    a->len++;
    return item;
}

void arrayCleanup(array *a) {
    if (a->data) {
        zfree(a->data);
    }
}
