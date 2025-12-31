#include "generated_wrappers.hpp"

extern "C" {
#include "server.h"
}

// Use a class name descriptive of your test unit
class ExampleTest : public ::testing::Test {
    // standard boilerplate supporting mocked functions
  protected:
    MockValkey mock;
    RealValkey real;

    // The SetUp() function is called before each test.
    void SetUp() override {
        memset(&server, 0, sizeof(valkeyServer));
        server.hz = CONFIG_DEFAULT_HZ;
    }

    // The TearDown() function is called after each test.
    void TearDown() override {
    }
};

// Include this (should end in "DeathTest") if testing that code asserts/dies.
using ExampleDeathTest = ExampleTest;

// Example of a DeathTest, which passes only if the code crashes.
TEST_F(ExampleDeathTest, TestSimpleDeath) {
    EXPECT_DEATH(
        {
            *(static_cast<volatile char*>(0)) = 'x'; // SEGV
        },
        "");
}

// Simple assertions test
TEST_F(ExampleTest, TestAssertions) {
    int a = 5, b = 3;
    const char* str = "hello";
    // Use EXPECT_ macros to test a condition.  If the value is not as expected, the test will fail.
    // Use ASSERT_ macros to test a condition AND immediately end the test.
    // Prefer to use EXPECT_ macros unless the test can't reasonably continue. This allows multiple
    // conditions to be tested and reported rather than ending at the first unexpected value.
    EXPECT_EQ(8, a + b);
    EXPECT_LE(b, a);
    EXPECT_GT(a, b);
    EXPECT_STREQ(str, "hello");
    ASSERT_EQ(2, a - b);
}

// Test matcher works in custom_matchers.hpp
TEST_F(ExampleTest, TestMatchers) {
    robj* robj_str = createStringObject("test", 4);
    ASSERT_NE(robj_str, nullptr); // "ASSERT" is correct here, because the test can't reasonably continue
    EXPECT_THAT(robj_str, robjEqualsStr("test"));
    decrRefCount(robj_str);
}

// Verify mocking works via processCommand
TEST_F(ExampleTest, TestMocking) {
    client prev_client = {};
    client c = {};
    server.current_client = &prev_client;
    c.flag.blocked = 1;

    // verifies that processCommand() is called once
    EXPECT_CALL(mock, processCommand(_)).WillOnce(Return(C_OK));
    // resetClient must not be invoked for a blocked client
    EXPECT_CALL(mock, resetClient(_)).Times(0);
    ASSERT_EQ(processCommandAndResetClient(&c), C_OK);
    ASSERT_EQ(server.current_client, &prev_client);
}
