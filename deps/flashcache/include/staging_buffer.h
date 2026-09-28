#ifndef __FLASHCACHE_STAGING_BUFFER_H
#define __FLASHCACHE_STAGING_BUFFER_H

#include "include/log_entry.h"
#include "include/index.h"

/* StagingBufferEntry holds the serialized (key, value) pair which have not
 * yet been flushed to flash.
 */
typedef struct stagingBufferEntry {
    char *item;
    size_t item_len;
    size_t user_data;  // User state specific to the entry

    struct stagingBufferEntry *next;
    struct stagingBufferEntry *prev;
    struct indexEntry *index_entry;
    struct logEntry log_entry;
} stagingBufferEntry;

/* Staging buffer holds a linked list of the staging buffer entry. New items are
 * added to the head of the list.
 */
typedef struct stagingBuffer {
    // Head of the linked list
    struct stagingBufferEntry *head;

    // Tail of the linked list
    struct stagingBufferEntry *tail;

    // Total size of all the items in the linked list
    size_t total_item_size;
} stagingBuffer;

// Creates a new staging buffer.
struct stagingBuffer *stagingBufferCreate();

// Add a new item to the staging buffer.
struct stagingBufferEntry *stagingBufferAddItem(struct stagingBuffer *staging_buffer,
        char *item, size_t item_len, size_t user_data);

// Delete an entry from the staging buffer.
void stagingBufferDeleteEntry(struct stagingBuffer *staging_buffer,
        struct stagingBufferEntry *entry, int8_t free_item);

// Returns the head entry of the staging buffer linked list.
struct stagingBufferEntry *stagingBufferGetHead(struct stagingBuffer *staging_buffer);

// Returns the tail entry of the staging buffer linked list.
struct stagingBufferEntry *stagingBufferGetTail(struct stagingBuffer *staging_buffer);

// Returns the total size of all the items in the linked list.
size_t stagingBufferGetTotalItemSize(struct stagingBuffer *staging_buffer);

// Deletes all the resources hold by staging buffer and frees the staging buffer. The caller
// should not use the staging buffer after releasing it.
void stagingBufferRelease(struct stagingBuffer *staging_buffer);
#endif  // __FLASHCACHE_STAGING_BUFFER_H
