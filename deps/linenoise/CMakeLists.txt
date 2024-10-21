project(linenoise)

set(SRCS "${CMAKE_CURRENT_LIST_DIR}/linenoise.c" "${CMAKE_CURRENT_LIST_DIR}/linenoise.h")
add_library(linenoise STATIC ${SRCS})
