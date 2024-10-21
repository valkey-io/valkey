cmake_minimum_required(VERSION 3.20)

# Must be done first
if (APPLE)
    # Force clang compiler on macOS
    find_program(CLANGPP "clang++")
    find_program(CLANG "clang")
    if (CLANG AND CLANGPP)
        message(STATUS "Found ${CLANGPP}, ${CLANG}")
        set(CMAKE_CXX_COMPILER ${CLANGPP})
        set(CMAKE_C_COMPILER ${CLANG})
    endif ()
endif ()

# Options
option(BUILD_TESTS "Build valkey-unit-tests" OFF)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")
project("valkey")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(ValkeySetup)
add_subdirectory(src)

# Include the packaging module
include(Packaging)

# Clear cached variables defined on this file
unset(BUILD_TESTS CACHE)
unset(CLANGPP CACHE)
unset(CLANG CACHE)
