#include "../zmalloc.h"
#include "test_help.h"

int test_zmallocAllocReallocCallocAndFree(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    size_t used_memory_before = zmalloc_used_memory();
    void *ptr, *ptr2;

    ptr = zmalloc(123);
    TEST_PRINT_INFO("Allocated 123 bytes; used: %lld\n",
                    (long long)zmalloc_used_memory() - used_memory_before);

    ptr = zrealloc(ptr, 456);
    TEST_PRINT_INFO("Reallocated to 456 bytes; used: %lld\n",
                    (long long)zmalloc_used_memory() - used_memory_before);

    ptr2 = zcalloc(123);
    TEST_PRINT_INFO("Callocated 123 bytes; used: %lld\n",
                    (long long)zmalloc_used_memory() - used_memory_before);

    zfree(ptr);
    zfree(ptr2);
    TEST_PRINT_INFO("Freed pointers; used: %lld\n",
                    (long long)zmalloc_used_memory() - used_memory_before);

    TEST_ASSERT(zmalloc_used_memory() == used_memory_before);

    return 0;
}

int test_zmallocAllocZeroByteAndFree(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    size_t used_memory_before = zmalloc_used_memory();
    void *ptr;

    ptr = zmalloc(0);
    TEST_PRINT_INFO("Allocated 0 bytes; used: %zu\n", zmalloc_used_memory());
    zfree(ptr);

    TEST_ASSERT(zmalloc_used_memory() == used_memory_before);

    return 0;
}
