include(CheckIncludeFiles)
include(ProcessorCount)
include(Utils)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Generate compile_commands.json file for IDEs code completion support
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

processorcount(VALKEY_PROCESSOR_COUNT)
message(STATUS "Processor count: ${VALKEY_PROCESSOR_COUNT}")

# Installed executables will have this permissions
set(VALKEY_EXE_PERMISSIONS
    OWNER_EXECUTE
    OWNER_WRITE
    OWNER_READ
    GROUP_EXECUTE
    GROUP_READ
    WORLD_EXECUTE
    WORLD_READ)

set(VALKEY_SERVER_CFLAGS "")
set(VALKEY_SERVER_LDFLAGS "")

# ----------------------------------------------------
# Helper functions & macros
# ----------------------------------------------------
macro (add_valkey_server_compiler_options value)
    set(VALKEY_SERVER_CFLAGS "${VALKEY_SERVER_CFLAGS} ${value}")
endmacro ()

macro (add_valkey_server_linker_option value)
    list(APPEND VALKEY_SERVER_LDFLAGS ${value})
endmacro ()

macro (get_valkey_server_linker_option return_value)
    list(JOIN VALKEY_SERVER_LDFLAGS " " ${value} ${return_value})
endmacro ()

set(IS_FREEBSD 0)
if (CMAKE_SYSTEM_NAME MATCHES "^.*BSD$|DragonFly")
    message(STATUS "Building for FreeBSD compatible system")
    set(IS_FREEBSD 1)
    include_directories("/usr/local/include")
    add_valkey_server_compiler_options("-DUSE_BACKTRACE")
endif ()

# Helper function for creating symbolic link so that: link -> source
macro (valkey_create_symlink source link)
    install(
        CODE "execute_process(                      \
    COMMAND /bin/bash ${CMAKE_BINARY_DIR}/CreateSymlink.sh \
    ${source} \
    ${link}   \
    )"
        COMPONENT "valkey")
endmacro ()

# Install a binary
macro (valkey_install_bin target)
    # Install cli tool and create a redis symbolic link
    install(
        TARGETS ${target}
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        PERMISSIONS ${VALKEY_EXE_PERMISSIONS}
        COMPONENT "valkey")
endmacro ()

# Helper function that defines, builds and installs `target` In addition, it creates a symbolic link between the target
# and `link_name`
macro (valkey_build_and_install_bin target sources ld_flags libs link_name)
    add_executable(${target} ${sources})

    if (USE_JEMALLOC
        OR USE_TCMALLOC
        OR USE_TCMALLOC_MINIMAL)
        # Using custom allocator
        target_link_libraries(${target} ${ALLOCATOR_LIB})
    endif ()

    # Place this line last to ensure that ${ld_flags} is placed last on the linker line
    target_link_libraries(${target} ${libs} ${ld_flags})
    target_link_libraries(${target} hiredis)
    if (USE_TLS)
        # Add required libraries needed for TLS
        target_link_libraries(${target} OpenSSL::SSL hiredis_ssl)
    endif ()

    if (IS_FREEBSD)
        target_link_libraries(${target} execinfo)
    endif ()

    # Enable all warnings + fail on warning
    target_compile_options(${target} PRIVATE -Werror -Wall)

    # Install cli tool and create a redis symbolic link
    valkey_install_bin(${target})
    valkey_create_symlink(${target} ${link_name})
endmacro ()

# Helper function that defines, builds and installs `target` module.
macro (valkey_build_and_install_module target sources ld_flags libs)
    add_library(${target} SHARED ${sources})

    if (USE_JEMALLOC)
        # Using jemalloc
        target_link_libraries(${target} jemalloc)
    endif ()

    # Place this line last to ensure that ${ld_flags} is placed last on the linker line
    target_link_libraries(${target} ${libs} ${ld_flags})
    if (USE_TLS)
        # Add required libraries needed for TLS
        target_link_libraries(${target} OpenSSL::SSL hiredis_ssl)
    endif ()

    if (IS_FREEBSD)
        target_link_libraries(${target} execinfo)
    endif ()

    # Install cli tool and create a redis symbolic link
    valkey_install_bin(${target})
