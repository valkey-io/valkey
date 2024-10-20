cmake_minimum_required(VERSION 3.20)

# Must be done first
if (APPLE)
    # Force clang compiler on macOS
    set(CMAKE_CXX_COMPILER "/usr/bin/clang++")
    set(CMAKE_C_COMPILER "/usr/bin/clang")
endif ()

# Options
option(WITH_TLS "Build valkey-server with TLS support" OFF)
option(BUILD_TESTS "Build valkey-unit-tests" OFF)
option(WITH_RDMA "Build valkey-rdma module" OFF)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")
project("valkey")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(ValkeySetup)
add_subdirectory(src)

# Include the packaging module
include(Packaging)

# Clear variables declared on this file from the cache
unset(WITH_TLS CACHE)
unset(BUILD_TESTS CACHE)
unset(WITH_RDMA CACHE)
