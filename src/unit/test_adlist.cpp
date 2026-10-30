/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"
#include <cstring>

extern "C" {
#include "adlist.h"
#include "zmalloc.h"
}

static void *newValue(int v) {
    int *p = (int *)zmalloc(sizeof(*p));
    *p = v;
    return p;
}

static int valueOf(void *p) {
    return *(int *)p;
}

/* Verify that 'l' holds exactly 'expected' with consistent links, head, tail
 * and len. ASSERT is only used where continuing would dereference NULL. */
static void verifyList(list *l, const int *expected, unsigned long count) {
    EXPECT_EQ(listLength(l), count);
    if (count == 0) {
        EXPECT_EQ(listFirst(l), (listNode *)NULL);
        EXPECT_EQ(listLast(l), (listNode *)NULL);
        return;
    }
    ASSERT_NE(listFirst(l), (listNode *)NULL);
    ASSERT_NE(listLast(l), (listNode *)NULL);
    EXPECT_EQ(listPrevNode(listFirst(l)), (listNode *)NULL);
    EXPECT_EQ(listNextNode(listLast(l)), (listNode *)NULL);

    listNode *node = listFirst(l);
    listNode *prev = NULL;
    for (unsigned long i = 0; i < count; i++) {
        ASSERT_NE(node, (listNode *)NULL);
        EXPECT_EQ(listPrevNode(node), prev);
        EXPECT_EQ(valueOf(listNodeValue(node)), expected[i]);
        prev = node;
        node = listNextNode(node);
    }
    EXPECT_EQ(node, (listNode *)NULL);
    EXPECT_EQ(listLast(l), prev);
}

/* Free method that calls free, and records each pointer it is called with
 * so tests can tell exactly what adlist freed, and that it used the
 * configured method rather than calling zfree() itself. */
#define MAX_RECORDED 64
static void *freed[MAX_RECORDED];
static int free_calls = 0;

static void recordingZfree(void *ptr) {
    if (free_calls < MAX_RECORDED) freed[free_calls] = ptr;
    free_calls++;
    zfree(ptr);
}

static void verifyFreed(void *const *expected, int count) {
    ASSERT_EQ(free_calls, count);
    for (int i = 0; i < count; i++) {
        int seen = 0;
        for (int j = 0; j < count; j++) {
            if (freed[j] == expected[i]) seen++;
        }
        EXPECT_EQ(seen, 1) << "expected pointer #" << i;
    }
}

static void *plus100Dup(void *ptr) {
    return newValue(valueOf(ptr) + 100);
}

static void *failingDup(void *ptr) {
    if (valueOf(ptr) == 2) return NULL;
    return plus100Dup(ptr);
}

static int valueMatch(void *ptr, void *key) {
    return valueOf(ptr) == valueOf(key);
}

class AdlistTest : public ::testing::Test {
  protected:
    list *l;
    void *values[MAX_RECORDED]; /* values[i] holds i + 1, as added by fill() */

    void SetUp() override {
        l = listCreate();
        listSetFreeMethod(l, recordingZfree);
        free_calls = 0;
    }

    void TearDown() override {
        listRelease(l);
    }

    void fill(int n) {
        for (int i = 0; i < n; i++) {
            values[i] = newValue(i + 1);
            listAddNodeTail(l, values[i]);
        }
    }
};

TEST_F(AdlistTest, ListCreate) {
    list *fresh = listCreate();
    verifyList(fresh, NULL, 0);
    EXPECT_EQ(listGetDupMethod(fresh), (void *(*)(void *))NULL);
    EXPECT_EQ(listGetFreeMethod(fresh), (void (*)(void *))NULL);
    EXPECT_EQ(listGetMatchMethod(fresh), (int (*)(void *, void *))NULL);
    listRelease(fresh);
}

TEST_F(AdlistTest, ListRelease) {
    size_t used_before = zmalloc_used_memory();
    list *other = listCreate();
    size_t used_empty_list = zmalloc_used_memory();
    EXPECT_GT(used_empty_list, used_before);
    listSetFreeMethod(other, recordingZfree);
    void *other_values[] = {newValue(1), newValue(2)};
    listAddNodeTail(other, other_values[0]);
    listAddNodeTail(other, other_values[1]);
    listRelease(other);
    verifyFreed(other_values, 2);
    EXPECT_EQ(zmalloc_used_memory(), used_before);
}

