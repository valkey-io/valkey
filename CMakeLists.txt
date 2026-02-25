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
option(BUILD_LUA "Build Valkey Lua scripting engine" ON)
option(BUILD_UNIT_TESTS "Build valkey-unit-tests" OFF)
option(BUILD_TEST_MODULES "Build all test modules" OFF)
option(BUILD_EXAMPLE_MODULES "Build example modules" OFF)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")
project("valkey")
add_compile_options(-Wundef)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(ValkeySetup)
add_subdirectory(src)
add_subdirectory(tests)

# Include the packaging module
include(Packaging)

# Clear cached variables from the cache
unset(BUILD_LUA CACHE)
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

# Helper to copy runtest scripts to allow running tests with CMake built binaries
function(copy_runtest_script script_name)
    set(src "${CMAKE_SOURCE_DIR}/${script_name}")
    set(dst "${CMAKE_BINARY_DIR}/${script_name}")
    file(READ "${src}" contents)

    # Split at the first newline (after shebang #!/bin/sh)
    string(FIND "${contents}" "\n" index_of_first_newline_char)
    math(EXPR first_index_script_body "${index_of_first_newline_char} + 1")

    string(SUBSTRING "${contents}" 0 ${first_index_script_body} shebang_line)
    string(SUBSTRING "${contents}" ${first_index_script_body} -1 script_body)

    # Insert our environment variable lines
    set(insert_content
"# Most tests assume running from the project root
cd ${CMAKE_SOURCE_DIR}
export VALKEY_BIN_DIR=\"${CMAKE_BINARY_DIR}/bin\"
")
    # Reconstruct the full script
    set(new_contents "${shebang_line}${insert_content}${script_body}")
    file(WRITE "${dst}" "${new_contents}")
    file(CHMOD "${dst}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endfunction()

copy_runtest_script("runtest")
copy_runtest_script("runtest-cluster")
copy_runtest_script("runtest-moduleapi")
copy_runtest_script("runtest-rdma")
copy_runtest_script("runtest-sentinel")