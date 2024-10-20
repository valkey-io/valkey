project(fpconv)

set(SRCS "${CMAKE_CURRENT_LIST_DIR}/fpconv_dtoa.c" "${CMAKE_CURRENT_LIST_DIR}/fpconv_dtoa.h")
add_library(fpconv STATIC ${SRCS})
