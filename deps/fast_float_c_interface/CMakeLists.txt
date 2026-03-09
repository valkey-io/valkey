project(fast_float_c_interface)

set(SRCS "${CMAKE_CURRENT_LIST_DIR}/fast_float_strtod.c")
add_library(fast_float STATIC ${SRCS})
# Add -fPIC
set_target_properties(fast_float PROPERTIES POSITION_INDEPENDENT_CODE ON)