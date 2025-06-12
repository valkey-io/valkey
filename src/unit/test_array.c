#include "../array.h"

#include "test_help.h"

typedef struct {
    uint8_t uint8;
    uint64_t uint64;
} test_struct;

int test_array(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* The test cases cover the following scenarios:
     * 1) Whether the array pre-allocates memory during initialization;
     * 2) Element sizes smaller than, equal to, and larger than sizeof(void*);
     * 3) Usage of each API.
     */
    array uint8_array;
    array uint64_array;
    array struct_array;
    arrayInit(&uint8_array, 0, sizeof(uint8_t));
    arrayInit(&uint64_array, 10, sizeof(uint64_t));
    arrayInit(&struct_array, 128, sizeof(test_struct));

    for (uint64_t i = 0; i < 128; i++) {
        uint8_t *uint8_item = arrayPush(&uint8_array);
        *uint8_item = i;

        uint64_t *uint64_item = arrayPush(&uint64_array);
        *uint64_item = i * 1000;

        test_struct *struct_item = arrayPush(&struct_array);
        struct_item->uint8 = i;
        struct_item->uint64 = i * 1000;
    }

    TEST_ASSERT_MESSAGE("uint8_array length", arrayLen(&uint8_array) == 128);
    TEST_ASSERT_MESSAGE("uint64_array length", arrayLen(&uint64_array) == 128);
    TEST_ASSERT_MESSAGE("struct_array length", arrayLen(&struct_array) == 128);
    for (uint32_t i = 0; i < arrayLen(&uint8_array); i++) {
        uint8_t *uint8_item = arrayGet(&uint8_array, i);
        TEST_ASSERT_MESSAGE("uint8_item value", *uint8_item == i);

        uint64_t *uint64_item = arrayGet(&uint64_array, i);
        TEST_ASSERT_MESSAGE("uint64_item value", *uint64_item == i * 1000);

        test_struct *struct_item = arrayGet(&struct_array, i);
        TEST_ASSERT_MESSAGE("struct_item uint8 value", struct_item->uint8 == i);
        TEST_ASSERT_MESSAGE("struct_item uint64 value", struct_item->uint64 == i * 1000);
    }

    arrayCleanup(&uint8_array);
    arrayCleanup(&uint64_array);
    arrayCleanup(&struct_array);
    return 0;
}
