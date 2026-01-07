#include "gmock/gmock.h"
#include "gtest/gtest.h"

int main(int argc, char **argv) {
    // The following line must be executed to initialize GoogleTest before running the tests.
    ::testing::InitGoogleMock(&argc, argv);
    return RUN_ALL_TESTS();
}