TEST_F(AdlistTest, ListReleaseWithoutFreeMethod) {
    /* Without a free method the value is left to the caller: only the node
     * and the list struct are freed. */
    void *value = newValue(1);
    size_t used_before = zmalloc_used_memory();
    list *other = listCreate();
    listAddNodeTail(other, value);
    listRelease(other);
    EXPECT_EQ(zmalloc_used_memory(), used_before);
    zfree(value);
}

TEST_F(AdlistTest, ListReleaseVoid) {
    size_t used_before = zmalloc_used_memory();
    list *other = listCreate();
    listSetFreeMethod(other, recordingZfree);
    void *value = newValue(1);
    listAddNodeTail(other, value);
    listReleaseVoid(other);
    verifyFreed(&value, 1);
    EXPECT_EQ(zmalloc_used_memory(), used_before);
}

TEST_F(AdlistTest, ListEmpty) {
    size_t used_before = zmalloc_used_memory();
    fill(2);
    listEmpty(l);
    verifyList(l, NULL, 0);
    verifyFreed(values, 2);
    EXPECT_EQ(zmalloc_used_memory(), used_before);

    listAddNodeTail(l, newValue(1));
    int expected[] = {1};
    verifyList(l, expected, 1);
}

TEST_F(AdlistTest, ListEmptyNull) {
    listEmpty(NULL);
}

TEST_F(AdlistTest, ListAddNodeHead) {
    EXPECT_EQ(listAddNodeHead(l, newValue(2)), l);
    int expected1[] = {2};
    verifyList(l, expected1, 1);
    EXPECT_EQ(listAddNodeHead(l, newValue(1)), l);
    int expected2[] = {1, 2};
    verifyList(l, expected2, 2);
}

TEST_F(AdlistTest, ListAddNodeTail) {
    EXPECT_EQ(listAddNodeTail(l, newValue(1)), l);
    int expected1[] = {1};
    verifyList(l, expected1, 1);
    EXPECT_EQ(listAddNodeTail(l, newValue(2)), l);
    int expected2[] = {1, 2};
    verifyList(l, expected2, 2);
}

TEST_F(AdlistTest, ListInsertNodeAfterTail) {
    fill(2);
    EXPECT_EQ(listInsertNode(l, listLast(l), newValue(3), 1), l);
    int expected[] = {1, 2, 3};
    verifyList(l, expected, 3);
}

TEST_F(AdlistTest, ListInsertNodeAfterMiddle) {
    fill(2);
    EXPECT_EQ(listInsertNode(l, listFirst(l), newValue(10), 1), l);
    int expected[] = {1, 10, 2};
    verifyList(l, expected, 3);
}

TEST_F(AdlistTest, ListInsertNodeBeforeHead) {
    fill(2);
    EXPECT_EQ(listInsertNode(l, listFirst(l), newValue(0), 0), l);
    int expected[] = {0, 1, 2};
    verifyList(l, expected, 3);
}

TEST_F(AdlistTest, ListInsertNodeBeforeMiddle) {
    fill(2);
    EXPECT_EQ(listInsertNode(l, listLast(l), newValue(10), 0), l);
    int expected[] = {1, 10, 2};
    verifyList(l, expected, 3);
}

TEST_F(AdlistTest, ListDelNode) {
    size_t used_empty = zmalloc_used_memory();
    fill(4);
    listDelNode(l, listIndex(l, 1));
    listDelNode(l, listFirst(l));
    listDelNode(l, listLast(l));
    int expected[] = {3};
    verifyList(l, expected, 1);
    void *deleted[] = {values[1], values[0], values[3]};
    verifyFreed(deleted, 3);

    listDelNode(l, listFirst(l));
    verifyList(l, NULL, 0);
    verifyFreed(values, 4);
    EXPECT_EQ(zmalloc_used_memory(), used_empty);
}

TEST_F(AdlistTest, ListDelNodeWithoutFreeMethod) {
    listSetFreeMethod(l, NULL);
    void *value = newValue(1);
    size_t used_with_value = zmalloc_used_memory();
    listAddNodeTail(l, value);
    listDelNode(l, listFirst(l));
    EXPECT_EQ(zmalloc_used_memory(), used_with_value);
    zfree(value);
}

TEST_F(AdlistTest, ListDelNodeWhileIterating) {
    fill(3);
    listIter iter;
    listNode *node;
    listRewind(l, &iter);
    while ((node = listNext(&iter)) != NULL) {
        if (valueOf(listNodeValue(node)) == 2) listDelNode(l, node);
    }
    int expected[] = {1, 3};
    verifyList(l, expected, 2);
}

