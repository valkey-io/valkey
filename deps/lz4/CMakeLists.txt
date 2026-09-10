project(lz4)

set(SRCS
    "${CMAKE_CURRENT_LIST_DIR}/lz4.c"
    "${CMAKE_CURRENT_LIST_DIR}/lz4.h"
    "${CMAKE_CURRENT_LIST_DIR}/lz4hc.c"
    "${CMAKE_CURRENT_LIST_DIR}/lz4hc.h"
    "${CMAKE_CURRENT_LIST_DIR}/lz4frame.c"
    "${CMAKE_CURRENT_LIST_DIR}/lz4frame.h"
    "${CMAKE_CURRENT_LIST_DIR}/xxhash.c"
    "${CMAKE_CURRENT_LIST_DIR}/xxhash.h")

add_library(lz4 STATIC ${SRCS})
target_compile_definitions(lz4 PRIVATE XXH_NAMESPACE=VALKEY_LZ4_)
set_target_properties(lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON)
