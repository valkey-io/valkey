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

if (APPLE)
    # The macOS linker has no '--wrap' option, so the symbols are renamed in the
    # object files instead (see the symbol wrapping section at the end of this
    # file). This needs the LLVM tools, which Homebrew does not add to PATH. The
    # standard Homebrew prefixes for both Apple Silicon (/opt/homebrew) and Intel
    # (/usr/local) are passed as PATHS, so they are searched last and a tool that
    # the developer put on PATH wins.
    set(LLVM_TOOLS_PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
    find_program(LLVM_NM NAMES llvm-nm PATHS ${LLVM_TOOLS_PATHS})
    find_program(LLVM_OBJCOPY NAMES llvm-objcopy PATHS ${LLVM_TOOLS_PATHS})
    find_program(LLVM_LLC NAMES llc PATHS ${LLVM_TOOLS_PATHS})
    if (NOT LLVM_NM OR NOT LLVM_OBJCOPY)
        message(FATAL_ERROR "llvm-nm and llvm-objcopy are required to build the unit tests. "
                            "Install them via: brew install llvm")
    endif ()

    # The static libraries holding the objects with the renamed symbols
    set(WRAPPED_DIR "${CMAKE_CURRENT_BINARY_DIR}/wrapped")
    set(WRAPPED_VALKEY_LIB "${WRAPPED_DIR}/libwrapped-valkeylib.a")
    set(WRAPPED_TESTS_LIB "${WRAPPED_DIR}/libwrapped-tests.a")
    # An object library, because the objects are post processed before linking
    add_library(valkeylib-gtest OBJECT ${VALKEY_SERVER_SRCS})
else ()
    add_library(valkeylib-gtest STATIC ${VALKEY_SERVER_SRCS})
endif ()

add_dependencies(valkeylib-gtest release_header)
target_compile_options(valkeylib-gtest PRIVATE -Og -g -fno-lto)
target_compile_definitions(valkeylib-gtest PRIVATE "${COMPILE_DEFINITIONS}")
target_include_directories(valkeylib-gtest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/deps)
target_link_libraries(valkeylib-gtest PRIVATE ffc)

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
# A Makefile build writes its own copy of generated_wrappers.cpp into the source
# directory. Compiling that copy as well would define the '__wrap_' functions twice.
list(REMOVE_ITEM GTEST_SRCS "${CMAKE_CURRENT_LIST_DIR}/generated_wrappers.cpp")

# The same Makefile build also writes generated_wrappers.hpp there. The test files
# use '#include "generated_wrappers.hpp"', which is looked up in the directory of
# the including file first, so that copy shadows the generated header and no
# include path can override it. Stop with a clear message instead of compiling
# the test files against an out of date header.
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/generated_wrappers.hpp")
    message(FATAL_ERROR "${CMAKE_CURRENT_LIST_DIR}/generated_wrappers.hpp is left over from a Makefile "
                        "build and would shadow the generated header. Remove it with: make -C src/unit clean")
endif ()

# Create GoogleTest executable (C++ code compiled with C++ compiler)
if (APPLE)
    # The test sources are built separately, so that their objects can be post
    # processed (see the symbol wrapping section at the end of this file)
    add_executable(valkey-unit-gtests ${GENERATED_WRAPPERS_DIR}/generated_wrappers.cpp)
else ()
    add_executable(valkey-unit-gtests ${GTEST_SRCS} ${GENERATED_WRAPPERS_DIR}/generated_wrappers.cpp)
endif ()

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
)
# -fno-var-tracking-assignments is only supported by GCC
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(valkey-unit-gtests PRIVATE -fno-var-tracking-assignments)
endif()

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
    target_link_libraries(valkeylib-gtest PRIVATE jemalloc)
    target_link_libraries(valkey-unit-gtests PRIVATE jemalloc)
endif ()

if (IS_FREEBSD)
    target_link_libraries(valkey-unit-gtests PRIVATE execinfo)
endif ()

# Link with GoogleTest using target names
if (APPLE)
    # The wrapped copy of the server objects is linked instead of the
    # valkeylib-gtest target itself
    target_link_libraries(
        valkey-unit-gtests
        PRIVATE
        "${WRAPPED_VALKEY_LIB}"
        fpconv
        lz4
        lualib
        hdr_histogram
        valkey::valkey
        GTest::gtest_main
        GTest::gmock
        pthread
        ${VALKEY_SERVER_LDFLAGS})
else ()
    target_link_libraries(
        valkey-unit-gtests
        PRIVATE
        valkeylib-gtest
        fpconv
        lz4
        lualib
        hdr_histogram
        valkey::valkey
        GTest::gtest_main
        GTest::gmock
        pthread
        ${VALKEY_SERVER_LDFLAGS})
endif ()

if (USE_TLS)
    # Add required libraries needed for TLS
    target_link_libraries(valkey-unit-gtests PRIVATE OpenSSL::SSL valkey::valkey_tls)
endif ()

