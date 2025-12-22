/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "list.h"
#include "../../valkeymodule.h"

typedef struct ListNode {
    void *val;
    struct ListNode *next;
} ListNode;

typedef struct List {
    ListNode *head;
    int length;
} List;

typedef struct ListIter {
    ListNode *current;
} ListIter;

List *list_create(void) {
    List *list = ValkeyModule_Alloc(sizeof(List));
    list->head = NULL;
    list->length = 0;
    return list;
}

void list_destroy(List *list) {
    ListNode *current = list->head;
    while (current) {
        ListNode *next = current->next;
        ValkeyModule_Free(current);
        current = next;
    }
    ValkeyModule_Free(list);
}

int list_length(List *list) {
    return list->length;
}

void list_add(List *list, void *val) {
    ListNode *new_node = ValkeyModule_Calloc(1, sizeof(ListNode));
    new_node->val = val;
    new_node->next = NULL;
    if (!list->head) {
        list->head = new_node;
    } else {
        ListNode *current = list->head;
        while (current->next) {
            current = current->next;
        }
        current->next = new_node;
    }
    list->length++;
}

ListIter *list_get_iter(List *list) {
    ListIter *iter = ValkeyModule_Calloc(1, sizeof(ListIter));
    iter->current = list->head;
    return iter;
}

void *list_iter_next(ListIter *iter) {
    if (!iter->current) {
        return NULL;
    }
    void *val = iter->current->val;
    iter->current = iter->current->next;
    return val;
}

void list_release_iter(ListIter *iter) {
    ValkeyModule_Free(iter);
}
