#include "minheap.h"
#include "zmalloc.h"
#include <assert.h>

#define MINHEAP_INITIAL_CAPACITY 8

static void swap(minheapNode *a, minheapNode *b) {
    minheapNode tmp = *a;
    *a = *b;
    *b = tmp;
}

static void swim(minheap *heap, unsigned long idx) {
    while (idx > 1) {
        unsigned long parent = idx / 2;
        if (heap->nodes[idx].key < heap->nodes[parent].key) {
            swap(&heap->nodes[idx], &heap->nodes[parent]);
            idx = parent;
        } else {
            break;
        }
    }
}

static void sink(minheap *heap, unsigned long idx) {
    while (1) {
        unsigned long smallest = idx;
        unsigned long left = 2 * idx;
        unsigned long right = 2 * idx + 1;

        if (left <= heap->len && heap->nodes[left].key < heap->nodes[smallest].key)
            smallest = left;
        if (right <= heap->len && heap->nodes[right].key < heap->nodes[smallest].key)
            smallest = right;

        if (smallest != idx) {
            swap(&heap->nodes[idx], &heap->nodes[smallest]);
            idx = smallest;
        } else {
            break;
        }
    }
}

minheap *minheapCreate(void) {
    minheap *heap = zmalloc(sizeof(minheap));
    heap->nodes = NULL;
    heap->len = 0;
    heap->capacity = 0;
    return heap;
}

void minheapRelease(minheap *heap) {
    if (heap->nodes) zfree(heap->nodes);
    zfree(heap);
}

void *minheapPeekMin(minheap *heap) {
    if (heap->len == 0) return NULL;
    return heap->nodes[1].value;
}

long long minheapPeekMinKey(minheap *heap) {
    assert(heap->len > 0);
    return heap->nodes[1].key;
}

void minheapInsert(minheap *heap, long long key, void *value) {
    if (heap->len + 1 >= heap->capacity) {
        unsigned long new_cap = heap->capacity == 0 ? MINHEAP_INITIAL_CAPACITY : heap->capacity * 2;
        heap->nodes = zrealloc(heap->nodes, new_cap * sizeof(minheapNode));
        heap->capacity = new_cap;
    }
    heap->len++;
    heap->nodes[heap->len].key = key;
    heap->nodes[heap->len].value = value;
    swim(heap, heap->len);
}

void *minheapExtractMin(minheap *heap) {
    if (heap->len == 0) return NULL;
    void *min_value = heap->nodes[1].value;
    heap->nodes[1] = heap->nodes[heap->len];
    heap->len--;
    if (heap->len > 0) {
        sink(heap, 1);
    }
    return min_value;
}

void *minheapGet(minheap *heap, unsigned long idx) {
    if (idx >= heap->len) return NULL;
    return heap->nodes[idx + 1].value;
}