if (APPLE)
    # Symbol wrapping for macOS.
    #
    # The macOS linker has no '--wrap' option, so wrap-objects.py renames the
    # symbols in the object files instead: '___real_<name>' in the object that
    # defines the function, '___wrap_<name>' in every object that calls it.
    # This is what the Makefile build does as well.
    #
    # generated_wrappers.cpp is left out, because it defines the '__wrap_'
    # functions and calls the '__real_' ones.
    add_library(valkey-unit-gtests-objs OBJECT ${GTEST_SRCS})
    add_dependencies(valkey-unit-gtests-objs generate_wrappers)

    set_target_properties(valkey-unit-gtests-objs PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    target_compile_definitions(valkey-unit-gtests-objs PRIVATE "${COMPILE_DEFINITIONS}")
    target_compile_options(valkey-unit-gtests-objs PRIVATE
        -Og
        -g
        -Wno-deprecated-declarations
        -Wno-write-strings
    )
    target_include_directories(valkey-unit-gtests-objs PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/deps
        ${CMAKE_CURRENT_LIST_DIR}
        ${GENERATED_WRAPPERS_DIR}
    )
    # Nothing is linked here, but the test sources need the same headers
    # (GoogleTest, OpenSSL, ...) as the executable
    target_link_libraries(valkey-unit-gtests-objs PRIVATE
        fpconv
        lz4
        lualib
        hdr_histogram
        valkey::valkey
        GTest::gtest_main
        GTest::gmock)
    if (USE_TLS)
        target_link_libraries(valkey-unit-gtests-objs PRIVATE OpenSSL::SSL valkey::valkey_tls)
    endif ()
    if (USE_JEMALLOC)
        target_link_libraries(valkey-unit-gtests-objs PRIVATE jemalloc)
    endif ()

    # The wrapped objects are collected into two static libraries:
    #   * the server objects, linked as a normal library
    #   * the test objects, force loaded so that all gtest cases are registered
    set(WRAP_OBJECTS_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/wrap-objects.py")
    set(WRAP_OBJECTS_ARGS
        --nm "${LLVM_NM}"
        --objcopy "${LLVM_OBJCOPY}"
        --ar "${CMAKE_AR}"
        --wrappers "${CMAKE_CURRENT_LIST_DIR}/wrappers.h")
    if (LLVM_LLC)
        list(APPEND WRAP_OBJECTS_ARGS --llc "${LLVM_LLC}")
    endif ()
    set(WRAP_OBJECTS_DEPS
        "${WRAP_OBJECTS_SCRIPT}"
        "${CMAKE_CURRENT_LIST_DIR}/wrapper_util.py"
        "${CMAKE_CURRENT_LIST_DIR}/wrappers.h")

    add_custom_command(
        OUTPUT "${WRAPPED_VALKEY_LIB}"
        COMMAND python3 "${WRAP_OBJECTS_SCRIPT}" ${WRAP_OBJECTS_ARGS}
                --output-dir "${WRAPPED_DIR}/valkeylib"
                --archive "${WRAPPED_VALKEY_LIB}"
                $<TARGET_OBJECTS:valkeylib-gtest>
        DEPENDS valkeylib-gtest $<TARGET_OBJECTS:valkeylib-gtest> ${WRAP_OBJECTS_DEPS}
        COMMAND_EXPAND_LISTS
        COMMENT "Wrapping symbols in the Valkey server objects"
    )

    add_custom_command(
        OUTPUT "${WRAPPED_TESTS_LIB}"
        COMMAND python3 "${WRAP_OBJECTS_SCRIPT}" ${WRAP_OBJECTS_ARGS}
                --output-dir "${WRAPPED_DIR}/tests"
                --archive "${WRAPPED_TESTS_LIB}"
                $<TARGET_OBJECTS:valkey-unit-gtests-objs>
        DEPENDS valkey-unit-gtests-objs $<TARGET_OBJECTS:valkey-unit-gtests-objs> ${WRAP_OBJECTS_DEPS}
        COMMAND_EXPAND_LISTS
        COMMENT "Wrapping symbols in the unit test objects"
    )

    add_custom_target(wrap_objects DEPENDS "${WRAPPED_VALKEY_LIB}" "${WRAPPED_TESTS_LIB}")
    add_dependencies(valkey-unit-gtests wrap_objects)
    target_link_options(valkey-unit-gtests PRIVATE "-Wl,-force_load,${WRAPPED_TESTS_LIB}")

    # The test archive is passed as a linker flag, so both archives are listed
    # explicitly to relink the test binary when they change
    set_property(TARGET valkey-unit-gtests APPEND PROPERTY LINK_DEPENDS
                 "${WRAPPED_VALKEY_LIB};${WRAPPED_TESTS_LIB}")

    # 'make clean' does not know about the wrapped object files. The property
    # needs CMake 3.15 or newer, older versions just ignore it and leave the
    # files behind.
    set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${WRAPPED_DIR}")
endif ()

# Enable testing and discover tests
enable_testing()
include(GoogleTest)
gtest_discover_tests(valkey-unit-gtests)

# Add custom test target using gtest-parallel
add_custom_target(test-unit
    COMMAND sh -c "TEST_ARGS=''; \
        [ -n \"$accurate\" ] && TEST_ARGS=\"$TEST_ARGS --accurate\"; \
        [ -n \"$large_memory\" ] && TEST_ARGS=\"$TEST_ARGS --large-memory\"; \
        [ -n \"$valgrind\" ] && TEST_ARGS=\"$TEST_ARGS --valgrind\"; \
        [ -n \"$seed\" ] && TEST_ARGS=\"$TEST_ARGS --seed $seed\"; \
        python3 ${CMAKE_SOURCE_DIR}/deps/gtest-parallel/gtest_parallel.py $<TARGET_FILE:valkey-unit-gtests> --timeout_per_test 300 --gtest_filter=\"$UNIT_TEST_PATTERN\"* -- $TEST_ARGS"
    DEPENDS valkey-unit-gtests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running tests with gtest-parallel"
    VERBATIM
)