endmacro ()

# Determine if we are building in Release or Debug mode
if (CMAKE_BUILD_TYPE MATCHES Debug OR CMAKE_BUILD_TYPE MATCHES DebugFull)
    set(VALKEY_DEBUG_BUILD 1)
    set(VALKEY_RELEASE_BUILD 0)
    message(STATUS "Building in debug mode")
else ()
    set(VALKEY_DEBUG_BUILD 0)
    set(VALKEY_RELEASE_BUILD 1)
    message(STATUS "Building in release mode")
endif ()

# ----------------------------------------------------
# Helper functions - end
# ----------------------------------------------------

# ----------------------------------------------------
# Build options (allocator, tls, rdma et al)
# ----------------------------------------------------

if (NOT BUILD_MALLOC)
    if (APPLE)
        set(BUILD_MALLOC "libc")
    elseif (UNIX)
        set(BUILD_MALLOC "jemalloc")
    endif ()
endif ()

# User may pass different allocator library. Using -DBUILD_MALLOC=<libname>, make sure it is a valid value
if (BUILD_MALLOC)
    if ("${BUILD_MALLOC}" STREQUAL "jemalloc")
        set(MALLOC_LIB "jemalloc")
        set(ALLOCATOR_LIB "jemalloc")
        add_valkey_server_compiler_options("-DUSE_JEMALLOC")
        set(USE_JEMALLOC 1)
    elseif ("${BUILD_MALLOC}" STREQUAL "libc")
        set(MALLOC_LIB "libc")
    elseif ("${BUILD_MALLOC}" STREQUAL "tcmalloc")
        set(MALLOC_LIB "tcmalloc")
        valkey_pkg_config(libtcmalloc ALLOCATOR_LIB)

        add_valkey_server_compiler_options("-DUSE_TCMALLOC")
        set(USE_TCMALLOC 1)
    elseif ("${BUILD_MALLOC}" STREQUAL "tcmalloc_minimal")
        set(MALLOC_LIB "tcmalloc_minimal")
        valkey_pkg_config(libtcmalloc_minimal ALLOCATOR_LIB)

        add_valkey_server_compiler_options("-DUSE_TCMALLOC")
        set(USE_TCMALLOC_MINIMAL 1)
    else ()
        message(FATAL_ERROR "BUILD_MALLOC can be one of: jemalloc, libc, tcmalloc or tcmalloc_minimal")
    endif ()
endif ()

message(STATUS "Using ${MALLOC_LIB}")

