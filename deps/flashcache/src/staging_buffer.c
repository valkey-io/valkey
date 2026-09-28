#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "include/util.h"
#include "include/staging_buffer.h"

stagingBuffer *stagingBufferCreate() {
    stagingBuffer *staging_buffer = (stagingBuffer *) fcMalloc(
            sizeof(stagingBuffer));
    flashcacheAssert(staging_buffer != NULL);

    staging_buffer->head = NULL;
    staging_buffer->tail = NULL;
    staging_buffer->total_item_size = 0;
    return staging_buffer;
}

stagingBufferEntry *stagingBufferAddItem(stagingBuffer *staging_buffer,
        char *item, size_t item_len, size_t user_data) {
    stagingBufferEntry *entry = (stagingBufferEntry *) fcMalloc(
            sizeof(stagingBufferEntry));
    flashcacheAssert(entry != NULL);

    entry->item = item;
    entry->item_len = item_len;
    entry->user_data = user_data;
    entry->next = staging_buffer->head;
    entry->prev = NULL;
    entry->index_entry = NULL;
    memset(&(entry->log_entry), 0, sizeof(logEntry));

    if (staging_buffer->head) {
        staging_buffer->head->prev = entry;
    } else {
        staging_buffer->tail = entry;
    }

    staging_buffer->head = entry;
    staging_buffer->total_item_size += item_len;
    return entry;
}

void stagingBufferDeleteEntry(stagingBuffer *staging_buffer,
        stagingBufferEntry *entry, int8_t free_item) {
    if (staging_buffer->head == entry) {
        staging_buffer->head = entry->next;
    }
    if (staging_buffer->tail == entry) {
        staging_buffer->tail = entry->prev;
    }

    if (entry->next) {
        entry->next->prev = entry->prev;
    }
    if (entry->prev) {
        entry->prev->next = entry->next;
    }

    staging_buffer->total_item_size -= entry->item_len;

    if (free_item) {
        fcFree(entry->item);
    }
    fcFree(entry);
}

stagingBufferEntry *stagingBufferGetHead(stagingBuffer *staging_buffer) {
    return staging_buffer->head;
}

stagingBufferEntry *stagingBufferGetTail(stagingBuffer *staging_buffer) {
    return staging_buffer->tail;
}

size_t stagingBufferGetTotalItemSize(stagingBuffer *staging_buffer) {
    return staging_buffer->total_item_size;
}

void stagingBufferRelease(stagingBuffer *staging_buffer) {
    stagingBufferEntry *entry = stagingBufferGetHead(staging_buffer);
    while (entry) {
        stagingBufferEntry *next = entry->next;
        stagingBufferDeleteEntry(staging_buffer, entry, 1);
        entry = next;
    }
    fcFree(staging_buffer);
}
