#include "../testhelp.h"
#include "../serverassert.h"
#include "../zmalloc.h"

#define UNUSED(x) ((void)(x))
#define TEST(name) printf("test — %s\n", name);

int zmalloc_test(int argc, char **argv, int flags) {
    void *ptr, *ptr2;

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    printf("Malloc prefix size: %d\n", (int) PREFIX_SIZE);

    TEST("Initial used memory is 0") {
        assert(zmalloc_used_memory() == 0);
    }

    TEST("Allocated 123 bytes") {
        ptr = zmalloc(123);
        printf("Allocated 123 bytes; used: %zu\n", zmalloc_used_memory());
    }

    TEST("Reallocated to 456 bytes") {
        ptr = zrealloc(ptr, 456);
        printf("Reallocated to 456 bytes; used: %zu\n", zmalloc_used_memory());
    }

    TEST("Callocated 123 bytes") {
        ptr2 = zcalloc(123);
        printf("Callocated 123 bytes; used: %zu\n", zmalloc_used_memory());
    }

    TEST("Freed pointers") {
        zfree(ptr);
        zfree(ptr2);
        printf("Freed pointers; used: %zu\n", zmalloc_used_memory());
    }

    TEST("Allocated 0 bytes") {
        ptr = zmalloc(0);
        printf("Allocated 0 bytes; used: %zu\n", zmalloc_used_memory());
        zfree(ptr);
    }

    TEST("At the end used memory is 0") {
        assert(zmalloc_used_memory() == 0);
    }

    return 0;
}
