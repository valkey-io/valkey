#include <string.h>

#include "../endianconv.h"
#include "test_help.h"

int test_endianconv(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char buf[32];

    snprintf(buf, sizeof(buf), "ciaoroma");
    memrev16(buf);
    TEST_ASSERT(!strcmp(buf, "icaoroma"));

    snprintf(buf, sizeof(buf), "ciaoroma");
    memrev32(buf);
    TEST_ASSERT(!strcmp(buf, "oaicroma"));

    snprintf(buf, sizeof(buf), "ciaoroma");
    memrev64(buf);
    TEST_ASSERT(!strcmp(buf, "amoroaic"));

    return 0;
}
