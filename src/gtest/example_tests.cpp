#include "generated_wrappers.hpp"

extern "C" {
    #include "dict.h"
}

class ExampleTest : public ::testing::Test {
    protected:
        MockValkey mock;
        RealValkey real;

    void SetUp() override {
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
    }

    void TearDown() override {}
};

// Simple assertions test
TEST_F(ExampleTest, TestAssertions) {
    int a = 5, b = 3;
    const char *str = "hello";
    EXPECT_EQ(8, a + b);
    EXPECT_LE(b, a);
    EXPECT_GT(a, b);
    EXPECT_STREQ(str, "hello");
    ASSERT_EQ(2, a - b);
}

// Test matcher works in custom_matchers.hpp
TEST_F(ExampleTest, TestMatchers) {
    robj *robj_str = createStringObject("test", 4);
    EXPECT_THAT(robj_str, robjEqualsStr("test"));
    decrRefCount(robj_str);
}

// Verify mocking works via zfree
TEST_F(ExampleTest, TestMocking) {
    // zfree should be called in dictRelease
    EXPECT_CALL(mock, zfree(_)).Times(AtLeast(1));
    dict *d = dictCreate(&keylistDictType);
    dictRelease(d);
}