# TLS support
if (BUILD_TLS)
    valkey_parse_build_option(${BUILD_TLS} USE_TLS)
    if (USE_TLS EQUAL 1)
        # Only search for OpenSSL if needed
        find_package(OpenSSL REQUIRED)
        message(STATUS "OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
        message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")
        include_directories(${OPENSSL_INCLUDE_DIR})
    endif ()

    if (USE_TLS EQUAL 1)
        add_valkey_server_compiler_options("-DUSE_OPENSSL=1")
        add_valkey_server_compiler_options("-DBUILD_TLS_MODULE=0")
    else ()
        # Build TLS as a module RDMA can only be built as a module. So disable it
        message(WARNING "BUILD_TLS can be one of: [ON | OFF | 1 | 0], but '${BUILD_TLS}' was provided")
        message(STATUS "TLS support is disabled")
        set(USE_TLS 0)
    endif ()
else ()
    # By default, TLS is disabled
    message(STATUS "TLS is disabled")
    set(USE_TLS 0)
endif ()

if (BUILD_RDMA)
    set(BUILD_RDMA_MODULE 0)
    # RDMA support (Linux only)
    if (LINUX AND NOT APPLE)
        valkey_parse_build_option(${BUILD_RDMA} USE_RDMA)
        find_package(PkgConfig REQUIRED)
        # Locate librdmacm & libibverbs, fail if we can't find them
        valkey_pkg_config(librdmacm RDMACM_LIBS)
        valkey_pkg_config(libibverbs IBVERBS_LIBS)
        message(STATUS "${RDMACM_LIBS};${IBVERBS_LIBS}")
        list(APPEND RDMA_LIBS "${RDMACM_LIBS};${IBVERBS_LIBS}")

        if (USE_RDMA EQUAL 2) # Module
            message(STATUS "Building RDMA as module")
            add_valkey_server_compiler_options("-DUSE_RDMA=2")
            set(BUILD_RDMA_MODULE 2)
        elseif (USE_RDMA EQUAL 1) # Builtin
            message(STATUS "Building RDMA as builtin")
            add_valkey_server_compiler_options("-DUSE_RDMA=1")
            add_valkey_server_compiler_options("-DBUILD_RDMA_MODULE=0")
            list(APPEND SERVER_LIBS "${RDMA_LIBS}")
        endif ()
    else ()
        message(WARNING "RDMA is only supported on Linux platforms")
    endif ()
else ()
    # By default, RDMA is disabled
    message(STATUS "RDMA is disabled")
    set(USE_RDMA 0)
endif ()

set(BUILDING_ARM64 0)
set(BUILDING_ARM32 0)

if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm64" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
    set(BUILDING_ARM64 1)
endif ()

if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm")
    set(BUILDING_ARM32 1)
endif ()

message(STATUS "Building on ${CMAKE_HOST_SYSTEM_NAME}")
if (BUILDING_ARM64)
    message(STATUS "Compiling valkey for ARM64")
    add_valkey_server_linker_option("-funwind-tables")
endif ()

if (APPLE)
    add_valkey_server_linker_option("-rdynamic")
    add_valkey_server_linker_option("-ldl")
elseif (UNIX)
    add_valkey_server_linker_option("-rdynamic")
    add_valkey_server_linker_option("-pthread")
    add_valkey_server_linker_option("-ldl")
    add_valkey_server_linker_option("-lm")
endif ()

if (VALKEY_DEBUG_BUILD)
    # Debug build, use enable "-fno-omit-frame-pointer"
    add_valkey_server_compiler_options("-fno-omit-frame-pointer")
endif ()

# Check for Atomic
check_include_files(stdatomic.h HAVE_C11_ATOMIC)
if (HAVE_C11_ATOMIC)
    add_valkey_server_compiler_options("-std=gnu11")
else ()
    add_valkey_server_compiler_options("-std=c99")
endif ()

# Sanitizer
if (BUILD_SANITIZER)
    # Common CFLAGS
    list(APPEND VALKEY_SANITAIZER_CFLAGS "-fno-sanitize-recover=all")
    list(APPEND VALKEY_SANITAIZER_CFLAGS "-fno-omit-frame-pointer")
    if ("${BUILD_SANITIZER}" STREQUAL "address")
        list(APPEND VALKEY_SANITAIZER_CFLAGS "-fsanitize=address")
        list(APPEND VALKEY_SANITAIZER_LDFLAGS "-fsanitize=address")
    elseif ("${BUILD_SANITIZER}" STREQUAL "thread")
        list(APPEND VALKEY_SANITAIZER_CFLAGS "-fsanitize=thread")
        list(APPEND VALKEY_SANITAIZER_LDFLAGS "-fsanitize=thread")
    elseif ("${BUILD_SANITIZER}" STREQUAL "undefined")
        list(APPEND VALKEY_SANITAIZER_CFLAGS "-fsanitize=undefined")
        list(APPEND VALKEY_SANITAIZER_LDFLAGS "-fsanitize=undefined")
    else ()
        message(FATAL_ERROR "Unknown sanitizer: ${BUILD_SANITIZER}")
    endif ()
endif ()

include_directories("${CMAKE_SOURCE_DIR}/deps/hiredis")
include_directories("${CMAKE_SOURCE_DIR}/deps/linenoise")
include_directories("${CMAKE_SOURCE_DIR}/deps/lua/src")
include_directories("${CMAKE_SOURCE_DIR}/deps/hdr_histogram")
include_directories("${CMAKE_SOURCE_DIR}/deps/fpconv")

add_subdirectory("${CMAKE_SOURCE_DIR}/deps")

# Update linker flags for the allocator
if (USE_JEMALLOC)
    include_directories("${CMAKE_SOURCE_DIR}/deps/jemalloc/include")
endif ()

# Common compiler flags
add_valkey_server_compiler_options("-pedantic")

# ----------------------------------------------------
# Build options (allocator, tls, rdma et al) - end
# ----------------------------------------------------

# -------------------------------------------------
# Code Generation section
# -------------------------------------------------
find_program(PYTHON_EXE python3)
if (PYTHON_EXE)
    # Python based code generation
    message(STATUS "Found python3: ${PYTHON_EXE}")
    # Rule for generating commands.def file from json files
    message(STATUS "Adding target generate_commands_def")
    file(GLOB COMMAND_FILES_JSON "${CMAKE_SOURCE_DIR}/src/commands/*.json")
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/commands_def_generated
        DEPENDS ${COMMAND_FILES_JSON}
        COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/utils/generate-command-code.py
        COMMAND touch ${CMAKE_BINARY_DIR}/commands_def_generated
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src")
    add_custom_target(generate_commands_def DEPENDS ${CMAKE_BINARY_DIR}/commands_def_generated)

    # Rule for generating fmtargs.h
    message(STATUS "Adding target generate_fmtargs_h")
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/fmtargs_generated
        DEPENDS ${CMAKE_SOURCE_DIR}/utils/generate-fmtargs.py
        COMMAND sed '/Everything/,$$d' fmtargs.h > fmtargs.h.tmp
        COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/utils/generate-fmtargs.py >> fmtargs.h.tmp
        COMMAND mv fmtargs.h.tmp fmtargs.h
        COMMAND touch ${CMAKE_BINARY_DIR}/fmtargs_generated
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src")
    add_custom_target(generate_fmtargs_h DEPENDS ${CMAKE_BINARY_DIR}/fmtargs_generated)

    # Rule for generating test_files.h
    message(STATUS "Adding target generate_test_files_h")
    file(GLOB UNIT_TEST_SRCS "${CMAKE_SOURCE_DIR}/src/unit/*.c")
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/test_files_generated
        DEPENDS "${UNIT_TEST_SRCS};${CMAKE_SOURCE_DIR}/utils/generate-unit-test-header.py"
        COMMAND ${PYTHON_EXE} ${CMAKE_SOURCE_DIR}/utils/generate-unit-test-header.py
        COMMAND touch ${CMAKE_BINARY_DIR}/test_files_generated
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src")
    add_custom_target(generate_test_files_h DEPENDS ${CMAKE_BINARY_DIR}/test_files_generated)
else ()
    # Fake targets
    add_custom_target(generate_commands_def)
    add_custom_target(generate_fmtargs_h)
    add_custom_target(generate_test_files_h)
endif ()

# Generate release.h file (always)
add_custom_target(
    release_header
    COMMAND sh -c '${CMAKE_SOURCE_DIR}/src/mkreleasehdr.sh'
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src")

# -------------------------------------------------
# Code Generation section - end
# -------------------------------------------------

# ----------------------------------------------------------
# All our source files are defined in SourceFiles.cmake file
# ----------------------------------------------------------
include(SourceFiles)

# Clear the below variables from the cache
unset(CMAKE_C_FLAGS CACHE)
unset(VALKEY_SERVER_LDFLAGS CACHE)
unset(VALKEY_SERVER_CFLAGS CACHE)
unset(PYTHON_EXE CACHE)
unset(HAVE_C11_ATOMIC CACHE)
unset(USE_TLS CACHE)
unset(USE_RDMA CACHE)
unset(BUILD_TLS CACHE)
unset(BUILD_RDMA CACHE)
unset(BUILD_MALLOC CACHE)
unset(USE_JEMALLOC CACHE)
unset(BUILD_TLS_MODULE CACHE)
unset(BUILD_TLS_BUILTIN CACHE)
