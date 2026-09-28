cmake_minimum_required(VERSION 3.14)
project(FlashCache LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Collect all source files
file(GLOB FC_SOURCES "src/*.c")

# Create static library
add_library(flashcache STATIC ${FC_SOURCES})

# Include directories — the source files use #include "include/xxx.h"
# so we need the src/ directory on the include path.
target_include_directories(flashcache PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

# Use in-tree jemalloc (same allocator as Valkey core).
# JEMALLOC_MANGLE remaps malloc/free/malloc_usable_size to je_* variants.
target_include_directories(flashcache PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../jemalloc/include
)
target_compile_definitions(flashcache PRIVATE JEMALLOC_MANGLE)

# Link against libaio (required for async I/O)
target_link_libraries(flashcache PUBLIC aio)

# Compiler flags (no -Werror yet)
target_compile_options(flashcache PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-missing-field-initializers
    -Wno-sign-compare
    -Wno-cast-qual
    -Wno-double-promotion
    -Wno-format-nonliteral
    -Wno-pedantic
    -D_GNU_SOURCE
)

# Architecture-specific flags for CRC32
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    target_compile_options(flashcache PRIVATE -msse4.2)
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    target_compile_options(flashcache PRIVATE -march=armv8-a+crc)
endif()