TEST_F(AdlistTest, ListGetIterator) {
    fill(2);
    size_t used_before = zmalloc_used_memory();
    listIter *iter = listGetIterator(l, AL_START_HEAD);
    ASSERT_NE(iter, (listIter *)NULL);
    listNode *node;
    int i = 1;
    while ((node = listNext(iter)) != NULL) EXPECT_EQ(valueOf(listNodeValue(node)), i++);
    EXPECT_EQ(i, 3);
    listReleaseIterator(iter);

    iter = listGetIterator(l, AL_START_TAIL);
    ASSERT_NE(iter, (listIter *)NULL);
    i = 2;
    while ((node = listNext(iter)) != NULL) EXPECT_EQ(valueOf(listNodeValue(node)), i--);
    EXPECT_EQ(i, 0);
    listReleaseIterator(iter);
    EXPECT_EQ(zmalloc_used_memory(), used_before);
}

TEST_F(AdlistTest, ListNext) {
    fill(2);
    listIter iter;
    listRewind(l, &iter);
    EXPECT_EQ(listNext(&iter), listFirst(l));
    EXPECT_EQ(listNext(&iter), listLast(l));
    EXPECT_EQ(listNext(&iter), (listNode *)NULL);
}

TEST_F(AdlistTest, ListDupWithoutDupMethod) {
    fill(2);
    listSetMatchMethod(l, valueMatch);
    list *copy = listDup(l);
    ASSERT_NE(copy, (list *)NULL);
    int expected[] = {1, 2};
    verifyList(copy, expected, 2);
    EXPECT_EQ(listNodeValue(listFirst(copy)), listNodeValue(listFirst(l)));
    EXPECT_EQ(listGetFreeMethod(copy), recordingZfree);
    EXPECT_EQ(listGetMatchMethod(copy), valueMatch);
    listSetFreeMethod(copy, NULL);
    listRelease(copy);
}

TEST_F(AdlistTest, ListDupWithDupMethod) {
    fill(2);
    listSetDupMethod(l, plus100Dup);
    list *copy = listDup(l);
    ASSERT_NE(copy, (list *)NULL);
    EXPECT_EQ(listGetDupMethod(copy), plus100Dup);
    int expected_copy[] = {101, 102};
    verifyList(copy, expected_copy, 2);
    EXPECT_NE(listNodeValue(listFirst(copy)), listNodeValue(listFirst(l)));
    listRelease(copy);
    int expected_orig[] = {1, 2};
    verifyList(l, expected_orig, 2);
}

TEST_F(AdlistTest, ListDupFailure) {
    fill(2);
    listSetDupMethod(l, failingDup);
    size_t used_before = zmalloc_used_memory();
    EXPECT_EQ(listDup(l), (list *)NULL);
    EXPECT_EQ(zmalloc_used_memory(), used_before);
    int expected[] = {1, 2};
    verifyList(l, expected, 2);
}

TEST_F(AdlistTest, ListSearchKey) {
    listAddNodeTail(l, newValue(2));
    listAddNodeTail(l, newValue(2));
    listNode *second = listLast(l);

    void *same_value = newValue(2);
    void *missing_value = newValue(9);

    EXPECT_EQ(listSearchKey(l, listNodeValue(second)), second);
    EXPECT_EQ(listSearchKey(l, same_value), (listNode *)NULL);

    listSetMatchMethod(l, valueMatch);
    EXPECT_EQ(listSearchKey(l, same_value), listFirst(l));
    EXPECT_EQ(listSearchKey(l, missing_value), (listNode *)NULL);

    zfree(same_value);
    zfree(missing_value);
}

TEST_F(AdlistTest, ListIndex) {
    fill(2);
    long indexes[] = {0, 1, -1, -2};
    int expected[] = {1, 2, 2, 1};
    for (int i = 0; i < 4; i++) {
        listNode *node = listIndex(l, indexes[i]);
        ASSERT_NE(node, (listNode *)NULL) << "index " << indexes[i];
        EXPECT_EQ(valueOf(listNodeValue(node)), expected[i]) << "index " << indexes[i];
    }

    EXPECT_EQ(listIndex(l, 2), (listNode *)NULL);
    EXPECT_EQ(listIndex(l, -3), (listNode *)NULL);
}

TEST_F(AdlistTest, ListRewind) {
    fill(2);
    listIter iter;
    listNode *node;
    int i = 1;
    listRewind(l, &iter);
    while ((node = listNext(&iter)) != NULL) EXPECT_EQ(valueOf(listNodeValue(node)), i++);
    EXPECT_EQ(i, 3);
}

