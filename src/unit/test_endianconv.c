#include "../endianconv.h"
#include "test_help.h"

int test_endianconv(int argc, char *argv[], int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    char buf[32];

    snprintf(buf,sizeof(buf),"ciaoroma");
    memrev16(buf);
    printf("%s\n", buf);

    snprintf(buf,sizeof(buf),"ciaoroma");
    memrev32(buf);
    printf("%s\n", buf);

    snprintf(buf,sizeof(buf),"ciaoroma");
    memrev64(buf);
    printf("%s\n", buf);

    return 0;
}