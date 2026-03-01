cmake_minimum_required(VERSION 3.14)
project(valkey_unit_gtests)

# GoogleTest requires at least C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_valkey_server_linker_option(VALKEY_SERVER_LDFLAGS)

# Build GoogleTest-based tests
message(STATUS "Building gtest unit tests")
if (USE_TLS)
    if (BUILD_TLS_MODULE)
        # TLS as a module
        list(APPEND COMPILE_DEFINITIONS "USE_OPENSSL=2")
    else (BUILD_TLS_MODULE)
        # Built-in TLS support
        list(APPEND COMPILE_DEFINITIONS "USE_OPENSSL=1")
        list(APPEND COMPILE_DEFINITIONS "BUILD_TLS_MODULE=0")
    endif ()
endif ()

# Propagate allocator defines so that C++ test code uses the same
# malloc_usable_size variant (e.g. je_malloc_usable_size) as the C library.
if (USE_JEMALLOC)
    list(APPEND COMPILE_DEFINITIONS "USE_JEMALLOC")
elseif (USE_TCMALLOC OR USE_TCMALLOC_MINIMAL)
    list(APPEND COMPILE_DEFINITIONS "USE_TCMALLOC")
endif ()

# Fetch GoogleTest using FetchContent (modern approach)
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0 # Pinned GoogleTest version
)

FetchContent_MakeAvailable(googletest)

# Build Valkey sources as a static library for the test (C code compiled with C compiler)
add_library(valkeylib-gtest STATIC ${VALKEY_SERVER_SRCS})
target_compile_options(valkeylib-gtest PRIVATE -Og -g -fno-lto)
target_compile_definitions(valkeylib-gtest PRIVATE "${COMPILE_DEFINITIONS}")
target_include_directories(valkeylib-gtest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/deps)

# Generate wrapper files from wrappers.h
set(GENERATED_WRAPPERS_DIR ${CMAKE_BINARY_DIR}/gtest_generated)
file(MAKE_DIRECTORY ${GENERATED_WRAPPERS_DIR})
add_custom_command(
    OUTPUT ${GENERATED_WRAPPERS_DIR}/generated_wrappers.cpp ${GENERATED_WRAPPERS_DIR}/generated_wrappers.hpp
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/generate-wrappers.py ${GENERATED_WRAPPERS_DIR}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wrappers.h ${CMAKE_CURRENT_LIST_DIR}/generate-wrappers.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Generating wrapper files from wrappers.h"
)

# Create a target for generating wrapper files
add_custom_target(generate_wrappers DEPENDS ${GENERATED_WRAPPERS_DIR}/generated_wrappers.cpp ${GENERATED_WRAPPERS_DIR}/generated_wrappers.hpp)

# Collect C++ test source files (excluding generated ones for now)
file(GLOB GTEST_SRCS "${CMAKE_CURRENT_LIST_DIR}/*.cpp")
list(REMOVE_ITEM GTEST_SRCS "${GENERATED_WRAPPERS_DIR}/generated_wrappers.cpp")

# Create GoogleTest executable (C++ code compiled with C++ compiler)
add_executable(valkey-unit-gtests ${GTEST_SRCS} ${GENERATED_WRAPPERS_DIR}/generated_wrappers.cpp)

# Make sure generated files are created before building
add_dependencies(valkey-unit-gtests generate_wrappers)

# Set C++ properties
set_target_properties(valkey-unit-gtests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Add C++ compile definitions and flags for C header compatibility
target_compile_definitions(valkey-unit-gtests PRIVATE "${COMPILE_DEFINITIONS}")

# Allow C-style void pointer conversions in C++
target_compile_options(valkey-unit-gtests PRIVATE -Og -g)

# Disable warnings
target_compile_options(valkey-unit-gtests PRIVATE
    -Wno-deprecated-declarations
    -Wno-write-strings
    -fno-var-tracking-assignments
)

# Include directories for C++ compilation
target_include_directories(valkey-unit-gtests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps
    ${CMAKE_CURRENT_LIST_DIR}
    ${GENERATED_WRAPPERS_DIR}
)

if (UNIX AND NOT APPLE)
    # Avoid duplicate symbols on non macOS
    target_link_options(valkey-unit-gtests PRIVATE "-Wl,--allow-multiple-definition")

    # Extract all wrapped function names from wrappers.h and add --wrap linker flags
    execute_process(
        COMMAND python3 -c "
import re
import sys
with open('${CMAKE_CURRENT_LIST_DIR}/wrappers.h', 'r') as f:
    for line in f:
        m = re.match(r'.*__wrap_(\\w+)\\(.*\\);', line)
        if m:
            print(m.group(1))
"
        OUTPUT_VARIABLE WRAPPED_FUNCTIONS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Convert the list of functions to linker flags
    string(REPLACE "\n" ";" WRAPPED_FUNCTIONS_LIST "${WRAPPED_FUNCTIONS}")
    foreach(FUNC ${WRAPPED_FUNCTIONS_LIST})
        target_link_options(valkey-unit-gtests PRIVATE "-Wl,--wrap=${FUNC}")
    endforeach()
endif ()

if (USE_JEMALLOC)
    # Using jemalloc - link to both the static library and the executable
    target_link_libraries(valkeylib-gtest jemalloc)
    target_link_libraries(valkey-unit-gtests jemalloc)
endif ()

if (IS_FREEBSD)
    target_link_libraries(valkey-unit-gtests execinfo)
endif ()

# Link with GoogleTest using target names
target_link_libraries(
    valkey-unit-gtests
    valkeylib-gtest
    fpconv
    lualib
    hdr_histogram
    valkey::valkey
    GTest::gtest_main
    GTest::gmock
    pthread
    ${VALKEY_SERVER_LDFLAGS})

if (USE_TLS)
    # Add required libraries needed for TLS
    target_link_libraries(valkey-unit-gtests OpenSSL::SSL valkey::valkey_tls)
endif ()

# Enable testing and discover tests
enable_testing()
include(GoogleTest)
gtest_discover_tests(valkey-unit-gtests)

# Add custom test target using gtest-parallel
add_custom_target(test-unit
    COMMAND python3 ${CMAKE_SOURCE_DIR}/deps/gtest-parallel/gtest_parallel.py $<TARGET_FILE:valkey-unit-gtests>
    DEPENDS valkey-unit-gtests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running tests with gtest-parallel"
)
