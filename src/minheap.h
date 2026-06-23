#ifndef __MINHEAP_H__
#define __MINHEAP_H__

/* Min-heap implementation. 1-indexed: root at nodes[1], nodes[0] unused. */

#include <stddef.h>

typedef struct minheapNode {
    long long key;
    void *value;
} minheapNode;

typedef struct minheap {
    minheapNode *nodes;
    unsigned long len;
    unsigned long capacity;
} minheap;

minheap *minheapCreate(void);
void minheapRelease(minheap *heap);
void minheapInsert(minheap *heap, long long key, void *value);
void *minheapExtractMin(minheap *heap);
void *minheapPeekMin(minheap *heap);
long long minheapPeekMinKey(minheap *heap);
void *minheapGet(minheap *heap, unsigned long idx);

#define minheapLen(h) ((h)->len)

#endif
