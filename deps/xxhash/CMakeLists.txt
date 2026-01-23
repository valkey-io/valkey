set(XXHASH_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/xxhash.c"
    "${CMAKE_CURRENT_LIST_DIR}/xxhash.h"
)

add_library(xxhash STATIC ${XXHASH_SRCS})
