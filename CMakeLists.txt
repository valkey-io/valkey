cmake_minimum_required(VERSION 3.10)

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
option(BUILD_UNIT_TESTS "Build valkey-unit-tests" OFF)
option(BUILD_TEST_MODULES "Build all test modules" OFF)
option(BUILD_EXAMPLE_MODULES "Build example modules" OFF)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")
project("valkey")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(ValkeySetup)
add_subdirectory(src)
add_subdirectory(tests)

# Include the packaging module
include(Packaging)

# Clear cached variables from the cache
unset(BUILD_TESTS CACHE)
unset(CLANGPP CACHE)
unset(CLANG CACHE)
unset(BUILD_RDMA_MODULE CACHE)
unset(BUILD_TLS_MODULE CACHE)
unset(BUILD_UNIT_TESTS CACHE)
unset(BUILD_TEST_MODULES CACHE)
unset(BUILD_EXAMPLE_MODULES CACHE)
unset(USE_TLS CACHE)
unset(DEBUG_FORCE_DEFRAG CACHE)
