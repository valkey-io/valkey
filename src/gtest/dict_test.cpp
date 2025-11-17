#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "generated_wrappers.hpp"

using namespace ::testing;

extern "C" {
    #include "dict.h"
    extern dictType keylistDictType;
}

class DictTest : public ::testing::Test {
    protected:
        MockValkey mock;
        RealValkey real;

    void SetUp() override {
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
    }

    void TearDown() override {

    }
};

TEST_F(DictTest, testDictApis) {
    struct dict *d = dictCreate(&keylistDictType);
    list* vlist1 = listCreate();

    robj * key1 = createObject(OBJ_STRING, sdsnew("key1"));

    // Verify robjEqualsStr works in custom_matchers.hpp
    EXPECT_THAT(key1, robjEqualsStr("key1"));
    EXPECT_EQ(dictAdd(d, key1, vlist1), C_OK);
    EXPECT_EQ(dictSize(d), 1u);
    EXPECT_EQ(dictGetVal(dictFind(d, key1)), vlist1);

    // check that we are freeing the allocated key in dictAdd.
    // 1 for key, 1 for list release, 1 for dict entry.
    EXPECT_CALL(mock, zfree(_)).Times(3);
    EXPECT_EQ(dictDelete(d, key1), C_OK);
    EXPECT_EQ(dictFind(d, key1), nullptr);

    // 2 for dict tables + 1 for dict.
    EXPECT_CALL(mock, zfree(_)).Times(3);
    dictRelease(d);
}
