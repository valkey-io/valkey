/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "listpack.h"
#include "quicklist.h"
#include "server.h"

int listTypeReplaceAtIndex(robj *o, int index, robj *value);
}

class ListObjectTest : public ::testing::Test {
  protected:
    int old_fill;
    int old_compress;

    void SetUp() override {
        old_fill = server.list_max_listpack_size;
        old_compress = server.list_compress_depth;
        server.list_compress_depth = 0;
    }

    void TearDown() override {
        server.list_max_listpack_size = old_fill;
        server.list_compress_depth = old_compress;
    }

    static void PushTail(robj *list, const std::string &value) {
        robj *element = createStringObject(value.data(), value.size());
        listTypePush(list, element, LIST_TAIL);
        decrRefCount(element);
    }

    static int Replace(robj *list, int index, const std::string &value) {
        robj *element = createStringObject(value.data(), value.size());
        int replaced = listTypeReplaceAtIndex(list, index, element);
        decrRefCount(element);
        return replaced;
    }

    static std::vector<std::string> Values(robj *list) {
        std::vector<std::string> values;
        listTypeIterator *iter = listTypeInitIterator(list, 0, LIST_TAIL);
        listTypeEntry entry;
        while (listTypeNext(iter, &entry)) {
            robj *value = listTypeGet(&entry);
            robj *decoded = getDecodedObject(value);
            sds str = (sds)objectGetVal(decoded);
            values.emplace_back(str, sdslen(str));
            decrRefCount(decoded);
            decrRefCount(value);
        }
        listTypeReleaseIterator(iter);
        return values;
    }
};

TEST_F(ListObjectTest, SmallReplacementAtCountLimitStaysListpack) {
    server.list_max_listpack_size = 3;
    robj *list = createListListpackObject();
    PushTail(list, "a");
    PushTail(list, "b");
    PushTail(list, "c");

    ASSERT_EQ(objectGetEncoding(list), OBJ_ENCODING_LISTPACK);
    ASSERT_TRUE(Replace(list, 1, "x"));
    EXPECT_EQ(objectGetEncoding(list), OBJ_ENCODING_LISTPACK);
    EXPECT_EQ(Values(list), (std::vector<std::string>{"a", "x", "c"}));

    decrRefCount(list);
}

TEST_F(ListObjectTest, LargeReplacementUsesPlainNode) {
    server.list_max_listpack_size = 3;
    robj *list = createListListpackObject();
    PushTail(list, "a");
    std::string large(9000, 'x');

    ASSERT_TRUE(Replace(list, 0, large));
    ASSERT_EQ(objectGetEncoding(list), OBJ_ENCODING_QUICKLIST);
    quicklist *ql = (quicklist *)objectGetVal(list);
    ASSERT_EQ(ql->count, 1u);
    ASSERT_EQ(ql->len, 1u);
    ASSERT_NE(ql->head, nullptr);
    EXPECT_EQ(ql->head->container, static_cast<unsigned int>(QUICKLIST_NODE_CONTAINER_PLAIN));
    EXPECT_EQ(ql->head->sz, large.size());
    EXPECT_EQ(memcmp(ql->head->entry, large.data(), large.size()), 0);

    decrRefCount(list);
}

TEST_F(ListObjectTest, LargeMiddleReplacementPreservesLayoutAndOrder) {
    server.list_max_listpack_size = 3;
    robj *list = createListListpackObject();
    PushTail(list, "left");
    PushTail(list, "middle");
    PushTail(list, "right");
    std::string large(9000, 'x');

    ASSERT_TRUE(Replace(list, 1, large));
    ASSERT_EQ(objectGetEncoding(list), OBJ_ENCODING_QUICKLIST);
    quicklist *ql = (quicklist *)objectGetVal(list);
    ASSERT_EQ(ql->count, 3u);
    ASSERT_EQ(ql->len, 3u);
    ASSERT_NE(ql->head, nullptr);
    ASSERT_NE(ql->head->next, nullptr);
    ASSERT_NE(ql->tail, nullptr);
    EXPECT_EQ(ql->head->container, static_cast<unsigned int>(QUICKLIST_NODE_CONTAINER_PACKED));
    EXPECT_EQ(ql->head->next->container, static_cast<unsigned int>(QUICKLIST_NODE_CONTAINER_PLAIN));
    EXPECT_EQ(ql->tail->container, static_cast<unsigned int>(QUICKLIST_NODE_CONTAINER_PACKED));
    EXPECT_EQ(ql->head->count, 1u);
    EXPECT_EQ(ql->head->next->count, 1u);
    EXPECT_EQ(ql->tail->count, 1u);
    EXPECT_EQ(Values(list), (std::vector<std::string>{"left", large, "right"}));

    decrRefCount(list);
}

TEST_F(ListObjectTest, InvalidIndexesPreserveListpackStorage) {
    server.list_max_listpack_size = 3;
    robj *list = createListListpackObject();
    PushTail(list, "a");
    PushTail(list, "b");
    PushTail(list, "c");
    unsigned char *before = (unsigned char *)objectGetVal(list);
    size_t before_size = lpBytes(before);
    std::vector<unsigned char> snapshot(before, before + before_size);

    EXPECT_FALSE(Replace(list, 3, "x"));
    EXPECT_EQ(objectGetVal(list), before);
    EXPECT_EQ(objectGetEncoding(list), OBJ_ENCODING_LISTPACK);
    EXPECT_EQ(lpBytes((unsigned char *)objectGetVal(list)), before_size);
    EXPECT_EQ(memcmp(objectGetVal(list), snapshot.data(), before_size), 0);

    EXPECT_FALSE(Replace(list, -4, "y"));
    EXPECT_EQ(objectGetVal(list), before);
    EXPECT_EQ(objectGetEncoding(list), OBJ_ENCODING_LISTPACK);
    EXPECT_EQ(lpBytes((unsigned char *)objectGetVal(list)), before_size);
    EXPECT_EQ(memcmp(objectGetVal(list), snapshot.data(), before_size), 0);

    decrRefCount(list);
}

TEST_F(ListObjectTest, SmallReplacementAllowsQuicklistToListpackShrink) {
    server.list_max_listpack_size = 3;
    robj *list = createQuicklistObject(server.list_max_listpack_size, server.list_compress_depth);
    PushTail(list, std::string(9000, 'x'));
    ASSERT_EQ(objectGetEncoding(list), OBJ_ENCODING_QUICKLIST);

    ASSERT_TRUE(Replace(list, 0, "small"));
    ASSERT_EQ(objectGetEncoding(list), OBJ_ENCODING_QUICKLIST);
    listTypeTryConversion(list, LIST_CONV_SHRINKING, nullptr, nullptr);
    EXPECT_EQ(objectGetEncoding(list), OBJ_ENCODING_LISTPACK);
    EXPECT_EQ(Values(list), (std::vector<std::string>{"small"}));

    decrRefCount(list);
}
