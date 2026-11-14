project(croaring)

set(SRCS "${CMAKE_CURRENT_LIST_DIR}/roaring.c" "${CMAKE_CURRENT_LIST_DIR}/roaring.h")

add_library(croaring STATIC ${SRCS})
# Symbols stay hidden so that they cannot clash with a copy of CRoaring
# bundled in a module.
set_target_properties(croaring PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden C_STANDARD 11
                                          C_EXTENSIONS ON)
target_compile_options(croaring PRIVATE -O3)