TEST_F(AdlistTest, ListRewindTail) {
    fill(2);
    listIter iter;
    listNode *node;
    int i = 2;
    listRewindTail(l, &iter);
    while ((node = listNext(&iter)) != NULL) EXPECT_EQ(valueOf(listNodeValue(node)), i--);
    EXPECT_EQ(i, 0);
}

TEST_F(AdlistTest, ListRotateTailToHead) {
    listRotateTailToHead(l);
    verifyList(l, NULL, 0);
    fill(1);
    listRotateTailToHead(l);
    int expected1[] = {1};
    verifyList(l, expected1, 1);

    listEmpty(l);
    fill(3);
    listRotateTailToHead(l);
    int expected2[] = {3, 1, 2};
    verifyList(l, expected2, 3);
}

TEST_F(AdlistTest, ListRotateHeadToTail) {
    listRotateHeadToTail(l);
    verifyList(l, NULL, 0);
    fill(1);
    listRotateHeadToTail(l);
    int expected1[] = {1};
    verifyList(l, expected1, 1);

    listEmpty(l);
    fill(3);
    listRotateHeadToTail(l);
    int expected2[] = {2, 3, 1};
    verifyList(l, expected2, 3);
}

TEST_F(AdlistTest, ListJoin) {
    list *o = listCreate();
    listSetFreeMethod(o, zfree);

    listAddNodeTail(o, newValue(1));
    listAddNodeTail(o, newValue(2));
    listJoin(l, o);
    int expected1[] = {1, 2};
    verifyList(l, expected1, 2);
    verifyList(o, NULL, 0);

    listJoin(l, o);
    verifyList(l, expected1, 2);
    verifyList(o, NULL, 0);

    listAddNodeTail(o, newValue(3));
    listAddNodeTail(o, newValue(4));
    listJoin(l, o);
    int expected2[] = {1, 2, 3, 4};
    verifyList(l, expected2, 4);
    verifyList(o, NULL, 0);
    listRelease(o);
}

TEST_F(AdlistTest, ListInitNode) {
    listNode node;
    int value = 42;
    memset(&node, 0xff, sizeof(node));
    listInitNode(&node, &value);
    EXPECT_EQ(node.prev, (listNode *)NULL);
    EXPECT_EQ(node.next, (listNode *)NULL);
    EXPECT_EQ(listNodeValue(&node), &value);
}

TEST_F(AdlistTest, ListLinkNodeHead) {
    listNode nodes[2];
    int v[2] = {1, 2};
    for (int i = 0; i < 2; i++) listInitNode(&nodes[i], &v[i]);
    listLinkNodeHead(l, &nodes[1]);
    int expected1[] = {2};
    verifyList(l, expected1, 1);
    listLinkNodeHead(l, &nodes[0]);
    int expected2[] = {1, 2};
    verifyList(l, expected2, 2);
    for (int i = 0; i < 2; i++) listUnlinkNode(l, &nodes[i]);
}

TEST_F(AdlistTest, ListLinkNodeTail) {
    listNode nodes[2];
    int v[2] = {1, 2};
    for (int i = 0; i < 2; i++) listInitNode(&nodes[i], &v[i]);
    listLinkNodeTail(l, &nodes[0]);
    int expected1[] = {1};
    verifyList(l, expected1, 1);
    listLinkNodeTail(l, &nodes[1]);
    int expected2[] = {1, 2};
    verifyList(l, expected2, 2);
    for (int i = 0; i < 2; i++) listUnlinkNode(l, &nodes[i]);
}

TEST_F(AdlistTest, ListUnlinkNode) {
    fill(4);
    listNode *nodes[4] = {listIndex(l, 0), listIndex(l, 1), listIndex(l, 2), listIndex(l, 3)};
    size_t used_before = zmalloc_used_memory();

    listUnlinkNode(l, nodes[1]);
    int expected1[] = {1, 3, 4};
    verifyList(l, expected1, 3);

    listUnlinkNode(l, nodes[0]);
    int expected2[] = {3, 4};
    verifyList(l, expected2, 2);

    listUnlinkNode(l, nodes[3]);
    int expected3[] = {3};
    verifyList(l, expected3, 1);

    listUnlinkNode(l, nodes[2]);
    verifyList(l, NULL, 0);

    EXPECT_EQ(free_calls, 0);
    EXPECT_EQ(zmalloc_used_memory(), used_before);
    for (int i = 0; i < 4; i++) {
        zfree(listNodeValue(nodes[i]));
        zfree(nodes[i]);
    }
}
